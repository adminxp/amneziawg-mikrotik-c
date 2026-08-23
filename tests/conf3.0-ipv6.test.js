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
              'syncIpv6FromEndpoint', 'ipv6MtuProbeLines', 'ipv6Enabled',
              'getTunnelNetwork6', 'vethAddrArgs', 'containerV6Lines',
              'containerV6UninstallLines', 'buildServerCommands'];
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

/* ---- force IPv4: native ISP IPv6 must not walk past the tunnel ----
 * Nothing IPv6 is routed into the tunnel (the wg interface only ever gets an
 * /ip/address), so a router with native IPv6 sends every v6 flow straight out
 * of WAN and the policy routing never sees it. */
ok('the force-IPv4 box is on by default', w.document.getElementById('v6block-enable').checked);
ok('non-ru rejects forwarded IPv6 with a TCP reset',
   /\/ipv6\/firewall\/filter\/add chain=forward action=reject reject-with=tcp-reset protocol=tcp in-interface-list=LAN out-interface-list=WAN comment=awg-proxy-1-v6-force4/.test(nonRu4), nonRu4);
ok('non-ru rejects the rest with admin-prohibited',
   nonRu4.indexOf('action=reject reject-with=icmp-admin-prohibited in-interface-list=LAN out-interface-list=WAN') >= 0, nonRu4);
ok('non-ru keeps ULA/link-local/multicast direct',
   nonRu4.indexOf(':foreach v6net in={"fc00::/7";"fe80::/10";"ff00::/8"}') >= 0 &&
   nonRu4.indexOf('/ipv6/firewall/address-list/add list=awg-proxy-1-v6-direct address=$v6net') >= 0, nonRu4);
// The escape hatch only works if it is consulted before the rejects.
const iAccept6 = nonRu4.indexOf('action=accept dst-address-list=awg-proxy-1-v6-direct');
const iReject6 = nonRu4.indexOf('action=reject reject-with=tcp-reset');
ok('the direct list is consulted before the rejects', iAccept6 >= 0 && iAccept6 < iReject6);
// Cyrillic in comment= makes RouterOS drop the whole command without a word.
ok('every generated line is ASCII', !/[^\x00-\x7f]/.test(nonRu4),
   (nonRu4.match(/^.*[^\x00-\x7f].*$/m) || [''])[0]);
// The clamp stays IPv4-only on purpose: there is no IPv6 in the tunnel to clamp.
ok('no IPv6 MSS clamp is emitted', nonRu4.indexOf('/ipv6/firewall/mangle') < 0);
ok('the IPv4 MSS clamp is still there',
   nonRu4.indexOf('/ip/firewall/mangle/add chain=forward action=change-mss new-mss=clamp-to-pmtu') >= 0);

const dnsFwd6 = w.buildDnsRoutingScenario('awg-proxy-2', [], '').join('\n');
ok('dns-routing gets the same rules', dnsFwd6.indexOf('awg-proxy-2-v6-force4') >= 0);

w.document.getElementById('v6block-enable').checked = false;
const nonRuNoV6 = w.buildNonRuScenario('awg-proxy-1', [], '', '198.51.100.1', 'disk1', 'container').join('\n');
ok('unticking the box emits no /ipv6 rules at all',
   nonRuNoV6.indexOf('v6-force4') < 0 && nonRuNoV6.indexOf('/ipv6/firewall') < 0);
w.document.getElementById('v6block-enable').checked = true;

// /ipv6 is a separate menu — the uninstall sweep over /ip never reaches it.
const unC = w.buildUninstallScriptSource(false, 'disk1', 'awg-proxy-1',
                                         {natSrc: '172.19.0.0/30', hostAddr: '172.19.0.1/30'}, 'non-ru');
const unS = w.buildStandaloneUninstallScriptSource(false, 'awg-proxy-1', 'non-ru');
[['container', unC], ['standalone', unS]].forEach(function (pair) {
    ok('uninstall (' + pair[0] + ') removes the force-IPv4 rules',
       pair[1].indexOf('/ipv6/firewall/filter/remove [find where comment~"awg-proxy-1-v6-force4"]') >= 0 &&
       pair[1].indexOf('/ipv6/firewall/address-list/remove [find where list="awg-proxy-1-v6-direct"]') >= 0, pair[1]);
});

/* ---- the container's own IPv6 leg ----
 * A veth with only an IPv4 address leaves the container link-local and no
 * more: it can neither dial an IPv6 server nor be dialled over IPv6. */
const n6a = w.getTunnelNetwork6('awg-server-1');
const n6b = w.getTunnelNetwork6('awg-server-2');
ok('ULA is a proper fd00::/8 /64', /^fd[0-9a-f]{2}:[0-9a-f]{4}:[0-9a-f]{4}::\/64$/.test(n6a.net), n6a.net);
eq('router side is ::1', n6a.hostAddr, n6a.net.replace('::/64', '::1/64'));
eq('container side is ::2', n6a.vethAddr, n6a.net.replace('::/64', '::2/64'));
ok('the /64 is stable for a prefix', w.getTunnelNetwork6('awg-server-1').net === n6a.net);
ok('two prefixes get different /64s', n6a.net !== n6b.net, n6a.net + ' vs ' + n6b.net);

const v4only = w.vethAddrArgs({vethAddr: '172.19.120.142/30', vethGw: '172.19.120.141'}, null);
eq('veth without ipv6 is unchanged', v4only, 'address=172.19.120.142/30 gateway=172.19.120.141');
const dual = w.vethAddrArgs({vethAddr: '172.19.120.142/30', vethGw: '172.19.120.141'}, n6a);
ok('veth carries both families', dual.indexOf('address=172.19.120.142/30,' + n6a.vethAddr) === 0, dual);
ok('veth gets an ipv6 default route', dual.indexOf('gateway6=' + n6a.vethGw) > 0, dual);

const outLines = w.containerV6Lines('awg-proxy-1', n6a, true).join('\n');
ok('outbound leg: address on the router side', outLines.indexOf('/ipv6/address/add address=' + n6a.hostAddr) >= 0, outLines);
ok('outbound leg: NAT66 out of WAN', /\/ipv6\/firewall\/nat\/add chain=srcnat action=masquerade src-address=/.test(outLines), outLines);
ok('outbound leg: forward accept for the veth', outLines.indexOf('in-interface=veth-awg-proxy-1') >= 0, outLines);
// The container has no RA client of its own; the router must advertise the
// prefix on the veth so the container's kernel SLAACs a default route. Without
// this the egress source degrades to ::1 and nothing leaves (proven live).
ok('outbound leg: veth advertises the prefix (RA)',
   outLines.indexOf('/ipv6/address/add address=' + n6a.hostAddr + ' interface=veth-awg-proxy-1 advertise=yes') >= 0, outLines);
ok('outbound leg: explicit nd sender on the veth',
   outLines.indexOf('/ipv6/nd/add interface=veth-awg-proxy-1 comment=awg-proxy-1-v6') >= 0, outLines);
// SLAAC must stay off: the container keeps sourcing from the static ::2 the
// veth gave it, which is the address the hub's dst-nat conntrack reverses.
ok('outbound leg: prefix advertised without autoconfiguration',
   outLines.indexOf('/ipv6/nd/prefix/add interface=veth-awg-proxy-1 prefix=' + n6a.net + ' autonomous=no') >= 0, outLines);
// The stock nd entry covers interface=all, so advertise=yes immediately spawns
// a dynamic autonomous=yes prefix and the static add then fails with
// "configuration for this prefix already exists". Declaring it first is the
// whole fix — assert the order, not just the presence.
['awg-proxy-1', 'awg-server-1'].forEach(function (p) {
    const lines = w.containerV6Lines(p, n6a, p === 'awg-proxy-1');
    const iPrefix = lines.findIndex(function (s) { return s.indexOf('/ipv6/nd/prefix/add') === 0; });
    const iAddr = lines.findIndex(function (s) { return s.indexOf('/ipv6/address/add') === 0; });
    const iNd = lines.findIndex(function (s) { return s.indexOf('/ipv6/nd/add') === 0; });
    ok('prefix is declared before advertise=yes (' + p + ')',
       iPrefix >= 0 && iAddr > iPrefix && iNd > iPrefix, lines.join('\n'));
});
const inLines = w.containerV6Lines('awg-server-1', n6a, false).join('\n');
ok('inbound-only leg: no NAT66', inLines.indexOf('srcnat') < 0, inLines);
ok('inbound-only leg: still gets the address', inLines.indexOf('/ipv6/address/add') >= 0, inLines);
// A hub cannot answer an off-link client without a default route of its own,
// and RouterOS never puts the veth's gateway6 into the container namespace.
ok('inbound-only leg: also gets a default route via RA',
   inLines.indexOf('advertise=yes') >= 0 &&
   inLines.indexOf('/ipv6/nd/add interface=veth-awg-server-1') >= 0 &&
   inLines.indexOf('autonomous=no') >= 0, inLines);
ok('inbound-only leg: replies are accepted above any local policy',
   inLines.indexOf('/ipv6/firewall/filter/add chain=forward action=accept in-interface=veth-awg-server-1 place-before=0') >= 0, inLines);
const undo = w.containerV6UninstallLines('awg-proxy-1', '  ').join('\n');
['-v6-nat', '-dstnat6', '-v6-out', '-awg-in6', '-v6'].forEach(function (tag) {
    ok('uninstall removes ' + tag, undo.indexOf('awg-proxy-1' + tag + ']') >= 0, undo);
});
ok('uninstall removes the nd sender and its prefix',
   undo.indexOf('/ipv6/nd/remove [find where interface=veth-awg-proxy-1]') >= 0 &&
   undo.indexOf('/ipv6/nd/prefix/remove [find where interface=veth-awg-proxy-1]') >= 0, undo);

/* ---- server (1:N hub) over IPv6 ---- */
function hubScript(ipv6) {
    w.document.getElementById('ipv6-enable').checked = ipv6;
    const ap = {jc: 4, jmin: 40, jmax: 70, s1: 98, s2: 131, s3: 25, s4: 16,
                h1: 1245842713, h2: 2087463251, h3: 3145627891, h4: 4012783461,
                hpKey: 'n0Xq8yQzT5rWvB2mJ7hLdF4sPcA9uKgE1oYtZ3iRxN0=',
                level: 'v3', port: 51833, wgListenPort: 34567, chain: [], cps: null};
    return w.buildServerCommands('mi.example.ru', ap, '10.66.12.0/24', 'disk1', 'usb1',
                                 'awg-server-2', '192.168.11.0/24', '192.168.12.0/24',
                                 'l3', 1, '8.8.8.8');
}

const hub6 = hubScript(true);
const n6hub = w.getTunnelNetwork6('awg-server-2');
ok('hub listens on [::] so both families land on one socket',
   hub6.server.indexOf('key=AWG_LISTEN value="[::]:51833"') >= 0);
ok('hub veth gets gateway6', hub6.server.indexOf('gateway6=' + n6hub.vethGw) >= 0);
ok('hub dst-nats IPv6 to the container',
   hub6.server.indexOf('/ipv6/firewall/nat/add chain=dstnat action=dst-nat protocol=udp dst-port=51833') >= 0 &&
   hub6.server.indexOf('to-address=' + n6hub.vethAddrBare) >= 0);
ok('hub dstnat66 does not pin the router address (it changes)',
   !/\/ipv6\/firewall\/nat\/add chain=dstnat[^\n]*dst-address=/.test(hub6.server));
ok('hub accepts the AWG port in the v6 forward chain',
   /\/ipv6\/firewall\/filter\/add chain=forward action=accept protocol=udp dst-port=51833[^\n]*in-interface-list=WAN/.test(hub6.server));
// The uninstall block always lists the v6 objects, so look for the add.
ok('hub has no NAT66 for the container (it only answers)',
   hub6.server.indexOf('/ipv6/firewall/nat/add chain=srcnat') < 0);
ok('hub client gets an IPv6 way out', hub6.client.indexOf('awg-server-2-v6-nat') >= 0);
ok('hub client veth gets gateway6', hub6.client.indexOf('gateway6=' + n6hub.vethGw) >= 0);
ok('uninstall of the hub clears the v6 objects',
   hub6.server.indexOf('/ipv6/firewall/nat/remove [find where comment=awg-server-2-dstnat6]') >= 0);

const hub4 = hubScript(false);
ok('without ipv6 the hub listens as before', hub4.server.indexOf('key=AWG_LISTEN value=":51833"') >= 0);
ok('without ipv6 no gateway6', hub4.server.indexOf('gateway6=') < 0);
ok('without ipv6 no /ipv6/firewall rules', hub4.server.indexOf('/ipv6/firewall/nat/add') < 0 &&
   hub4.server.indexOf('/ipv6/firewall/filter/add') < 0);
ok('without ipv6 the client stays IPv4-only', hub4.client.indexOf('gateway6=') < 0 &&
   hub4.client.indexOf('/ipv6/firewall/nat/add') < 0 &&
   hub4.client.indexOf('/ipv6/address/add') < 0);
ok('the IPv4 dst-nat is there either way',
   hub4.server.indexOf('/ip/firewall/nat/add chain=dstnat') >= 0 &&
   hub6.server.indexOf('/ip/firewall/nat/add chain=dstnat') >= 0);

/* ---- normal mode: an IPv6 endpoint needs the same leg ---- */
w.document.getElementById('ipv6-enable').checked = true;
const outNormal6 = generateWith('[2001:db8::1]:443');
const n6norm = w.getTunnelNetwork6('awg-proxy-1');
ok('normal mode: veth gets gateway6 for an IPv6 server',
   outNormal6.indexOf('gateway6=' + n6norm.vethGw) >= 0);
ok('normal mode: container gets NAT66 out of WAN',
   outNormal6.indexOf('/ipv6/firewall/nat/add chain=srcnat action=masquerade src-address=' + n6norm.net) >= 0);
ok('normal mode: uninstall clears it',
   outNormal6.indexOf('/ipv6/address/remove [find where comment=awg-proxy-1-v6]') >= 0);
w.document.getElementById('ipv6-enable').checked = false;
const outNormal4 = generateWith('198.51.100.1:443');
ok('normal mode over IPv4 is untouched',
   outNormal4.indexOf('gateway6=') < 0 && outNormal4.indexOf('/ipv6/firewall/nat/add') < 0);

console.log('\n' + passes + '/' + (passes + fails) + ' checks passed' + (fails ? ', ' + fails + ' FAILED' : ''));
process.exit(fails ? 1 : 0);
