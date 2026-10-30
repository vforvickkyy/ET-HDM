/*
 * Eastape HFS engine: logging, error strings and small helpers.
 */

#include "internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EHFS_VERSION_STRING "0.1.0-dev"

static void (*log_handler)(int, const char*, void*);
static void* log_ctx;

const char* ehfs_version(void) {
	return EHFS_VERSION_STRING;
}

void ehfs_set_log_handler(void (*handler)(int level, const char* message, void* ctx), void* ctx) {
	log_handler = handler;
	log_ctx = ctx;
}

void ehfs_log(int level, const char* fmt, ...) {
	char message[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	if(log_handler) {
		log_handler(level, message, log_ctx);
		return;
	}
	static const char* const names[] = { "debug", "info", "warning", "error" };
	if(level > EHFS_LOG_DEBUG)
		fprintf(stderr, "ehfs %s: %s\n", names[level & 3], message);
}

const char* ehfs_strerror(int err) {
	if(err < 0)
		err = -err;
	switch(err) {
		case 0:            return "Success";
		case EHFS_EDIRTY:  return "The volume's journal has unsaved changes (drive was not ejected properly on a Mac)";
		case EHFS_ENOTHFS: return "Not an HFS+ volume";
		default:           return strerror(err);
	}
}

void ehfs_copy_string(char* dst, size_t dst_size, const char* src) {
	if(!dst_size)
		return;
	size_t n = strlen(src);
	if(n >= dst_size)
		n = dst_size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

size_t ehfs_utf16_to_utf8(const uint8_t* src, size_t units, bool big_endian, char* dst, size_t dst_size) {
	size_t out = 0;
	if(!dst_size)
		return 0;
	for(size_t i = 0; i < units; i++) {
		uint32_t c = big_endian ? ehfs_be16(src + 2 * i) : ehfs_le16(src + 2 * i);
		if(c == 0)
			break;
		if(c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
			uint32_t lo = big_endian ? ehfs_be16(src + 2 * (i + 1)) : ehfs_le16(src + 2 * (i + 1));
			if(lo >= 0xDC00 && lo <= 0xDFFF) {
				c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
				i++;
			}
		}
		if(c >= 0xD800 && c <= 0xDFFF)
			c = 0xFFFD;   /* unpaired surrogate */

		char tmp[4];
		size_t n;
		if(c < 0x80) { tmp[0] = (char)c; n = 1; }
		else if(c < 0x800) { tmp[0] = (char)(0xC0 | c >> 6); tmp[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
		else if(c < 0x10000) { tmp[0] = (char)(0xE0 | c >> 12); tmp[1] = (char)(0x80 | (c >> 6 & 0x3F)); tmp[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
		else { tmp[0] = (char)(0xF0 | c >> 18); tmp[1] = (char)(0x80 | (c >> 12 & 0x3F)); tmp[2] = (char)(0x80 | (c >> 6 & 0x3F)); tmp[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
		if(out + n >= dst_size)
			break;
		memcpy(dst + out, tmp, n);
		out += n;
	}
	dst[out] = '\0';
	return out;
}
