/* Вопрос про мощность роутера и проверка роутера перед установкой. Через jsdom.
 *
 * Две вещи, которые здесь закреплены.
 *
 * НАСТРОЙКИ ПРОИЗВОДИТЕЛЬНОСТИ. Их три и они имеют смысл только вместе:
 * по ядру каждому рабочему потоку, приоритет этим потокам и раскладка разбора
 * входящих пакетов по ОСТАЛЬНЫМ ядрам. Последнее — главная ловушка: если маска
 * ядер для приёма пересекается с теми, где сидят потоки, выигрыш съедается
 * целиком. Потоки стоят на ядрах 1 и 2, значит приёму остаются 0 и 3, то есть
 * маска 9. Поэтому тест проверяет не только наличие настроек, но и то, что
 * маска с пинами не пересекается.
 *
 * ПРОВЕРКА РОУТЕРА. Её единственная ценность в том, что она НИЧЕГО не меняет:
 * человек запускает её на чужом рабочем роутере до установки. Команды-советы
 * она печатает, а не выполняет. Отличить одно от другого просто: вырезаем из
 * скрипта всё, что в кавычках, и смотрим, не осталось ли изменяющих команд.
 *
 * Только для разработки: у самого прокси зависимостей нет, а здесь нужна одна.
 *   npm install jsdom && node tests/conf3.0-perfcheck.test.js
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

const need = ['buildDiagnosticScript', 'perfMode', 'generate', 'clearAll'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 perf-mode and router-check tests ===');

/* ------------------------------------------------- 1. вопрос про мощность */

function conf() {
    return ['[Interface]',
        'PrivateKey = ' + 'A'.repeat(43) + '=',
        'Address = 10.9.0.2/32',
        'DNS = 1.1.1.1',
        'Jc = 4', 'Jmin = 40', 'Jmax = 70',
        'S1 = 98', 'S2 = 131',
        'H1 = 1245842713', 'H2 = 2087463251', 'H3 = 3145627891', 'H4 = 4012783461',
        '[Peer]',
        'PublicKey = ' + 'B'.repeat(43) + '=',
        'AllowedIPs = 0.0.0.0/0',
        'Endpoint = 198.51.100.7:51833'].join(NL);
}

function generateWith(perf) {
    w.clearAll();
    const ta = w.document.getElementById('conf-input');
    ta.value = conf();
    ta.dispatchEvent(new w.Event('input', { bubbles: true }));
    const radio = w.document.querySelector('input[name="perf-mode"][value="' + perf + '"]');
    if (!radio) throw new Error('no perf-mode radio with value ' + perf);
    radio.checked = true;
    w.document.getElementById('errors-container').innerHTML = '';
    w.generate();
    const err = w.document.getElementById('errors-container').textContent.trim();
    if (err) throw new Error('generate() reported: ' + err);
    return w.document.getElementById('output').dataset.plain || '';
}

ok('по умолчанию выбран автоматический режим', w.perfMode() === 'auto', w.perfMode());

const auto = generateWith('auto');
ok('в автоматическом режиме настройки под условием о числе ядер',
   /:if \(\[\/system\/resource\/get cpu-count\] >= 4\) do=\{[^}]*AWG_RT/.test(auto),
   auto.split(NL).filter(l => l.indexOf('AWG_RT') >= 0).join(' | '));
ok('и в них входят все четыре переменные',
   ['AWG_CPU_C2S', 'AWG_CPU_S2C', 'AWG_RT', 'AWG_RPS'].every(k => auto.indexOf(k) >= 0));

const strong = generateWith('strong');
ok('для мощного роутера условие не нужно',
   strong.indexOf('AWG_RT') >= 0 && !/cpu-count\] >= 4\) do=\{[^}]*AWG_RT/.test(strong),
   strong.split(NL).filter(l => l.indexOf('AWG_RT') >= 0).join(' | '));

const weak = generateWith('weak');
ok('для слабого роутера настроек нет вовсе',
   weak.indexOf('AWG_RT') < 0 && weak.indexOf('AWG_RPS') < 0 &&
   weak.indexOf('AWG_CPU_C2S') < 0);
ok('и он честно пишет, почему их нет',
   /# Perf: skipped/.test(weak), weak.split(NL).filter(l => /Perf:/.test(l)).join(' | '));

// Главная ловушка: маска приёма не должна накрывать ядра, занятые потоками.
const pins = [];
let m;
const pinRe = /AWG_CPU_[CS]2[SC] value="(\d+)"/g;
while ((m = pinRe.exec(auto)) !== null) pins.push(parseInt(m[1], 10));
const maskM = /AWG_RPS value="([0-9a-f]+)"/.exec(auto);
ok('маска ядер для приёма задана', !!maskM, auto.indexOf('AWG_RPS') >= 0 ? 'есть, но не разобралась' : 'нет');
if (maskM) {
    const mask = parseInt(maskM[1], 16);
    const overlap = pins.filter(c => (mask >> c) & 1);
    ok('маска приёма не пересекается с ядрами рабочих потоков',
       pins.length === 2 && overlap.length === 0,
       'пины=' + pins.join(',') + ' маска=0x' + maskM[1] + ' пересечение=' + overlap.join(','));
}

/* ------------------------------------------------- 2. проверка роутера */

const diag = w.buildDiagnosticScript();

// Вырезаем содержимое кавычек: всё, что внутри, — печатаемый совет, а не команда.
const bare = diag.replace(/"[^"]*"/g, '""');
const mutations = bare.split(NL).filter(l =>
    /\/(set|add|remove|enable|disable|update|reboot|repull|start|stop)\b/.test(l));
ok('проверка не меняет на роутере ничего', mutations.length === 0,
   mutations.slice(0, 3).join(' | '));

ok('команды-советы всё-таки печатаются',
   /\/system\/device-mode\/update container=yes/.test(diag) &&
   /\/interface\/list\/add name=/.test(diag));

const checks = {
    'версию RouterOS': /\/system\/resource\/get version/,
    'порог 7.24 для привилегированного режима': /\$minor < 24/,
    'пакет container': /\/system\/package\/find where name~"container"/,
    'режим устройства': /\/system\/device-mode\/get container/,
    'разрешён ли планировщик': /\/system\/device-mode\/get scheduler/,
    'архитектуру': /architecture-name/,
    'число ядер': /cpu-count/,
    'свободную память': /free-memory/,
    'свободное место': /free-hdd-space/,
    'флешку': /\/disk\/find where type="hardware"/,
    'часы': /\/system\/clock\/get date/,
    'настройки DNS': /\/ip\/dns\/get servers/,
    'работает ли разрешение имён': /:resolve "github\.com"/,
    'списки интерфейсов': /\/interface\/list\/find where name=\$n/,
    'уже установленные контейнеры': /\/container\/find/,
};
Object.keys(checks).forEach(function (what) {
    ok('проверяет ' + what, checks[what].test(diag));
});

ok('каждая находка помечена так, что её видно', /Lines marked ! need your attention/.test(diag));
ok('в конце подводится итог', /Everything is ready/.test(diag) && /to fix first/.test(diag));

// Всё, что читается у старых RouterOS может отсутствовать, должно быть в :do/on-error,
// иначе проверка упадёт ровно там, где она нужнее всего.
['device-mode/get container', 'device-mode/get scheduler', 'system/package/find'].forEach(function (f) {
    const line = diag.split(NL).filter(l => l.indexOf(f) >= 0)[0] || '';
    ok('чтение "' + f + '" защищено от старых версий',
       line.indexOf(':do {') >= 0 && line.indexOf('on-error={}') >= 0, line);
});

ok('кнопка проверки есть на странице', !!w.document.getElementById('btn-diag'));
ok('и ей есть куда писать', !!w.document.getElementById('diag-output'));

ok('на страницу не приходит ни одного сетевого вызова', netCalls.length === 0,
   netCalls.join(', '));

console.log('');
console.log(passes + '/' + (passes + fails) + ' checks passed');
process.exit(fails ? 1 : 0);
