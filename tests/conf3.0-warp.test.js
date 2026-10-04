/* WARP-style configs: AmneziaWG keys left out, the app defaults taken instead.
 *
 * AWG configs generated for Cloudflare WARP usually carry Jc/Jmin/Jmax (and
 * sometimes I1) and nothing else: Cloudflare speaks plain WireGuard, so S1/S2
 * and H1-H4 are at their defaults, and the AmneziaWG apps fill them in without
 * a word. The page used to reject such a config with "S1 -- missing". The
 * invariants: a missing key gets exactly the app default, an explicit value is
 * never overridden, the page says what it filled in, and a config with no
 * obfuscation at all is called plain WireGuard instead of passing silently.
 *
 * Dev-only:
 *   npm install jsdom && node tests/conf3.0-warp.test.js
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
function eq(name, a, b) { ok(name, a === b, JSON.stringify(a) + ' !== ' + JSON.stringify(b)); }

function offlineGuard(win) {
    for (const name of ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource']) {
        try { win[name] = function () { throw new Error('test tried the network via ' + name); }; } catch (e) { /* ignore */ }
    }
}
const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/', beforeParse: offlineGuard });
const w = dom.window;

const KEY = 'A'.repeat(43) + '=';
const CF_PUB = 'bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo=';

/* The shape WARP generators put out: Jc/Jmin/Jmax, a long I1, dual-stack
 * Address, MTU 1280, Cloudflare's endpoint - and no S or H at all. */
function warp(extra) {
    return [
        '[Interface]',
        'PrivateKey = ' + KEY,
        'Address = 172.16.0.2/32, 2606:4700:110:8a36:df92:102a:9602:fa18/128',
        'DNS = 1.1.1.1, 1.0.0.1, 2606:4700:4700::1111',
        'MTU = 1280',
        'Jc = 4',
        'Jmin = 40',
        'Jmax = 70',
    ].concat(extra || [], [
        '',
        '[Peer]',
        'PublicKey = ' + CF_PUB,
        'AllowedIPs = 0.0.0.0/0, ::/0',
        'Endpoint = 162.159.192.1:2408',
    ]).join(NL);
}

console.log('=== conf3.0 WARP / missing AmneziaWG keys ===');

/* ---- validation ---- */
const p = w.parseConf(warp());
const errs = w.validate(p);
eq('a WARP config validates clean', errs.length, 0);
ok('every missing key is filled with the app default',
   p.interface.S1 === '0' && p.interface.S2 === '0' && p.interface.H1 === '1' &&
   p.interface.H2 === '2' && p.interface.H3 === '3' && p.interface.H4 === '4',
   JSON.stringify(p.interface));
eq('and the page knows which ones it filled', p.defaulted.join(','), 'S1,S2,H1,H2,H3,H4');
eq('the keys the config did have stay as written', [p.interface.Jc, p.interface.Jmin, p.interface.Jmax].join(','), '4,40,70');

const p2 = w.parseConf(warp());
w.validate(p2);
w.validate(p2);
eq('validating twice does not forget what was filled', p2.defaulted.join(','), 'S1,S2,H1,H2,H3,H4');

/* An explicit value is never overridden, and only what is missing is filled. */
const part = w.parseConf(warp(['S1 = 15', 'H1 = 123456']));
eq('a partial config validates clean', w.validate(part).length, 0);
eq('explicit S1 is kept', part.interface.S1, '15');
eq('explicit H1 is kept', part.interface.H1, '123456');
eq('only the absent keys are listed', part.defaulted.join(','), 'S2,H2,H3,H4');

/* A default that collides with an explicit H: the proxy refuses overlapping H
 * and would not start, so the page has to say so - and say which one it filled. */
const clash = w.parseConf(warp(['H1 = 2']));
const clashErr = w.validate(clash).filter(function (e) { return /overlap/.test(e); });
eq('an explicit H1 equal to the default H2 is rejected', clashErr.length, 1);
ok('and the message names the filled-in key', /H2 not in the config/.test(clashErr[0] || ''), clashErr[0]);
const ranges = w.parseConf(warp(['H1 = 100-200', 'H2 = 150-300', 'H3 = 400', 'H4 = 500']));
const rangeErr = w.validate(ranges).filter(function (e) { return /overlap/.test(e); });
ok('overlapping explicit ranges are rejected too', rangeErr.length === 1 && /H1 and H2/.test(rangeErr[0]), rangeErr.join(' | '));
const apart = w.parseConf(warp(['H1 = 100-200', 'H2 = 201-300', 'H3 = 400', 'H4 = 500']));
eq('touching but disjoint ranges are fine', w.validate(apart).length, 0);

/* Header protection still demands S >= 12 - a default of 0 does not slip past it. */
const hp = w.parseConf(warp(['HeaderProtectionKey = ' + KEY]));
ok('header protection with default S is still an error',
   w.validate(hp).some(function (e) { return /S1 -- must be >= 12/.test(e); }));

/* Nothing AmneziaWG at all. */
const plainText = warp().split(NL).filter(function (l) { return !/^J/.test(l); }).join(NL);
const plain = w.parseConf(plainText);
eq('a plain WireGuard config is not an error', w.validate(plain).length, 0);
ok('but it is recognised as plain WireGuard', w.isPlainWireGuard(plain.interface));
ok('a WARP config is not plain WireGuard (it has junk)', !w.isPlainWireGuard(p.interface));
const withCps = w.parseConf(plainText.replace('[Peer]', 'I1 = <b 0xc200000001><r 10>' + NL + NL + '[Peer]'));
w.validate(withCps);
ok('CPS alone is enough not to be plain WireGuard', !w.isPlainWireGuard(withCps.interface));

/* ---- what the page shows ---- */
const shown = w.renderFields(p);
ok('the filled fields are marked as defaults', /S1<\/span><span class="fval">0 · /.test(shown), shown.slice(0, 300));
ok('a note says what was filled in', shown.indexOf('S1 = 0, S2 = 0, H1 = 1, H2 = 2, H3 = 3, H4 = 4') >= 0);
ok('no plain-WireGuard warning for WARP', shown.indexOf(w.I18N.en.plainWgNote) < 0 && shown.indexOf(w.I18N.ru.plainWgNote) < 0);
const shownPlain = w.renderFields(plain);
ok('plain WireGuard gets its warning',
   shownPlain.indexOf(w.I18N.en.plainWgNote) >= 0 || shownPlain.indexOf(w.I18N.ru.plainWgNote) >= 0);
const full = w.parseConf(warp(['S1 = 15', 'S2 = 20', 'H1 = 11', 'H2 = 22', 'H3 = 33', 'H4 = 44']));
w.validate(full);
ok('a complete config gets no defaults note', w.renderFields(full).indexOf(w.I18N.en.awgDefaultsNote) < 0 &&
   w.renderFields(full).indexOf(w.I18N.ru.awgDefaultsNote) < 0);

/* ---- end to end: the container gets the same values the app would use ---- */
function generate(text) {
    w.clearAll();
    const ta = w.document.getElementById('conf-input');
    ta.value = text;
    ta.dispatchEvent(new w.Event('input', { bubbles: true }));
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    const err = w.document.getElementById('errors-container').textContent.trim();
    return { err: err, out: w.document.getElementById('output').dataset.plain || '' };
}
const g = generate(warp(['I1 = <b 0xc2000000011419fa4bb3599f336777de79f81ca9a8d80d91eeec000044c635cef024a8><r 16>']));
eq('the page generates a script for a WARP config', g.err, '');
[['AWG_JC', '4'], ['AWG_JMIN', '40'], ['AWG_JMAX', '70'], ['AWG_S1', '0'], ['AWG_S2', '0'],
 ['AWG_H1', '1'], ['AWG_H2', '2'], ['AWG_H3', '3'], ['AWG_H4', '4']].forEach(function (kv) {
    ok('container env ' + kv[0] + '=' + kv[1], g.out.indexOf('key=' + kv[0] + ' value="' + kv[1] + '"') >= 0,
       g.out.split(NL).filter(function (l) { return l.indexOf(kv[0]) >= 0; }).join(' | '));
});
ok('the CPS template reaches the container', /key=AWG_I1 value="<b 0xc2/.test(g.out));
ok('the endpoint reaches the container', g.out.indexOf('162.159.192.1:2408') >= 0);

/* ---- a bare wgcf-style WARP config, as people actually paste it ----------
 * Hostname endpoint, public DNS, not a single AmneziaWG key: it has to go
 * through, take the config's DNS for the container, and offer the junk packets
 * that are the whole point of running it through the proxy. */
const bare = [
    '[Interface]',
    'PrivateKey = ' + KEY,
    'Address = 172.16.0.2/32, 2606:4700:110:84de:d5b5:abc3:c864:f390/128',
    'DNS = 1.1.1.1, 1.0.0.1, 2606:4700:4700::1111, 2606:4700:4700::1001',
    'MTU = 1280',
    '',
    '[Peer]',
    'PublicKey = ' + CF_PUB,
    'AllowedIPs = 0.0.0.0/0, ::/0',
    'Endpoint = engage.cloudflareclient.com:2408',
].join(NL);
w.document.getElementById('awg-dns').value = '';
const gb = generate(bare);
ok('a bare WARP config with a hostname endpoint generates',
   gb.out.length > 0 && !w.document.getElementById('errors-container').querySelector('.alert-error'), gb.err);
ok('saying it took the DNS from the config', /1\.1\.1\.1/.test(gb.err), gb.err);
ok('the container resolves it through the DNS from the config', gb.out.indexOf('key=AWG_DNS value="1.1.1.1"') >= 0);
const shownBare = w.document.getElementById('fields-display').innerHTML;
ok('it is called plain WireGuard', shownBare.indexOf('addWarpJunk()') >= 0);
w.addWarpJunk();
const ta = w.document.getElementById('conf-input').value;
ok('the junk button adds Jc/Jmin/Jmax to the config', /Jc = 4/.test(ta) && /Jmin = 40/.test(ta) && /Jmax = 70/.test(ta));
ok('inside [Interface], before [Peer]', ta.indexOf('Jmax = 70') < ta.indexOf('[Peer]'));
const afterJunk = w.document.getElementById('output').dataset.plain || '';
ok('and regenerates with the junk packets in the container env',
   afterJunk.indexOf('key=AWG_JC value="4"') >= 0 && afterJunk.indexOf('key=AWG_JMAX value="70"') >= 0);
ok('the plain-WireGuard warning is gone after that',
   w.document.getElementById('fields-display').innerHTML.indexOf('addWarpJunk()') < 0);
w.addWarpJunk();
const twice = w.document.getElementById('conf-input').value;
eq('pressing it again does not duplicate the lines', (twice.match(/Jc = 4/g) || []).length, 1);
w.document.getElementById('awg-dns').value = '';

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
