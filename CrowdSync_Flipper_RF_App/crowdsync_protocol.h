#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CS_GROUPS 10
#define CS_PACKET_BYTES 22
#define CS_MAX_INTERVALS 422
#define CS_FREQUENCY_HZ 433920000UL

typedef struct {
    uint8_t effect;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} CsColor;

typedef struct {
    uint32_t durations[CS_MAX_INTERVALS];
    uint16_t count;
    uint32_t length_us;
} CsWaveform;

typedef struct {
    CsWaveform waves[2];
    uint32_t spacing_us[2];
    uint32_t frame_limit; /* Zero means repeat until stopped. */
    volatile uint32_t frames_emitted;
    uint16_t index;
    uint8_t phase;
    bool beat;
} CsStream;

bool cs_color_valid(CsColor color);
bool cs_packet_build(const CsColor groups[CS_GROUPS], uint8_t packet[CS_PACKET_BYTES]);
bool cs_waveform_build(const uint8_t packet[CS_PACKET_BYTES], bool preamble, CsWaveform *wave);
bool cs_stream_init(CsStream *stream, const CsColor groups[CS_GROUPS], bool preamble, uint32_t interval_us,
                    uint32_t frame_limit, bool beat, uint32_t beat_period_us, uint32_t beat_on_us,
                    int8_t momentary_group);
bool cs_stream_next(CsStream *stream, bool *level, uint32_t *duration);
