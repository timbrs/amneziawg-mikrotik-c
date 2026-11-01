/* Two failure modes that both end in a half-applied RouterOS configuration:
 *
 *   #17  policy routing matches in-interface-list=LAN, which does not exist on
 *        every router (CHR especially). The add fails, the script carries on,
 *        and the install looks complete but routes nothing.
 *   #64  the same for WAN, plus place-before=0 on an empty /ip/firewall/filter,
 *        which RouterOS rejects outright.
 *
 * The invariants: whatever a script depends on is checked before it changes
 * anything, and no rule depends on the firewall table already being populated.
 *
 * Dev-only:
 *   npm install jsdom && node tests/conf3.0-preflight.test.js
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
// Site-to-site is hidden in the configurator; its checks are kept as they
// were, wrapped in skip() and not run. TODO: if nobody asks for the mode back
// by the end of 2026, delete them in 2027 together with it.
const SITE_TO_SITE_HIDDEN = 'site-to-site is hidden in the configurator';
function skip(name, why, checks) { console.log('  SKIP  ' + name + ': ' + why); }

const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' });
const w = dom.window;

const need = ['interfaceListPreflightLines', 'firewallTopRule', 'buildDnsRoutingScenario',
              'buildNonRuScenario', 'buildServerCommands', 'buildSiteToSiteCommands',
              'buildV6ForceIpv4', 'generateAWGParams'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 preflight / rule-order tests ===');

/* A RouterOS line that changes state. Quoted strings are blanked first, so the
 * help text inside the preflight (:put "  /interface/list/add name=LAN") does
 * not count as a mutation. */
function mutates(line) {
    const bare = line.replace(/"[^"]*"/g, '""');
    return /\/(ip|ipv6|interface|container|system|routing|disk|file|tool|certificate)\/[a-z0-9/-]*\/(add|set|remove|move|enable|disable|format-drive)(\s|$)/.test(bare);
}
function firstMutation(text) {
    const lines = text.split(NL);
    for (let i = 0; i < lines.length; i++) if (mutates(lines[i])) return i;
    return -1;
}
function firstCheckOf(text, name) {
    const lines = text.split(NL);
    for (let i = 0; i < lines.length; i++)
        if (lines[i].indexOf('/interface/list/find where name="' + name + '"') >= 0) return i;
    return -1;
}

/* Any script that leans on an interface list has to check it first. */
function assertGuarded(label, text) {
    ['LAN', 'WAN'].forEach(function (list) {
        const used = text.indexOf('interface-list=' + list) >= 0;
        if (!used) return;
        const chk = firstCheckOf(text, list);
        const mut = firstMutation(text);
        ok(label + ': uses ' + list + ' and checks it', chk >= 0,
           text.split(NL).slice(0, 6).join(' | '));
        ok(label + ': the ' + list + ' check comes before the first change',
           chk >= 0 && (mut < 0 || chk < mut),
           'check@' + chk + ' mutation@' + mut + ' -> ' + (mut >= 0 ? text.split(NL)[mut] : ''));
    });
    // place-before=0 is fine, but only inside the non-empty guard right above it.
    const ls = text.split(NL);
    const unguarded = ls.filter(function (l, i) {
        if (l.indexOf('place-before=0') < 0) return false;
        for (var k = i - 1; k >= 0 && k >= i - 2; k--)
            if (ls[k].indexOf(':if ([:len [') >= 0 && ls[k].indexOf('/find]] > 0) do={') >= 0) return false;
        return true;
    });
    ok(label + ': no place-before=0 outside a non-empty guard', unguarded.length === 0,
       unguarded[0] || '');
}

/* ---- helper behaviour ---- */
const pf = w.interfaceListPreflightLines(['WAN']).join(NL);
ok('preflight tests for existence', pf.indexOf('[:len [/interface/list/find where name="WAN"]] = 0') >= 0, pf);
ok('preflight aborts rather than warning', pf.indexOf(':error') >= 0, pf);
ok('preflight says nothing was changed', pf.indexOf('nothing has been changed') >= 0, pf);
ok('preflight tells the user how to fix it', pf.indexOf('/interface/list/add name=WAN') >= 0, pf);
ok('preflight makes no changes of its own', firstMutation(pf) < 0,
   pf.split(NL)[firstMutation(pf)] || '');

const top = w.firewallTopRule('/ip/firewall/filter', 'chain=input action=accept', 'tag-1');
ok('top rule checks the table is not empty first',
   top.indexOf(':if ([:len [/ip/firewall/filter/find]] > 0) do={') >= 0, top);
ok('top rule uses place-before only in that branch',
   top.indexOf('comment=tag-1 place-before=0') >= 0, top);
ok('top rule has a plain add for the empty table',
   top.split('} else={')[1].indexOf('/ip/firewall/filter/add ') >= 0, top);
ok('the empty-table branch carries no place-before',
   top.split('} else={')[1].indexOf('place-before') < 0, top);
ok('top rule carries its comment in both branches',
   (top.match(/comment=tag-1/g) || []).length === 2, top);
// /move is not an option here: with fasttrack on, index 0 is a dynamic builtin
// and RouterOS answers "cannot move builtin" (seen on 7.24.2).
ok('the rule is never repositioned with /move', top.indexOf('/move ') < 0, top);

/* ---- the routing scenarios (#17) ---- */
assertGuarded('dns-fwd', w.buildDnsRoutingScenario('awg-proxy-1', ['youtube'], 'CloudFlare').join(NL));
assertGuarded('non-ru', w.buildNonRuScenario('awg-proxy-1', [], 'CloudFlare', '198.51.100.1', 'disk1', 'container').join(NL));

/* ---- server / hub mode (#64) ---- */
const ap = w.generateAWGParams('v3');
ap.port = 30892;
ap.wgListenPort = 40001;
const srv = w.buildServerCommands('198.51.100.1', ap, '10.182.242.0/24', 'disk1', 'disk1',
                                  'awg-server-1', '', '', 'awg', 1, '1.1.1.1');
assertGuarded('server', srv.server);
assertGuarded('server client leg', srv.client);

/* ---- site to site ---- */
skip('site-to-site A and B', SITE_TO_SITE_HIDDEN, function () {
    const s2sParams = w.generateAWGParams('v2');
    s2sParams.port = 30893;
    s2sParams.wgListenPort = 40002;
    const s2s = w.buildSiteToSiteCommands('198.51.100.2', '10.99.99.0/30', s2sParams, 'disk1', 'disk1',
                                          'awg-s2s-1', '1.1.1.1', '', '');
    assertGuarded('site-to-site A', s2s.sideA);
    assertGuarded('site-to-site B', s2s.sideB);
});

/* ---- normal install, IPv6 leg uses out-interface-list=WAN ---- */
const v6 = w.containerV6Lines('awg-proxy-1', w.getTunnelNetwork6('awg-proxy-1'), true).join(NL);
ok('ipv6 container leg guards its top rule too',
   v6.indexOf(':if ([:len [/ipv6/firewall/filter/find]] > 0) do={') >= 0, v6);
ok('and has a plain add for an empty ipv6 filter table',
   v6.split('} else={')[1] && v6.split('} else={')[1].indexOf('/ipv6/firewall/filter/add ') >= 0, v6);

/* ---- rules still end up on top ---- */
const srvFilterAdds = srv.server.split(NL).filter(function (l) {
    return /^\s*\/ip\/firewall\/filter\/add /.test(l);
});
ok('server still installs its accept rules', srvFilterAdds.length >= 6, String(srvFilterAdds.length));
// Every top rule appears twice: once with place-before, once without, under a
// non-empty check. Neither copy may be left unguarded.
const guarded = (srv.server.match(/:if \(\[:len \[\/ipv?6?\/?firewall\/filter\/find\]\] > 0\) do=\{/g) || []).length;
ok('each top rule is guarded by a non-empty check', guarded >= 3, String(guarded));
ok('no place-before survives outside a guard',
   srv.server.split(NL).every(function (l, i, all) {
       if (l.indexOf('place-before=0') < 0) return true;
       for (var k = i - 1; k >= 0 && k >= i - 3; k--)
           if (all[k].indexOf(':if ([:len [') >= 0) return true;
       return false;
   }), 'an unguarded place-before=0 is present');

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
