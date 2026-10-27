#include "crowdsync_protocol.h"
#include <string.h>

bool cs_color_valid(CsColor color)
{
    return color.effect <= 15 && color.red <= 15 && color.green <= 15 && color.blue <= 15;
}

bool cs_packet_build(const CsColor groups[CS_GROUPS], uint8_t packet[CS_PACKET_BYTES])
{
    if (!groups || !packet)
        return false;
    memset(packet, 0, CS_PACKET_BYTES);
    for (size_t i = 0; i < CS_GROUPS; ++i) {
        if (!cs_color_valid(groups[i]))
            return false;
        packet[2 * i] = (groups[i].effect << 4) | groups[i].red;
        packet[2 * i + 1] = (groups[i].green << 4) | groups[i].blue;
        packet[20] ^= packet[2 * i] ^ packet[2 * i + 1];
    }
    return true;
}

static void cs_wave_append(CsWaveform *wave, uint32_t duration)
{
    wave->durations[wave->count++] = duration;
    wave->length_us += duration;
}

bool cs_waveform_build(const uint8_t packet[CS_PACKET_BYTES], bool preamble, CsWaveform *wave)
{
    if (!packet || !wave)
        return false;
    memset(wave, 0, sizeof(*wave));
    if (preamble) {
        for (size_t i = 0; i < 32; ++i) {
            cs_wave_append(wave, 120);
            cs_wave_append(wave, 320);
        }
    }
    cs_wave_append(wave, 120);
    cs_wave_append(wave, 600);
    cs_wave_append(wave, 120);
    cs_wave_append(wave, 1600);
    for (size_t i = 0; i < CS_PACKET_BYTES; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            bool one = (packet[i] >> bit) & 1;
            cs_wave_append(wave, one ? 320 : 120);
            cs_wave_append(wave, one ? 120 : 320);
        }
    }
    cs_wave_append(wave, 120);
    cs_wave_append(wave, 2400);
    return true;
}

bool cs_stream_init(CsStream *stream, const CsColor groups[CS_GROUPS], bool preamble, uint32_t interval_us,
                    uint32_t frame_limit, bool beat, uint32_t beat_period_us, uint32_t beat_on_us,
                    int8_t momentary_group)
{
    if (!stream || !groups || momentary_group < -1 || momentary_group >= CS_GROUPS)
        return false;
    memset(stream, 0, sizeof(*stream));
    CsColor colors[CS_GROUPS];
    memcpy(colors, groups, sizeof(colors));
    if (beat) {
        for (size_t i = 0; i < CS_GROUPS; ++i) {
            if (momentary_group == -1 || i == (size_t)momentary_group)
                colors[i].effect = 0;
        }
    }
    uint8_t packet[CS_PACKET_BYTES];
    if (!cs_packet_build(colors, packet))
        return false;
    cs_waveform_build(packet, preamble, &stream->waves[0]);
    if (beat) {
        for (size_t i = 0; i < CS_GROUPS; ++i) {
            if (momentary_group == -1 || i == (size_t)momentary_group)
                colors[i] = (CsColor){0};
        }
        cs_packet_build(colors, packet);
        cs_waveform_build(packet, preamble, &stream->waves[1]);
        if (beat_on_us < stream->waves[0].length_us || beat_period_us < beat_on_us ||
            beat_period_us - beat_on_us < stream->waves[1].length_us)
            return false;
        stream->spacing_us[0] = beat_on_us;
        stream->spacing_us[1] = beat_period_us - beat_on_us;
    } else {
        if (interval_us < stream->waves[0].length_us)
            return false;
        stream->spacing_us[0] = interval_us;
    }
    stream->frame_limit = frame_limit;
    stream->beat = beat;
    return true;
}

bool cs_stream_next(CsStream *stream, bool *level, uint32_t *duration)
{
    if (!stream || !level || !duration)
        return false;
    CsWaveform *wave = &stream->waves[stream->phase];
    if (stream->index == wave->count) {
        stream->index = 0;
        stream->frames_emitted++;
        if (stream->beat)
            stream->phase ^= 1;
        wave = &stream->waves[stream->phase];
    }
    if (stream->frame_limit && stream->frames_emitted >= stream->frame_limit)
        return false;
    *level = !(stream->index & 1);
    *duration = wave->durations[stream->index];
    if (stream->index == wave->count - 1) {
        *duration += stream->spacing_us[stream->phase] - wave->length_us;
    }
    stream->index++;
    return true;
}
