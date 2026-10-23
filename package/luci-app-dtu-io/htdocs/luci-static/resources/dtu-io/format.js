'use strict';
'require baseclass';

return baseclass.extend({
	current: function(value) {
		if ((typeof value !== 'string' && typeof value !== 'number') ||
			!/^\-?\d+$/.test(String(value)))
			return '数据异常';
		var microamps = Number(value);
		if (!Number.isSafeInteger(microamps) || Math.abs(microamps) > Number.MAX_SAFE_INTEGER - 5)
			return '数据异常';
		/* Round integer microamps to 0.01 mA before division. This avoids
		 * binary floating point turning 6795 / 1000 into a 6.79 tie. */
		var cents = Math.floor((Math.abs(microamps) + 5) / 10);
		return (microamps < 0 && cents ? '-' : '') +
			Math.floor(cents / 100) + '.' + String(cents % 100).padStart(2, '0');
	}
});
