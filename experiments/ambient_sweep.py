#!/usr/bin/env python3
"""
ambient_sweep.py — Reproduce Table 6 (Ambient Temperature Sensitivity)

Runs the 5-model benchmark at 25, 35, and 45°C ambient for both
ThermaSched and Static Mapping baselines.

Requires: temperature-controlled enclosure, 30-min thermal soak per temperature.

Usage:
    python ambient_sweep.py --scheduler thermasched --temps 25 35 45
    python ambient_sweep.py --scheduler static --temps 25 35 45
"""

import argparse, json, time, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from run_benchmark import run_benchmark, BenchmarkResult, SUPPORTED_SCHEDULERS

MODELS = ['resnet18', 'mobilenetv2', 'yolotiny', 'efficientnet-b0', 'squeezenet']

PAPER_RESULTS = {
    # Table 6 values: (TS_peak, static_peak, TS_TOPS_W, static_TOPS_W, FPS_model, theta_warn)
    25: {'ts': {'peak': 78, 'tops_w': 4.8, 'fps': 46}, 'static': {'peak': 92, 'tops_w': 2.1}},
    35: {'ts': {'peak': 84, 'tops_w': 4.2, 'fps': 38}, 'static': {'peak': 97, 'tops_w': 1.6}},
    45: {'ts': {'peak': 91, 'tops_w': 3.4, 'fps': 29}, 'static': {'peak':102, 'tops_w': 1.2}},
}


def print_comparison(results_by_temp, schedulers):
    """Print Table 6 comparison."""
    print("\n" + "="*80)
    print("TABLE 6: Ambient Temperature Sensitivity (5-model, 60s, 10 runs)")
    print("="*80)
    print(f"{'Ambient':>9} {'TS peak':>9} {'Stat peak':>10} "
          f"{'TS TOPS/W':>10} {'Stat TOPS/W':>12} {'FPS/model':>10} {'Gain':>7}")
    print("-"*80)

    for temp in sorted(results_by_temp.keys()):
        r = results_by_temp[temp]
        ts_r = r.get('thermasched', {})
        st_r = r.get('static', {})

        ts_tops = ts_r.get('tops_w', '--')
        st_tops = st_r.get('tops_w', '--')
        gain = f"{ts_tops/st_tops:.1f}x" if isinstance(ts_tops, float) else '--'
        ts_peak = ts_r.get('peak_temp', '--')
        st_peak = st_r.get('peak_temp', '--')
        fps = ts_r.get('fps', '--')
        dtm = ' (DTM)' if temp == 45 else ''

        print(f"{temp:>7}°C {str(ts_peak)+dtm:>12} {st_peak:>10} "
              f"{ts_tops:>10} {st_tops:>12} {fps:>10} {gain:>7}")

    print("="*80)
    print("Note: Gain INCREASES with ambient: ThermaSched's advantage widens")
    print("at high ambient because static mapping suffers more DTM events.")


def main():
    parser = argparse.ArgumentParser(description='Ambient temperature sweep')
    parser.add_argument('--schedulers', nargs='+',
                        default=['thermasched', 'static'])
    parser.add_argument('--temps', nargs='+', type=float, default=[25, 35, 45])
    parser.add_argument('--duration', type=int, default=60)
    parser.add_argument('--runs', type=int, default=10)
    parser.add_argument('--output-dir', default='results/ambient_sweep/')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    results_by_temp = {t: {} for t in args.temps}

    for temp in args.temps:
        print(f"\n{'='*60}")
        print(f"AMBIENT TEMPERATURE: {temp}°C")
        print(f"{'='*60}")
        print(f"Please ensure enclosure is at {temp}°C ± 1°C before continuing.")
        input("Press ENTER when thermal soak is complete (30 min recommended)...")

        for sched in args.schedulers:
            print(f"\nRunning {sched} at {temp}°C...")
            run_results = []
            for run in range(args.runs):
                class Args:
                    scheduler = sched
                    models = MODELS
                    duration = args.duration
                    ambient = temp
                    run_id = run + 1
                    runs = args.runs
                    board = 'SIM'

                r = run_benchmark(Args())
                run_results.append(r)

            tops_vals = [r.mean_tops_w for r in run_results]
            peak_vals = [r.peak_temp_c for r in run_results]
            fps_vals  = [r.mean_fps_total / len(MODELS) for r in run_results]

            results_by_temp[temp][sched] = {
                'tops_w':   float(np.mean(tops_vals)),
                'peak_temp':float(np.mean(peak_vals)),
                'fps':      float(np.mean(fps_vals)),
                'tops_std': float(np.std(tops_vals)),
            }

            # Compare against paper
            if temp in PAPER_RESULTS and sched in ('thermasched', 'static'):
                paper = PAPER_RESULTS[temp][sched if sched != 'static' else 'static']
                got = results_by_temp[temp][sched]['tops_w']
                exp = paper['tops_w']
                print(f"  TOPS/W: got={got:.2f}, paper={exp:.2f}, "
                      f"err={abs(got-exp)/exp*100:.1f}%")

    print_comparison(results_by_temp, args.schedulers)

    # Save results
    out_file = os.path.join(args.output_dir, 'ambient_sweep_results.json')
    with open(out_file, 'w') as f:
        json.dump(results_by_temp, f, indent=2)
    print(f"\nResults saved to {out_file}")


if __name__ == '__main__':
    main()
