/* The direct-AmneziaWG-client .conf the server (1:N hub) mode hands out.
 *
 * The invariant under test: every protocol-significant field of the generated
 * profile has to survive into every representation that needs it — container
 * env, share link and the client .conf. A field present in the env but missing
 * from the .conf produces a client that handshakes and then goes quiet, which
 * is what #66 reported.
 *
 * Dev-only:
 *   npm install jsdom && node tests/conf3.0-awgconf.test.js
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

const need = ['buildAwgConf', 'awgProfileConfLines', 'parseConf', 'validate',
              'buildServerCommands', 'generateAWGParams'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 direct-client .conf tests ===');

const KEY = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=';

/* A full v3.1 profile in the generator's own shape. */
function profile(extra) {
    const p = {
        jc: 3, jmin: 73, jmax: 660,
        s1: 46, s2: 43, s3: 26, s4: 12,
        h1: '1648096627-1648096648',
        h2: '4120673005-4120673019',
        h3: '1467089208-1467089235',
        h4: '4197712562-4197712614',
        hpKey: KEY,
        cps: ['<b 0xc9f859323c7d><r 11>', '<b 0x482d><r 5>']
    };
    for (const k in (extra || {})) p[k] = extra[k];
    return p;
}

function conf(p) {
    return w.buildAwgConf({
        clientPriv: KEY,
        serverPub: KEY,
        awgParams: p,
        endpoint: '198.51.100.1',
        awgPort: 30892,
        tunClientIP: '10.182.242.2',
        dnsServer: '1.1.1.1'
    });
}

/* ---- v3 / v3.1 fields reach the client (#66) ---- */
const c3 = conf(profile({ randomTrailers: 'on', disableCookies: 'on' }));
ok('S3 present', c3.indexOf('S3 = 26') >= 0, c3);
ok('S4 present', c3.indexOf('S4 = 12') >= 0, c3);
ok('HeaderProtectionKey present', c3.indexOf('HeaderProtectionKey = ' + KEY) >= 0, c3);
ok('RandomTrailers present', c3.indexOf('RandomTrailers = on') >= 0, c3);
ok('DisableCookies present', c3.indexOf('DisableCookies = on') >= 0, c3);
ok('I1 present', c3.indexOf('I1 = <b 0xc9f859323c7d><r 11>') >= 0, c3);
ok('I2 present', c3.indexOf('I2 = <b 0x482d><r 5>') >= 0, c3);
ok('H ranges kept verbatim', c3.indexOf('H1 = 1648096627-1648096648') >= 0, c3);
ok('endpoint carries the AWG port', c3.indexOf('Endpoint = 198.51.100.1:30892') >= 0, c3);
ok('DNS line emitted when asked', c3.indexOf('DNS = 1.1.1.1') >= 0, c3);

/* The whole point: the emitted .conf has to parse and validate as one. */
const reparsed = w.parseConf(c3);
const errs = w.validate(reparsed);
eq('generated .conf validates clean', errs.length, 0, JSON.stringify(errs));
eq('round trip keeps S3', reparsed.interface.S3, '26');
eq('round trip keeps S4', reparsed.interface.S4, '12');
eq('round trip keeps the header key', reparsed.interface.HeaderProtectionKey, KEY);
eq('round trip keeps I2', reparsed.interface.I2, '<b 0x482d><r 5>');

/* ---- a v1 profile must not grow fields it never had ---- */
const v1 = profile();
delete v1.s3; delete v1.s4; delete v1.hpKey; v1.cps = null;
const c1 = conf(v1);
ok('v1 conf has no S3', c1.indexOf('S3') < 0, c1);
ok('v1 conf has no S4', c1.indexOf('S4') < 0, c1);
ok('v1 conf has no HeaderProtectionKey', c1.indexOf('HeaderProtectionKey') < 0, c1);
ok('v1 conf has no I1', c1.indexOf('I1') < 0, c1);
ok('v1 conf still validates', w.validate(w.parseConf(c1)).length === 0, c1);
ok('zero S3/S4 are omitted, not written as 0',
   conf(profile({ s3: 0, s4: 0 })).indexOf('S3 = 0') < 0);

/* ---- representation invariant, end to end through server mode ---- */
const ap = profile();
ap.level = 'v3';
ap.port = 30892;
ap.wgListenPort = 40001;
ap.chain = [];
const out = w.buildServerCommands('198.51.100.1', ap, '10.182.242.0/24',
                                  'disk1', 'disk1', 'awg-server-1',
                                  '', '', 'awg', 1, '1.1.1.1');

/* Field -> how it looks in the env, how it looks in the .conf. */
const FIELDS = [
    ['S3', 'AWG_S3 value="26"', 'S3 = 26'],
    ['S4', 'AWG_S4 value="12"', 'S4 = 12'],
    ['HeaderProtectionKey', 'AWG_HEADER_PROTECTION_KEY value="' + KEY + '"',
     'HeaderProtectionKey = ' + KEY]
];
FIELDS.forEach(function (f) {
    const inEnv = out.server.indexOf(f[1]) >= 0;
    const inConf = out.awgConf.indexOf(f[2]) >= 0;
    ok(f[0] + ' is in the container env', inEnv, out.server.slice(0, 200));
    ok(f[0] + ' is in the direct-client .conf', inConf, out.awgConf);
    ok(f[0] + ' agrees between env and .conf', inEnv === inConf);
});

/* CPS templates are generated per client inside buildServerCommands, so compare
 * whatever it settled on rather than the ones we passed in. */
const envI1 = /AWG_I1 value="([^"]+)"/.exec(out.server);
ok('server env carries I1', !!envI1, out.server.slice(0, 300));
if (envI1) ok('the same I1 reaches the client .conf',
              out.awgConf.indexOf('I1 = ' + envI1[1]) >= 0, out.awgConf);

ok('the generated client .conf validates', w.validate(w.parseConf(out.awgConf)).length === 0,
   JSON.stringify(w.validate(w.parseConf(out.awgConf))));

/* ---- a .conf for every client, not only for 'awg' mode (#69) ----
 *
 * A hub whose first client was a MikroTik used to offer no .conf at all, and
 * neither did every client added from its share link. The .conf is only worth
 * offering if it really is the same client slot: its private key has to be the
 * peer the hub gets, and the hub has to route a full tunnel for it. */
function pubOf(privB64) {
    const base = new Uint8Array(32); base[0] = 9;
    return w.uint8ToBase64(w.curve25519ScalarMult(new Uint8Array(Buffer.from(privB64, 'base64')), base));
}
function peerKeyIn(script) {
    const m = /\/interface\/wireguard\/peers\/add [^\n]*public-key="([^"]+)"/.exec(script);
    return m ? m[1] : null;
}
function confPriv(c) {
    const m = /^PrivateKey = (\S+)$/m.exec(c);
    return m ? m[1] : null;
}

['l3', 'none'].forEach(function (mode) {
    const ap2 = profile();
    ap2.level = 'v3'; ap2.port = 30892; ap2.wgListenPort = 40001; ap2.chain = [];
    const lan = mode === 'l3' ? '192.168.1.0/24' : '';
    const cl = mode === 'l3' ? '192.168.2.0/24' : '';
    const r = w.buildServerCommands('198.51.100.1', ap2, '10.182.242.0/24', 'disk1', 'disk1',
                                    'awg-server-1', lan, cl, mode, 1, '');
    ok(mode + ': first client gets a .conf', !!r.awgConf);
    ok(mode + ': that .conf validates', w.validate(w.parseConf(r.awgConf)).length === 0, r.awgConf);
    const priv = confPriv(r.awgConf);
    ok(mode + ': .conf key is the peer the hub gets',
       !!priv && pubOf(priv) === peerKeyIn(r.server), priv);
    ok(mode + ': hub routes a full tunnel for it',
       r.server.indexOf('-ft-nat') >= 0 && r.server.indexOf('-ft-fwd') >= 0);
});

/* The add-client page, opened from a share link made in l3 mode. */
(function () {
    const ap3 = profile();
    ap3.level = 'v3'; ap3.port = 30892; ap3.wgListenPort = 40001; ap3.chain = [];
    const first = w.buildServerCommands('198.51.100.1', ap3, '10.182.242.0/24', 'disk1', 'disk1',
                                        'awg-server-1', '192.168.1.0/24', '192.168.2.0/24', 'l3', 1, '');
    const d2 = new JSDOM(html, { runScripts: 'dangerously', url: first.shareURL });
    const w2 = d2.window;
    ok('share link opens the add-client page', !!w2.serverFromURL);
    if (!w2.serverFromURL) return;
    const r = w2.buildAddClientCommands(w2.serverFromURL, 'disk1', '192.168.3.0/24', 'l3', '');
    ok('added client gets a .conf', !!r.awgConf);
    ok('added client .conf validates', w2.validate(w2.parseConf(r.awgConf)).length === 0, r.awgConf);
    const priv = confPriv(r.awgConf);
    ok('added client .conf key is the peer the hub update adds',
       !!priv && pubOf(priv) === peerKeyIn(r.serverUpdate), priv);
    ok('added client .conf points at the same hub',
       r.awgConf.indexOf('Endpoint = 198.51.100.1:30892') >= 0, r.awgConf);
    ok('added client .conf takes the next tunnel address',
       r.awgConf.indexOf('Address = 10.182.242.3/32') >= 0, r.awgConf);

    /* And the button that hands it out is actually there. */
    w2.renderClientPanel(r, 'l3', 2);
    const btn = w2.document.getElementById('srv-download-conf');
    const hint = w2.document.getElementById('srv-download-conf-hint');
    ok('l3: download button is shown', btn.style.display !== 'none');
    eq('l3: button says it is the phone/PC alternative', btn.getAttribute('data-i18n'), 'btnDownloadConfAlt');
    ok('l3: hint about the shared key is shown', hint.style.display !== 'none' && hint.textContent.length > 0);
    eq('l3: the file offered is this client', w2.lastAwgConf, r.awgConf);
    eq('l3: file is named after the client', w2.lastAwgConfName, 'awg-client-2.conf');
    ok('l3: the panel still shows the MikroTik script',
       w2.document.getElementById('srv-out-client').dataset.plain === r.client);

    w2.renderClientPanel(r, 'awg', 2);
    ok('awg: download button is shown', btn.style.display !== 'none');
    eq('awg: plain label', btn.getAttribute('data-i18n'), 'btnDownloadConf');
    eq('awg: no shared-key hint (there is no script)', hint.style.display, 'none');
})();

/* ---- the serializer itself ---- */
const lines = w.awgProfileConfLines(profile());
ok('serializer emits Jc first', lines[0].indexOf('Jc = ') === 0, lines.join(' | '));
ok('serializer puts S3/S4 before H1',
   lines.indexOf('S3 = 26') < lines.findIndex(function (l) { return l.indexOf('H1 = ') === 0; }));

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
