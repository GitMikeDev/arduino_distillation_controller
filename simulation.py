"""
simulation.py

Distillation controller logic simulator - 1:1 mirror of dist_controller_final.ino.

Modes:
  python simulation.py                       -> if 'example_run.csv' exists, replay it
  python simulation.py <file.csv>            -> replay a given CSV
  python simulation.py --synthetic           -> generated synthetic scenario

CSV-replay mode:
  - Reads logs exported from ThingSpeak (Time, Temp_Keg, Temp_Column, Pressure, Rate, State).
  - Iterates through samples, feeds sensors into sim and runs controller logic.
  - Detects manual operator interventions (any rate increase = ?plus, since logic only decreases).
  - Counts: critical stops, recoveries, final volumes (predicted vs integrated from CSV).

No external dependencies - stdlib only.
"""

import csv
import os
import sys
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum

# === Process constants (1:1 with dist_controller_final.ino) ===
TEMP_HYSTERESIS_C            = 0.4
COLLECTION_RATE_HYSTERESIS   = 2
MAX_COLLECTION_RATE          = 40
MIN_PWM_COLLECTION_RATE      = 20
INITIAL_RATE_FORESHOTS       = 10
INITIAL_RATE_HEARTS          = 40
INITIAL_RATE_TAILS           = 10
FORESHOTS_CUTOFF_VOLUME_ML   = 200.0
VOLUME_CALIBRATION_FACTOR    = 1.35
MIN_OPERATING_TEMP_C         = 75.0
MAX_KEG_TEMP_C               = 96.0

VOLUME_CALC_INTERVAL_S         = 5
STABILIZATION_CHECK_INTERVAL_S = 150

# Ethanol boiling point calibration (linear interp vs atmospheric pressure)
ETHANOL_BP_LOW_PRESS_HPA  = 962.21
ETHANOL_BP_HIGH_PRESS_HPA = 1062.29
ETHANOL_BP_LOW_TEMP_C     = 77.0
ETHANOL_BP_HIGH_TEMP_C    = 79.5

DEFAULT_CSV = "example_run.csv"


class State(Enum):
    IDLE      = "Idle"
    FORESHOTS = "Foreshots"
    HEARTS    = "Hearts"
    TAILS     = "Tails"
    FINISHED  = "Finished"


STATE_FROM_NUM = {
    0: State.IDLE,
    1: State.FORESHOTS,
    2: State.HEARTS,
    3: State.TAILS,
}


def fmap(v, in_lo, in_hi, out_lo, out_hi):
    return (v - in_lo) * (out_hi - out_lo) / (in_hi - in_lo) + out_lo


@dataclass
class Sim:
    t: int = 0
    kegT: float = 20.0
    columnT: float = 20.0
    pressure: float = 1013.0
    boilingPoint: float = 0.0
    boilingPointAdj: float = 0.0
    state: State = State.IDLE
    isStable: bool = False
    recoveryRequired: bool = False
    manualPause: bool = False
    rateForeshots: int = INITIAL_RATE_FORESHOTS
    rateHearts: int = INITIAL_RATE_HEARTS
    rateTails: int = INITIAL_RATE_TAILS
    collectionRate: int = 0
    foreshotsVolume: float = 0.0
    heartsVolume: float = 0.0
    tailsVolume: float = 0.0
    lastStabCheck: int = 0
    lastVolumeCalc: int = 0
    log: list = field(default_factory=list)

    def fmt_t(self):
        return f"{self.t//3600:02d}:{(self.t%3600)//60:02d}:{self.t%60:02d}"

    def event(self, msg):
        self.log.append(f"[{self.fmt_t()}]  {msg}")

    # --- mirror of C++ controller functions ---

    def update_boiling_point(self):
        self.boilingPoint = fmap(
            self.pressure,
            ETHANOL_BP_LOW_PRESS_HPA, ETHANOL_BP_HIGH_PRESS_HPA,
            ETHANOL_BP_LOW_TEMP_C,    ETHANOL_BP_HIGH_TEMP_C,
        ) + self.boilingPointAdj

    def critical_safety_check(self):
        out_of_range = (self.columnT >= self.boilingPoint + TEMP_HYSTERESIS_C
                        or self.columnT < MIN_OPERATING_TEMP_C)
        if out_of_range and self.isStable:
            self.event(
                f"!!! CRITICAL STOP - column {self.columnT:.2f}C out of range "
                f"[{MIN_OPERATING_TEMP_C:.1f} .. {self.boilingPoint + TEMP_HYSTERESIS_C:.2f}]"
            )
            self.isStable = False
            self.recoveryRequired = True

    def check_and_adjust_stabilization(self):
        if self.isStable:
            return
        cooled = self.columnT <= self.boilingPoint - TEMP_HYSTERESIS_C / 2.0
        if cooled and self.columnT >= MIN_OPERATING_TEMP_C:
            if self.recoveryRequired:
                if self.state == State.FORESHOTS:
                    self.rateForeshots = max(0, self.rateForeshots - COLLECTION_RATE_HYSTERESIS)
                    new_rate = self.rateForeshots
                elif self.state == State.HEARTS:
                    self.rateHearts = max(0, self.rateHearts - COLLECTION_RATE_HYSTERESIS)
                    new_rate = self.rateHearts
                elif self.state == State.TAILS:
                    self.rateTails = max(0, self.rateTails - COLLECTION_RATE_HYSTERESIS)
                    new_rate = self.rateTails
                else:
                    new_rate = -1
                self.event(
                    f"  RECOVERY: soft restart - target rate -= {COLLECTION_RATE_HYSTERESIS} "
                    f"(-> {new_rate} ml/min)"
                )
                self.recoveryRequired = False
            else:
                self.event(
                    f"  STABILIZATION: column {self.columnT:.2f}C in range "
                    f"[<= {self.boilingPoint - TEMP_HYSTERESIS_C/2:.2f}] - starting at full rate"
                )
            self.isStable = True

    def handle_process_logic(self):
        if self.state == State.IDLE:
            self.collectionRate = 0
        elif self.state == State.FORESHOTS:
            self.collectionRate = self.rateForeshots if self.isStable else 0
            if self.foreshotsVolume >= FORESHOTS_CUTOFF_VOLUME_ML:
                self.event(f"  FORESHOTS DONE ({self.foreshotsVolume:.1f} ml) -> IDLE")
                self.state = State.IDLE
                self.collectionRate = 0
        elif self.state == State.HEARTS:
            self.collectionRate = self.rateHearts if self.isStable else 0
        elif self.state == State.TAILS:
            self.collectionRate = self.rateTails if self.isStable else 0
        elif self.state == State.FINISHED:
            self.collectionRate = 0

        if self.manualPause:
            self.collectionRate = 0

        if self.kegT >= MAX_KEG_TEMP_C and self.state != State.FINISHED:
            self.event(f"!!! KEG TEMPERATURE {self.kegT:.2f}C >= MAX -> process FINISHED")
            self.state = State.FINISHED
            self.collectionRate = 0

    def update_collected_volume(self, dt_seconds):
        if self.collectionRate <= 0:
            return
        if 0 < self.collectionRate < MIN_PWM_COLLECTION_RATE:
            actual = MIN_PWM_COLLECTION_RATE
        else:
            actual = self.collectionRate
        delta = actual * (dt_seconds / 60.0) * VOLUME_CALIBRATION_FACTOR
        if self.state == State.FORESHOTS:
            self.foreshotsVolume += delta
        elif self.state == State.HEARTS:
            self.heartsVolume += delta
        elif self.state == State.TAILS:
            self.tailsVolume += delta

    def step_logic(self):
        """Single iteration of controller logic. Caller sets self.t and sensor readings."""
        prev_rate = self.collectionRate

        self.update_boiling_point()
        self.critical_safety_check()
        self.handle_process_logic()

        dt = self.t - self.lastVolumeCalc
        if dt >= VOLUME_CALC_INTERVAL_S:
            self.lastVolumeCalc = self.t
            self.update_collected_volume(dt)

        if self.state not in (State.IDLE, State.FINISHED):
            if self.t - self.lastStabCheck >= STABILIZATION_CHECK_INTERVAL_S:
                self.lastStabCheck = self.t
                self.check_and_adjust_stabilization()

        if self.collectionRate != prev_rate:
            self.event(f"  rate {prev_rate} -> {self.collectionRate} ml/min")


# ============================================================
# CSV REPLAY
# ============================================================

def parse_decimal(s):
    """Accept both '77.38' and '77,38' (Polish decimal separator from older ThingSpeak exports)."""
    return float(s.replace(',', '.'))


def load_csv(path):
    rows = []
    with open(path, 'r', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            rows.append({
                'ts':       datetime.fromisoformat(r['Time']),
                'kegT':     parse_decimal(r['Temp_Keg']),
                'columnT':  parse_decimal(r['Temp_Column']),
                'pressure': parse_decimal(r['Pressure']),
                'rate':     int(r['Rate']),
                'state':    int(r['State']),
            })
    return rows


def run_csv_replay(path):
    rows = load_csv(path)
    if not rows:
        print(f"CSV {path} is empty.")
        return

    sim = Sim()
    t0 = rows[0]['ts']

    # Initialize from first CSV row
    first = rows[0]
    sim.kegT     = first['kegT']
    sim.columnT  = first['columnT']
    sim.pressure = first['pressure']
    sim.state    = STATE_FROM_NUM.get(first['state'], State.IDLE)
    initial_rate = first['rate']
    if sim.state == State.HEARTS:
        sim.rateHearts = initial_rate if initial_rate > 0 else INITIAL_RATE_HEARTS
    elif sim.state == State.FORESHOTS:
        sim.rateForeshots = initial_rate if initial_rate > 0 else INITIAL_RATE_FORESHOTS
    elif sim.state == State.TAILS:
        sim.rateTails = initial_rate if initial_rate > 0 else INITIAL_RATE_TAILS
    sim.isStable = (initial_rate > 0)
    sim.update_boiling_point()

    print("=== CSV Replay ===")
    print(f"File:           {path}")
    print(f"Samples:        {len(rows)}  (sampling ~{(rows[1]['ts']-rows[0]['ts']).total_seconds():.0f}s)")
    print(f"Time range:     {first['ts']:%H:%M:%S} -> {rows[-1]['ts']:%H:%M:%S}")
    avg_p = sum(r['pressure'] for r in rows) / len(rows)
    bp_avg = fmap(avg_p,
                  ETHANOL_BP_LOW_PRESS_HPA, ETHANOL_BP_HIGH_PRESS_HPA,
                  ETHANOL_BP_LOW_TEMP_C,    ETHANOL_BP_HIGH_TEMP_C)
    print(f"Pressure:       {min(r['pressure'] for r in rows):.2f} - {max(r['pressure'] for r in rows):.2f} hPa "
          f"(mean {avg_p:.2f} -> BP {bp_avg:.2f}C)")
    print(f"Stop threshold: BP + {TEMP_HYSTERESIS_C} = {bp_avg + TEMP_HYSTERESIS_C:.2f}C")
    print(f"Recovery thr.:  BP - {TEMP_HYSTERESIS_C/2} = {bp_avg - TEMP_HYSTERESIS_C/2:.2f}C")
    print(f"Initial state:  {sim.state.value}, rate {initial_rate}, isStable={sim.isStable}")
    print()

    # Replay
    csv_volume = 0.0
    last_t = 0
    prev_active_csv_rate = initial_rate if initial_rate > 0 else INITIAL_RATE_HEARTS
    manual_plus_clicks = 0

    for i, row in enumerate(rows):
        sim.t = int((row['ts'] - t0).total_seconds())
        sim.kegT     = row['kegT']
        sim.columnT  = row['columnT']
        sim.pressure = row['pressure']

        # Integrate volume from CSV (actual, ground truth)
        csv_rate = row['rate']
        if i > 0:
            dt_csv = sim.t - last_t
            if csv_rate > 0:
                phys = MIN_PWM_COLLECTION_RATE if 0 < csv_rate < MIN_PWM_COLLECTION_RATE else csv_rate
                csv_volume += phys * (dt_csv / 60.0) * VOLUME_CALIBRATION_FACTOR
        last_t = sim.t

        # Detect manual operator intervention from CSV:
        # controller logic only DECREASES rate (recovery -=2). An increase = manual ?plus.
        if csv_rate > 0:
            if csv_rate > prev_active_csv_rate:
                diff = csv_rate - prev_active_csv_rate
                clicks = diff // 2
                sim.event(
                    f"OPERATOR: rate {prev_active_csv_rate} -> {csv_rate} ml/min "
                    f"(manual {clicks}x ?plus)"
                )
                manual_plus_clicks += clicks
                # Sync sim with reality
                if sim.state == State.HEARTS:
                    sim.rateHearts = csv_rate
                elif sim.state == State.FORESHOTS:
                    sim.rateForeshots = csv_rate
                elif sim.state == State.TAILS:
                    sim.rateTails = csv_rate
            prev_active_csv_rate = csv_rate

        # Run controller logic
        sim.step_logic()

    # Stats
    critical_stops = sum(1 for line in sim.log if "CRITICAL STOP" in line)
    recoveries     = sum(1 for line in sim.log if "RECOVERY:" in line)
    operator_plus  = sum(1 for line in sim.log if "?plus" in line)

    # CSV: count pump stops (rate transitions from >0 to 0)
    csv_stops = 0
    prev_csv_rate = initial_rate
    for r in rows[1:]:
        if prev_csv_rate > 0 and r['rate'] == 0:
            csv_stops += 1
        prev_csv_rate = r['rate']

    # Print log
    for line in sim.log:
        print(line)

    print()
    print("=== Summary ===")
    print(f"Runtime:                  {sim.fmt_t()}")
    print(f"Final state (sim):        {sim.state.value}")
    print(f"Final state in CSV:       {STATE_FROM_NUM[rows[-1]['state']].value} (rate {rows[-1]['rate']})")
    print(f"Final keg temp:           {sim.kegT:.2f}C")
    print(f"Final column temp:        {sim.columnT:.2f}C")
    print()
    print(f"Critical stops (sim):     {critical_stops}")
    print(f"Critical stops in CSV:    {csv_stops}  (counted as rate>0 -> 0 transitions)")
    print(f"Recoveries (sim):         {recoveries}")
    print(f"Operator ?plus events:    {operator_plus}  ({manual_plus_clicks} clicks total)")
    print()
    print(f"Hearts volume (sim):      {sim.heartsVolume:7.1f} ml")
    print(f"Hearts volume from CSV:   {csv_volume:7.1f} ml  (rate integrated over time, calibration {VOLUME_CALIBRATION_FACTOR})")
    print(f"Difference:               {sim.heartsVolume - csv_volume:+.1f} ml")
    print()
    print(f"Final target rate (sim):  {sim.rateHearts} ml/min")
    print(f"Final target rate (CSV):  {prev_active_csv_rate} ml/min  (last non-zero observed)")


# ============================================================
# SYNTHETIC SCENARIO
# ============================================================

def synth_sensors(t):
    """Synthetic profile: warmup + foreshots + hearts with 4 spikes."""
    P = 1013.0
    if t < 60:
        kegT = 20.0 + 60.0 * (t / 60.0)
        colT = 20.0 + 58.0 * (t / 60.0)
    else:
        rel = t - 60
        kegT = min(96.5, 80.0 + 16.0 * (rel / 5400.0))
        baseline = 78.0
        spike = 0.0
        for spike_start in (2400, 3300, 4200, 5100):
            if spike_start <= t < spike_start + 90:
                spike = 1.0
        colT = baseline + spike
    return kegT, colT, P


def run_synthetic():
    sim = Sim()
    bp_at_1013 = fmap(1013.0,
                      ETHANOL_BP_LOW_PRESS_HPA, ETHANOL_BP_HIGH_PRESS_HPA,
                      ETHANOL_BP_LOW_TEMP_C,    ETHANOL_BP_HIGH_TEMP_C)
    print("=== Distillation controller simulation - synthetic mode ===")
    print(f"Pressure (constant): 1013 hPa, theoretical BP: {bp_at_1013:.2f}C")
    print(f"Stop threshold: {bp_at_1013 + TEMP_HYSTERESIS_C:.2f}C, recovery threshold: {bp_at_1013 - TEMP_HYSTERESIS_C/2:.2f}C")
    print()

    actions = {"foreshots": False, "hearts": False}

    while sim.state != State.FINISHED and sim.t < 7200:
        sim.t += 1
        kegT, colT, P = synth_sensors(sim.t)
        sim.kegT, sim.columnT, sim.pressure = kegT, colT, P

        if sim.t == 60 and not actions["foreshots"]:
            sim.event("USER: short press -> FORESHOTS")
            sim.state = State.FORESHOTS
            sim.isStable = False
            sim.lastStabCheck = 0
            actions["foreshots"] = True

        if (sim.state == State.IDLE and actions["foreshots"]
                and not actions["hearts"]
                and sim.foreshotsVolume >= FORESHOTS_CUTOFF_VOLUME_ML):
            sim.event("USER: long press -> HEARTS")
            sim.state = State.HEARTS
            sim.isStable = False
            sim.lastStabCheck = 0
            actions["hearts"] = True

        sim.step_logic()

    for line in sim.log:
        print(line)

    print()
    print("=== Final summary ===")
    print(f"Final state:    {sim.state.value}")
    print(f"Runtime:        {sim.fmt_t()}")
    print(f"Collected:")
    print(f"  Foreshots:  {sim.foreshotsVolume:7.1f} ml")
    print(f"  Hearts:     {sim.heartsVolume:7.1f} ml")
    print(f"  Tails:      {sim.tailsVolume:7.1f} ml")


# ============================================================
# DISPATCH
# ============================================================

def main():
    if len(sys.argv) > 1:
        arg = sys.argv[1]
        if arg == "--synthetic":
            run_synthetic()
            return
        if os.path.exists(arg):
            run_csv_replay(arg)
            return
        print(f"File not found: {arg}")
        sys.exit(1)

    if os.path.exists(DEFAULT_CSV):
        run_csv_replay(DEFAULT_CSV)
    else:
        run_synthetic()


if __name__ == "__main__":
    main()
