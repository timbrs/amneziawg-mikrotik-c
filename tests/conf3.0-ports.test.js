/* Port bands in AWG_REMOTE, driven through jsdom.
 *
 * A server that redirects a whole band of UDP ports to its AmneziaWG port lets
 * the client draw a fresh one per connection. The band travels in the .conf as
 * a comment ("# AllowedPorts = ..."), so the same file still imports into any
 * WireGuard client; the configurator is what turns it into AWG_REMOTE.
 *
 * Dev-only: the proxy itself has no dependencies, and this needs one.
 *   npm install jsdom && node tests/conf3.0-ports.test.js
 * Exits non-zero if anything is broken.
 *
 * Lives outside docs/ on purpose: the Pages workflow publishes that whole
 * directory, and test code has no business on the public site.
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

const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' });
const w = dom.window;

const need = ['parseConf', 'isValidPortSpec', 'endpointWithPorts', 'remoteEndpoint',
              'validate', 'generate', 'clearAll', 'getTunnelNetwork',
              'buildStandaloneCommands'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 port-band tests ===');

/* ---- helpers ---- */
const BAND = '20150-20299,21500-21649,23450-23599';

function conf(endpoint, portsComment) {
    const lines = [
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
        'Endpoint = ' + endpoint
    ];
    // Exactly where the bot writes it: straight after Endpoint.
    if (portsComment !== null) lines.push('# AllowedPorts = ' + portsComment);
    lines.push('AllowedIPs = 0.0.0.0/0');
    return lines.join(NL);
}

// Paste as a user would: the input handler is part of what is under test.
function paste(text) {
    const ta = w.document.getElementById('conf-input');
    ta.value = text;
    ta.dispatchEvent(new w.Event('input', { bubbles: true }));
}
function generateWith(endpoint, portsComment) {
    w.clearAll();
    paste(conf(endpoint, portsComment));
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    const err = w.document.getElementById('errors-container').textContent.trim();
    if (err) throw new Error('generate() reported: ' + err);
    return w.document.getElementById('output').dataset.plain || '';
}
function remoteEnv(script) {
    const head = '/container/envs/add list=awg-proxy-1-env key=AWG_REMOTE value="';
    const lines = script.split(NL);
    for (let i = 0; i < lines.length; i++) {
        const at = lines[i].indexOf(head);
        if (at >= 0) return lines[i].slice(at + head.length).replace(/"$/, '');
    }
    return null;
}

/* ---- the grammar, same one the proxy parses ---- */
ok('single port', w.isValidPortSpec('51820'));
ok('list', w.isValidPortSpec('443,8080'));
ok('range', w.isValidPortSpec('20150-20299'));
ok('mixed', w.isValidPortSpec(BAND + ',443'));
ok('port 0 rejected', !w.isValidPortSpec('0'));
ok('above 65535 rejected', !w.isValidPortSpec('70000'));
ok('reversed range rejected', !w.isValidPortSpec('6800-6085'));
ok('trailing comma rejected', !w.isValidPortSpec('443,'));
ok('leading comma rejected', !w.isValidPortSpec(',443'));
ok('empty token rejected', !w.isValidPortSpec('443,,8080'));
ok('letters rejected', !w.isValidPortSpec('a-b'));
ok('spaces rejected', !w.isValidPortSpec('443, 8080'));
ok('empty spec rejected', !w.isValidPortSpec(''));
ok('33 tokens rejected',
   !w.isValidPortSpec(Array.from({ length: 33 }, (_, i) => 20000 + i).join(',')));
ok('32 tokens accepted',
   w.isValidPortSpec(Array.from({ length: 32 }, (_, i) => 20000 + i).join(',')));

/* ---- parsing the comment ---- */
eq('the comment is picked up', w.parseConf(conf('198.51.100.1:443', BAND)).allowedPorts, BAND);
eq('no comment, no band', w.parseConf(conf('198.51.100.1:443', null)).allowedPorts, undefined);
eq('a ; comment counts too',
   w.parseConf(conf('198.51.100.1:443', null).replace('AllowedIPs', '; AllowedPorts = 443,8080' + NL + 'AllowedIPs')).allowedPorts,
   '443,8080');
eq('the comment never becomes a peer field',
   w.parseConf(conf('198.51.100.1:443', BAND)).peer.AllowedPorts, undefined);
eq('Endpoint itself is left alone',
   w.parseConf(conf('198.51.100.1:443', BAND)).peer.Endpoint, '198.51.100.1:443');

/* ---- endpoint rewriting ---- */
eq('ipv4 endpoint takes the band',
   w.endpointWithPorts('198.51.100.1:443', BAND), '198.51.100.1:' + BAND);
eq('hostname endpoint takes the band',
   w.endpointWithPorts('vpn.example.com:443', '443,8080'), 'vpn.example.com:443,8080');
eq('ipv6 endpoint keeps its brackets',
   w.endpointWithPorts('[2001:db8::1]:443', '6000-6100'), '[2001:db8::1]:6000-6100');

/* ---- the generated script ---- */
eq('band reaches AWG_REMOTE', remoteEnv(generateWith('198.51.100.1:443', BAND)),
   '198.51.100.1:' + BAND);
eq('no comment: AWG_REMOTE is what it always was',
   remoteEnv(generateWith('198.51.100.1:443', null)), '198.51.100.1:443');
eq('a garbage band is ignored, not passed on',
   remoteEnv(generateWith('198.51.100.1:443', '6800-6085')), '198.51.100.1:443');
eq('an ipv6 endpoint keeps its brackets in AWG_REMOTE',
   remoteEnv(generateWith('[2001:db8::1]:443', '6000-6100')), '[2001:db8::1]:6000-6100');

// AWG_REMOTE is the only place a list belongs. Everything else — routes, the
// AAAA probe, the address-exclusion rules — reads Endpoint, and a list there
// would be a syntax error on the router.
const script = generateWith('198.51.100.1:443', BAND);
const banded = script.split(NL).filter(l => l.indexOf(BAND) >= 0);
eq('the band appears on exactly one line', banded.length, 1);
ok('and that line is AWG_REMOTE', banded[0].indexOf('key=AWG_REMOTE') >= 0, banded[0]);
const routing = w.document.getElementById('routing-output').dataset.plain || '';
ok('the routing block carries no port list at all', routing.indexOf(BAND) < 0);

/* ---- standalone mode gets the same AWG_REMOTE ---- */
w.clearAll();
const parsed = w.parseConf(conf('198.51.100.1:443', BAND));
w.validate(parsed);
const standalone = w.buildStandaloneCommands(parsed, '192.168.88.5', 'awg-proxy-1',
                                             w.getTunnelNetwork('awg-proxy-1'), 'tunnel');
ok('standalone exports the band too',
   standalone.indexOf('export AWG_REMOTE=\\"198.51.100.1:' + BAND + '\\"') >= 0,
   standalone.split(NL).filter(l => l.indexOf('AWG_REMOTE') >= 0).join(' | '));

/* ---- the "server takes several ports" box ---------------------------------
 * The band can come from the .conf comment or be typed in. A comment ticks the
 * box and fills it in by itself; what the person typed is never overwritten by
 * a later paste; unticking means no band, whatever the .conf says. */
const doc = w.document;
const box = doc.getElementById('ports-enable');
const field = doc.getElementById('ports-spec');
const panel = doc.getElementById('ports-panel');
function generateNow() {
    doc.getElementById('errors-container').innerHTML = '';
    doc.getElementById('output').dataset.plain = '';
    w.generate();
    return { out: doc.getElementById('output').dataset.plain || '',
             err: doc.getElementById('errors-container').textContent.trim() };
}

w.clearAll();
ok('starts unticked and folded', !box.checked && panel.style.display === 'none');
paste(conf('198.51.100.1:443', BAND));
ok('a .conf band ticks the box', box.checked && panel.style.display === 'block');
eq('and fills it in', field.value, BAND);
paste(conf('198.51.100.1:443', null));
ok('a .conf without one takes it back', !box.checked && field.value === '');

w.clearAll();
paste(conf('198.51.100.1:443', null));
box.checked = true; w.onPortsToggle();
field.value = '30000-30999'; w.portsTyped();
eq('a typed band reaches AWG_REMOTE', remoteEnv(generateNow().out), '198.51.100.1:30000-30999');
paste(conf('198.51.100.1:443', BAND));
eq('a later paste leaves the typed band alone', field.value, '30000-30999');
eq('and it still wins', remoteEnv(generateNow().out), '198.51.100.1:30000-30999');

box.checked = false; w.onPortsToggle();
eq('unticked: no band, even with one in the .conf', remoteEnv(generateNow().out), '198.51.100.1:443');

box.checked = true; w.onPortsToggle();
field.value = '30000 - 30999'; w.portsTyped();
const bad = generateNow();
ok('a band the proxy cannot read is refused', bad.err.length > 0 && bad.out === '', bad.err);
ok('and the field is flagged', field.classList.contains('field-invalid') &&
   doc.getElementById('ports-spec-error').style.display !== 'none');
field.value = ''; w.portsTyped();
ok('ticked and empty is refused too', generateNow().err.length > 0);

// With an AmneziaWG config pasted, the WARP mode would switch itself back.
doc.getElementById('conf-input').value = '';
function shownIn(mode) {
    doc.querySelector('input[name="deploy-mode"][value="' + mode + '"]').checked = true;
    w.onModeChange();
    return doc.getElementById('ports-row').style.display !== 'none';
}
ok('offered for a container install', shownIn('container'));
ok('and for a proxy on a device of its own', shownIn('standalone'));
ok('not in WARP mode: Cloudflare has its own list', !shownIn('warp'));
ok('not for site-to-site', !shownIn('site-to-site'));
ok('not for the hub', !shownIn('server'));
shownIn('container');

const cmd = panel.querySelector('pre').textContent;
const m = /raw\.githubusercontent\.com\/timbrs\/amneziawg-mikrotik-c\/main\/(\S+) \| sudo bash$/.exec(cmd);
ok('the server command fetches a script that exists here', !!m && fs.existsSync(path.join(__dirname, '..', m[1])), cmd);
ok('every line is translated', ['portsLabel', 'portsHint', 'portsHowTo', 'portsMore', 'errPortsInvalid']
   .every(function (k) { return w.I18N.en[k] && w.I18N.ru[k] && w.I18N.en[k] !== w.I18N.ru[k]; }));

console.log(NL + passes + '/' + (passes + fails) + ' checks passed' + (fails ? ', ' + fails + ' FAILED' : ''));
process.exit(fails ? 1 : 0);
