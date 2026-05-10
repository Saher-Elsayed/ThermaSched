/**
 * thermasched.c — ThermaSched main scheduling loop
 *
 * Implements the 8.3 µs per-epoch scheduling decision described in
 * Algorithm 1 of the paper. Runs on ARM Cortex-A53 bare-metal.
 *
 * Key design constraints:
 *   - No dynamic memory allocation (bare-metal, no OS)
 *   - No floating-point exceptions (NaN/Inf checks on all predictions)
 *   - DVFS via MMCM fractional-divider mode (no clock gating, 4 µs transition)
 *   - DPR via ICAP3E (3.2 GB/s, 2.1 ms worst-case for 3.2 MB bitstream)
 */

#include "thermasched.h"
#include "thermal_model.h"
#include "scoring.h"
#include "noc_model.h"
#include "dvfs_ctrl.h"
#include "weight_buffer.h"
#include "sysmon.h"
#include <string.h>
#include <math.h>

/* Embedded default thermal parameters (Table 1 of the paper) */
static const thermal_params_t DEFAULT_PARAMS = {
    .C_j  = {1.42f, 1.38f, 1.35f, 1.41f},     /* J/°C               */
    .G_j  = {0.302f, 0.383f, 0.365f, 0.320f},  /* W/°C (vertical)   */
    .G_jk = {0.042f, 0.038f, 0.045f},           /* W/°C (adjacent)   */
    .C_jl = {0.012f, 0.011f},                   /* W/°C (non-adj.)   */
    .tau_j= {4.70f, 3.60f, 3.70f, 4.40f},      /* seconds           */
};

/* AXI4 crossbar contention lookup table (Table 3.3 in paper)
 * Index = N_active streams (1–4), value = bandwidth fraction */
static const float NOC_BW_FRACTION[5] = {0.0f, 1.00f, 0.88f, 0.71f, 0.58f};

/* ── Internal helpers ────────────────────────────────────────────────────── */

static int cmp_layers_desc(const void *a, const void *b)
{
    const layer_desc_t *la = (const layer_desc_t *)a;
    const layer_desc_t *lb = (const layer_desc_t *)b;
    /* Sort descending by MAC ops (heaviest layers first) */
    if (lb->mac_ops > la->mac_ops) return 1;
    if (lb->mac_ops < la->mac_ops) return -1;
    return 0;
}

static void read_sysmon_temps(thermasched_ctx_t *ctx)
{
    for (int j = 0; j < TS_NUM_TILES; j++) {
        ctx->tiles[j].temp_c = sysmon_read_tile_temp(j);
        /* Kalman-style sensor confidence weighting:
         * If DVFS was active last epoch, sensor variance = 0.6°C;
         * otherwise 0.3°C. Use prediction if sensor is noisy. */
        float sensor_var = ctx->dvfs.active_last_epoch ? 0.6f : 0.3f;
        float model_var  = 0.4f;  /* Thermal model variance estimate */
        float k = model_var / (model_var + sensor_var);
        ctx->tiles[j].temp_c = ctx->tiles[j].temp_predicted_c
            + k * (ctx->tiles[j].temp_c - ctx->tiles[j].temp_predicted_c);
    }
}

static float compute_s_reconfig(const layer_desc_t *layer,
                                 const tile_state_t *tile)
{
    if (tile->current_model == layer->model_id)
        return 1.0f;  /* No DPR needed */

    /* Penalty: exponential decay with DPR-to-epoch ratio
     * Using worst-case DPR_lat = 2100 µs, T_epoch = 100,000 µs */
    float ratio = (float)TS_DPR_LATENCY_US / (float)(TS_EPOCH_MS * 1000);
    return expf(-ratio);   /* ≈ 0.979 for worst-case 2.1 ms DPR */
}

static float compute_s_comm(const layer_desc_t *layer, int tile_j,
                              int n_active_streams)
{
    if (n_active_streams < 1) n_active_streams = 1;
    if (n_active_streams > 4) n_active_streams = 4;

    float bw_fraction = NOC_BW_FRACTION[n_active_streams];
    /* Communication cost proportional to weight tensor size */
    float bw_peak = 4800.0f;  /* MB/s */
    float bw_needed = (float)layer->weight_bytes / (1024.0f * 1024.0f)
                      * 1000.0f;  /* MB/s at 1ms fetch */
    float s_comm = 1.0f - (bw_needed / (bw_peak * bw_fraction));
    return fmaxf(0.0f, fminf(1.0f, s_comm));
}

/* ── Public API implementation ───────────────────────────────────────────── */

int thermasched_init(thermasched_ctx_t *ctx, const char *params_path)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(thermasched_ctx_t));

    /* Load thermal parameters */
    thermal_params_t params;
    if (params_path) {
        if (thermal_load_params(params_path, &params) != 0) {
            /* Fall back to embedded defaults */
            params = DEFAULT_PARAMS;
        }
    } else {
        params = DEFAULT_PARAMS;
    }
    thermal_model_init(&ctx->thermal, &params);

    /* Initialize SYSMON interface */
    sysmon_init();

    /* Initialize NoC model */
    noc_model_init(&ctx->noc, NOC_BW_FRACTION, 4800.0f);

    /* Initialize weight buffer: 4.5 MB, 4-way set-assoc, 72 lines × 64 KB */
    weight_buffer_init(&ctx->wbuf, 72, 64 * 1024);

    /* Initialize DVFS: MMCM fractional-divider mode, pre-loaded dividers */
    dvfs_init(&ctx->dvfs, TS_FREQ_MIN_MHZ, TS_FREQ_MAX_MHZ, TS_DVFS_STEP_MHZ);

    /* Set initial tile frequencies to maximum */
    for (int j = 0; j < TS_NUM_TILES; j++) {
        ctx->tiles[j].freq_mhz     = TS_FREQ_MAX_MHZ;
        ctx->tiles[j].temp_c       = 25.0f;  /* Room temp at startup */
        ctx->tiles[j].current_model = MODEL_UNKNOWN;
    }

    ctx->ambient_temp_c = 25.0f;
    return 0;
}

int thermasched_run_epoch(thermasched_ctx_t *ctx,
                           const layer_desc_t *layers_in, uint32_t n,
                           schedule_result_t *result)
{
    if (!ctx || !layers_in || !result || n == 0) return -1;
    if (n > TS_MAX_LAYERS) n = TS_MAX_LAYERS;

    uint64_t t_start = get_cycle_count();

    /* ── Stage 1: Read SYSMON temperatures  (1.2 µs) ─────────────────── */
    read_sysmon_temps(ctx);

    /* ── Stage 2: Predict temperatures for all tiles via Eq. (2) ──────── */
    float theta_hat[TS_NUM_TILES];
    thermal_predict(&ctx->thermal, ctx->tiles, ctx->ambient_temp_c,
                    (float)(TS_EPOCH_MS) / 1000.0f, theta_hat);

    /* ── Stage 3: Sort layers by MAC ops descending  (O(N log N)) ──────── */
    layer_desc_t sorted_layers[TS_MAX_LAYERS];
    memcpy(sorted_layers, layers_in, n * sizeof(layer_desc_t));
    /* Simple insertion sort (N ≤ 32, avoids stdlib qsort overhead) */
    for (uint32_t i = 1; i < n; i++) {
        layer_desc_t key = sorted_layers[i];
        int j2 = (int)i - 1;
        while (j2 >= 0 && sorted_layers[j2].mac_ops < key.mac_ops) {
            sorted_layers[j2 + 1] = sorted_layers[j2];
            j2--;
        }
        sorted_layers[j2 + 1] = key;
    }

    /* ── Stage 4+5: Score and greedily assign each layer  (3.8 µs) ────── */
    result->num_layers = n;
    result->dvfs_cmds  = 0;
    result->dpr_cmds   = 0;
    int n_active_streams = 0;  /* Tracks concurrent AXI streams for S_comm */

    for (uint32_t i = 0; i < n; i++) {
        const layer_desc_t *layer = &sorted_layers[i];
        float best_score = -1e9f;
        int   best_tile  = 0;

        for (int j = 0; j < TS_NUM_TILES; j++) {
            /* Skip tiles with DPR in progress that won't finish this epoch */
            if (ctx->tiles[j].is_reconfiguring &&
                ctx->tiles[j].dpr_remaining_us > (uint32_t)(TS_EPOCH_MS * 1000))
                continue;

            /* S_thermal: normalized thermal headroom */
            float s_thermal = (TS_DTM_THRESHOLD_C - theta_hat[j])
                              / TS_DTM_THRESHOLD_C;
            s_thermal = fmaxf(0.0f, fminf(1.0f, s_thermal));

            /* S_comm: contention-corrected NoC efficiency */
            float s_comm = compute_s_comm(layer, j, n_active_streams);

            /* S_reconfig: DPR penalty */
            float s_reconfig = compute_s_reconfig(layer, &ctx->tiles[j]);

            /* S_cache: estimated weight buffer hit rate */
            float s_cache = weight_buffer_predict_hit(&ctx->wbuf,
                                layer->model_id, layer->layer_id);

            /* Composite score Eq. (3) */
            float score = ALPHA * s_thermal
                        + BETA  * s_comm
                        + GAMMA * s_reconfig
                        + DELTA * s_cache;

            if (score > best_score) {
                best_score = score;
                best_tile  = j;
            }
        }

        result->assignment[i] = (uint8_t)best_tile;

        /* Update predicted temperature for the assigned tile
         * to reflect the workload being added */
        theta_hat[best_tile] += thermal_workload_delta(&ctx->thermal,
                                    layer, best_tile);

        /* Issue DPR command if tile type mismatch */
        if (ctx->tiles[best_tile].current_model != layer->model_id) {
            ctx->tiles[best_tile].current_model = layer->model_id;
            ctx->tiles[best_tile].is_reconfiguring = true;
            ctx->tiles[best_tile].dpr_remaining_us = TS_DPR_LATENCY_US;
            result->dpr_cmds++;
            ctx->dpr_events_total++;
        }

        /* Prefetch weights for next layer in queue via DMA */
        if (i + 1 < n)
            weight_buffer_prefetch(&ctx->wbuf, sorted_layers[i+1].model_id,
                                   sorted_layers[i+1].layer_id);

        n_active_streams = (n_active_streams < 4) ? n_active_streams + 1 : 4;
    }

    /* ── Stage 6: Proactive DVFS  (2.1 µs) ─────────────────────────────── */
    float theta_warn = thermasched_compute_theta_warn(ctx->ambient_temp_c);
    ctx->dvfs.active_last_epoch = false;

    for (int j = 0; j < TS_NUM_TILES; j++) {
        if (theta_hat[j] > theta_warn) {
            uint32_t new_freq = ctx->tiles[j].freq_mhz - TS_DVFS_STEP_MHZ;
            if (new_freq >= TS_FREQ_MIN_MHZ) {
                dvfs_set_freq(&ctx->dvfs, j, new_freq);
                ctx->tiles[j].freq_mhz = new_freq;
                result->dvfs_cmds++;
                ctx->dvfs_events_total++;
                ctx->dvfs.active_last_epoch = true;
            }
        } else if (theta_hat[j] < theta_warn - 5.0f &&
                   ctx->tiles[j].freq_mhz < TS_FREQ_MAX_MHZ) {
            /* Restore frequency if thermal headroom recovered */
            uint32_t new_freq = ctx->tiles[j].freq_mhz + TS_DVFS_STEP_MHZ;
            dvfs_set_freq(&ctx->dvfs, j, new_freq);
            ctx->tiles[j].freq_mhz = new_freq;
        }
        ctx->tiles[j].temp_predicted_c = theta_hat[j];
    }

    /* ── Stage 7: Record latency ────────────────────────────────────────── */
    uint64_t t_end = get_cycle_count();
    result->decision_time_ns = cycles_to_ns(t_end - t_start);
    result->predicted_peak_temp_c = 0.0f;
    for (int j = 0; j < TS_NUM_TILES; j++) {
        if (theta_hat[j] > result->predicted_peak_temp_c)
            result->predicted_peak_temp_c = theta_hat[j];
    }

    ctx->epoch_count++;

    /* Check for unexpected DTM (should not occur at ≤25°C ambient) */
    for (int j = 0; j < TS_NUM_TILES; j++) {
        if (ctx->tiles[j].temp_c >= TS_DTM_THRESHOLD_C)
            ctx->dtm_events_total++;
    }

    return 0;
}

float thermasched_compute_theta_warn(float ambient_c)
{
    /* Linear interpolation:
     *   25°C → 75°C warn
     *   35°C → 73°C warn
     *   45°C → 71°C warn
     * Slope: -0.2°C warn per +1°C ambient */
    float warn = TS_WARN_BASE_C - 0.2f * (ambient_c - 25.0f);
    return fmaxf(65.0f, fminf(TS_WARN_BASE_C, warn));
}

void thermasched_dump_stats(const thermasched_ctx_t *ctx)
{
    uart_printf("\n=== ThermaSched Statistics (epoch %llu) ===\n",
                (unsigned long long)ctx->epoch_count);
    for (int j = 0; j < TS_NUM_TILES; j++) {
        uart_printf("  T%d: %.1f°C (pred %.1f°C), %u MHz, util=%.1f%%\n",
                    j,
                    ctx->tiles[j].temp_c,
                    ctx->tiles[j].temp_predicted_c,
                    ctx->tiles[j].freq_mhz,
                    ctx->tiles[j].utilization * 100.0f);
    }
    uart_printf("  DVFS events: %u total, %.1f/epoch avg\n",
                ctx->dvfs_events_total,
                (float)ctx->dvfs_events_total / (float)ctx->epoch_count);
    uart_printf("  DPR events:  %u total, %.1f/epoch avg\n",
                ctx->dpr_events_total,
                (float)ctx->dpr_events_total / (float)ctx->epoch_count);
    uart_printf("  DTM events:  %u total (expected: 0 at ≤25C ambient)\n",
                ctx->dtm_events_total);
    uart_printf("  Ambient: %.1f°C, θ_warn: %.1f°C\n",
                ctx->ambient_temp_c,
                thermasched_compute_theta_warn(ctx->ambient_temp_c));
}

void thermasched_shutdown(thermasched_ctx_t *ctx)
{
    /* Restore all tiles to maximum frequency */
    for (int j = 0; j < TS_NUM_TILES; j++) {
        dvfs_set_freq(&ctx->dvfs, j, TS_FREQ_MAX_MHZ);
        ctx->tiles[j].freq_mhz = TS_FREQ_MAX_MHZ;
    }
    /* Flush weight buffer prefetch queue */
    weight_buffer_flush(&ctx->wbuf);
    /* Dump final statistics */
    thermasched_dump_stats(ctx);
}
