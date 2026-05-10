# tile_pblocks.xdc — Vivado Pblock Constraints for ThermaSched 4-Tile Configuration
#
# Defines reconfigurable partitions (Pblocks) for the four DNN compute tiles
# on Xilinx ZU9EG (XCZU9EG-2FFVB1156) with the ZCU102 evaluation board.
#
# Physical layout: Linear row along X-axis
#   T0: Left edge   (~3.2mm × 2.8mm, borders PCB thermal vias)
#   T1: Inner left  (~3.2mm × 2.8mm)
#   T2: Inner right (~3.2mm × 2.8mm, BRAM column boundary at T1-T2 boundary)
#   T3: Right edge  (~3.2mm × 2.8mm, borders PCB thermal vias)
#
# Note on G_j asymmetry (Table 1):
#   Inner tiles T1,T2 show higher calibrated G_j (0.383, 0.365 W/°C) than
#   edge tiles T0,T3 (0.302, 0.320 W/°C). This is attributed to a
#   calibration-condition artifact (see §3.1) rather than a floorplan effect.
#
# Reference: ThermaSched §3.1 (Tile Floorplan and Physical Adjacency)
# Vivado version: 2023.1
# Device: XCZU9EG-2FFVB1156

# ── Tile T0 (Left edge) ─────────────────────────────────────────────────────
# Resources: DSP columns X0Y0-X0Y3, LUT slices, BRAM
# Thermal diode placement: geometric center of Pblock

create_pblock pblock_tile0
add_cells_to_pblock [get_pblocks pblock_tile0] [get_cells -quiet [list design_1_i/tile0]]
resize_pblock [get_pblocks pblock_tile0] -add {SLICE_X0Y0:SLICE_X49Y59}
resize_pblock [get_pblocks pblock_tile0] -add {DSP48E2_X0Y0:DSP48E2_X2Y23}
resize_pblock [get_pblocks pblock_tile0] -add {RAMB18_X0Y0:RAMB18_X1Y23}
resize_pblock [get_pblocks pblock_tile0] -add {RAMB36_X0Y0:RAMB36_X1Y11}
set_property SNAPPING_MODE ON [get_pblocks pblock_tile0]
set_property IS_SOFT FALSE [get_pblocks pblock_tile0]

# SYSMON thermal diode assignment for T0
set_property LOC XADC_X0Y0 [get_cells -quiet {design_1_i/tile0/sysmon_inst}]

# ── Tile T1 (Inner left) ─────────────────────────────────────────────────────
# Note: Adjacent to T0 (sharing X=50 boundary) and T2 (sharing X=100 boundary)
# BRAM column boundary is between T1 and T2 at approximately X=100 in device coords

create_pblock pblock_tile1
add_cells_to_pblock [get_pblocks pblock_tile1] [get_cells -quiet [list design_1_i/tile1]]
resize_pblock [get_pblocks pblock_tile1] -add {SLICE_X50Y0:SLICE_X99Y59}
resize_pblock [get_pblocks pblock_tile1] -add {DSP48E2_X3Y0:DSP48E2_X5Y23}
resize_pblock [get_pblocks pblock_tile1] -add {RAMB18_X2Y0:RAMB18_X3Y23}
resize_pblock [get_pblocks pblock_tile1] -add {RAMB36_X2Y0:RAMB36_X3Y11}
set_property SNAPPING_MODE ON [get_pblocks pblock_tile1]
set_property IS_SOFT FALSE [get_pblocks pblock_tile1]

set_property LOC XADC_X0Y1 [get_cells -quiet {design_1_i/tile1/sysmon_inst}]

# ── Tile T2 (Inner right) ────────────────────────────────────────────────────
# BRAM column structure differs from T1 side — contributes to G_12 asymmetry

create_pblock pblock_tile2
add_cells_to_pblock [get_pblocks pblock_tile2] [get_cells -quiet [list design_1_i/tile2]]
resize_pblock [get_pblocks pblock_tile2] -add {SLICE_X100Y0:SLICE_X149Y59}
resize_pblock [get_pblocks pblock_tile2] -add {DSP48E2_X6Y0:DSP48E2_X8Y23}
resize_pblock [get_pblocks pblock_tile2] -add {RAMB18_X4Y0:RAMB18_X5Y23}
resize_pblock [get_pblocks pblock_tile2] -add {RAMB36_X4Y0:RAMB36_X5Y11}
set_property SNAPPING_MODE ON [get_pblocks pblock_tile2]
set_property IS_SOFT FALSE [get_pblocks pblock_tile2]

set_property LOC XADC_X0Y2 [get_cells -quiet {design_1_i/tile2/sysmon_inst}]

# ── Tile T3 (Right edge) ─────────────────────────────────────────────────────
# Borders device right edge — PCB thermal via proximity causes G_23 asymmetry

create_pblock pblock_tile3
add_cells_to_pblock [get_pblocks pblock_tile3] [get_cells -quiet [list design_1_i/tile3]]
resize_pblock [get_pblocks pblock_tile3] -add {SLICE_X150Y0:SLICE_X199Y59}
resize_pblock [get_pblocks pblock_tile3] -add {DSP48E2_X9Y0:DSP48E2_X11Y23}
resize_pblock [get_pblocks pblock_tile3] -add {RAMB18_X6Y0:RAMB18_X7Y23}
resize_pblock [get_pblocks pblock_tile3] -add {RAMB36_X6Y0:RAMB36_X7Y11}
set_property SNAPPING_MODE ON [get_pblocks pblock_tile3]
set_property IS_SOFT FALSE [get_pblocks pblock_tile3]

set_property LOC XADC_X0Y3 [get_cells -quiet {design_1_i/tile3/sysmon_inst}]

# ── Static region (ICAP3E controller, AXI crossbar, scheduler interface) ─────

create_pblock pblock_static
add_cells_to_pblock [get_pblocks pblock_static] [get_cells -quiet [list design_1_i/static_region]]
resize_pblock [get_pblocks pblock_static] -add {SLICE_X200Y0:SLICE_X229Y119}
resize_pblock [get_pblocks pblock_static] -add {DSP48E2_X12Y0:DSP48E2_X13Y47}
resize_pblock [get_pblocks pblock_static] -add {RAMB18_X8Y0:RAMB18_X8Y47}
resize_pblock [get_pblocks pblock_static] -add {RAMB36_X8Y0:RAMB36_X8Y23}
set_property SNAPPING_MODE ON [get_pblocks pblock_static]

# ── Decoupler placement constraints ───────────────────────────────────────────
# PR decouplers prevent tile outputs from corrupting the NoC during DPR
set_property LOC SLICE_X49Y29 [get_cells -quiet {design_1_i/tile0_decoupler}]
set_property LOC SLICE_X99Y29 [get_cells -quiet {design_1_i/tile1_decoupler}]
set_property LOC SLICE_X149Y29 [get_cells -quiet {design_1_i/tile2_decoupler}]
set_property LOC SLICE_X199Y29 [get_cells -quiet {design_1_i/tile3_decoupler}]

# ── Clock constraints ─────────────────────────────────────────────────────────
# MMCM fractional-divider mode: pre-loaded dividers for 25 MHz steps
# Allows lock re-acquisition in <4 µs without output clock gating
# Reference: Xilinx PG065 (MMCM/PLL User Guide)

create_clock -period 3.333 -name clk_tile0 [get_ports clk_tile0_p]
create_clock -period 3.333 -name clk_tile1 [get_ports clk_tile1_p]
create_clock -period 3.333 -name clk_tile2 [get_ports clk_tile2_p]
create_clock -period 3.333 -name clk_tile3 [get_ports clk_tile3_p]
create_clock -period 6.667 -name clk_axi   [get_ports clk_axi_p]

# Cross-clock domain constraints (tile clocks are asynchronous to AXI)
set_clock_groups -asynchronous \
    -group {clk_tile0} \
    -group {clk_tile1} \
    -group {clk_tile2} \
    -group {clk_tile3} \
    -group {clk_axi}

# ── I/O timing ────────────────────────────────────────────────────────────────
set_input_delay  -clock clk_axi 1.0 [get_ports {axi_*}]
set_output_delay -clock clk_axi 1.0 [get_ports {axi_*}]
