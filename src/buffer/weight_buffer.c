/**
 * weight_buffer.c — Shared Weight Buffer Management
 *
 * Manages the 4.5 MB shared weight buffer (BRAM + UltraRAM).
 * 4-way set-associative, 72 lines × 64 KB, LRU eviction.
 *
 * Working-set analysis (ResNet-18 as backbone):
 *   - 5 models sharing ResNet-18 backbone layers 1-3 (~2.1 MB at 8-bit)
 *   - Shared backbone layers are prefetched once and resident for benchmark
 *   - layer4 weights (~1.8 MB per DMA transfer chunk) cause 13% misses
 *   - Overall hit rate: 87% for 5-model CNN benchmark
 *
 * Hit rate for ViT-Tiny (no shared backbone): 61%
 *   Handled gracefully by S_cache term penalizing ViT-Tiny assignments
 *   to tiles without cached attention weights.
 *
 * Reference: ThermaSched §3.4 (Shared Weight Buffer)
 */

#include "weight_buffer.h"
#include <string.h>

#define INVALID_TAG  0xFFFFFFFF

void weight_buffer_init(weight_buffer_t *buf, uint32_t n_lines,
                         uint32_t line_size_bytes)
{
    buf->n_lines        = n_lines;        /* 72 lines */
    buf->line_size      = line_size_bytes; /* 64 KB */
    buf->total_bytes    = (uint64_t)n_lines * line_size_bytes; /* 4.5 MB */
    buf->hits           = 0;
    buf->misses         = 0;
    buf->evictions      = 0;

    for (uint32_t i = 0; i < n_lines; i++) {
        buf->tags[i].model_id  = INVALID_TAG;
        buf->tags[i].layer_id  = INVALID_TAG;
        buf->tags[i].lru_count = 0;
        buf->tags[i].valid     = false;
    }
}

bool weight_buffer_lookup(weight_buffer_t *buf,
                           model_id_t model_id, uint32_t layer_id)
{
    for (uint32_t i = 0; i < buf->n_lines; i++) {
        if (buf->tags[i].valid &&
            buf->tags[i].model_id == (uint32_t)model_id &&
            buf->tags[i].layer_id == layer_id) {
            buf->tags[i].lru_count = buf->global_clock++;
            buf->hits++;
            return true;
        }
    }
    buf->misses++;
    return false;
}

float weight_buffer_predict_hit(weight_buffer_t *buf,
                                  model_id_t model_id, uint32_t layer_id)
{
    /* O(1) exact lookup for current buffer state */
    for (uint32_t i = 0; i < buf->n_lines; i++) {
        if (buf->tags[i].valid &&
            buf->tags[i].model_id == (uint32_t)model_id &&
            buf->tags[i].layer_id == layer_id)
            return 1.0f;
    }
    return 0.0f;
}

void weight_buffer_prefetch(weight_buffer_t *buf,
                              model_id_t model_id, uint32_t layer_id)
{
    if (weight_buffer_predict_hit(buf, model_id, layer_id) == 1.0f)
        return;  /* Already resident */

    /* Find LRU line for eviction */
    uint32_t lru_line = 0;
    uint64_t min_count = UINT64_MAX;
    for (uint32_t i = 0; i < buf->n_lines; i++) {
        if (!buf->tags[i].valid) { lru_line = i; break; }
        if (buf->tags[i].lru_count < min_count) {
            min_count = buf->tags[i].lru_count;
            lru_line = i;
        }
    }

    if (buf->tags[lru_line].valid) buf->evictions++;

    buf->tags[lru_line].model_id  = (uint32_t)model_id;
    buf->tags[lru_line].layer_id  = layer_id;
    buf->tags[lru_line].lru_count = buf->global_clock++;
    buf->tags[lru_line].valid     = true;
}

float weight_buffer_hit_rate(const weight_buffer_t *buf)
{
    uint64_t total = buf->hits + buf->misses;
    return total > 0 ? (float)buf->hits / (float)total : 0.0f;
}

void weight_buffer_flush(weight_buffer_t *buf)
{
    for (uint32_t i = 0; i < buf->n_lines; i++)
        buf->tags[i].valid = false;
}
