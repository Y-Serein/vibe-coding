#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FRAME_BYTES 4
#define CHUNK_FRAMES 1024

enum mode {
	MODE_PRESERVE_SINGLE,
	MODE_AVERAGE,
	MODE_LEFT_ONLY,
	MODE_RIGHT_ONLY,
	MODE_PRESERVE_TO_MONO,
	MODE_AVERAGE_TO_MONO,
	MODE_LEFT_TO_MONO,
	MODE_RIGHT_TO_MONO,
};

static int16_t clamp_s16(int value)
{
	if (value > 32767)
		return 32767;
	if (value < -32768)
		return -32768;
	return (int16_t)value;
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

int main(int argc, char **argv)
{
	uint8_t inbuf[FRAME_BYTES * CHUNK_FRAMES + FRAME_BYTES - 1];
	uint8_t outbuf[FRAME_BYTES * CHUNK_FRAMES];
	enum mode mode = MODE_PRESERVE_SINGLE;
	size_t carry = 0;

	if (argc > 1) {
		if (strcmp(argv[1], "--average") == 0)
			mode = MODE_AVERAGE;
		else if (strcmp(argv[1], "--preserve-single") == 0)
			mode = MODE_PRESERVE_SINGLE;
		else if (strcmp(argv[1], "--left-only") == 0)
			mode = MODE_LEFT_ONLY;
		else if (strcmp(argv[1], "--right-only") == 0)
			mode = MODE_RIGHT_ONLY;
		else if (strcmp(argv[1], "--preserve-to-mono") == 0)
			mode = MODE_PRESERVE_TO_MONO;
		else if (strcmp(argv[1], "--average-to-mono") == 0)
			mode = MODE_AVERAGE_TO_MONO;
		else if (strcmp(argv[1], "--left-to-mono") == 0)
			mode = MODE_LEFT_TO_MONO;
		else if (strcmp(argv[1], "--right-to-mono") == 0)
			mode = MODE_RIGHT_TO_MONO;
		else {
			fprintf(stderr,
				"Usage: %s [--preserve-single|--average|--left-only|--right-only|--preserve-to-mono|--average-to-mono|--left-to-mono|--right-to-mono]\n",
				argv[0]);
			return 2;
		}
	}

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
		frames = total / FRAME_BYTES;
		for (i = 0; i < frames; i++) {
			uint8_t *frame = inbuf + i * FRAME_BYTES;
			int16_t left;
			int16_t right;
			int mono;

			left = (int16_t)((uint16_t)frame[0] |
					 ((uint16_t)frame[1] << 8));
			right = (int16_t)((uint16_t)frame[2] |
					  ((uint16_t)frame[3] << 8));

			if (mode == MODE_LEFT_ONLY || mode == MODE_LEFT_TO_MONO)
				mono = left;
			else if (mode == MODE_RIGHT_ONLY ||
				 mode == MODE_RIGHT_TO_MONO)
				mono = right;
			else if ((mode == MODE_PRESERVE_SINGLE ||
				  mode == MODE_PRESERVE_TO_MONO) && left == 0)
				mono = right;
			else if ((mode == MODE_PRESERVE_SINGLE ||
				  mode == MODE_PRESERVE_TO_MONO) && right == 0)
				mono = left;
			else
				mono = ((int)left + (int)right) / 2;

			left = clamp_s16(mono);
			outbuf[out_len++] = (uint8_t)((uint16_t)left & 0xff);
			outbuf[out_len++] =
				(uint8_t)(((uint16_t)left >> 8) & 0xff);
			if (mode == MODE_PRESERVE_TO_MONO ||
			    mode == MODE_AVERAGE_TO_MONO ||
			    mode == MODE_LEFT_TO_MONO ||
			    mode == MODE_RIGHT_TO_MONO)
				continue;
			outbuf[out_len] = outbuf[out_len - 2];
			out_len++;
			outbuf[out_len] = outbuf[out_len - 2];
			out_len++;
		}

		if (out_len > 0 && write_all(outbuf, out_len) < 0)
			return 1;

		carry = total - frames * FRAME_BYTES;
		if (carry > 0)
			memmove(inbuf, inbuf + frames * FRAME_BYTES, carry);
	}
}
