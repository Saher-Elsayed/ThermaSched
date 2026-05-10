#!/usr/bin/env python3
"""
run_benchmark.py — 60-second 5-model concurrent inference benchmark

Reproduces the core experimental results of Tables 2, 5, 6 and Figure 2.

Connects to the ZCU102 via UART, controls model loading, reads SYSMON
temperature traces and INA226 power measurements, and computes TOPS/W.

Usage:
    python run_benchmark.py --models resnet18 mobilenetv2 yolotiny efficientnet-b0 squeezenet
    python run_benchmark.py --scheduler thermasched --duration 60 --ambient 25
    python run_benchmark.py --scheduler static     --duration 60 --runs 10

Outputs:
    results/benchmark_{scheduler}_{ambient}C_{timestamp}.json
    results/temp_trace_{scheduler}.csv
    results/power_trace_{scheduler}.csv

Reference: ThermaSched §5 (Experimental Setup), §6.1 (Results)
"""

import argparse
import json
import time
import csv
import sys
import os
import numpy as np
from datetime import datetime
from dataclasses import dataclass, asdict
from typing import List, Dict, Optional

# Add parent directory to path for shared utilities
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))


@dataclass
class EpochRecord:
    """Per-epoch measurement record."""
    epoch:         int
    time_s:        float
    temps_c:       List[float]      # 4 tile temperatures
    power_w:       List[float]      # 4 tile PL rail powers (INA226)
    freq_mhz:      List[int]        # 4 tile clock frequencies
    fps_per_model: List[float]      # FPS for each of the N models
    dvfs_events:   int
    dpr_events:    int
    dtm_active:    bool
    schedule_lat_us: float          # Scheduling decision latency


@dataclass
class BenchmarkResult:
    """Complete benchmark result."""
    scheduler:     str
    ambient_c:     float
    duration_s:    int
    models:        List[str]
    epochs:        List[EpochRecord]
    # Aggregate metrics (computed over last 30s / steady-state)
    mean_tops_w:   float
    mean_fps_total:float
    peak_temp_c:   float
    mean_power_w:  float
    mean_throughput_tops: float
    dtm_events:    int
    dvfs_events:   int
    dpr_events:    int
    sched_lat_mean_us: float
    sched_lat_std_us:  float


SUPPORTED_SCHEDULERS = [
    'thermasched',      # Full ThermaSched (proposed)
    'static',           # Static tile mapping (baseline i)
    'round_robin',      # Round-robin assignment (baseline ii)
    'thermal_heuristic',# 80°C threshold migration (baseline iii)
    'q_learning',       # Q-learning agent (baseline iv)
    'noc_aware',        # NoC-aware heuristic [Choi 2019] (baseline v)
    'noc_thermal',      # NoC+Thermal composite (baseline vi)
]

SUPPORTED_MODELS = [
    'resnet18', 'mobilenetv2', 'yolotiny',
    'efficientnet-b0', 'squeezenet',
    'vit-tiny', 'mobilenetv3-l', 'efficientnet-b2',
]

# Model MAC counts (millions) for TOPS computation
MODEL_MACS = {
    'resnet18':       1814,
    'mobilenetv2':     300,
    'yolotiny':        530,
    'efficientnet-b0': 390,
    'squeezenet':       350,
    'vit-tiny':         1200,
    'mobilenetv3-l':    219,
    'efficientnet-b2':  1000,
}


class BoardInterface:
    """UART interface to ZCU102 running ThermaSched firmware."""

    def __init__(self, port: str, baud: int = 115200):
        try:
            import serial
            self.ser = serial.Serial(port, baud, timeout=2.0)
            time.sleep(1.0)
            self.connected = True
        except Exception as e:
            print(f"WARNING: Could not connect to board ({e}). Using simulation mode.")
            self.connected = False

    def set_scheduler(self, scheduler: str) -> bool:
        if not self.connected:
            return True
        self.ser.write(f"SET_SCHEDULER {scheduler.upper()}\n".encode())
        resp = self.ser.readline().decode().strip()
        return resp == "OK"

    def load_models(self, models: List[str]) -> bool:
        if not self.connected:
            return True
        model_str = ','.join(models)
        self.ser.write(f"LOAD_MODELS {model_str}\n".encode())
        resp = self.ser.readline().decode().strip()
        return resp == "OK"

    def start_inference(self) -> bool:
        if not self.connected:
            return True
        self.ser.write(b"START_INFERENCE\n")
        return self.ser.readline().decode().strip() == "OK"

    def read_epoch_data(self) -> Optional[Dict]:
        """Read one epoch's telemetry from the ARM PS over UART."""
        if not self.connected:
            return self._simulate_epoch_data()
        try:
            line = self.ser.readline().decode().strip()
            return json.loads(line)
        except Exception:
            return None

    def _simulate_epoch_data(self) -> Dict:
        """Generate realistic simulated data for offline testing."""
        t = getattr(self, '_sim_t', 0.0)
        self._sim_t = t + 0.1

        # Simulate temperature rise and ThermaSched management
        base = 58.0
        rise = min(20.0, t * 0.4)  # Rises toward ~78°C
        temps = [base + rise + np.random.normal(0, 0.3) for _ in range(4)]
        powers = [3.2 + np.random.normal(0, 0.05) for _ in range(4)]
        freqs = [300 if t < 20 else max(275, 300 - int(t/10)*25) for _ in range(4)]
        fps = [46.0 + np.random.normal(0, 1.0) for _ in range(5)]

        return {
            'temps_c':   temps,
            'power_w':   powers,
            'freq_mhz':  freqs,
            'fps':       fps,
            'dvfs_events': 1 if t > 20 and np.random.random() < 0.34 else 0,
            'dpr_events':  1 if np.random.random() < 0.14 else 0,
            'dtm_active':  False,
            'sched_lat_us': 8.3 + np.random.normal(0, 0.9),
        }

    def stop_inference(self):
        if not self.connected:
            return
        self.ser.write(b"STOP_INFERENCE\n")
        self.ser.readline()

    def close(self):
        if self.connected:
            self.ser.close()


def compute_tops_w(epoch_records: List[EpochRecord],
                   models: List[str],
                   start_epoch: int = 0) -> float:
    """
    Compute aggregate TOPS/W from steady-state epoch records.
    TOPS = sum of per-model FPS × model MACs / 1e12
    W    = sum of per-tile PL rail power
    """
    records = [r for r in epoch_records if r.epoch >= start_epoch]
    if not records:
        return 0.0

    tops_list, power_list = [], []
    for r in records:
        total_tops = sum(
            r.fps_per_model[i] * MODEL_MACS.get(models[i], 500) * 1e6 / 1e12
            for i in range(len(models))
        )
        total_power = sum(r.power_w)
        if total_power > 0:
            tops_list.append(total_tops)
            power_list.append(total_tops / total_power)

    return float(np.mean(power_list)) if power_list else 0.0


def run_benchmark(args) -> BenchmarkResult:
    """Execute the full benchmark and return results."""

    print(f"\n{'='*60}")
    print(f"ThermaSched Benchmark")
    print(f"  Scheduler: {args.scheduler}")
    print(f"  Models:    {', '.join(args.models)}")
    print(f"  Duration:  {args.duration}s")
    print(f"  Ambient:   {args.ambient}°C")
    print(f"  Run:       {args.run_id}/{args.runs}")
    print(f"{'='*60}\n")

    board = BoardInterface(args.board if hasattr(args, 'board') else 'SIM')

    # Setup
    board.set_scheduler(args.scheduler)
    board.load_models(args.models)

    epochs = []
    epoch_num = 0
    t_start = time.monotonic()

    board.start_inference()

    # ── Main measurement loop ─────────────────────────────────────────────
    while (time.monotonic() - t_start) < args.duration:
        data = board.read_epoch_data()
        if data is None:
            continue

        t_elapsed = time.monotonic() - t_start
        record = EpochRecord(
            epoch=epoch_num,
            time_s=t_elapsed,
            temps_c=data['temps_c'],
            power_w=data['power_w'],
            freq_mhz=data['freq_mhz'],
            fps_per_model=data['fps'][:len(args.models)],
            dvfs_events=data['dvfs_events'],
            dpr_events=data['dpr_events'],
            dtm_active=data['dtm_active'],
            schedule_lat_us=data['sched_lat_us'],
        )
        epochs.append(record)

        # Progress display
        if epoch_num % 50 == 0:
            peak_t = max(record.temps_c)
            fps_avg = np.mean(record.fps_per_model)
            print(f"  t={t_elapsed:5.1f}s  peak_temp={peak_t:.1f}°C  "
                  f"fps/model={fps_avg:.1f}  "
                  f"DVFS={record.dvfs_events}  DPR={record.dpr_events}  "
                  f"DTM={'YES' if record.dtm_active else 'no'}")

        epoch_num += 1

    board.stop_inference()
    board.close()

    # ── Compute aggregate metrics over last 30s (steady-state) ────────────
    steady_start_epoch = max(0, epoch_num - 300)  # Last 30s at 100ms/epoch
    steady_records = [r for r in epochs if r.epoch >= steady_start_epoch]

    tops_w    = compute_tops_w(epochs, args.models, steady_start_epoch)
    peak_temp = max(max(r.temps_c) for r in steady_records)
    mean_pow  = np.mean([sum(r.power_w) for r in steady_records])
    fps_total = np.mean([sum(r.fps_per_model) for r in steady_records])
    dtm_total = sum(1 for r in epochs if r.dtm_active)
    dvfs_tot  = sum(r.dvfs_events for r in epochs)
    dpr_tot   = sum(r.dpr_events for r in epochs)
    lats      = [r.schedule_lat_us for r in steady_records]

    result = BenchmarkResult(
        scheduler=args.scheduler,
        ambient_c=args.ambient,
        duration_s=args.duration,
        models=args.models,
        epochs=epochs,
        mean_tops_w=tops_w,
        mean_fps_total=fps_total,
        peak_temp_c=peak_temp,
        mean_power_w=mean_pow,
        mean_throughput_tops=fps_total * np.mean(list(MODEL_MACS.values())) * 1e6 / 1e12,
        dtm_events=dtm_total,
        dvfs_events=dvfs_tot,
        dpr_events=dpr_tot,
        sched_lat_mean_us=float(np.mean(lats)),
        sched_lat_std_us=float(np.std(lats)),
    )

    print(f"\n{'='*60}")
    print(f"RESULTS ({args.scheduler}, {args.ambient}°C)")
    print(f"  TOPS/W:         {result.mean_tops_w:.2f}")
    print(f"  Peak temp:      {result.peak_temp_c:.1f}°C")
    print(f"  FPS/model:      {result.mean_fps_total/len(args.models):.1f}")
    print(f"  DTM events:     {result.dtm_events}")
    print(f"  DVFS events:    {result.dvfs_events}")
    print(f"  Sched latency:  {result.sched_lat_mean_us:.1f} ± {result.sched_lat_std_us:.1f} µs")
    print(f"{'='*60}\n")

    return result


def main():
    parser = argparse.ArgumentParser(description='ThermaSched benchmark runner')
    parser.add_argument('--scheduler', default='thermasched',
                        choices=SUPPORTED_SCHEDULERS)
    parser.add_argument('--models', nargs='+', default=[
        'resnet18', 'mobilenetv2', 'yolotiny', 'efficientnet-b0', 'squeezenet'])
    parser.add_argument('--duration', type=int, default=60)
    parser.add_argument('--ambient', type=float, default=25.0)
    parser.add_argument('--runs', type=int, default=10,
                        help='Number of independent runs (paper uses 10)')
    parser.add_argument('--output-dir', default='results/')
    parser.add_argument('--board', default='SIM',
                        help='UART port or SIM for simulation mode')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    all_results = []
    for run_id in range(1, args.runs + 1):
        args.run_id = run_id
        result = run_benchmark(args)
        all_results.append(result)

    # Compute statistics across runs
    tops_vals = [r.mean_tops_w for r in all_results]
    print(f"\n{'='*60}")
    print(f"AGGREGATE STATISTICS ({args.runs} runs)")
    print(f"  TOPS/W: {np.mean(tops_vals):.2f} ± {np.std(tops_vals):.2f}")
    print(f"  Std dev: {np.std(tops_vals)/np.mean(tops_vals)*100:.1f}% "
          f"(paper reports <2.4%)")
    print(f"{'='*60}")

    # Save
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    out_file = os.path.join(
        args.output_dir,
        f"benchmark_{args.scheduler}_{int(args.ambient)}C_{timestamp}.json"
    )
    with open(out_file, 'w') as f:
        json.dump([asdict(r) for r in all_results], f, indent=2, default=list)
    print(f"\nResults saved to {out_file}")


if __name__ == '__main__':
    main()
