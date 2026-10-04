/* DNS-forwarder selection in the configurator, driven through jsdom.
 *
 * Dev-only: the proxy itself has no dependencies, and this needs one.
 *   npm install jsdom && node tests/conf3.0-dns.test.js
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

const need = ['autoSelectDoh', 'detectDohFromDns', 'dnsServersForScript', 'dnsIpsForForwarder',
              'getSelectedDohForwarder', 'isPublicIPv4', 'generate', 'clearAll', 'onScenarioChange',
              'parseConf', 'validate', 'getTunnelNetwork', 'buildStandaloneCommands'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 DNS-forwarder tests ===');

/* ---- helpers ---- */
function dohRadio(value) {
    const radios = w.document.getElementsByName('doh-fwd');
    for (let i = 0; i < radios.length; i++) if (radios[i].value === value) return radios[i];
    throw new Error('no doh radio ' + value);
}
// A user click, as the page sees it: the radio goes checked and change bubbles.
function pick(value) {
    const r = dohRadio(value);
    r.checked = true;
    r.dispatchEvent(new w.Event('change', { bubbles: true }));
}
function setScenario(value) {
    const radios = w.document.getElementsByName('scenario');
    for (let i = 0; i < radios.length; i++) radios[i].checked = radios[i].value === value;
    w.onScenarioChange();
}
// Back to a fresh page: nothing auto-detected, nothing picked by hand.
function freshForm() {
    w.clearAll();
    setScenario('dns-fwd');
}
function conf(dnsLine) {
    return [
        '[Interface]',
        'PrivateKey = ' + 'A'.repeat(43) + '=',
        'Address = 10.13.13.2/32',
        dnsLine,
        'Jc = 4', 'Jmin = 40', 'Jmax = 70',
        'S1 = 30', 'S2 = 40',
        'H1 = 1111111111', 'H2 = 2222222222', 'H3 = 3333333333', 'H4 = 444444444',
        '',
        '[Peer]',
        'PublicKey = ' + 'B'.repeat(43) + '=',
        'Endpoint = 198.51.100.1:443',
        'AllowedIPs = 0.0.0.0/0'
    ].join(NL);
}
// Paste as a user would: the input handler is part of what is under test.
function paste(dnsLine) {
    const ta = w.document.getElementById('conf-input');
    ta.value = conf(dnsLine);
    ta.dispatchEvent(new w.Event('input', { bubbles: true }));
}
function generateNow() {
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    const err = w.document.getElementById('errors-container').textContent.trim();
    if (err) throw new Error('generate() reported: ' + err);
    return {
        main: w.document.getElementById('output').dataset.plain || '',
        routing: w.document.getElementById('routing-output').dataset.plain || ''
    };
}
function generateWith(dnsLine) { paste(dnsLine); return generateNow(); }
function serversLine(script) {
    const head = '/ip/dns/set servers=';
    const lines = script.split(NL);
    for (let i = 0; i < lines.length; i++) {
        if (lines[i].indexOf(head) === 0) return lines[i].slice(head.length);
    }
    return null;
}
function lineWith(script, needle) {
    const lines = script.split(NL);
    for (let i = 0; i < lines.length; i++) if (lines[i].indexOf(needle) >= 0) return lines[i];
    return '';
}

/* ---- auto-detection still works ---- */
freshForm();
w.autoSelectDoh('1.1.1.1');
eq('1.1.1.1 ticks CloudFlare', w.getSelectedDohForwarder(), 'CloudFlare');
freshForm();
w.autoSelectDoh('9.9.9.9, 149.112.112.112');
eq('9.9.9.9 ticks Quad9', w.getSelectedDohForwarder(), 'Quad9');
freshForm();
w.autoSelectDoh('10.8.0.1');
eq('an unknown resolver leaves the default alone', w.getSelectedDohForwarder(), 'GoogleDNS');

// Pasting the .conf is enough - no need to hit Generate to see the provider.
freshForm();
paste('DNS = 77.88.8.8');
eq('pasting a .conf ticks the provider right away', w.getSelectedDohForwarder(), 'Yandex');

/* ---- a hand-picked provider is never overwritten ---- */
freshForm();
w.autoSelectDoh('1.1.1.1');
pick('GoogleDNS');
w.autoSelectDoh('1.1.1.1');
eq('auto-detect no longer moves a hand-picked radio', w.getSelectedDohForwarder(), 'GoogleDNS');

freshForm();
w.autoSelectDoh('1.1.1.1');
eq('clearAll brings auto-detect back', w.getSelectedDohForwarder(), 'CloudFlare');

/* ---- the generated script follows the picked provider ---- */
freshForm();
const asIs = generateWith('DNS = 1.1.1.1');
eq('untouched: servers come from the .conf', serversLine(asIs.main), '1.1.1.1');
ok('untouched: CloudFlare DoH forwarder',
   asIs.routing.indexOf('doh-servers=https://cloudflare-dns.com/dns-query name=CloudFlare') >= 0);

freshForm();
paste('DNS = 1.1.1.1');
pick('GoogleDNS');
const picked = generateNow();
eq('picked Google: /ip/dns/set follows the pick', serversLine(picked.main), '8.8.8.8,8.8.4.4');
ok('picked Google: the swap is spelled out in the script',
   picked.main.indexOf('# NOTE: servers follow the DNS-forwarder picked in the configurator') >= 0);
// Раньше сюда ставился /32-маршрут через wg-интерфейс. Такой маршрут переживает
// смерть туннеля — WG-интерфейс не уходит в down — и резолв упирается в дохлый
// туннель, унося с собой всё, что работало бы напрямую. Теперь DNS идёт общим
// путём: политикой в активный туннель, а при падении всех — по default.
// Ищем именно исполняемые строки: в NEXT STEPS такой маршрут показан внутри :put
// как пример ручной команды, и он к делу не относится.
const realRoutes = picked.main.split(NL)
    .filter(l => l.indexOf(':put') < 0 && /^\s*\/ip\/route\/add /.test(l));
ok('picked Google: DNS не прибит к туннелю /32-маршрутом',
   !realRoutes.some(l => /dst-address=(8\.8\.8\.8|8\.8\.4\.4)\/32.*gateway=wg-/.test(l)),
   realRoutes.join(' | ').slice(0, 160));
ok('picked Google: и это объяснено в самом скрипте',
   picked.main.indexOf('идёт общим маршрутом') >= 0);
ok('picked Google: nothing points at the .conf resolver any more',
   picked.main.indexOf('1.1.1.1') < 0, lineWith(picked.main, '1.1.1.1'));
ok('picked Google: DoH forwarder is Google too',
   picked.routing.indexOf('doh-servers=https://dns.google/dns-query name=GoogleDNS') >= 0);
ok('picked Google: nothing CloudFlare survives in the routing block',
   picked.routing.toLowerCase().indexOf('cloudflare') < 0,
   lineWith(picked.routing.toLowerCase(), 'cloudflare'));
ok('picked Google: FWD entries forward to Google',
   picked.routing.indexOf('forward-to=GoogleDNS') >= 0 &&
   picked.routing.indexOf('forward-to=CloudFlare') < 0);

/* ---- what must NOT be swapped ---- */
freshForm();
paste('DNS = 10.8.0.1');
pick('GoogleDNS');
eq('a private resolver from the .conf stays put', serversLine(generateNow().main), '10.8.0.1');

freshForm();
paste('DNS = 1.1.1.1, 10.8.0.1');
pick('GoogleDNS');
eq('only the public half is swapped', serversLine(generateNow().main), '8.8.8.8,8.8.4.4,10.8.0.1');

freshForm();
setScenario('tunnel');
paste('DNS = 1.1.1.1');
pick('GoogleDNS');
eq('tunnel-only scenario has no DoH panel, so no swap', serversLine(generateNow().main), '1.1.1.1');
setScenario('dns-fwd');

/* ---- custom resolver ---- */
freshForm();
pick('__custom');
w.document.getElementById('doh-custom-url').value = 'https://doh.example.com/dns-query';
eq('a custom DoH URL gives no plain IP to set',
   w.dnsServersForScript('1.1.1.1', 'dns-fwd').join(','), '1.1.1.1');
w.document.getElementById('doh-custom-url').value = '208.67.222.222';
eq('a custom public IP is used as the resolver',
   w.dnsServersForScript('1.1.1.1', 'dns-fwd').join(','), '208.67.222.222');
w.document.getElementById('doh-custom-url').value = '192.168.88.1';
eq('a custom LAN resolver is not pinned into the tunnel',
   w.dnsServersForScript('1.1.1.1', 'dns-fwd').join(','), '1.1.1.1');

ok('isPublicIPv4 rejects the private ranges',
   !w.isPublicIPv4('10.0.0.1') && !w.isPublicIPv4('172.16.0.1') && !w.isPublicIPv4('172.31.255.254') &&
   !w.isPublicIPv4('192.168.1.1') && !w.isPublicIPv4('127.0.0.1') && !w.isPublicIPv4('169.254.1.1') &&
   !w.isPublicIPv4('100.64.0.1') && !w.isPublicIPv4('224.0.0.1') && !w.isPublicIPv4('0.0.0.0'));
ok('isPublicIPv4 accepts public ones',
   w.isPublicIPv4('8.8.8.8') && w.isPublicIPv4('1.1.1.1') && w.isPublicIPv4('172.32.0.1') &&
   w.isPublicIPv4('192.169.0.1') && w.isPublicIPv4('208.67.222.222'));

/* ---- standalone mode gets the same treatment ---- */
freshForm();
pick('Quad9');
const parsed = w.parseConf(conf('DNS = 1.1.1.1'));
w.validate(parsed);
const standalone = w.buildStandaloneCommands(parsed, '192.168.88.5', 'awg-proxy-1',
                                             w.getTunnelNetwork('awg-proxy-1'), 'dns-fwd');
eq('standalone follows the pick too', serversLine(standalone), '9.9.9.9,149.112.112.112');
const realRoutesSa = standalone.split(NL)
    .filter(l => l.indexOf(':put') < 0 && /^\s*\/ip\/route\/add /.test(l));
ok('standalone тоже не прибивает DNS к туннелю',
   !realRoutesSa.some(l => /dst-address=9\.9\.9\.9\/32.*gateway=wg-/.test(l)),
   realRoutesSa.join(' | ').slice(0, 160));
// Смысл выбора DNS при этом сохраняется: сервера подставлены в /ip/dns/set.
ok('но выбранные сервера по-прежнему прописаны',
   serversLine(standalone) === '9.9.9.9,149.112.112.112');


/* ---- AWG_DNS: the container's own resolver (#54) ------------------------
 * The image is scratch, so without AWG_DNS there is no /etc/resolv.conf and a
 * hostname endpoint never resolves. Server and site-to-site modes already
 * demanded this; the normal mode silently did not. */

function confWith(endpoint) {
    return [
        '[Interface]',
        'PrivateKey = ' + 'A'.repeat(43) + '=',
        'Address = 10.13.13.2/32',
        'Jc = 4', 'Jmin = 40', 'Jmax = 70',
        'S1 = 30', 'S2 = 40',
        'H1 = 1111111111', 'H2 = 2222222222', 'H3 = 3333333333', 'H4 = 444444444',
        '',
        '[Peer]',
        'PublicKey = ' + 'B'.repeat(43) + '=',
        'Endpoint = ' + endpoint,
        'AllowedIPs = 0.0.0.0/0'
    ].join(NL);
}

function runGenerate(endpoint, dns) {
    w.document.getElementById('awg-dns').value = dns;
    w.document.getElementById('conf-input').value = confWith(endpoint);
    w.document.getElementById('errors-container').innerHTML = '';
    w.document.getElementById('output').dataset.plain = '';
    w.generate();
    return {
        err: w.document.getElementById('errors-container').textContent.trim(),
        out: w.document.getElementById('output').dataset.plain || ''
    };
}

const dnsField = w.document.getElementById('awg-dns');
ok('normal mode has a container DNS field', !!dnsField);

const missingDns = runGenerate('vpn.example.com:443', '');
ok('a hostname endpoint without AWG_DNS is refused', missingDns.err.length > 0, missingDns.err);
ok('and nothing is generated', missingDns.out.length === 0);

const withDns = runGenerate('vpn.example.com:443', '192.168.88.1');
ok('a hostname endpoint with AWG_DNS generates', withDns.err.length === 0, withDns.err);
ok('AWG_DNS reaches the container env',
   withDns.out.indexOf('key=AWG_DNS value="192.168.88.1"') >= 0,
   withDns.out.split(NL).filter(function (l) { return l.indexOf('AWG_') >= 0; }).join(' | ').slice(0, 200));

const literal = runGenerate('198.51.100.1:443', '');
ok('an IP endpoint needs no AWG_DNS', literal.err.length === 0, literal.err);
ok('and none is written', literal.out.indexOf('AWG_DNS') < 0);

const literalWithDns = runGenerate('198.51.100.1:443', '192.168.88.1');
ok('an explicit AWG_DNS is honoured even for an IP endpoint',
   literalWithDns.out.indexOf('key=AWG_DNS value="192.168.88.1"') >= 0);

w.document.getElementById('awg-dns').value = '';

/* ---- the field that blocks Generate is pointed at, not just named -------- */
const dnsErr = w.document.getElementById('awg-dns-error');
const refused = runGenerate('vpn.example.com:443', '');
ok('a refused DNS field is marked red', dnsField.classList.contains('field-invalid'));
ok('the error is repeated right under the field',
   dnsErr && dnsErr.style.display !== 'none' && dnsErr.textContent === refused.err, dnsErr && dnsErr.textContent);
ok('and the cursor is put in it', w.document.activeElement === dnsField);
dnsField.value = '1';
dnsField.dispatchEvent(new w.Event('input', { bubbles: true }));
ok('typing into it clears the mark', !dnsField.classList.contains('field-invalid') && dnsErr.style.display === 'none');
dnsField.value = '';
runGenerate('vpn.example.com:443', '');
runGenerate('198.51.100.1:443', '');
ok('a later successful run clears the mark too', !dnsField.classList.contains('field-invalid'));

/* The row sits right under the config, not a dozen options down. */
const dnsRow = w.document.getElementById('dns-row');
const scenarioRow = w.document.getElementById('scenario-row');
ok('the DNS row comes before the routing options',
   !!(dnsRow.compareDocumentPosition(scenarioRow) & w.Node.DOCUMENT_POSITION_FOLLOWING));
ok('and right after the config box',
   !!(w.document.getElementById('conf-input').compareDocumentPosition(dnsRow) & w.Node.DOCUMENT_POSITION_FOLLOWING));

/* A public resolver in the config is reachable without the tunnel: taken as is. */
function runWithConfDns(endpoint, confDns) {
    w.document.getElementById('awg-dns').value = '';
    w.document.getElementById('conf-input').value =
        confWith(endpoint).replace('Address = 10.13.13.2/32', 'Address = 10.13.13.2/32' + NL + 'DNS = ' + confDns);
    w.document.getElementById('errors-container').innerHTML = '';
    w.document.getElementById('output').dataset.plain = '';
    w.generate();
    return {
        err: w.document.getElementById('errors-container').querySelector('.alert-error') ? 'error' : '',
        note: w.document.getElementById('errors-container').textContent.trim(),
        out: w.document.getElementById('output').dataset.plain || '',
        field: w.document.getElementById('awg-dns').value
    };
}
const pub = runWithConfDns('vpn.example.com:443', '1.1.1.1, 2606:4700:4700::1111');
eq('a public DNS in the config is taken for the container', pub.field, '1.1.1.1');
ok('and generation goes through', pub.err === '' && pub.out.indexOf('key=AWG_DNS value="1.1.1.1"') >= 0, pub.note);
ok('the page says it filled the field in', /1\.1\.1\.1/.test(pub.note), pub.note);
const priv = runWithConfDns('vpn.example.com:443', '10.8.0.1');
ok('a private DNS from the config is not taken - it lives inside the tunnel',
   priv.err === 'error' && priv.field === '' && priv.out === '');
const cgnat = runWithConfDns('vpn.example.com:443', '100.64.0.1, 8.8.8.8');
eq('the first public one wins over a CGNAT address before it', cgnat.field, '8.8.8.8');
w.document.getElementById('awg-dns').value = '';

console.log(NL + passes + '/' + (passes + fails) + ' checks passed' + (fails ? ', ' + fails + ' FAILED' : ''));
process.exit(fails ? 1 : 0);
