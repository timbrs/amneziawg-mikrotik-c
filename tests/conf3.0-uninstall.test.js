/* Что удаление одной установки НЕ имеет права трогать. Через jsdom.
 *
 * Один роутер держит несколько установок сразу, и часть объектов на нём — общие.
 * Самый дорогой из них — address-list RU: его наполняет <prefix>-ru-update, а
 * смотрят на него правила всех установок (dst-address-list=!RU). Стоит одному
 * uninstall его снести — и у всех остальных весь российский трафик уезжает в
 * туннель, причём восстановить список сможет только тот, у кого остался скрипт
 * обновления. Ровно это и случилось на живом роутере: игровой трафик ушёл в VPN,
 * а на втором роутере скрипта не оказалось вовсе.
 *
 * Dev-only: npm install jsdom && node tests/conf3.0-uninstall.test.js
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
// Site-to-site спрятан в конфигураторе; его проверки оставлены как были,
// обёрнуты в skip() и не запускаются. TODO: если до конца 2026 года режим
// никто не попросит вернуть, в 2027 удалить их вместе с ним.
const SITE_TO_SITE_HIDDEN = 'site-to-site спрятан в конфигураторе';
function skip(name, why, checks) { console.log('  SKIP  ' + name + ': ' + why); }

const w = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' }).window;
const need = ['buildUninstallScriptSource', 'buildStandaloneUninstallScriptSource',
              'buildServerUninstallSource', 'buildS2SUninstallSource', 'getTunnelNetwork'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) { console.log('  FAIL  не найдены: ' + missing.join(', ')); process.exit(1); }

console.log('=== conf3.0 uninstall: не трогать чужое ===');

const P = 'awg-proxy-1';
const net = w.getTunnelNetwork(P);
const sources = {
    'клиент (non-ru)':   w.buildUninstallScriptSource(true, 'disk1', P, net, 'non-ru'),
    'клиент (dns-fwd)':  w.buildUninstallScriptSource(true, 'disk1', P, net, 'dns-fwd'),
    'клиент (tunnel)':   w.buildUninstallScriptSource(false, 'disk1', P, net, 'tunnel'),
    'standalone':        w.buildStandaloneUninstallScriptSource(true, P, 'non-ru'),
    'сервер':            w.buildServerUninstallSource(P, 'disk1', net),
};
skip('site-to-site', SITE_TO_SITE_HIDDEN, function () {
    sources['site-to-site'] = w.buildS2SUninstallSource(P, 'disk1', net, false);
});

for (const [label, src] of Object.entries(sources)) {
    // Общий список RU не принадлежит установке — трогать его нельзя.
    ok(label + ': не сносит общий список RU',
       !/address-list\/remove \[find where list=RU and comment=RIPE-RU\]/.test(src),
       (src.split(NL).find(l => l.indexOf('list=RU') >= 0) || '').trim().slice(0, 90));
    // Заодно: никаких массовых удалений без привязки к префиксу.
    const wide = src.split(NL).filter(l =>
        /\/(ip|routing|system|tool|interface)\/[a-z/-]*remove \[find\]/.test(l));
    ok(label + ': нет удалений [find] без условия', wide.length === 0, wide.join(' | ').slice(0, 120));
}

// Свои записи в списке RU (их ставит установка, помечая префиксом) убирать можно и нужно.
const cli = sources['клиент (non-ru)'];
ok('свои записи с префиксом всё же убираются',
   cli.indexOf('comment~"' + P + '-private"') >= 0);
ok('и свой vpn-server список тоже',
   cli.indexOf('list="' + P + '-vpn-server"') >= 0);

// А наполнение списка RU — дело скрипта обновления, там очистка перед импортом законна.
const w2 = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/' }).window;
w2.clearAll();
const ta = w2.document.getElementById('conf-input');
ta.value = [
    '[Interface]', 'PrivateKey = ' + 'A'.repeat(43) + '=', 'Address = 10.13.13.2/32', 'DNS = 1.1.1.1',
    'Jc = 4', 'Jmin = 40', 'Jmax = 70', 'S1 = 30', 'S2 = 40',
    'H1 = 1111111111', 'H2 = 2222222222', 'H3 = 3333333333', 'H4 = 444444444', '',
    '[Peer]', 'PublicKey = ' + 'B'.repeat(43) + '=', 'Endpoint = 198.51.100.1:443', 'AllowedIPs = 0.0.0.0/0'
].join(NL);
ta.dispatchEvent(new w2.Event('input', { bubbles: true }));
[...w2.document.getElementsByName('scenario')].forEach(r => {
    r.checked = (r.value === 'non-ru');
    r.dispatchEvent(new w2.Event('change', { bubbles: true }));
});
w2.generate();
const routing = (w2.document.getElementById('routing-output') || {}).dataset
    ? w2.document.getElementById('routing-output').dataset.plain || '' : '';
ok('скрипт обновления списка по-прежнему чистит его перед импортом',
   routing.indexOf('list=RU and comment=RIPE-RU') >= 0);
ok('и сам список наполняет он же',
   routing.indexOf('-ru-update') >= 0);

console.log('');
console.log(passes + '/' + (passes + fails) + ' checks passed');
process.exit(fails ? 1 : 0);
