# Hardware Setup Guide

## ZCU102 Board Configuration for ThermaSched

### Required Hardware

| Item | Specification |
|------|--------------|
| FPGA Board | Xilinx ZCU102 Rev 1.1 (ZU9EG-2FFVB1156) |
| Current sensors | 4× Texas Instruments INA226 (one per PL supply rail) |
| DC power analyzer | Keysight N6705C (calibration reference, ±2%) |
| Ambient sensor | External ADC via PMOD connector (±0.5°C) |
| JTAG programmer | Xilinx USB Cable DLC10 |

### INA226 Placement

Mount INA226 sensors on the four PL supply rails:

```
Rail        Shunt          Sense voltage      I2C address
─────────────────────────────────────────────────────────
VCCINT_T0   0.01 Ω (100W)  VCCO_HP_T0        0x40
VCCINT_T1   0.01 Ω         VCCO_HP_T1        0x41
VCCINT_T2   0.01 Ω         VCCO_HP_T2        0x44
VCCINT_T3   0.01 Ω         VCCO_HP_T3        0x45
```

Calibration against Keysight N6705C yields ±2% accuracy at 1 kHz sampling.

### SYSMON Thermal Diode Placement

Four remote thermal diodes are placed at the **geometric center** of each
compute tile Pblock during Vivado floorplanning (see `tile_pblocks.xdc`).

SYSMON provides:
- ±1°C accuracy (factory calibrated)
- 10 kHz sampling rate via XADC
- Memory-mapped access from ARM PS (2 clock cycles per read)
- 0.3°C variance under static workloads; 0.6°C under DVFS transitions

### Power Measurement Protocol

```bash
# Calibrate INA226 against N6705C reference
python calibration/calibrate_power_sensors.py --port /dev/ttyUSB0

# Verify calibration
python calibration/verify_params.py --mode power
```

### DVFS Configuration

ThermaSched uses the MMCM in **fractional-divider mode** with pre-loaded
divider settings for each of the 9 supported frequencies (100–300 MHz, 25 MHz steps).

This allows lock re-acquisition in **<4 µs** without gating the output clock.
See Xilinx PG065 (MMCM/PLL User Guide), Section on Dynamic Reconfiguration.

**Critical**: Do not use standard MMCM reconfiguration (which requires clock
gating and 10–100 µs lock time). ThermaSched's 4 µs DVFS claim depends on
fractional-divider mode with pre-loaded divider tables.

### DPR Interface

Partial reconfiguration uses the **ICAP3E internal port** (not PCAP):

| Interface | Throughput | 3.2 MB latency |
|-----------|-----------|----------------|
| PCAP (PS-side) | ~400 MB/s | ~8 ms |
| **ICAP3E (PL-internal)** | ~3.2 GB/s | **~1 ms** |

Measured DPR latency with ICAP3E: **2.1 ms** worst-case (includes ICAP
controller overhead, configuration frame processing, and readback verification).

The ICAP3E controller instantiated in the static PL region consumes ~3,800 LUTs.

### Build and Flash

```bash
# Build Vivado project (requires Vivado 2023.1)
cd hardware/
vivado -mode batch -source build_project.tcl

# Synthesize and implement (approximately 8 hours)
vivado -mode batch -source run_impl.tcl

# Build ARM bare-metal firmware
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
make -j4

# Program board
cd ../scripts
./flash_firmware.sh \
  --bitstream ../hardware/top_design.bit \
  --elf ../build/thermasched.elf
```

### Ambient Temperature Control

For the 35°C and 45°C ambient experiments, place the ZCU102 board in a
**temperature-controlled enclosure** (e.g., environmental test chamber or
insulated box with resistive heaters). Allow 30 minutes of thermal soak
before beginning measurements.

At 45°C ambient, ensure the board's fan is operating correctly — the
ZU9EG has an active fan requirement at elevated junction temperatures.

### Verification Checklist

Before running experiments:

- [ ] INA226 calibrated against Keysight N6705C (≤2% error)
- [ ] SYSMON readings match IR camera at ambient temperature (≤1°C error)
- [ ] MMCM fractional-divider mode confirmed (DVFS transition ≤4 µs)
- [ ] ICAP3E controller functional (DPR of test bitstream completes in ≤2.1 ms)
- [ ] All 4 Pblocks verified in Vivado Device view (no overlap)
- [ ] Thermal calibration complete (thermal_params.csv generated)
- [ ] `verify_params.py` passes all checks
