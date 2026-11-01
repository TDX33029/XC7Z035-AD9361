#ifndef PN11_FRAME_H
#define PN11_FRAME_H

#include <stdint.h>
#include <string.h>

/* PS-local source, packed byte-by-byte MSB first, matching the UI PN11
 * convention: output bit 0, feedback bit 0 XOR bit 2, shift right.
 * This is the reciprocal implementation of x^11+x^9+1. No randomizer or
 * PCM line encoding is applied here: those remain in the PL modulator. */
#define PN11_REPLAY_BYTES 16384U
#define PN11_PERIOD_BITS 2047U

typedef struct {
    uint32_t raw_bits;       /* used only with framed=0; default one PN period */
    uint64_t sync_word;      /* permits a 33-bit sync word */
    uint16_t seed;           /* 1..0x7ff; default 0x7ff */
    uint16_t frame_bytes;    /* 1..2048, must have payload capacity */
    uint16_t frame_count;    /* 1: minor frame; 2..256: 8-bit SFID major frame */
    uint8_t framed;
    uint8_t sync_bits;       /* 16..33 */
    uint8_t sfid_start;      /* 0/1; count=256 requires start=0 */
    uint8_t invert;
} pn11_frame_config_t;

enum { PN11_OK=0, PN11_BAD_CONFIG=1, PN11_BUFFER_SMALL=2 };

static inline void pn11_frame_defaults(pn11_frame_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->raw_bits = PN11_PERIOD_BITS;
    cfg->sync_word = UINT64_C(0xFE6B2840);
    cfg->seed = 0x7ffU;
    cfg->frame_bytes = 512U;
    cfg->frame_count = 1U;
    cfg->framed = 1U;
    cfg->sync_bits = 32U;
}

static inline void pn11_set_bit(uint8_t *dst, uint32_t position, uint8_t bit)
{
    if (bit)
        dst[position >> 3] |= (uint8_t)(1U << (7U - (position & 7U)));
}

static inline uint8_t pn11_next(uint16_t *state, uint8_t invert)
{
    uint8_t bit = (uint8_t)((*state & 1U) ^ invert);
    uint16_t feedback = (uint16_t)(((*state >> 0) ^ (*state >> 2)) & 1U);
    *state = (uint16_t)((*state >> 1) | (feedback << 10));
    return bit;
}

/* Validate everything before touching dst or *valid_bits. Payload PN state
 * continues across minor frames; sync/SFID do not advance it. Cached replay
 * restarts this complete buffer, hence PN phase also restarts at its boundary.
 * For an unframed continuous-period PN11 replay use raw_bits=2047. */
static inline int pn11_frame_build(const pn11_frame_config_t *cfg,
                                  uint8_t *dst, uint32_t capacity,
                                  uint32_t *valid_bits)
{
    uint32_t count, bytes, bits, frame, pos, i, payload_start;
    uint16_t state;
    if (!cfg || !dst || !valid_bits || cfg->seed == 0U || cfg->seed > 0x7ffU ||
        cfg->invert > 1U || cfg->framed > 1U)
        return PN11_BAD_CONFIG;
    if (cfg->framed) {
        if (cfg->sync_bits < 16U || cfg->sync_bits > 33U ||
            cfg->sync_word >= (UINT64_C(1) << cfg->sync_bits) ||
            cfg->frame_bytes == 0U || cfg->frame_bytes > 2048U ||
            cfg->frame_count == 0U || cfg->frame_count > 256U ||
            cfg->sfid_start > 1U ||
            (cfg->frame_count > 1U && (uint32_t)cfg->sfid_start + cfg->frame_count > 256U))
            return PN11_BAD_CONFIG;
        payload_start = cfg->sync_bits + (cfg->frame_count > 1U ? 8U : 0U);
        if ((uint32_t)cfg->frame_bytes * 8U <= payload_start)
            return PN11_BAD_CONFIG;
        bytes = (uint32_t)cfg->frame_bytes * cfg->frame_count;
        bits = bytes * 8U;
        count = cfg->frame_count;
    } else {
        if (cfg->raw_bits == 0U || cfg->raw_bits > PN11_REPLAY_BYTES * 8U)
            return PN11_BAD_CONFIG;
        bits = cfg->raw_bits;
        bytes = (bits + 7U) >> 3;
        count = 0U;
        payload_start = 0U;
    }
    if (bytes > PN11_REPLAY_BYTES || bytes > capacity)
        return PN11_BUFFER_SMALL;
    memset(dst, 0, bytes);
    state = cfg->seed;
    if (!cfg->framed) {
        for (pos = 0U; pos < bits; ++pos)
            pn11_set_bit(dst, pos, pn11_next(&state, cfg->invert));
    } else {
        for (frame = 0U; frame < count; ++frame) {
            pos = frame * (uint32_t)cfg->frame_bytes * 8U;
            for (i = 0U; i < cfg->sync_bits; ++i)
                pn11_set_bit(dst, pos+i, (uint8_t)((cfg->sync_word >> (cfg->sync_bits-1U-i)) & 1U));
            if (count > 1U) {
                uint8_t sfid = (uint8_t)(frame + cfg->sfid_start);
                for (i = 0U; i < 8U; ++i)
                    pn11_set_bit(dst, pos+cfg->sync_bits+i, (uint8_t)((sfid >> (7U-i)) & 1U));
            }
            for (i = payload_start; i < (uint32_t)cfg->frame_bytes*8U; ++i)
                pn11_set_bit(dst, pos+i, pn11_next(&state, cfg->invert));
        }
    }
    *valid_bits = bits;
    return PN11_OK;
}

#endif
