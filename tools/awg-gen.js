#!/usr/bin/env node
/* awg-gen: RouterOS commands from an AmneziaWG .conf, on the command line.
 *
 * The configurator page (docs/conf3.0.html) is the only copy of the generation
 * logic. This loads it in jsdom, fills the form the way a person would - picks
 * the mode, pastes the config, ticks the boxes - presses Generate and prints
 * what the page shows. The script is the page's own, and a fix to the page
 * reaches the command line with no second copy to keep in step.
 *
 * Offline like the page: the HTML is read from disk, jsdom loads nothing, and
 * fetch/XHR/sockets are cut off before the page's script runs. A private key
 * given to this tool stays on this machine - which is why it is a local tool
 * and not a service.
 *
 *   npm install jsdom
 *   node tools/awg-gen.js amnezia.conf > awg.rsc
 *   node tools/awg-gen.js --help
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { parseArgs } = require('util');

const ROOT = path.join(__dirname, '..');
const PAGE = path.join(ROOT, 'docs', 'conf3.0.html');
// Where the page is published. Only the address is used, nothing is fetched
// from it, but the page builds repository and share links from it.
const PAGE_URL = 'https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html';

const CONF = ['container', 'standalone', 'warp'];
// Site-to-site is hidden on the page, so it is not offered here either: its
// parts and options below stay, but --mode does not take it and its options
// are not parsed or listed.
// TODO: if nobody asks for it back by the end of 2026, remove it in 2027
// together with the page's code (see the note at the mode radios there).
const HUB = ['server'];
const MODES = CONF.concat(HUB);
const ROUTED = ['dns-fwd', 'non-ru'];

// What Generate puts on the page, in page order. A part is printed when its
// block is shown and not empty.
const PARTS = {
    conf: [
        { name: 'offline', id: 'offline-output', title: 'offlineTitle', note: 'offlineNote' },
        { name: 'install', id: 'output', title: 'outputTitle' },
        { name: 'routing', id: 'routing-output', title: 'routingTitle', note: 'routingNote' },
        { name: 'uninstall', id: 'uninstall-output', title: 'uninstallTitle' },
    ],
    'site-to-site': [
        { name: 'side-a', id: 's2s-out-a', title: 's2sOutputA' },
        { name: 'uninstall-a', id: 's2s-uninst-a', title: 's2sUninstallA' },
        { name: 'side-b', id: 's2s-out-b', title: 's2sOutputB' },
        { name: 'uninstall-b', id: 's2s-uninst-b', title: 's2sUninstallB' },
    ],
    server: [
        { name: 'link', id: 'srv-share-url', title: 'srvSaveLink', note: 'srvEnvHint' },
        { name: 'server', id: 'srv-out-server', title: 'srvOutputServer' },
        { name: 'client', id: 'srv-out-client', heading: 'srv-client-title' },
        { name: 'server-update', id: 'srv-out-update', title: 'srvServerUpdate' },
    ],
};

function partsOf(mode) { return PARTS[CONF.indexOf(mode) >= 0 ? 'conf' : mode]; }

class UsageError extends Error {}

function lower(s) { return String(s).trim().toLowerCase(); }

function oneOf(what, v, allowed, aliases) {
    const k = lower(v);
    const hit = allowed.find(a => lower(a) === k) || (aliases && aliases[k]);
    if (!hit) throw new UsageError(what + ' ' + JSON.stringify(v) + ': expected ' +
        allowed.filter(a => a !== '__custom').map(a => a === '__auto' ? 'auto' : a).join(', '));
    return hit;
}

// "youtube,telegram" against the page's SERVICES or EXCLUSIONS: a key or the
// name shown next to the box; "all" and "none" as on the buttons.
function listOf(what, v, map) {
    const keys = Object.keys(map);
    const k = lower(v);
    if (k === 'all') return keys;
    if (k === '' || k === 'none') return [];
    return k.split(',').map(s => s.trim()).filter(Boolean).map(function (s) {
        const hit = keys.find(key => key === s || lower(map[key].list) === s || lower(map[key].label || '') === s);
        if (!hit) throw new UsageError(what + ' ' + JSON.stringify(s) + ': expected some of ' + keys.join(', '));
        return hit;
    });
}

// Form controls worked the way a person would: a click on a box or a radio,
// so its onchange handlers run, and an input event after typing.
function controls(w) {
    const d = w.document;
    const byId = id => d.getElementById(id);
    const fire = (el, type) => el.dispatchEvent(new w.Event(type, { bubbles: true }));
    return {
        w: w, d: d, byId: byId,
        type(id, v) { const el = byId(id); el.value = v; fire(el, 'input'); fire(el, 'change'); },
        select(id, v) { const el = byId(id); el.value = v; fire(el, 'change'); },
        tick(el, on) { if (el.checked !== on) el.click(); },
        check(id, on) { this.tick(byId(id), on); },
        choose(name, v) { const el = d.querySelector('input[name="' + name + '"][value="' + v + '"]'); if (!el.checked) el.click(); },
        values(name) { return [].map.call(d.getElementsByName(name), el => el.value); },
        options(id) { return [].map.call(byId(id).options, o => o.value); },
    };
}

// A known option of the select, or "Custom..." and the text field next to it.
function pickOrCustom(ui, sel, custom, v, aliases) {
    const k = (aliases && aliases[lower(v)]) || v;
    if (ui.options(sel).indexOf(k) >= 0 && k !== '__custom') ui.select(sel, k);
    else { ui.select(sel, '__custom'); ui.type(custom, v); }
}

// A button under the parsed fields. The page shows it after Generate, and only
// for a config it fits, so: Generate, then the button, as a person would.
function press(ui, flag, call, what) {
    ui.w.generate();
    const b = ui.d.querySelector('#fields-display button[onclick="' + call + '"]');
    if (!b) throw new UsageError(flag + ' is for ' + what + '; the page does not offer it for this one');
    b.click();
}

const AUTO = { auto: '__auto' };
const hubId = (mode, s2s, srv) => mode === 'site-to-site' ? s2s : srv;
// An option for no mode that is offered is left out, as if it did not exist.
const offered = o => !o.modes || o.modes.some(m => MODES.indexOf(m) >= 0);

// One entry per option, applied in this order. `modes`, `scenarios` and
// `nets` say where the page shows the control; elsewhere the option is
// refused rather than silently dropped.
const OPTIONS = [
    { group: 'General' },
    { name: 'mode', arg: 'MODE', help: 'container (default), standalone, warp, server (MikroTik as an AmneziaWG server)' },
    { name: 'prefix', arg: 'NAME', help: 'tunnel name: awg-proxy-1 by default, awg-server-1 for server',
      apply: (ui, v) => pickOrCustom(ui, 'prefix-select', 'prefix-custom', v) },
    { name: 'part', arg: 'NAME', help: 'print only this part, exactly as the page shows it (see Parts)' },
    { name: 'lang', arg: 'ru|en', help: 'language of errors, notes and part headings (default ru)' },
    { name: 'quiet', short: 'q', bool: true, help: 'no notes on stderr, errors only' },
    { name: 'help', short: 'h', bool: true, help: 'this text' },

    { group: 'Container, standalone and WARP: the .conf is the input' },
    // The page's two buttons that rewrite the pasted config, pressed first.
    { name: 'warp-quic', bool: true, modes: CONF,
      help: 'Cloudflare WARP: send a packet shaped like Chrome QUIC before the handshake (I1 and Jc = 0 go into the config)',
      apply: ui => press(ui, '--warp-quic', 'addWarpQuicCover()', 'a Cloudflare WARP config') },
    { name: 'warp-junk', bool: true, modes: CONF,
      help: 'config with no obfuscation at all (plain WireGuard, bare WARP): add junk packets before the handshake (Jc = 4, Jmin = 40, Jmax = 70)',
      apply(ui, v, mode, opts) {
          if (opts['warp-quic']) throw new UsageError('--warp-junk and --warp-quic exclude each other');
          press(ui, '--warp-junk', 'addWarpJunk()', 'a config with no AmneziaWG obfuscation');
      } },
    { name: 'scenario', arg: 'NAME', modes: CONF, help: 'dns-fwd (default), tunnel, non-ru',
      apply: (ui, v) => ui.choose('scenario', oneOf('--scenario', v, ui.values('scenario'))) },
    { name: 'services', arg: 'LIST', modes: CONF, scenarios: ['dns-fwd'],
      help: 'services routed through the tunnel, comma-separated, or all / none (default: the page\'s preselection)',
      apply(ui, v) {
          const want = listOf('--services', v, ui.w.SERVICES);
          ui.d.querySelectorAll('input[name="svc"]').forEach(cb => ui.tick(cb, want.indexOf(cb.value) >= 0));
      } },
    { name: 'domain', arg: 'NAME', multiple: true, modes: CONF, scenarios: ['dns-fwd'],
      help: 'one more domain routed through the tunnel; repeat for several',
      apply(ui, list) {
          list.forEach(function (domain) {
              ui.byId('custom-domain-input').value = domain;
              ui.w.addCustomDomain();
              if (ui.w.getCustomDomains().indexOf(lower(domain)) < 0)
                  throw new UsageError('--domain ' + JSON.stringify(domain) + ': not a domain name');
          });
      } },
    { name: 'exclude', arg: 'LIST', modes: CONF, scenarios: ['non-ru'],
      help: 'services kept off the tunnel, comma-separated, or none (default: the page\'s preselection)',
      apply(ui, v) {
          const want = listOf('--exclude', v, ui.w.EXCLUSIONS);
          ui.d.querySelectorAll('input[name="excl"]').forEach(cb => ui.tick(cb, want.indexOf(cb.value) >= 0));
      } },
    { name: 'doh', arg: 'NAME|URL|IP', modes: CONF, scenarios: ROUTED,
      help: 'DNS forwarder: google, cloudflare, comss, quad9, yandex, a DoH URL or a DNS IP (default: picked by the config\'s DNS)',
      apply(ui, v) {
          const named = ui.values('doh-fwd').filter(n => n !== '__custom');
          const hit = named.find(n => lower(n) === lower(v) || lower(n) === lower(v) + 'dns');
          if (hit) { ui.choose('doh-fwd', hit); return; }
          if (!/^https:\/\/\S+$/.test(v) && !/^[0-9a-fA-F.:]+$/.test(v))
              throw new UsageError('--doh ' + JSON.stringify(v) + ': expected ' + named.join(', ') + ', an https:// URL or an IP');
          ui.choose('doh-fwd', '__custom');
          ui.type('doh-custom-url', v);
      } },
    { name: 'dns', arg: 'IP',
      help: 'DNS the container resolves the endpoint with (AWG_DNS). Needed for a hostname endpoint unless the config has a public DNS',
      apply: (ui, v, mode) => ui.type(mode === 'site-to-site' ? 's2s-dns' : mode === 'server' ? 'srv-dns' : 'awg-dns', v) },
    { name: 'storage', arg: 'DISK', modes: ['container', 'warp'],
      help: 'container storage: auto (default; usb1, else disk1), a disk from the page\'s list or any other mount point',
      apply: (ui, v) => pickOrCustom(ui, 'storage-select', 'storage-custom', v, AUTO) },
    { name: 'proxy-ip', arg: 'IP', modes: ['standalone'], help: 'address of the device that runs the proxy',
      apply: (ui, v) => ui.type('proxy-ip', v) },
    { name: 'offline', bool: true, help: 'add step 0: what to download for an install without internet access',
      apply: ui => ui.check('offline-enable', true) },
    { name: 'no-netwatch', bool: true, modes: CONF, scenarios: ROUTED, help: 'no netwatch failover',
      apply: ui => ui.check('netwatch-enable', false) },
    { name: 'no-force-ipv4', bool: true, modes: CONF, scenarios: ROUTED, help: 'do not block native IPv6 past the tunnel',
      apply: ui => ui.check('v6block-enable', false) },
    { name: 'no-autoupdate', bool: true, help: 'no daily check for container updates',
      apply: ui => ui.check('autoupdate-enable', false) },
    { name: 'perf', arg: 'auto|strong|weak', help: 'router class for the performance tuning (default auto: asked from the router)',
      apply: (ui, v) => ui.choose('perf-mode', oneOf('--perf', v, ui.values('perf-mode'))) },
    { name: 'clear-df', bool: true, help: 'clear the DF bit on the proxy\'s UDP (AWG_NO_DF=1)',
      apply: ui => ui.check('df-enable', true) },
    { name: 'ipv6', bool: true, help: 'the server is reached over IPv6 (set by itself for an IPv6 endpoint)',
      apply: ui => ui.check('ipv6-enable', true) },
    { name: 'extra-time', arg: 'TIME', help: 'hold DNS address-list entries past their TTL, e.g. 30m',
      apply(ui, v) { ui.check('extratime-enable', true); ui.type('extratime-value', v); } },
    { name: 'path-mtu', arg: 'N', help: 'MTU of the underlay link: 1500 (default), 1492 for PPPoE',
      apply: (ui, v) => ui.type('path-mtu', v) },
    { name: 'no-warp-ports', bool: true, modes: CONF, help: 'WARP: keep the endpoint port as written instead of all WARP ports',
      apply: ui => ui.check('warp-ports-enable', false) },

    { group: 'Server mode: no .conf, keys are generated on this machine' },
    // The level goes first: changing it rolls new parameters, port included.
    { name: 'proto', arg: 'LEVEL', modes: HUB, help: 'AmneziaWG level: v3 (default), v2, v1.5, v1',
      apply(ui, v, mode) {
          const id = hubId(mode, 's2s-proto-level', 'srv-proto-level');
          ui.select(id, oneOf('--proto', /^v/i.test(v) ? v : 'v' + v, ui.options(id)));
      } },
    { name: 'no-compat', bool: true, modes: HUB, help: 'no fallback chain for peers on older levels',
      apply: (ui, v, mode) => ui.check(hubId(mode, 's2s-proto-compat', 'srv-proto-compat'), false) },
    { name: 'endpoint', arg: 'HOST', modes: HUB, help: 'public IP or name of the server router',
      apply: (ui, v, mode) => ui.type(hubId(mode, 's2s-endpoint-b', 'srv-endpoint'), v) },
    { name: 'port', arg: 'N', modes: HUB, help: 'AmneziaWG port on it (default: random)',
      apply: (ui, v, mode) => ui.type(hubId(mode, 's2s-awg-port', 'srv-awg-port'), v) },
    { name: 'subnet', arg: 'CIDR', modes: HUB, help: 'tunnel subnet, a /24 (default: random)',
      apply: (ui, v, mode) => ui.type(hubId(mode, 's2s-tunnel-subnet', 'srv-tunnel-subnet'), v) },
    { name: 'lan-a', arg: 'CIDR', modes: ['site-to-site'], help: 'LAN of side A (optional)',
      apply: (ui, v) => ui.type('s2s-lan-a', v) },
    { name: 'lan-b', arg: 'CIDR', modes: ['site-to-site'], help: 'LAN of side B (optional)',
      apply: (ui, v) => ui.type('s2s-lan-b', v) },
    { name: 'storage-a', arg: 'DISK', modes: ['site-to-site'], help: 'storage on side A: auto (default), disk1, usb1, usb1-part1',
      apply: (ui, v) => ui.select('s2s-storage-a', oneOf('--storage-a', v, ui.options('s2s-storage-a'), AUTO)) },
    { name: 'storage-b', arg: 'DISK', modes: ['site-to-site'], help: 'storage on side B, as above',
      apply: (ui, v) => ui.select('s2s-storage-b', oneOf('--storage-b', v, ui.options('s2s-storage-b'), AUTO)) },
    { name: 'net', arg: 'none|l3|awg', modes: ['server'], help: 'network merging: l3 (default), none, awg (a phone or laptop client)',
      apply: (ui, v) => ui.choose('srv-net', oneOf('--net', v, ui.values('srv-net'))) },
    { name: 'lan-server', arg: 'CIDR', modes: ['server'], nets: ['none', 'l3'], help: 'LAN of the server router (required for l3)',
      apply: (ui, v) => ui.type('srv-lan-server', v) },
    { name: 'lan-client', arg: 'CIDR', modes: ['server'], nets: ['none', 'l3'], help: 'LAN of the client (required for l3)',
      apply: (ui, v) => ui.type('srv-lan-client', v) },
    { name: 'storage-server', arg: 'DISK', modes: ['server'], help: 'storage on the server router: auto (default), disk1, usb1, usb1-part1',
      apply: (ui, v) => ui.select('srv-storage-server', oneOf('--storage-server', v, ui.options('srv-storage-server'), AUTO)) },
    { name: 'storage-client', arg: 'DISK', modes: ['server'], nets: ['none', 'l3'], help: 'storage on the client router, as above',
      apply: (ui, v) => ui.select('srv-storage-client', oneOf('--storage-client', v, ui.options('srv-storage-client'), AUTO)) },
    { name: 'client', arg: 'N', modes: ['server'], help: 'client number (default 1)',
      apply: (ui, v) => ui.type('srv-client-num', v) },
    { name: 'share-url', arg: 'URL', modes: ['server'], help: 'add a client to an existing server router: the link saved when it was set up' },
];

function helpText(page) {
    const L = [
        'awg-gen - MikroTik RouterOS commands from an AmneziaWG .conf, offline',
        '',
        'Usage:',
        '  node tools/awg-gen.js [options] FILE.conf      (or - for stdin, or pipe it in)',
        '  node tools/awg-gen.js --mode server [options]',
        '',
        'Runs the configurator page docs/conf3.0.html in jsdom and prints the',
        'script the page would show: the same code makes it, and nothing leaves',
        'this machine. Needs Node 20+ and jsdom (npm install jsdom).',
        '',
        'The script goes to stdout, notes and errors to stderr. Each part starts',
        'with a "# >>> [name]" heading; run the parts in that order, each one',
        'after the previous has finished. Exit code: 0 done, 1 the page refused',
        'the input (its error is printed), 2 bad command line.',
    ];
    OPTIONS.forEach(function (o) {
        if (o.group) { L.push('', o.group + ':'); return; }
        if (!offered(o)) return;
        const flag = (o.short ? '-' + o.short + ', ' : '    ') + '--' + o.name + (o.arg ? ' ' + o.arg : '');
        const words = o.help.split(' ');
        let line = '  ' + flag.padEnd(28);
        if (line.length > 30) { L.push(line.trimEnd()); line = ' '.repeat(30); }
        words.forEach(function (word) {
            if (line.length + word.length > 79 && line.trim()) { L.push(line.trimEnd()); line = ' '.repeat(30); }
            line += word + ' ';
        });
        L.push(line.trimEnd());
    });
    L.push('', 'Parts (--part):');
    L.push('  container, standalone, warp: ' + PARTS.conf.map(p => p.name).join(', '));
    L.push('  server: ' + PARTS.server.map(p => p.name).join(', '));
    if (page) {
        const on = map => Object.keys(map).map(k => k + (map[k].on ? '*' : '')).join(', ');
        L.push('', 'Services for --services (* = on by default):', '  ' + on(page.w.SERVICES));
        L.push('Services for --exclude:', '  ' + on(page.w.EXCLUSIONS));
        L.push('Disks for --storage:', '  ' + page.options('storage-select').filter(v => v !== '__custom')
            .map(v => v === '__auto' ? 'auto' : v).join(', '));
    }
    L.push('', 'Examples:',
        '  node tools/awg-gen.js amnezia.conf > awg.rsc',
        '  node tools/awg-gen.js --part routing --services youtube,telegram amnezia.conf',
        '  node tools/awg-gen.js --scenario non-ru --exclude steam,epic amnezia.conf',
        '  node tools/awg-gen.js --mode server --net awg --endpoint 198.51.100.1 --part server');
    return L.join('\n') + '\n';
}

function loadPage(url, beforeParse) {
    let jsdom;
    try { jsdom = require('jsdom'); }
    catch (e) {
        if (e.code !== 'MODULE_NOT_FOUND') throw e;
        throw new UsageError('jsdom is not installed. Run "npm install jsdom" in ' + ROOT);
    }
    const errors = [];
    const vc = new jsdom.VirtualConsole();
    vc.on('jsdomError', e => errors.push(e.message));
    const dom = new jsdom.JSDOM(fs.readFileSync(PAGE, 'utf8'), {
        runScripts: 'dangerously',
        url: url,
        virtualConsole: vc,
        beforeParse(win) {
            for (const name of ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource']) {
                try { win[name] = function () { throw new Error('awg-gen works offline: ' + name + ' is disabled'); }; } catch (e) { /* ignore */ }
            }
            if (beforeParse) beforeParse(win);
        },
    });
    const ui = controls(dom.window);
    ui.pageErrors = errors;
    return ui;
}

// The page's buttons that have an option here.
const BUTTONS = { 'addWarpQuicCover()': '--warp-quic', 'addWarpJunk()': '--warp-junk' };

// Text of a message block: a button becomes its option, line breaks and list
// items are kept, so an error with several reasons stays one per line.
function textOf(el) {
    const c = el.cloneNode(true);
    c.querySelectorAll('button').forEach(function (b) {
        const flag = BUTTONS[b.getAttribute('onclick')];
        b.replaceWith(flag ? ' (' + flag + ')' : '');
    });
    c.querySelectorAll('br').forEach(b => b.replaceWith('\n'));
    c.querySelectorAll('li').forEach(li => li.replaceWith('\n- ' + li.textContent + '\n'));
    return c.textContent.split('\n').map(s => s.replace(/\s+/g, ' ').trim()).filter(Boolean).join('\n');
}

function shown(el) {
    for (; el; el = el.parentElement) if (el.style && el.style.display === 'none') return false;
    return true;
}

function indent(s) { return s.split('\n').join('\n  '); }

function run(argv, io) {
    let opts, positionals;
    try {
        const spec = {};
        OPTIONS.forEach(function (o) {
            if (!o.name || !offered(o)) return;
            spec[o.name] = { type: o.bool ? 'boolean' : 'string' };
            if (o.short) spec[o.name].short = o.short;
            if (o.multiple) spec[o.name].multiple = true;
        });
        ({ values: opts, positionals } = parseArgs({ args: argv, options: spec, allowPositionals: true, strict: true }));
    } catch (e) {
        io.err('awg-gen: ' + e.message + '\nTry: node tools/awg-gen.js --help\n');
        return 2;
    }

    let ui;
    try {
        if (opts.help) {
            // The lists of services and disks come from the page; the rest of
            // the help does not need it.
            let page = null, why = '';
            try { page = loadPage(PAGE_URL); } catch (e) { why = e.message; }
            io.out(helpText(page) + (why ? '\n(' + why + ')\n' : ''));
            if (page) page.w.close();
            return 0;
        }

        const mode = opts.mode ? oneOf('--mode', opts.mode, MODES) : opts['share-url'] ? 'server' : 'container';
        const scenario = opts.scenario ? oneOf('--scenario', opts.scenario, ROUTED.concat('tunnel')) : 'dns-fwd';
        const net = opts.net ? oneOf('--net', opts.net, ['none', 'l3', 'awg']) : 'l3';
        OPTIONS.forEach(function (o) {
            if (!o.name || opts[o.name] === undefined) return;
            if (o.modes && o.modes.indexOf(mode) < 0)
                throw new UsageError('--' + o.name + ' is for --mode ' + o.modes.join(', ') + ', not ' + mode);
            if (o.scenarios && o.scenarios.indexOf(scenario) < 0)
                throw new UsageError('--' + o.name + ' is for --scenario ' + o.scenarios.join(', ') + ', not ' + scenario);
            if (o.nets && o.nets.indexOf(net) < 0)
                throw new UsageError('--' + o.name + ' is for --net ' + o.nets.join(', ') + ', not ' + net);
        });
        const parts = partsOf(mode);
        if (opts.part) oneOf('--part', opts.part, parts.map(p => p.name));
        const lang = opts.lang ? oneOf('--lang', opts.lang, ['ru', 'en']) : 'ru';

        let conf = null;
        if (CONF.indexOf(mode) >= 0) {
            if (positionals.length > 1) throw new UsageError('one .conf at a time, got ' + positionals.length);
            const file = positionals[0];
            if (file && file !== '-') {
                try { conf = fs.readFileSync(file, 'utf8'); }
                catch (e) { throw new UsageError('cannot read ' + file + ': ' + e.message); }
            } else if (io.stdin !== undefined) conf = io.stdin;
            else if (!file && process.stdin.isTTY) throw new UsageError('no .conf given: pass a file, or pipe it in');
            else conf = fs.readFileSync(0, 'utf8');
            conf = conf.replace(/^\uFEFF/, '');
        } else if (positionals.length) {
            throw new UsageError('--mode ' + mode + ' takes no .conf: its keys are generated here');
        }

        // An add-client link opens the page with the hub filled in, as in a browser.
        let url = PAGE_URL;
        if (opts['share-url']) {
            const c = /(?:[?&]c=)?([A-Za-z0-9_-]+)$/.exec(opts['share-url'].trim());
            if (!c) throw new UsageError('--share-url: expected the link the page gave when the server router was set up');
            url += '?c=' + c[1];
        }
        ui = loadPage(url, io.beforeParse);
        const w = ui.w;
        if (opts['share-url'] && !w.serverFromURL)
            throw new UsageError('--share-url: the page did not take this link (wrong or from an older page)');

        if (lang !== 'ru') w.setLang(lang);
        ui.choose('deploy-mode', mode);
        if (conf !== null) ui.type('conf-input', conf);
        OPTIONS.forEach(function (o) {
            if (o.apply && opts[o.name] !== undefined) o.apply(ui, opts[o.name], mode, opts);
        });
        w.generate();

        const errBox = ui.byId('errors-container');
        const notes = [];
        ui.d.querySelectorAll('#fields-display .alert, #errors-container .alert').forEach(function (el) {
            if (el.closest('#errors-container') && el.classList.contains('alert-error')) return;
            notes.push((el.classList.contains('alert-info') ? 'note: ' : 'warning: ') + indent(textOf(el)));
        });
        const failed = errBox.querySelector('.alert-error');
        if (!opts.quiet) notes.forEach(n => io.err(n + '\n'));
        if (failed) {
            io.err('error: ' + indent(textOf(failed)) + '\n');
            // The page marks the field to fix; name the option that fills it.
            ui.d.querySelectorAll('.field-invalid').forEach(function (el) {
                if (el.id === 'awg-dns') io.err('  (set it with --dns)\n');
            });
            return 1;
        }

        const out = [];
        parts.forEach(function (p) {
            const el = ui.byId(p.id);
            const text = el.tagName === 'INPUT' ? el.value : (el.dataset.plain || '');
            if (!text || !shown(el)) return;
            out.push({ part: p, text: text.endsWith('\n') ? text : text + '\n' });
        });
        if (!out.length) {
            io.err('error: the page produced nothing' + (ui.pageErrors.length ? ':\n  ' + ui.pageErrors.join('\n  ') : '') + '\n');
            return 1;
        }
        if (opts.part) {
            const hit = out.find(o => o.part.name === lower(opts.part));
            if (!hit) {
                io.err('error: no ' + lower(opts.part) + ' part with these options; the page made ' +
                       out.map(o => o.part.name).join(', ') + '\n');
                return 1;
            }
            io.out(hit.text);
            return 0;
        }
        io.out(out.map(function (o) {
            const p = o.part;
            const title = p.heading ? ui.byId(p.heading).textContent : w.t(p.title);
            let head = '# >>> [' + p.name + '] ' + title.replace(/\s+/g, ' ').trim() + '\n';
            if (p.note) head += '# >>> ' + w.t(p.note).replace(/\s+/g, ' ').trim() + '\n';
            // The share link is not a command: keep it a comment in the joint output.
            return head + (p.name === 'link' ? '# ' + o.text : o.text);
        }).join('\n'));
        return 0;
    } catch (e) {
        if (!(e instanceof UsageError)) throw e;
        io.err('awg-gen: ' + e.message + '\n');
        return 2;
    } finally {
        if (ui) ui.w.close();
    }
}

module.exports = { run, PAGE_URL, PARTS };

if (require.main === module) {
    process.exitCode = run(process.argv.slice(2), {
        out: s => process.stdout.write(s),
        err: s => process.stderr.write(s),
    });
}
