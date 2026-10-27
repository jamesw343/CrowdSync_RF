#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <subghz/devices/devices.h>
#include <subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <toolbox/level_duration.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crowdsync_protocol.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SETTINGS_PATH APP_DATA_PATH("settings.ff")

typedef enum { CsToggle, CsHold, CsBurst, CsOnce, CsBeat, CsSendModeCount } CsSendMode;
typedef enum { CsEditor, CsTransmit, CsHelp } CsPage;
typedef enum {
    RowTransmit,
    RowMode,
    RowGroup,
    RowEffect,
    RowRed,
    RowGreen,
    RowBlue,
    RowSendMode,
    RowInterval,
    RowBurst,
    RowPreamble,
    RowBeatPeriod,
    RowBeatOn,
    RowCopyAll,
    RowSave,
    RowLoad,
    RowHelp,
    RowCount
} CsRow;

static const uint32_t intervals_ms[] = {100, 150, 250, 500, 1000, 2000};
static const uint32_t bursts_ms[] = {1000, 3000, 5000, 10000, 25000};
static const uint32_t beats_ms[] = {250, 333, 400, 500, 600, 750, 1000, 1500, 2000};
static const uint32_t on_ms[] = {100, 125, 150, 200, 250, 300, 500};
static const char *send_names[] = {"Toggle", "Hold + off", "Burst", "Once", "Beat"};
static const char *effects[] = {"Steady",       "Blink 6Hz",    "Blink 10Hz",  "Blink 14Hz",
                                "Blink 20Hz",   "Blink 25Hz",   "Flash/5ms",   "Flash/2ms",
                                "Fade out/9ms", "Fade out/5ms", "Fade in/9ms", "Fade in/5ms",
                                "Off/on/9ms",   "Off/on/2ms",   "Hold state",  "Power save"};

typedef struct {
    bool advanced;
    bool preamble;
    uint8_t group;
    CsColor simple;
    CsColor groups[CS_GROUPS];
    uint8_t send_mode;
    uint8_t interval;
    uint8_t burst;
    uint8_t beat_period;
    uint8_t beat_on;
} CsSettings;

typedef struct {
    Gui *gui;
    ViewPort *viewport;
    FuriMessageQueue *input;
    FuriMutex *mutex;
    const SubGhzDevice *radio;
    CsSettings settings;
    CsStream stream;
    CsPage page;
    uint8_t row;
    uint8_t help_page;
    bool transmitting;
    bool holding;
    bool exiting;
    bool acquired;
    bool tx_acquisition;
    bool tx_ordinary;
    bool tx_power_save;
    bool tx_show_time;
    uint32_t last_activity;
    uint32_t tx_started;
    uint32_t tx_duration_ms;
    uint32_t notice_until;
    char notice[32];
    char tx_name[20];
} CsApp;

static CsSettings cs_defaults(void)
{
    CsSettings s = {0};
    s.preamble = true;
    s.group = 0;
    s.simple = (CsColor){.red = 15};
    for (size_t i = 0; i < CS_GROUPS; ++i)
        s.groups[i] = s.simple;
    s.interval = 2;
    s.burst = 1;
    s.beat_period = 3;
    return s;
}

static CsColor *cs_selected(CsSettings *s)
{
    return s->advanced ? &s->groups[s->group] : &s->simple;
}

static void cs_notice(CsApp *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text);
    app->notice_until = furi_get_tick() + furi_ms_to_ticks(3500);
}

/* One versioned array keeps disk IO independent of C struct padding. */
#define SETTINGS_VALUES 53
static void cs_settings_pack(const CsSettings *s, uint32_t data[SETTINGS_VALUES])
{
    size_t k = 0;
    data[k++] = s->advanced;
    data[k++] = s->preamble;
    data[k++] = s->group;
    data[k++] = s->send_mode;
    data[k++] = s->interval;
    data[k++] = s->burst;
    data[k++] = s->beat_period;
    data[k++] = s->beat_on;
    data[k++] = 0; /* Reserved for future settings. */
    for (size_t i = 0; i <= CS_GROUPS; ++i) {
        CsColor c = i ? s->groups[i - 1] : s->simple;
        data[k++] = c.effect;
        data[k++] = c.red;
        data[k++] = c.green;
        data[k++] = c.blue;
    }
}

static bool cs_settings_unpack(CsSettings *s, const uint32_t data[SETTINGS_VALUES])
{
    if (data[0] > 1 || data[1] > 1 || data[2] >= CS_GROUPS || data[3] >= CsSendModeCount ||
        data[4] >= COUNT(intervals_ms) || data[5] >= COUNT(bursts_ms) || data[6] >= COUNT(beats_ms) ||
        data[7] >= COUNT(on_ms) || data[8] != 0)
        return false;
    for (size_t k = 9; k < SETTINGS_VALUES; ++k)
        if (data[k] > 15)
            return false;
    CsSettings next = {0};
    next.advanced = data[0];
    next.preamble = data[1];
    next.group = data[2];
    next.send_mode = data[3];
    next.interval = data[4];
    next.burst = data[5];
    next.beat_period = data[6];
    next.beat_on = data[7];
    if (on_ms[next.beat_on] + 100 > beats_ms[next.beat_period])
        return false;
    size_t k = 9;
    for (size_t i = 0; i <= CS_GROUPS; ++i) {
        CsColor color = {data[k], data[k + 1], data[k + 2], data[k + 3]};
        k += 4;
        if (i)
            next.groups[i - 1] = color;
        else
            next.simple = color;
    }
    *s = next;
    return true;
}

static bool cs_settings_file(CsSettings *settings, bool save)
{
    Storage *storage = furi_record_open(RECORD_STORAGE);
    if (save)
        storage_simply_mkdir(storage, APP_DATA_PATH(""));
    FlipperFormat *file = flipper_format_file_alloc(storage);
    FuriString *type = furi_string_alloc();
    uint32_t data[SETTINGS_VALUES], version = 0;
    bool ok = false;
    if (save) {
        cs_settings_pack(settings, data);
        ok = flipper_format_file_open_always(file, SETTINGS_PATH) &&
             flipper_format_write_header_cstr(file, "CrowdSync Settings", 1) &&
             flipper_format_write_uint32(file, "Settings", data, SETTINGS_VALUES);
    } else {
        ok = flipper_format_file_open_existing(file, SETTINGS_PATH) &&
             flipper_format_read_header(file, type, &version) && version == 1 &&
             furi_string_equal(type, "CrowdSync Settings") &&
             flipper_format_read_uint32(file, "Settings", data, SETTINGS_VALUES) && cs_settings_unpack(settings, data);
    }
    flipper_format_free(file);
    furi_string_free(type);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static LevelDuration cs_yield(void *context)
{
    CsApp *app = context;
    bool level;
    uint32_t duration;
    if (!cs_stream_next(&app->stream, &level, &duration))
        return level_duration_reset();
    return level_duration_make(level, duration);
}

/* Estimate activity from elapsed waveform time, not the DMA-prefetched frame counter. */
static void cs_activity_update(CsApp *app)
{
    uint32_t now = furi_get_tick();
    uint32_t hz = furi_kernel_get_tick_frequency();
    if (app->acquired && (uint32_t)(now - app->last_activity) >= 6 * hz)
        app->acquired = false;
    if (!app->transmitting)
        return;
    uint64_t elapsed = (uint64_t)(uint32_t)(now - app->tx_started) * 1000000 / hz;
    CsStream *stream = &app->stream;
    uint64_t completed = 0;
    if (stream->beat) {
        uint64_t cycle = (uint64_t)stream->spacing_us[0] + stream->spacing_us[1];
        uint64_t base = elapsed / cycle * cycle;
        uint64_t first = base + stream->waves[0].length_us;
        uint64_t second = base + stream->spacing_us[0] + stream->waves[1].length_us;
        if (second <= elapsed)
            completed = second;
        else if (first <= elapsed)
            completed = first;
        else if (base)
            completed = base - cycle + stream->spacing_us[0] + stream->waves[1].length_us;
    } else if (elapsed >= stream->waves[0].length_us) {
        uint64_t index = (elapsed - stream->waves[0].length_us) / stream->spacing_us[0];
        if (stream->frame_limit && index >= stream->frame_limit)
            index = stream->frame_limit - 1;
        completed = index * stream->spacing_us[0] + stream->waves[0].length_us;
    }
    if (!completed)
        return;
    if (app->tx_power_save) {
        app->acquired = false;
        return;
    }
    uint64_t acquisition_end = (uint64_t)(stream->frame_limit ? stream->frame_limit - 1 : 0) * stream->spacing_us[0] +
                               stream->waves[0].length_us;
    if (app->tx_acquisition && completed >= acquisition_end)
        app->acquired = true;
    uint32_t longest_spacing = stream->spacing_us[0];
    if (stream->beat && stream->spacing_us[1] > longest_spacing)
        longest_spacing = stream->spacing_us[1];
    // A final inter-packet gap still counts toward a sustained three-second burst.
    if (!app->tx_acquisition && app->tx_ordinary && stream->frame_limit != 1 && elapsed >= 3000000 &&
        completed + longest_spacing >= 3000000)
        app->acquired = true;
    if (app->acquired) {
        app->last_activity = app->tx_started + (uint32_t)(completed * hz / 1000000);
        if ((uint32_t)(now - app->last_activity) >= 6 * hz)
            app->acquired = false;
    }
}

static void cs_stop(CsApp *app)
{
    if (!app->transmitting)
        return;
    cs_activity_update(app);
    /* Stop DMA before touching the stream again. */
    subghz_devices_stop_async_tx(app->radio);
    subghz_devices_idle(app->radio);
    subghz_devices_sleep(app->radio);
    subghz_devices_end(app->radio);
    furi_hal_power_suppress_charge_exit();
    furi_hal_power_insomnia_exit();
    app->transmitting = false;
}

static bool cs_start(CsApp *app, bool black, bool all, uint32_t burst_ms, const char *name)
{
    cs_stop(app);
    CsSettings *s = &app->settings;
    CsColor colors[CS_GROUPS];
    for (size_t i = 0; i < CS_GROUPS; ++i)
        colors[i] = s->advanced ? s->groups[i] : s->simple;
    if (black) {
        for (size_t i = 0; i < CS_GROUPS; ++i) {
            if (all || !s->advanced || i == s->group)
                colors[i] = (CsColor){0};
        }
    }
    uint32_t interval = burst_ms ? 100 : intervals_ms[s->interval];
    uint32_t frame_us = s->preamble ? 96480 : 82400;
    uint32_t limit = 0;
    bool beat = !burst_ms && s->send_mode == CsBeat;
    if (burst_ms)
        limit = (burst_ms + interval - 1) / interval;
    else if (s->send_mode == CsOnce) {
        limit = 1;
        interval = 0;
    } else if (s->send_mode == CsBurst) {
        limit = (bursts_ms[s->burst] + interval - 1) / interval;
    }
    bool valid =
        cs_stream_init(&app->stream, colors, s->preamble, interval ? interval * 1000 : frame_us, limit, beat,
                       beats_ms[s->beat_period] * 1000, on_ms[s->beat_on] * 1000, s->advanced ? (int8_t)s->group : -1);
    if (!valid) {
        cs_notice(app, "Invalid timing settings");
        return false;
    }
    if (!app->radio || !furi_hal_region_is_frequency_allowed(CS_FREQUENCY_HZ)) {
        cs_notice(app, "433.92 TX not allowed");
        return false;
    }
    /* The internal device has no begin hook; its false return is not a failure. */
    if (!subghz_devices_is_connect(app->radio)) {
        cs_notice(app, "Radio unavailable");
        return false;
    }
    subghz_devices_reset(app->radio);
    subghz_devices_load_preset(app->radio, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(app->radio, CS_FREQUENCY_HZ);
    furi_hal_power_insomnia_enter();
    furi_hal_power_suppress_charge_enter();
    if (!subghz_devices_start_async_tx(app->radio, cs_yield, app)) {
        subghz_devices_sleep(app->radio);
        subghz_devices_end(app->radio);
        furi_hal_power_suppress_charge_exit();
        furi_hal_power_insomnia_exit();
        cs_notice(app, "Radio TX failed");
        return false;
    }
    app->tx_duration_ms = 0;
    app->tx_show_time = !(black && burst_ms == 300);
    if (burst_ms >= 3000 || (!burst_ms && s->send_mode == CsBurst))
        app->tx_duration_ms = limit * interval;
    if (!burst_ms) {
        if (s->send_mode == CsOnce) {
            name = "Sending once";
            app->tx_show_time = false;
        } else
            name = s->send_mode == CsBeat ? "Beat" : "Sending";
    }
    app->tx_acquisition = black && all && burst_ms >= 3000;
    app->tx_power_save = false;
    app->tx_ordinary = true;
    for (size_t i = 0; i < CS_GROUPS; ++i) {
        uint8_t effect = beat && (!s->advanced || i == s->group) ? 0 : colors[i].effect;
        if (effect >= 6 && effect != 14)
            app->tx_ordinary = false;
        if (effect == 15)
            app->tx_power_save = true;
    }
    app->transmitting = true;
    app->tx_started = furi_get_tick();
    snprintf(app->tx_name, sizeof(app->tx_name), "%s", name);
    app->notice_until = 0;
    return true;
}

static uint8_t cs_cycle(uint8_t value, size_t count, int direction)
{
    return (value + count + direction) % count;
}

static void cs_adjust(CsApp *app, int direction)
{
    CsSettings *s = &app->settings;
    CsColor *c = cs_selected(s);
    switch (app->row) {
    case RowMode:
        s->advanced = !s->advanced;
        break;
    case RowGroup:
        if (s->advanced)
            s->group = cs_cycle(s->group, CS_GROUPS, direction);
        break;
    case RowEffect:
        c->effect = cs_cycle(c->effect, 16, direction);
        break;
    case RowRed:
        c->red = cs_cycle(c->red, 16, direction);
        break;
    case RowGreen:
        c->green = cs_cycle(c->green, 16, direction);
        break;
    case RowBlue:
        c->blue = cs_cycle(c->blue, 16, direction);
        break;
    case RowSendMode:
        s->send_mode = cs_cycle(s->send_mode, CsSendModeCount, direction);
        break;
    case RowInterval:
        s->interval = cs_cycle(s->interval, COUNT(intervals_ms), direction);
        break;
    case RowBurst:
        s->burst = cs_cycle(s->burst, COUNT(bursts_ms), direction);
        break;
    case RowPreamble:
        s->preamble = !s->preamble;
        break;
    case RowBeatPeriod:
        s->beat_period = cs_cycle(s->beat_period, COUNT(beats_ms), direction);
        while (on_ms[s->beat_on] + 100 > beats_ms[s->beat_period])
            --s->beat_on;
        break;
    case RowBeatOn:
        do {
            s->beat_on = cs_cycle(s->beat_on, COUNT(on_ms), direction);
        } while (on_ms[s->beat_on] + 100 > beats_ms[s->beat_period]);
        break;
    default:
        break;
    }
}

static void cs_activate(CsApp *app)
{
    switch (app->row) {
    case RowTransmit:
        app->page = CsTransmit;
        break;
    case RowCopyAll: {
        CsColor color = *cs_selected(&app->settings);
        for (size_t i = 0; i < CS_GROUPS; ++i)
            app->settings.groups[i] = color;
        cs_notice(app, "Copied to all 10 groups");
        break;
    }
    case RowSave:
        cs_notice(app, cs_settings_file(&app->settings, true) ? "Settings saved" : "Save failed: check SD");
        break;
    case RowLoad:
        cs_notice(app, cs_settings_file(&app->settings, false) ? "Settings loaded" : "No valid saved settings");
        break;
    case RowHelp:
        app->page = CsHelp;
        app->help_page = 0;
        break;
    default:
        cs_adjust(app, 1);
        break;
    }
}

static void cs_input_handle(CsApp *app, const InputEvent *event)
{
    if (event->key == InputKeyBack && event->type == InputTypeShort) {
        app->holding = false;
        cs_stop(app);
        if (app->page == CsEditor)
            app->exiting = true;
        else
            app->page = CsEditor;
        return;
    }
    if (app->page == CsTransmit) {
        if (app->settings.send_mode == CsToggle && event->key == InputKeyOk) {
            if (event->type == InputTypePress) {
                if (app->transmitting)
                    cs_stop(app);
                else
                    cs_start(app, false, false, 0, send_names[CsToggle]);
            }
            return;
        }
        if (app->settings.send_mode == CsHold && event->key == InputKeyOk) {
            if (event->type == InputTypePress) {
                app->holding = cs_start(app, false, false, 0, "Holding");
            } else if (event->type == InputTypeRelease && app->holding) {
                app->holding = false;
                cs_start(app, true, false, 300, "Off");
            }
            return;
        }
        if (event->type != InputTypeShort)
            return;
        if (event->key == InputKeyOk) {
            if (app->transmitting)
                cs_stop(app);
            else
                cs_start(app, false, false, 0, send_names[app->settings.send_mode]);
        } else if (event->key == InputKeyLeft) {
            app->holding = false;
            cs_start(app, true, true, 300, "Off");
        } else if (event->key == InputKeyRight) {
            app->holding = false;
            cs_stop(app);
        } else if (event->key == InputKeyUp) {
            app->holding = false;
            cs_start(app, true, true, 3000, "Acquiring");
        } else if (event->key == InputKeyDown) {
            app->holding = false;
            cs_start(app, true, true, 25000, "Waking");
        }
        return;
    }
    if (event->type != InputTypeShort && event->type != InputTypeRepeat)
        return;
    if (app->page == CsHelp) {
        if (event->key == InputKeyRight || event->key == InputKeyDown || event->key == InputKeyOk)
            app->help_page = cs_cycle(app->help_page, 4, 1);
        else if (event->key == InputKeyLeft || event->key == InputKeyUp)
            app->help_page = cs_cycle(app->help_page, 4, -1);
        return;
    }
    if (event->key == InputKeyUp)
        app->row = cs_cycle(app->row, RowCount, -1);
    else if (event->key == InputKeyDown)
        app->row = cs_cycle(app->row, RowCount, 1);
    else if (event->key == InputKeyLeft)
        cs_adjust(app, -1);
    else if (event->key == InputKeyRight)
        cs_adjust(app, 1);
    else if (event->key == InputKeyOk && event->type == InputTypeShort)
        cs_activate(app);
}

static void cs_row_text(CsApp *app, uint8_t row, char *text, size_t size)
{
    CsSettings *s = &app->settings;
    CsColor *c = cs_selected(s);
    switch (row) {
    case RowTransmit:
        snprintf(text, size, "Open transmitter >");
        break;
    case RowMode:
        snprintf(text, size, "Mode: %s", s->advanced ? "Advanced" : "Simple");
        break;
    case RowGroup:
        if (s->advanced)
            snprintf(text, size, "Group: %u", s->group + 1);
        else
            snprintf(text, size, "Group: All 10");
        break;
    case RowEffect:
        snprintf(text, size, "FX %u %s", c->effect, effects[c->effect]);
        break;
    case RowRed:
        snprintf(text, size, "Red: %u", c->red);
        break;
    case RowGreen:
        snprintf(text, size, "Green: %u", c->green);
        break;
    case RowBlue:
        snprintf(text, size, "Blue: %u", c->blue);
        break;
    case RowSendMode:
        snprintf(text, size, "Send: %s", send_names[s->send_mode]);
        break;
    case RowInterval:
        snprintf(text, size, "Interval: %lu ms", (unsigned long)intervals_ms[s->interval]);
        break;
    case RowBurst:
        snprintf(text, size, "Burst: %lu s", (unsigned long)(bursts_ms[s->burst] / 1000));
        break;
    case RowPreamble:
        snprintf(text, size, "Preamble: %s", s->preamble ? "On" : "Off");
        break;
    case RowBeatPeriod:
        snprintf(text, size, "Beat: %lu ms", (unsigned long)beats_ms[s->beat_period]);
        break;
    case RowBeatOn:
        snprintf(text, size, "Beat on: %lu ms", (unsigned long)on_ms[s->beat_on]);
        break;
    case RowCopyAll:
        snprintf(text, size, "Copy color to all");
        break;
    case RowSave:
        snprintf(text, size, "Save settings");
        break;
    case RowLoad:
        snprintf(text, size, "Load settings");
        break;
    case RowHelp:
        snprintf(text, size, "Help / controls");
        break;
    default:
        text[0] = 0;
        break;
    }
}

static void cs_tx_status(const CsApp *app, char *text, size_t size)
{
    if (!app->transmitting) {
        snprintf(text, size, "433.92 Idle");
        return;
    }
    if (!app->tx_show_time) {
        snprintf(text, size, "%s", app->tx_name);
        return;
    }
    uint64_t elapsed_ms =
        (uint64_t)(uint32_t)(furi_get_tick() - app->tx_started) * 1000 / furi_kernel_get_tick_frequency();
    uint32_t seconds;
    if (app->tx_duration_ms) {
        uint32_t remaining = elapsed_ms < app->tx_duration_ms ? app->tx_duration_ms - elapsed_ms : 0;
        seconds = (remaining + 999) / 1000;
    } else
        seconds = elapsed_ms / 1000;
    snprintf(text, size, "%s: %lus", app->tx_name, (unsigned long)seconds);
}

static void cs_draw(Canvas *canvas, void *context)
{
    CsApp *app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    char text[64];
    if (app->page == CsEditor) {
        canvas_draw_str(canvas, 2, 9, "CrowdSync");
        canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom,
                                app->settings.advanced ? "Advanced" : "Simple");
        canvas_draw_line(canvas, 0, 12, 127, 12);
        uint8_t first = app->row < 4 ? 0 : app->row - 3;
        for (uint8_t n = 0; n < 4; ++n) {
            uint8_t row = first + n;
            if (row >= RowCount)
                break;
            if (row == app->row) {
                canvas_draw_box(canvas, 0, 15 + n * 10, 123, 10);
                canvas_invert_color(canvas);
            }
            cs_row_text(app, row, text, sizeof(text));
            canvas_draw_str(canvas, 3, 23 + n * 10, text);
            if (row == app->row)
                canvas_invert_color(canvas);
        }
        canvas_draw_box(canvas, 125, 15 + (app->row * 32 / (RowCount - 1)), 2, 8);
        canvas_draw_str(canvas, 1, 63,
                        (int32_t)(app->notice_until - furi_get_tick()) > 0 ? app->notice : "<> change   OK select");
    } else if (app->page == CsTransmit) {
        CsSettings *s = &app->settings;
        CsColor *c = cs_selected(s);
        cs_tx_status(app, text, sizeof(text));
        canvas_draw_str(canvas, 1, 9, text);
        uint32_t hz = furi_kernel_get_tick_frequency();
        uint32_t age = furi_get_tick() - app->last_activity;
        if (app->acquired && age < 6 * hz)
            snprintf(text, sizeof(text), "ACQ %lus", (unsigned long)((6 * hz - age - 1) / hz + 1));
        else
            snprintf(text, sizeof(text), "ACQ?");
        canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, text);
        if (s->advanced)
            snprintf(text, sizeof(text), "GRP%u", s->group + 1);
        else
            snprintf(text, sizeof(text), "ALL");
        canvas_draw_str(canvas, 1, 20, text);
        snprintf(text, sizeof(text), "FX%u", c->effect);
        canvas_draw_str(canvas, 33, 20, text);
        snprintf(text, sizeof(text), "R%u", c->red);
        canvas_draw_str(canvas, 60, 20, text);
        snprintf(text, sizeof(text), "G%u", c->green);
        canvas_draw_str(canvas, 84, 20, text);
        snprintf(text, sizeof(text), "B%u", c->blue);
        canvas_draw_str(canvas, 108, 20, text);
        snprintf(text, sizeof(text), "%s / %lu ms", send_names[s->send_mode],
                 (unsigned long)(s->send_mode == CsBeat ? beats_ms[s->beat_period] : intervals_ms[s->interval]));
        canvas_draw_str(canvas, 1, 30, text);
        const char *action = s->send_mode == CsHold ? "Hold OK; release = off" : "OK start / stop TX";
        if ((int32_t)(app->notice_until - furi_get_tick()) > 0)
            action = app->notice;
        canvas_draw_str(canvas, 1, 41, action);
        // Draw arrows directly so the controls do not depend on Unicode font support.
        canvas_draw_line(canvas, 3, 52, 3, 45);
        canvas_draw_line(canvas, 3, 45, 1, 47);
        canvas_draw_line(canvas, 3, 45, 5, 47);
        canvas_draw_str(canvas, 11, 52, "Acquire 3s");
        canvas_draw_line(canvas, 69, 45, 69, 52);
        canvas_draw_line(canvas, 69, 52, 67, 50);
        canvas_draw_line(canvas, 69, 52, 71, 50);
        canvas_draw_str(canvas, 77, 52, "Wake 25s");
        canvas_draw_line(canvas, 8, 59, 1, 59);
        canvas_draw_line(canvas, 1, 59, 3, 57);
        canvas_draw_line(canvas, 1, 59, 3, 61);
        canvas_draw_str(canvas, 11, 63, "Off");
        canvas_draw_line(canvas, 38, 59, 45, 59);
        canvas_draw_line(canvas, 45, 59, 43, 57);
        canvas_draw_line(canvas, 45, 59, 43, 61);
        canvas_draw_str(canvas, 50, 63, "Stop");
        canvas_draw_str(canvas, 80, 63, "Back:Menu");
    } else {
        static const char *help[4][5] = {
            {"Simple: all 10 groups", "Advanced: edit each", "Other groups stay cached", "<> change; OK selects",
             "Save stores both profiles"},
            {"Toggle: OK start/stop", "Hold: release sends off", "Burst: timed transmission", "Once: one packet only",
             "Beat: color/black pulses"},
            {"Up: dark acquire 3s", "Down: dark wake 25s", "Left: all off 300ms", "Right/Back stop radio",
             "Stop does not send off"},
            {"Preamble helps after idle", "Repeat below 6s timeout", "Random effects turn RF off",
             "Black may wait for RF", "Beat uses effect 0"},
        };
        snprintf(text, sizeof(text), "CrowdSync help %u/4", app->help_page + 1);
        canvas_draw_str(canvas, 1, 9, text);
        for (size_t i = 0; i < 5; ++i)
            canvas_draw_str(canvas, 1, 20 + i * 10, help[app->help_page][i]);
    }
    furi_mutex_release(app->mutex);
}

static void cs_input_callback(InputEvent *event, void *context)
{
    CsApp *app = context;
    furi_message_queue_put(app->input, event, FuriWaitForever);
}

int32_t crowdsync_app(void *argument)
{
    UNUSED(argument);
    CsApp *app = malloc(sizeof(CsApp));
    memset(app, 0, sizeof(*app));
    app->settings = cs_defaults();
    cs_settings_file(&app->settings, false);
    app->input = furi_message_queue_alloc(32, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->gui = furi_record_open(RECORD_GUI);
    app->viewport = view_port_alloc();
    subghz_devices_init();
    app->radio = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    view_port_draw_callback_set(app->viewport, cs_draw, app);
    view_port_input_callback_set(app->viewport, cs_input_callback, app);
    gui_add_view_port(app->gui, app->viewport, GuiLayerFullscreen);
    while (!app->exiting) {
        InputEvent event;
        bool input_ready = furi_message_queue_get(app->input, &event, furi_ms_to_ticks(50)) == FuriStatusOk;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if (input_ready)
            cs_input_handle(app, &event);
        cs_activity_update(app);
        if (app->transmitting && subghz_devices_is_async_complete_tx(app->radio))
            cs_stop(app);
        furi_mutex_release(app->mutex);
        view_port_update(app->viewport);
    }
    cs_stop(app);
    view_port_enabled_set(app->viewport, false);
    gui_remove_view_port(app->gui, app->viewport);
    view_port_free(app->viewport);
    subghz_devices_deinit();
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->input);
    free(app);
    return 0;
}
