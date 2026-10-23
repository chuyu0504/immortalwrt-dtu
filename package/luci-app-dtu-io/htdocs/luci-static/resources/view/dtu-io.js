'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require ui';

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

return view.extend({
	handleSave: null,
	handleSaveApply: null,
	handleReset: null,

	load: function() {
		return L.resolveDefault(callStatus(), null);
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
					'每秒刷新。DO 状态为驱动电平回读，不含外部触点反馈。'
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
				[ '通道', '端子', '状态' ], (status.di || []).map(function(ch) {
					return [ ch.name, String(ch.terminal), digitalValue(ch, true) ];
				})));
			children.push(E('h3', [ 'DO · 数字输出' ]), table(
				[ '通道', '类型 / 端子', '当前输出', '控制' ], (status.do || []).map(function(ch) {
					var disabled = readOnly || !ch.available || !ch.controllable ||
						!!status.config_error || !!pending[ch.id];
					return [ ch.name,
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
			dom.content(statusNode, children);
		}
		renderStatus(data);
		poll.add(refresh, 1);
		return statusNode;
	}
});
