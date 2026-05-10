# ThermaSched

**Thermal-Aware Multi-Tenant DNN Workload Scheduling for Energy-Efficient MPSoC FPGA Edge Inference**

[![Paper](https://img.shields.io/badge/Paper-SUSCOM%202025-blue)](https://doi.org/10.xxxx/suscom.2025.xxxxxx)
[![Platform](https://img.shields.io/badge/Platform-Xilinx%20ZU9EG-red)](https://www.xilinx.com/products/silicon-devices/soc/zynq-ultrascale-mpsoc.html)
[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE)
[![Python](https://img.shields.io/badge/Python-3.9%2B-yellow)](https://www.python.org/)

---

## Overview

ThermaSched is a lightweight thermal-aware workload scheduler running on the ARM Cortex-A53 processing system (PS) of a Xilinx Zynq UltraScale+ ZU9EG MPSoC. It manages concurrent DNN inference across four independently reconfigurable PL compute tiles, preventing DTM-induced clock throttling through proactive DVFS and thermally-informed tile assignment.

**Key results (ZU9EG, 5 concurrent ImageNet models, 25°C ambient):**

| Metric | Static Mapping | ThermaSched | Gain |
|--------|---------------|-------------|------|
| Energy efficiency | 2.1 TOPS/W | **4.8 TOPS/W** | **2.3×** |
| Peak tile temperature | 92°C (DTM) | **78°C** | No DTM |
| Per-model throughput (5 models) | 22 FPS | **46 FPS** | **2.1×** |
| Scheduling latency | — | **8.3 µs** | — |
| Scheduling jitter (1σ) | — | **0.9 µs** | — |

---

## Repository Structure

```
thermasched/
├── src/
│   ├── scheduler/          # Core scheduling algorithm (ARM bare-metal C)
│   │   ├── thermasched.c   # Main scheduler loop
│   │   ├── thermasched.h
│   │   ├── scoring.c       # Composite scoring function S(li, tj)
│   │   └── scoring.h
│   ├── thermal/            # Lumped-parameter thermal model
│   │   ├── thermal_model.c # Extended thermal model with non-adjacent coupling
│   │   ├── thermal_model.h
│   │   └── sysmon.c        # SYSMON XADC interface
│   ├── noc/                # Contention-aware AXI4 NoC cost model
│   │   ├── noc_model.c
│   │   └── noc_model.h
│   ├── dvfs/               # Proactive DVFS controller
│   │   ├── dvfs_ctrl.c
│   │   └── dvfs_ctrl.h
│   └── buffer/             # Shared weight buffer management
│       ├── weight_buffer.c
│       └── weight_buffer.h
├── hardware/
│   ├── vivado_constraints/ # Xilinx XDC Pblock constraints for 4 tiles
│   │   └── tile_pblocks.xdc
│   └── ip_cores/           # AXI4 crossbar and ICAP3E controller configs
│       ├── axi_crossbar_cfg.tcl
│       └── icap3e_ctrl.vhd
├── calibration/            # Thermal model calibration scripts
│   ├── step_response.py    # Step-response measurement and curve fitting
│   ├── calibrate_all.py    # Full calibration pipeline
│   └── verify_params.py    # Parameter verification against Table 1
├── experiments/            # Experiment runners and result analysis
│   ├── run_benchmark.py    # 60-second 5-model benchmark
│   ├── ambient_sweep.py    # 25/35/45°C ambient sensitivity
│   ├── ablation_study.py   # Component ablation
│   └── analyze_results.py  # Result parsing and table generation
├── simpy_simulation/       # SimPy discrete-event scalability simulation
│   ├── thermasched_sim.py  # Full SimPy model (latency-only)
│   ├── scale_sweep.py      # K=4,8,16,32 latency projection
│   └── validate_sim.py     # 8% agreement verification vs hardware
├── data/
│   ├── measurements/       # Raw measurement data (CSV)
│   │   ├── thermal_params.csv      # Calibrated C_j, G_j, G_jk, C_jl
│   │   ├── sysmon_traces/          # Raw SYSMON temperature logs
│   │   ├── ina226_power/           # Per-rail power measurements
│   │   └── dpr_latency/            # DPR latency distribution data
│   └── models/             # Quantized DNN model weight files (metadata)
│       └── model_manifest.json
├── tests/                  # Unit and integration tests
│   ├── test_thermal_model.py
│   ├── test_scoring.py
│   ├── test_noc_model.py
│   └── test_calibration.py
├── scripts/                # Utility scripts
│   ├── flash_firmware.sh   # Program ARM PS firmware via JTAG
│   ├── setup_env.sh        # Environment setup
│   └── generate_tables.py  # Reproduce paper tables from raw data
├── docs/
│   ├── hardware_setup.md   # ZCU102 board setup and power measurement
│   ├── calibration_guide.md
│   └── api_reference.md
├── requirements.txt
├── CMakeLists.txt          # ARM bare-metal build system
└── LICENSE
```

---

## Hardware Requirements

| Component | Specification |
|-----------|--------------|
| FPGA Board | Xilinx ZCU102 (ZU9EG MPSoC) |
| Vivado version | 2023.1 |
| ARM toolchain | arm-none-eabi-gcc 12.2 |
| Current sensors | Texas Instruments INA226 (×4, one per PL rail) |
| Host PC (calibration) | Any Linux x86-64 with Python 3.9+ |
| JTAG programmer | Xilinx USB Cable (DLC10) or equivalent |

---

## Software Requirements

```
pip install -r requirements.txt
```

See `requirements.txt` for Python dependencies (NumPy, SciPy, SimPy, Matplotlib, Pandas).

---

## Quick Start

### 1. Calibrate the thermal model

```bash
cd calibration/
python calibrate_all.py --board /dev/ttyUSB0 --output ../data/measurements/thermal_params.csv
```

This runs the 15-minute step-response calibration on the physical board and extracts C_j, G_j, G_jk, C_jl for all four tiles.

### 2. Verify calibrated parameters

```bash
python verify_params.py --params ../data/measurements/thermal_params.csv
```

Expected output matches Table 1 of the paper:

```
Tile T0: C=1.42 J/°C  G=0.302 W/°C  tau=4.70 s
Tile T1: C=1.38 J/°C  G=0.383 W/°C  tau=3.60 s
Tile T2: C=1.35 J/°C  G=0.365 W/°C  tau=3.70 s
Tile T3: C=1.41 J/°C  G=0.320 W/°C  tau=4.40 s
Euler stability: dt=0.1s << 2*tau_min=7.2s  [PASS]
```

### 3. Build and flash the scheduler firmware

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
make -j4
cd ../scripts
./flash_firmware.sh --bitstream ../hardware/top_design.bit --elf ../build/thermasched.elf
```

### 4. Run the 5-model benchmark

```bash
cd experiments/
python run_benchmark.py --duration 60 --models resnet18 mobilenetv2 yolotiny efficientnet-b0 squeezenet
```

### 5. Run the SimPy scalability simulation

```bash
cd simpy_simulation/
python scale_sweep.py --tiles 4 8 16 32 --runs 100
```

---

## Reproducing Paper Results

All tables and figures in the paper can be reproduced from raw measurement data:

```bash
python scripts/generate_tables.py --data data/measurements/ --output results/
```

This generates:
- `results/table1_thermal_params.csv` — Table 1 (thermal model parameters)
- `results/table2_model_suite.csv` — Table 2 (8-model energy efficiency)
- `results/table5_scheduler_comparison.csv` — Table 5 (scheduler comparison)
- `results/table6_ambient.csv` — Table 6 (ambient temperature sensitivity)
- `results/figure2a_temperature.pdf` — Figure 2a (temperature vs. time)
- `results/figure4b_mae_horizon.pdf` — Figure 4b (MAE vs. prediction horizon)

---

## Thermal Model Parameters (Table 1)

| Parameter | T0 | T1 | T2 | T3 |
|-----------|----|----|----|----|
| C_j (J/°C) | 1.42 | 1.38 | 1.35 | 1.41 |
| G_j (W/°C) | 0.302 | 0.383 | 0.365 | 0.320 |
| τ_j (s) | 4.70 | 3.60 | 3.70 | 4.40 |

Adjacent: G_01=0.042, G_12=0.038, G_23=0.045 W/°C
Non-adjacent: C_02=0.012, C_13=0.011 W/°C

---

## Citation

```bibtex
@article{elsayed2025thermasched,
  title     = {{ThermaSched}: Thermal-Aware Multi-Tenant {DNN} Workload Scheduling
               for Energy-Efficient {MPSoC} {FPGA} Edge Inference},
  author    = {anny},
  journal   = {Sustainable Computing: Informatics and Systems},
  year      = {2025},
  doi       = {10.xxxx/suscom.2025.xxxxxx},
  note      = {Implemented on Xilinx ZU9EG MPSoC. Code: https://github.com/selsayed25/ThermaSched}
}
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.

---

## Contact

ann
Department of Computer and Information Science, University of Pennsylvania
