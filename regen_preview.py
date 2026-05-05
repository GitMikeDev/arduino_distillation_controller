"""
regen_preview.py

Regenerates web_ui_preview.html from the current index.h - extracts HTML from the
rawliteral block and appends a mock layer (offline preview, not present on real Arduino).

Usage: python regen_preview.py
"""

import re
from pathlib import Path

ROOT = Path(__file__).parent
SRC = ROOT / "index.h"
DST = ROOT / "web_ui_preview.html"

content = SRC.read_text(encoding="utf-8")
m = re.search(r'R"rawliteral\((.*)\)rawliteral";', content, re.DOTALL)
if not m:
    raise SystemExit("rawliteral block not found in index.h")
html = m.group(1).strip()

MOCK = """
<!-- ===== MOCK LAYER (offline preview only - not present on real Arduino) ===== -->
<script>
(function() {
    let state = 'Hearts';
    let isStable = 1;
    let paused = 0;
    let rate = 40;
    let kegT = 85.0;
    let colT = 77.30;
    let press = 970.5;
    let foreVol = 200.0;
    let heartVol = 1500.0;
    let tailVol = 0.0;
    const boil = 77.20;
    let adj = 0.0;
    let foreshotsCompleted = false;

    setInterval(() => {
        colT += (Math.random() - 0.5) * 0.2;
        if (colT > 78.0) colT -= 0.05;
        if (colT < 76.5) colT += 0.05;
        if (kegT < 95) kegT += 0.003;
        if (rate > 0 && isStable && !paused) {
            const phys = rate < 20 ? 20 : rate;
            const delta = (phys / 60) * 1.5 * 1.35;
            if (state === 'Foreshots') foreVol += delta;
            else if (state === 'Hearts') heartVol += delta;
            else if (state === 'Tails') tailVol += delta;
        }
    }, 1000);

    window.fetch = function(url) {
        if (url.includes('?plus'))      rate = Math.min(40, rate + 2);
        else if (url.includes('?minus')) rate = Math.max(0, rate - 2);
        else if (url.includes('?foreshots')) {
            if (state !== 'Foreshots') {
                state = 'Foreshots'; rate = 10; isStable = 0;
                foreshotsCompleted = false;
                setTimeout(() => isStable = 1, 3000);
            }
        }
        else if (url.includes('?hearts')) {
            if (state !== 'Hearts') {
                state = 'Hearts'; rate = 40; isStable = 0;
                foreshotsCompleted = false;
                setTimeout(() => isStable = 1, 3000);
            }
        }
        else if (url.includes('?tails')) {
            if (state !== 'Tails') {
                state = 'Tails'; rate = 10; isStable = 0;
                foreshotsCompleted = false;
                setTimeout(() => isStable = 1, 3000);
            }
        }
        else if (url.includes('?stop'))  { paused = 1; }
        else if (url.includes('?start')) { paused = 0; }
        else if (url.includes('?histp')) adj += 0.2;
        else if (url.includes('?histm')) adj -= 0.2;
        else if (url.includes('?restart')) { /* mock no-op */ }

        const displayRate = (paused || !isStable) ? 0 : rate;
        let displayState = state;
        if (state === 'Idle' && foreshotsCompleted) displayState = 'Foreshots Done';

        const body = ';' + displayState + ';' + kegT.toFixed(2) + ';' + colT.toFixed(2) +
                     ';' + press.toFixed(2) + ';' + displayRate +
                     ';' + foreVol.toFixed(1) + ' ml;' +
                     heartVol.toFixed(1) + ' ml;' +
                     tailVol.toFixed(1) + ' ml;' +
                     (boil + adj).toFixed(2) + ';' + adj.toFixed(2) + ';' +
                     isStable + ';' + paused + ';';
        return Promise.resolve({ ok: true, text: () => Promise.resolve(body) });
    };

    window.__setScenario = function(name) {
        if (name === 'ready')             { state = 'Idle';     isStable = 1; rate = 0; paused = 0; foreshotsCompleted = false; }
        else if (name === 'heating')      { state = 'Idle';     isStable = 0; rate = 0; paused = 0; }
        else if (name === 'finished')     { state = 'Finished'; isStable = 0; rate = 0; paused = 0; }
        else if (name === 'destabilize')  { isStable = 0; }
        else if (name === 'restabilize')  { isStable = 1; }
        else if (name === 'foreshotsDone'){ state = 'Idle';     isStable = 1; rate = 0; paused = 0; foreshotsCompleted = true; }
    };

    document.addEventListener('DOMContentLoaded', () => {
        const banner = document.createElement('div');
        banner.style.cssText = 'background:#f39c12;color:white;padding:6px 10px;font-size:12px;text-align:center;font-family:sans-serif;';
        banner.textContent = 'OFFLINE PREVIEW - data is locally simulated (mock layer)';
        document.body.insertBefore(banner, document.body.firstChild);

        const panel = document.createElement('div');
        panel.style.cssText = 'background:#ecf0f1;padding:8px;text-align:center;font-family:sans-serif;font-size:12px;border-bottom:1px solid #bdc3c7;';

        const scenarios = [
            ['ready',          'READY'],
            ['heating',        'HEATING'],
            ['finished',       'FINISHED'],
            ['destabilize',    'DESTABILIZE'],
            ['restabilize',    'RESTABILIZE'],
            ['foreshotsDone',  'FORESHOTS DONE']
        ];
        const label = document.createElement('b');
        label.textContent = 'Test: ';
        panel.appendChild(label);
        scenarios.forEach(([key, text]) => {
            const b = document.createElement('button');
            b.textContent = text;
            b.style.cssText = 'margin:2px;padding:4px 8px;';
            b.addEventListener('click', () => window.__setScenario(key));
            panel.appendChild(b);
        });
        document.body.insertBefore(panel, document.body.firstChild.nextSibling);
    });
})();
</script>
"""

html = html.replace("</body>", MOCK + "</body>")
DST.write_text(html, encoding="utf-8")
print(f"OK -> {DST.name}  ({len(html)} bytes)")
