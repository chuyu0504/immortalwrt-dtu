#!/usr/bin/env python3
"""Exercise the real AI timer callbacks with deterministic serial/timer stubs.

Run with python3 scripts/test-dtu-io-timing.py [C compiler]. No device is touched.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'package/utils/dtu-io/src/dtu-io.c').read_text()


def callback(name):
    match = re.search(r'^static void ' + name + r'\([^\n]*\)\n\{.*?^\}',
                      source, re.M | re.S)
    if not match:
        raise RuntimeError('Cannot locate callback: ' + name)
    return match.group()


definitions = '\n'.join(re.findall(r'^#define AI_.*$', source, re.M))
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define TCIFLUSH 0
struct uloop_timeout { int delay; bool armed; };
static struct uloop_timeout ai_timer, ai_deadline;
static struct { int fd; } ai_port = {7};
static unsigned char ai_pending[64];
static bool supported = true, ai_waiting, ai_valid = true;
static char hardware_error[160], ai_error[160];
static int writes, flushes, closed;
static bool write_fails;
static void uloop_timeout_set(struct uloop_timeout *t, int delay)
{ t->delay = delay; t->armed = true; }
static void uloop_timeout_cancel(struct uloop_timeout *t) { t->armed = false; }
static bool ai_open(void) { ai_port.fd = 7; return true; }
static void ai_close(void) { ++closed; ai_port.fd = -1; }
static int tcflush(int fd, int mode)
{ assert(fd == 7 && mode == TCIFLUSH); ++flushes; return 0; }
static ssize_t write(int fd, const void *data, size_t count)
{
    assert(fd == 7 && count == 11 && !memcmp(data, "AT+AIREAD\r\n", count));
    ++writes;
    return write_fails ? 0 : count;
}
'''
harness += definitions + '\n' + '\n'.join(
    callback(name) for name in ('ai_failure', 'ai_timeout', 'ai_query'))
harness += r'''
int main(void)
{
    ai_query(&ai_timer);
    assert(writes == 1 && flushes == 1 && ai_waiting);
    assert(ai_timer.delay == 1000 && ai_deadline.delay == 1000);
    /* Query wins a tie with the deadline: partial reply must survive. */
    memset(ai_pending, 0x5a, sizeof(ai_pending));
    ai_query(&ai_timer);
    assert(writes == 1 && flushes == 1 && ai_waiting && ai_deadline.armed);
    for (size_t i = 0; i < sizeof(ai_pending); ++i) assert(ai_pending[i] == 0x5a);
    ai_timeout(&ai_deadline);
    assert(!ai_waiting && !ai_valid && !ai_deadline.armed);
    assert(!strcmp(ai_error, "AI response timeout"));
    ai_query(&ai_timer);
    assert(writes == 2 && ai_waiting && ai_deadline.armed);
    /* Deadline wins the tie: next request must retain its own deadline. */
    ai_timeout(&ai_deadline);
    ai_query(&ai_timer);
    assert(writes == 3 && ai_waiting && ai_deadline.armed);
    /* Normal completed replies permit one request at every subsequent tick. */
    for (int i = 0; i < 10; ++i) {
        ai_waiting = false;
        uloop_timeout_cancel(&ai_deadline);
        ai_query(&ai_timer);
        assert(writes == 4 + i && ai_timer.delay == 1000 && ai_waiting);
    }
    ai_waiting = false;
    write_fails = true;
    ai_query(&ai_timer);
    assert(!ai_waiting && !ai_valid && !ai_deadline.armed && closed == 1);
    write_fails = false;
    ai_query(&ai_timer);
    assert(ai_port.fd == 7 && ai_waiting && ai_deadline.armed);
    puts("PASS 1s cadence, in-flight protection, both timeout orders and write recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='dtu-io-timing-') as directory:
    directory = pathlib.Path(directory)
    test = directory / 'timing.c'
    binary = directory / 'timing.exe'
    test.write_text(harness)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'cc', '-std=c11',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
