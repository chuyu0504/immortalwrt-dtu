// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTU_IO_DEBOUNCE_H
#define DTU_IO_DEBOUNCE_H
#include <stdbool.h>
#include <stdint.h>

struct input_state {
	bool raw, stable, candidate, valid;
	uint64_t since;
};

static inline void input_sample(struct input_state *state, bool raw, bool inverted,
				unsigned int debounce_ms, uint64_t now)
{
	bool value = raw ^ inverted;
	state->raw = raw;
	if (!state->valid || state->candidate != value) {
		state->candidate = value;
		state->since = now;
	}
	if (!state->valid || now - state->since >= debounce_ms)
		state->stable = value;
	state->valid = true;
}
#endif
