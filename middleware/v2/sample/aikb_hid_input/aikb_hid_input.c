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
#include <sys/wait.h>
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
#define GPIOE_BASE 0x05021000u
#define GPIO_PAGE_SIZE 0x1000u
#define IOBLK_RTC_BASE 0x05027000u
#define IOBLK_RTC_PAGE_SIZE 0x1000u

#define GPIO_SWPORTA_DDR 0x04u
#define GPIO_EXT_PORTA 0x50u

#define IOBLK_PULL_UP_BIT BIT_U32(2)
#define IOBLK_PULL_DOWN_BIT BIT_U32(3)
#define IOBLK_NONE 0u
#define PINMUX_FUNC_MASK 0x7u
#define PINMUX_FUNC_GPIO 0x3u

/* HID transport parameters — must match the gadget descriptor in
 * buildroot/board/cvitek/SG200X/overlay/etc/init.d/S08usbdev. */
#define HID_REPORT_LEN 64
#define HID_HEADER_SIZE 6
#define HID_MAX_PAYLOAD (HID_REPORT_LEN - HID_HEADER_SIZE)
#define KBD_REPORT_LEN 8

#define REPORT_ID_HOST_BOUND 0x10
#define REPORT_ID_DEVICE_BOUND 0x20
#define REPORT_ID_FEATURE 0x30

/* Command bytes. Matches src/vibe_bridge/hid_protocol.py::Cmd. */
#define CMD_REQUEST_SESSION 0x01
#define CMD_SESSION_RESPONSE 0x02
#define CMD_SESSION_INVALID 0x03
#define CMD_SESSION_HEARTBEAT 0x04   /* H->B every 10s per live sid; touch last_hb */
#define CMD_SESSION_FOCUS 0x05       /* B->H when board selects a sid */
#define CMD_KEY_EVENT 0x10
#define CMD_ENCODER_EVENT 0x11
#define CMD_PERMISSION_RES 0x12      /* B->H: req_id(8B LE) + decision(allow/deny/always) */
#define CMD_WINDOW_SWITCH 0x20       /* deprecated: board ignores */
#define CMD_WINDOW_ACTIVATE 0x21     /* deprecated: board ignores */
#define CMD_VT100_STREAM 0x30
#define CMD_UI_SCALE_CHANGE 0x40
#define CMD_STATUS_UPDATE 0x50
#define CMD_TOKEN_USAGE 0x51    /* H->B: token in/out/cost_cents per sid (3x u64 LE) */
#define CMD_TURN_APPEND 0x52    /* H->B: conversation turn chunk; role(1B) + text */
#define CMD_PERMISSION_REQ 0x53 /* H->B: pending permission; req_id(8B) + tool_len(1B) + tool + args */
#define CMD_AGENT_META 0x54     /* H->B: agent meta; kind(1B) + cwd_len(1B) + cwd + branch */
#define CMD_FEEDBACK_EVENT 0x60
#define CMD_ERROR 0xFF

/* Status bytes for CMD_SESSION_RESPONSE / CMD_SESSION_INVALID payloads. */
#define SESSION_OK 0x00
#define SESSION_CREATED 0x01
#define SESSION_INVALID_S 0x02
#define SESSION_EXPIRED 0x03
#define SESSION_POOL_FULL 0x04
#define SESSION_RECLAIMED 0x05
#define SESSION_DISCONNECTED 0x06

/* State bytes carried in CMD_STATUS_UPDATE payload[0]. Board grid shows the
 * latest value for each sid (in addition to the heartbeat-derived CONNECTED /
 * DISCONNECTED indicator). */
#define SESSION_STATE_CONNECTED 0x00
#define SESSION_STATE_DISCONNECTED 0x01
#define SESSION_STATE_RUN 0x02
#define SESSION_STATE_WAIT 0x03
#define SESSION_STATE_DONE 0x04
#define SESSION_STATE_ERROR 0x05

/* Heartbeat policy: 30s without CMD_SESSION_HEARTBEAT marks DISCONNECTED;
 * 60s without it frees the slot. */
#define SESSION_HEARTBEAT_TIMEOUT_MS 30000u
#define SESSION_GC_TIMEOUT_MS 60000u

#define SESSION_BROADCAST 0u
#define MAX_SESSIONS 256
#define PLUGIN_HINT_MAX 24
#define KEY_COUNT 7
#define ENCODER_STEPS_PER_EVENT 2
#define REJECT_KEY_INDEX 0
#define VOICE_KEY_INDEX 1
#define CONFIRM_KEY_INDEX 6
#define AIKB_USB_MIC_BRIDGE "/mnt/system/usr/bin/aikb_usb_mic_bridge"
#define AIKB_NOTIFY_TONE "/mnt/system/usr/bin/aikb_notify_tone"
#define NOTIFY_TONE_COOLDOWN_MS 800u
#define AIKB_PROFILE_CONFIG_DIR "/mnt/system/etc/aikb"
#define AIKB_PROFILE_CONFIG "/mnt/system/etc/aikb/profiles.conf"
#define AIKB_PROFILE_BOOT_CONFIG "/boot/aikb_profiles.conf"
#define AIKB_PROFILE_ID_MAX 16
#define AIKB_PROFILE_SOUND_VOLUME_DEFAULT 35u
#define VIBE_ACTION_NONE 0xffu

#define HID_MOD_LEFT_CTRL 0x01u
#define HID_USAGE_A 0x04u
#define HID_USAGE_C 0x06u
#define HID_USAGE_V 0x19u
#define HID_USAGE_ENTER 0x28u
#define HID_USAGE_ESC 0x29u
#define HID_USAGE_TAB 0x2bu
#define HID_USAGE_SPACE 0x2cu

enum notify_tone_kind {
	NOTIFY_TONE_PENDING = 0,
	NOTIFY_TONE_DONE,
	NOTIFY_TONE_ERROR,
	NOTIFY_TONE_COUNT,
};

enum gpio_bank {
	GPIO_BANK_A,
	GPIO_BANK_E,
};

struct mmio_page {
	uint32_t base;
	size_t size;
	volatile uint8_t *ptr;
};

struct pin_def {
	const char *name;
	enum gpio_bank bank;
	uint8_t gpio_bit;
	uint32_t pinmux_addr;
	uint32_t ioblk_addr;
	uint32_t ioblk_alt_addr;
};

struct debouncer {
	bool stable;
	bool raw;
	uint64_t raw_since_ms;
};

struct config {
	const char *hid_path;
	const char *kbd_hid_path;
	const char *screen_out_path;
	const char *ctrl_out_path;
	const char *event_out_path;
	const char *ui_ctrl_in_path;
	unsigned poll_ms;
	unsigned debounce_ms;
	bool debug;
	bool reverse;
	bool no_hid;
};

struct key_binding {
	uint8_t modifier;
	uint8_t usage;
};

struct session_entry {
	bool used;
	bool disconnected;
	uint8_t state_byte;          /* last SESSION_STATE_* received from host */
	uint64_t last_active_ms;
	uint64_t last_heartbeat_ms;
	char plugin_hint[PLUGIN_HINT_MAX];
};

struct profile_config {
	char active_profile[AIKB_PROFILE_ID_MAX];
	bool sound_enabled;
	unsigned sound_volume;
	struct key_binding keymap[KEY_COUNT];
	uint8_t actionmap[KEY_COUNT];
};

/* View state owned by aikb_lcd_ui; mirrored here so KEY/ENC HID upstream knows
 * which sid to stamp on each event. lcd_ui announces transitions via the
 * --ui-ctrl-in FIFO (lines: "view picker|terminal", "select N", "focus N"). */
enum board_view {
	BOARD_VIEW_TERMINAL = 0,
	BOARD_VIEW_PICKER = 1,
};

static enum board_view g_view = BOARD_VIEW_TERMINAL;
static uint16_t g_active_sid = 0;    /* terminal view: sid whose VT100 we render */
static uint16_t g_selected_sid = 0;  /* picker view: sid currently highlighted */
static uint64_t g_diag_vt100_rx_pkts;
static uint64_t g_diag_vt100_rx_bytes;
static uint64_t g_diag_vt100_fwd_pkts;
static uint64_t g_diag_vt100_fwd_bytes;
static uint64_t g_diag_vt100_drop_dead;
static uint64_t g_diag_vt100_drop_sid;
static uint64_t g_diag_vt100_write_retry;
static uint64_t g_diag_vt100_write_fail;
static uint64_t g_notify_tone_last_ms[NOTIFY_TONE_COUNT];
static struct profile_config g_profile = {
	.active_profile = "vibe",
	.sound_enabled = false,
	.sound_volume = AIKB_PROFILE_SOUND_VOLUME_DEFAULT,
};

struct key_preset {
	const char *id;
	uint8_t modifier;
	uint8_t usage;
};

struct vibe_action_preset {
	const char *id;
	uint8_t bit;
};

static const struct key_preset g_key_presets[] = {
	{ "none", 0, 0 },
	{ "esc", 0, HID_USAGE_ESC },
	{ "tab", 0, HID_USAGE_TAB },
	{ "space", 0, HID_USAGE_SPACE },
	{ "enter", 0, HID_USAGE_ENTER },
	{ "ctrl_a", HID_MOD_LEFT_CTRL, HID_USAGE_A },
	{ "ctrl_c", HID_MOD_LEFT_CTRL, HID_USAGE_C },
	{ "ctrl_v", HID_MOD_LEFT_CTRL, HID_USAGE_V },
};

static const struct vibe_action_preset g_vibe_action_presets[] = {
	{ "none", VIBE_ACTION_NONE },
	{ "reject", 0 },
	{ "voice", 1 },
	{ "session", 2 },
	{ "review", 3 },
	{ "vote_review", 3 },
	{ "sleep", 4 },
	{ "agent_model", 4 },
	{ "multi", 5 },
	{ "multi_function", 5 },
	{ "confirm", 6 },
};

static const struct key_binding g_default_keymap[KEY_COUNT] = {
	{ 0, HID_USAGE_ESC },
	{ 0, HID_USAGE_TAB },
	{ 0, HID_USAGE_SPACE },
	{ HID_MOD_LEFT_CTRL, HID_USAGE_A },
	{ HID_MOD_LEFT_CTRL, HID_USAGE_C },
	{ HID_MOD_LEFT_CTRL, HID_USAGE_V },
	{ 0, HID_USAGE_ENTER },
};

static const uint8_t g_default_actionmap[KEY_COUNT] = {
	0, 1, 2, 3, 4, 5, 6,
};

static const struct pin_def g_keys[] = {
	{ "key0_A15", GPIO_BANK_A, 15, 0x0300103cu, 0x03001908u, IOBLK_NONE },
	{ "key1_A22", GPIO_BANK_A, 22, 0x03001050u, 0x0300191cu, IOBLK_NONE },
	{ "key2_A25", GPIO_BANK_A, 25, 0x03001054u, 0x03001920u, IOBLK_NONE },
	{ "key3_A27", GPIO_BANK_A, 27, 0x03001058u, 0x03001924u, IOBLK_NONE },
	{ "key4_A23", GPIO_BANK_A, 23, 0x0300105cu, 0x03001928u, IOBLK_NONE },
	{ "key5_A24", GPIO_BANK_A, 24, 0x03001060u, 0x0300192cu, IOBLK_NONE },
	{ "key6_P19", GPIO_BANK_E, 19, 0x030010d4u, 0x05027090u, 0x0502705cu },
};

static const struct pin_def g_enc_a = {
	"encA_P22", GPIO_BANK_E, 22, 0x030010e0u, 0x0502709cu, 0x05027068u
};

static const struct pin_def g_enc_b = {
	"encB_P23", GPIO_BANK_E, 23, 0x030010e4u, 0x050270a0u, 0x0502706cu
};

static const struct pin_def g_enc_e = {
	"encE_A30", GPIO_BANK_A, 30, 0x03001078u, IOBLK_NONE, IOBLK_NONE
};

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reload_profile;

/* Session table. Index 0 is reserved as the broadcast sid; valid sids occupy
 * 1..MAX_SESSIONS so the C array indexes the wire sid directly. */
static struct session_entry g_sessions[MAX_SESSIONS + 1];
static uint16_t g_next_sid = 1;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void on_reload_signal(int sig)
{
	(void)sig;
	g_reload_profile = 1;
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

static struct mmio_page *ioblk_page_for_pin(struct mmio_page *pinmux,
					    struct mmio_page *rtc_ioblk,
					    const struct pin_def *pin)
{
	if (pin->ioblk_addr >= IOBLK_RTC_BASE &&
	    pin->ioblk_addr < IOBLK_RTC_BASE + IOBLK_RTC_PAGE_SIZE)
		return rtc_ioblk;
	return pinmux;
}

static const char *gpio_bank_name(enum gpio_bank bank)
{
	return bank == GPIO_BANK_E ? "E" : "A";
}

static uint32_t enable_pin_pull_up(struct mmio_page *pinmux,
				   struct mmio_page *rtc_ioblk,
				   uint32_t addr)
{
	struct pin_def tmp = {
		.ioblk_addr = addr,
	};
	struct mmio_page *ioblk_page;
	uint32_t ioblk;

	ioblk_page = ioblk_page_for_pin(pinmux, rtc_ioblk, &tmp);
	ioblk = mmio_read32(ioblk_page, addr);
	ioblk |= IOBLK_PULL_UP_BIT;
	ioblk &= ~IOBLK_PULL_DOWN_BIT;
	mmio_write32(ioblk_page, addr, ioblk);
	return ioblk;
}

static void configure_pin(struct mmio_page *pinmux,
			  struct mmio_page *rtc_ioblk,
			  const struct pin_def *pin, bool debug)
{
	uint32_t mux = mmio_read32(pinmux, pin->pinmux_addr);
	uint32_t ioblk = 0;
	uint32_t ioblk_alt = 0;

	mux = (mux & ~PINMUX_FUNC_MASK) | PINMUX_FUNC_GPIO;

	mmio_write32(pinmux, pin->pinmux_addr, mux);
	if (pin->ioblk_addr != IOBLK_NONE)
		ioblk = enable_pin_pull_up(pinmux, rtc_ioblk, pin->ioblk_addr);
	if (pin->ioblk_alt_addr != IOBLK_NONE)
		ioblk_alt = enable_pin_pull_up(pinmux, rtc_ioblk,
					       pin->ioblk_alt_addr);

	if (debug) {
		if (pin->ioblk_addr == IOBLK_NONE) {
			fprintf(stderr,
				"aikb_hid_input: %-9s gpio=%s%u pinmux=0x%08x ioblk=n/a\n",
				pin->name, gpio_bank_name(pin->bank), pin->gpio_bit,
				mux);
		} else if (pin->ioblk_alt_addr != IOBLK_NONE) {
			fprintf(stderr,
				"aikb_hid_input: %-9s gpio=%s%u pinmux=0x%08x ioblk=0x%08x alt=0x%08x\n",
				pin->name, gpio_bank_name(pin->bank), pin->gpio_bit,
				mux, ioblk, ioblk_alt);
		} else {
			fprintf(stderr,
				"aikb_hid_input: %-9s gpio=%s%u pinmux=0x%08x ioblk=0x%08x\n",
				pin->name, gpio_bank_name(pin->bank), pin->gpio_bit,
				mux, ioblk);
		}
	}
}

static void configure_gpio_input(struct mmio_page *gpio, uint32_t gpio_base,
				 uint32_t mask, const char *name, bool debug)
{
	uint32_t ddr = mmio_read32(gpio, gpio_base + GPIO_SWPORTA_DDR);

	ddr &= ~mask;
	mmio_write32(gpio, gpio_base + GPIO_SWPORTA_DDR, ddr);

	if (debug)
		fprintf(stderr,
			"aikb_hid_input: GPIO%s_DDR=0x%08x input_mask=0x%08x\n",
			name, ddr, mask);
}

static int configure_board_inputs(struct mmio_page *pinmux,
				  struct mmio_page *rtc_ioblk,
				  struct mmio_page *gpio_a,
				  struct mmio_page *gpio_e, bool debug)
{
	uint32_t mask_a = 0;
	uint32_t mask_e = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(g_keys); i++) {
		configure_pin(pinmux, rtc_ioblk, &g_keys[i], debug);
		if (g_keys[i].bank == GPIO_BANK_E)
			mask_e |= BIT_U32(g_keys[i].gpio_bit);
		else
			mask_a |= BIT_U32(g_keys[i].gpio_bit);
	}

	configure_pin(pinmux, rtc_ioblk, &g_enc_a, debug);
	configure_pin(pinmux, rtc_ioblk, &g_enc_b, debug);
	configure_pin(pinmux, rtc_ioblk, &g_enc_e, debug);
	mask_e |= BIT_U32(g_enc_a.gpio_bit);
	mask_e |= BIT_U32(g_enc_b.gpio_bit);
	if (g_enc_e.bank == GPIO_BANK_E)
		mask_e |= BIT_U32(g_enc_e.gpio_bit);
	else
		mask_a |= BIT_U32(g_enc_e.gpio_bit);

	if (mask_a)
		configure_gpio_input(gpio_a, GPIOA_BASE, mask_a, "A", debug);
	if (mask_e)
		configure_gpio_input(gpio_e, GPIOE_BASE, mask_e, "E", debug);

	return 0;
}

static bool gpio_level(uint32_t ext_port, uint8_t bit)
{
	return (ext_port & BIT_U32(bit)) != 0;
}

static bool pin_level(const struct pin_def *pin, uint32_t ext_a, uint32_t ext_e)
{
	uint32_t ext_port = pin->bank == GPIO_BANK_E ? ext_e : ext_a;

	return gpio_level(ext_port, pin->gpio_bit);
}

static bool pin_pressed(const struct pin_def *pin, uint32_t ext_a, uint32_t ext_e)
{
	return !pin_level(pin, ext_a, ext_e);
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

static uint8_t build_key_bits(const struct debouncer keys[KEY_COUNT])
{
	uint8_t bits = 0;
	size_t i;

	for (i = 0; i < KEY_COUNT; i++) {
		if (keys[i].stable)
			bits |= (uint8_t)(1u << i);
	}

	return bits;
}

static uint8_t build_vibe_key_bits(uint8_t physical_bits)
{
	uint8_t bits = 0;

	for (size_t i = 0; i < KEY_COUNT; i++) {
		uint8_t action_bit = g_profile.actionmap[i];

		if (!(physical_bits & (uint8_t)(1u << i)))
			continue;
		if (action_bit < 8u)
			bits |= (uint8_t)(1u << action_bit);
	}
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
			g_sessions[sid].disconnected = false;
			g_sessions[sid].state_byte = SESSION_STATE_CONNECTED;
			g_sessions[sid].last_active_ms = now;
			g_sessions[sid].last_heartbeat_ms = now;
			copy_hint(&g_sessions[sid], hint, hint_len);
			return sid;
		}
	}

	lru = pick_lru();
	if (lru == 0)
		return 0;

	*evicted_out = lru;
	g_sessions[lru].used = true;
	g_sessions[lru].disconnected = false;
	g_sessions[lru].state_byte = SESSION_STATE_CONNECTED;
	g_sessions[lru].last_active_ms = now;
	g_sessions[lru].last_heartbeat_ms = now;
	copy_hint(&g_sessions[lru], hint, hint_len);
	return lru;
}

static const char *session_state_name(uint8_t state)
{
	switch (state) {
	case SESSION_STATE_CONNECTED:    return "connected";
	case SESSION_STATE_DISCONNECTED: return "disconnected";
	case SESSION_STATE_RUN:          return "run";
	case SESSION_STATE_WAIT:         return "wait";
	case SESSION_STATE_DONE:         return "done";
	case SESSION_STATE_ERROR:        return "error";
	default:                         return "unknown";
	}
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

static int send_key_event(int hid_fd, uint16_t sid, uint8_t key_bits,
			  bool enc_pressed, bool debug)
{
	uint8_t payload[2] = {
		key_bits,
		enc_pressed ? 0x01u : 0x00u,
	};

	return send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND, CMD_KEY_EVENT,
			       sid, payload, sizeof(payload), debug);
}

static bool kbd_report_has_usage(const uint8_t report[KBD_REPORT_LEN],
				 uint8_t usage)
{
	for (size_t i = 2; i < KBD_REPORT_LEN; i++) {
		if (report[i] == usage)
			return true;
	}
	return false;
}

static void build_kbd_report(uint8_t key_bits, uint8_t report[KBD_REPORT_LEN])
{
	size_t slot = 2;

	memset(report, 0, KBD_REPORT_LEN);
	for (size_t i = 0; i < KEY_COUNT; i++) {
		const struct key_binding *binding = &g_profile.keymap[i];

		if (!(key_bits & (uint8_t)(1u << i)))
			continue;
		report[0] |= binding->modifier;
		if (!binding->usage || slot >= KBD_REPORT_LEN ||
		    kbd_report_has_usage(report, binding->usage))
			continue;
		report[slot++] = binding->usage;
	}
}

static int write_kbd_report(int fd, const uint8_t report[KBD_REPORT_LEN])
{
	ssize_t n = write(fd, report, KBD_REPORT_LEN);

	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 1;
		return -1;
	}
	if (n != KBD_REPORT_LEN) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int send_kbd_report(int kbd_fd, uint8_t key_bits, bool debug)
{
	uint8_t report[KBD_REPORT_LEN];
	int rc;

	build_kbd_report(key_bits, report);
	rc = write_kbd_report(kbd_fd, report);
	if (debug && rc == 0) {
		fprintf(stderr,
			"aikb_hid_input: kbd tx mod=0x%02x keys=%02x,%02x,%02x,%02x,%02x,%02x bits=0x%02x\n",
			report[0], report[2], report[3], report[4], report[5],
			report[6], report[7], key_bits);
	}
	return rc;
}

static int send_encoder_event(int hid_fd, uint16_t sid, int delta, bool debug)
{
	uint8_t payload;

	if (delta > 127)
		delta = 127;
	if (delta < -127)
		delta = -127;
	payload = (uint8_t)(int8_t)delta;
	return send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND, CMD_ENCODER_EVENT,
			       sid, &payload, 1, debug);
}

static int send_session_focus(int hid_fd, uint16_t sid, bool debug)
{
	return send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND, CMD_SESSION_FOCUS,
			       sid, NULL, 0, debug);
}

static int send_permission_response(int hid_fd, uint16_t sid, uint64_t req_id,
				    uint8_t decision, bool debug)
{
	uint8_t payload[9];

	memcpy(payload, &req_id, 8);
	payload[8] = decision;
	return send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND, CMD_PERMISSION_RES,
			       sid, payload, sizeof(payload), debug);
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

static bool profile_id_valid(const char *id)
{
	return !strcmp(id, "normal") || !strcmp(id, "vibe") ||
	       !strcmp(id, "custom1") || !strcmp(id, "custom2") ||
	       !strcmp(id, "custom3");
}

static bool profile_is_vibe(void)
{
	return !strcmp(g_profile.active_profile, "vibe");
}

static bool profile_keyboard_enabled(void)
{
	return !profile_is_vibe();
}

static const char *key_binding_id(const struct key_binding *binding)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_key_presets); i++) {
		if (g_key_presets[i].modifier == binding->modifier &&
		    g_key_presets[i].usage == binding->usage)
			return g_key_presets[i].id;
	}
	return NULL;
}

static void key_binding_format(const struct key_binding *binding,
			       char *buf, size_t len)
{
	const char *id = key_binding_id(binding);

	if (id) {
		snprintf(buf, len, "%s", id);
		return;
	}
	snprintf(buf, len, "mod:%02x,usage:%02x",
		 binding->modifier, binding->usage);
}

static const char *vibe_action_id(uint8_t bit)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_vibe_action_presets); i++) {
		if (g_vibe_action_presets[i].bit == bit)
			return g_vibe_action_presets[i].id;
	}
	return NULL;
}

static void vibe_action_format(uint8_t bit, char *buf, size_t len)
{
	const char *id = vibe_action_id(bit);

	if (id) {
		snprintf(buf, len, "%s", id);
		return;
	}
	snprintf(buf, len, "bit:%02x", bit);
}

static bool parse_hex_byte(const char *value, unsigned *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(value, &end, 16);
	if (errno || !end || *end || v > 0xfful)
		return false;
	*out = (unsigned)v;
	return true;
}

static bool parse_key_binding_value(const char *value,
				    struct key_binding *binding)
{
	char tmp[64];
	char *mod_text;
	char *usage_text;
	char *sep;
	unsigned mod;
	unsigned usage;

	for (size_t i = 0; i < ARRAY_SIZE(g_key_presets); i++) {
		if (!strcmp(value, g_key_presets[i].id)) {
			binding->modifier = g_key_presets[i].modifier;
			binding->usage = g_key_presets[i].usage;
			return true;
		}
	}

	snprintf(tmp, sizeof(tmp), "%s", value);
	mod_text = tmp;
	if (!strncmp(mod_text, "mod:", 4))
		mod_text += 4;
	sep = strchr(mod_text, ',');
	if (!sep)
		sep = strchr(mod_text, ':');
	if (!sep)
		return false;
	*sep = '\0';
	usage_text = sep + 1;
	if (!strncmp(usage_text, "usage:", 6))
		usage_text += 6;
	if (!parse_hex_byte(mod_text, &mod) ||
	    !parse_hex_byte(usage_text, &usage))
		return false;
	binding->modifier = (uint8_t)mod;
	binding->usage = (uint8_t)usage;
	return true;
}

static bool parse_vibe_action_value(const char *value, uint8_t *bit)
{
	unsigned parsed;

	for (size_t i = 0; i < ARRAY_SIZE(g_vibe_action_presets); i++) {
		if (!strcmp(value, g_vibe_action_presets[i].id)) {
			*bit = g_vibe_action_presets[i].bit;
			return true;
		}
	}
	if (!strncmp(value, "bit:", 4))
		value += 4;
	if (!parse_hex_byte(value, &parsed))
		return false;
	if (parsed >= KEY_COUNT && parsed != VIBE_ACTION_NONE)
		return false;
	*bit = (uint8_t)parsed;
	return true;
}

static int profile_key_index(const char *key)
{
	char *end = NULL;
	unsigned long idx;

	if (strncmp(key, "key", 3) != 0)
		return -1;
	errno = 0;
	idx = strtoul(key + 3, &end, 10);
	if (errno || !end || *end || idx >= KEY_COUNT)
		return -1;
	return (int)idx;
}

static int profile_action_index(const char *key)
{
	char *end = NULL;
	unsigned long idx;

	if (strncmp(key, "action", 6) != 0)
		return -1;
	errno = 0;
	idx = strtoul(key + 6, &end, 10);
	if (errno || !end || *end || idx >= KEY_COUNT)
		return -1;
	return (int)idx;
}

static void profile_set_defaults(void)
{
	snprintf(g_profile.active_profile, sizeof(g_profile.active_profile),
		 "%s", "vibe");
	g_profile.sound_enabled = false;
	g_profile.sound_volume = AIKB_PROFILE_SOUND_VOLUME_DEFAULT;
	memcpy(g_profile.keymap, g_default_keymap, sizeof(g_profile.keymap));
	memcpy(g_profile.actionmap, g_default_actionmap,
	       sizeof(g_profile.actionmap));
}

static char *trim_ascii(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	end = s + strlen(s);
	while (end > s &&
	       (end[-1] == ' ' || end[-1] == '\t' ||
		end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';
	return s;
}

static bool parse_bool_value(const char *value, bool *out)
{
	if (!strcmp(value, "1") || !strcmp(value, "true") ||
	    !strcmp(value, "on") || !strcmp(value, "yes")) {
		*out = true;
		return true;
	}
	if (!strcmp(value, "0") || !strcmp(value, "false") ||
	    !strcmp(value, "off") || !strcmp(value, "no")) {
		*out = false;
		return true;
	}
	return false;
}

static bool parse_percent_value(const char *value, unsigned *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(value, &end, 10);
	if (errno || !end || *end || v > 100ul)
		return false;
	*out = (unsigned)v;
	return true;
}

static void profile_apply_pair(const char *key, const char *value, bool debug)
{
	int key_index;
	int action_index;
	bool b;
	unsigned percent;

	key_index = profile_key_index(key);
	if (key_index >= 0) {
		struct key_binding binding;

		if (parse_key_binding_value(value, &binding)) {
			g_profile.keymap[key_index] = binding;
		} else if (debug) {
			fprintf(stderr,
				"aikb_hid_input: ignored bad key binding %s=%s\n",
				key, value);
		}
		return;
	}
	action_index = profile_action_index(key);
	if (action_index >= 0) {
		uint8_t bit;

		if (parse_vibe_action_value(value, &bit)) {
			g_profile.actionmap[action_index] = bit;
		} else if (debug) {
			fprintf(stderr,
				"aikb_hid_input: ignored bad vibe action %s=%s\n",
				key, value);
		}
		return;
	}
	if (!strcmp(key, "active_profile") || !strcmp(key, "profile")) {
		if (profile_id_valid(value)) {
			snprintf(g_profile.active_profile,
				 sizeof(g_profile.active_profile), "%s", value);
		} else if (debug) {
			fprintf(stderr, "aikb_hid_input: ignored bad profile id: %s\n",
				value);
		}
		return;
	}
	if (!strcmp(key, "sound_enabled") || !strcmp(key, "sound")) {
		if (parse_bool_value(value, &b))
			g_profile.sound_enabled = b;
		return;
	}
	if (!strcmp(key, "sound_volume") || !strcmp(key, "volume")) {
		if (parse_percent_value(value, &percent))
			g_profile.sound_volume = percent;
		return;
	}
}

static bool profile_load_file(const char *path, bool debug)
{
	FILE *fp;
	char line[160];
	bool loaded = false;

	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		char *p = trim_ascii(line);
		char *eq;
		char *key;
		char *value;

		if (!p[0] || p[0] == '#')
			continue;
		eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		key = trim_ascii(p);
		value = trim_ascii(eq + 1);
		profile_apply_pair(key, value, debug);
		loaded = true;
	}
	fclose(fp);
	return loaded;
}

static void profile_load(bool debug)
{
	profile_set_defaults();
	if (profile_load_file(AIKB_PROFILE_CONFIG, debug))
		return;
	(void)profile_load_file(AIKB_PROFILE_BOOT_CONFIG, debug);
}

static int write_all_fd(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static bool profile_save(bool debug)
{
	char tmp_path[128];
	char buf[1024];
	int fd;
	int dirfd;
	int n;
	size_t off = 0;
	bool ok = false;

	if (mkdir("/mnt/system/etc", 0755) != 0 && errno != EEXIST)
		return false;
	if (mkdir(AIKB_PROFILE_CONFIG_DIR, 0755) != 0 && errno != EEXIST)
		return false;
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", AIKB_PROFILE_CONFIG);
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return false;
	n = snprintf(buf, sizeof(buf),
		     "version=1\nactive_profile=%s\nsound_enabled=%u\nsound_volume=%u\n",
		     g_profile.active_profile, g_profile.sound_enabled ? 1u : 0u,
		     g_profile.sound_volume);
	if (n > 0 && n < (int)sizeof(buf)) {
		off = (size_t)n;
		for (size_t i = 0; i < KEY_COUNT; i++) {
			char binding[32];

			key_binding_format(&g_profile.keymap[i], binding,
					   sizeof(binding));
			n = snprintf(buf + off, sizeof(buf) - off,
				     "key%zu=%s\n", i, binding);
			if (n < 0 || (size_t)n >= sizeof(buf) - off) {
				off = sizeof(buf);
				break;
			}
			off += (size_t)n;
		}
		for (size_t i = 0; i < KEY_COUNT; i++) {
			char action[32];

			vibe_action_format(g_profile.actionmap[i], action,
					   sizeof(action));
			n = snprintf(buf + off, sizeof(buf) - off,
				     "action%zu=%s\n", i, action);
			if (n < 0 || (size_t)n >= sizeof(buf) - off) {
				off = sizeof(buf);
				break;
			}
			off += (size_t)n;
		}
		if (off < sizeof(buf) &&
		    write_all_fd(fd, buf, off) == 0 &&
		    fsync(fd) == 0) {
			ok = true;
		}
	}
	close(fd);
	if (!ok) {
		unlink(tmp_path);
		return false;
	}
	if (rename(tmp_path, AIKB_PROFILE_CONFIG) != 0) {
		unlink(tmp_path);
		return false;
	}
	dirfd = open(AIKB_PROFILE_CONFIG_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd >= 0) {
		(void)fsync(dirfd);
		close(dirfd);
	}
	if (debug)
		fprintf(stderr, "aikb_hid_input: saved profile %s sound=%u volume=%u\n",
			g_profile.active_profile, g_profile.sound_enabled ? 1u : 0u,
			g_profile.sound_volume);
	return true;
}

static void profile_format_state(char *buf, size_t len)
{
	snprintf(buf, len, "CFG v=1 p=%s s=%u vol=%u",
		 g_profile.active_profile,
		 g_profile.sound_enabled ? 1u : 0u, g_profile.sound_volume);
}

static void send_config_text(int hid_fd, const char *text, bool debug)
{
	size_t len;

	if (hid_fd < 0 || !text)
		return;
	len = strlen(text);
	if (len > HID_MAX_PAYLOAD)
		len = HID_MAX_PAYLOAD;
	(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
			      CMD_FEEDBACK_EVENT, SESSION_BROADCAST,
			      (const uint8_t *)text, (uint16_t)len, debug);
}

static void handle_config_command(int hid_fd, const uint8_t *payload,
				  uint16_t plen, bool debug)
{
	char text[HID_MAX_PAYLOAD + 1];
	char response[HID_MAX_PAYLOAD + 1];
	char *cmd;

	if (plen == 0)
		return;
	if (plen >= sizeof(text))
		plen = sizeof(text) - 1;
	memcpy(text, payload, plen);
	text[plen] = '\0';
	cmd = trim_ascii(text);
	if (strncmp(cmd, "CFG", 3) != 0)
		return;
	cmd = trim_ascii(cmd + 3);
	if (!strcmp(cmd, "GET")) {
		profile_format_state(response, sizeof(response));
		send_config_text(hid_fd, response, debug);
		return;
	}
	if (!strcmp(cmd, "RESET")) {
		profile_set_defaults();
		if (!profile_save(debug)) {
			send_config_text(hid_fd, "ERR save_failed", debug);
			return;
		}
		profile_format_state(response, sizeof(response));
		send_config_text(hid_fd, response, debug);
		return;
	}
	if (!strncmp(cmd, "SET", 3)) {
		char *saveptr = NULL;
		char *tok;

		cmd = trim_ascii(cmd + 3);
		for (tok = strtok_r(cmd, " ", &saveptr); tok;
		     tok = strtok_r(NULL, " ", &saveptr)) {
			char *eq = strchr(tok, '=');

			if (!eq)
				continue;
			*eq = '\0';
			profile_apply_pair(trim_ascii(tok), trim_ascii(eq + 1),
					   debug);
		}
		if (!profile_save(debug)) {
			send_config_text(hid_fd, "ERR save_failed", debug);
			return;
		}
		profile_format_state(response, sizeof(response));
		send_config_text(hid_fd, response, debug);
		return;
	}
	send_config_text(hid_fd, "ERR unknown_cfg_command", debug);
}

static const char *notify_tone_name(enum notify_tone_kind kind)
{
	switch (kind) {
	case NOTIFY_TONE_PENDING: return "pending";
	case NOTIFY_TONE_DONE:    return "done";
	case NOTIFY_TONE_ERROR:   return "error";
	default:                  return "pending";
	}
}

static void spawn_notify_tone(enum notify_tone_kind kind, uint64_t now,
			      bool debug)
{
	const char *name;
	pid_t pid;

	if (kind >= NOTIFY_TONE_COUNT)
		return;
	if (!g_profile.sound_enabled)
		return;
	if (now - g_notify_tone_last_ms[kind] < NOTIFY_TONE_COOLDOWN_MS)
		return;
	g_notify_tone_last_ms[kind] = now;
	name = notify_tone_name(kind);

	pid = fork();
	if (pid < 0) {
		if (debug)
			fprintf(stderr, "aikb_hid_input: fork notify %s failed: %s\n",
				name, strerror(errno));
		return;
	}
	if (pid == 0) {
		int nullfd = open("/dev/null", O_RDWR);
		char volume[16];

		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO)
				close(nullfd);
		}
		snprintf(volume, sizeof(volume), "%u", g_profile.sound_volume);
		setenv("AIKB_NOTIFY_VOLUME", volume, 1);
		execl(AIKB_NOTIFY_TONE, AIKB_NOTIFY_TONE, name, (char *)NULL);
		_exit(127);
	}
	if (debug)
		fprintf(stderr, "aikb_hid_input: notify %s pid=%ld\n",
			name, (long)pid);
}

static void reap_voice_mic_children(void)
{
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

static void spawn_voice_mic_cmd(bool start, bool debug)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		if (debug)
			fprintf(stderr, "aikb_hid_input: fork mic %s failed: %s\n",
				start ? "start" : "stop", strerror(errno));
		return;
	}
	if (pid == 0) {
		int nullfd = open("/dev/null", O_RDWR);

		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO)
				close(nullfd);
		}
		execl(AIKB_USB_MIC_BRIDGE, AIKB_USB_MIC_BRIDGE,
		      start ? "mic-start" : "mic-stop", (char *)NULL);
		_exit(127);
	}
	if (debug)
		fprintf(stderr, "aikb_hid_input: voice mic %s pid=%ld\n",
			start ? "start" : "stop", (long)pid);
}

static void control_voice_mic(uint8_t old_bits, uint8_t new_bits, bool debug)
{
	static bool active;
	uint8_t voice_bit = (uint8_t)(1u << VOICE_KEY_INDEX);
	uint8_t reject_bit = (uint8_t)(1u << REJECT_KEY_INDEX);
	uint8_t confirm_bit = (uint8_t)(1u << CONFIRM_KEY_INDEX);
	bool voice_down = (new_bits & voice_bit) && !(old_bits & voice_bit);
	bool reject_down = (new_bits & reject_bit) && !(old_bits & reject_bit);
	bool confirm_down = (new_bits & confirm_bit) && !(old_bits & confirm_bit);

	if (voice_down && !active) {
		spawn_voice_mic_cmd(true, debug);
		active = true;
	} else if ((reject_down || confirm_down) && active) {
		spawn_voice_mic_cmd(false, debug);
		active = false;
	}
}

static void emit_key_down_events(int *event_fd, const struct config *cfg,
				 uint8_t old_bits, uint8_t new_bits,
				 bool old_enc_pressed, bool new_enc_pressed)
{
	static const char *key_lines[KEY_COUNT] = {
		"KEY 0 DOWN\n",
		"KEY 1 DOWN\n",
		"KEY 2 DOWN\n",
		"KEY 3 DOWN\n",
		"KEY 4 DOWN\n",
		"KEY 5 DOWN\n",
		"KEY 6 DOWN\n",
	};

	if (!cfg->event_out_path)
		return;
	for (size_t i = 0; i < KEY_COUNT; i++) {
		uint8_t bit = (uint8_t)(1u << i);

		if ((new_bits & bit) && !(old_bits & bit))
			(void)write_event_line(event_fd, cfg->event_out_path,
					       key_lines[i], cfg->debug);
	}
	if (new_enc_pressed && !old_enc_pressed)
		(void)write_event_line(event_fd, cfg->event_out_path,
				       "ENC_BTN DOWN\n", cfg->debug);
	else if (!new_enc_pressed && old_enc_pressed)
		(void)write_event_line(event_fd, cfg->event_out_path,
				       "ENC_BTN UP\n", cfg->debug);
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

static void log_input_snapshot(uint32_t ext_a, uint32_t ext_e, uint8_t key_bits,
			       bool enc_pressed, unsigned enc_state, int delta)
{
	bool any = false;

	fprintf(stderr,
		"aikb_hid_input: gpio ext_a=0x%08x ext_e=0x%08x keys=0x%02x enc_btn=%u enc_ab=%u delta=%d pressed=",
		ext_a, ext_e, key_bits, enc_pressed ? 1u : 0u, enc_state,
		delta);
	for (size_t i = 0; i < ARRAY_SIZE(g_keys); i++) {
		if (!(key_bits & (uint8_t)(1u << i)))
			continue;
		fprintf(stderr, "%s%s", any ? "," : "", g_keys[i].name);
		any = true;
	}
	if (enc_pressed) {
		fprintf(stderr, "%s%s", any ? "," : "", g_enc_e.name);
		any = true;
	}
	if (!any)
		fprintf(stderr, "none");
	fprintf(stderr, "\n");
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

static void sanitize_inline_text(char *buf, size_t len);

/* Write a "session N state X" / "session N removed" line to the ctrl-out FIFO
 * so aikb_lcd_ui can mirror the board-side session table into its grid. */
static void emit_session_state_line(int *ctrl_fd, const struct config *cfg,
				    uint16_t sid)
{
	char line[48];
	int n;

	if (!cfg->ctrl_out_path || sid == 0)
		return;
	n = snprintf(line, sizeof(line), "session %u state %s\n", sid,
		     session_state_name(g_sessions[sid].state_byte));
	if (n > 0 && n < (int)sizeof(line))
		(void)write_ctrl_line(ctrl_fd, cfg->ctrl_out_path, line,
				      cfg->debug);
}

static void emit_session_hint_line(int *ctrl_fd, const struct config *cfg,
				   uint16_t sid)
{
	char hint[PLUGIN_HINT_MAX];
	char line[PLUGIN_HINT_MAX + 32];
	int n;

	if (!cfg->ctrl_out_path || sid == 0 || !session_alive(sid))
		return;
	snprintf(hint, sizeof(hint), "%s", g_sessions[sid].plugin_hint);
	sanitize_inline_text(hint, strlen(hint));
	n = snprintf(line, sizeof(line), "session %u hint %s\n", sid, hint);
	if (n > 0 && n < (int)sizeof(line))
		(void)write_ctrl_line(ctrl_fd, cfg->ctrl_out_path, line,
				      cfg->debug);
}

static void emit_session_removed_line(int *ctrl_fd, const struct config *cfg,
				      uint16_t sid)
{
	char line[32];
	int n;

	if (!cfg->ctrl_out_path || sid == 0)
		return;
	n = snprintf(line, sizeof(line), "session %u removed\n", sid);
	if (n > 0 && n < (int)sizeof(line))
		(void)write_ctrl_line(ctrl_fd, cfg->ctrl_out_path, line,
				      cfg->debug);
}

/* Sanitize free-text fields embedded in ctrl-out lines: aikb_lcd_ui parses
 * one line at a time, so any CR/LF/TAB in user-supplied text must be folded
 * to plain spaces before emission. Modifies buf in place. */
static void sanitize_inline_text(char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len && buf[i] != '\0'; i++) {
		if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t')
			buf[i] = ' ';
	}
}

static const char *turn_role_name(uint8_t role)
{
	switch (role) {
	case 0:
		return "user";
	case 1:
		return "assistant";
	case 2:
		return "tool";
	case 3:
		return "system";
	default:
		return "unknown";
	}
}

static const char *agent_kind_name(uint8_t kind)
{
	switch (kind) {
	case 0:
		return "claude";
	case 1:
		return "codex";
	case 2:
		return "vscode";
	case 3:
		return "cursor";
	case 4:
		return "browser";
	default:
		return "unknown";
	}
}

/* TOKEN_USAGE payload: 3x uint64_t LE (input / output / cost_cents). */
static void emit_token_line(int *ctrl_fd, const struct config *cfg,
			    uint16_t sid, const uint8_t *payload, uint16_t plen)
{
	uint64_t input, output, cost;
	char line[80];
	int n;

	if (!cfg->ctrl_out_path || sid == 0 || plen < 24)
		return;
	memcpy(&input, payload + 0, 8);
	memcpy(&output, payload + 8, 8);
	memcpy(&cost, payload + 16, 8);
	n = snprintf(line, sizeof(line),
		     "session %u token in=%llu out=%llu cost=%llu\n",
		     sid, (unsigned long long)input,
		     (unsigned long long)output, (unsigned long long)cost);
	if (n > 0 && n < (int)sizeof(line))
		(void)write_ctrl_line(ctrl_fd, cfg->ctrl_out_path, line,
				      cfg->debug);
}

/* TURN_APPEND payload: role(1B) + utf-8 text chunk (≤ HID_MAX_PAYLOAD-1). */
static void emit_turn_line(int *ctrl_fd, const struct config *cfg,
			   uint16_t sid, const uint8_t *payload, uint16_t plen)
{
	uint8_t role;
	char text[HID_MAX_PAYLOAD];
	char line[HID_MAX_PAYLOAD + 48];
	size_t tlen;
	int n;

	if (!cfg->ctrl_out_path || sid == 0 || plen < 1)
		return;
	role = payload[0];
	tlen = (size_t)plen - 1;
	if (tlen >= sizeof(text))
		tlen = sizeof(text) - 1;
	memcpy(text, payload + 1, tlen);
	text[tlen] = '\0';
	sanitize_inline_text(text, tlen);
	n = snprintf(line, sizeof(line), "session %u turn role=%s text:%s\n",
		     sid, turn_role_name(role), text);
	if (n > 0 && n < (int)sizeof(line))
		(void)write_ctrl_line(ctrl_fd, cfg->ctrl_out_path, line,
				      cfg->debug);
}

/* PERMISSION_REQ payload: req_id(8B LE) + tool_len(1B) + tool + args_summary. */
static void emit_permission_line(int *ctrl_fd, const struct config *cfg,
				 uint16_t sid, const uint8_t *payload,
				 uint16_t plen)
{
	uint64_t req_id;
	uint8_t tool_len;
	char tool[32];
	char args[HID_MAX_PAYLOAD];
	char line[HID_MAX_PAYLOAD + 80];
	size_t alen;
	int n;

	if (!cfg->ctrl_out_path || sid == 0 || plen < 9)
		return;
	memcpy(&req_id, payload + 0, 8);
	tool_len = payload[8];
	if (tool_len > sizeof(tool) - 1)
		tool_len = sizeof(tool) - 1;
	if ((size_t)9 + tool_len > (size_t)plen)
		return;
	memcpy(tool, payload + 9, tool_len);
	tool[tool_len] = '\0';
	sanitize_inline_text(tool, tool_len);

	alen = (size_t)plen - 9 - tool_len;
	if (alen >= sizeof(args))
		alen = sizeof(args) - 1;
	memcpy(args, payload + 9 + tool_len, alen);
	args[alen] = '\0';
	sanitize_inline_text(args, alen);

	n = snprintf(line, sizeof(line),
		     "session %u permission reqid=%llu tool=%s args:%s\n",
		     sid, (unsigned long long)req_id, tool, args);
	if (n > 0 && n < (int)sizeof(line))
		(void)write_ctrl_line(ctrl_fd, cfg->ctrl_out_path, line,
				      cfg->debug);
}

/* AGENT_META payload: kind(1B) + cwd_len(1B) + cwd + branch. */
static void emit_meta_line(int *ctrl_fd, const struct config *cfg,
			   uint16_t sid, const uint8_t *payload, uint16_t plen)
{
	uint8_t kind;
	uint8_t cwd_len;
	char cwd[HID_MAX_PAYLOAD];
	char branch[HID_MAX_PAYLOAD];
	char line[HID_MAX_PAYLOAD + 64];
	size_t blen;
	int n;

	if (!cfg->ctrl_out_path || sid == 0 || plen < 2)
		return;
	kind = payload[0];
	cwd_len = payload[1];
	if (cwd_len > sizeof(cwd) - 1)
		cwd_len = sizeof(cwd) - 1;
	if ((size_t)2 + cwd_len > (size_t)plen)
		return;
	memcpy(cwd, payload + 2, cwd_len);
	cwd[cwd_len] = '\0';
	sanitize_inline_text(cwd, cwd_len);

	blen = (size_t)plen - 2 - cwd_len;
	if (blen >= sizeof(branch))
		blen = sizeof(branch) - 1;
	memcpy(branch, payload + 2 + cwd_len, blen);
	branch[blen] = '\0';
	sanitize_inline_text(branch, blen);

	n = snprintf(line, sizeof(line),
		     "session %u meta kind=%s cwd=%s branch=%s\n",
		     sid, agent_kind_name(kind), cwd, branch);
	if (n > 0 && n < (int)sizeof(line))
		(void)write_ctrl_line(ctrl_fd, cfg->ctrl_out_path, line,
				      cfg->debug);
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
			emit_session_removed_line(ctrl_fd, cfg, evicted);
			if (g_active_sid == evicted)
				g_active_sid = 0;
			if (g_selected_sid == evicted)
				g_selected_sid = 0;
		}
		if (new_sid != 0) {
			uint8_t st = SESSION_CREATED;
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_RESPONSE, new_sid,
					      &st, 1, cfg->debug);
			g_selected_sid = new_sid;
			emit_session_state_line(ctrl_fd, cfg, new_sid);
			emit_session_hint_line(ctrl_fd, cfg, new_sid);
		} else {
			uint8_t st = SESSION_POOL_FULL;
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_INVALID,
					      SESSION_BROADCAST, &st, 1,
					      cfg->debug);
		}
		break;
	}

	case CMD_SESSION_HEARTBEAT:
		if (sid == SESSION_BROADCAST)
			break;
		if (!session_alive(sid)) {
			/* Host thinks this sid lives, but board has already freed
			 * it. Tell host so it stops the heartbeat thread. */
			uint8_t st = SESSION_INVALID_S;
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_INVALID, sid, &st, 1,
					      cfg->debug);
			break;
		}
		g_sessions[sid].last_heartbeat_ms = now;
		g_sessions[sid].last_active_ms = now;
		if (g_sessions[sid].disconnected) {
			g_sessions[sid].disconnected = false;
			if (g_sessions[sid].state_byte == SESSION_STATE_DISCONNECTED)
				g_sessions[sid].state_byte = SESSION_STATE_CONNECTED;
			emit_session_state_line(ctrl_fd, cfg, sid);
		}
		break;

	case CMD_VT100_STREAM:
		g_diag_vt100_rx_pkts++;
		g_diag_vt100_rx_bytes += plen;
		if (!session_alive(sid)) {
			uint8_t st = SESSION_INVALID_S;
			g_diag_vt100_drop_dead++;
			if (cfg->debug)
				fprintf(stderr,
					"aikb_hid_input: rx vt100 sid=%u active=%u plen=%u drop=dead rx=%" PRIu64 "/%" PRIu64 " dead=%" PRIu64 "\n",
					sid, g_active_sid, plen,
					g_diag_vt100_rx_pkts,
					g_diag_vt100_rx_bytes,
					g_diag_vt100_drop_dead);
			(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
					      CMD_SESSION_INVALID, sid, &st, 1,
					      cfg->debug);
			break;
		}
		touch_session(sid, now);
		/* Stream gate: only the focused window's bytes reach the screen. */
		if (sid != g_active_sid) {
			g_diag_vt100_drop_sid++;
			if (cfg->debug)
				fprintf(stderr,
					"aikb_hid_input: rx vt100 sid=%u active=%u plen=%u drop=inactive rx=%" PRIu64 "/%" PRIu64 " sid_drop=%" PRIu64 "\n",
					sid, g_active_sid, plen,
					g_diag_vt100_rx_pkts,
					g_diag_vt100_rx_bytes,
					g_diag_vt100_drop_sid);
			break;
		}
		if (cfg->screen_out_path && plen > 0) {
			int rc = write_screen_bytes(screen_fd, cfg->screen_out_path,
						    payload, plen, cfg->debug);
			if (rc == 0) {
				g_diag_vt100_fwd_pkts++;
				g_diag_vt100_fwd_bytes += plen;
			} else if (rc > 0) {
				g_diag_vt100_write_retry++;
			} else {
				g_diag_vt100_write_fail++;
			}
			if (cfg->debug)
				fprintf(stderr,
					"aikb_hid_input: rx vt100 sid=%u active=%u plen=%u fwd_rc=%d rx=%" PRIu64 "/%" PRIu64 " fwd=%" PRIu64 "/%" PRIu64 " retry=%" PRIu64 " fail=%" PRIu64 "\n",
					sid, g_active_sid, plen, rc,
					g_diag_vt100_rx_pkts,
					g_diag_vt100_rx_bytes,
					g_diag_vt100_fwd_pkts,
					g_diag_vt100_fwd_bytes,
					g_diag_vt100_write_retry,
					g_diag_vt100_write_fail);
		}
		break;

	case CMD_STATUS_UPDATE:
		if (!session_alive(sid))
			break;
		touch_session(sid, now);
		if (plen >= 1) {
			uint8_t st = payload[0];
			uint8_t old = g_sessions[sid].state_byte;

			if (st <= SESSION_STATE_ERROR &&
			    old != st) {
				g_sessions[sid].state_byte = st;
				emit_session_state_line(ctrl_fd, cfg, sid);
				if (st == SESSION_STATE_DONE)
					spawn_notify_tone(NOTIFY_TONE_DONE, now,
							  cfg->debug);
				else if (st == SESSION_STATE_ERROR)
					spawn_notify_tone(NOTIFY_TONE_ERROR, now,
							  cfg->debug);
			}
		}
		break;

	case CMD_TOKEN_USAGE:
		if (!session_alive(sid))
			break;
		touch_session(sid, now);
		emit_token_line(ctrl_fd, cfg, sid, payload, plen);
		break;

	case CMD_TURN_APPEND:
		if (!session_alive(sid))
			break;
		touch_session(sid, now);
		emit_turn_line(ctrl_fd, cfg, sid, payload, plen);
		break;

	case CMD_PERMISSION_REQ:
		if (!session_alive(sid))
			break;
		touch_session(sid, now);
		emit_permission_line(ctrl_fd, cfg, sid, payload, plen);
		spawn_notify_tone(NOTIFY_TONE_PENDING, now, cfg->debug);
		break;

	case CMD_AGENT_META:
		if (!session_alive(sid))
			break;
		touch_session(sid, now);
		emit_meta_line(ctrl_fd, cfg, sid, payload, plen);
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

	case CMD_FEEDBACK_EVENT:
		handle_config_command(hid_fd, payload, plen, cfg->debug);
		break;

	case CMD_WINDOW_SWITCH:
	case CMD_WINDOW_ACTIVATE:
		/* Deprecated: board owns its own UI. Host cannot drive view
		 * transitions any more. */
		if (cfg->debug) {
			fprintf(stderr,
				"aikb_hid_input: ignored deprecated cmd 0x%02x sid=%u\n",
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

/* Read aikb_lcd_ui's --ui-ctrl-out FIFO and apply view / select / focus
 * commands. lcd_ui is the source of truth for the picker state machine;
 * aikb_hid_input only mirrors enough to stamp the right sid on outgoing
 * key/encoder events and to forward CMD_SESSION_FOCUS to the host. */
static int open_ui_ctrl_in(const char *path, bool debug)
{
	int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
	struct stat st;
	int fd;

	if (!path)
		return -1;
	if (stat(path, &st) < 0) {
		if (errno == ENOENT) {
			if (mkfifo(path, 0600) != 0 && errno != EEXIST) {
				if (debug)
					fprintf(stderr,
						"aikb_hid_input: mkfifo %s failed: %s\n",
						path, strerror(errno));
				return -1;
			}
		}
	}
	fd = open(path, flags);
	if (fd < 0 && debug) {
		fprintf(stderr, "aikb_hid_input: waiting for ui-ctrl-in %s: %s\n",
			path, strerror(errno));
	}
	return fd;
}

static void apply_ui_ctrl_line(int hid_fd, int *ctrl_fd,
			       const struct config *cfg, const char *line)
{
	unsigned val;
	unsigned long long req_id;
	char decision_name[16];
	uint8_t decision;

	if (!line[0])
		return;
	if (!strcmp(line, "view picker")) {
		g_view = BOARD_VIEW_PICKER;
		return;
	}
	if (!strcmp(line, "view terminal")) {
		g_view = BOARD_VIEW_TERMINAL;
		return;
	}
	if (sscanf(line, "select %u", &val) == 1) {
		if (val == 0 || val > MAX_SESSIONS) {
			g_selected_sid = 0;
			return;
		}
		g_selected_sid = (uint16_t)val;
		return;
	}
	if (sscanf(line, "focus %u", &val) == 1) {
		if (val == 0 || val > MAX_SESSIONS ||
		    !g_sessions[val].used ||
		    g_sessions[val].disconnected) {
			if (cfg->debug)
				fprintf(stderr,
					"aikb_hid_input: focus %u rejected (not alive)\n",
					val);
			return;
		}
		g_active_sid = (uint16_t)val;
		g_view = BOARD_VIEW_TERMINAL;
		g_sessions[val].last_active_ms = now_ms();
		if (hid_fd >= 0)
			(void)send_session_focus(hid_fd, (uint16_t)val,
						 cfg->debug);
		(void)ctrl_fd;
		return;
	}
	if (sscanf(line, "permission %u reqid=%llu decision=%15s",
		   &val, &req_id, decision_name) == 3) {
		if (val == 0 || val > MAX_SESSIONS ||
		    !g_sessions[val].used ||
		    g_sessions[val].disconnected) {
			if (cfg->debug)
				fprintf(stderr,
					"aikb_hid_input: permission %u rejected (not alive)\n",
					val);
			return;
		}
		if (!strcmp(decision_name, "allow"))
			decision = 0;
		else if (!strcmp(decision_name, "deny"))
			decision = 1;
		else if (!strcmp(decision_name, "always"))
			decision = 2;
		else {
			if (cfg->debug)
				fprintf(stderr,
					"aikb_hid_input: unknown permission decision: %s\n",
					decision_name);
			return;
		}
		if (hid_fd >= 0)
			(void)send_permission_response(hid_fd, (uint16_t)val,
						       (uint64_t)req_id,
						       decision, cfg->debug);
		(void)ctrl_fd;
		return;
	}
	if (cfg->debug)
		fprintf(stderr, "aikb_hid_input: ignored ui-ctrl line: %s\n", line);
}

static void drain_ui_ctrl(int *ui_ctrl_fd, int hid_fd, int *ctrl_fd,
			  const struct config *cfg)
{
	static char buf[128];
	static size_t len;
	char tmp[128];
	ssize_t n;

	if (!cfg->ui_ctrl_in_path)
		return;
	if (*ui_ctrl_fd < 0) {
		*ui_ctrl_fd = open_ui_ctrl_in(cfg->ui_ctrl_in_path, cfg->debug);
		if (*ui_ctrl_fd < 0)
			return;
	}
	for (;;) {
		n = read(*ui_ctrl_fd, tmp, sizeof(tmp));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			if (cfg->debug)
				fprintf(stderr,
					"aikb_hid_input: ui-ctrl read failed: %s\n",
					strerror(errno));
			close(*ui_ctrl_fd);
			*ui_ctrl_fd = -1;
			return;
		}
		if (n == 0)
			return;
		for (ssize_t i = 0; i < n; i++) {
			char ch = tmp[i];

			if (ch == '\r')
				continue;
			if (ch == '\n') {
				buf[len] = '\0';
				if (len > 0)
					apply_ui_ctrl_line(hid_fd, ctrl_fd, cfg, buf);
				len = 0;
			} else if (len + 1 < sizeof(buf)) {
				buf[len++] = ch;
			} else {
				len = 0;
			}
		}
	}
}

/* Heartbeat reaper. 30s without CMD_SESSION_HEARTBEAT marks a session
 * DISCONNECTED (notify host + lcd_ui); 60s frees the slot entirely. */
static void reap_sessions(int hid_fd, int *ctrl_fd, const struct config *cfg,
			  uint64_t now)
{
	uint16_t sid;

	for (sid = 1; sid <= MAX_SESSIONS; sid++) {
		struct session_entry *e = &g_sessions[sid];
		uint64_t silent;

		if (!e->used)
			continue;
		silent = now - e->last_heartbeat_ms;
		if (!e->disconnected && silent >= SESSION_HEARTBEAT_TIMEOUT_MS) {
			uint8_t st = SESSION_EXPIRED;

			e->disconnected = true;
			e->state_byte = SESSION_STATE_DISCONNECTED;
			emit_session_state_line(ctrl_fd, cfg, sid);
			if (hid_fd >= 0)
				(void)send_hid_packet(hid_fd, REPORT_ID_HOST_BOUND,
						      CMD_SESSION_INVALID, sid,
						      &st, 1, cfg->debug);
		}
		if (e->disconnected && silent >= SESSION_GC_TIMEOUT_MS) {
			e->used = false;
			e->disconnected = false;
			e->state_byte = SESSION_STATE_CONNECTED;
			e->plugin_hint[0] = '\0';
			emit_session_removed_line(ctrl_fd, cfg, sid);
			if (g_active_sid == sid)
				g_active_sid = 0;
			if (g_selected_sid == sid)
				g_selected_sid = 0;
		}
	}
}

/* -------------------------------------------------------------- arg parser */

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: aikb_hid_input [--hid PATH] [--kbd-hid PATH] [--screen-out PATH]\n"
		"                      [--ctrl-out PATH] [--event-out PATH]\n"
		"                      [--ui-ctrl-in PATH]\n"
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
	cfg->kbd_hid_path = "/dev/hidg1";
	cfg->screen_out_path = NULL;
	cfg->ctrl_out_path = NULL;
	cfg->event_out_path = NULL;
	cfg->ui_ctrl_in_path = NULL;
	cfg->poll_ms = 2;
	cfg->debounce_ms = 12;
	cfg->debug = false;
	cfg->reverse = false;
	cfg->no_hid = false;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--hid") == 0 && i + 1 < argc) {
			cfg->hid_path = argv[++i];
		} else if (strcmp(argv[i], "--kbd-hid") == 0 && i + 1 < argc) {
			cfg->kbd_hid_path = argv[++i];
		} else if (strcmp(argv[i], "--screen-out") == 0 && i + 1 < argc) {
			cfg->screen_out_path = argv[++i];
		} else if (strcmp(argv[i], "--ctrl-out") == 0 && i + 1 < argc) {
			cfg->ctrl_out_path = argv[++i];
		} else if (strcmp(argv[i], "--event-out") == 0 && i + 1 < argc) {
			cfg->event_out_path = argv[++i];
		} else if (strcmp(argv[i], "--ui-ctrl-in") == 0 && i + 1 < argc) {
			cfg->ui_ctrl_in_path = argv[++i];
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
	struct mmio_page rtc_ioblk = { 0 };
	struct mmio_page gpio_a = { 0 };
	struct mmio_page gpio_e = { 0 };
	struct debouncer keys[KEY_COUNT];
	struct debouncer enc_sw;
	uint64_t next_hid_try_ms = 0;
	uint32_t ext_a;
	uint32_t ext_e;
	unsigned enc_state;
	int enc_acc = 0;
	int pending_delta = 0;
	int event_delta_pending = 0;
	bool keys_dirty = false;
	bool kbd_need_sync = true;
	uint8_t key_bits;
	uint8_t hid_prev_key_bits = 0;
	uint8_t kbd_sent_key_bits = 0;
	uint8_t event_prev_key_bits = 0;
	bool hid_prev_enc_pressed = false;
	bool event_prev_enc_pressed = false;
	int mem_fd = -1;
	int hid_fd = -1;
	int kbd_fd = -1;
	int screen_fd = -1;
	int ctrl_fd = -1;
	int event_fd = -1;
	int ui_ctrl_fd = -1;
	uint64_t next_reap_ms = 0;
	uint64_t next_kbd_try_ms = 0;
	size_t i;

	if (parse_args(argc, argv, &cfg) != 0)
		return 2;

	profile_load(cfg.debug);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_reload_signal);
	signal(SIGPIPE, SIG_IGN);

	mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (mem_fd < 0) {
		fprintf(stderr, "aikb_hid_input: open /dev/mem failed: %s\n",
			strerror(errno));
		return 1;
	}

	if (mmio_map(mem_fd, &pinmux, PAGE_ALIGN_DOWN(PINMUX_BASE, 4096),
		     PINMUX_PAGE_SIZE) != 0 ||
	    mmio_map(mem_fd, &rtc_ioblk, PAGE_ALIGN_DOWN(IOBLK_RTC_BASE, 4096),
		     IOBLK_RTC_PAGE_SIZE) != 0 ||
	    mmio_map(mem_fd, &gpio_a, PAGE_ALIGN_DOWN(GPIOA_BASE, 4096),
		     GPIO_PAGE_SIZE) != 0 ||
	    mmio_map(mem_fd, &gpio_e, PAGE_ALIGN_DOWN(GPIOE_BASE, 4096),
		     GPIO_PAGE_SIZE) != 0) {
		mmio_unmap(&pinmux);
		mmio_unmap(&rtc_ioblk);
		mmio_unmap(&gpio_a);
		mmio_unmap(&gpio_e);
		close(mem_fd);
		return 1;
	}

	configure_board_inputs(&pinmux, &rtc_ioblk, &gpio_a, &gpio_e,
			       cfg.debug);

	ext_a = mmio_read32(&gpio_a, GPIOA_BASE + GPIO_EXT_PORTA);
	ext_e = mmio_read32(&gpio_e, GPIOE_BASE + GPIO_EXT_PORTA);
	for (i = 0; i < ARRAY_SIZE(g_keys); i++) {
		debounce_init(&keys[i], pin_pressed(&g_keys[i], ext_a, ext_e),
			      now_ms());
	}
	debounce_init(&enc_sw, pin_pressed(&g_enc_e, ext_a, ext_e), now_ms());
	enc_state = (pin_level(&g_enc_a, ext_a, ext_e) ? 2u : 0u) |
		    (pin_level(&g_enc_b, ext_a, ext_e) ? 1u : 0u);
	hid_prev_key_bits = build_key_bits(keys);
	hid_prev_key_bits = build_vibe_key_bits(hid_prev_key_bits);
	hid_prev_enc_pressed = enc_sw.stable;
	event_prev_key_bits = hid_prev_key_bits;
	event_prev_enc_pressed = enc_sw.stable;

	while (!g_stop) {
		uint64_t now = now_ms();
		unsigned new_enc_state;

		if (g_reload_profile) {
			uint8_t physical_bits;
			uint8_t vibe_bits;

			g_reload_profile = 0;
			profile_load(cfg.debug);
			physical_bits = build_key_bits(keys);
			vibe_bits = build_vibe_key_bits(physical_bits);
			hid_prev_key_bits = vibe_bits;
			event_prev_key_bits = vibe_bits;
			kbd_need_sync = true;
			if (cfg.debug) {
				fprintf(stderr,
					"aikb_hid_input: reloaded profile %s sound=%u volume=%u\n",
					g_profile.active_profile,
					g_profile.sound_enabled ? 1u : 0u,
					g_profile.sound_volume);
			}
		}

		if (!cfg.no_hid && cfg.kbd_hid_path && kbd_fd < 0 &&
		    now >= next_kbd_try_ms) {
			kbd_fd = open_hid(cfg.kbd_hid_path);
			if (kbd_fd < 0) {
				if (cfg.debug) {
					fprintf(stderr,
						"aikb_hid_input: waiting for %s: %s\n",
						cfg.kbd_hid_path, strerror(errno));
				}
				next_kbd_try_ms = now + 1000u;
			} else {
				kbd_sent_key_bits = 0;
				kbd_need_sync = true;
				if (cfg.debug) {
					fprintf(stderr, "aikb_hid_input: opened %s\n",
						cfg.kbd_hid_path);
				}
			}
		}

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
				hid_prev_key_bits =
					build_vibe_key_bits(build_key_bits(keys));
				hid_prev_enc_pressed = enc_sw.stable;
				keys_dirty = false;
				pending_delta = 0;
				if (cfg.debug) {
					fprintf(stderr, "aikb_hid_input: opened %s\n",
						cfg.hid_path);
				}
			}
		}

		ext_a = mmio_read32(&gpio_a, GPIOA_BASE + GPIO_EXT_PORTA);
		ext_e = mmio_read32(&gpio_e, GPIOE_BASE + GPIO_EXT_PORTA);
		for (i = 0; i < ARRAY_SIZE(g_keys); i++) {
			bool raw = pin_pressed(&g_keys[i], ext_a, ext_e);
			if (debounce_update(&keys[i], raw, now, cfg.debounce_ms))
				keys_dirty = true;
		}
		if (debounce_update(&enc_sw, pin_pressed(&g_enc_e, ext_a, ext_e),
				    now, cfg.debounce_ms)) {
			keys_dirty = true;
		}

		new_enc_state = (pin_level(&g_enc_a, ext_a, ext_e) ? 2u : 0u) |
				(pin_level(&g_enc_b, ext_a, ext_e) ? 1u : 0u);
		if (new_enc_state != enc_state) {
			int step = qdec[(enc_state << 2) | new_enc_state];
			if (cfg.reverse)
				step = -step;
			enc_acc += step;
			enc_state = new_enc_state;

			while (enc_acc >= ENCODER_STEPS_PER_EVENT) {
				pending_delta++;
				event_delta_pending++;
				enc_acc -= ENCODER_STEPS_PER_EVENT;
			}
			while (enc_acc <= -ENCODER_STEPS_PER_EVENT) {
				pending_delta--;
				event_delta_pending--;
				enc_acc += ENCODER_STEPS_PER_EVENT;
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
			uint8_t vibe_key_bits;

			key_bits = build_key_bits(keys);
			vibe_key_bits = build_vibe_key_bits(key_bits);
			kbd_need_sync = true;
			if (profile_is_vibe()) {
				control_voice_mic(event_prev_key_bits, vibe_key_bits,
						  cfg.debug);
				emit_key_down_events(&event_fd, &cfg,
						     event_prev_key_bits,
						     vibe_key_bits,
						     event_prev_enc_pressed,
						     enc_sw.stable);
			}
			event_prev_key_bits = vibe_key_bits;
			event_prev_enc_pressed = enc_sw.stable;
		}
		if (event_delta_pending) {
			if (profile_is_vibe())
				emit_encoder_events(&event_fd, &cfg,
						    event_delta_pending);
			event_delta_pending = 0;
		}

		if (hid_fd >= 0)
			drain_packets(hid_fd, &screen_fd, &ctrl_fd, &cfg);

		drain_ui_ctrl(&ui_ctrl_fd, hid_fd, &ctrl_fd, &cfg);
		reap_voice_mic_children();

		if (now >= next_reap_ms) {
			reap_sessions(hid_fd, &ctrl_fd, &cfg, now);
			next_reap_ms = now + 1000u;
		}

		if (cfg.debug && (keys_dirty || pending_delta)) {
			key_bits = build_key_bits(keys);
			log_input_snapshot(ext_a, ext_e, key_bits,
					   enc_sw.stable, new_enc_state,
					   pending_delta);
		}

		if (!cfg.no_hid && kbd_fd >= 0) {
			int rc = 0;

			key_bits = build_key_bits(keys);
			if (!profile_keyboard_enabled()) {
				if (kbd_need_sync || kbd_sent_key_bits != 0)
					rc = send_kbd_report(kbd_fd, 0, cfg.debug);
				if (rc == 0) {
					kbd_sent_key_bits = 0;
					kbd_need_sync = false;
				}
			} else if (kbd_need_sync || key_bits != kbd_sent_key_bits) {
				rc = send_kbd_report(kbd_fd, key_bits, cfg.debug);
				if (rc == 0) {
					kbd_sent_key_bits = key_bits;
					kbd_need_sync = false;
				}
			}
			if (rc < 0) {
				if (cfg.debug) {
					fprintf(stderr,
						"aikb_hid_input: keyboard HID write failed: %s\n",
						strerror(errno));
				}
				close(kbd_fd);
				kbd_fd = -1;
				kbd_need_sync = true;
				next_kbd_try_ms = now + 1000u;
			}
		}

		if (cfg.no_hid) {
			keys_dirty = false;
			pending_delta = 0;
		} else if (hid_fd >= 0) {
			int rc = 0;
			/* Per Board/Host contract: KEY/ENCODER events MUST carry
			 * the currently-displayed sid, and picker-view input is
			 * board-local (no HID emission). */
			bool send_to_host = profile_is_vibe() &&
					     (g_view == BOARD_VIEW_TERMINAL) &&
					     g_active_sid != 0;
			uint16_t event_sid = g_active_sid;

			if (keys_dirty) {
				uint8_t vibe_key_bits;
				uint8_t pressed_bits;
				bool enc_pressed_event;

				key_bits = build_key_bits(keys);
				vibe_key_bits = build_vibe_key_bits(key_bits);
				pressed_bits =
					vibe_key_bits & (uint8_t)~hid_prev_key_bits;
				enc_pressed_event =
					enc_sw.stable && !hid_prev_enc_pressed;
				if (send_to_host &&
				    (pressed_bits || enc_pressed_event)) {
					rc = send_key_event(hid_fd, event_sid,
							    pressed_bits,
							    enc_pressed_event,
							    cfg.debug);
				}
				if (rc == 0) {
					hid_prev_key_bits = vibe_key_bits;
					hid_prev_enc_pressed = enc_sw.stable;
					keys_dirty = false;
				}
			}

			if (rc == 0 && pending_delta) {
				if (send_to_host) {
					rc = send_encoder_event(hid_fd, event_sid,
								pending_delta,
								cfg.debug);
				}
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

	spawn_voice_mic_cmd(false, cfg.debug);
	reap_voice_mic_children();

	if (hid_fd >= 0)
		close(hid_fd);
	if (kbd_fd >= 0) {
		(void)send_kbd_report(kbd_fd, 0, cfg.debug);
		close(kbd_fd);
	}
	if (screen_fd >= 0)
		close(screen_fd);
	if (ctrl_fd >= 0)
		close(ctrl_fd);
	if (event_fd >= 0)
		close(event_fd);
	if (ui_ctrl_fd >= 0)
		close(ui_ctrl_fd);
	mmio_unmap(&pinmux);
	mmio_unmap(&rtc_ioblk);
	mmio_unmap(&gpio_a);
	mmio_unmap(&gpio_e);
	close(mem_fd);

	return 0;
}
