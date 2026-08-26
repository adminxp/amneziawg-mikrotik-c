/* The update script must never be what kills a tunnel for good. Driven through jsdom.
 *
 * A nightly /container/repull is a robot with root on a live router, and the two
 * ways it can go wrong are both silent:
 *
 *   1. It bricks the container. A pull that dies leaves it with no image-id; a pull
 *      that ends in "skip importing same version" can leave root-dir empty, so the
 *      container starts, dies on execvpe and stops. Either way the tunnel is down.
 *   2. It then decides everything is fine. The version was already recorded, so the
 *      next night the script says "already on vX - nothing to pull" and walks away
 *      from a dead tunnel. Forever, with nobody watching at 04:30.
 *
 * So the invariants below are about recovery, not about updating:
 *   - health is checked BEFORE the version, and a sick container is pulled again
 *     no matter what version it claims to be;
 *   - the version is only recorded once the container is actually back up;
 *   - every failure path logs and leaves the next run able to retry;
 *   - the script never touches netwatch, routes or the WireGuard interface - while
 *     the container is down, that failover is what keeps the router online.
 *
 * Dev-only: the proxy itself has no dependencies, and this needs one.
 *   npm install jsdom && node tests/conf3.0-recovery.test.js
 * Exits non-zero if anything is broken.
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

// The page must never reach the network from a test run, so every way out is
// replaced with a throw before the document is parsed. jsdom already refuses to
// load subresources (no `resources: "usable"`), and `example.invalid` is a
// reserved TLD - this covers the rest, and turns a future fetch() added to the
// configurator into a failed test rather than traffic from CI.
const netCalls = [];
function offlineGuard(win) {
    const boom = name => function () {
        netCalls.push(name);
        throw new Error('test tried to use the network via ' + name);
    };
    for (const name of ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource']) {
        try { win[name] = boom(name); } catch (e) { /* read-only in some builds */ }
    }
    try { win.navigator.sendBeacon = boom('sendBeacon'); } catch (e) { /* ignore */ }
}

const dom = new JSDOM(html, {
    runScripts: 'dangerously',
    url: 'https://example.invalid/',
    beforeParse: offlineGuard,
});
const w = dom.window;

const need = ['buildUpdateScriptSource', 'buildUpdateSchedulerLines', 'generate', 'clearAll'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 update-recovery tests ===');

const P = 'awg-proxy-1';
const upd = w.buildUpdateScriptSource(P);
const at = needle => upd.indexOf(needle);

/* ---------------------------------------------------------------- 1. order */
// The whole failure mode is a version check that runs first and short-circuits.
const healthAt = at(':local healthy true');
const versionAt = at(':local curVer ""');
const decisionAt = at(':local doPull true');
ok('health is established before the version is even read',
   healthAt > 0 && versionAt > healthAt, 'health@' + healthAt + ' version@' + versionAt);
ok('and the skip decision comes after both',
   decisionAt > versionAt, 'decision@' + decisionAt);

/* ------------------------------------------------- 2. sickness beats version */
// Every skip lives inside `:if ($healthy)`, so an unhealthy container always pulls.
ok('a sick container is never skipped as up to date',
   /:local doPull true[\s\S]{0,40}:if \(\$healthy\) do=\{[\s\S]{0,400}:if \(\$doPull\) do=\{/.test(upd));
// An unreachable release API must not turn into a nightly pull on every router:
// that is a restart the tunnel does not need and anonymous registry traffic nobody
// asked for. A healthy container is only pulled against a version we confirmed.
ok('an unknown published version leaves a healthy container alone',
   /:if \(\$newVer = ""\) do=\{[\s\S]{0,120}:set doPull false/.test(upd) &&
   upd.indexOf('leaving the healthy container alone') > 0);
ok('and the pull only happens when something actually says so',
   /:if \(\$doPull\) do=\{/.test(upd));
ok('and says out loud that it is forcing a pull',
   at(':if (!$healthy) do={') > 0 && at('container unhealthy, forcing a pull') > 0);
ok('an empty image-id counts as sick',
   /:if \(\[:len \[\/container\/get \$cid image-id\]\] = 0\) do=\{ :set healthy false \}/.test(upd));
ok('a stopped container that should be running counts as sick',
   /:do \{ :if \(\[\/container\/get \$cid stopped\] = true\) do=\{ :set healthy false \} \} on-error=\{\}/.test(upd));
ok('"should be running" is read from start-on-boot, not guessed',
   /:do \{ :if \(\[\/container\/get \$cid start-on-boot\] != true\) do=\{ :set wantRunning false \} \} on-error=\{\}/.test(upd));

/* ------------------------------------------------------- 3. cheapest repair */
// If the image is intact and the container is merely stopped, a start fixes it
// without a pull - and without the ~10s restart a pull would cost.
ok('a stopped-but-intact container is just started',
   /:if \(!\$healthy && \$wantRunning && \[:len \[\/container\/get \$cid image-id\]\] > 0\) do=\{/.test(upd));
ok('and the start is verified, not assumed',
   upd.indexOf('Container is back up') > 0);

/* ------------------------------------------------ 4. escalation, in order */
const retryAt = at('Pull failed - retrying once in 15s');
const rebuildAt = at('Container did not come back up - rebuilding it from the registry');
const giveUpAt = at('Rebuild failed - the tunnel is down.');
ok('a failed pull is retried', retryAt > 0);
ok('then the container is rebuilt from the registry', rebuildAt > retryAt);
ok('and only then does it give up', giveUpAt > rebuildAt);
ok('the rebuild reuses the original root-dir, not a guessed one',
   /\/container\/add remote-image=\$img interface=veth-awg-proxy-1 envlist=awg-proxy-1-env hostname=awg-proxy-1 root-dir=\$rootDirOrig /.test(upd));
ok('and the rebuilt container is verified too',
   /:do \{ :if \(\[\/container\/get \[:pick \$newCids \(\[:len \$newCids\] - 1\)\] stopped\] != true\) do=\{ :set backUp true \} \} on-error=\{\}/.test(upd));

// An in-place upgrade leaves the previous container stopped on the same veth. Acting
// on that one instead of the live one would "update" a spare and leave the tunnel
// running an old image - or worse, start both.
ok('the running container is the one picked, not the stopped spare',
   /:local cids \[\/container\/find where interface=veth-awg-proxy-1 and !stopped\]/.test(upd));
ok('with start-on-boot as the tie-break when nothing is running',
   /:set cids \[\/container\/find where interface=veth-awg-proxy-1 and start-on-boot=yes\]/.test(upd));
ok('and the newest id wins, never a stale first match',
   upd.indexOf(':local cid [:pick $cids ([:len $cids] - 1)]') > 0 &&
   upd.indexOf('[:pick $cids 0]') < 0);

/* ------------------------------------- 5. never record a version we did not get */
// This is the line that turns one bad night into a permanently dead tunnel.
ok('the version is recorded only when the container is back up',
   /:if \(\$backUp && \$newVer != ""\) do=\{/.test(upd));
ok('and "update done" is likewise only claimed when it is true',
   /:if \(\$backUp\) do=\{[\s\S]{0,120}Update done/.test(upd));

/* ------------------------------------------------- 6. the next run must run */
// A script that dies mid-way leaves no log and no retry, so the destructive steps
// are all wrapped: an error inside :do does not abort the rest of the script.
const risky = [
    '/container/add remote-image=$img interface=veth-awg-proxy-1',
    '/container/stop $cid',
    '/container/remove $cid',
    '/container/set $cid remote-image=$img',
];
risky.forEach(cmd => {
    const line = upd.split(NL).find(l => l.indexOf(cmd) >= 0) || '';
    ok('guarded against aborting the script: ' + cmd.slice(0, 34),
       line.indexOf(':do {') >= 0 && line.indexOf('on-error={}') >= 0, line.trim().slice(0, 90));
});
ok('the script never raises :error itself', upd.indexOf(':error') < 0);
ok('every give-up path leaves a log line to find in the morning',
   (upd.match(/:log (error|warning)/g) || []).length >= 3);
ok('and says the next run will retry', upd.indexOf('runs again on the next schedule') > 0);

/* ------------------------- 7. hands off everything that keeps the router online */
// While the tunnel is down, netwatch failover is what keeps traffic flowing. The
// update script must not remove, disable or re-add any of it.
const forbidden = ['/tool/netwatch', '/ip/route', '/routing/', '/interface/wireguard',
                   '/ip/firewall', '/ip/dns', '/system/scheduler', '/system/reboot'];
forbidden.forEach(m => ok('never touches ' + m, upd.indexOf(m) < 0,
                          (upd.split(NL).find(l => l.indexOf(m) >= 0) || '').trim().slice(0, 80)));
ok('and only ever writes the one env key it owns',
   (upd.match(/\/container\/envs\/(add|remove)/g) || []).length === 2 &&
   (upd.match(/AWG_IMAGE_VER/g) || []).length >= 2);

/* --------------------------------- 8. the nightly retry actually exists */
w.document.getElementById('autoupdate-enable').checked = true;
const sched = w.buildUpdateSchedulerLines(P).join(NL);
ok('the scheduler repeats daily, so a bad night is retried the next one',
   /interval=1d/.test(sched) && /on-event="\/system\/script\/run awg-proxy-1-update"/.test(sched));
ok('it is skipped rather than fatal when device-mode forbids schedulers',
   /:do \{ :set schedOk \[\/system\/device-mode\/get scheduler\] \} on-error=\{\}/.test(sched) &&
   sched.indexOf('Update by hand') > 0);

/* ------------------------------- 9. a broken file install can still be saved */
// repull needs a registry install. If a file-installed container is dead and the
// router is new enough, pointing it at the registry is the only way back.
ok('a dead file install is pointed at the registry on 7.22+',
   /:if \(\$img = "" && \$minor >= 22 && !\$healthy\) do=\{/.test(upd));
ok('a healthy file install is left exactly as it is',
   upd.indexOf('/container/repull only updates registry installs') > 0);
ok('armv5 boards get their own tag when that happens',
   upd.indexOf(':set imgTag "latest-armv5"') > 0);
ok('and it is logged, since it changes how the container is installed',
   upd.indexOf('broken file install, switching to the registry') > 0);

/* -------------------- 9b. it has to parse on the router at all */
// A script that does not parse never runs, so the scheduler silently does nothing
// and a bricked container stays bricked. These are the RouterOS-isms that bite.
const updLines = upd.split(NL);
const concatOutsideParens = updLines.filter(l => /:(log (info|warning|error)|put) "[^"]*" \./.test(l));
ok('no bare "text" . $var concatenation in :put/:log (RouterOS wants parens)',
   concatOutsideParens.length === 0, concatOutsideParens.join(' | ').slice(0, 140));
// \" is an escaped quote inside a RouterOS string, not a delimiter
const oddQuotes = updLines.filter(l => (((l.replace(/\\"/g, '').match(/"/g)) || []).length % 2) !== 0);
ok('every line has balanced quotes', oddQuotes.length === 0, oddQuotes.join(' | ').slice(0, 140));
const opens = (upd.match(/[[({]/g) || []).length, closes = (upd.match(/[\])}]/g) || []).length;
ok('brackets balance across the whole script', opens === closes, opens + ' vs ' + closes);
ok('the script body is one source={...} block',
   /^\/system\/script\/add name=awg-proxy-1-update comment=awg-proxy-1 source=\{/.test(upd) &&
   upd.trim().endsWith('}'));

/* ------------- 9c. RouterOS must not decide to pull on its own */
// This is what actually bricked a live container: a container that carries both
// file= and remote-image= is "changed" in RouterOS eyes, so it downloads the image
// by itself - on `set remote-image`, and on 7.23 at other moments too. If that
// download breaks (broken tar), the container is left with no image and the tunnel
// is down. A pull must only ever happen because something asked for one.
ok('the update script pins ignore-remote-image-change before pulling',
   /:do \{ \[:parse "\/container\/set \$cid ignore-remote-image-change=yes"\] \} on-error=\{\}/.test(upd));
ok('and again on a container it rebuilds',
   upd.indexOf('ignore-remote-image-change=yes"] } on-error={}') > 0);
// 7.22 and older have no such property: :parse defers it to runtime so the script
// still parses there, and :do swallows the "no such argument" error.
ok('it is written so older RouterOS still parses the script',
   /\[:parse "[^"]*ignore-remote-image-change/.test(upd));

/* ------------------------- 10. the safety net is actually installed alongside */
w.clearAll();
const ta = w.document.getElementById('conf-input');
ta.value = [
    '[Interface]', 'PrivateKey = ' + 'A'.repeat(43) + '=', 'Address = 10.13.13.2/32',
    'DNS = 1.1.1.1',
    'Jc = 4', 'Jmin = 40', 'Jmax = 70', 'S1 = 30', 'S2 = 40',
    'H1 = 1111111111', 'H2 = 2222222222', 'H3 = 3333333333', 'H4 = 444444444', '',
    '[Peer]', 'PublicKey = ' + 'B'.repeat(43) + '=', 'Endpoint = 198.51.100.1:443',
    'AllowedIPs = 0.0.0.0/0'
].join(NL);
ta.dispatchEvent(new w.Event('input', { bubbles: true }));
// netwatch failover belongs to the split-routing scenarios, not to "tunnel only"
[...w.document.getElementsByName('scenario')].forEach(r => {
    r.checked = (r.value === 'non-ru');
    r.dispatchEvent(new w.Event('change', { bubbles: true }));
});
w.document.getElementById('netwatch-enable').checked = true;
w.document.getElementById('autoupdate-enable').checked = true;
w.document.getElementById('errors-container').innerHTML = '';
w.generate();
const script = w.document.getElementById('output').dataset.plain || '';
// The routing half of the output is a separate block on the page.
const routingEl = w.document.getElementById('routing-output');
const routing = (routingEl && routingEl.dataset.plain) || '';

ok('the generated install carries the update script',
   script.indexOf('/system/script/add name=' + P + '-update ') >= 0);
ok('and the routing half carries the netwatch failover',
   /\/tool\/netwatch\/add/.test(routing), 'routing block ' + routing.length + ' chars');

// The failover disables this tunnel's default route so traffic falls through to the
// next tunnel by distance, or straight out of WAN. It must never disable the WAN.
const nwLine = routing.split(NL).find(l => l.indexOf('/tool/netwatch/add') >= 0) || '';
ok('down-script disables the tunnel route, not connectivity',
   /down-script="\/ip\/route\/disable \[find where comment=/.test(nwLine), nwLine.slice(0, 130));
ok('and up-script puts it back when the tunnel returns',
   /up-script="\/ip\/route\/enable \[find where comment=/.test(nwLine));
ok('the probe is pinned to this tunnel, so failback is honest',
   nwLine.indexOf('src-address=$wgAddr') >= 0 && routing.indexOf(P + '-probe') >= 0);
ok('a bricked container therefore costs a disabled route, not a dead router',
   /dst-address=0\.0\.0\.0\/0/.test(nwLine));

ok('and the uninstall takes the update job out with everything else',
   script.indexOf('/system/scheduler/remove [find where comment~"' + P + '"]') >= 0 &&
   script.indexOf('/system/script/remove [find where comment~"' + P + '"') >= 0);

/* ---------------------------------------------- the tests stay offline */
// Not a promise in a comment: if the page ever grows a fetch() or a CDN <script>,
// these fail instead of quietly making network calls wherever the tests run.
ok('no network call was made while running the page', netCalls.length === 0,
   netCalls.join(', '));
ok('the page pulls in no external subresources',
   !/<script[^>]+src=|<link[^>]+href="https?:|@import\s+url\(https?:/i.test(html));
ok('and no runtime network APIs are used in its source',
   !/\bfetch\s*\(|XMLHttpRequest|navigator\.sendBeacon|new\s+WebSocket/.test(html));

console.log('');
console.log(passes + '/' + (passes + fails) + ' checks passed');
process.exit(fails ? 1 : 0);
