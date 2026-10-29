/* "More server ports" of a MikroTik hub, driven through jsdom.
 *
 * Port rotation in the proxy only works when AWG_REMOTE names several ports.
 * For a hub that means the hub's own firewall has to forward the whole band
 * to the one port its container listens on, and every client — the first one
 * and each one added later from the share link — has to get that band.
 *
 * Dev-only: npm install jsdom && node tests/conf3.0-hubband.test.js
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

const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/conf3.0.html' });
const w = dom.window;
const doc = w.document;

console.log('=== conf3.0 hub port band ===');

const BAND = '30000-30999';
function params() {
    const ap = w.generateChainParams();
    ap.port = 27015;
    ap.wgListenPort = 40001;
    return ap;
}
// The rule itself, not the uninstall line that removes it by the same comment.
function lineWith(script, needle) {
    return script.split(NL).filter(function (l) {
        return l.indexOf(needle) >= 0 && l.indexOf('remove') < 0 && l.indexOf('dst-port=') >= 0;
    });
}
function remoteOf(script) {
    const m = /key=AWG_REMOTE value="([^"]+)"/.exec(script);
    return m ? m[1] : null;
}

/* ---- the hub ---- */
const ap = params();
ap.band = BAND;
const r = w.buildServerCommands('198.51.100.1', ap, '10.10.10.0/24', 'disk1', 'disk1',
                                'awg-server-1', '', '', 'none', 1, '');
const dnat = lineWith(r.server, 'comment=awg-server-1-dstnat')[0] || '';
ok('dst-nat takes the main port and the band', dnat.indexOf('dst-port=27015,' + BAND + ' ') >= 0, dnat);
ok('and sends them all to the one container port', dnat.indexOf('to-ports=27015 ') >= 0, dnat);
const fwd = lineWith(r.server, 'awg-server-1-awg-in')[0] || '';
ok('the forward rule lets the band in', fwd.indexOf('dst-port=27015,' + BAND + ' ') >= 0, fwd);
ok('the container still listens on the main port only',
   r.server.indexOf('key=AWG_LISTEN value=":27015"') >= 0);

/* ---- its first client ---- */
eq('the client dials the band', remoteOf(r.client), '198.51.100.1:27015,' + BAND);
ok('a direct client .conf keeps a plain Endpoint', /^Endpoint = 198\.51\.100\.1:27015$/m.test(r.awgConf), r.awgConf);
ok('and the band as a comment', r.awgConf.indexOf('# AllowedPorts = 27015,' + BAND) >= 0, r.awgConf);
ok('which this page reads back', w.parseConf(r.awgConf).allowedPorts === '27015,' + BAND);
ok('and the .conf still validates', w.validate(w.parseConf(r.awgConf)).length === 0);

/* ---- a client added later from the share link ---- */
const d2 = new JSDOM(html, { runScripts: 'dangerously', url: r.shareURL });
const w2 = d2.window;
ok('the link opens the add-client page', !!w2.serverFromURL);
if (w2.serverFromURL) {
    eq('the link carries the band', w2.serverFromURL.awgBand, BAND);
    ok('the field shows it and is not for editing',
       w2.document.getElementById('srv-awg-band').value === BAND &&
       w2.document.getElementById('srv-awg-band').readOnly);
    const a = w2.buildAddClientCommands(w2.serverFromURL, 'disk1', '', 'none', '');
    eq('the added client dials the band too', remoteOf(a.client), '198.51.100.1:27015,' + BAND);
    ok('its .conf has the band comment', a.awgConf.indexOf('# AllowedPorts = 27015,' + BAND) >= 0);
    const d3 = new JSDOM(html, { runScripts: 'dangerously', url: a.shareURL });
    eq('and the next link still carries it', d3.window.serverFromURL && d3.window.serverFromURL.awgBand, BAND);
}

/* ---- no band: exactly what it was ---- */
const plain = w.buildServerCommands('198.51.100.1', params(), '10.10.10.0/24', 'disk1', 'disk1',
                                    'awg-server-1', '', '', 'none', 1, '');
ok('no band: dst-nat on the main port only',
   (lineWith(plain.server, 'comment=awg-server-1-dstnat')[0] || '').indexOf('dst-port=27015 ') >= 0);
eq('no band: AWG_REMOTE is one port', remoteOf(plain.client), '198.51.100.1:27015');
ok('no band: no comment in the .conf', plain.awgConf.indexOf('AllowedPorts') < 0);
const d4 = new JSDOM(html, { runScripts: 'dangerously', url: plain.shareURL });
eq('a link without a band reads as none', d4.window.serverFromURL && d4.window.serverFromURL.awgBand, '');

/* ---- an IPv6 hub forwards the band on IPv6 too ---- */
(function () {
    const box = doc.getElementById('ipv6-enable') || doc.getElementById('ipv6');
    if (!box) { ok('IPv6 switch found', false); return; }
    box.checked = true;
    const ap6 = params(); ap6.band = BAND;
    const r6 = w.buildServerCommands('2001:db8::1', ap6, '10.10.10.0/24', 'disk1', 'disk1',
                                     'awg-server-1', '', '', 'none', 1, '');
    box.checked = false;
    const d6 = lineWith(r6.server, 'awg-server-1-dstnat6')[0] || '';
    ok('IPv6 dst-nat takes the band', d6.indexOf('dst-port=27015,' + BAND + ' ') >= 0, d6);
    eq('the IPv6 client dials the band', remoteOf(r6.client), '[2001:db8::1]:27015,' + BAND);
})();

/* ---- the form refuses a band the proxy cannot read ---- */
function generateWith(band) {
    doc.querySelector('input[name="deploy-mode"][value="server"]').checked = true;
    w.onModeChange();
    doc.querySelector('input[name="srv-net"][value="none"]').checked = true;
    w.onSrvNetChange();
    doc.getElementById('srv-endpoint').value = '198.51.100.1';
    doc.getElementById('srv-awg-port').value = '27015';
    doc.getElementById('srv-awg-band').value = band;
    doc.getElementById('errors-container').innerHTML = '';
    w.generateServer();
    return { err: doc.getElementById('errors-container').textContent.trim(),
             client: doc.getElementById('srv-out-client').dataset.plain || '' };
}
let g = generateWith('30000 - 30999');
ok('spaces are refused', g.err.length > 0 && doc.getElementById('srv-awg-band').classList.contains('field-invalid'), g.err);
g = generateWith('1,2,3,4,5,6,7,8,9');
ok('more than 8 pieces are refused', g.err.length > 0, g.err);
g = generateWith(BAND);
ok('a good band goes through', !g.err, g.err);
ok('and reaches the client', g.client.indexOf('198.51.100.1:27015,' + BAND) >= 0);
ok('every new line is translated', ['srvAwgBand', 'srvAwgBandHint', 'errSrvBand']
   .every(function (k) { return w.I18N.en[k] && w.I18N.ru[k] && w.I18N.en[k] !== w.I18N.ru[k]; }));

function eq(name, a, b) { ok(name, a === b, JSON.stringify(a) + ' !== ' + JSON.stringify(b)); }

console.log(NL + passes + '/' + (passes + fails) + ' checks passed' + (fails ? ', ' + fails + ' FAILED' : ''));
process.exit(fails ? 1 : 0);
