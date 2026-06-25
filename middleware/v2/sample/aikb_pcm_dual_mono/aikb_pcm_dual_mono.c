#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK_FRAMES 1024
#define S16_FRAME_BYTES 4
#define S32_FRAME_BYTES 8
#define MAX_FRAME_BYTES S32_FRAME_BYTES
#define OUT_FRAME_BYTES 4
#define DELAY_HISTORY_FRAMES 17

enum mode {
	MODE_PRESERVE_SINGLE,
	MODE_AVERAGE,
	MODE_LEFT_ONLY,
	MODE_RIGHT_ONLY,
	MODE_PRESERVE_TO_MONO,
	MODE_AVERAGE_TO_MONO,
	MODE_LEFT_TO_MONO,
	MODE_RIGHT_TO_MONO,
	MODE_LEFT_AGC,
	MODE_RIGHT_AGC,
	MODE_AVERAGE_AGC,
	MODE_DELAY_SUM_AGC,
	MODE_CARDIOID_AGC,
	MODE_LEFT_AGC_TO_MONO,
	MODE_RIGHT_AGC_TO_MONO,
	MODE_AVERAGE_AGC_TO_MONO,
	MODE_DELAY_SUM_AGC_TO_MONO,
	MODE_CARDIOID_AGC_TO_MONO,
};

enum sample_format {
	FORMAT_S16_LE,
	FORMAT_S32_LE,
};

struct dsp_config {
	int hpf_enable;
	int hpf_alpha_q15;
	int agc_target;
	int agc_gate;
	int agc_min_q8;
	int agc_max_q8;
	int limiter;
	int noise_floor;
	int noise_gate;
	int noise_atten_q8;
	int delay_samples;
	int cardioid_q8;
	int startup_mute_frames;
	int startup_fade_frames;
};

struct dsp_state {
	int32_t hpf_x_prev;
	int32_t hpf_y_prev;
	int32_t env;
	int gain_q8;
	int16_t left_history[DELAY_HISTORY_FRAMES];
	int16_t right_history[DELAY_HISTORY_FRAMES];
	int history_pos;
	int frame_count;
};

static int16_t clamp_s16(int value)
{
	if (value > 32767)
		return 32767;
	if (value < -32768)
		return -32768;
	return (int16_t)value;
}

static int env_int(const char *name, int fallback, int min_value, int max_value)
{
	const char *value = getenv(name);
	char *end = NULL;
	long parsed;

	if (value == NULL || *value == '\0')
		return fallback;
	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0')
		return fallback;
	if (parsed < min_value)
		return min_value;
	if (parsed > max_value)
		return max_value;
	return (int)parsed;
}

static void dsp_config_init(struct dsp_config *cfg)
{
	cfg->hpf_enable = env_int("AIKB_PCM_HPF", 1, 0, 1);
	cfg->hpf_alpha_q15 =
		env_int("AIKB_PCM_HPF_ALPHA_Q15", 32384, 0, 32767);
	cfg->agc_target = env_int("AIKB_PCM_AGC_TARGET", 7500, 1, 30000);
	cfg->agc_gate = env_int("AIKB_PCM_AGC_GATE", 250, 0, 30000);
	cfg->agc_min_q8 = env_int("AIKB_PCM_AGC_MIN_Q8", 96, 1, 4096);
	cfg->agc_max_q8 = env_int("AIKB_PCM_AGC_MAX_Q8", 768, 1, 4096);
	cfg->limiter = env_int("AIKB_PCM_LIMIT", 20000, 1000, 32767);
	cfg->noise_floor = env_int("AIKB_PCM_NOISE_FLOOR", 300, 0, 30000);
	cfg->noise_gate = env_int("AIKB_PCM_NOISE_GATE", 0, 0, 30000);
	cfg->noise_atten_q8 = env_int("AIKB_PCM_NOISE_ATTEN_Q8", 64, 0, 256);
	cfg->delay_samples = env_int("AIKB_PCM_DELAY_SAMPLES", 1, -8, 8);
	cfg->cardioid_q8 = env_int("AIKB_PCM_CARDIOID_Q8", 24, 0, 256);
	cfg->startup_mute_frames =
		env_int("AIKB_PCM_STARTUP_MUTE_FRAMES", 12000, 0, 96000);
	cfg->startup_fade_frames =
		env_int("AIKB_PCM_STARTUP_FADE_FRAMES", 12000, 0, 96000);
	if (cfg->agc_min_q8 > cfg->agc_max_q8)
		cfg->agc_min_q8 = cfg->agc_max_q8;
	if (cfg->noise_floor > cfg->noise_gate)
		cfg->noise_floor = cfg->noise_gate;
}

static void dsp_state_init(struct dsp_state *state)
{
	memset(state, 0, sizeof(*state));
	state->gain_q8 = 256;
}

static int16_t s32_to_s16(int64_t sample, int shift)
{
	int64_t div = (int64_t)1 << shift;

	if (sample >= 0)
		sample = (sample + div / 2) / div;
	else
		sample = (sample - div / 2) / div;
	if (sample > 32767)
		return 32767;
	if (sample < -32768)
		return -32768;
	return (int16_t)sample;
}

static int mode_to_mono(enum mode mode)
{
	return mode == MODE_PRESERVE_TO_MONO ||
	       mode == MODE_AVERAGE_TO_MONO ||
	       mode == MODE_LEFT_TO_MONO ||
	       mode == MODE_RIGHT_TO_MONO ||
	       mode == MODE_LEFT_AGC_TO_MONO ||
	       mode == MODE_RIGHT_AGC_TO_MONO ||
	       mode == MODE_AVERAGE_AGC_TO_MONO ||
	       mode == MODE_DELAY_SUM_AGC_TO_MONO ||
	       mode == MODE_CARDIOID_AGC_TO_MONO;
}

static int mode_uses_dsp(enum mode mode)
{
	return mode == MODE_LEFT_AGC ||
	       mode == MODE_RIGHT_AGC ||
	       mode == MODE_AVERAGE_AGC ||
	       mode == MODE_DELAY_SUM_AGC ||
	       mode == MODE_CARDIOID_AGC ||
	       mode == MODE_LEFT_AGC_TO_MONO ||
	       mode == MODE_RIGHT_AGC_TO_MONO ||
	       mode == MODE_AVERAGE_AGC_TO_MONO ||
	       mode == MODE_DELAY_SUM_AGC_TO_MONO ||
	       mode == MODE_CARDIOID_AGC_TO_MONO;
}

static int16_t history_get(const int16_t *history, int pos, int delay)
{
	int idx;

	if (delay <= 0)
		return 0;
	idx = pos - delay;
	while (idx < 0)
		idx += DELAY_HISTORY_FRAMES;
	idx %= DELAY_HISTORY_FRAMES;
	return history[idx];
}

static void history_put(struct dsp_state *state, int16_t left, int16_t right)
{
	state->left_history[state->history_pos] = left;
	state->right_history[state->history_pos] = right;
	state->history_pos++;
	if (state->history_pos >= DELAY_HISTORY_FRAMES)
		state->history_pos = 0;
}

static int select_mono(enum mode mode, int16_t left, int16_t right,
		       struct dsp_state *state,
		       const struct dsp_config *cfg)
{
	int delay = cfg->delay_samples;
	int delayed_left = left;
	int delayed_right = right;
	int mono;

	if (delay > 0)
		delayed_left = history_get(state->left_history,
					   state->history_pos, delay);
	else if (delay < 0)
		delayed_right = history_get(state->right_history,
					    state->history_pos, -delay);

	switch (mode) {
	case MODE_LEFT_ONLY:
	case MODE_LEFT_TO_MONO:
	case MODE_LEFT_AGC:
	case MODE_LEFT_AGC_TO_MONO:
		mono = left;
		break;
	case MODE_RIGHT_ONLY:
	case MODE_RIGHT_TO_MONO:
	case MODE_RIGHT_AGC:
	case MODE_RIGHT_AGC_TO_MONO:
		mono = right;
		break;
	case MODE_PRESERVE_SINGLE:
	case MODE_PRESERVE_TO_MONO:
		if (left == 0)
			mono = right;
		else if (right == 0)
			mono = left;
		else
			mono = ((int)left + (int)right) / 2;
		break;
	case MODE_DELAY_SUM_AGC:
	case MODE_DELAY_SUM_AGC_TO_MONO:
		mono = (delayed_left + delayed_right) / 2;
		break;
	case MODE_CARDIOID_AGC:
	case MODE_CARDIOID_AGC_TO_MONO:
		mono = (int)left - ((int)delayed_right * cfg->cardioid_q8) / 256;
		break;
	case MODE_AVERAGE:
	case MODE_AVERAGE_TO_MONO:
	case MODE_AVERAGE_AGC:
	case MODE_AVERAGE_AGC_TO_MONO:
	default:
		mono = ((int)left + (int)right) / 2;
		break;
	}

	history_put(state, left, right);
	return mono;
}

static int16_t dsp_process_sample(struct dsp_state *state,
				  const struct dsp_config *cfg, int sample)
{
	int32_t y = sample;
	int32_t abs_y;
	int32_t diff;
	int desired_q8;
	int64_t amplified;
	int32_t out;

	if (cfg->hpf_enable) {
		int64_t filtered = (int64_t)sample - state->hpf_x_prev +
				   (((int64_t)cfg->hpf_alpha_q15 *
				     state->hpf_y_prev) >> 15);

		if (filtered > 32767)
			filtered = 32767;
		else if (filtered < -32768)
			filtered = -32768;
		state->hpf_x_prev = sample;
		state->hpf_y_prev = (int32_t)filtered;
		y = (int32_t)filtered;
	}

	abs_y = y < 0 ? -y : y;
	if (abs_y > state->env)
		state->env += (abs_y - state->env) >> 4;
	else
		state->env += (abs_y - state->env) >> 8;

	if (state->env <= 0 || state->env < cfg->agc_gate)
		desired_q8 = 256;
	else
		desired_q8 = (cfg->agc_target * 256) / state->env;
	if (desired_q8 < cfg->agc_min_q8)
		desired_q8 = cfg->agc_min_q8;
	if (desired_q8 > cfg->agc_max_q8)
		desired_q8 = cfg->agc_max_q8;

	diff = desired_q8 - state->gain_q8;
	if (diff < 0)
		state->gain_q8 += diff >> 3;
	else
		state->gain_q8 += diff >> 8;
	if (diff < 0 && state->gain_q8 > desired_q8)
		state->gain_q8--;
	else if (diff > 0 && state->gain_q8 < desired_q8)
		state->gain_q8++;

	amplified = ((int64_t)y * state->gain_q8) / 256;
	if (amplified > cfg->limiter)
		amplified = cfg->limiter;
	else if (amplified < -cfg->limiter)
		amplified = -cfg->limiter;
	out = (int32_t)amplified;
	if (cfg->noise_gate > 0 && state->env < cfg->noise_gate) {
		int atten_q8 = cfg->noise_atten_q8;

		if (state->env > cfg->noise_floor &&
		    cfg->noise_gate > cfg->noise_floor) {
			int span = cfg->noise_gate - cfg->noise_floor;
			int above = state->env - cfg->noise_floor;

			atten_q8 += ((256 - atten_q8) * above) / span;
		}
		out = (int32_t)(((int64_t)out * atten_q8) / 256);
	}
	if (state->frame_count < cfg->startup_mute_frames) {
		out = 0;
	} else if (state->frame_count <
		   cfg->startup_mute_frames + cfg->startup_fade_frames &&
		   cfg->startup_fade_frames > 0) {
		int fade_pos = state->frame_count - cfg->startup_mute_frames;

		out = (int32_t)(((int64_t)out * fade_pos) /
				cfg->startup_fade_frames);
	}
	state->frame_count++;
	return (int16_t)out;
}

static int write_all(const uint8_t *buf, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t n = write(STDOUT_FILENO, buf + done, size - done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
}

static int parse_mode(const char *arg, enum sample_format *format,
		      enum mode *mode)
{
	const char *name;

	*format = FORMAT_S16_LE;
	if (strncmp(arg, "--s32-", 6) == 0) {
		*format = FORMAT_S32_LE;
		name = arg + 6;
	} else if (strncmp(arg, "--", 2) == 0) {
		name = arg + 2;
	} else {
		return -1;
	}

	if (strcmp(name, "average") == 0)
		*mode = MODE_AVERAGE;
	else if (strcmp(name, "preserve-single") == 0)
		*mode = MODE_PRESERVE_SINGLE;
	else if (strcmp(name, "left-only") == 0)
		*mode = MODE_LEFT_ONLY;
	else if (strcmp(name, "right-only") == 0)
		*mode = MODE_RIGHT_ONLY;
	else if (strcmp(name, "preserve-to-mono") == 0)
		*mode = MODE_PRESERVE_TO_MONO;
	else if (strcmp(name, "average-to-mono") == 0)
		*mode = MODE_AVERAGE_TO_MONO;
	else if (strcmp(name, "left-to-mono") == 0)
		*mode = MODE_LEFT_TO_MONO;
	else if (strcmp(name, "right-to-mono") == 0)
		*mode = MODE_RIGHT_TO_MONO;
	else if (strcmp(name, "left-agc") == 0)
		*mode = MODE_LEFT_AGC;
	else if (strcmp(name, "right-agc") == 0)
		*mode = MODE_RIGHT_AGC;
	else if (strcmp(name, "average-agc") == 0)
		*mode = MODE_AVERAGE_AGC;
	else if (strcmp(name, "delaysum-agc") == 0 ||
		 strcmp(name, "delay-sum-agc") == 0 ||
		 strcmp(name, "dual-ns") == 0)
		*mode = MODE_DELAY_SUM_AGC;
	else if (strcmp(name, "cardioid-agc") == 0)
		*mode = MODE_CARDIOID_AGC;
	else if (strcmp(name, "left-agc-to-mono") == 0)
		*mode = MODE_LEFT_AGC_TO_MONO;
	else if (strcmp(name, "right-agc-to-mono") == 0)
		*mode = MODE_RIGHT_AGC_TO_MONO;
	else if (strcmp(name, "average-agc-to-mono") == 0)
		*mode = MODE_AVERAGE_AGC_TO_MONO;
	else if (strcmp(name, "delaysum-agc-to-mono") == 0 ||
		 strcmp(name, "delay-sum-agc-to-mono") == 0 ||
		 strcmp(name, "dual-ns-to-mono") == 0)
		*mode = MODE_DELAY_SUM_AGC_TO_MONO;
	else if (strcmp(name, "cardioid-agc-to-mono") == 0)
		*mode = MODE_CARDIOID_AGC_TO_MONO;
	else
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t inbuf[MAX_FRAME_BYTES * CHUNK_FRAMES + MAX_FRAME_BYTES - 1];
	uint8_t outbuf[OUT_FRAME_BYTES * CHUNK_FRAMES];
	enum sample_format format = FORMAT_S16_LE;
	enum mode mode = MODE_PRESERVE_SINGLE;
	struct dsp_config dsp_cfg;
	struct dsp_state dsp_state;
	size_t frame_bytes = S16_FRAME_BYTES;
	int s32_shift = env_int("AIKB_PCM_S32_SHIFT", 13, 0, 24);
	size_t carry = 0;

	dsp_config_init(&dsp_cfg);
	dsp_state_init(&dsp_state);
	if (argc > 1 && parse_mode(argv[1], &format, &mode) < 0) {
		fprintf(stderr,
			"Usage: %s [--preserve-single|--average|--left-only|--right-only|--preserve-to-mono|--average-to-mono|--left-to-mono|--right-to-mono|--left-agc|--right-agc|--average-agc|--delaysum-agc|--dual-ns|--cardioid-agc|--left-agc-to-mono|--right-agc-to-mono|--average-agc-to-mono|--delaysum-agc-to-mono|--dual-ns-to-mono|--cardioid-agc-to-mono|--s32-...]\n",
			argv[0]);
		return 2;
	}
	if (format == FORMAT_S32_LE)
		frame_bytes = S32_FRAME_BYTES;

	for (;;) {
		ssize_t n = read(STDIN_FILENO, inbuf + carry,
				 sizeof(inbuf) - carry);
		size_t total;
		size_t frames;
		size_t out_len = 0;
		size_t i;

		if (n == 0)
			return carry == 0 ? 0 : 1;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}

		total = carry + (size_t)n;
		frames = total / frame_bytes;
		for (i = 0; i < frames; i++) {
			uint8_t *frame = inbuf + i * frame_bytes;
			int16_t left;
			int16_t right;
			int mono;

			if (format == FORMAT_S16_LE) {
				left = (int16_t)((uint16_t)frame[0] |
						 ((uint16_t)frame[1] << 8));
				right = (int16_t)((uint16_t)frame[2] |
						  ((uint16_t)frame[3] << 8));
			} else {
				int32_t left32 = (int32_t)((uint32_t)frame[0] |
						  ((uint32_t)frame[1] << 8) |
						  ((uint32_t)frame[2] << 16) |
						  ((uint32_t)frame[3] << 24));
				int32_t right32 = (int32_t)((uint32_t)frame[4] |
						   ((uint32_t)frame[5] << 8) |
						   ((uint32_t)frame[6] << 16) |
						   ((uint32_t)frame[7] << 24));
				left = s32_to_s16(left32, s32_shift);
				right = s32_to_s16(right32, s32_shift);
			}

			mono = select_mono(mode, left, right, &dsp_state,
					   &dsp_cfg);
			if (mode_uses_dsp(mode))
				left = dsp_process_sample(&dsp_state, &dsp_cfg,
							  mono);
			else
				left = clamp_s16(mono);

			outbuf[out_len++] = (uint8_t)((uint16_t)left & 0xff);
			outbuf[out_len++] =
				(uint8_t)(((uint16_t)left >> 8) & 0xff);
			if (mode_to_mono(mode))
				continue;
			outbuf[out_len] = outbuf[out_len - 2];
			out_len++;
			outbuf[out_len] = outbuf[out_len - 2];
			out_len++;
		}

		if (out_len > 0 && write_all(outbuf, out_len) < 0)
			return 1;

		carry = total - frames * frame_bytes;
		if (carry > 0)
			memmove(inbuf, inbuf + frames * frame_bytes, carry);
	}
}
