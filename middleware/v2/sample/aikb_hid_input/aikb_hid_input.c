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

/*
 * aikb_hid_input
 *
 * Bridge between the board GPIO/encoder hardware and the USB HID gadget
 * exposed at /dev/hidg0. Emits the new vibe-bridge protocol — every report
 * carries a 6-byte header [report_id][cmd][sid_lo][sid_hi][plen_lo][plen_hi]
 * and a session id is allocated by this firmware in response to
 * CMD_REQUEST_SESSION. CMD_VT100_STREAM payloads are forwarded verbatim to
 * the screen FIFO that aikb_lcd_ui consumes; multi-window routing stays in
 * the host daemon for now.
 *
 * See tools/vibe-bridge/docs/hid_protocol.md for the wire format and
 * tools/vibe-bridge/request.md for the architectural contract.
 */

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

/* HID transport parameters — must match the gadget descriptor in
 * buildroot/board/cvitek/SG200X/overlay/etc/init.d/S08usbdev. */
#define HID_REPORT_LEN 64
#define HID_HEADER_SIZE 6
#define HID_MAX_PAYLOAD (HID_REPORT_LEN - HID_HEADER_SIZE)

#define REPORT_ID_HOST_BOUND 0x10
#define REPORT_ID_DEVICE_BOUND 0x20
#define REPORT_ID_FEATURE 0x30

/* Command bytes. Matches src/vibe_bridge/hid_protocol.py::Cmd. */
#define CMD_REQUEST_SESSION 0x01
#define CMD_SESSION_RESPONSE 0x02
#define CMD_SESSION_INVALID 0x03
#define CMD_KEY_EVENT 0x10
#define CMD_ENCODER_EVENT 0x11
#define CMD_WINDOW_SWITCH 0x20
#define CMD_WINDOW_ACTIVATE 0x21
#define CMD_VT100_STREAM 0x30
#define CMD_UI_SCALE_CHANGE 0x40
#define CMD_STATUS_UPDATE 0x50
#define CMD_FEEDBACK_EVENT 0x60
#define CMD_ERROR 0xFF

/* Status bytes for CMD_SESSION_RESPONSE / CMD_SESSION_INVALID payloads. */
#define SESSION_OK 0x00
#define SESSION_CREATED 0x01
#define SESSION_INVALID_S 0x02
#define SESSION_EXPIRED 0x03
#define SESSION_POOL_FULL 0x04
#define SESSION_RECLAIMED 0x05

#define SESSION_BROADCAST 0u
#define MAX_SESSIONS 256
#define PLUGIN_HINT_MAX 24

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
	const char *screen_out_path;
	const char *ctrl_out_path;
	const char *event_out_path;
	unsigned poll_ms;
	unsigned debounce_ms;
	bool debug;
	bool reverse;
	bool no_hid;
};

struct session_entry {
	bool used;
	uint64_t last_active_ms;
	char plugin_hint[PLUGIN_HINT_MAX];
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

/* Session table. Index 0 is reserved as the broadcast sid; valid sids occupy
 * 1..MAX_SESSIONS so the C array indexes the wire sid directly. */
static struct session_entry g_sessions[MAX_SESSIONS + 1];
static uint16_t g_next_sid = 1;

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

/* Builds a HID frame with the new 6-byte header and writes it. plen is
 * truncated to HID_MAX_PAYLOAD; trailing bytes are zero-padded so the report
 * is always exactly HID_REPORT_LEN. */
static int send_hid_packet(int hid_fd, uint8_t report_id, uint8_t cmd,
			   uint16_t sid, const uint8_t *payload, uint16_t plen,
			   bool debug)
{
	uint8_t report[HID_REPORT_LEN];
	int rc;

	if (plen > HID_MAX_PAYLOAD)
		plen = HID_MAX_PAYLOAD;

	memset(report, 0, sizeof(report));
	report[0] = report_id;
	report[1] = cmd;
	report[2] = (uint8_t)(sid & 0xffu);
	report[3] = (uint8_t)((sid >> 8) & 0xffu);
	report[4] = (uint8_t)(plen & 0xffu);
	report[5] = (uint8_t)((plen >> 8) & 0xffu);
	if (plen && payload)
		memcpy(&report[HID_HEADER_SIZE], payload, plen);

	rc = write_hid_report(hid_fd, report);
	if (debug && rc == 0) {
		fprintf(stderr,
			"aikb_hid_input: tx id=0x%02x cmd=0x%02x sid=%u plen=%u\n",
			report_id, cmd, sid, plen);
	}
	return rc;
}

/* ------------------------------------------------------------- session API */

static bool session_alive(uint16_t sid)
{
	return sid >= 1 && sid <= MAX_SESSIONS && g_sessions[sid].used;
}

static void touch_session(uint16_t sid, uint64_t now)
{
	if (session_alive(sid))
		g_sessions[sid].last_active_ms = now;
}

static void copy_hint(struct session_entry *e, const uint8_t *hint, uint16_t hint_len)
{
	uint16_t copy = hint_len;
	if (copy > PLUGIN_HINT_MAX - 1)
		copy = PLUGIN_HINT_MAX - 1;
	if (copy && hint)
		memcpy(e->plugin_hint, hint, copy);
	e->plugin_hint[copy] = '\0';
}

static uint16_t pick_lru(void)
{
	uint16_t lru = 0;
	uint64_t oldest = UINT64_MAX;
	uint16_t s;

	for (s = 1; s <= MAX_SESSIONS; s++) {
		if (g_sessions[s].used && g_sessions[s].last_active_ms < oldest) {
			lru = s;
			oldest = g_sessions[s].last_active_ms;
		}
	}
	return lru;
}

/* Allocate a fresh sid. On pool-full, evicts the LRU entry and returns its
 * sid in *evicted_out so the caller can notify the old owner with
 * CMD_SESSION_INVALID(RECLAIMED) before announcing the new ownership. */
static uint16_t alloc_session(const uint8_t *hint, uint16_t hint_len,
			      uint64_t now, uint16_t *evicted_out)
{
	int probe;
	uint16_t lru;

	*evicted_out = 0;

	for (probe = 0; probe < MAX_SESSIONS; probe++) {
		uint16_t sid = g_next_sid;
		g_next_sid++;
		if (g_next_sid > MAX_SESSIONS)
			g_next_sid = 1;
		if (!g_sessions[sid].used) {
			g_sessions[sid].used = true;
			g_sessions[sid].last_active_ms = now;
			copy_hint(&g_sessions[sid], hint, hint_len);
			return sid;
		}
	}

	lru = pick_lru();
	if (lru == 0)
		return 0;

	*evicted_out = lru;
	g_sessions[lru].last_active_ms = now;
	copy_hint(&g_sessions[lru], hint, hint_len);
	return lru;
}

/* ----------------------------------------------------------- screen output */

static int open_screen_out(const char *path, bool debug)
{
	struct stat st;
	int flags = O_WRONLY | O_NONBLOCK | O_CLOEXEC;
	int fd;

	if (!path)
		return -1;

	if (stat(path, &st) < 0) {
		if (errno == ENOENT) {
			if (mkfifo(path, 0600) != 0 && errno != EEXIST) {
				if (debug) {
					fprintf(stderr,
						"aikb_hid_input: mkfifo %s failed: %s\n",
						path, strerror(errno));
				}
				return -1;
			}
		} else {
			if (debug) {
				fprintf(stderr,
					"aikb_hid_input: stat %s failed: %s\n",
					path, strerror(errno));
			}
			return -1;
		}
	}

	if (stat(path, &st) == 0 && !S_ISFIFO(st.st_mode))
		flags |= O_CREAT | O_APPEND;

	fd = open(path, flags, 0600);
	if (fd < 0 && debug) {
		fprintf(stderr, "aikb_hid_input: waiting for screen output %s: %s\n",
			path, strerror(errno));
	}

	return fd;
}

static int write_screen_bytes(int *screen_fd, const char *path,
			      const uint8_t *data, size_t len, bool debug)
{
	size_t off = 0;

	if (!path || len == 0)
		return 0;

	if (*screen_fd < 0) {
		*screen_fd = open_screen_out(path, debug);
		if (*screen_fd < 0)
			return 1;
	}

	while (off < len) {
		ssize_t n = write(*screen_fd, data + off, len - off);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 1;
			if (debug) {
				fprintf(stderr,
					"aikb_hid_input: screen output write failed: %s\n",
					strerror(errno));
			}
			close(*screen_fd);
			*screen_fd = -1;
			return -1;
		}
		if (n == 0)
			return 1;
		off += (size_t)n;
	}

	return 0;
}

/* ----------------------------------------------------------- input reports */

static int send_key_event(int hid_fd, uint8_t key_bits, bool debug)
{
	return send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND, CMD_KEY_EVENT,
			       SESSION_BROADCAST, &key_bits, 1, debug);
}

static int send_encoder_event(int hid_fd, int delta, bool debug)
{
	uint8_t payload;

	if (delta > 127)
		delta = 127;
	if (delta < -127)
		delta = -127;
	payload = (uint8_t)(int8_t)delta;
	return send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND, CMD_ENCODER_EVENT,
			       SESSION_BROADCAST, &payload, 1, debug);
}

static int write_event_line(int *event_fd, const char *path, const char *line,
			    bool debug)
{
	size_t len;

	if (!path || !line)
		return 0;
	len = strlen(line);
	return write_screen_bytes(event_fd, path, (const uint8_t *)line, len,
				  debug);
}

static void emit_key_down_events(int *event_fd, const struct config *cfg,
				 uint8_t old_bits, uint8_t new_bits)
{
	static const char *key_lines[3] = {
		"KEY 0 DOWN\n",
		"KEY 1 DOWN\n",
		"KEY 2 DOWN\n",
	};

	if (!cfg->event_out_path)
		return;
	for (size_t i = 0; i < 3; i++) {
		uint8_t bit = (uint8_t)(1u << i);

		if ((new_bits & bit) && !(old_bits & bit))
			(void)write_event_line(event_fd, cfg->event_out_path,
					       key_lines[i], cfg->debug);
	}
	if ((new_bits & 0x80u) && !(old_bits & 0x80u))
		(void)write_event_line(event_fd, cfg->event_out_path,
				       "ENC_BTN DOWN\n", cfg->debug);
}

static void emit_encoder_events(int *event_fd, const struct config *cfg,
				int delta)
{
	const char *line;

	if (!cfg->event_out_path || delta == 0)
		return;
	line = delta > 0 ? "ENC +1\n" : "ENC -1\n";
	while (delta > 0) {
		(void)write_event_line(event_fd, cfg->event_out_path, line,
				       cfg->debug);
		delta--;
	}
	while (delta < 0) {
		(void)write_event_line(event_fd, cfg->event_out_path, line,
				       cfg->debug);
		delta++;
	}
}

/* ---------------------------------------------------------- packet handler */

static int write_ctrl_line(int *ctrl_fd, const char *path, const char *line,
			   bool debug)
{
	size_t len = strlen(line);
	size_t off = 0;

	if (!path || len == 0)
		return 0;
	if (*ctrl_fd < 0) {
		*ctrl_fd = open_screen_out(path, debug);
		if (*ctrl_fd < 0)
			return 1;
	}
	while (off < len) {
		ssize_t n = write(*ctrl_fd, line + off, len - off);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 1;
			if (debug) {
				fprintf(stderr,
					"aikb_hid_input: ctrl write failed: %s\n",
					strerror(errno));
			}
			close(*ctrl_fd);
			*ctrl_fd = -1;
			return -1;
		}
		if (n == 0)
			return 1;
		off += (size_t)n;
	}
	return 0;
}

static void handle_packet(int hid_fd, int *screen_fd, int *ctrl_fd,
			  const struct config *cfg,
			  const uint8_t *report, ssize_t report_len)
{
	uint8_t cmd;
	uint16_t sid;
	uint16_t plen;
	const uint8_t *payload;
	uint64_t now;

	if (report_len < (ssize_t)HID_HEADER_SIZE) {
		if (cfg->debug)
			fprintf(stderr,
				"aikb_hid_input: short packet len=%zd\n", report_len);
		return;
	}
	if (report[0] != REPORT_ID_DEVICE_BOUND) {
		if (cfg->debug)
			fprintf(stderr,
				"aikb_hid_input: ignored report id=0x%02x\n", report[0]);
		return;
	}

	cmd = report[1];
	sid = (uint16_t)report[2] | ((uint16_t)report[3] << 8);
	plen = (uint16_t)report[4] | ((uint16_t)report[5] << 8);
	if (plen > HID_MAX_PAYLOAD ||
	    (size_t)plen + HID_HEADER_SIZE > (size_t)report_len) {
		if (cfg->debug)
			fprintf(stderr,
				"aikb_hid_input: truncated payload plen=%u rlen=%zd\n",
				plen, report_len);
		return;
	}
	payload = report + HID_HEADER_SIZE;
	now = now_ms();

	switch (cmd) {
	case CMD_REQUEST_SESSION: {
		uint16_t evicted = 0;
		uint16_t new_sid = alloc_session(payload, plen, now, &evicted);

		if (evicted) {
			uint8_t st = SESSION_RECLAIMED;
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_INVALID, evicted,
					      &st, 1, cfg->debug);
		}
		if (new_sid != 0) {
			uint8_t st = SESSION_CREATED;
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_RESPONSE, new_sid,
					      &st, 1, cfg->debug);
		} else {
			uint8_t st = SESSION_POOL_FULL;
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_INVALID,
					      SESSION_BROADCAST, &st, 1,
					      cfg->debug);
		}
		break;
	}

	case CMD_VT100_STREAM:
		if (!session_alive(sid)) {
			uint8_t st = SESSION_INVALID_S;
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_INVALID, sid, &st, 1,
					      cfg->debug);
			break;
		}
		touch_session(sid, now);
		if (cfg->screen_out_path && plen > 0) {
			(void)write_screen_bytes(screen_fd, cfg->screen_out_path,
						 payload, plen, cfg->debug);
		}
		break;

	case CMD_STATUS_UPDATE:
		if (session_alive(sid))
			touch_session(sid, now);
		/* Status payload is opaque to firmware; the host owns its meaning. */
		break;

	case CMD_UI_SCALE_CHANGE:
		/*
		 * payload[0] = cell_w, payload[1] = cell_h. Forwarded as a
		 * single ASCII line ("cell W H\n") to aikb_lcd_ui's --ctrl
		 * FIFO so the renderer can resize at runtime. Sid is ignored
		 * because cell geometry is global to the panel.
		 */
		if (cfg->ctrl_out_path && plen >= 2) {
			char line[32];
			int n = snprintf(line, sizeof(line), "cell %u %u\n",
					 (unsigned)payload[0],
					 (unsigned)payload[1]);
			if (n > 0 && n < (int)sizeof(line)) {
				(void)write_ctrl_line(ctrl_fd,
						      cfg->ctrl_out_path,
						      line, cfg->debug);
			}
		} else if (cfg->debug) {
			fprintf(stderr,
				"aikb_hid_input: ui_scale dropped sid=%u plen=%u (no --ctrl-out)\n",
				sid, plen);
		}
		break;

	case CMD_WINDOW_SWITCH:
	case CMD_WINDOW_ACTIVATE:
		/* Multi-window routing lives in the host daemon for now. The
		 * firmware will gain real handlers once aikb_lcd_ui learns to
		 * keep multiple buffers. */
		if (cfg->debug) {
			fprintf(stderr,
				"aikb_hid_input: cmd 0x%02x sid=%u (no-op in firmware)\n",
				cmd, sid);
		}
		break;

	default:
		if (cfg->debug) {
			fprintf(stderr,
				"aikb_hid_input: unknown cmd 0x%02x sid=%u\n",
				cmd, sid);
		}
		break;
	}
}

static void drain_packets(int hid_fd, int *screen_fd, int *ctrl_fd,
			  const struct config *cfg)
{
	for (;;) {
		uint8_t report[HID_REPORT_LEN];
		ssize_t n = read(hid_fd, report, sizeof(report));

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			return;
		}

		if (n == 0)
			return;

		handle_packet(hid_fd, screen_fd, ctrl_fd, cfg, report, n);
	}
}

/* -------------------------------------------------------------- arg parser */

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: aikb_hid_input [--hid PATH] [--screen-out PATH]\n"
		"                      [--ctrl-out PATH] [--event-out PATH]\n"
		"                      [--poll-ms N] [--debounce-ms N]\n"
		"                      [--reverse] [--debug] [--no-hid]\n");
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
	cfg->screen_out_path = NULL;
	cfg->ctrl_out_path = NULL;
	cfg->event_out_path = NULL;
	cfg->poll_ms = 2;
	cfg->debounce_ms = 12;
	cfg->debug = false;
	cfg->reverse = false;
	cfg->no_hid = false;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--hid") == 0 && i + 1 < argc) {
			cfg->hid_path = argv[++i];
		} else if (strcmp(argv[i], "--screen-out") == 0 && i + 1 < argc) {
			cfg->screen_out_path = argv[++i];
		} else if (strcmp(argv[i], "--ctrl-out") == 0 && i + 1 < argc) {
			cfg->ctrl_out_path = argv[++i];
		} else if (strcmp(argv[i], "--event-out") == 0 && i + 1 < argc) {
			cfg->event_out_path = argv[++i];
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

/* ------------------------------------------------------------------- main */

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
	int event_delta_pending = 0;
	bool keys_dirty = true;
	uint8_t key_bits;
	uint8_t event_prev_key_bits = 0;
	int mem_fd = -1;
	int hid_fd = -1;
	int screen_fd = -1;
	int ctrl_fd = -1;
	int event_fd = -1;
	size_t i;

	if (parse_args(argc, argv, &cfg) != 0)
		return 2;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

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
	event_prev_key_bits = build_key_bits(keys, &enc_sw);

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
				keys_dirty = true;
				if (cfg.debug)
					fprintf(stderr, "aikb_hid_input: opened %s\n",
						cfg.hid_path);
			}
		}

		ext_port = mmio_read32(&gpio, GPIOA_BASE + GPIO_EXT_PORTA);
		for (i = 0; i < ARRAY_SIZE(g_keys); i++) {
			bool raw = active_low_pressed(ext_port, g_keys[i].gpio_bit);
			if (debounce_update(&keys[i], raw, now, cfg.debounce_ms))
				keys_dirty = true;
		}
		if (debounce_update(&enc_sw,
				    active_low_pressed(ext_port, g_enc_e.gpio_bit),
				    now, cfg.debounce_ms)) {
			keys_dirty = true;
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
				event_delta_pending++;
				enc_acc -= 4;
			}
			while (enc_acc <= -4) {
				pending_delta--;
				event_delta_pending--;
				enc_acc += 4;
			}
		}

		if (pending_delta > 127)
			pending_delta = 127;
		if (pending_delta < -127)
			pending_delta = -127;

		if (event_delta_pending > 127)
			event_delta_pending = 127;
		if (event_delta_pending < -127)
			event_delta_pending = -127;

		if (keys_dirty) {
			key_bits = build_key_bits(keys, &enc_sw);
			emit_key_down_events(&event_fd, &cfg, event_prev_key_bits,
					     key_bits);
			event_prev_key_bits = key_bits;
		}
		if (event_delta_pending) {
			emit_encoder_events(&event_fd, &cfg, event_delta_pending);
			event_delta_pending = 0;
		}

		if (hid_fd >= 0)
			drain_packets(hid_fd, &screen_fd, &ctrl_fd, &cfg);

		if (cfg.no_hid) {
			if (cfg.debug && (keys_dirty || pending_delta)) {
				key_bits = build_key_bits(keys, &enc_sw);
				fprintf(stderr,
					"aikb_hid_input: no-hid keys=0x%02x delta=%d\n",
					key_bits, pending_delta);
			}
			keys_dirty = false;
			pending_delta = 0;
		} else if (hid_fd >= 0) {
			int rc = 0;

			if (keys_dirty) {
				key_bits = build_key_bits(keys, &enc_sw);
				rc = send_key_event(hid_fd, key_bits, cfg.debug);
				if (rc == 0)
					keys_dirty = false;
			}

			if (rc == 0 && pending_delta) {
				rc = send_encoder_event(hid_fd, pending_delta, cfg.debug);
				if (rc == 0)
					pending_delta = 0;
			}

			if (rc < 0) {
				if (cfg.debug) {
					fprintf(stderr,
						"aikb_hid_input: HID write failed: %s\n",
						strerror(errno));
				}
				close(hid_fd);
				hid_fd = -1;
				next_hid_try_ms = now + 1000u;
			}
		}

		sleep_ms(cfg.poll_ms);
	}

	if (hid_fd >= 0)
		close(hid_fd);
	if (screen_fd >= 0)
		close(screen_fd);
	if (ctrl_fd >= 0)
		close(ctrl_fd);
	if (event_fd >= 0)
		close(event_fd);
	mmio_unmap(&pinmux);
	mmio_unmap(&gpio);
	close(mem_fd);

	return 0;
}
