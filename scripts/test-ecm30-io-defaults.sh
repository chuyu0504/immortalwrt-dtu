#!/bin/sh
# Exercise the real UCI CLI in an isolated directory, including saved settings.
set -eu

source_hook="${1:-target/linux/sunxi/base-files/etc/uci-defaults/97-ecm30-edgenode}"
source_hook="$(readlink -f "$source_hook")"
command -v uci >/dev/null
test_root="$(mktemp -d /tmp/ecm30-io-defaults.XXXXXX)"
trap 'rm -rf "$test_root"' EXIT INT TERM

fixture() {
	case_root="$test_root/$1"
	mkdir -p "$case_root/config" "$case_root/delta"
	echo ebyte,ecm30 > "$case_root/board_name"
	cat > "$case_root/config/edgenode" <<'EOF'
config node 'node'
	option heartbeat_interval_sec '317'
config hardware 'hardware'
	option serial_port '/dev/ttyS5'
	option serial_rs485 '0'
	option serial_rs485_user_note 'retain'
config modem 'modem'
	option at_port '/dev/ttyUSB7'
config platform 'custom'
	option url 'http://custom.invalid'
	option enabled '0'
EOF
	cat > "$case_root/config/4ginfo" <<'EOF'
config modem 'modem'
	option enabled '0'
	option at_port '/dev/ttyUSB7'
	option monitor_interval '47'
	option apn 'private-apn'
EOF
	cat > "$case_root/config/network" <<'EOF'
config interface 'wwan_4g'
	option metric '77'
	option ipv6 '1'
	option delegate '1'
config interface 'wwan_4g6'
	option proto 'dhcpv6'
EOF
	cat > "$case_root/config/firewall" <<'EOF'
config zone 'wan'
	option name 'wan'
	list network 'wwan_4g'
	list network 'wan'
EOF
	sed -e "s#/etc/config/#$case_root/config/#g" \
	    -e "s#/etc/init.d/#$case_root/no-init/#g" \
	    -e "s#/sysupgrade.tgz#$case_root/upgrade.tgz#g" \
	    -e "s#/tmp/sysupgrade.tar#$case_root/upgrade.tar#g" \
	    -e "s#/tmp/sysinfo/board_name#$case_root/board_name#g" \
	    "$source_hook" > "$case_root/hook"
}

case_uci() {
	uci -c "$case_root/config" -P "$case_root/delta" "$@"
}

run_hook() {
	CASE_ROOT="$case_root" sh -c '
		uci() { command uci -c "$CASE_ROOT/config" -P "$CASE_ROOT/delta" "$@"; }
		. "$CASE_ROOT/hook"
	'
}

fixture fresh
run_hook
[ "$(case_uci get edgenode.hardware.serial_port)" = /dev/ttyS1 ]
[ "$(case_uci get edgenode.hardware.serial_rs485)" = 1 ]
[ "$(case_uci get edgenode.modem.at_port)" = /dev/ttyUSB0 ]
[ "$(case_uci get 4ginfo.modem.enabled)" = 1 ]
[ "$(case_uci get network.wwan_4g.metric)" = 20 ]
[ "$(case_uci get edgenode.custom.url)" = http://custom.invalid ]
echo 'PASS first boot applies ECM30 hardware defaults and retains platforms'

for archive in tgz tar; do
	fixture "keep-$archive"
	touch "$case_root/upgrade.$archive"
	sha256sum "$case_root"/config/* > "$case_root/before.sha256"
	run_hook
	sha256sum -c "$case_root/before.sha256" >/dev/null
	echo "PASS restored $archive backup preserves custom UART, 4G, network and firewall"
done

fixture migrate
touch "$case_root/upgrade.tgz"
case_uci set edgenode.hardware.serial_port=/dev/ttyS1
case_uci commit edgenode
sha256sum "$case_root/config/4ginfo" "$case_root/config/network" \
	"$case_root/config/firewall" > "$case_root/before.sha256"
run_hook
[ "$(case_uci get edgenode.hardware.serial_rs485)" = 1 ]
[ "$(case_uci get edgenode.hardware.serial_rs485_user_note)" = retain ]
[ "$(case_uci get edgenode.custom.enabled)" = 0 ]
sha256sum -c "$case_root/before.sha256" >/dev/null
echo 'PASS legacy ttyS1 migration changes only the RS485 setting'

sha256sum "$case_root"/config/* > "$case_root/before.sha256"
run_hook
sha256sum -c "$case_root/before.sha256" >/dev/null
echo 'PASS repeat upgrade is idempotent'
