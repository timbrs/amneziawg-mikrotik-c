/* Привилегированный режим контейнера: он должен включаться — и не ломать старые
 * RouterOS. Гоняется через jsdom.
 *
 * Зачем он вообще. Приёмный буфер UDP-сокета внутри контейнера упирается в
 * 416 КБ: это около миллисекунды трафика, и на отдаче пакеты выбрасывает ядро
 * ещё до того, как прокси успеет их прочитать. Поднять потолок изнутри нельзя —
 * каталога /proc/sys/net/core в контейнере нет вовсе. Привилегированный режим
 * (RouterOS 7.24+) снимает ровно одну проверку: вызов SO_RCVBUFFORCE начинает
 * работать, и буфер вырастает до 32 МБ.
 *
 * Чем это опасно и что здесь закреплено:
 *   - параметра privileged нет до 7.24, и если передать его прямо в
 *     /container/add, установка на 7.20-7.23 упадёт целиком. Поэтому он идёт
 *     отдельной строкой через [:parse] внутри :do/on-error — старая версия
 *     просто пропустит её;
 *   - пересборка контейнера не должна терять режим, иначе буфер молча вернётся
 *     к 416 КБ;
 *   - уже установленным он включается ОДИН раз: /container/set перезапускает
 *     контейнер, и делать это каждую ночь значит рвать туннель каждую ночь.
 *
 * Только для разработки: у самого прокси зависимостей нет, а здесь нужна одна.
 *   npm install jsdom && node tests/conf3.0-privileged.test.js
 * Возвращает ненулевой код, если что-то сломано.
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

const need = ['buildImageSetupLines', 'buildUpdateScriptSource'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 privileged-mode tests ===');

const P = 'awg-proxy-1';
const install = w.buildImageSetupLines(P, 'disk1', {}).join(NL);
const upd = w.buildUpdateScriptSource(P);

/* ------------------------------------------------- 1. установка */

ok('установка включает привилегированный режим',
   /\/container\/set \[find where interface=veth-awg-proxy-1\] privileged=yes/.test(install));

// Главная ловушка: на 7.20-7.23 параметра нет, и неизвестный аргумент в add
// обрывает весь скрипт. [:parse] откладывает разбор до выполнения, on-error
// проглатывает отказ.
const privLine = install.split(NL).filter(l => l.indexOf('privileged=yes') >= 0);
ok('включение обёрнуто в :parse и on-error — старые RouterOS не сломаются',
   privLine.length > 0 && privLine.every(l =>
       l.indexOf('[:parse') >= 0 && l.indexOf('on-error={}') >= 0),
   privLine.join(' | '));

ok('privileged не передаётся аргументом /container/add',
   !/\/container\/add[^\n]*privileged=/.test(install),
   install.split(NL).filter(l => l.indexOf('/container/add') >= 0).join(' | '));

// Контейнер должен получить режим до запуска, иначе первый старт пройдёт со
// старым потолком буфера и понадобится лишний перезапуск.
const setAt = install.indexOf('privileged=yes');
const startAt = install.indexOf('/container/start');
ok('режим выставляется до запуска контейнера', setAt > 0 && startAt > setAt,
   'set=' + setAt + ' start=' + startAt);

/* ------------------------------------------------- 2. пересборка */

ok('пересборка контейнера не теряет привилегированный режим',
   /\/container\/set \[find where interface=veth-awg-proxy-1\] privileged=yes/.test(upd));

/* ------------------------------------------------- 3. миграция уже стоящих */

ok('скрипт обновления читает текущее состояние режима',
   /:local priv true/.test(upd) && /\/container\/get \$cid privileged/.test(upd));

ok('включение происходит только когда режим ещё не включён',
   /:if \(\$priv != true && \$wantRunning && \$healthy\) do=\{/.test(upd));

// /container/set перезапускает контейнер сам, но неуправляемо. Останов с
// ожиданием делает разрыв предсказуемым и коротким.
const migAt = upd.indexOf(':local priv true');
const migStop = upd.indexOf('/container/stop $cid', migAt);
const migSet = upd.indexOf('privileged=yes', migAt);
ok('перед сменой режима контейнер останавливается',
   migAt > 0 && migStop > migAt && migSet > migStop,
   'миграция=' + migAt + ' stop=' + migStop + ' set=' + migSet);

ok('чтение состояния тоже защищено от старых RouterOS',
   /:do \{ :set priv \[\[:parse ":return \[\/container\/get \$cid privileged\]"\]\] \} on-error=\{\}/.test(upd));

// Если после включения контейнер не поднялся, дальше по скрипту должно идти
// обычное лечение, а не рапорт об успехе.
ok('неудачный запуск после смены режима помечает контейнер больным',
   /:do \{ :if \(\[\/container\/get \$cid stopped\] = true\) do=\{ :set healthy false \} \} on-error=\{\}/
       .test(upd.slice(migAt)));

ok('скрипт обновления сообщает о разовом включении в лог',
   /:log info "awg-proxy-1: enabling privileged mode"/.test(upd));

/* ------------------------------------------------- 4. ничего лишнего */

ok('на страницу не приходит ни одного сетевого вызова', netCalls.length === 0,
   netCalls.join(', '));

console.log('');
console.log(passes + '/' + (passes + fails) + ' checks passed');
process.exit(fails ? 1 : 0);
