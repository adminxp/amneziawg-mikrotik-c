/* Offline install (#51).
 *
 * A router that cannot reach GitHub used to need a second router with a working
 * tunnel just to get the first one installed. The installer already prefers a
 * local archive when it finds one; what was missing was any way to know which
 * file to fetch, and the RU list still spent a minute per run resolving a name
 * that will never answer.
 *
 * Dev-only:
 *   npm install jsdom && node tests/conf3.0-offline.test.js
 */
const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

const NL = String.fromCharCode(10);
const html = fs.readFileSync(path.join(__dirname, '..', 'docs', 'conf3.0.html'), 'utf8');

let fails = 0, passes = 0;
function ok(name, cond, extra) {
    if (cond) { passes++; console.log('  PASS  ' + name); }
    else { fails++; console.log('  FAIL  ' + name + (extra ? ': ' + extra : '')); }
}

const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' });
const w = dom.window;

const need = ['offlineEnabled', 'buildOfflinePrepScript', 'imageFileDetectLines',
              'buildImageSetupLines', 'buildNonRuScenario', 'generate'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 offline install tests ===');

const box = w.document.getElementById('offline-enable');
ok('offline checkbox exists, off by default', box && box.checked === false);

function setOffline(v) { box.checked = v; }

/* A line that changes RouterOS state, with quoted strings blanked so the help
 * text the prep script prints does not count. */
function mutates(line) {
    const bare = line.replace(/"[^"]*"/g, '""');
    return /\/(ip|ipv6|interface|container|system|routing|disk|file|tool)\/[a-z0-9/-]*\/(add|set|remove|move|enable|disable|format-drive)(\s|$)/.test(bare)
        || /\/import(\s|$)/.test(bare) || /\/tool\/fetch/.test(bare);
}

/* ---- step 0 is read-only and names both files ---- */
const prep = w.buildOfflinePrepScript('awg-proxy-1', '__auto', true);
const prepLines = prep.split(NL);
ok('step 0 changes nothing', !prepLines.some(mutates),
   prepLines.filter(mutates).join(' | '));
ok('step 0 never formats a drive', prep.indexOf('format-drive') < 0, prep);
ok('step 0 detects the architecture', prep.indexOf('/system/resource/get architecture-name') >= 0);
ok('step 0 knows about the 7.20 build', prep.indexOf('-7.20-Docker') >= 0);
ok('step 0 knows about the armv5 boards', prep.indexOf('awg-proxy-armv5') >= 0);
ok('step 0 prints the image name', prep.indexOf(':put ("   file: " . $file)') >= 0, prep);
ok('step 0 prints the image url', prep.indexOf('releases/latest/download/') >= 0, prep);
ok('step 0 prints where the image goes', prep.indexOf('put it here') >= 0, prep);
ok('step 0 prints the RU list when it is used', prep.indexOf('ru_ranges_timeout.rsc') >= 0, prep);
ok('step 0 picks the RU flavour from device-mode',
   prep.indexOf('/system/device-mode/get scheduler') >= 0, prep);

const prepNoRu = w.buildOfflinePrepScript('awg-proxy-1', 'disk1', false);
ok('no RU list, no RU section', prepNoRu.indexOf('ru_ranges') < 0, prepNoRu);
ok('disk1 keeps the plain files path', prepNoRu.indexOf(':local dest $file') >= 0, prepNoRu);

const prepUsb = w.buildOfflinePrepScript('awg-proxy-1', 'usb1', true);
ok('an external disk is named explicitly', prepUsb.indexOf(':set disk "usb1"') >= 0, prepUsb);

/* The installer and step 0 must never name different files. */
const detect = w.imageFileDetectLines('  ').join(NL);
['awg-proxy-arm64', 'awg-proxy-arm', 'awg-proxy-armv5', 'awg-proxy-amd64', '-7.20-Docker'].forEach(function (n) {
    ok('step 0 and the installer share the name "' + n + '"',
       prep.indexOf(n) >= 0 && detect.indexOf(n) >= 0);
});

/* ---- the installer stops reaching out ---- */
setOffline(false);
const onlineSetup = w.buildImageSetupLines('awg-proxy-1', 'disk1', {}).join(NL);
setOffline(true);
const offlineSetup = w.buildImageSetupLines('awg-proxy-1', 'disk1', {}).join(NL);
setOffline(false);

ok('online: the release archive is fetched', onlineSetup.indexOf('/tool/fetch url=$url') >= 0);
ok('offline: nothing is fetched', offlineSetup.indexOf('/tool/fetch') < 0,
   offlineSetup.split(NL).filter(function (l) { return l.indexOf('/tool/fetch') >= 0; }).join(' | '));
ok('offline: no registry pull either', offlineSetup.indexOf('remote-image=') < 0,
   offlineSetup.split(NL).filter(function (l) { return l.indexOf('remote-image=') >= 0; }).join(' | '));
ok('offline: a local archive is still used', offlineSetup.indexOf('Using image already on the router') >= 0);
ok('offline: a missing archive is named, not a network error',
   offlineSetup.indexOf('Image not found on the router') >= 0 &&
   offlineSetup.indexOf(':error "Offline install: image file missing"') >= 0, offlineSetup);
ok('offline: the error repeats the download url', offlineSetup.indexOf('releases/latest/download/') >= 0);

/* ---- the RU update script ---- */
function ru(offline) {
    setOffline(offline);
    const out = w.buildNonRuScenario('awg-proxy-1', [], 'CloudFlare', '198.51.100.1', 'disk1', 'container').join(NL);
    setOffline(false);
    return out;
}
const ruOn = ru(false), ruOff = ru(true);
ok('online: the RU list is downloaded', ruOn.indexOf('ru-ranges/releases') >= 0);
ok('online: and waits for the network first', ruOn.indexOf('waitNet < 60') >= 0);
ok('offline: the RU list is not downloaded', ruOff.indexOf('ru-ranges/releases') < 0);
ok('offline: no minute-long wait for a name that will not resolve',
   ruOff.indexOf('waitNet < 60') < 0, ruOff.slice(0, 200));
ok('offline: no misleading no-network warning', ruOff.indexOf('no network after 60s') < 0);
ok('offline: either list file on the router is accepted', ruOff.indexOf('altName') >= 0, ruOff);
ok('offline: a missing list is logged as such',
   ruOff.indexOf('offline install and no list file on the router') >= 0, ruOff);
ok('offline: the cached list is still imported', ruOff.indexOf('RU list: imported from cache') >= 0);

/* ---- end to end: the section shows up ---- */
function generateWith(offline) {
    setOffline(offline);
    w.document.getElementById('awg-dns').value = '';
    w.document.getElementById('conf-input').value = [
        '[Interface]',
        'PrivateKey = ' + 'A'.repeat(43) + '=',
        'Address = 10.13.13.2/32',
        'Jc = 4', 'Jmin = 40', 'Jmax = 70',
        'S1 = 30', 'S2 = 40',
        'H1 = 1111111111', 'H2 = 2222222222', 'H3 = 3333333333', 'H4 = 444444444',
        '',
        '[Peer]',
        'PublicKey = ' + 'B'.repeat(43) + '=',
        'Endpoint = 198.51.100.1:443',
        'AllowedIPs = 0.0.0.0/0'
    ].join(NL);
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    const err = w.document.getElementById('errors-container').textContent.trim();
    const sec = w.document.getElementById('offline-section');
    const res = {
        err: err,
        shown: sec.style.display === 'block',
        text: w.document.getElementById('offline-output').dataset.plain || '',
        install: w.document.getElementById('output').dataset.plain || ''
    };
    setOffline(false);
    return res;
}

const gOn = generateWith(false);
ok('online: generation still works', gOn.err.length === 0, gOn.err);
ok('online: no step 0 section', !gOn.shown);

const gOff = generateWith(true);
ok('offline: generation works', gOff.err.length === 0, gOff.err);
ok('offline: step 0 is shown', gOff.shown);
ok('offline: step 0 names the image', gOff.text.indexOf('Container image') >= 0, gOff.text);
ok('offline: the install script does not fetch', gOff.install.indexOf('/tool/fetch url=$url') < 0);

/* ---- no request amplification ------------------------------------------
 * Everything here runs on thousands of routers against a handful of hosts, so
 * a network call that ends up inside a loop is not a small mistake. Assert the
 * shape rather than trusting review: every outbound request is a single shot. */

const NETCMD = /(\/tool\/fetch|remote-image=|:resolve)/;
const LOOPOPEN = /^\s*:(while|for|foreach)/;

function loopedNetOps(text) {
    const lines = text.split(NL);
    let depth = 0;
    const bad = [];
    lines.forEach(function (l) {
        if (LOOPOPEN.test(l)) depth++;
        else if (/^\s*\}/.test(l) && depth > 0) depth--;
        if (depth > 0 && NETCMD.test(l) && l.indexOf(':resolve') < 0) bad.push(l.trim());
    });
    return bad;
}

function requestCount(text) {
    return (text.match(/\/tool\/fetch |remote-image=\$img /g) || []).length;
}

const installOnline = generateWith(false).install;
const installOffline = generateWith(true).install;

ok('no fetch or registry pull sits inside a loop (install)',
   loopedNetOps(installOnline).length === 0, loopedNetOps(installOnline).join(' | '));
ok('no fetch or registry pull sits inside a loop (RU update)',
   loopedNetOps(ruOn).length === 0, loopedNetOps(ruOn).join(' | '));
ok('the online install makes only a handful of requests',
   requestCount(installOnline) <= 6, String(requestCount(installOnline)));
ok('the offline install makes fewer still',
   requestCount(installOffline) < requestCount(installOnline),
   requestCount(installOffline) + ' vs ' + requestCount(installOnline));
ok('step 0 makes none at all', requestCount(prep) === 0, String(requestCount(prep)));

// An offline router cannot reach api.github.com, so a nightly update check
// would fail every night forever.
ok('online: the daily update scheduler is created',
   installOnline.indexOf('/system/scheduler/add name=awg-proxy-1-update') >= 0);
ok('offline: no daily update scheduler',
   installOffline.indexOf('/system/scheduler/add name=awg-proxy-1-update') < 0,
   installOffline.split(NL).filter(function (l) { return l.indexOf('scheduler/add') >= 0; }).join(' | '));
ok('offline: but the update script is still there to run by hand',
   installOffline.indexOf('awg-proxy-1-update') >= 0);

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
