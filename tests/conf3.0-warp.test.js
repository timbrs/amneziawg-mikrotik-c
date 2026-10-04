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
/* A bare config to Cloudflare is WARP, not a broken copy: it gets the WARP
 * block. The plain-WireGuard warning is for every other server. */
const shownPlain = w.renderFields(plain);
ok('bare WARP gets the WARP block, not the plain-WireGuard warning',
   shownPlain.indexOf('addWarpQuicCover()') >= 0 &&
   shownPlain.indexOf(w.I18N.en.plainWgNote) < 0 && shownPlain.indexOf(w.I18N.ru.plainWgNote) < 0);
const other = w.parseConf(plainText.replace(CF_PUB, 'B'.repeat(43) + '=')
    .replace('162.159.192.1:2408', '198.51.100.7:51820'));
w.validate(other);
const shownOther = w.renderFields(other);
ok('plain WireGuard to another server gets its warning',
   shownOther.indexOf(w.I18N.en.plainWgNote) >= 0 || shownOther.indexOf(w.I18N.ru.plainWgNote) >= 0);
ok('and no WARP block', shownOther.indexOf('addWarpQuicCover()') < 0);
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

/* ---- the QUIC-shaped packet before the handshake --------------------------
 * Byte for byte what a Chrome QUIC v1 client Initial shows to anyone without
 * the keys (from a capture of Chromium talking to Cloudflare): a 1250-byte
 * datagram, long header 0xc_, version 1, 8-byte DCID, empty SCID, no token,
 * Length 1232 covering exactly the rest. */
function cpsSize(tmpl) {
    let n = 0;
    tmpl.replace(/<b 0x([0-9a-fA-F]*)>|<r (\d+)>/g, function (_, hex, r) {
        n += hex !== undefined ? hex.length / 2 : Number(r);
    });
    return n;
}
const tpl = w.warpQuicI1();
ok('the template matches the Chrome Initial shape', w.WARP_QUIC_RE.test(tpl), tpl);
ok('the proxy can parse it', w.isValidCPSTemplate(tpl));
eq('a 1250-byte datagram, as Chrome sends', cpsSize(tpl), 1250);
const hdr = /^<b 0x(c[0-9a-f])00000001(08)><r 8><b 0x(00)(00)(44d0)><r (\d+)>$/.exec(tpl);
ok('long header, QUIC v1, DCID length 8', !!hdr);
eq('empty SCID, as Chrome sends', hdr && hdr[3], '00');
eq('no token', hdr && hdr[4], '00');
eq('the Length varint covers exactly the rest', hdr && (parseInt(hdr[5], 16) & 0x3fff), Number(hdr && hdr[6]));
const firsts = new Set();
for (let i = 0; i < 64; i++) firsts.add(w.warpQuicI1().slice(5, 7));
ok('the first byte is not one fixed value for every config', firsts.size > 1, [...firsts].join(','));

const before = generate(bare);
ok('a bare WARP config offers the QUIC-shaped packet',
   w.document.getElementById('fields-display').innerHTML.indexOf('addWarpQuicCover()') >= 0, before.err);
w.addWarpQuicCover();
const cover = w.document.getElementById('conf-input').value;
const i1 = (/^I1 = (.+)$/m.exec(cover) || [])[1] || '';
ok('the button writes the template as I1', w.WARP_QUIC_RE.test(i1), i1);
ok('and Jc = 0, so no junk goes in front of it', /^Jc = 0$/m.test(cover) && !/^Jm(in|ax)/m.test(cover));
ok('inside [Interface], before [Peer]', cover.indexOf('Jc = 0') < cover.indexOf('[Peer]') && cover.indexOf('I1 =') < cover.indexOf('[Peer]'));
const afterCover = w.document.getElementById('output').dataset.plain || '';
ok('AWG_I1 reaches the container env', afterCover.indexOf('key=AWG_I1 value="' + i1 + '"') >= 0);
ok('and AWG_JC is 0', afterCover.indexOf('key=AWG_JC value="0"') >= 0);
const coverShown = w.document.getElementById('fields-display').innerHTML;
ok('the button is gone once the packet is there', coverShown.indexOf('addWarpQuicCover()') < 0);
ok('and the page says it is there',
   coverShown.indexOf(w.I18N.en.warpQuicOn) >= 0 || coverShown.indexOf(w.I18N.ru.warpQuicOn) >= 0);
ok('a WARP port other than 443 is called out',
   coverShown.indexOf(w.I18N.en.warpQuicPort) >= 0 || coverShown.indexOf(w.I18N.ru.warpQuicPort) >= 0);
w.addWarpQuicCover();
eq('pressing it again does not duplicate I1', (w.document.getElementById('conf-input').value.match(/^I1 =/gm) || []).length, 1);

/* A generator's own I-packets go: they would follow ours on the wire. */
generate(warp(['I1 = <b 0xc2000000011419fa4bb3><r 16>', 'I2 = <r 40>']));
w.addWarpQuicCover();
const replaced = w.document.getElementById('conf-input').value;
ok('existing I1-I5 are replaced, not stacked',
   (replaced.match(/^I\d =/gm) || []).length === 1 && w.WARP_QUIC_RE.test((/^I1 = (.+)$/m.exec(replaced) || [])[1]));
w.document.getElementById('awg-dns').value = '';

/* ---- WARP ports -----------------------------------------------------------
 * Cloudflare takes WARP over WireGuard on 2408, 500, 1701 and 4500, and the
 * proxy can draw from a list, so a WARP config gets them all - the config's
 * own port first. Only AWG_REMOTE carries the list. */
function remoteEnv(script) {
    const m = /key=AWG_REMOTE value="([^"]*)"/.exec(script);
    return m ? m[1] : null;
}
w.document.getElementById('awg-dns').value = '';
const gp = generate(bare);
eq('a WARP config gets every WARP port', remoteEnv(gp.out), 'engage.cloudflareclient.com:2408,500,1701,4500');
const portLines = gp.out.split(NL).filter(function (l) { return l.indexOf('2408,500') >= 0; });
ok('on the AWG_REMOTE line only', portLines.length === 1 && portLines[0].indexOf('key=AWG_REMOTE') >= 0, portLines.join(' | '));
eq('an odd port from the config goes first',
   remoteEnv(generate(warp().replace('162.159.192.1:2408', '162.159.192.1:854')).out),
   '162.159.192.1:854,2408,500,1701,4500');
eq('the WARP key is enough, whatever the endpoint', remoteEnv(generate(warp()).out), '162.159.192.1:2408,500,1701,4500');
eq('an AllowedPorts comment wins',
   remoteEnv(generate(warp().replace('AllowedIPs', '# AllowedPorts = 500' + NL + 'AllowedIPs')).out), '162.159.192.1:500');
w.clearAll();
w.document.getElementById('conf-input').value = warp();
w.document.getElementById('warp-ports-enable').checked = false;
w.generate();
eq('unticked, the endpoint stays as written',
   remoteEnv(w.document.getElementById('output').dataset.plain || ''), '162.159.192.1:2408');
const otherText = plainText.replace(CF_PUB, 'B'.repeat(43) + '=').replace('162.159.192.1:2408', '198.51.100.7:51820');
eq('another server gets no WARP ports', remoteEnv(generate(otherText).out), '198.51.100.7:51820');

/* ---- the short form for WARP ----------------------------------------------
 * Pasting a WARP config hides the rows it does not need, keeps the DNS field
 * only when the container has no public resolver to take, and shows the
 * random-port box ticked. Everything stays one click away. */
function paste(text) {
    const ta = w.document.getElementById('conf-input');
    ta.value = text;
    ta.dispatchEvent(new w.Event('input', { bubbles: true }));
}
const body = w.document.body;
const hiddenRows = ['netwatch-row', 'v6block-row', 'autoupdate-row', 'perf-row', 'df-row', 'ipv6-row',
                    'extratime-row', 'offline-row', 'path-mtu-row', 'storage-row', 'prefix-row'];
w.clearAll();
paste(bare);
ok('pasting WARP switches to the short form', body.classList.contains('warp-simple'));
ok('the advanced rows are the ones that step aside',
   hiddenRows.every(function (id) { return w.document.getElementById(id).classList.contains('warp-adv'); }));
ok('the routing scenario stays', !w.document.getElementById('scenario-row').classList.contains('warp-adv'));
eq('the WARP row is shown', w.document.getElementById('warp-row').style.display, 'block');
ok('with the random-port box ticked', w.document.getElementById('warp-ports-enable').checked);
eq('listing the ports', w.document.getElementById('warp-ports-list').textContent, '2408,500,1701,4500');
ok('no DNS question: 1.1.1.1 from the config will do',
   w.document.getElementById('dns-row').classList.contains('warp-adv'));
paste(bare.replace(/^DNS = .*$/m, 'DNS = 10.64.0.1'));
ok('a hostname with only a private resolver asks for DNS',
   !w.document.getElementById('dns-row').classList.contains('warp-adv'));
paste(warp().replace(/^DNS = .*$/m, 'DNS = 10.64.0.1'));
ok('an IP endpoint never needs DNS', w.document.getElementById('dns-row').classList.contains('warp-adv'));
w.toggleWarpAdvanced();
ok('"show all settings" brings everything back', !body.classList.contains('warp-simple'));
w.toggleWarpAdvanced();
ok('and hides it again', body.classList.contains('warp-simple'));
w.document.getElementById('awg-dns').value = '';
const gs = generate(bare);
ok('the short form still generates', gs.out.length > 0 && gs.out.indexOf('key=AWG_REMOTE') >= 0, gs.err);
paste(otherText);
ok('another server gets the full form', !body.classList.contains('warp-simple'));
eq('and no WARP row', w.document.getElementById('warp-row').style.display, 'none');
paste(bare);
w.document.querySelector('input[name="deploy-mode"][value="site-to-site"]').checked = true;
w.onModeChange();
ok('site-to-site ignores the pasted WARP config', !body.classList.contains('warp-simple'));
w.clearAll();
ok('clearing resets the form', !body.classList.contains('warp-simple'));

/* ---- no AmneziaVPN: generate a WARP config -------------------------------- */
const gen = w.document.getElementById('warp-gen');
ok('the WARP generator option is on the page', !!gen);
ok('it links the generator',
   !!gen.querySelector('a[href="https://lanrat.github.io/wireguard-warp-generator/"]'));
ok('and wgcf as the other way', !!gen.querySelector('a[href="https://github.com/ViRb3/wgcf"]'));
ok('every line of it is translated', ['warpGenTitle', 'warpGenStep1', 'warpGenStep2', 'warpGenStep3', 'warpGenAlt', 'warpGenNote']
   .every(function (k) { return w.I18N.en[k] && w.I18N.ru[k] && w.I18N.en[k] !== w.I18N.ru[k]; }));
w.document.querySelector('input[name="deploy-mode"][value="server"]').checked = true;
w.onModeChange();
eq('hidden where there is no config to paste', gen.style.display, 'none');
w.clearAll();
eq('shown again in the container mode', gen.style.display, 'block');
const words = Object.keys(w.I18N.en).filter(function (k) { return /^warp/.test(k); })
    .map(function (k) { return w.I18N.en[k] + ' ' + w.I18N.ru[k]; }).join(' ');
ok('neutral wording', !/bypass|circumvent|censor|обход|цензур/i.test(words));

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
