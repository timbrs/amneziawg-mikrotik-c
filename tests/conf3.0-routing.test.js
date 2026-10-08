/* Two gaps in DNS-based routing:
 *
 *   #41  Telegram is reached over MTProto, straight to Telegram's own IP
 *        ranges. RouterOS never sees a DNS query for it, so a domain-only rule
 *        leaves the address-list empty and the traffic bypasses the tunnel.
 *   #49  CDN answers carry very short TTLs, so an address can expire out of the
 *        list while a browser is still using it.
 *
 * Dev-only:
 *   npm install jsdom && node tests/conf3.0-routing.test.js
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

const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' });
const w = dom.window;

const need = ['buildDnsRoutingScenario', 'buildNonRuScenario', 'buildIpEntries',
              'extraTimeEnabled', 'extraTimeValue', 'extraTimeSetLines',
              'extraTimeResetLines', 'buildUninstallScriptSource'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 DNS-routing tests ===');

const extraBox = w.document.getElementById('extratime-enable');
const extraVal = w.document.getElementById('extratime-value');
function dnsFwd(services) {
    return w.buildDnsRoutingScenario('awg-proxy-1', services || ['telegram'], 'CloudFlare').join(NL);
}

/* ---- #41: Telegram needs its ranges seeded ---- */
const tg = dnsFwd(['telegram']);
const TG_RANGES = ['91.108.4.0/22', '91.108.8.0/22', '91.108.12.0/22', '91.108.16.0/22',
                   '91.108.20.0/22', '91.108.56.0/22', '149.154.160.0/20', '185.76.151.0/24'];
TG_RANGES.forEach(function (r) {
    ok('telegram range ' + r + ' is seeded', tg.indexOf('"' + r + '"') >= 0);
});
ok('the ranges land in the Telegram list',
   tg.indexOf('/ip/firewall/address-list/add list=Telegram address=$net') >= 0, tg);
ok('and are tagged so uninstall removes them',
   tg.indexOf('comment=awg-proxy-1-excl-ip') >= 0, tg);
ok('the domains are still forwarded too', tg.indexOf('address-list=Telegram') >= 0 &&
   tg.indexOf('type=FWD') >= 0);

/* A service with no bare-IP problem must not grow a static block. */
const yt = dnsFwd(['youtube']);
ok('youtube gets no static ranges', yt.indexOf('address-list/add list=YouTube') < 0, yt);
ok('and no empty static section either',
   yt.indexOf('Static IP ranges for services that bypass DNS') < 0, yt);

/* ---- #41: the caveat is stated where it is read ---- */
ok('the script says clients must use the router for DNS',
   yt.indexOf('Clients must use this router as their DNS server') >= 0, yt.slice(0, 400));
ok('and names the ways it silently fails',
   yt.indexOf('Private DNS') >= 0 && yt.indexOf('cached address') >= 0, yt.slice(0, 600));
ok('and says how to check', yt.indexOf('/ip/firewall/address-list/print where list=') >= 0, yt);

/* ---- #49: opt-in, never clobbers ---- */
extraBox.checked = false;
ok('off by default', w.extraTimeEnabled() === false);
ok('and emits nothing', w.extraTimeSetLines('awg-proxy-1').length === 0);
ok('nor an uninstall step', w.extraTimeResetLines('  ').length === 0);
ok('routing script is untouched when off', dnsFwd().indexOf('address-list-extra-time') < 0);

extraBox.checked = true;
extraVal.value = '30m';
const set = w.extraTimeSetLines('awg-proxy-1').join(NL);
ok('reads the current value first', set.indexOf('[/ip/dns/get address-list-extra-time]') >= 0, set);
ok('only writes when the router is at the default', set.indexOf(':if ($cur = 0s) do={') >= 0, set);
ok('writes the chosen value', set.indexOf('/ip/dns/set address-list-extra-time=30m') >= 0, set);
ok('says so when it leaves an existing value alone',
   set.indexOf('left alone') >= 0, set);

const reset = w.extraTimeResetLines('  ').join(NL);
ok('uninstall compares before restoring',
   reset.indexOf(':if ([/ip/dns/get address-list-extra-time] = 30m) do={') >= 0, reset);
ok('and restores the RouterOS default', reset.indexOf('address-list-extra-time=0s') >= 0, reset);

/* A value the user changed by hand after install must survive an uninstall,
 * which is exactly what comparing before restoring buys. */
extraVal.value = '1h';
ok('the compared value follows the field',
   w.extraTimeResetLines('  ').join(NL).indexOf('= 1h)') >= 0);
extraVal.value = 'nonsense';
ok('a nonsense value falls back to 30m', w.extraTimeValue() === '30m');
extraVal.value = '30m';

/* ---- #49 end to end ---- */
ok('dns-fwd applies it', dnsFwd().indexOf('/ip/dns/set address-list-extra-time=30m') >= 0);
const nru = w.buildNonRuScenario('awg-proxy-1', ['steam'], 'CloudFlare', '198.51.100.1', 'disk1', 'container').join(NL);
ok('non-ru applies it too', nru.indexOf('/ip/dns/set address-list-extra-time=30m') >= 0);

const un = w.buildUninstallScriptSource(true, 'disk1', 'awg-proxy-1',
                                        w.getTunnelNetwork('awg-proxy-1'), 'dns-fwd');
ok('uninstall carries the conditional reset',
   un.indexOf('address-list-extra-time] = 30m') >= 0 &&
   un.indexOf('address-list-extra-time=0s') >= 0, un.slice(0, 300));

extraBox.checked = false;
const unOff = w.buildUninstallScriptSource(true, 'disk1', 'awg-proxy-1',
                                           w.getTunnelNetwork('awg-proxy-1'), 'dns-fwd');
ok('and nothing when the option was never used',
   unOff.indexOf('address-list-extra-time') < 0);

/* ---- #67: исключения non-ru ---- */
const nexL = w.buildNonRuScenario('awg-proxy-1', ['steam', 'epic'], 'CloudFlare', '198.51.100.1', 'disk1', 'container');
function idx(re) { for (let i = 0; i < nexL.length; i++) if (re.test(nexL[i])) return i; return -1; }
const iSteam = idx(/mangle.add .*action=accept dst-address-list=Steam /);
const iEpic = idx(/mangle.add .*action=accept dst-address-list=EpicGames /);
const iMarkConn = idx(/mangle.add .*action=mark-connection/);
const iMarkRoute = idx(/mangle.add .*action=mark-routing/);
ok('exclusion accept rules exist', iSteam >= 0 && iEpic >= 0);
ok('accept stands above mark-connection and mark-routing',
   iSteam < iMarkConn && iEpic < iMarkConn && iMarkConn < iMarkRoute);
const iClean = idx(/mangle.remove \[find where comment=awg-proxy-1-non-ru\]/);
ok('a re-run first drops its own earlier mangle rules', iClean >= 0 && iClean < iSteam, String(iClean));
ok('and its earlier masquerade', idx(/nat.remove \[find where comment=awg-proxy-1-non-ru\]/) >= 0);
ok('the cleanup is exact-match, not a prefix wildcard',
   !/mangle.remove .*comment~/.test(nexL.join(NL)));
const nexS = nexL.join(NL);
['steamcontent.com', 'steamserver.net', 'steam-chat.com'].forEach(function (d) {
    ok('Steam exclusion forwards ' + d, nexS.indexOf('"' + d + '"') >= 0);
});

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
