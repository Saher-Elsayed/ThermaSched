# Thermal Model Calibration Guide

## Overview

ThermaSched's extended lumped-parameter thermal model requires per-device
calibration to extract the five parameter sets in Table 1 of the paper.
The calibration procedure takes approximately **15 minutes** on the ZCU102 board.

## Parameters to Calibrate

| Symbol | Description | Typical range |
|--------|-------------|---------------|
| C_j | Thermal capacitance (J/°C) | 1.35–1.45 |
| G_j | Vertical conductance (W/°C) | 0.30–0.39 |
| τ_j = C_j/G_j | Thermal time constant (s) | 3.5–4.8 |
| G_jk | Adjacent lateral conductance (W/°C) | 0.036–0.047 |
| C_jl | Non-adjacent coupling (W/°C) | 0.010–0.014 |

**Important**: C_jl ≈ 28–32% of G_jk — this is physically substantial and
should not be treated as a small correction (see paper §4.2 and RA's review).

## Step-by-Step Calibration

### Step 1: Install calibration firmware

```bash
./scripts/flash_firmware.sh \
  --bitstream hardware/calibration_design.bit \
  --elf build/calibration_fw.elf
```

### Step 2: Run automated calibration

```bash
python calibration/calibrate_all.py \
  --board /dev/ttyUSB0 \
  --output data/measurements/thermal_params.csv \
  --power 2.5
```

This runs four phases:
1. **Vertical calibration** (4 × 120s): Extract C_j, G_j, τ_j per tile
2. **Adjacent coupling** (3 × 60s): Extract G_01, G_12, G_23
3. **Non-adjacent coupling** (2 × 90s): Extract C_02, C_13
4. **Cross-validation** (1 × 120s): 500s multi-workload verification

### Step 3: Verify parameters

```bash
python calibration/verify_params.py \
  --params data/measurements/thermal_params.csv
```

Expected output:
```
=== ThermaSched Thermal Parameter Verification (Table 1) ===

C_j[0]: got=1.420, expected=1.420, err=0.0%  [OK]
C_j[1]: got=1.380, expected=1.380, err=0.0%  [OK]
...
G_j[0]: got=0.302, expected=0.302, err=0.0%  [OK]
G_j[1]: got=0.383, expected=0.383, err=0.0%  [OK]
...
Euler stability: dt=0.1s, 2*tau_min=7.2s  [PASS]
G_j consistency (G_j = C_j/tau_j):
  T0: C_j/tau_j=0.3021, G_j=0.3020, diff=0.0001  [OK]
  ...
Overall: ALL CHECKS PASSED
```

## G_j Asymmetry Note

Table 1 shows inner tiles T1 and T2 have higher vertical conductance
(0.383, 0.365 W/°C) than edge tiles T0 and T3 (0.302, 0.320 W/°C).

This is **not a physical conductance difference** but a calibration artifact:
during single-tile step-response measurement, inner tiles are flanked by
already-heated neighboring tiles. This compresses the effective die-to-ambient
gradient, producing an apparent higher G_j.

If you perform **simultaneous multi-tile calibration** (all tiles powered
simultaneously), the G_j asymmetry should reduce. Single-tile sequential
calibration is simpler and sufficient for ThermaSched's scheduling accuracy
requirements (MAE = 0.9°C at 100ms horizon).

## Recalibration Schedule

After **30–50 hours** of continuous operation, SYSMON thermal diodes
may exhibit cumulative drift of ~0.5°C due to self-heating and junction
resistance aging. Recalibration takes 15 minutes (offline).

Energy cost: 15 W × 15 min = 13.5 Wh per cycle = 0.3–0.6% of operational energy.

## Calibration on a Different Board

If deploying on a different ZU9EG board (or a different FPGA device entirely),
run the full calibration procedure. Parameter values will differ from Table 1
based on:
- PCB thermal via placement (affects G_jk asymmetry)
- BRAM column boundaries (affects G_jk at T1-T2 boundary)
- Package thermal resistance (affects G_j baseline)
- Junction temperature at given power level (affects C_j)

The calibration procedure is device-agnostic; only the resulting parameters
differ.
