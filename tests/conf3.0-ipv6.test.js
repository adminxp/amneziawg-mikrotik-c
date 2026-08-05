/* IPv6 behaviour of the configurator, driven through jsdom.
 *
 * Dev-only: the proxy itself has no dependencies, and this needs one.
 *   npm install jsdom && node tests/conf3.0-ipv6.test.js
 * Exits non-zero on the first broken expectation.
 *
 * Lives outside docs/ on purpose: the Pages workflow publishes that whole
 * directory, and test code has no business on the public site.
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

// The page's script is an IIFE-free top-level block, so its functions land on window.
const need = ['isIPv6Literal', 'endpointJoin', 'endpointHost', 'wgMtuForS4',
              'isValidEndpoint', 'isValidEndpointHost', 'isIPLiteral',
              'syncIpv6FromEndpoint', 'ipv6MtuProbeLines', 'ipv6Enabled'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 IPv6 tests ===');

/* ---- literal detection ---- */
ok('literal ::1', w.isIPv6Literal('::1'));
ok('literal 2001:db8::1', w.isIPv6Literal('2001:db8::1'));
ok('literal bracketed', w.isIPv6Literal('[2001:db8::1]'));
ok('literal full form', w.isIPv6Literal('2001:0db8:0000:0000:0000:0000:0000:0001'));
ok('literal v4-mapped', w.isIPv6Literal('::ffff:192.0.2.1'));
ok('not literal: ipv4', !w.isIPv6Literal('198.51.100.1'));
ok('not literal: hostname', !w.isIPv6Literal('vpn.example.com'));
ok('not literal: two :: groups', !w.isIPv6Literal('2001::db8::1'));
ok('not literal: too few groups', !w.isIPv6Literal('ab:cd'));
ok('not literal: non-hex', !w.isIPv6Literal('2001:db8::zz'));
ok('not literal: short full form', !w.isIPv6Literal('1:2:3:4:5:6:7'));

/* ---- MTU formula ---- */
eq('mtu ipv4 S4=0  (no mtu= needed)', w.wgMtuForS4(0, false), 0);
eq('mtu ipv4 S4=16 (no mtu= needed)', w.wgMtuForS4(16, false), 0);
eq('mtu ipv4 S4=40', w.wgMtuForS4(40, false), 1392);
eq('mtu ipv6 S4=0', w.wgMtuForS4(0, true), 1408);
eq('mtu ipv6 S4=12', w.wgMtuForS4(12, true), 1408);
eq('mtu ipv6 S4=16', w.wgMtuForS4(16, true), 1392);
ok('ipv6 always lowers the mtu', w.wgMtuForS4(0, true) > 0 && w.wgMtuForS4(16, true) > 0);
// The 1280 floor (RouterOS/IPv6 minimum) wins over the formula at large S4;
// that pre-existing clamp is out of scope here, so only check below it.
let ceilingOk = true;
for (let s4 = 0; s4 <= 200; s4 += 4) {
    const m = w.wgMtuForS4(s4, true);
    if (m > 1280 && 40 + 8 + s4 + 16 + m + 16 > 1500) {
        ok('ipv6 ceiling fits 1500 at S4=' + s4, false); ceilingOk = false; break;
    }
}
if (ceilingOk) ok('ipv6 ceiling fits 1500 for every S4 above the 1280 floor', true);

/* ---- endpoint join / split ---- */
eq('join ipv4', w.endpointJoin('198.51.100.1', 443), '198.51.100.1:443');
eq('join hostname', w.endpointJoin('vpn.example.com', 443), 'vpn.example.com:443');
eq('join ipv6', w.endpointJoin('2001:db8::1', 443), '[2001:db8::1]:443');
eq('join ipv6 already bracketed', w.endpointJoin('[2001:db8::1]', 443), '[2001:db8::1]:443');
eq('host of ipv4 endpoint', w.endpointHost('198.51.100.1:443'), '198.51.100.1');
eq('host of ipv6 endpoint', w.endpointHost('[2001:db8::1]:443'), '2001:db8::1');
eq('host of hostname endpoint', w.endpointHost('vpn.example.com:443'), 'vpn.example.com');

/* ---- validators ---- */
ok('endpoint [v6]:port valid', w.isValidEndpoint('[2001:db8::1]:443'));
ok('endpoint v4:port valid', w.isValidEndpoint('198.51.100.1:51820'));
ok('endpoint name:port valid', w.isValidEndpoint('vpn.example.com:443'));
ok('endpoint bare v6 rejected', !w.isValidEndpoint('2001:db8::1:443'));
ok('endpoint bad port rejected', !w.isValidEndpoint('[2001:db8::1]:99999'));
ok('endpoint no port rejected', !w.isValidEndpoint('[2001:db8::1]'));
ok('endpointHost accepts v6', w.isValidEndpointHost('2001:db8::1'));
ok('endpointHost accepts bracketed v6', w.isValidEndpointHost('[2001:db8::1]'));
ok('endpointHost accepts v4', w.isValidEndpointHost('198.51.100.1'));
ok('endpointHost accepts name', w.isValidEndpointHost('vpn.example.com'));
ok('endpointHost rejects junk', !w.isValidEndpointHost('not a host'));

/* ---- checkbox auto-tick ---- */
const box = w.document.getElementById('ipv6-enable');
ok('checkbox exists, off by default', box && box.checked === false);
w.syncIpv6FromEndpoint('[2001:db8::1]:443');
ok('ipv6 literal ticks the box', box.checked === true);
w.syncIpv6FromEndpoint('198.51.100.1:443');
ok('ipv4 literal unticks the box', box.checked === false);
box.checked = true;
w.syncIpv6FromEndpoint('vpn.example.com:443');
ok('hostname leaves a manual tick alone', box.checked === true);
box.checked = false;
w.syncIpv6FromEndpoint('vpn.example.com:443');
ok('hostname leaves an untick alone', box.checked === false);

/* live input event on the .conf textarea */
const ta = w.document.getElementById('conf-input');
ta.value = '[Peer]\nEndpoint = [2001:db8::1]:443\n';
ta.dispatchEvent(new w.Event('input', { bubbles: true }));
ok('textarea input ticks the box', box.checked === true);
ta.value = '[Peer]\nEndpoint = 198.51.100.1:443\n';
ta.dispatchEvent(new w.Event('input', { bubbles: true }));
ok('textarea input unticks the box', box.checked === false);

/* ---- :resolve block ---- */
const noProbe4 = w.ipv6MtuProbeLines('awg-proxy-1', '198.51.100.1:443', 0);
eq('no :resolve for an ipv4 literal', noProbe4.length, 0);
const noProbe6 = w.ipv6MtuProbeLines('awg-proxy-1', '[2001:db8::1]:443', 0);
eq('no :resolve for an ipv6 literal', noProbe6.length, 0);
const probe = w.ipv6MtuProbeLines('awg-proxy-1', 'vpn.example.com:443', 16).join('\n');
ok(':resolve emitted for a hostname', probe.indexOf(':resolve vpn.example.com type=ipv6') >= 0, probe);
ok(':resolve guarded by on-error', /:do \{[\s\S]*\} on-error=\{\}/.test(probe), probe);
ok(':resolve sets the lowered mtu', probe.indexOf('mtu=1392') >= 0, probe);
ok(':resolve targets the right interface',
   probe.indexOf('/interface/wireguard/set [find name=wg-awg-proxy-1]') >= 0, probe);

/* ---- generated script, end to end ---- */
function generateWith(endpoint) {
    w.document.getElementById('conf-input').value = [
        '[Interface]',
        'PrivateKey = ' + 'A'.repeat(43) + '=',
        'Address = 10.13.13.2/32',
        'DNS = 1.1.1.1',
        'Jc = 4', 'Jmin = 40', 'Jmax = 70',
        'S1 = 30', 'S2 = 40',
        'H1 = 1111111111', 'H2 = 2222222222', 'H3 = 3333333333', 'H4 = 444444444',
        '',
        '[Peer]',
        'PublicKey = ' + 'B'.repeat(43) + '=',
        'Endpoint = ' + endpoint,
        'AllowedIPs = 0.0.0.0/0'
    ].join('\n');
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    const err = w.document.getElementById('errors-container').textContent.trim();
    if (err) throw new Error('generate() reported: ' + err);
    return w.document.getElementById('output').textContent;
}

const out6 = generateWith('[2001:db8::1]:443');
ok('AWG_REMOTE keeps the brackets',
   out6.indexOf('key=AWG_REMOTE value="[2001:db8::1]:443"') >= 0);
ok('wg interface gets a lowered mtu for an ipv6 endpoint', /listen-port=\d+ mtu=1408 /.test(out6),
   (out6.match(/\/interface\/wireguard\/add[^\n]*/) || [''])[0]);
ok('no :resolve block for a literal', out6.indexOf(':resolve') < 0);
ok('no ipv4 address-list entry for an ipv6 server', out6.indexOf('[2001:db8::1] list=') < 0);

const out4 = generateWith('198.51.100.1:443');
ok('ipv4 endpoint keeps AWG_REMOTE as-is',
   out4.indexOf('key=AWG_REMOTE value="198.51.100.1:443"') >= 0);
ok('ipv4 endpoint leaves the mtu alone (S4=0)', out4.indexOf(' mtu=') < 0,
   (out4.match(/\/interface\/wireguard\/add[^\n]*/) || [''])[0]);

const outName = generateWith('vpn.example.com:443');
ok('hostname endpoint emits the :resolve block', outName.indexOf(':resolve vpn.example.com type=ipv6') >= 0);
ok('hostname endpoint keeps the ipv4 mtu by default', outName.indexOf('listen-port') >= 0 &&
   !/listen-port=\d+ mtu=/.test(outName));

/* ---- non-ru scenario: the loop guard is IPv4-only ---- */
const nonRu6 = w.buildNonRuScenario('awg-proxy-1', [], '', '2001:db8::1', 'disk1', 'container').join('\n');
ok('ipv6 server: no /ip/firewall/address-list entry',
   nonRu6.indexOf('address=2001:db8::1 list=') < 0);
ok('ipv6 server: says why the loop guard is skipped',
   nonRu6.indexOf('AWG server is IPv6') >= 0);
const nonRu4 = w.buildNonRuScenario('awg-proxy-1', [], '', '198.51.100.1', 'disk1', 'container').join('\n');
ok('ipv4 server: loop guard still emitted',
   nonRu4.indexOf('address=198.51.100.1 list=awg-proxy-1-vpn-server') >= 0);

console.log('\n' + passes + '/' + (passes + fails) + ' checks passed' + (fails ? ', ' + fails + ' FAILED' : ''));
process.exit(fails ? 1 : 0);
