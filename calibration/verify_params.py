#!/usr/bin/env python3
"""
verify_params.py — Verify calibrated thermal parameters against Table 1.

Usage:
    python verify_params.py --params ../data/measurements/thermal_params.csv
"""
import argparse, csv, sys, math

EXPECTED = {
    'C_j':   [1.42, 1.38, 1.35, 1.41],
    'G_j':   [0.302, 0.383, 0.365, 0.320],
    'tau_j': [4.70, 3.60, 3.70, 4.40],
    'G_jk':  [0.042, 0.038, 0.045],
    'C_jl':  [0.012, 0.011],
}
TOL = 0.15  # 15% tolerance

def load_csv(path):
    params = {}
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].startswith('#'): continue
            key = row[0]
            vals = [float(v) for v in row[1:] if v.strip()]
            params[key] = vals
    return params

def check(name, got, expected, tol=TOL):
    ok = True
    for i, (g, e) in enumerate(zip(got, expected)):
        err = abs(g - e) / e
        status = 'OK' if err <= tol else 'WARN'
        if err > tol: ok = False
        print(f"  {name}[{i}]: got={g:.4f}, expected={e:.4f}, err={err:.1%}  [{status}]")
    return ok

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--params', default='../data/measurements/thermal_params.csv')
    args = parser.parse_args()

    params = load_csv(args.params)
    all_ok = True

    print("\n=== ThermaSched Thermal Parameter Verification (Table 1) ===\n")

    for key in ['C_j', 'G_j', 'tau_j']:
        if key not in params:
            print(f"MISSING: {key}"); all_ok = False; continue
        all_ok &= check(key, params[key], EXPECTED[key])

    if 'G_jk' in params:
        all_ok &= check('G_jk', params['G_jk'], EXPECTED['G_jk'])
    if 'C_jl' in params:
        all_ok &= check('C_jl', params['C_jl'], EXPECTED['C_jl'])

    # Euler stability
    tau_min = min(params.get('tau_j', [3.6]))
    dt = 0.1
    euler_ok = dt <= 2 * tau_min
    print(f"\nEuler stability: dt={dt}s, 2*tau_min={2*tau_min:.1f}s  "
          f"[{'PASS' if euler_ok else 'FAIL'}]")
    all_ok &= euler_ok

    # G_j consistency: G_j should equal C_j / tau_j
    print("\nG_j consistency (G_j = C_j/tau_j):")
    for j in range(4):
        derived = params['C_j'][j] / params['tau_j'][j]
        diff = abs(derived - params['G_j'][j])
        ok = diff < 0.005
        print(f"  T{j}: C_j/tau_j={derived:.4f}, G_j={params['G_j'][j]:.4f}, "
              f"diff={diff:.4f}  [{'OK' if ok else 'WARN'}]")
        all_ok &= ok

    print(f"\nOverall: {'ALL CHECKS PASSED' if all_ok else 'WARNINGS DETECTED'}")
    return 0 if all_ok else 1

if __name__ == '__main__':
    sys.exit(main())
