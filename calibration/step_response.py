#!/usr/bin/env python3
"""
step_response.py — Thermal Model Calibration via Step-Response Measurement

Applies a known constant-power workload to each tile individually and records
the SYSMON temperature transient. Extracts C_j, G_j, and tau_j via exponential
curve fitting (R² > 0.98 required).

Also extracts adjacent lateral conductances G_jk by heating tile j while
tile k is idle, measuring steady-state elevation on tile k.

Usage:
    python step_response.py --board /dev/ttyUSB0 --output thermal_params.csv

Hardware requirements:
    - ZCU102 board running calibration firmware (calibration_fw.elf)
    - TI INA226 current sensors on each PL tile supply rail
    - Ambient temperature sensor (external ADC via PMOD)

Estimated runtime: ~15 minutes (120s per tile × 4 tiles, plus lateral sweeps)

Reference: ThermaSched §3.1 and §4.2.1, Table 1
"""

import argparse
import csv
import time
import numpy as np
from scipy.optimize import curve_fit
from scipy.stats import pearsonr
import serial
import logging

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s [%(levelname)s] %(message)s')
log = logging.getLogger(__name__)


# ── Physical constants and expected parameter ranges ─────────────────────────

EXPECTED_C_J  = [1.42, 1.38, 1.35, 1.41]   # J/°C  (±15% tolerance)
EXPECTED_G_J  = [0.302, 0.383, 0.365, 0.320] # W/°C  (±15% tolerance)
EXPECTED_TAU  = [4.70, 3.60, 3.70, 4.40]     # s     (±20% tolerance)
EXPECTED_G_JK = [0.042, 0.038, 0.045]         # W/°C adjacent
EXPECTED_C_JL = [0.012, 0.011]                # W/°C non-adjacent
CALIBRATION_POWER_W = 2.5   # Known constant power applied during calibration
STEP_DURATION_S     = 120   # Duration of each step-response measurement
SAMPLE_RATE_HZ      = 10    # SYSMON sampling rate


# ── Thermal model functions ───────────────────────────────────────────────────

def thermal_step_response(t, T_inf, tau, T0):
    """
    First-order exponential rise: T(t) = T_inf - (T_inf - T0) * exp(-t/tau)
    where T_inf = T0 + P/G_j (steady-state), tau = C_j/G_j
    """
    return T_inf - (T_inf - T0) * np.exp(-t / tau)


def fit_step_response(t, T, tile_id):
    """
    Fit the exponential step-response to extract tau_j, C_j, G_j.
    Returns (C_j, G_j, tau_j, r_squared, T_inf)
    """
    T0 = T[0]
    T_inf_init = T[-1]

    try:
        popt, pcov = curve_fit(
            thermal_step_response,
            t, T,
            p0=[T_inf_init, 4.0, T0],
            bounds=([T0, 0.5, T0-2], [T0+50, 15.0, T0+5]),
            maxfev=5000
        )
        T_inf, tau, T0_fit = popt

        # R² goodness of fit
        T_pred = thermal_step_response(t, *popt)
        r_sq, _ = pearsonr(T, T_pred)
        r_sq = r_sq ** 2

        if r_sq < 0.98:
            log.warning(f"Tile T{tile_id}: R²={r_sq:.4f} < 0.98 — check calibration")

        # Extract parameters
        G_j = CALIBRATION_POWER_W / (T_inf - T0)
        C_j = tau * G_j

        log.info(f"Tile T{tile_id}: tau={tau:.2f}s, C_j={C_j:.3f}J/°C, "
                 f"G_j={G_j:.3f}W/°C, R²={r_sq:.4f}")
        return C_j, G_j, tau, r_sq, T_inf

    except RuntimeError as e:
        log.error(f"Tile T{tile_id}: Curve fit failed: {e}")
        return None


def measure_lateral_coupling(board, j, k, duration_s=60):
    """
    Heat tile j, measure steady-state elevation on idle tile k.
    Returns G_jk = P_j * delta_T_k / (delta_T_j * delta_T_k) [simplified]

    More precisely: at steady state, heat conducted from j to k:
        q_jk = G_jk * (theta_j - theta_k)
    This heat is conducted away via tile k's vertical conductance:
        q_jk = G_k * (theta_k - theta_amb)
    So: G_jk = G_k * (theta_k - theta_amb) / (theta_j - theta_k)
    """
    log.info(f"Measuring lateral coupling G_{j}{k}: heating T{j}, monitoring T{k}")

    board.write(f"HEAT {j} {CALIBRATION_POWER_W:.2f}\n".encode())
    time.sleep(duration_s)

    # Read steady-state temperatures from all tiles
    board.write(b"READ_TEMPS\n")
    line = board.readline().decode().strip()
    temps = [float(x) for x in line.split(',')]

    board.write(f"COOL {j}\n".encode())
    time.sleep(30)  # Allow to cool

    theta_j   = temps[j]
    theta_k   = temps[k]
    theta_amb = temps[4]  # External ambient sensor

    G_k = EXPECTED_G_J[k]  # Use previously calibrated value
    G_jk = G_k * (theta_k - theta_amb) / (theta_j - theta_k + 1e-6)

    log.info(f"G_{j}{k} = {G_jk:.4f} W/°C "
             f"(θ_j={theta_j:.1f}, θ_k={theta_k:.1f}, θ_amb={theta_amb:.1f})")
    return G_jk


# ── Main calibration pipeline ─────────────────────────────────────────────────

def run_calibration(port, output_path):
    """Full calibration pipeline: step-response for C_j/G_j + lateral G_jk."""

    log.info(f"Connecting to board at {port}")
    board = serial.Serial(port, baudrate=115200, timeout=5.0)
    time.sleep(2.0)

    results = {
        'C_j': [], 'G_j': [], 'tau_j': [], 'r_sq': [],
        'G_jk': [], 'C_jl': []
    }

    # ── Step 1: Per-tile step-response calibration ─────────────────────────
    for tile_id in range(4):
        log.info(f"\n{'='*50}")
        log.info(f"Step-response calibration: Tile T{tile_id}")
        log.info(f"{'='*50}")

        # Apply constant power workload
        board.write(f"APPLY_WORKLOAD {tile_id} {CALIBRATION_POWER_W:.2f}\n".encode())
        time.sleep(1.0)

        # Read ambient temperature
        board.write(b"READ_AMBIENT\n")
        T_amb = float(board.readline().decode().strip())
        log.info(f"Ambient: {T_amb:.1f}°C")

        # Collect temperature trace
        n_samples = STEP_DURATION_S * SAMPLE_RATE_HZ
        t_arr = np.linspace(0, STEP_DURATION_S, n_samples)
        T_arr = np.zeros(n_samples)

        log.info(f"Collecting {n_samples} samples over {STEP_DURATION_S}s...")
        for i in range(n_samples):
            board.write(f"READ_TILE_TEMP {tile_id}\n".encode())
            T_arr[i] = float(board.readline().decode().strip())
            time.sleep(1.0 / SAMPLE_RATE_HZ)

        # Cool down
        board.write(f"STOP_WORKLOAD {tile_id}\n".encode())
        log.info(f"Cooling tile T{tile_id}...")
        time.sleep(60)

        # Fit curve
        fit = fit_step_response(t_arr, T_arr, tile_id)
        if fit is None:
            log.error(f"Calibration FAILED for tile T{tile_id}")
            board.close()
            return -1

        C_j, G_j, tau, r_sq, T_inf = fit
        results['C_j'].append(C_j)
        results['G_j'].append(G_j)
        results['tau_j'].append(tau)
        results['r_sq'].append(r_sq)

        # Warn if parameters are far from expected values
        tol = 0.20
        if abs(C_j - EXPECTED_C_J[tile_id]) / EXPECTED_C_J[tile_id] > tol:
            log.warning(f"T{tile_id}: C_j={C_j:.3f} deviates >{tol*100:.0f}% "
                        f"from expected {EXPECTED_C_J[tile_id]:.3f}")

    # ── Step 2: Adjacent lateral coupling G_jk ─────────────────────────────
    log.info("\nMeasuring adjacent lateral conductances G_jk...")
    pairs = [(0, 1), (1, 2), (2, 3)]
    for j, k in pairs:
        G_jk = measure_lateral_coupling(board, j, k)
        results['G_jk'].append(G_jk)

    # ── Step 3: Non-adjacent coupling C_jl ────────────────────────────────
    log.info("\nMeasuring non-adjacent coupling C_jl...")
    non_adj_pairs = [(0, 2), (1, 3)]
    for j, l in non_adj_pairs:
        C_jl = measure_lateral_coupling(board, j, l, duration_s=90)
        results['C_jl'].append(C_jl)
        log.info(f"C_{j}{l} = {C_jl:.4f} W/°C "
                 f"(fraction of G_jk: {C_jl/np.mean(results['G_jk']):.1%})")

    board.close()

    # ── Save results ────────────────────────────────────────────────────────
    with open(output_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['# ThermaSched Thermal Model Parameters'])
        writer.writerow(['# Generated by step_response.py'])
        writer.writerow(['# Columns: parameter, T0, T1, T2, T3'])
        writer.writerow(['C_j'] + [f'{v:.4f}' for v in results['C_j']])
        writer.writerow(['G_j'] + [f'{v:.4f}' for v in results['G_j']])
        writer.writerow(['tau_j'] + [f'{v:.4f}' for v in results['tau_j']])
        writer.writerow(['r_sq'] + [f'{v:.4f}' for v in results['r_sq']])
        writer.writerow(['G_jk'] + [f'{v:.4f}' for v in results['G_jk']] + [''])
        writer.writerow(['C_jl'] + [f'{v:.4f}' for v in results['C_jl']] + ['', ''])

    log.info(f"\nCalibration complete. Results saved to {output_path}")
    print_summary(results)
    return 0


def print_summary(results):
    """Print calibration summary matching Table 1 of the paper."""
    print("\n" + "="*55)
    print("CALIBRATION SUMMARY (compare against Table 1 of paper)")
    print("="*55)
    print(f"{'Param':<12} {'T0':>8} {'T1':>8} {'T2':>8} {'T3':>8}")
    print("-"*55)
    print(f"{'C_j (J/°C)':<12} " +
          " ".join(f"{v:>8.3f}" for v in results['C_j']))
    print(f"{'G_j (W/°C)':<12} " +
          " ".join(f"{v:>8.3f}" for v in results['G_j']))
    print(f"{'tau_j (s)':<12} " +
          " ".join(f"{v:>8.2f}" for v in results['tau_j']))
    print(f"{'R²':<12} " +
          " ".join(f"{v:>8.4f}" for v in results['r_sq']))
    print("-"*55)
    pairs = ['G_01', 'G_12', 'G_23']
    print("Adjacent conductances G_jk (W/°C):")
    for name, val in zip(pairs, results['G_jk']):
        print(f"  {name} = {val:.4f}")
    print("Non-adjacent conductances C_jl (W/°C):")
    nap = ['C_02', 'C_13']
    for name, val in zip(nap, results['C_jl']):
        g_avg = np.mean(results['G_jk'])
        print(f"  {name} = {val:.4f}  ({val/g_avg:.1%} of mean G_jk)")

    tau_min = min(results['tau_j'])
    dt = 0.1
    print(f"\nEuler stability: dt={dt}s, 2*tau_min={2*tau_min:.1f}s  "
          f"[{'PASS' if dt <= 2*tau_min else 'FAIL'}]")
    print("="*55)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Thermal model step-response calibration')
    parser.add_argument('--board', required=True,
                        help='Serial port (e.g. /dev/ttyUSB0)')
    parser.add_argument('--output', default='../data/measurements/thermal_params.csv',
                        help='Output CSV path')
    args = parser.parse_args()
    exit(run_calibration(args.board, args.output))
