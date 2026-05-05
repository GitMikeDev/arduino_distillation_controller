// File: index.h

const char index_html[] = R"rawliteral(
<!DOCTYPE HTML>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Distillation Controller</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f0f2f5; margin: 0; padding: 0; text-align: center; color: #333; }
    h1 { background-color: #2c3e50; color: white; margin: 0; padding: 15px; font-size: 22px; letter-spacing: 2px;}
    .container { padding: 10px; max-width: 600px; margin: auto; }

    /* Panels and grid */
    .status-panel { background: white; padding: 15px; border-radius: 10px; margin-bottom: 15px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); font-size: 18px; font-weight: bold; }
    #stan-text { color: #d35400; }
    #stan-hint { color: #95a5a6; font-weight: normal; font-size: 14px; margin-left: 6px; }

    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 15px; }
    .grid-3 { grid-template-columns: 1fr 1fr 1fr; }
    .grid-3 .box-val { font-size: 18px; }
    .grid-3 .box { padding: 12px 4px; }
    .box { background: white; padding: 15px 5px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); }
    .box-title { font-size: 14px; color: #7f8c8d; margin-bottom: 5px;}
    .box-val { font-size: 24px; font-weight: bold; color: #2980b9; }

    /* Collection rate controls */
    .controls { background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); margin-bottom: 15px;}
    .rate-val { font-size: 28px; font-weight: bold; margin: 0 20px; vertical-align: middle;}
    .btn-circle { background-color: #34495e; color: white; border: none; border-radius: 50%; width: 60px; height: 60px; font-size: 30px; cursor: pointer; transition: 0.1s; vertical-align: middle;}
    .btn-circle:active { transform: scale(0.9); }

    /* Standard buttons */
    .btn-row { display: flex; justify-content: space-between; gap: 10px; margin-bottom: 15px; }
    .btn { flex: 1; padding: 15px 0; border: none; border-radius: 8px; font-size: 16px; font-weight: bold; cursor: pointer; transition: 0.2s; color: white; text-transform: uppercase;}
    .btn:active { opacity: 0.8; transform: scale(0.95); }

    .bg-gray { background-color: #95a5a6; }
    .bg-red { background-color: #e74c3c; }
    .bg-green { background-color: #2ecc71; }
    .bg-blue { background-color: #3498db; }

    /* Mode button states */
    .btn-mode-stable  { background-color: #2ecc71 !important; }   /* green - active and stable */
    .btn-mode-waiting { background-color: #f39c12 !important; }   /* yellow - selected, waiting for stabilization */
    .btn-mode-disabled { opacity: 0.4; }
    .btn-mode-blink { animation: btn-blink 1.4s ease-in-out infinite; }
    @keyframes btn-blink {
      0%, 100% { background-color: #95a5a6; }
      50%      { background-color: #f1c40f; }
    }

    /* Pause - all lockable actions in purple */
    .paused-purple { background-color: #9b59b6 !important; opacity: 0.7; }
    button:disabled { cursor: not-allowed; }

    .conn-err { background-color: #c0392b; color: white; padding: 5px; font-size: 12px; display: none; }
  </style>
</head>
<body>
  <h1>DISTILLATION</h1>
  <div id="error-bar" class="conn-err">No connection to system!</div>

  <div class="container">
    <!-- Status -->
    <div class="status-panel">
      Now: <span id="stan-text">Loading...</span><span id="stan-hint"></span>
    </div>

    <!-- Sensor readings -->
    <div class="grid">
      <div class="box"><div class="box-title">Keg Temp</div><div class="box-val"><span id="v-keg">--</span> &deg;C</div></div>
      <div class="box"><div class="box-title">Column Temp</div><div class="box-val"><span id="v-kol">--</span> &deg;C</div></div>
      <div class="box"><div class="box-title">Pressure</div><div class="box-val"><span id="v-cis">--</span> hPa</div></div>
      <div class="box"><div class="box-title">Boiling Point</div><div class="box-val"><span id="v-wrz">--</span> &deg;C</div></div>
    </div>

    <!-- Collected volumes -->
    <div class="grid grid-3">
      <div class="box"><div class="box-title">Foreshots</div><div class="box-val"><span id="v-fore">--</span></div></div>
      <div class="box"><div class="box-title">Hearts</div><div class="box-val"><span id="v-heart">--</span></div></div>
      <div class="box"><div class="box-title">Tails</div><div class="box-val"><span id="v-tail">--</span></div></div>
    </div>

    <!-- Rate control -->
    <div class="controls">
      <div class="box-title" style="margin-bottom: 15px;">Collection rate [ml/min]</div>
      <button class="btn-circle lock-on-pause" onclick="cmd('?minus')">-</button>
      <span class="rate-val" id="v-rate">--</span>
      <button class="btn-circle lock-on-pause" onclick="cmd('?plus')">+</button>
    </div>

    <!-- Modes -->
    <div class="btn-row">
      <button id="btn-fore"  class="btn bg-gray lock-on-pause" onclick="cmd('?foreshots')">Foreshots</button>
      <button id="btn-heart" class="btn bg-gray lock-on-pause" onclick="cmd('?hearts')">Hearts</button>
      <button id="btn-tail"  class="btn bg-gray lock-on-pause" onclick="cmd('?tails')">Tails</button>
    </div>

    <!-- Pause / Resume -->
    <div class="btn-row">
      <button class="btn bg-red" onclick="cmd('?stop')">Pause</button>
      <button class="btn bg-green" onclick="cmd('?start')">Resume</button>
    </div>

    <!-- BP correction -->
    <div class="controls">
      <div class="box-title" style="margin-bottom: 10px;">BP correction (current: <span id="v-hist">--</span>)</div>
      <button class="btn bg-gray lock-on-pause" style="width: 40%; padding: 10px;" onclick="cmd('?histm')">- -</button>
      <button class="btn bg-gray lock-on-pause" style="width: 40%; padding: 10px;" onclick="cmd('?histp')">+ +</button>
    </div>

    <!-- Restart (at the very bottom) -->
    <div class="btn-row">
      <button class="btn bg-blue" onclick="cmd('?restart')">Restart</button>
    </div>
  </div>

  <script>
    // Send command on button click. Restart requires confirmation - destructive action.
    function cmd(action) {
      if (action === '?restart' && !confirm('Restart the controller? Current process will be interrupted.')) return;
      fetch(action).then(r => r.text()).then(updateData).catch(showErr);
    }

    // Polls system state in the background (every 1.5s).
    function poll() {
      fetch('?data').then(r => r.text()).then(updateData).catch(showErr);
    }

    // Last known rate per mode - for "previously X" hint when column briefly unstable.
    const ACTIVE_MODES = ['Foreshots', 'Hearts', 'Tails'];
    const lastRateByMode = {};

    // Update displayed values from sensor response.
    function updateData(res) {
      document.getElementById('error-bar').style.display = 'none';
      if(!res.startsWith(';')) return;
      let d = res.split(';');

      let stable = (d[11] === '1');
      let paused = (d[12] === '1');
      let currentRate = parseInt(d[5]) || 0;

      // Remember last non-zero rate for the active mode
      if (currentRate > 0 && !paused && ACTIVE_MODES.indexOf(d[1]) !== -1) {
        lastRateByMode[d[1]] = currentRate;
      }

      let stanText = d[1] + (paused ? " (PAUSED)" : (stable ? " (Stable)" : " (Waiting...)"));
      document.getElementById('stan-text').innerText = stanText;

      // Hint "previously X ml/min" - only when active mode, unstable, not paused
      let hint = '';
      if (ACTIVE_MODES.indexOf(d[1]) !== -1 && !stable && !paused && lastRateByMode[d[1]]) {
        hint = ' — previously ' + lastRateByMode[d[1]] + ' ml/min';
      }
      document.getElementById('stan-hint').innerText = hint;

      document.getElementById('v-keg').innerText = d[2];
      document.getElementById('v-kol').innerText = d[3];
      document.getElementById('v-cis').innerText = d[4];
      document.getElementById('v-rate').innerText = d[5];
      document.getElementById('v-fore').innerText = d[6];
      document.getElementById('v-heart').innerText = d[7];
      document.getElementById('v-tail').innerText = d[8];
      document.getElementById('v-wrz').innerText = d[9];
      document.getElementById('v-hist').innerText = d[10];

      refreshUI(d[1], stable, paused);
    }

    // Single point of UI updates / button locking.
    // Priority: pause > mode. When paused - all .lock-on-pause go purple+disabled.
    // Otherwise - mode logic (green/yellow/blink).
    function refreshUI(state, isStable, paused) {
      const modeBtns = {
        'Foreshots': document.getElementById('btn-fore'),
        'Hearts':    document.getElementById('btn-heart'),
        'Tails':     document.getElementById('btn-tail')
      };
      const lockables = document.querySelectorAll('.lock-on-pause');

      // 1) reset everything
      for (const el of lockables) {
        el.classList.remove('paused-purple');
        el.disabled = false;
      }
      for (const btn of Object.values(modeBtns)) {
        btn.classList.remove('btn-mode-stable', 'btn-mode-waiting', 'btn-mode-disabled', 'btn-mode-blink');
      }

      // 2) if not paused - apply mode logic
      if (!paused) {
        const activeBtn = modeBtns[state];
        for (const btn of Object.values(modeBtns)) {
          if (activeBtn) {
            if (btn === activeBtn) {
              // Active mode - always disabled (cannot re-select the same one)
              btn.classList.add(isStable ? 'btn-mode-stable' : 'btn-mode-waiting');
              btn.disabled = true;
            } else if (!isStable) {
              btn.classList.add('btn-mode-disabled');
              btn.disabled = true;
            }
          } else if (isStable) {
            btn.classList.add('btn-mode-blink');
          }
        }
      }

      // 3) pause overrides everything - purple + disabled
      if (paused) {
        for (const el of lockables) {
          el.classList.add('paused-purple');
          el.disabled = true;
        }
      }
    }

    function showErr() { document.getElementById('error-bar').style.display = 'block'; }

    // Start polling loop (every 1500 ms)
    setInterval(poll, 1500);
    poll(); // First fetch immediately
  </script>
</body>
</html>
)rawliteral";
