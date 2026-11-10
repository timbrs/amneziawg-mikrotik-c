/* tools/awg-gen.js prints what the page prints.
 *
 * The CLI fills the configurator form for the user. The invariant: for the
 * same input, every part it prints is byte for byte what the page shows after
 * a person picks the same things by hand. Both sides run with the same frozen
 * clock and the same seeded randomness, so keys, ports and the date line
 * match too. Then the command line itself: files, stdin, help, exit codes,
 * and that the page runs with the network cut off.
 *
 * Dev-only:
 *   npm install jsdom && node tests/awg-gen.test.js
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { JSDOM } = require('jsdom');
const gen = require('../tools/awg-gen.js');

const NL = String.fromCharCode(10);
const html = fs.readFileSync(path.join(__dirname, '..', 'docs', 'conf3.0.html'), 'utf8');
const TOOL = path.join(__dirname, '..', 'tools', 'awg-gen.js');

let fails = 0, passes = 0;
function ok(name, cond, extra) {
    if (cond) { passes++; console.log('  PASS  ' + name); }
    else { fails++; console.log('  FAIL  ' + name + (extra ? ': ' + extra : '')); }
}
// Site-to-site is hidden on the page and in the CLI; its checks are kept as
// they were, wrapped in skip() and not run. TODO: if nobody asks for the mode
// back by the end of 2026, delete them in 2027 together with it.
const SITE_TO_SITE_HIDDEN = 'site-to-site is hidden in the configurator';
function skip(name, why, checks) { console.log('  SKIP  ' + name + ': ' + why); }
function same(name, a, b) {
    if (a === b) { ok(name, true); return; }
    let i = 0;
    while (i < a.length && a[i] === b[i]) i++;
    ok(name, false, 'differ at ' + i + ': ' + JSON.stringify(a.slice(i, i + 80)) + ' vs ' + JSON.stringify(b.slice(i, i + 80)));
}

// The same clock and the same random stream in every page this test opens.
function freeze(win) {
    let s = 0x2545f491;
    const rnd = function () { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; return (s >>> 0) / 4294967296; };
    win.Math.random = rnd;
    Object.defineProperty(win, 'crypto', { configurable: true, value: { getRandomValues(a) {
        for (let i = 0; i < a.length; i++) a[i] = Math.floor(rnd() * Math.pow(2, 8 * a.BYTES_PER_ELEMENT));
        return a;
    } } });
    const T = Date.UTC(2026, 0, 2, 3, 4, 5);
    const D = win.Date;
    win.Date = class extends D { constructor(...a) { super(...(a.length ? a : [T])); } static now() { return T; } };
}

function offlineGuard(win) {
    for (const name of ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource']) {
        try { win[name] = function () { throw new Error('test tried the network via ' + name); }; } catch (e) { /* ignore */ }
    }
}

// A person at the page: clicks, typing, the Generate button.
function person(url) {
    const dom = new JSDOM(html, { runScripts: 'dangerously', url: url || gen.PAGE_URL,
                                  beforeParse(win) { offlineGuard(win); freeze(win); } });
    const w = dom.window, d = w.document;
    const fire = (el, type) => el.dispatchEvent(new w.Event(type, { bubbles: true }));
    return {
        w: w, d: d,
        click(sel) { d.querySelector(sel).click(); return this; },
        radio(name, v) { return this.click('input[name="' + name + '"][value="' + v + '"]'); },
        box(sel, on) { const el = d.querySelector(sel); if (el.checked !== on) el.click(); return this; },
        type(id, v) { const el = d.getElementById(id); el.value = v; fire(el, 'input'); return this; },
        pick(id, v) { const el = d.getElementById(id); el.value = v; fire(el, 'change'); return this; },
        generate() { return this.click('#btn-generate'); },
        text(id) {
            const el = d.getElementById(id);
            const t = el.tagName === 'INPUT' ? el.value : (el.dataset.plain || '');
            return t.endsWith(NL) ? t : t + NL;
        },
        close() { w.close(); },
    };
}

function cli(args, stdin, hook) {
    let out = '', err = '';
    const code = gen.run(args, { stdin: stdin, out: s => { out += s; }, err: s => { err += s; },
                                 beforeParse: hook || freeze });
    return { code: code, out: out, err: err };
}

const KEY = 'A'.repeat(43) + '=';
const AWG = [
    '[Interface]',
    'PrivateKey = ' + KEY,
    'Address = 10.8.1.2/32',
    'DNS = 1.1.1.1, 1.0.0.1',
    'Jc = 4', 'Jmin = 10', 'Jmax = 50',
    'S1 = 33', 'S2 = 61', 'S3 = 18', 'S4 = 14',
    'H1 = 1033585427-1065342551', 'H2 = 1214585315-1270342551',
    'H3 = 1401585315-1450342551', 'H4 = 1601585315-1650342551',
    '',
    '[Peer]',
    'PublicKey = ' + 'B'.repeat(43) + '=',
    'PresharedKey = ' + 'C'.repeat(43) + '=',
    'AllowedIPs = 0.0.0.0/0, ::/0',
    'Endpoint = 203.0.113.10:51820',
    'PersistentKeepalive = 25',
].join(NL);
const WARP = [
    '[Interface]',
    'PrivateKey = ' + KEY,
    'Address = 172.16.0.2/32, 2606:4700:110:84de:d5b5:abc3:c864:f390/128',
    'DNS = 1.1.1.1, 1.0.0.1',
    'MTU = 1280',
    '',
    '[Peer]',
    'PublicKey = bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo=',
    'AllowedIPs = 0.0.0.0/0, ::/0',
    'Endpoint = engage.cloudflareclient.com:2408',
].join(NL);

const CONF_PARTS = { offline: 'offline-output', install: 'output', routing: 'routing-output', uninstall: 'uninstall-output' };
const S2S_PARTS = { 'side-a': 's2s-out-a', 'uninstall-a': 's2s-uninst-a', 'side-b': 's2s-out-b', 'uninstall-b': 's2s-uninst-b' };
const SRV_PARTS = { link: 'srv-share-url', server: 'srv-out-server', client: 'srv-out-client' };

/* One case: the CLI with `args` against a person doing `byHand`. Every listed
 * part is compared alone (--part) and found, in order, in the joint output. */
function check(name, args, stdin, byHand, parts, url) {
    console.log('--- ' + name);
    const p = person(url);
    byHand(p);
    const all = cli(args, stdin);
    ok(name + ': exit 0', all.code === 0, all.err);
    let at = -1;
    Object.keys(parts).forEach(function (part) {
        const want = p.text(parts[part]);
        ok(name + ': the page has a ' + part + ' part', want.trim().length > 0);
        const one = cli(args.concat(['--part', part]), stdin);
        same(name + ': --part ' + part + ' is what the page shows', one.out, want);
        const pos = all.out.indexOf(part === 'link' ? '# ' + want : want);
        ok(name + ': ' + part + ' is in the joint output, in page order', pos > at, 'at ' + pos + ' after ' + at);
        if (pos > at) at = pos;
    });
    const heads = (all.out.match(/^# >>> \[[a-z-]+\]/gm) || []).map(h => h.slice(7, -1));
    same(name + ': and nothing else', heads.join(','), Object.keys(parts).join(','));
    p.close();
    return all;
}

console.log('=== awg-gen: the CLI prints what the page prints ===');

check('AmneziaWG 2.0, all defaults', [], AWG,
      p => p.type('conf-input', AWG).generate(),
      { install: 'output', routing: 'routing-output', uninstall: 'uninstall-output' });

check('tunnel only, USB, own prefix, offline, English',
      ['--scenario', 'tunnel', '--storage', 'usb1', '--prefix', 'awg-office', '--offline', '--lang', 'en'], AWG,
      p => p.click('#lang-toggle').type('conf-input', AWG).radio('scenario', 'tunnel').pick('storage-select', 'usb1')
            .pick('prefix-select', '__custom').type('prefix-custom', 'awg-office').box('#offline-enable', true).generate(),
      { offline: 'offline-output', install: 'output', uninstall: 'uninstall-output' });

check('all non-RU traffic, exclusions, DoH, PPPoE',
      ['--scenario', 'non-ru', '--exclude', 'steam,epic', '--doh', 'quad9', '--no-netwatch', '--path-mtu', '1492'], AWG,
      p => p.type('conf-input', AWG).radio('scenario', 'non-ru').box('input[name="excl"][value="epic"]', true)
            .radio('doh-fwd', 'Quad9').box('#netwatch-enable', false).type('path-mtu', '1492').generate(),
      { install: 'output', routing: 'routing-output', uninstall: 'uninstall-output' });

check('picked services, own domain, every tuning knob',
      ['--services', 'youtube,telegram', '--domain', 'example.org', '--extra-time', '1h',
       '--no-force-ipv4', '--no-autoupdate', '--clear-df', '--ipv6', '--storage', 'sata1-part1', '--dns', '192.168.88.1'], AWG,
      function (p) {
          p.type('conf-input', AWG).click('button[onclick="toggleAllServices(false)"]')
           .box('input[name="svc"][value="youtube"]', true).box('input[name="svc"][value="telegram"]', true);
          p.d.getElementById('custom-domain-input').value = 'example.org';
          p.click('button[onclick="addCustomDomain()"]')
           .box('#extratime-enable', true).type('extratime-value', '1h').box('#v6block-enable', false)
           .box('#autoupdate-enable', false).box('#df-enable', true).box('#ipv6-enable', true)
           .pick('storage-select', '__custom').type('storage-custom', 'sata1-part1').type('awg-dns', '192.168.88.1').generate();
      },
      { install: 'output', routing: 'routing-output', uninstall: 'uninstall-output' });

const thin = cli(['--scenario', 'tunnel', '-'], AWG);
ok('ACK thinning: only on 64-bit routers with 4+ cores',
   /architecture-name\] = "arm64"\) && \(\[\/system\/resource\/get cpu-count\] >= 4/.test(thin.out));
ok('ACK thinning: the queue comes after the interface it hangs on',
   thin.out.indexOf('/interface/wireguard/add name=wg-awg-proxy-1') < thin.out.indexOf('/queue/tree/add name=awg-proxy-1-ack'));
ok('ACK thinning: the uninstall script takes all of it away',
   /queue\/tree\/remove \[find where comment="awg-proxy-1-ack"\]/.test(thin.out) &&
   /queue\/type\/remove \[find where name="awg-proxy-1-ack"\]/.test(thin.out) &&
   /mangle\/remove \[find where comment="awg-proxy-1-ack"\]/.test(thin.out));
ok('ACK thinning: the queue goes before its type in the uninstall script',
   thin.out.indexOf('queue/tree/remove') < thin.out.indexOf('queue/type/remove'));

check('standalone proxy device', ['--mode', 'standalone', '--proxy-ip', '192.168.88.10', '--scenario', 'tunnel'], AWG,
      p => p.radio('deploy-mode', 'standalone').type('conf-input', AWG).type('proxy-ip', '192.168.88.10')
            .radio('scenario', 'tunnel').generate(),
      { install: 'output', uninstall: 'uninstall-output' });

const warp = check('Cloudflare WARP with the QUIC-shaped packet', ['--mode', 'warp', '--warp-quic'], WARP,
      p => p.radio('deploy-mode', 'warp').type('conf-input', WARP).generate()
            .click('#fields-display button[onclick="addWarpQuicCover()"]').generate(),
      { install: 'output', routing: 'routing-output', uninstall: 'uninstall-output' });
ok('WARP: the packet reaches the container', /key=AWG_I1 value="<b 0xc/.test(warp.out));

check('bare WARP with junk packets', ['--mode', 'warp', '--warp-junk', '--scenario', 'tunnel', '--no-warp-ports'], WARP,
      p => p.radio('deploy-mode', 'warp').type('conf-input', WARP).generate()
            .click('#fields-display button[onclick="addWarpJunk()"]').radio('scenario', 'tunnel')
            .box('#warp-ports-enable', false).generate(),
      { install: 'output', uninstall: 'uninstall-output' });

skip('site-to-site, AWG 2.0 without the fallback chain', SITE_TO_SITE_HIDDEN, function () {
check('site-to-site, AWG 2.0 without the fallback chain',
      ['--mode', 'site-to-site', '--proto', 'v2', '--no-compat', '--endpoint', '198.51.100.1', '--port', '40000',
       '--subnet', '10.9.9.0/30', '--dns', '192.168.88.1', '--lan-a', '192.168.1.0/24', '--lan-b', '192.168.2.0/24',
       '--storage-a', 'usb1'], undefined,
      p => p.radio('deploy-mode', 'site-to-site').pick('s2s-proto-level', 'v2').box('#s2s-proto-compat', false)
            .type('s2s-endpoint-b', '198.51.100.1').type('s2s-awg-port', '40000').type('s2s-tunnel-subnet', '10.9.9.0/30')
            .type('s2s-dns', '192.168.88.1').type('s2s-lan-a', '192.168.1.0/24').type('s2s-lan-b', '192.168.2.0/24')
            .pick('s2s-storage-a', 'usb1').generate(),
      S2S_PARTS);
});

const hub = check('server hub, L3, client 2',
      ['--mode', 'server', '--endpoint', 'vpn.example.com', '--dns', '192.168.88.1',
       '--lan-server', '192.168.1.0/24', '--lan-client', '192.168.2.0/24', '--client', '2'], undefined,
      p => p.radio('deploy-mode', 'server').type('srv-endpoint', 'vpn.example.com').type('srv-dns', '192.168.88.1')
            .type('srv-lan-server', '192.168.1.0/24').type('srv-lan-client', '192.168.2.0/24')
            .type('srv-client-num', '2').generate(),
      SRV_PARTS);

check('server hub, a phone as the client', ['--mode', 'server', '--net', 'awg', '--endpoint', '198.51.100.1'], undefined,
      p => p.radio('deploy-mode', 'server').radio('srv-net', 'awg').type('srv-endpoint', '198.51.100.1').generate(),
      SRV_PARTS);

const link = (/^# (https:\S+\?c=\S+)$/m.exec(hub.out) || [])[1];
ok('the hub prints its share link', !!link, hub.out.slice(0, 300));
if (link) {
    check('adding client 3 from the saved link',
          ['--share-url', link, '--dns', '192.168.88.1', '--lan-client', '192.168.3.0/24', '--client', '3'], undefined,
          p => p.type('srv-dns', '192.168.88.1').type('srv-lan-client', '192.168.3.0/24').type('srv-client-num', '3').generate(),
          { link: 'srv-share-url', client: 'srv-out-client', 'server-update': 'srv-out-update' }, link);
}

/* ---- errors: the page's own words, exit 1 ---- */
console.log('--- errors');
function pageErrors(byHand) {
    const p = person();
    byHand(p);
    const box = p.d.getElementById('errors-container');
    const items = [].map.call(box.querySelectorAll('li'), li => li.textContent.trim());
    const text = items.length ? items : [box.textContent.trim()];
    p.close();
    return text;
}
function refused(name, args, stdin, byHand) {
    const r = cli(args, stdin);
    const want = pageErrors(byHand);
    ok(name + ': exit 1, nothing on stdout', r.code === 1 && r.out === '', r.code + ' ' + r.out.slice(0, 80));
    ok(name + ': the page has an error to show', want.length > 0 && want[0].length > 0);
    want.forEach(e => ok(name + ': says "' + e.slice(0, 60) + '"', r.err.indexOf(e) >= 0, r.err));
    return r;
}
const broken = AWG.replace('PrivateKey = ' + KEY, 'PrivateKey = xyz').replace(/^Endpoint = .*$/m, '');
refused('a broken config', [], broken, p => p.type('conf-input', broken).generate());
const hostOnly = AWG.replace('203.0.113.10:51820', 'vpn.example.com:51820').replace('1.1.1.1, 1.0.0.1', '10.8.0.1');
const noDns = refused('a hostname endpoint and no public DNS', ['--lang', 'en'], hostOnly,
                      p => p.click('#lang-toggle').type('conf-input', hostOnly).generate());
ok('names the option that fixes it', /--dns/.test(noDns.err), noDns.err);
refused('standalone without the device address', ['--mode', 'standalone'], AWG,
        p => p.radio('deploy-mode', 'standalone').type('conf-input', AWG).generate());
skip('site-to-site without side B', SITE_TO_SITE_HIDDEN, function () {
refused('site-to-site without side B', ['--mode', 'site-to-site'], undefined,
        p => p.radio('deploy-mode', 'site-to-site').generate());
});

/* ---- the command line ---- */
console.log('--- command line');
function usage(name, args, stdin, re) {
    const r = cli(args, stdin);
    ok(name + ': exit 2', r.code === 2, r.code + ' ' + r.err);
    ok(name + ': says why', re.test(r.err), r.err);
}
usage('an unknown option', ['--frobnicate'], AWG, /Unknown option/);
usage('an unknown mode', ['--mode', 'cloud'], AWG, /container, standalone, warp, server$/m);
usage('site-to-site, hidden on the page', ['--mode', 'site-to-site'], undefined, /container, standalone, warp, server$/m);
usage('an option of site-to-site only', ['--lan-a', '192.168.1.0/24'], AWG, /Unknown option '--lan-a'/);
usage('an unknown service', ['--services', 'youtub'], AWG, /youtube/);
usage('an option from another mode', ['--endpoint', '198.51.100.1'], AWG, /--endpoint is for --mode server, not container/);
usage('services outside DNS routing', ['--scenario', 'tunnel', '--services', 'youtube'], AWG, /--scenario dns-fwd/);
usage('a part this mode does not make', ['--part', 'side-a'], AWG, /install/);
usage('a .conf where keys are generated', ['--mode', 'server', 'x.conf'], undefined, /takes no \.conf/);
usage('junk for a config that has it', ['--warp-junk'], AWG, /--warp-junk/);
usage('not a domain', ['--domain', 'not a domain'], AWG, /--domain/);
const gone = cli(['--scenario', 'tunnel', '--part', 'routing'], AWG);
ok('a part the options did not make: exit 1, the parts it did make named',
   gone.code === 1 && /install, uninstall/.test(gone.err), gone.err);

let guarded = '';
cli([], AWG, function (win) {
    try { win.fetch('https://example.invalid/'); } catch (e) { guarded = e.message; }
    freeze(win);
});
ok('the page runs with the network cut off', /offline/.test(guarded), guarded);

const quiet = cli(['-q', '--mode', 'warp'], WARP);
ok('--quiet leaves stderr empty', quiet.code === 0 && quiet.err === '', quiet.err);
const loud = cli(['--mode', 'warp', '--lang', 'en'], WARP);
ok('notes go to stderr, with the page buttons as options', /^note: /m.test(loud.err) && /\(--warp-quic\)/.test(loud.err), loud.err);

/* A real process: a file, stdin, a BOM, --help. No PersistentKeepalive, so
 * two runs differ only in the date line and the random WireGuard port. */
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'awg-gen-'));
const plain = AWG.replace(/^PersistentKeepalive.*$/m, '');
const file = path.join(tmp, 'amnezia.conf');
fs.writeFileSync(file, plain);
fs.writeFileSync(path.join(tmp, 'bom.conf'), '\uFEFF' + plain.replace(/\n/g, '\r\n'));
const node = (args, input) => spawnSync(process.execPath, [TOOL].concat(args), { input: input, encoding: 'utf8' });
const undated = s => s.replace(/^# \d{4}-\d\d-\d\d \d\d:\d\d:\d\d$/m, '').replace(/listen-port=\d+/g, 'listen-port=N');
const fromFile = node([file]);
ok('node tools/awg-gen.js FILE: exit 0', fromFile.status === 0, fromFile.stderr);
ok('starts with the install part', /^# >>> \[install\] /.test(fromFile.stdout), fromFile.stdout.slice(0, 80));
ok('and is a RouterOS script', fromFile.stdout.indexOf('/container/add') > 0 && fromFile.stdout.indexOf('/system/script/run awg-proxy-1-uninstall') > 0);
const fromStdin = node([], plain);
same('stdin gives the same script', undated(fromStdin.stdout), undated(fromFile.stdout));
same('"-" means stdin', undated(node(['-'], plain).stdout), undated(fromFile.stdout));
same('a BOM and CRLF from Notepad change nothing', undated(node([path.join(tmp, 'bom.conf')]).stdout), undated(fromFile.stdout));
const help = node(['--help']);
ok('--help: exit 0', help.status === 0, help.stderr);
ok('lists the modes, the parts and the services from the page',
   /--mode MODE/.test(help.stdout) && /--part NAME/.test(help.stdout) && /youtube\*/.test(help.stdout) &&
   /server: link, server, client/.test(help.stdout), help.stdout.slice(0, 200));
ok('and no site-to-site: it is hidden', !/site-to-site|side-a|--lan-a/.test(help.stdout), help.stdout.slice(0, 200));
const bad = node(['--lang', 'en'], plain.replace(/^S1 = .*$/m, 'S1 = x'));
ok('a refused config: exit 1, the reason on stderr, nothing on stdout',
   bad.status === 1 && /S1/.test(bad.stderr) && bad.stdout === '', bad.status + ' ' + bad.stderr);
fs.rmSync(tmp, { recursive: true, force: true });

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
