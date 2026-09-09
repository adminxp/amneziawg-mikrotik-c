/* WireGuard MTU: the S4/transport ceiling, the underlay path MTU and an MTU
 * carried by the .conf, and how the three combine.
 *
 * Dev-only, same as conf3.0-ipv6.test.js:
 *   npm install jsdom && node tests/conf3.0-mtu.test.js
 * Exits non-zero on the first broken expectation.
 */
const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

const html = fs.readFileSync(path.join(__dirname, '..', 'docs', 'conf3.0.html'), 'utf8');

let fails = 0, passes = 0;
function ok(name, cond, extra) {
    if (cond) { passes++; console.log('  PASS  ' + name); }
    else { fails++; console.log('  FAIL  ' + name + (extra ? ': ' + extra : '')); }
}
function eq(name, a, b) { ok(name, a === b, JSON.stringify(a) + ' !== ' + JSON.stringify(b)); }

const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' });
const w = dom.window;

const need = ['wgMtuForS4', 'wgMtuCeiling', 'effectiveWgMtu', 'wgMtuOpt',
              'explicitMtu', 'pathMtuValue', 'parseConf', 'generate'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 MTU tests ===');

const pathMtu = w.document.getElementById('path-mtu');
const ipv6Box = w.document.getElementById('ipv6-enable');
ok('path MTU field exists', !!pathMtu);
eq('path MTU defaults to 1500', pathMtu.value, '1500');

/* ---- the 1500-byte path keeps behaving exactly as before ---- */
eq('1500 ipv4 S4=0  -> default', w.wgMtuForS4(0, false, 1500), 0);
eq('1500 ipv4 S4=16 -> default', w.wgMtuForS4(16, false, 1500), 0);
eq('1500 ipv4 S4=17 -> 1408', w.wgMtuForS4(17, false, 1500), 1408);
eq('1500 ipv4 S4=40 -> 1392', w.wgMtuForS4(40, false, 1500), 1392);
eq('1500 ipv6 S4=0  -> 1408', w.wgMtuForS4(0, true, 1500), 1408);
eq('1500 ipv6 S4=16 -> 1392', w.wgMtuForS4(16, true, 1500), 1392);
eq('omitting pathMtu means 1500', w.wgMtuForS4(40, false), w.wgMtuForS4(40, false, 1500));

/* ---- PPPoE: 1492 is where the old fixed-1500 assumption broke (#21) ---- */
eq('1492 ipv4 S4=0  -> default still fits', w.wgMtuForS4(0, false, 1492), 0);
eq('1492 ipv4 S4=8  -> default still fits', w.wgMtuForS4(8, false, 1492), 0);
eq('1492 ipv4 S4=12 -> 1408', w.wgMtuForS4(12, false, 1492), 1408);
eq('1492 ipv4 S4=16 -> 1408', w.wgMtuForS4(16, false, 1492), 1408);

/* The invariant the formula exists for: the outer datagram must fit the path.
 * outer = IP + 8 (UDP) + S4 + 16 (WG header) + MTU + 16 (Poly1305 tag). */
function outer(mtu, s4, ipv6) { return (ipv6 ? 40 : 20) + 8 + s4 + 16 + mtu + 16; }
let fits = true;
[1492, 1500, 1480, 1400].forEach(function (p) {
    [false, true].forEach(function (v6) {
        for (let s4 = 0; s4 <= 64; s4++) {
            const m = w.wgMtuForS4(s4, v6, p) || 1420;
            if (m > 1280 && outer(m, s4, v6) > p) {
                ok('outer datagram fits path=' + p + ' ipv6=' + v6 + ' S4=' + s4, false,
                   'mtu=' + m + ' -> outer ' + outer(m, s4, v6));
                fits = false;
                return;
            }
        }
    });
});
if (fits) ok('outer datagram fits the path for every S4 above the 1280 floor', true);

/* ---- an MTU in the .conf must survive the import (#54) ---- */
eq('explicit MTU read', w.explicitMtu({ MTU: '1300' }), 1300);
eq('absent MTU is 0', w.explicitMtu({}), 0);
eq('nonsense MTU ignored', w.explicitMtu({ MTU: 'yes' }), 0);
eq('out-of-range MTU ignored', w.explicitMtu({ MTU: '9000' }), 0);

ipv6Box.checked = false;
pathMtu.value = '1500';
eq('MTU=1300 survives a config the ceiling would leave alone',
   w.effectiveWgMtu({ S4: 0, MTU: '1300' }), 1300);
eq('MTU=1300 wins over a higher S4 ceiling',
   w.effectiveWgMtu({ S4: 40, MTU: '1300' }), 1300);
eq('the ceiling still wins when it is the lower of the two',
   w.effectiveWgMtu({ S4: 40, MTU: '1400' }), 1392);
eq('an MTU at or above the default emits nothing',
   w.effectiveWgMtu({ S4: 0, MTU: '1420' }), 0);
eq('no MTU, no S4, plain path -> nothing to set',
   w.effectiveWgMtu({ S4: 0 }), 0);
eq('wgMtuOpt renders the option', w.wgMtuOpt({ S4: 0, MTU: '1300' }), ' mtu=1300');
eq('wgMtuOpt renders nothing when the default is right', w.wgMtuOpt({ S4: 0 }), '');

/* ---- the path MTU field feeds effectiveWgMtu ---- */
pathMtu.value = '1492';
eq('path field lowers the ceiling', w.effectiveWgMtu({ S4: 12 }), 1408);
eq('path field is read live', w.pathMtuValue(), 1492);
pathMtu.value = '';
eq('empty path field falls back to 1500', w.pathMtuValue(), 1500);
pathMtu.value = '99';
eq('out-of-range path field falls back to 1500', w.pathMtuValue(), 1500);
pathMtu.value = '1500';

/* ---- end to end through the generated script ---- */
function generateWith(extraIface, pathValue) {
    pathMtu.value = String(pathValue || 1500);
    ipv6Box.checked = false;
    w.document.getElementById('conf-input').value = [
        '[Interface]',
        'PrivateKey = ' + 'A'.repeat(43) + '=',
        'Address = 10.13.13.2/32',
        'DNS = 1.1.1.1',
        'Jc = 4', 'Jmin = 40', 'Jmax = 70',
        'S1 = 30', 'S2 = 40'
    ].concat(extraIface).concat([
        'H1 = 1111111111', 'H2 = 2222222222', 'H3 = 3333333333', 'H4 = 444444444',
        '',
        '[Peer]',
        'PublicKey = ' + 'B'.repeat(43) + '=',
        'Endpoint = 198.51.100.1:443',
        'AllowedIPs = 0.0.0.0/0'
    ]).join('\n');
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    return w.document.getElementById('output').dataset.plain || '';
}

const plain = generateWith([], 1500);
ok('plain v1 config on a 1500 path carries no mtu=',
   /interface\/wireguard\/add[^\n]*/.exec(plain) &&
   /interface\/wireguard\/add[^\n]*/.exec(plain)[0].indexOf('mtu=') < 0,
   /interface\/wireguard\/add[^\n]*/.exec(plain) && /interface\/wireguard\/add[^\n]*/.exec(plain)[0]);

const withMtu = generateWith(['MTU = 1300'], 1500);
ok('MTU from the config reaches the wireguard interface',
   /interface\/wireguard\/add[^\n]*mtu=1300/.test(withMtu),
   /interface\/wireguard\/add[^\n]*/.exec(withMtu)[0]);

const pppoe = generateWith(['S4 = 12'], 1492);
ok('PPPoE path lowers the mtu for S4=12',
   /interface\/wireguard\/add[^\n]*mtu=1408/.test(pppoe),
   /interface\/wireguard\/add[^\n]*/.exec(pppoe)[0]);

const pppoePlain = generateWith([], 1492);
ok('PPPoE path leaves a no-S4 config alone',
   /interface\/wireguard\/add[^\n]*/.exec(pppoePlain)[0].indexOf('mtu=') < 0,
   /interface\/wireguard\/add[^\n]*/.exec(pppoePlain)[0]);

/* MTU is a recognised field, so it shows up in the parsed view instead of
 * being silently swallowed. */
const parsed = w.parseConf('[Interface]\nMTU = 1300\n');
eq('parseConf keeps MTU', parsed.interface.MTU, '1300');
ok('MTU is rendered among the parsed fields',
   w.document.getElementById('fields-display').innerHTML.indexOf('MTU') >= 0);

pathMtu.value = '1500';

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
