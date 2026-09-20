// Run with node RPI/tests/test_controls.js. Compare against the actual FLX4 script.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../../Mixxx/bitedj/res/controllers');
function load(file, tab) {
    const calls = [];
    const context = {
        engine: {
            getValue: () => tab,
            setValue: (...args) => calls.push(['set', ...args]),
        },
        script: {triggerControl: (...args) => calls.push(['trigger', ...args])},
    };
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(path.join(root, file), 'utf8'), context);
    return {context, calls};
}
for (const tab of [0, 1, 2]) {
    for (const [handler, values] of [['browseRotate', [1, 127]], ['browsePress', [127, 0]]]) {
        for (const value of values) {
            const pi = load('mrow-controls.js', tab);
            const flx = load('Pioneer-DDJ-FLX4-script.js', tab);
            pi.context.MROWControls[handler](0, 0, value, 0);
            flx.context.PioneerDDJFLX4[handler](0, 0, value, 0);
            assert.deepEqual(pi.calls, flx.calls, `${handler} tab=${tab} value=${value}`);
            if (handler === 'browsePress' && value === 0) assert.equal(pi.calls.length, 0);
        }
    }
}
console.log('12 FLX4 browse equivalence checks passed');
