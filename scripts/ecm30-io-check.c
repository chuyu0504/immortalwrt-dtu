// SPDX-License-Identifier: GPL-2.0-only
/* ECM30 diagnostics using Linux GPIO v2 and serial ioctls, never NAND writes.
 * Default: inspect UART configuration and read the four digital inputs.
 * --uart-loopback: internal UART test; does not verify external transceivers.
 * --external-loopback: wired ttyS5 <-> ttyS2 and ttyS1 <-> ttyS3 test.
 * --pulse-outputs: pulse unloaded outputs and leave all GPIO levels low.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/gpio.h>
#include <linux/serial.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const unsigned int di[] = {112, 138, 143, 144};
static const unsigned int dout[] = {36, 37, 38, 117, 115, 114, 113};
static volatile sig_atomic_t interrupted;

static void stop_check(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

static int gpio_request(int chip, const unsigned int *offsets, unsigned int count,
			uint64_t flags)
{
	struct gpio_v2_line_request request = {0};

	memcpy(request.offsets, offsets, count * sizeof(*offsets));
	request.num_lines = count;
	request.config.flags = flags;
	snprintf(request.consumer, sizeof(request.consumer), "ecm30-io-check");
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request) < 0)
		return -1;
	return request.fd;
}

static int gpio_check(bool pulse)
{
	struct gpiochip_info chip = {0};
	struct gpio_v2_line_values values = {.mask = 15};
	unsigned int offsets[] = {36,37,38,111,112,113,114,115,117,138,143,144,194,195,203,205,208};
	const char *names[] = {"Y0.0","Y0.1","Y0.2","KEY","X0.0","Y1.3","Y1.2","Y1.1","Y1.0","X0.1","X0.2","X0.3","RS485_1_DE","WATCHDOG","LED0","RS485_0_DE","RUN"};
	int fd = open("/dev/gpiochip0", O_RDONLY | O_CLOEXEC), lines = -1, ret = 1;

	if (fd < 0 || ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) < 0)
		goto out;
	if (strcmp(chip.label, "2000000.pinctrl") || chip.lines != 224) {
		fprintf(stderr, "Unexpected GPIO controller: %s (%u lines)\n", chip.label, chip.lines);
		goto out;
	}
	for (unsigned int i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		struct gpio_v2_line_info info = {.offset = offsets[i]};
		if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &info) < 0)
			goto out;
		printf("GPIO offset=%u name=%s consumer=%s flags=0x%" PRIx64 "\n",
		       info.offset, info.name, info.consumer, (uint64_t)info.flags);
		if (strcmp(info.name, names[i])) {
			errno = EINVAL;
			goto out;
		}
	}
	lines = gpio_request(fd, di, 4, GPIO_V2_LINE_FLAG_INPUT);
	if (lines < 0 || ioctl(lines, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
		goto out;
	printf("DI raw_bits=0x%" PRIx64 " (X0.0..X0.3; electrical polarity not inferred)\n", (uint64_t)values.bits);
	close(lines);
	lines = -1;
	if (pulse) {
		lines = gpio_request(fd, dout, 7, GPIO_V2_LINE_FLAG_OUTPUT);
		if (lines < 0)
			goto out;
		values.mask = 127;
		for (unsigned int i = 0; i < 7; i++) {
			uint64_t expected = UINT64_C(1) << i;
			values.bits = expected;
			if (ioctl(lines, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0 ||
			    ioctl(lines, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0 ||
			    values.bits != expected)
				goto out;
			usleep(100000);
			values.bits = 0;
			if (ioctl(lines, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0 ||
			    ioctl(lines, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0 || values.bits)
				goto out;
			printf("DO offset=%u GPIO high/low readback=PASS\n", dout[i]);
			if (interrupted)
				goto out;
		}
	}
	ret = 0;
out:
	if (ret)
		perror("GPIO check");
	if (lines >= 0) {
		/* Best-effort low state even when an output test failed. */
		if (pulse) {
			values.mask = 127;
			values.bits = 0;
			(void)ioctl(lines, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
		}
		close(lines);
	}
	if (fd >= 0)
		close(fd);
	return ret;
}

static int uart_loopback(int fd, unsigned int port, struct serial_rs485 *saved)
{
	struct termios old, configured;
	struct serial_rs485 mode = {0};
	int mctrl, ret = 1;
	unsigned char sent[1024], received[1024];
	const speed_t rates[] = {B9600, B115200};

	if (tcgetattr(fd, &old) || ioctl(fd, TIOCMGET, &mctrl))
		return 1;
	configured = old;
	cfmakeraw(&configured);
	configured.c_cflag &= ~(CRTSCTS | HUPCL);
	configured.c_cflag |= CLOCAL | CREAD;
	if (port == 1 || port == 3)
		mode.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND | SER_RS485_RX_DURING_TX;
	if (ioctl(fd, TIOCSRS485, &mode))
		return 1;
	int loop = mctrl | TIOCM_LOOP;
	if (ioctl(fd, TIOCMSET, &loop))
		goto out;
	for (unsigned int rate = 0; rate < 2; rate++) {
		cfsetispeed(&configured, rates[rate]);
		cfsetospeed(&configured, rates[rate]);
		if (tcsetattr(fd, TCSANOW, &configured) || tcflush(fd, TCIOFLUSH))
			goto out;
		for (unsigned int round = 0; round < 3; round++) {
			for (unsigned int i = 0; i < sizeof(sent); i++)
				sent[i] = (i * 37 + round * 61) & 255;
			ssize_t n = write(fd, sent, sizeof(sent));
			if (n != (ssize_t)sizeof(sent))
				goto out;
			size_t used = 0;
			struct timespec start, now;
			clock_gettime(CLOCK_MONOTONIC, &start);
			while (used < sizeof(received)) {
				if (interrupted)
					goto out;
				struct pollfd event = {.fd = fd, .events = POLLIN};
				int ready = poll(&event, 1, 100);
				if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL)))
					goto out;
				if (ready > 0) {
					n = read(fd, received + used, sizeof(received) - used);
					if (n > 0)
						used += n;
					else if (n < 0 && errno != EAGAIN && errno != EINTR)
						goto out;
				}
				clock_gettime(CLOCK_MONOTONIC, &now);
				if (now.tv_sec - start.tv_sec >= 4) {
					fprintf(stderr, "ttyS%u internal loopback timeout (%zu/1024 bytes)\n", port, used);
					goto out;
				}
			}
			if (memcmp(sent, received, sizeof(sent)))
				goto out;
		}
		printf("UART ttyS%u internal_loopback baud=%u bytes=3072 PASS\n", port, rate ? 115200 : 9600);
	}
	ret = 0;
out:
	if (ioctl(fd, TIOCMSET, &mctrl)) ret = 1;
	if (tcsetattr(fd, TCSANOW, &old)) ret = 1;
	if (ioctl(fd, TIOCSRS485, saved)) ret = 1;
	(void)tcflush(fd, TCIFLUSH);
	return ret;
}

static int uart_check(bool loopback)
{
	int failed = 0;
	for (unsigned int i = 1; i <= 5; i++) {
		char path[32];
		struct serial_rs485 mode = {0};
		snprintf(path, sizeof(path), "/dev/ttyS%u", i);
		int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0 || ioctl(fd, TIOCEXCL) || ioctl(fd, TIOCGRS485, &mode)) {
			perror(path);
			if (fd >= 0) close(fd);
			failed = 1;
			continue;
		}
		printf("UART ttyS%u rs485_flags=0x%x before_ms=%u after_ms=%u\n",
		       i, mode.flags, mode.delay_rts_before_send, mode.delay_rts_after_send);
		if ((i == 1 || i == 3) &&
		    (mode.flags & (SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND)) !=
		    (SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND)) {
			fprintf(stderr, "ttyS%u is missing the board's RS485 defaults\n", i);
			failed = 1;
		}
		/* UART4 is wired to the internal analog converter, never a loopback target. */
		if (loopback && i != 4 && uart_loopback(fd, i, &mode))
			failed = 1;
		(void)ioctl(fd, TIOCNXCL);
		close(fd);
	}
	return failed;
}

struct uart_state {
	int fd, mctrl;
	struct termios termios;
	struct serial_rs485 rs485;
	bool saved;
};

static void uart_restore(struct uart_state *state)
{
	if (state->fd < 0)
		return;
	if (state->saved) {
		(void)tcflush(state->fd, TCIOFLUSH);
		(void)ioctl(state->fd, TIOCMSET, &state->mctrl);
		(void)tcsetattr(state->fd, TCSANOW, &state->termios);
		(void)ioctl(state->fd, TIOCSRS485, &state->rs485);
	}
	(void)ioctl(state->fd, TIOCNXCL);
	close(state->fd);
}

static int uart_prepare(struct uart_state *state, unsigned int port, bool rs485)
{
	char path[32];
	struct serial_rs485 mode = {0};
	int mctrl;

	snprintf(path, sizeof(path), "/dev/ttyS%u", port);
	state->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (state->fd < 0 || ioctl(state->fd, TIOCEXCL) ||
	    tcgetattr(state->fd, &state->termios) ||
	    ioctl(state->fd, TIOCMGET, &state->mctrl) ||
	    ioctl(state->fd, TIOCGRS485, &state->rs485))
		return -1;
	state->saved = true;
	/* Disable the UART's internal loop: only the external wire may carry data. */
	mctrl = state->mctrl & ~TIOCM_LOOP;
	if (ioctl(state->fd, TIOCMSET, &mctrl) ||
	    ioctl(state->fd, TIOCMGET, &mctrl) || (mctrl & TIOCM_LOOP))
		return -1;
	if (rs485)
		mode.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
	if (ioctl(state->fd, TIOCSRS485, &mode) ||
	    ioctl(state->fd, TIOCGRS485, &mode))
		return -1;
	if (mode.flags != (rs485 ? SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND : 0)) {
		errno = EINVAL;
		return -1;
	}
	printf("UART ttyS%u external test internal_loop=off rs485_flags=0x%x\n", port, mode.flags);
	return 0;
}

static int uart_set_rate(struct uart_state *state, speed_t rate)
{
	struct termios configured = state->termios;

	cfmakeraw(&configured);
	configured.c_cflag &= ~(CRTSCTS | HUPCL | CSIZE | PARENB | CSTOPB);
	configured.c_cflag |= CLOCAL | CREAD | CS8;
	configured.c_cc[VMIN] = 0;
	configured.c_cc[VTIME] = 0;
	if (cfsetispeed(&configured, rate) || cfsetospeed(&configured, rate) ||
	    tcsetattr(state->fd, TCSANOW, &configured) || tcflush(state->fd, TCIOFLUSH))
		return -1;
	return 0;
}

static int uart_transfer(int source, int destination, unsigned int salt)
{
	unsigned char sent[1024], received[1024];
	size_t written = 0, used = 0;
	struct timespec start, now;

	for (unsigned int i = 0; i < sizeof(sent); i++)
		sent[i] = (i * 37 + salt * 61 + (i >> 8)) & 255;
	clock_gettime(CLOCK_MONOTONIC, &start);
	while (written < sizeof(sent) || used < sizeof(received)) {
		struct pollfd events[] = {
			{.fd = source, .events = written < sizeof(sent) ? POLLOUT : 0},
			{.fd = destination, .events = POLLIN},
		};
		if (interrupted) {
			errno = EINTR;
			return -1;
		}
		int ready = poll(events, 2, 100);
		if (ready < 0 && errno != EINTR)
			return -1;
		for (unsigned int i = 0; i < 2; i++) {
			if (events[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				errno = EIO;
				return -1;
			}
		}
		if (events[0].revents & POLLOUT) {
			ssize_t count = write(source, sent + written, sizeof(sent) - written);
			if (count > 0) written += count;
			else if (count < 0 && errno != EAGAIN && errno != EINTR) return -1;
		}
		if (events[1].revents & POLLIN) {
			ssize_t count = read(destination, received + used, sizeof(received) - used);
			if (count > 0) used += count;
			else if (count < 0 && errno != EAGAIN && errno != EINTR) return -1;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= 5) {
			fprintf(stderr, "External transfer timeout: sent=%zu received=%zu\n", written, used);
			errno = ETIMEDOUT;
			return -1;
		}
	}
	if (memcmp(sent, received, sizeof(sent))) {
		for (unsigned int i = 0; i < sizeof(sent); i++) {
			if (sent[i] != received[i]) {
				fprintf(stderr, "External transfer mismatch at %u: sent=%02x received=%02x\n",
					i, sent[i], received[i]);
				break;
			}
		}
		errno = EIO;
		return -1;
	}
	return tcdrain(source);
}

static int uart_external_pair(unsigned int first, unsigned int second, bool rs485)
{
	struct uart_state states[] = {{.fd = -1}, {.fd = -1}};
	const speed_t rates[] = {B9600, B115200};
	int ret = 1;

	if (uart_prepare(&states[0], first, rs485) || uart_prepare(&states[1], second, rs485))
		goto out;
	for (unsigned int rate = 0; rate < 2; rate++) {
		if (uart_set_rate(&states[0], rates[rate]) || uart_set_rate(&states[1], rates[rate]))
			goto out;
		for (unsigned int round = 0; round < 4; round++) {
			if (uart_transfer(states[0].fd, states[1].fd, round * 2) ||
			    uart_transfer(states[1].fd, states[0].fd, round * 2 + 1))
				goto out;
		}
		printf("%s external ttyS%u <-> ttyS%u baud=%u 8N1 bytes_per_direction=4096 PASS\n",
		       rs485 ? "RS485" : "RS232", first, second, rate ? 115200 : 9600);
	}
	ret = 0;
out:
	if (ret) perror("External UART pair");
	uart_restore(&states[1]);
	uart_restore(&states[0]);
	return ret;
}

int main(int argc, char **argv)
{
	bool loopback = false, pulse = false, external = false;
	char compatible[256] = {0};
	int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, compatible, sizeof(compatible) - 1) <= 0 ||
	    strcmp(compatible, "ebyte,ecm30")) {
		fprintf(stderr, "This diagnostic is for the Ebyte ECM30 only\n");
		if (fd >= 0) close(fd);
		return 1;
	}
	close(fd);
	signal(SIGINT, stop_check);
	signal(SIGTERM, stop_check);
	signal(SIGHUP, stop_check);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--uart-loopback")) loopback = true;
		else if (!strcmp(argv[i], "--external-loopback")) external = true;
		else if (!strcmp(argv[i], "--pulse-outputs")) pulse = true;
		else {
			fprintf(stderr, "Usage: %s [--uart-loopback] [--external-loopback] [--pulse-outputs]\n", argv[0]);
			return 1;
		}
	}
	int failed = gpio_check(pulse) | uart_check(loopback);
	if (external) {
		failed |= uart_external_pair(5, 2, false);
		failed |= uart_external_pair(1, 3, true);
	}
	puts(failed ? "ECM30 IO check FAIL" : "ECM30 IO check PASS");
	return failed;
}
