/* Разбор версии RouterOS в сгенерированных скриптах (#70).
 *
 * Скрипт брал всё между первой точкой и следующей точкой/пробелом и делал
 * [:tonum]. На "7.25beta5 (testing)" это "25beta5" -> пусто, minor считался
 * меньше 20, и бета получала образ 7.20-Docker вместо OCI. Теперь minor - это
 * ведущие цифры после первой точки.
 *
 * Скрипт RouterOS здесь не запустить, поэтому проверяются две вещи:
 *   - сгенерированный текст (во всех местах, где разбирается версия) совпадает
 *     со строками minorParseLines и не содержит старого разбора;
 *   - эти пять строк построчно повторены в JS (rosMinor) и прогнаны по
 *     реальным форматам строки версии.
 *
 * Dev-only:
 *   npm install jsdom && node tests/conf3.0-version.test.js
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

const need = ['minorParseLines', 'imageFileDetectLines', 'buildUpdateScriptSource', 'buildDiagnosticScript', 'buildOfflinePrepScript'];
const missing = need.filter(n => typeof w[n] !== 'function');
if (missing.length) {
    console.log('  FAIL  functions not reachable: ' + missing.join(', '));
    process.exit(1);
}

console.log('=== conf3.0 RouterOS version parsing tests ===');

/* Те же пять строк, что генерирует minorParseLines, по одной на оператор. */
function rosMinor(ver) {
    const dotPos = ver.indexOf('.');                       // :find $ver "."
    const rest = ver.slice(dotPos + 1) + ' ';              // [:pick ...] . " "
    let minor = 0, mi = 0;
    while ('0123456789'.indexOf(rest.charAt(mi)) >= 0) {   // [:find "0123456789" <char>] != nil
        minor = minor * 10 + Number(rest.charAt(mi));
        mi++;
    }
    return minor;
}

const lines = w.minorParseLines('  ');
eq('пять строк разбора', lines.length, 5);
ok('строки разбора совпадают с эмуляцией',
   lines[0] === '  :local dotPos [:find $ver "."]' &&
   lines[1] === '  :local rest ([:pick $ver ($dotPos + 1) [:len $ver]] . " ")' &&
   lines[2] === '  :local minor 0' &&
   lines[3] === '  :local mi 0' &&
   lines[4].indexOf('  :while ([:typeof [:find "0123456789" [:pick $rest $mi ($mi + 1)]]] != "nil") do={') === 0 &&
   lines[4].indexOf(':set minor ($minor * 10 + [:tonum [:pick $rest $mi ($mi + 1)]]); :set mi ($mi + 1) }') > 0,
   lines.join(NL));

/* Реальные форматы строки /system/resource/get version -> minor и формат образа. */
const cases = [
    ['7.21.3 (stable)',      21, 'oci'],
    ['7.25beta5 (testing)',  25, 'oci'],
    ['7.22rc1 (testing)',    22, 'oci'],
    ['7.22beta1 (testing)',  22, 'oci'],
    ['7.20.8 (long-term)',   20, 'classic'],
    ['7.20beta3 (testing)',  20, 'classic'],
    ['7.22',                 22, 'oci'],
    ['7.21 (stable)',        21, 'oci'],
    ['7.19.4 (stable)',      19, 'classic'],
    ['7.9 (stable)',          9, 'classic'],
    ['7.24.2 (stable)',      24, 'oci'],
    ['7.100rc2 (testing)',  100, 'oci']
];
cases.forEach(function (c) {
    const m = rosMinor(c[0]);
    eq('minor из "' + c[0] + '"', m, c[1]);
    // граница формата: <= 20 - classic Docker (-7.20-Docker), 21+ - OCI
    eq('образ для "' + c[0] + '"', m <= 20 ? 'classic' : 'oci', c[2]);
});

/* В сгенерированных скриптах нет старого разбора, и новый стоит на месте. */
const detect = w.imageFileDetectLines('  ').join(NL);
ok('установка/предпроверка образа: новый разбор', detect.indexOf(lines.join(NL)) >= 0, detect);
ok('образ выбирается по <= 20', detect.indexOf(':if ($minor <= 20) do={ :set suffix "-7.20-Docker" }') >= 0);

const upd = w.buildUpdateScriptSource('awg-proxy-1');
ok('ночное обновление: новый разбор', upd.indexOf(w.minorParseLines('    ').join(NL)) >= 0, upd);

const pre = w.buildDiagnosticScript();
ok('предпроверка: новый разбор', pre.indexOf(w.minorParseLines('').join(NL)) >= 0, pre.slice(0, 400));

[['установка', detect], ['обновление', upd], ['предпроверка', pre], ['оффлайн-подготовка', w.buildOfflinePrepScript('awg-proxy-1', 'disk1', true)]].forEach(function (p) {
    ok(p[0] + ': старого разбора по точке/пробелу нет',
       p[1].indexOf('endPos') < 0 && p[1].indexOf('[:tonum [:pick $rest 0') < 0);
});

/* На всей странице не осталось ни одного [:tonum] от куска версии. */
ok('в источнике страницы нет endPos', html.indexOf('endPos') < 0);

console.log('');
console.log(passes + '/' + (passes + fails) + ' checks passed');
process.exit(fails ? 1 : 0);
