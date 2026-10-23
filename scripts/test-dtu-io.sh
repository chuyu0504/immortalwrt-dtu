#!/bin/sh
# On-device integration test. The service must not already be installed/running.
# --outputs explicitly allows unloaded GPIO control, and always leaves DO low.
set -eu
binary="${1:?Usage: test-dtu-io.sh /tmp/dtu-io config-template [--outputs]}"
template="${2:?Missing configuration template}"
exercise="${3:-}"
work="$(mktemp -d /tmp/dtu-io-test.XXXXXX)"
pid=''
cleanup() {
	[ -z "$pid" ] || { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
	rm -rf "$work"
}
trap cleanup EXIT INT TERM
if ubus list dtu.io 2>/dev/null | grep -q '^dtu.io$'; then
	echo 'Refusing to replace a running I/O service' >&2
	exit 1
fi
cp "$template" "$work/dtu_io"
cfg() { uci -c "$work" -t "$work/delta" "$@"; }
start() {
	"$binary" -c "$work" >"$work/daemon.log" 2>&1 &
	pid=$!
	sleep 1
	kill -0 "$pid"
}
stop() { kill "$pid"; wait "$pid"; pid=''; }
status() { ubus call dtu.io status; }
value() { status | jsonfilter -e "$1"; }
reject() {
	if ubus call dtu.io set_output "$1" >/dev/null 2>&1; then
		echo "Unexpectedly accepted: $1" >&2
		exit 1
	fi
}
start
status
[ "$(value '@.board')" = ECM30 ]
[ "$(value '@.di[3].id')" = di3 ]
[ "$(value '@.do[6].id')" = do6 ]
[ "$(value '@.ai[3].available')" = false ]
[ -z "$(value '@.ai[0].value')" ]
reject '{"channel":"WATCHDOG","value":true}'
reject '{"channel":"di0","value":true}'
reject '{"channel":"do7","value":true}'
reject '{"channel":"do0","value":"1"}'
reject '{"channel":"do0","value":1}'
reject '{"channel":"do0"}'
echo 'PASS fixed channel boundary, strict argument types, unavailable AI has no fabricated value'
before="$(value '@.di[0].value')"
cfg set dtu_io.di0.inverted=0
cfg set dtu_io.di0.label='Input test'
cfg commit dtu_io
ubus call dtu.io reload
sleep 1
[ "$(value '@.di[0].label')" = 'Input test' ]
[ "$(value '@.di[0].value')" != "$before" ]
echo 'PASS input label and inversion reload'
cfg set dtu_io.di0.debounce_ms=1001
cfg commit dtu_io
if ubus call dtu.io reload >/dev/null 2>&1; then exit 1; fi
[ "$(value '@.di[0].debounce_ms')" = 20 ]
[ -n "$(value '@.config_error')" ]
cfg set dtu_io.di0.debounce_ms=20
cfg commit dtu_io
ubus call dtu.io reload
[ -z "$(value '@.config_error')" ]
echo 'PASS invalid config rejected atomically and recoverable'
if [ "$exercise" = --outputs ]; then
	# Test all channels independently; another output must never be clobbered.
	for i in 0 1 2 3 4 5 6; do
		ubus call dtu.io set_output "{\"channel\":\"do$i\",\"value\":true}"
	done
	[ "$(value '@.do[*].value' | grep -c true)" = 7 ]
	cfg set dtu_io.do0.label='Output test'
	cfg commit dtu_io
	ubus call dtu.io reload
	[ "$(value '@.do[*].value' | grep -c true)" = 7 ]
	cfg set dtu_io.do0.controllable=0
	cfg commit dtu_io
	ubus call dtu.io reload
	reject '{"channel":"do0","value":false}'
	[ "$(value '@.do[0].value')" = true ]
	cfg set dtu_io.do0.controllable=1
	cfg commit dtu_io
	ubus call dtu.io reload
	for i in 0 1 2 3 4 5 6; do
		ubus call dtu.io set_output "{\"channel\":\"do$i\",\"value\":false}"
	done
	[ "$(value '@.do[*].value' | grep -c false)" = 7 ]
	echo 'PASS seven outputs, readback, reload preservation and control lock'
	cfg set dtu_io.do0.startup=1
	cfg commit dtu_io
	ubus call dtu.io reload
	[ "$(value '@.do[0].value')" = false ]
	stop
	start
	[ "$(value '@.do[0].value')" = true ]
	cfg set dtu_io.do0.startup=0
	cfg commit dtu_io
	ubus call dtu.io reload
	[ "$(value '@.do[0].value')" = true ]
	stop
	start
	[ "$(value '@.do[*].value' | grep -c false)" = 7 ]
	echo 'PASS startup policy applies on restart only; all outputs restored low'
fi
stop
echo 'PASS device I/O integration'
