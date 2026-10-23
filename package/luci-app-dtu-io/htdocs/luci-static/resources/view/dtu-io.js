'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require ui';
'require uci';
'require form';

var callStatus = rpc.declare({ object: 'dtu.io', method: 'status', reject: true, expect: {} });
var callOutput = rpc.declare({
	object: 'dtu.io', method: 'set_output', params: [ 'channel', 'value' ],
	reject: true, expect: {}
});

function badge(text, good) {
	return E('span', {
		'style': 'display:inline-block;padding:.2rem .65rem;border-radius:1rem;' +
			'font-weight:600;background:' + (good ? '#e5f4eb;color:#17633a' : '#f1f3f5;color:#505a64')
	}, [ text ]);
}

function table(columns, rows) {
	return E('div', { 'class': 'table' }, [
		E('div', { 'class': 'tr table-titles' }, columns.map(function(text) {
			return E('div', { 'class': 'th left' }, [ text ]);
		}))
	].concat(rows.map(function(row) {
		return E('div', { 'class': 'tr' }, row.map(function(cell) {
			return E('div', { 'class': 'td left' }, Array.isArray(cell) ? cell : [ cell ]);
		}));
	})));
}

function digitalValue(channel, input) {
	return channel.available ? badge(input ? (channel.value ? '1' : '0') :
		(channel.value ? '1 · 高' : '0 · 低'), channel.value) :
		E('span', { 'title': channel.error || '' }, [ '不可读' ]);
}

function configurationMap(status) {
	var map = new form.Map('dtu_io', '通道设置',
		'保存并应用后生效；通道设置会随“保留配置”升级保存。');
	var inputs = status.di || [], outputs = status.do || [];

	function section(type, title, channels) {
		var section = map.section(form.TableSection, type, title);
		section.anonymous = true;
		section.addremove = false;
		section.cfgsections = function() {
			return channels.map(function(channel) { return channel.id; });
		};
		var option = section.option(form.DummyValue, '_channel', '通道');
		option.cfgvalue = function(id) {
			var channel = channels.find(function(item) { return item.id === id; });
			return channel.name + ' · 端子 ' + channel.terminal;
		};
		option = section.option(form.Value, 'label', '名称');
		option.rmempty = false;
		option.validate = function(id, value) {
			return value && value.length <= 32 && !/[\x00-\x1f\x7f]/.test(value) ||
				'请输入 1～32 个字符，不含控制字符';
		};
		return section;
	}

	var s = section('input', 'DI · 数字输入', inputs);
	var o = s.option(form.Flag, 'inverted', '反相');
	o.rmempty = false;
	o.default = '0';
	o.description = '勾选后，高电平显示为 0，低电平显示为 1。';
	o = s.option(form.Value, 'debounce_ms', '消抖（毫秒）');
	o.datatype = 'and(uinteger,range(0,1000))';
	o.rmempty = false;
	o.default = '20';
	o.description = '每 20 毫秒采样；用于状态监视，不用于高速脉冲计数。';

	s = section('output', 'DO · 数字输出', outputs);
	o = s.option(form.ListValue, 'startup', '服务启动时输出');
	o.value('0', '0 · 低');
	o.value('1', '1 · 高');
	o.rmempty = false;
	o.default = '0';
	o.description = '下次开机或 IO 服务重新启动时生效；保存设置不会切换当前输出。';
	o = s.option(form.Flag, 'controllable', '允许手动控制');
	o.rmempty = false;
	o.default = '1';
	o.description = '关闭后拒绝手动写入，当前输出保持不变。';
	return map;
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('dtu_io'), L.resolveDefault(callStatus(), null) ]);
	},

	render: function(data) {
		var self = this, statusNode = E('div');
		var pending = {}, readOnly = !L.hasViewPermission();
		function refresh() {
			return callStatus().then(renderStatus).catch(function(error) {
				dom.content(statusNode, E('div', { 'class': 'alert-message warning' }, [
					'IO 服务暂不可用：' + error.message
				]));
			});
		}
		function setOutput(channel, value) {
			if (readOnly || pending[channel.id])
				return;
			pending[channel.id] = true;
			renderStatus(self.status);
			return callOutput(channel.id, value).catch(function(error) {
				ui.addNotification(null, E('p', [ '输出设置失败：' + error.message ]), 'error');
			}).finally(function() {
				delete pending[channel.id];
				return refresh();
			});
		}
		function renderStatus(status) {
			self.status = status;
			if (!status || !status.supported) {
				dom.content(statusNode, E('div', { 'class': 'alert-message warning' }, [
					status ? '此设备尚无已验证的 IO 驱动。' : 'IO 服务未连接。'
				]));
				return;
			}
			var children = [ E('h2', [ '设备 IO · ' + status.board ]),
				E('p', { 'class': 'cbi-section-descr' }, [
					'每秒刷新状态。DI 默认按高电平为 1 显示，可按实际接线设置反相；' +
					'DO 显示驱动电平回读，不代表外部负载或继电器触点反馈。'
				]) ];
			if (status.error || status.config_error)
				children.push(E('div', { 'class': 'alert-message warning' }, [
					status.error || ('配置未生效：' + status.config_error)
				]));
			if (status.volatile_config)
				children.push(E('div', { 'class': 'alert-message notice' }, [
					'当前为 RAM 临时运行，IO 设置重启后不保留。正式安装后可持久保存。'
				]));
			children.push(E('h3', [ 'DI · 数字输入' ]), table(
				[ '通道', '名称', '端子', '状态', '原始电平' ], (status.di || []).map(function(ch) {
					return [ ch.name, ch.label, String(ch.terminal), digitalValue(ch, true),
						ch.available ? (ch.raw ? '高' : '低') : '—' ];
				})));
			children.push(E('h3', [ 'DO · 数字输出' ]), table(
				[ '通道', '名称', '类型 / 端子', '当前输出', '手动控制' ], (status.do || []).map(function(ch) {
					var disabled = readOnly || !ch.available || !ch.controllable ||
						!!status.config_error || !!pending[ch.id];
					return [ ch.name, ch.label,
						(ch.kind === 'relay' ? '继电器' : '晶体管') + ' / ' + ch.terminal,
						digitalValue(ch), [
							E('button', { 'class': 'cbi-button cbi-button-action',
								'disabled': disabled || ch.value === false || null,
								'click': function() { return setOutput(ch, false); }
							}, [ '置 0' ]), ' ',
							E('button', { 'class': 'cbi-button cbi-button-action',
								'disabled': disabled || ch.value === true || null,
								'click': function() { return setOutput(ch, true); }
							}, [ '置 1' ]),
							ch.controllable ? '' : ' 已锁定'
						] ];
				})));
			children.push(E('h3', [ 'AI · 模拟量输入' ]));
			if ((status.ai || []).length)
				children.push(table([ '通道', '端子', '量程', '状态' ], status.ai.map(function(ch) {
					return [ ch.name, String(ch.terminal), '4～20 mA', '采集协议待适配' ];
				})));
			else
				children.push(E('p', [ '此设备未提供模拟量输入。' ]));
			children.push(E('h3', [ 'AO · 模拟量输出' ]), E('p', [ '此设备未提供模拟量输出。' ]));
			dom.content(statusNode, children);
		}
		renderStatus(data[1]);
		poll.add(refresh, 1);
		if (!data[1] || !data[1].supported) {
			this.handleSave = this.handleSaveApply = this.handleReset = null;
			return statusNode;
		}
		return configurationMap(data[1]).render().then(function(configuration) {
			return E([], [ statusNode, configuration ]);
		});
	}
});
