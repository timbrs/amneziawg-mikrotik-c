/* "First, prepare the router": the three things RouterOS needs before any
 * generated command can run sit at the top of the page, above the mode choice.
 *
 * People used to paste the .conf, run the commands and only then find out
 * that the container package or device-mode was missing, because that lived
 * in the README's quick start. The invariants: the card comes before the
 * config card, carries the exact device-mode command, is translated, and is
 * shown for every mode that puts a container on a MikroTik.
 *
 * Dev-only:
 *   npm install jsdom && node tests/conf3.0-prep.test.js
 */
const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

const html = fs.readFileSync(path.join(__dirname, '..', 'docs', 'conf3.0.html'), 'utf8');

let fails = 0, passes = 0;
function ok(name, cond, extra) {
    if (cond) { passes++; console.log('  PASS  ' + name); }
    else { fails++; console.log('  FAIL  ' + name + (extra ? ': ' + extra : '')); }
}

function offlineGuard(win) {
    for (const name of ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource']) {
        try { win[name] = function () { throw new Error('test tried the network via ' + name); }; } catch (e) { /* ignore */ }
    }
}
const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.invalid/', beforeParse: offlineGuard });
const w = dom.window;
const doc = w.document;

console.log('=== conf3.0 router preparation ===');

const card = doc.getElementById('prep-card');
ok('the card is there', !!card);
const confCard = doc.getElementById('conf-input').closest('.card');
ok('it comes before the config card',
   !!(card.compareDocumentPosition(confCard) & w.Node.DOCUMENT_POSITION_FOLLOWING));
ok('and before the mode choice',
   !!(card.compareDocumentPosition(doc.querySelector('input[name="deploy-mode"]')) & w.Node.DOCUMENT_POSITION_FOLLOWING));
// Folded by default so it does not push the .conf field off the first screen,
// but a summary line says it opens.
ok('it is a fold that opens', card.tagName === 'DETAILS' && !!card.querySelector('summary'));
ok('folded at first', !card.open);
ok('the summary names it and says it opens',
   card.querySelector('summary [data-i18n="prepTitle"]') && card.querySelector('summary [data-i18n="prepTap"]'));
const cmds = Array.prototype.map.call(card.querySelectorAll('pre.prep-cmd'), function (e) { return e.textContent; });
ok('the device-mode command, exactly',
   cmds.indexOf('/system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes') >= 0, cmds.join(' | '));
// RouterOS 7.18+ fetches the package itself: the same three steps WinBox does
// with Check For Updates, Enable and Apply Changes.
ok('the package from the router itself, in the terminal',
   cmds.indexOf('/system/package/update/check-for-updates\n/system/package/enable container\n/system/package/apply-changes') >= 0,
   JSON.stringify(cmds));
ok('and the WinBox way', ['Check For Updates', 'Enable', 'Apply Changes']
   .every(function (c) { return card.textContent.indexOf(c) >= 0; }));
ok('the checks to run', ['/system/resource/print', '/system/package/print', '/system/device-mode/print']
   .every(function (c) { return card.textContent.indexOf(c) >= 0; }));
ok('the package download link', !!card.querySelector('a[href="https://mikrotik.com/download"]'));

const keys = Array.prototype.map.call(card.querySelectorAll('[data-i18n]'), function (e) {
    return e.getAttribute('data-i18n');
});
ok('every line is translated', keys.length >= 10 && keys.every(function (k) {
    return w.I18N.en[k] && w.I18N.ru[k] && w.I18N.en[k] !== w.I18N.ru[k];
}), keys.filter(function (k) { return !(w.I18N.ru[k] && w.I18N.ru[k] !== w.I18N.en[k]); }).join(','));
w.setLang('ru');
ok('in Russian on switching', doc.querySelector('[data-i18n="prepTitle"]').textContent === 'Сначала подготовьте роутер');
w.setLang('en');
ok('and back', doc.querySelector('[data-i18n="prepTitle"]').textContent === 'First, prepare the router');

function mode(value) {
    doc.querySelector('input[name="deploy-mode"][value="' + value + '"]').checked = true;
    w.onModeChange();
    return card.style.display !== 'none';
}
['container', 'warp', 'site-to-site', 'server'].forEach(function (m) {
    ok('shown in the ' + m + ' mode', mode(m));
});
ok('hidden for the proxy on a device of its own', !mode('standalone'));
ok('back again', mode('container'));

console.log('');
console.log(fails ? (passes + '/' + (passes + fails) + ' checks passed, ' + fails + ' FAILED')
                  : (passes + '/' + passes + ' checks passed'));
process.exit(fails ? 1 : 0);
