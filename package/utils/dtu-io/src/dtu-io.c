// SPDX-License-Identifier: GPL-2.0-only
/* One owner for terminal GPIOs and the internal ADC UART.
 * No flash access, arbitrary-pin API or arbitrary serial commands.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <uci.h>
#include "debounce.h"
#include "ai-protocol.h"

#define INPUTS 4
#define OUTPUTS 7
#define LABEL_SIZE 97
#define SAMPLE_MS 20

struct pin {
	const char *id, *name, *kind;
	unsigned int offset, terminal;
};

static const struct pin inputs[INPUTS] = {
	{"di0", "X0.0", "digital", 112, 26},
	{"di1", "X0.1", "digital", 138, 27},
	{"di2", "X0.2", "digital", 143, 28},
	{"di3", "X0.3", "digital", 144, 29},
};
static const struct pin outputs[OUTPUTS] = {
	{"do0", "Y0.0", "relay", 36, 17},
	{"do1", "Y0.1", "relay", 37, 18},
	{"do2", "Y0.2", "relay", 38, 19},
	{"do3", "Y1.0", "transistor", 117, 21},
	{"do4", "Y1.1", "transistor", 115, 22},
	{"do5", "Y1.2", "transistor", 114, 23},
	{"do6", "Y1.3", "transistor", 113, 24},
};

struct channel_config {
	char label[LABEL_SIZE];
	bool inverted, startup, controllable;
	unsigned int debounce_ms;
};
struct configuration {
	struct channel_config di[INPUTS], dout[OUTPUTS];
};
static struct configuration config;
static struct input_state state[INPUTS];
static struct ubus_context *bus;
static struct blob_buf reply;
static struct uloop_timeout sample_timer;
static struct uloop_timeout ai_timer, ai_deadline;
static struct uloop_fd ai_port = {.fd = -1};
static struct termios ai_saved_termios;
static struct ai_reply ai_pending, ai_values;
static bool ai_valid, ai_waiting;
static uint64_t ai_last_read;
static char ai_error[160];
static int di_fd = -1, do_fd = -1;
static bool supported, di_valid, do_valid;
static uint64_t do_bits;
static const char *config_directory;
static char hardware_error[160], di_error[160], do_error[160], config_error[160];

static uint64_t monotonic_ms(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void ai_close(void)
{
	if (ai_port.fd < 0)
		return;
	uloop_fd_delete(&ai_port);
	(void)tcsetattr(ai_port.fd, TCSANOW, &ai_saved_termios);
	(void)ioctl(ai_port.fd, TIOCNXCL);
	close(ai_port.fd);
	ai_port.fd = -1;
}

static void ai_failure(const char *reason)
{
	ai_waiting = ai_valid = false;
	snprintf(ai_error, sizeof(ai_error), "%s", reason);
	uloop_timeout_cancel(&ai_deadline);
}

static void ai_received(struct uloop_fd *port, unsigned int events)
{
	(void)events;
	char data[256];
	ssize_t count;
	if (port->error || port->eof) {
		ai_failure("AI serial device disconnected");
		ai_close();
		return;
	}
	while ((count = read(port->fd, data, sizeof(data))) > 0) {
		if (!ai_waiting)
			continue;
		ai_feed(&ai_pending, data, count);
		if (ai_pending.failed) {
			ai_failure("Invalid AI response");
			return;
		}
		if (ai_complete(&ai_pending)) {
			ai_values = ai_pending;
			ai_last_read = monotonic_ms();
			ai_valid = true;
			ai_waiting = false;
			ai_error[0] = 0;
			uloop_timeout_cancel(&ai_deadline);
		}
	}
	if (count < 0 && errno != EAGAIN && errno != EINTR) {
		ai_failure("Cannot read AI serial device");
		ai_close();
	}
}

static bool ai_open(void)
{
	int fd = open("/dev/ttyS4", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	struct termios raw;
	if (fd < 0)
		goto fail;
	if (tcgetattr(fd, &ai_saved_termios) || ioctl(fd, TIOCEXCL)) {
		close(fd);
		goto fail;
	}
	ai_port.fd = fd;
	raw = ai_saved_termios;
	cfmakeraw(&raw);
	raw.c_cflag &= ~(CRTSCTS | HUPCL);
	raw.c_cflag |= CLOCAL | CREAD;
	cfsetispeed(&raw, B9600);
	cfsetospeed(&raw, B9600);
	ai_port.cb = ai_received;
	if (tcsetattr(fd, TCSANOW, &raw) || uloop_fd_add(&ai_port, ULOOP_READ)) {
		ai_close();
		goto fail;
	}
	return true;
fail:
	ai_failure("AI serial device unavailable or busy");
	return false;
}

static void ai_timeout(struct uloop_timeout *timer)
{
	(void)timer;
	ai_failure("AI response timeout");
}

static void ai_query(struct uloop_timeout *timer)
{
	const char command[] = "AT+AIREAD\r\n";
	uloop_timeout_set(timer, 2000);
	if (!supported || hardware_error[0])
		return;
	if (ai_port.fd < 0 && !ai_open())
		return;
	memset(&ai_pending, 0, sizeof(ai_pending));
	if (tcflush(ai_port.fd, TCIFLUSH) ||
	    write(ai_port.fd, command, sizeof(command) - 1) != (ssize_t)sizeof(command) - 1) {
		ai_failure("Cannot query AI serial device");
		ai_close();
		return;
	}
	ai_waiting = true;
	uloop_timeout_set(&ai_deadline, 1000);
}

static void defaults(struct configuration *next)
{
	memset(next, 0, sizeof(*next));
	for (unsigned int i = 0; i < INPUTS; i++) {
		snprintf(next->di[i].label, LABEL_SIZE, "%s", inputs[i].name);
		/* Factory auto_test/get_di.sh: GPIO low = input active. */
		next->di[i].inverted = true;
		next->di[i].debounce_ms = 20;
	}
	for (unsigned int i = 0; i < OUTPUTS; i++) {
		snprintf(next->dout[i].label, LABEL_SIZE, "%s", outputs[i].name);
		next->dout[i].controllable = true;
	}
}

static bool number_option(struct uci_context *ctx, struct uci_section *section,
			  const char *name, unsigned int limit, unsigned int *value)
{
	const char *text = uci_lookup_option_string(ctx, section, name);
	struct uci_option *option = uci_lookup_option(ctx, section, name);
	char *end;
	unsigned long number;
	if (option && option->type != UCI_TYPE_STRING)
		return false;
	if (!text)
		return true;
	if (!*text || strspn(text, "0123456789") != strlen(text))
		return false;
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno || *end || number > limit)
		return false;
	*value = number;
	return true;
}

static bool channel_options(struct uci_context *ctx, struct uci_section *section,
			    struct channel_config *item, bool output)
{
	const char *label = uci_lookup_option_string(ctx, section, "label");
	struct uci_option *label_option = uci_lookup_option(ctx, section, "label");
	unsigned int a = output ? item->startup : item->inverted;
	unsigned int b = output ? item->controllable : item->debounce_ms;
	if (label_option && label_option->type != UCI_TYPE_STRING)
		return false;
	if (label) {
		if (!*label || strlen(label) >= LABEL_SIZE)
			return false;
		for (const unsigned char *p = (const unsigned char *)label; *p; p++)
			if (*p < 32 || *p == 127)
				return false;
		snprintf(item->label, LABEL_SIZE, "%s", label);
	}
	if (!number_option(ctx, section, output ? "startup" : "inverted", 1, &a) ||
	    !number_option(ctx, section, output ? "controllable" : "debounce_ms",
			   output ? 1 : 1000, &b))
		return false;
	if (output) {
		item->startup = a;
		item->controllable = b;
	} else {
		item->inverted = a;
		item->debounce_ms = b;
	}
	return true;
}

static bool load_config(struct configuration *next)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *package = NULL;
	struct uci_element *element;
	bool ok = false;
	defaults(next);
	if (!ctx) {
		snprintf(config_error, sizeof(config_error), "Cannot allocate UCI context");
		return false;
	}
	if (config_directory)
		uci_set_confdir(ctx, config_directory);
	if (uci_load(ctx, "dtu_io", &package) != UCI_OK) {
		snprintf(config_error, sizeof(config_error), "Cannot read dtu_io configuration");
		goto out;
	}
	uci_foreach_element(&package->sections, element) {
		struct uci_section *section = uci_to_section(element);
		for (unsigned int i = 0; i < INPUTS + OUTPUTS; i++) {
			bool output = i >= INPUTS;
			unsigned int n = output ? i - INPUTS : i;
			const struct pin *pin = output ? &outputs[n] : &inputs[n];
			if (strcmp(element->name, pin->id))
				continue;
			if (strcmp(section->type, output ? "output" : "input") ||
			    !channel_options(ctx, section,
				output ? &next->dout[n] : &next->di[n], output)) {
				snprintf(config_error, sizeof(config_error), "Invalid configuration: %s", pin->id);
				goto out;
			}
		}
	}
	config_error[0] = 0;
	ok = true;
out:
	uci_free_context(ctx);
	return ok;
}

static bool read_board(void)
{
	char name[80];
	FILE *file = fopen("/tmp/sysinfo/board_name", "r");
	if (!file)
		return false;
	bool ok = fgets(name, sizeof(name), file) != NULL;
	fclose(file);
	if (!ok)
		return false;
	name[strcspn(name, "\r\n")] = 0;
	return !strcmp(name, "ebyte,ecm30");
}

static int request_lines(int chip, const struct pin *pins, unsigned int count,
			 bool output)
{
	struct gpio_v2_line_request request = {0};
	request.num_lines = count;
	request.config.flags = output ? GPIO_V2_LINE_FLAG_OUTPUT : GPIO_V2_LINE_FLAG_INPUT;
	snprintf(request.consumer, sizeof(request.consumer), "dtu-io");
	for (unsigned int i = 0; i < count; i++)
		request.offsets[i] = pins[i].offset;
	if (output) {
		/* Apply all startup levels in the initial request, without a low glitch. */
		request.config.num_attrs = 1;
		request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		request.config.attrs[0].mask = (UINT64_C(1) << count) - 1;
		for (unsigned int i = 0; i < count; i++)
			if (config.dout[i].startup)
				request.config.attrs[0].attr.values |= UINT64_C(1) << i;
	}
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request) < 0)
		return -1;
	return request.fd;
}

static void hardware_init(void)
{
	struct gpiochip_info info = {0};
	int chip;
	supported = read_board();
	if (!supported) {
		snprintf(hardware_error, sizeof(hardware_error), "No verified I/O profile for this board");
		return;
	}
	chip = open("/dev/gpiochip0", O_RDONLY | O_CLOEXEC);
	if (chip < 0 || ioctl(chip, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
		snprintf(hardware_error, sizeof(hardware_error), "GPIO controller: %s", strerror(errno));
		goto out;
	}
	if (strcmp(info.label, "2000000.pinctrl") || info.lines != 224) {
		snprintf(hardware_error, sizeof(hardware_error), "GPIO controller does not match ECM30");
		goto out;
	}
	/* Validate every terminal before claiming any. Never touch UART DE or watchdog. */
	for (unsigned int i = 0; i < INPUTS + OUTPUTS; i++) {
		const struct pin *pin = i < INPUTS ? &inputs[i] : &outputs[i - INPUTS];
		struct gpio_v2_line_info line = {.offset = pin->offset};
		if (ioctl(chip, GPIO_V2_GET_LINEINFO_IOCTL, &line) < 0 || strcmp(line.name, pin->name)) {
			snprintf(hardware_error, sizeof(hardware_error), "GPIO identity mismatch: %s", pin->name);
			goto out;
		}
	}
	di_fd = request_lines(chip, inputs, INPUTS, false);
	if (di_fd < 0)
		snprintf(di_error, sizeof(di_error), "Request DI: %s", strerror(errno));
	do_fd = request_lines(chip, outputs, OUTPUTS, true);
	if (do_fd < 0)
		snprintf(do_error, sizeof(do_error), "Request DO: %s", strerror(errno));
out:
	if (chip >= 0)
		close(chip);
}

static void sample(struct uloop_timeout *timer)
{
	struct gpio_v2_line_values di = {.mask = (1U << INPUTS) - 1};
	struct gpio_v2_line_values dout = {.mask = (1U << OUTPUTS) - 1};
	uint64_t now = monotonic_ms();
	di_valid = di_fd >= 0 && !ioctl(di_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &di);
	if (di_fd >= 0 && !di_valid)
		snprintf(di_error, sizeof(di_error), "Read DI: %s", strerror(errno));
	else if (di_valid)
		di_error[0] = 0;
	for (unsigned int i = 0; i < INPUTS; i++) {
		struct input_state *item = &state[i];
		if (!di_valid) {
			item->valid = false;
			continue;
		}
		input_sample(item, !!(di.bits & (UINT64_C(1) << i)),
			     config.di[i].inverted, config.di[i].debounce_ms, now);
	}
	do_valid = do_fd >= 0 && !ioctl(do_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &dout);
	if (do_fd >= 0 && !do_valid)
		snprintf(do_error, sizeof(do_error), "Read DO: %s", strerror(errno));
	else if (do_valid) {
		do_error[0] = 0;
		do_bits = dout.bits;
	}
	if (timer)
		uloop_timeout_set(timer, SAMPLE_MS);
}

static void pin_metadata(const struct pin *pin, const struct channel_config *item,
			 bool valid, const char *error)
{
	blobmsg_add_string(&reply, "id", pin->id);
	blobmsg_add_string(&reply, "name", pin->name);
	blobmsg_add_string(&reply, "kind", pin->kind);
	blobmsg_add_string(&reply, "label", item->label);
	blobmsg_add_u32(&reply, "terminal", pin->terminal);
	blobmsg_add_u8(&reply, "available", valid);
	if (!valid)
		blobmsg_add_string(&reply, "error", hardware_error[0] ? hardware_error : error);
}

static int status_method(struct ubus_context *ctx, struct ubus_object *object,
			 struct ubus_request_data *request, const char *method,
			 struct blob_attr *message)
{
	(void)object; (void)method; (void)message;
	void *array, *item;
	/* Sample timing is driven only by the timer, never by a caller's poll rate. */
	blob_buf_init(&reply, 0);
	blobmsg_add_string(&reply, "board", supported ? "ECM30" : "unknown");
	blobmsg_add_string(&reply, "version", "1.0.0");
	blobmsg_add_u8(&reply, "supported", supported);
	blobmsg_add_u32(&reply, "sample_ms", SAMPLE_MS);
	blobmsg_add_string(&reply, "error", hardware_error);
	blobmsg_add_string(&reply, "config_error", config_error);
	struct statfs config_fs;
	bool volatile_config = !statfs(config_directory ? config_directory : "/etc/config", &config_fs) &&
		(config_fs.f_type == 0x01021994 || (unsigned long)config_fs.f_type == 0x858458f6);
	blobmsg_add_u8(&reply, "volatile_config", volatile_config); /* tmpfs or ramfs */
	array = blobmsg_open_array(&reply, "di");
	for (unsigned int i = 0; supported && i < INPUTS; i++) {
		item = blobmsg_open_table(&reply, NULL);
		pin_metadata(&inputs[i], &config.di[i], di_valid, di_error);
		blobmsg_add_u8(&reply, "inverted", config.di[i].inverted);
		blobmsg_add_u32(&reply, "debounce_ms", config.di[i].debounce_ms);
		if (di_valid) {
			blobmsg_add_u8(&reply, "raw", state[i].raw);
			blobmsg_add_u8(&reply, "value", state[i].stable);
		}
		blobmsg_close_table(&reply, item);
	}
	blobmsg_close_array(&reply, array);
	array = blobmsg_open_array(&reply, "do");
	for (unsigned int i = 0; supported && i < OUTPUTS; i++) {
		item = blobmsg_open_table(&reply, NULL);
		pin_metadata(&outputs[i], &config.dout[i], do_valid, do_error);
		blobmsg_add_u8(&reply, "startup", config.dout[i].startup);
		blobmsg_add_u8(&reply, "controllable", config.dout[i].controllable);
		if (do_valid)
			blobmsg_add_u8(&reply, "value", !!(do_bits & (UINT64_C(1) << i)));
		blobmsg_close_table(&reply, item);
	}
	blobmsg_close_array(&reply, array);
	array = blobmsg_open_array(&reply, "ai");
	for (unsigned int i = 0; supported && i < 4; i++) {
		char id[8];
		snprintf(id, sizeof(id), "AI%u", i);
		item = blobmsg_open_table(&reply, NULL);
		blobmsg_add_string(&reply, "name", id);
		blobmsg_add_u32(&reply, "terminal", 11 + i);
		bool fresh = ai_valid && monotonic_ms() - ai_last_read < 5000;
		blobmsg_add_string(&reply, "range", "4-20 mA");
		blobmsg_add_string(&reply, "unit", "uA");
		blobmsg_add_u8(&reply, "available", fresh);
		if (fresh) {
			/* Factory mapping AI0->ch4, AI1->ch3, AI2->ch2, AI3->ch1.
			 * cur_val is integer microamps; the LuCI view formats it as mA. */
			blobmsg_add_string(&reply, "value", ai_values.current[3 - i]);
			blobmsg_add_string(&reply, "original", ai_values.original[3 - i]);
			blobmsg_add_u32(&reply, "age_ms", monotonic_ms() - ai_last_read);
		} else {
			blobmsg_add_string(&reply, "error", ai_error[0] ? ai_error : "Waiting for AI response");
		}
		blobmsg_close_table(&reply, item);
	}
	blobmsg_close_array(&reply, array);
	array = blobmsg_open_array(&reply, "ao");
	blobmsg_close_array(&reply, array);
	ubus_send_reply(ctx, request, reply.head);
	return UBUS_STATUS_OK;
}

enum { SET_CHANNEL, SET_VALUE, SET_COUNT };
static const struct blobmsg_policy set_policy[SET_COUNT] = {
	[SET_CHANNEL] = {.name = "channel", .type = BLOBMSG_TYPE_STRING},
	[SET_VALUE] = {.name = "value", .type = BLOBMSG_TYPE_BOOL},
};

static int set_method(struct ubus_context *ctx, struct ubus_object *object,
		      struct ubus_request_data *request, const char *method,
		      struct blob_attr *message)
{
	(void)object; (void)method;
	struct blob_attr *args[SET_COUNT];
	struct gpio_v2_line_values values = {0};
	unsigned int index;
	if (!message)
		return UBUS_STATUS_INVALID_ARGUMENT;
	blobmsg_parse(set_policy, SET_COUNT, args, blob_data(message), blob_len(message));
	if (!args[SET_CHANNEL] || !args[SET_VALUE] || blobmsg_get_u8(args[SET_VALUE]) > 1)
		return UBUS_STATUS_INVALID_ARGUMENT;
	for (index = 0; index < OUTPUTS; index++)
		if (!strcmp(blobmsg_get_string(args[SET_CHANNEL]), outputs[index].id))
			break;
	if (index == OUTPUTS)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!supported || do_fd < 0 || config_error[0])
		return UBUS_STATUS_NOT_SUPPORTED;
	if (!config.dout[index].controllable)
		return UBUS_STATUS_PERMISSION_DENIED;
	values.mask = UINT64_C(1) << index;
	values.bits = blobmsg_get_u8(args[SET_VALUE]) ? values.mask : 0;
	if (ioctl(do_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
		do_valid = false;
		snprintf(do_error, sizeof(do_error), "Write DO: %s", strerror(errno));
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	bool expected = !!values.bits;
	if (ioctl(do_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0 ||
	    !!(values.bits & values.mask) != expected) {
		do_valid = false;
		snprintf(do_error, sizeof(do_error), "DO readback failed");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	/* Updating one bit never replays a cached value over the other outputs. */
	do_bits = (do_bits & ~values.mask) | (values.bits & values.mask);
	do_valid = true;
	do_error[0] = 0;
	blob_buf_init(&reply, 0);
	blobmsg_add_u8(&reply, "value", expected);
	ubus_send_reply(ctx, request, reply.head);
	return UBUS_STATUS_OK;
}

static int reload_method(struct ubus_context *ctx, struct ubus_object *object,
			 struct ubus_request_data *request, const char *method,
			 struct blob_attr *message)
{
	(void)ctx; (void)object; (void)request; (void)method; (void)message;
	struct configuration next;
	if (!load_config(&next)) {
		fprintf(stderr, "%s; keeping previous settings\n", config_error);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	for (unsigned int i = 0; i < INPUTS; i++)
		if (config.di[i].inverted != next.di[i].inverted ||
		    config.di[i].debounce_ms != next.di[i].debounce_ms)
			state[i].valid = false;
	config = next;
	return UBUS_STATUS_OK;
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", status_method),
	UBUS_METHOD("set_output", set_method, set_policy),
	UBUS_METHOD_NOARG("reload", reload_method),
};
static struct ubus_object_type object_type = UBUS_OBJECT_TYPE("dtu.io", methods);
static struct ubus_object object = {
	.name = "dtu.io", .type = &object_type,
	.methods = methods, .n_methods = ARRAY_SIZE(methods),
};

static void disconnect(struct ubus_context *ctx)
{
	(void)ctx;
	uloop_end(); /* procd reconnects by respawning, re-registering the object. */
}

int main(int argc, char **argv)
{
	int result = 1;
	if (argc == 3 && !strcmp(argv[1], "-c"))
		config_directory = argv[2];
	else if (argc != 1) {
		fprintf(stderr, "Usage: dtu-io [-c config-directory]\n");
		return 2;
	}
	if (!load_config(&config)) {
		fprintf(stderr, "%s\n", config_error);
		return 1; /* Never apply output defaults from an invalid configuration. */
	}
	if (uloop_init())
		return 1;
	bus = ubus_connect(NULL);
	if (!bus)
		goto out;
	ubus_add_uloop(bus);
	bus->connection_lost = disconnect;
	/* Claim the singleton ubus object before acquiring or changing any GPIOs. */
	if (ubus_add_object(bus, &object))
		goto out;
	hardware_init();
	sample_timer.cb = sample;
	sample(&sample_timer);
	ai_timer.cb = ai_query;
	ai_deadline.cb = ai_timeout;
	uloop_timeout_set(&ai_timer, 1);
	uloop_run();
	result = 0;
out:
	uloop_timeout_cancel(&sample_timer);
	uloop_timeout_cancel(&ai_timer);
	uloop_timeout_cancel(&ai_deadline);
	ai_close();
	if (do_fd >= 0) {
		struct gpio_v2_line_values off = {.mask = (1U << OUTPUTS) - 1};
		if (ioctl(do_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &off) < 0)
			fprintf(stderr, "Cannot clear outputs on exit: %s\n", strerror(errno));
		close(do_fd);
	}
	if (di_fd >= 0)
		close(di_fd);
	if (bus)
		ubus_free(bus);
	blob_buf_free(&reply);
	uloop_done();
	return result;
}
