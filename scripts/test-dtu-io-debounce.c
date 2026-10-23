// SPDX-License-Identifier: GPL-2.0-only
/* cc -std=c11 -Wall -Wextra -Werror scripts/test-dtu-io-debounce.c -o /tmp/test-io */
#include <assert.h>
#include <stdio.h>
#include "../package/utils/dtu-io/src/debounce.h"

int main(void)
{
	struct input_state input = {0};
	input_sample(&input, false, false, 60, 1000);
	assert(input.valid && !input.stable);
	/* A short spike must be visible as raw, without changing interpreted state. */
	input_sample(&input, true, false, 60, 1020);
	assert(input.raw && !input.stable);
	input_sample(&input, false, false, 60, 1040);
	input_sample(&input, true, false, 60, 1060);
	input_sample(&input, true, false, 60, 1100);
	assert(!input.stable);
	input_sample(&input, true, false, 60, 1120);
	assert(input.stable);
	/* Falling edges require the same stable interval. */
	input_sample(&input, false, false, 60, 1140);
	input_sample(&input, false, false, 60, 1180);
	assert(input.stable);
	input_sample(&input, false, false, 60, 1200);
	assert(!input.stable);
	/* A read failure or interpretation change invalidates the previous sample. */
	input.valid = false;
	input_sample(&input, false, true, 1000, 1220);
	assert(!input.raw && input.stable);
	/* Zero debounce follows the sampled level in both directions. */
	input_sample(&input, true, true, 0, 1240);
	assert(!input.stable);
	input_sample(&input, false, true, 0, 1260);
	assert(input.stable);
	puts("PASS DI bounce rejection, both edges, inversion, recovery and zero delay");
	return 0;
}
