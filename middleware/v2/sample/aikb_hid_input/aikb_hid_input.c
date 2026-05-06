#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT_U32(n) (1u << (n))

#define PAGE_ALIGN_DOWN(x, p) ((x) & ~((uint32_t)((p) - 1)))

#define PINMUX_BASE 0x03001000u
#define PINMUX_PAGE_SIZE 0x1000u
#define GPIOA_BASE 0x03020000u
#define GPIO_PAGE_SIZE 0x1000u

#define GPIO_SWPORTA_DDR 0x04u
#define GPIO_EXT_PORTA 0x50u

#define IOBLK_PULL_UP_BIT BIT_U32(2)
#define IOBLK_PULL_DOWN_BIT BIT_U32(3)
#define PINMUX_FUNC_MASK 0x7u
#define PINMUX_FUNC_GPIOA 0x3u

#define HID_REPORT_LEN 64
#define REPORT_ID_INPUT 0x10
#define REPORT_ID_OUTPUT 0x20
#define REPORT_ID_ACK 0x21

struct mmio_page {
	uint32_t base;
	size_t size;
	volatile uint8_t *ptr;
};

struct pin_def {
	const char *name;
	uint8_t gpio_bit;
	uint32_t pinmux_addr;
	uint32_t ioblk_addr;
};

struct debouncer {
	bool stable;
	bool raw;
	uint64_t raw_since_ms;
};

struct config {
	const char *hid_path;
	unsigned poll_ms;
	unsigned debounce_ms;
	bool debug;
	bool reverse;
	bool no_hid;
};

static const struct pin_def g_keys[] = {
	{ "key0_A15", 15, 0x03001020u, 0x03001908u },
	{ "key1_A24", 24, 0x03001040u, 0x03001928u },
	{ "key2_A23", 23, 0x0300103cu, 0x03001924u },
};

static const struct pin_def g_enc_a = {
	"encA_A27", 27, 0x03001038u, 0x03001920u
};

static const struct pin_def g_enc_b = {
	"encB_A25", 25, 0x03001034u, 0x0300191cu
};

static const struct pin_def g_enc_e = {
	"encE_A22", 22, 0x03001030u, 0x03001918u
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000u;
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
		if (g_stop)
			break;
	}
}

static int mmio_map(int mem_fd, struct mmio_page *page, uint32_t base,
		    size_t size)
{
	page->base = base;
	page->size = size;
	page->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd,
			 (off_t)base);
	if (page->ptr == MAP_FAILED) {
		fprintf(stderr, "aikb_hid_input: mmap 0x%08x failed: %s\n",
			base, strerror(errno));
		page->ptr = NULL;
		return -1;
	}
	return 0;
}

static void mmio_unmap(struct mmio_page *page)
{
	if (page->ptr)
		munmap((void *)page->ptr, page->size);
	page->ptr = NULL;
}

static volatile uint32_t *mmio_reg(struct mmio_page *page, uint32_t addr)
{
	return (volatile uint32_t *)(page->ptr + (addr - page->base));
}

static uint32_t mmio_read32(struct mmio_page *page, uint32_t addr)
{
	return *mmio_reg(page, addr);
}

static void mmio_write32(struct mmio_page *page, uint32_t addr, uint32_t val)
{
	*mmio_reg(page, addr) = val;
}

static void configure_pin(struct mmio_page *pinmux, const struct pin_def *pin,
			  bool debug)
{
	uint32_t mux = mmio_read32(pinmux, pin->pinmux_addr);
	uint32_t ioblk = mmio_read32(pinmux, pin->ioblk_addr);

	mux = (mux & ~PINMUX_FUNC_MASK) | PINMUX_FUNC_GPIOA;
	ioblk |= IOBLK_PULL_UP_BIT;
	ioblk &= ~IOBLK_PULL_DOWN_BIT;

	mmio_write32(pinmux, pin->pinmux_addr, mux);
	mmio_write32(pinmux, pin->ioblk_addr, ioblk);

	if (debug) {
		fprintf(stderr,
			"aikb_hid_input: %-9s gpio=A%u pinmux=0x%08x ioblk=0x%08x\n",
			pin->name, pin->gpio_bit, mux, ioblk);
	}
}

static int configure_board_inputs(struct mmio_page *pinmux,
				  struct mmio_page *gpio, bool debug)
{
	uint32_t mask = 0;
	uint32_t ddr;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(g_keys); i++) {
		configure_pin(pinmux, &g_keys[i], debug);
		mask |= BIT_U32(g_keys[i].gpio_bit);
	}

	configure_pin(pinmux, &g_enc_a, debug);
	configure_pin(pinmux, &g_enc_b, debug);
	configure_pin(pinmux, &g_enc_e, debug);
	mask |= BIT_U32(g_enc_a.gpio_bit);
	mask |= BIT_U32(g_enc_b.gpio_bit);
	mask |= BIT_U32(g_enc_e.gpio_bit);

	ddr = mmio_read32(gpio, GPIOA_BASE + GPIO_SWPORTA_DDR);
	ddr &= ~mask;
	mmio_write32(gpio, GPIOA_BASE + GPIO_SWPORTA_DDR, ddr);

	if (debug)
		fprintf(stderr, "aikb_hid_input: GPIOA_DDR=0x%08x input_mask=0x%08x\n",
			ddr, mask);

	return 0;
}

static bool gpio_level(uint32_t ext_port, uint8_t bit)
{
	return (ext_port & BIT_U32(bit)) != 0;
}

static bool active_low_pressed(uint32_t ext_port, uint8_t bit)
{
	return !gpio_level(ext_port, bit);
}

static void debounce_init(struct debouncer *d, bool raw, uint64_t now)
{
	d->stable = raw;
	d->raw = raw;
	d->raw_since_ms = now;
}

static bool debounce_update(struct debouncer *d, bool raw, uint64_t now,
			    unsigned debounce_ms)
{
	if (raw != d->raw) {
		d->raw = raw;
		d->raw_since_ms = now;
		return false;
	}

	if (raw != d->stable && now - d->raw_since_ms >= debounce_ms) {
		d->stable = raw;
		return true;
	}

	return false;
}

static uint8_t build_key_bits(const struct debouncer keys[3],
			      const struct debouncer *enc_sw)
{
	uint8_t bits = 0;
	size_t i;

	for (i = 0; i < 3; i++) {
		if (keys[i].stable)
			bits |= (uint8_t)(1u << i);
	}

	if (enc_sw->stable)
		bits |= 0x80u;

	return bits;
}

static int open_hid(const char *path)
{
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return -1;

	return fd;
}

static int write_hid_report(int fd, const uint8_t report[HID_REPORT_LEN])
{
	ssize_t n = write(fd, report, HID_REPORT_LEN);

	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 1;
		return -1;
	}

	if (n != HID_REPORT_LEN) {
		errno = EIO;
		return -1;
	}

	return 0;
}

static int send_input_report(int fd, uint8_t key_bits, int encoder_delta,
			     bool debug)
{
	uint8_t report[HID_REPORT_LEN];
	int rc;

	if (encoder_delta > 127)
		encoder_delta = 127;
	if (encoder_delta < -127)
		encoder_delta = -127;

	memset(report, 0, sizeof(report));
	report[0] = REPORT_ID_INPUT;
	report[1] = key_bits;
	report[2] = (uint8_t)(int8_t)encoder_delta;

	rc = write_hid_report(fd, report);
	if (debug && rc == 0) {
		fprintf(stderr, "aikb_hid_input: report 0x10 keys=0x%02x delta=%d\n",
			key_bits, encoder_delta);
	}

	return rc;
}

static int send_ack_report(int fd, uint8_t seq, uint8_t status, bool debug)
{
	uint8_t report[HID_REPORT_LEN];
	int rc;

	memset(report, 0, sizeof(report));
	report[0] = REPORT_ID_ACK;
	report[1] = seq;
	report[2] = status;

	rc = write_hid_report(fd, report);
	if (debug && rc == 0) {
		fprintf(stderr, "aikb_hid_input: ack 0x21 seq=%u status=%u\n",
			seq, status);
	}

	return rc;
}

static void drain_output_reports(int fd, bool debug)
{
	for (;;) {
		uint8_t report[HID_REPORT_LEN];
		ssize_t n = read(fd, report, sizeof(report));

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			return;
		}

		if (n == 0)
			return;

		if (report[0] != REPORT_ID_OUTPUT) {
			if (debug)
				fprintf(stderr,
					"aikb_hid_input: ignored output report id=0x%02x len=%zd\n",
					report[0], n);
			continue;
		}

		if (debug) {
			fprintf(stderr,
				"aikb_hid_input: output 0x20 cmd=0x%02x seq=%u len=%u\n",
				report[1], report[2], report[3]);
		}
		(void)send_ack_report(fd, report[2], 0x00, debug);
	}
}

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: aikb_hid_input [--hid PATH] [--poll-ms N] [--debounce-ms N]\\n"
		"                      [--reverse] [--debug] [--no-hid]\\n");
}

static int parse_uint(const char *s, unsigned *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || !end || *end || v > 1000000ul)
		return -1;
	*out = (unsigned)v;
	return 0;
}

static int parse_args(int argc, char **argv, struct config *cfg)
{
	int i;

	cfg->hid_path = "/dev/hidg0";
	cfg->poll_ms = 2;
	cfg->debounce_ms = 12;
	cfg->debug = false;
	cfg->reverse = false;
	cfg->no_hid = false;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--hid") == 0 && i + 1 < argc) {
			cfg->hid_path = argv[++i];
		} else if (strcmp(argv[i], "--poll-ms") == 0 && i + 1 < argc) {
			if (parse_uint(argv[++i], &cfg->poll_ms) != 0)
				return -1;
		} else if (strcmp(argv[i], "--debounce-ms") == 0 && i + 1 < argc) {
			if (parse_uint(argv[++i], &cfg->debounce_ms) != 0)
				return -1;
		} else if (strcmp(argv[i], "--reverse") == 0) {
			cfg->reverse = true;
		} else if (strcmp(argv[i], "--debug") == 0) {
			cfg->debug = true;
		} else if (strcmp(argv[i], "--no-hid") == 0) {
			cfg->no_hid = true;
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			exit(0);
		} else {
			usage(stderr);
			return -1;
		}
	}

	if (cfg->poll_ms == 0)
		cfg->poll_ms = 1;

	return 0;
}

int main(int argc, char **argv)
{
	static const int8_t qdec[16] = {
		0, -1, 1, 0,
		1, 0, 0, -1,
		-1, 0, 0, 1,
		0, 1, -1, 0,
	};
	struct config cfg;
	struct mmio_page pinmux = { 0 };
	struct mmio_page gpio = { 0 };
	struct debouncer keys[3];
	struct debouncer enc_sw;
	uint64_t next_hid_try_ms = 0;
	uint32_t ext_port;
	unsigned enc_state;
	int enc_acc = 0;
	int pending_delta = 0;
	bool pending_report = true;
	uint8_t key_bits;
	int mem_fd = -1;
	int hid_fd = -1;
	size_t i;

	if (parse_args(argc, argv, &cfg) != 0)
		return 2;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (mem_fd < 0) {
		fprintf(stderr, "aikb_hid_input: open /dev/mem failed: %s\n",
			strerror(errno));
		return 1;
	}

	if (mmio_map(mem_fd, &pinmux, PAGE_ALIGN_DOWN(PINMUX_BASE, 4096),
		     PINMUX_PAGE_SIZE) != 0 ||
	    mmio_map(mem_fd, &gpio, PAGE_ALIGN_DOWN(GPIOA_BASE, 4096),
		     GPIO_PAGE_SIZE) != 0) {
		mmio_unmap(&pinmux);
		mmio_unmap(&gpio);
		close(mem_fd);
		return 1;
	}

	configure_board_inputs(&pinmux, &gpio, cfg.debug);

	ext_port = mmio_read32(&gpio, GPIOA_BASE + GPIO_EXT_PORTA);
	for (i = 0; i < ARRAY_SIZE(g_keys); i++) {
		debounce_init(&keys[i],
			      active_low_pressed(ext_port, g_keys[i].gpio_bit),
			      now_ms());
	}
	debounce_init(&enc_sw, active_low_pressed(ext_port, g_enc_e.gpio_bit),
		      now_ms());
	enc_state = (gpio_level(ext_port, g_enc_a.gpio_bit) ? 2u : 0u) |
		    (gpio_level(ext_port, g_enc_b.gpio_bit) ? 1u : 0u);

	while (!g_stop) {
		uint64_t now = now_ms();
		unsigned new_enc_state;

		if (!cfg.no_hid && hid_fd < 0 && now >= next_hid_try_ms) {
			hid_fd = open_hid(cfg.hid_path);
			if (hid_fd < 0) {
				if (cfg.debug) {
					fprintf(stderr,
						"aikb_hid_input: waiting for %s: %s\n",
						cfg.hid_path, strerror(errno));
				}
				next_hid_try_ms = now + 1000u;
			} else {
				pending_report = true;
				if (cfg.debug)
					fprintf(stderr, "aikb_hid_input: opened %s\n",
						cfg.hid_path);
			}
		}

		ext_port = mmio_read32(&gpio, GPIOA_BASE + GPIO_EXT_PORTA);
		for (i = 0; i < ARRAY_SIZE(g_keys); i++) {
			bool raw = active_low_pressed(ext_port, g_keys[i].gpio_bit);
			if (debounce_update(&keys[i], raw, now, cfg.debounce_ms))
				pending_report = true;
		}
		if (debounce_update(&enc_sw,
				    active_low_pressed(ext_port, g_enc_e.gpio_bit),
				    now, cfg.debounce_ms)) {
			pending_report = true;
		}

		new_enc_state = (gpio_level(ext_port, g_enc_a.gpio_bit) ? 2u : 0u) |
				(gpio_level(ext_port, g_enc_b.gpio_bit) ? 1u : 0u);
		if (new_enc_state != enc_state) {
			int step = qdec[(enc_state << 2) | new_enc_state];
			if (cfg.reverse)
				step = -step;
			enc_acc += step;
			enc_state = new_enc_state;

			while (enc_acc >= 4) {
				pending_delta++;
				enc_acc -= 4;
			}
			while (enc_acc <= -4) {
				pending_delta--;
				enc_acc += 4;
			}
		}

		if (pending_delta > 127)
			pending_delta = 127;
		if (pending_delta < -127)
			pending_delta = -127;
		if (pending_delta)
			pending_report = true;

		if (hid_fd >= 0)
			drain_output_reports(hid_fd, cfg.debug);

		if (pending_report && (cfg.no_hid || hid_fd >= 0)) {
			int rc = 0;
			key_bits = build_key_bits(keys, &enc_sw);
			if (!cfg.no_hid)
				rc = send_input_report(hid_fd, key_bits, pending_delta,
						       cfg.debug);
			else if (cfg.debug)
				fprintf(stderr,
					"aikb_hid_input: no-hid keys=0x%02x delta=%d\n",
					key_bits, pending_delta);

			if (rc < 0) {
				if (cfg.debug) {
					fprintf(stderr,
						"aikb_hid_input: HID write failed: %s\n",
						strerror(errno));
				}
				close(hid_fd);
				hid_fd = -1;
				next_hid_try_ms = now + 1000u;
			} else if (rc == 0) {
				pending_delta = 0;
				pending_report = false;
			}
		}

		sleep_ms(cfg.poll_ms);
	}

	if (hid_fd >= 0)
		close(hid_fd);
	mmio_unmap(&pinmux);
	mmio_unmap(&gpio);
	close(mem_fd);

	return 0;
}
