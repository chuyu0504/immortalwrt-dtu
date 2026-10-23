// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "../package/utils/dtu-io/src/ai-protocol.h"

static struct ai_reply parse(const char *data)
{
	struct ai_reply reply = {0};
	/* Serial data may arrive one byte at a time, including split CR/LF. */
	for (size_t i = 0; i < strlen(data); i++)
		ai_feed(&reply, data + i, 1);
	return reply;
}
int main(void)
{
	struct ai_reply reply = parse("AT+AIREAD\r\n+OK\r\nch4:400,12345\r\nch2:200,8000\r\nch1:100,0\r\nch3:300,20000\r\n");
	assert(ai_complete(&reply));
	assert(!strcmp(reply.current[3], "12345")); /* AI0, not original ADC 400. */
	assert(!strcmp(reply.current[2], "20000")); /* AI1 -> ch3. */
	assert(!strcmp(reply.current[1], "8000"));
	assert(!strcmp(reply.current[0], "0"));
	reply = parse("+OK\r\nch1:0,0\r\nch2:0,0\r\nch3:0,0\r\nch4:0,0\r\n");
	assert(ai_complete(&reply)); /* A real zero must remain a valid measurement. */
	reply = parse("ch1:0,0\nch2:0,0\nch3:0,0\nch4:0,0\n");
	assert(!ai_complete(&reply)); /* Missing acknowledgement, never publish. */
	reply = parse("+OK\nch1:1,2\nch2:3,4\nch3:5,6\n");
	assert(!ai_complete(&reply)); /* A partial set cannot masquerade as four zeroes. */
	reply = parse("+OK\nch1:1,2\nch1:3,4\nch2:0,0\nch3:0,0\nch4:0,0\n");
	assert(reply.failed);
	reply = parse("+OK\nch0:1,2\n"); assert(reply.failed);
	reply = parse("+OK\nch1:1,2,3\n"); assert(reply.failed);
	reply = parse("+OK\nch1:1,2x\n"); assert(reply.failed);
	reply = parse("+OK\nch1:1,-\n"); assert(reply.failed);
	reply = parse("+OK\nch1:-10,-1\nch2:0,0\nch3:0,0\nch4:0,0\n");
	assert(ai_complete(&reply) && !strcmp(reply.current[0], "-1"));
	char huge[200]; memset(huge, '9', sizeof(huge));
	reply = (struct ai_reply){0}; ai_feed(&reply, huge, sizeof(huge)); assert(reply.failed);
	puts("PASS factory AI cur_val selection, reverse channel mapping, split frames, real zero, malformed/partial/duplicate/oversized response rejection");
	return 0;
}
