/**
 * thermasched.h — ThermaSched: Thermal-Aware Multi-Tenant DNN Scheduler
 *
 * Main scheduler interface for ARM Cortex-A53 bare-metal deployment on
 * Xilinx Zynq UltraScale+ ZU9EG MPSoC.
 *
 * Scheduling decision executes in 8.3 µs:
 *   1.2 µs — SYSMON sensor read
 *   3.8 µs — Score compute (O(NK) with N=20 layers, K=4 tiles)
 *   2.1 µs — Tile selection + DVFS CMT update
 *   1.2 µs — DPR command dispatch
 *
 * Author: Saher Elsayed <selsayed@seas.upenn.edu>
 * Paper:  ThermaSched, SUSCOM 2025
 */

#ifndef THERMASCHED_H
#define THERMASCHED_H

#include <stdint.h>
#include <stdbool.h>
#include "thermal_model.h"
#include "scoring.h"
#include "dvfs_ctrl.h"
#include "weight_buffer.h"

/* ── Configuration ──────────────────────────────────────────────────────── */

#define TS_NUM_TILES        4       /**< Number of reconfigurable PL tiles   */
#define TS_MAX_LAYERS       32      /**< Maximum pending DNN layers per epoch */
#define TS_EPOCH_MS         100     /**< Scheduling epoch in milliseconds     */
#define TS_DTM_THRESHOLD_C  85.0f  /**< DTM activation temperature (°C)      */
#define TS_WARN_BASE_C      75.0f  /**< θ_warn at 25°C ambient               */
#define TS_DPR_LATENCY_US   2100   /**< Worst-case ICAP3E DPR latency (µs)  */
#define TS_DVFS_STEP_MHZ    25     /**< DVFS frequency step size (MHz)       */
#define TS_FREQ_MIN_MHZ     100    /**< Minimum tile clock frequency (MHz)   */
#define TS_FREQ_MAX_MHZ     300    /**< Maximum tile clock frequency (MHz)   */

/* ── Scoring weights (grid-searched, α+β+γ+δ=1) ─────────────────────── */
#define ALPHA   0.40f   /**< Thermal headroom weight                         */
#define BETA    0.20f   /**< NoC contention weight                           */
#define GAMMA   0.25f   /**< Reconfiguration penalty weight                  */
#define DELTA   0.15f   /**< Weight-buffer cache efficiency weight           */

/* ── Data structures ────────────────────────────────────────────────────── */

/** DNN model types supported by the systolic array tiles */
typedef enum {
    MODEL_RESNET18        = 0,
    MODEL_MOBILENETV2     = 1,
    MODEL_YOLOTINY        = 2,
    MODEL_EFFICIENTNET_B0 = 3,
    MODEL_SQUEEZENET      = 4,
    MODEL_VIT_TINY        = 5,
    MODEL_MOBILENETV3_L   = 6,
    MODEL_EFFICIENTNET_B2 = 7,
    MODEL_UNKNOWN         = 0xFF,
} model_id_t;

/** Tile precision modes supported by CMT-configurable clocking */
typedef enum {
    PREC_INT4  = 0,
    PREC_INT8  = 1,
    PREC_INT16 = 2,
} tile_precision_t;

/** Tile state: thermal, clock, and assignment status */
typedef struct {
    float        temp_c;           /**< Current temperature from SYSMON (°C)       */
    float        temp_predicted_c; /**< Predicted temperature after assignment       */
    uint32_t     freq_mhz;         /**< Current CMT output frequency (MHz)          */
    model_id_t   current_model;    /**< Model type currently loaded                 */
    tile_precision_t precision;    /**< Active precision mode                       */
    bool         is_reconfiguring; /**< True if DPR in progress                     */
    uint32_t     dpr_remaining_us; /**< Remaining DPR time if reconfiguring (µs)   */
    float        utilization;      /**< MAC utilization over last epoch (0–1)       */
} tile_state_t;

/** Pending DNN layer descriptor */
typedef struct {
    uint32_t    layer_id;          /**< Global layer identifier                     */
    model_id_t  model_id;          /**< Parent model                                */
    uint32_t    mac_ops;           /**< MAC operations (compute requirement c_i)    */
    uint32_t    weight_bytes;      /**< Weight tensor size in bytes (w_i)           */
    uint8_t     tile_type_needed;  /**< Required tile type (0=conv, 1=attn, 2=fc)  */
    bool        is_transformer;    /**< True for attention layers                   */
} layer_desc_t;

/** Per-epoch scheduling result */
typedef struct {
    uint8_t     assignment[TS_MAX_LAYERS]; /**< Layer→tile assignments (tile index)  */
    uint32_t    num_layers;                /**< Number of layers scheduled            */
    uint32_t    dvfs_cmds;                 /**< Number of DVFS step-downs issued      */
    uint32_t    dpr_cmds;                  /**< Number of DPR commands issued         */
    uint64_t    decision_time_ns;          /**< Total decision latency (ns)           */
    float       predicted_peak_temp_c;     /**< Predicted peak tile temperature       */
} schedule_result_t;

/** Global scheduler context */
typedef struct {
    tile_state_t    tiles[TS_NUM_TILES];
    thermal_model_t thermal;
    noc_model_t     noc;
    weight_buffer_t wbuf;
    dvfs_controller_t dvfs;
    float           ambient_temp_c;       /**< Measured ambient (from external ADC) */
    uint64_t        epoch_count;          /**< Total epochs completed               */
    uint32_t        dvfs_events_total;    /**< DVFS events across all epochs        */
    uint32_t        dpr_events_total;     /**< DPR events across all epochs         */
    uint32_t        dtm_events_total;     /**< DTM activations (should be 0 @ 25°C) */
} thermasched_ctx_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * Initialize the ThermaSched context.
 * Loads calibrated thermal parameters, configures SYSMON, initializes
 * weight buffer, and sets up the DVFS CMT in fractional-divider mode.
 *
 * @param ctx     Scheduler context to initialize
 * @param params  Path to thermal parameter CSV (NULL = use embedded defaults)
 * @return        0 on success, negative error code on failure
 */
int thermasched_init(thermasched_ctx_t *ctx, const char *params_path);

/**
 * Execute one scheduling epoch (target: 8.3 µs).
 *
 * Pipeline:
 *   1. Read SYSMON temperatures      [1.2 µs]
 *   2. Predict temperatures via Eq.2 [included in score compute]
 *   3. Sort layers by MAC ops desc   [O(N log N)]
 *   4. Score all (layer, tile) pairs [O(NK), 3.8 µs]
 *   5. Greedy assignment             [O(N)]
 *   6. Issue DVFS commands           [2.1 µs]
 *   7. Dispatch DPR commands         [1.2 µs]
 *
 * @param ctx    Scheduler context
 * @param layers Pending layer queue
 * @param n      Number of pending layers (≤ TS_MAX_LAYERS)
 * @param result Output scheduling result
 * @return       0 on success
 */
int thermasched_run_epoch(thermasched_ctx_t *ctx,
                          const layer_desc_t *layers, uint32_t n,
                          schedule_result_t *result);

/**
 * Compute the adaptive DVFS warning threshold θ_warn as a function
 * of measured ambient temperature.
 *
 * θ_warn = 75°C at 25°C ambient
 * θ_warn = 73°C at 35°C ambient
 * θ_warn = 71°C at 45°C ambient
 * Linear interpolation for intermediate values.
 *
 * @param ambient_c  Measured ambient temperature (°C)
 * @return           Adaptive θ_warn in °C
 */
float thermasched_compute_theta_warn(float ambient_c);

/**
 * Dump scheduler statistics to UART for debugging.
 * Reports per-tile temperature, utilization, DVFS event rate, and
 * worst-case epoch latency observed.
 *
 * @param ctx  Scheduler context
 */
void thermasched_dump_stats(const thermasched_ctx_t *ctx);

/**
 * Graceful shutdown: complete any in-progress DPR, restore all tiles to
 * maximum frequency, and write final statistics to shared memory.
 *
 * @param ctx  Scheduler context
 */
void thermasched_shutdown(thermasched_ctx_t *ctx);

#endif /* THERMASCHED_H */
