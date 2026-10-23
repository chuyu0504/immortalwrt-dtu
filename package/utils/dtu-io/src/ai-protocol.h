// SPDX-License-Identifier: GPL-2.0-only
/* Factory /root/ai.py: AT+AIREAD, +OK, chN:old_val,cur_val (9600 8N1).
 * Preserve numeric strings exactly; no undocumented scaling or clamping.
 */
#ifndef DTU_IO_AI_PROTOCOL_H
#define DTU_IO_AI_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

struct ai_reply {
	char line[96], original[4][32], current[4][32];
	unsigned int used, seen;
	bool ok, failed;
};

static inline bool ai_number(const char *start, size_t length)
{
	if (!length || length >= 32)
		return false;
	size_t i = start[0] == '-' ? 1 : 0;
	if (i == length)
		return false;
	for (; i < length; i++)
		if (start[i] < '0' || start[i] > '9')
			return false;
	return true;
}

static inline void ai_line(struct ai_reply *reply)
{
	char *line = reply->line;
	line[reply->used] = 0;
	if (!*line || !strcmp(line, "AT+AIREAD"))
		return;
	if (!strcmp(line, "+OK")) {
		if (reply->ok)
			reply->failed = true;
		reply->ok = true;
		return;
	}
	if (reply->used < 7 || strncmp(line, "ch", 2) ||
	    line[2] < '1' || line[2] > '4' || line[3] != ':') {
		reply->failed = true;
		return;
	}
	unsigned int channel = line[2] - '1';
	char *comma = strchr(line + 4, ',');
	if (!comma || !ai_number(line + 4, comma - line - 4) ||
	    !ai_number(comma + 1, strlen(comma + 1)) || (reply->seen & (1U << channel))) {
		reply->failed = true;
		return;
	}
	memcpy(reply->original[channel], line + 4, comma - line - 4);
	memcpy(reply->current[channel], comma + 1, strlen(comma + 1));
	reply->seen |= 1U << channel;
}

static inline void ai_feed(struct ai_reply *reply, const char *data, size_t length)
{
	for (size_t i = 0; i < length && !reply->failed; i++) {
		unsigned char ch = data[i];
		if (ch == '\r')
			continue;
		if (ch == '\n') {
			ai_line(reply);
			reply->used = 0;
		} else if (ch < 32 || ch > 126 || reply->used >= sizeof(reply->line) - 1) {
			reply->failed = true;
		} else {
			reply->line[reply->used++] = ch;
		}
	}
}

static inline bool ai_complete(const struct ai_reply *reply)
{
	return reply->ok && !reply->failed && reply->seen == 15 && !reply->used;
}
#endif
