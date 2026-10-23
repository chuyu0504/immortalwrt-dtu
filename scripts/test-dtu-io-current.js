// Test the actual LuCI formatter with half-up boundaries and invalid samples.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const filename = path.join(__dirname, '../package/luci-app-dtu-io/htdocs/luci-static/resources/dtu-io/format.js');
const format = new Function('baseclass', fs.readFileSync(filename, 'utf8'))({ extend: x => x });
for (const [input, expected] of [
	['0', '0.00'], ['4000', '4.00'], ['20000', '20.00'],
	['6794', '6.79'], ['6795', '6.80'], ['6796', '6.80'],
	['12344', '12.34'], ['12345', '12.35'], ['19995', '20.00'],
	['4', '0.00'], ['5', '0.01'], ['-4', '0.00'], ['-5', '-0.01'],
	['-6795', '-6.80'],
]) assert.equal(format.current(input), expected, input);
for (const input of ['', 'NaN', '4.5', '1e3', null, undefined, '99999999999999999999999999999'])
	assert.equal(format.current(input), '数据异常');
console.log('PASS actual LuCI uA-to-mA formatter: two decimals, half-up ties, zero, signed samples and invalid values');
