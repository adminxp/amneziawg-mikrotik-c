/* AWG 3.1 keys in the configurator, driven through jsdom.
 *
 * Dev-only, same as conf3.0-ipv6.test.js:
 *   npm install jsdom && node tests/conf3.1.test.js
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

const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' });
const w = dom.window;

const need = ['parseConf', 'validate', 'v3EnvLines', 'v3ExportLines'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.1 tests ===');

/* A full v3.1 config as amneziawg-tools' `showconf` writes one. */
function conf(extra) {
    return [
        '[Interface]',
        'PrivateKey = QMLpwZ3vTGjRXVDaOFhCXBBS0KOxlEHrJTeSXCXaK1c=',
        'Address = 10.8.1.2/32',
        'Jc = 4', 'Jmin = 50', 'Jmax = 800',
        'S1 = 77', 'S2 = 41', 'S3 = 33', 'S4 = 14',
        'H1 = 1000000', 'H2 = 2000000', 'H3 = 3000000', 'H4 = 4000000',
        'HeaderProtectionKey = QMLpwZ3vTGjRXVDaOFhCXBBS0KOxlEHrJTeSXCXaK1c=',
        extra,
        '',
        '[Peer]',
        'PublicKey = jNDoUHFpXCbcMFTKtCLbGvSuKGyPTFrJfBKPTvhBBnU=',
        'Endpoint = 198.51.100.1:443',
        'AllowedIPs = 0.0.0.0/0',
    ].filter(Boolean).join('\n');
}

/* ---- the keys parse and survive validation ---- */
const good = w.parseConf(conf('RandomTrailers = on\nDisableCookies = on'));
ok('RandomTrailers parsed', good.interface.RandomTrailers === 'on');
ok('DisableCookies parsed', good.interface.DisableCookies === 'on');
ok('a v3.1 config validates', w.validate(w.parseConf(
    conf('RandomTrailers = on\nDisableCookies = off'))).length === 0);
ok('0/1 form validates', w.validate(w.parseConf(
    conf('RandomTrailers = 1\nDisableCookies = 0'))).length === 0);

/* ---- garbage is rejected rather than silently carried over ---- */
const bad = w.validate(w.parseConf(conf('RandomTrailers = maybe')));
ok('a non-boolean is an error', bad.some(e => e.indexOf('RandomTrailers') >= 0), bad.join(' | '));

/* ---- and they reach the container as env vars ---- */
const env = w.v3EnvLines('awg-proxy-1', good.interface).join('\n');
ok('AWG_RANDOM_TRAILERS is exported',
   env.indexOf('key=AWG_RANDOM_TRAILERS value="on"') >= 0, env);
ok('AWG_DISABLE_COOKIES is exported',
   env.indexOf('key=AWG_DISABLE_COOKIES value="on"') >= 0, env);
ok('the 3.0 keys still come along',
   env.indexOf('key=AWG_HEADER_PROTECTION_KEY') >= 0, env);

const exp = w.v3ExportLines(good.interface).join('\n');
ok('the shell export block has them too',
   exp.indexOf('export AWG_RANDOM_TRAILERS=') >= 0 &&
   exp.indexOf('export AWG_DISABLE_COOKIES=') >= 0, exp);

/* ---- a plain v3 config gains nothing ---- */
const plain = w.parseConf(conf(''));
const plainEnv = w.v3EnvLines('awg-proxy-1', plain.interface).join('\n');
ok('without the keys nothing 3.1 is emitted',
   plainEnv.indexOf('AWG_RANDOM_TRAILERS') < 0 &&
   plainEnv.indexOf('AWG_DISABLE_COOKIES') < 0, plainEnv);

console.log('\n' + passes + '/' + (passes + fails) + ' checks passed' + (fails ? ', ' + fails + ' FAILED' : ''));
process.exit(fails ? 1 : 0);
