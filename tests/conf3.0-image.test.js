/* Where the container image comes from, and how it gets updated. Driven through jsdom.
 *
 * RouterOS 7.22 added /container/repull: it re-downloads the image and restarts the
 * container with the same add-time parameters. That only works for a container
 * installed from a registry, so the generated script prefers ghcr.io on 7.22+ and
 * keeps the release archive as the fallback for everything older or offline.
 *
 * Three behaviours worth pinning down, all learned the hard way on a live 7.22.1 box:
 *   - /container/add with remote-image returns before the pull starts and never
 *     raises, so the script has to watch image-id instead of using :do/on-error;
 *   - repull restarts the container every time, even when the digest has not moved,
 *     so the update script checks the published release before pulling;
 *   - an archive the user placed on the router by hand must survive the install.
 *
 * Dev-only: the proxy itself has no dependencies, and this needs one.
 *   npm install jsdom && node tests/conf3.0-image.test.js
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

const need = ['generate', 'clearAll', 'buildImageSetupLines', 'buildUpdateScriptSource',
              'buildUpdateSchedulerLines', 'isAutoUpdateEnabled'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 container-image tests ===');

/* ---- helpers ---- */
function conf(endpoint) {
    return [
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
        'Endpoint = ' + (endpoint || '198.51.100.1:443'),
        'AllowedIPs = 0.0.0.0/0'
    ].join(NL);
}
function paste(text) {
    const ta = w.document.getElementById('conf-input');
    ta.value = text;
    ta.dispatchEvent(new w.Event('input', { bubbles: true }));
}
// autoUpdate: whether the checkbox is ticked when generate() runs.
function generateWith(autoUpdate) {
    w.clearAll();
    paste(conf());
    w.document.getElementById('autoupdate-enable').checked = !!autoUpdate;
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    const err = w.document.getElementById('errors-container').textContent.trim();
    if (err) throw new Error('generate() reported: ' + err);
    return w.document.getElementById('output').dataset.plain || '';
}
function lineWith(script, needle) {
    return script.split(NL).filter(l => l.indexOf(needle) >= 0);
}

const script = generateWith(true);

/* ---- the registry branch ---- */
ok('the registry image is ghcr.io/<owner>/awg-proxy',
   /:local img \("ghcr\.io\/[^"]+\/awg-proxy:" \. \$imgTag\)/.test(script),
   lineWith(script, ':local img').join(' | '));
ok('the default tag is :latest', /:local imgTag "latest"/.test(script));
ok('armv5 gets its own tag', /:set imgTag "latest-armv5"/.test(script));
ok('the registry branch is gated on RouterOS 7.22+',
   /:if \(!\$added && \$minor >= 22\) do=\{/.test(script));
ok('the pull goes through remote-image',
   /\/container\/add remote-image=\$img /.test(script));

// /container/add returns immediately and a broken pull only shows up as an empty
// image-id, so on-error would sail straight past a dead registry.
ok('the script waits for image-id instead of trusting on-error',
   /:while \(\[:len \$cids\] > 0 && \[:len \[\/container\/get \[:pick \$cids 0\] image-id\]\] = 0/.test(script));
ok('a failed pull removes the half-made container',
   /:do \{ \/container\/remove \$cids \} on-error=\{\}/.test(script));
ok('and falls back to the release archive',
   script.indexOf('Registry unavailable, falling back to the release archive') >= 0);

/* ---- the offline branch ---- */
// An archive already on the router is the user's own copy: use it, leave it alone.
ok('an image already on the router is used first',
   script.indexOf('Using image already on the router: ') >= 0);
// People drop the archive next to the container (disk1/, usb1/), not in the path
// /tool/fetch would have written to.
ok('and it is looked for on the storage disk too',
   /:if \(\[:len \[\/file\/find where name=\$filePath\]\] = 0 && \[:len \[\/file\/find where name=\$altPath\]\] > 0\) do=\{/.test(script));
ok('/file/remove only fires for what the script downloaded',
   /:if \(\$fetched\) do=\{ \/file\/remove \$filePath \}/.test(script));
eq('and nothing removes it unconditionally',
   lineWith(script, '/file/remove $filePath').length, 1);

/* ---- the fetch branch is still there ---- */
ok('the release archive is still fetched when needed',
   /\/tool\/fetch url=\$url dst-path=\$filePath/.test(script));
ok('guarded by the free-space check',
   /:if \(\$freeStorage < 262144\) do=\{/.test(script));
ok('all three sources feed one /container/start',
   lineWith(script, '/container/start [find where interface=veth-awg-proxy-1]').length === 1);

// A fresh install must not leave RouterOS free to re-pull whenever it feels like
// it: that is what left a live container with a broken image and a dead tunnel.
ok('the install pins ignore-remote-image-change on the container',
   /\[:parse "\/container\/set \[find where interface=veth-awg-proxy-1\] ignore-remote-image-change=yes"\]/.test(script));
ok('wrapped so RouterOS 7.22 and older still parse the script',
   /:do \{ \[:parse "[^"]*ignore-remote-image-change[^"]*"\] \} on-error=\{\}/.test(script));

// RouterOS restarts a crashed container by itself when told to: on-failure fires on
// a non-zero exit (how the proxy dies) and leaves a hand-stopped container alone.
// Ten tries half a minute apart, then <prefix>-update takes over.
ok('the install asks RouterOS to restart a crashed container',
   /\[:parse "\/container\/set \[find where interface=veth-awg-proxy-1\] restart-policy=on-failure restart-interval=30s restart-max-count=10"\]/.test(script));
ok('and that line is also deferred for RouterOS older than 7.23',
   /:do \{ \[:parse "[^"]*restart-policy[^"]*"\] \} on-error=\{\}/.test(script));

/* ---- the update script ---- */
ok('an update script is created', /\/system\/script\/add name=awg-proxy-1-update /.test(script));
ok('it calls repull', /\/container\/repull \$cid/.test(script));
ok('it refuses politely on a file-based install',
   script.indexOf('/container/repull only updates registry installs') >= 0);
ok('it refuses politely below 7.22',
   script.indexOf('/container/repull needs RouterOS 7.22+') >= 0);

// repull restarts the container every time, so a version check is what keeps the
// nightly job from dropping the tunnel for nothing.
ok('it compares the published release before pulling',
   /:if \(\$newVer != "" && \$newVer = \$curVer\) do=\{[\s\S]{0,80}:set doPull false/.test(script));
ok('and says so instead of pulling', script.indexOf('nothing to pull, tunnel untouched') >= 0);
ok('the release version is read from the GitHub API',
   /\/tool\/fetch url="https:\/\/api\.github\.com\/repos\/[^"]+\/releases\/latest"/.test(script));
ok('the parsed tag is remembered in AWG_IMAGE_VER',
   /key=AWG_IMAGE_VER value=\$newVer/.test(script));

// A pull that dies leaves the container stopped with no image at all.
ok('a failed pull is retried once', script.indexOf('Pull failed - retrying once in 15s') >= 0);
ok('a container that was running is started again',
   /:if \(\$wantRunning\) do=\{/.test(script));
ok('and a twice-failed pull is logged as an error',
   /:log error "awg-proxy-1-update: rebuild failed, tunnel is down"/.test(script));

// An image-id is not proof of life: a pull that ends in "skip importing same
// version" can leave root-dir empty, and the container then dies on execvpe.
ok('the container is checked for real after the pull',
   /:do \{ :if \(\[\/container\/get \$cid stopped\] != true\) do=\{ :set backUp true \} \} on-error=\{\}/.test(script));
ok('and rebuilt from the registry if it stayed down',
   /:if \(!\$backUp\) do=\{/.test(script) &&
   /\/container\/add remote-image=\$img interface=veth-awg-proxy-1 /.test(script));
// Stale .backup directories are what confuses the rename dance in the first place.
ok('leftovers are cleared before the pull, not only after',
   lineWith(script, '/file/remove [find where name~($rootDirOrig . ".backup")]').length === 3);
// RouterOS puts the copy next to the container's real root-dir. A blue/green swap
// leaves it in <root>-n, older installs carry a version suffix - a pattern built
// from the prefix missed those copies, and they piled up until a pull broke.
ok('leftovers are looked for next to the real root-dir, not by prefix',
   script.indexOf('"/awg-proxy-1.backup"') < 0);

// Layers land in tmpdir: on a 16MB-flash router with the container on USB, the
// default (internal flash) has nowhere to put them.
ok('tmpdir is moved onto the disk that holds root-dir',
   /\/container\/config set tmpdir=\(\$disk \. "\/pull"\)/.test(script));
// Several installs on one router check minutes apart and a slow pull overlaps the
// next: a shared answer file gets read, or deleted, by the wrong script.
ok('the published-version answer lands in a file of its own',
   /:set verPath \(\$disk \. "\/pull\/awg-proxy-1-update-check\.json"\)/.test(script) &&
   script.indexOf('"awg-update-check.json"') < 0);
ok('repull leftovers are swept',
   /\/file\/remove \[find where name~\(\$rootDirOrig \. "\.backup"\)\]/.test(script));

/* ---- the scheduler follows the checkbox ---- */
ok('ticked: the daily scheduler is there',
   /\/system\/scheduler\/add name=awg-proxy-1-update /.test(script));
ok('at 04:30, clear of the 04:00 ru-daily job',
   /name=awg-proxy-1-update on-event="\/system\/script\/run awg-proxy-1-update" start-time=04:30:00 interval=1d/.test(script));
// device-mode can forbid schedulers; a bare add would abort the install script.
ok('the scheduler add is guarded by device-mode',
   /:do \{ :set schedOk \[\/system\/device-mode\/get scheduler\] \} on-error=\{\}/.test(script));

const noSched = generateWith(false);
ok('unticked: no scheduler',
   !/\/system\/scheduler\/add name=awg-proxy-1-update /.test(noSched));
ok('but the update script is still installed',
   /\/system\/script\/add name=awg-proxy-1-update /.test(noSched));
ok('and the next steps still point at it',
   noSched.indexOf('/system/script/run awg-proxy-1-update') >= 0);

/* ---- uninstall takes both back out ---- */
// These two used to sit inside the non-tunnel branch, which would have stranded
// the scheduler on a plain tunnel install.
const uninstall = w.buildUninstallScriptSource(false, 'disk1', 'awg-proxy-1',
                                               w.getTunnelNetwork('awg-proxy-1'), 'tunnel');
ok('tunnel scenario removes the scheduler',
   uninstall.indexOf('/system/scheduler/remove [find where comment~"awg-proxy-1"]') >= 0);
ok('tunnel scenario removes the update script',
   uninstall.indexOf('/system/script/remove [find where comment~"awg-proxy-1" and name!="awg-proxy-1-uninstall"]') >= 0);

const srvUninstall = w.buildServerUninstallSource('awg-proxy-1', 'disk1',
                                                  w.getTunnelNetwork('awg-proxy-1'));
ok('server uninstall removes them too',
   srvUninstall.indexOf('/system/scheduler/remove [find where comment~"awg-proxy-1"]') >= 0 &&
   srvUninstall.indexOf('/system/script/remove [find where comment~"awg-proxy-1"') >= 0);

const s2sUninstall = w.buildS2SUninstallSource('awg-proxy-1', 'disk1',
                                               w.getTunnelNetwork('awg-proxy-1'), false);
ok('site-to-site uninstall removes them too',
   s2sUninstall.indexOf('/system/scheduler/remove [find where comment~"awg-proxy-1"]') >= 0 &&
   s2sUninstall.indexOf('/system/script/remove [find where comment~"awg-proxy-1"') >= 0);

/* ---- external storage keeps its tmpdir handling ---- */
const usb = w.buildImageSetupLines('awg-proxy-1', 'usb1', {}).join(NL);
ok('usb install points tmpdir at the stick',
   usb.indexOf('/container/config set tmpdir=usb1/pull memory-high=200M') >= 0);
ok('and the container root-dir lives there',
   usb.indexOf(':local rootDir "usb1/awg-proxy-1"') >= 0);
ok('usb also accepts an archive sitting in the stick root',
   usb.indexOf(':local altPath ("usb1/" . $file)') >= 0);
ok('the registry branch is generated for usb too',
   /\/container\/add remote-image=\$img /.test(usb));

const auto = w.buildImageSetupLines('awg-proxy-1', '__auto', {}).join(NL);
ok('auto storage still picks the disk at runtime',
   auto.indexOf(':local rootDir ($awgDisk . "/awg-proxy-1")') >= 0);

/* ---- one generator, not two copies ---- */
// buildCommands and the server/S2S generators used to carry a verbatim copy each.
eq('only one place fetches the release archive',
   (html.match(/tool\/fetch url=\$url dst-path=\$filePath/g) || []).length, 1);

/* ---------------------------------------------- the tests stay offline */
// Not a promise in a comment: if the page ever grows a fetch() or a CDN <script>,
// these fail instead of quietly making network calls wherever the tests run.
ok('no network call was made while running the page', netCalls.length === 0,
   netCalls.join(', '));
ok('the page pulls in no external subresources',
   !/<script[^>]+src=|<link[^>]+href="https?:|@import\s+url\(https?:/i.test(html));
/* i18n values are applied with textContent, so markup inside one shows up as
 * literal <strong> on the page. The English fallback in the HTML may carry tags
 * (it is never displayed once a language is applied); the string table may not. */
['ru', 'en'].forEach(function (lang) {
    w.setLang(lang);
    const bad = [];
    w.document.querySelectorAll('[data-i18n]').forEach(function (el) {
        if (/<(strong|code|b|i|br|em)/i.test(el.textContent)) bad.push(el.getAttribute('data-i18n'));
    });
    ok('no literal markup on the page in ' + lang, bad.length === 0, bad.join(', '));
});
w.setLang('ru');

ok('and no runtime network APIs are used in its source',
   !/\bfetch\s*\(|XMLHttpRequest|navigator\.sendBeacon|new\s+WebSocket/.test(html));

console.log('');
console.log(passes + '/' + (passes + fails) + ' checks passed');
process.exit(fails ? 1 : 0);
