/*
 * Lilly custom status screen for the Lily58 128x32 SSD1306 OLED (LVGL 9).
 *
 * Pixels are written straight into a portrait 32x128 L8 buffer using the
 * 3x5 font / icons from lilly_gfx.h (generated from tools/oled/font.py), then
 * rotated into the visible 128x32 landscape canvas. tools/oled/preview.py
 * renders the same layout to PNG - keep the two in sync.
 *
 * Central (left):  connection icon + profile, caps-lock dot, battery with two
 *                  bars (top = left half, bottom = right half), key log, layer.
 * Peripheral:      link icon, own battery.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define LILLY_CENTRAL 1
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#endif
#else
#define LILLY_CENTRAL 0
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#endif

#include "lilly_gfx.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---------------------------------------------------------------- layout -- */
/* keep in sync with tools/oled/preview.py */

#define PW 32  /* portrait width  */
#define PH 128 /* portrait height */

#define ICON_X 1
#define ICON_Y 1
#define PROFILE_X 10
#define PROFILE_Y 4
#define CAPS_X 27
#define CAPS_Y 4
#define BAT_X 1
#define BAT_Y 16
#define BAT_W 27
#define BAT_H 9
#define BAR_X 3
#define BAR_MAX_W 23
#define LOG_Y 31
#define LOG_ROWS 11
#define LOG_COLS 8
#define LAYER_Y 116

#define LOG_CLEAR_AFTER K_SECONDS(10)

/* ---------------------------------------------------------------- canvas -- */

#define CF LV_COLOR_FORMAT_L8
#define ON_VAL (IS_ENABLED(CONFIG_LILLY_STATUS_SCREEN_INVERTED) ? 0xFF : 0x00)
#define OFF_VAL (IS_ENABLED(CONFIG_LILLY_STATUS_SCREEN_INVERTED) ? 0x00 : 0xFF)

static uint8_t px[PH][PW]; /* portrait frame, stride = PW */
static uint8_t landscape_buf[LV_CANVAS_BUF_SIZE(PH, PW, LV_COLOR_FORMAT_GET_BPP(CF),
                                                LV_DRAW_BUF_STRIDE_ALIGN)] __aligned(4);
static lv_obj_t *landscape;

static void fill(int x, int y, int w, int h, bool on) {
    for (int yy = MAX(y, 0); yy < MIN(y + h, PH); yy++) {
        for (int xx = MAX(x, 0); xx < MIN(x + w, PW); xx++) {
            px[yy][xx] = on ? ON_VAL : OFF_VAL;
        }
    }
}

static void bitmap(int x, int y, const uint8_t *rows, int w, int h, int scale) {
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (rows[r] & BIT(w - 1 - c)) {
                fill(x + c * scale, y + r * scale, scale, scale, true);
            }
        }
    }
}

static void text(int x, int y, const char *s, int scale) {
    for (; *s; s++, x += LILLY_ADV_X * scale) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z') {
            ch -= 'a' - 'A';
        }
        if (ch < 0x20 || ch > 0x7E) {
            ch = '?';
        }
        bitmap(x, y, lilly_font[ch - 0x20], LILLY_GLYPH_W, LILLY_GLYPH_H, scale);
    }
}

#define BAT_UNKNOWN 0xFF

static void bar(int y, int h, uint8_t level) {
    if (level == BAT_UNKNOWN) {
        return;
    }
    int w = (MIN(level, 100) * BAR_MAX_W + 50) / 100;
    if (level > 0 && w == 0) {
        w = 1;
    }
    fill(BAR_X, y, w, h, true);
}

static void battery(uint8_t top, uint8_t bottom, bool split) {
    fill(BAT_X, BAT_Y, BAT_W, BAT_H, true);
    fill(BAT_X + 1, BAT_Y + 1, BAT_W - 2, BAT_H - 2, false);
    fill(BAT_X + BAT_W, BAT_Y + 2, 2, BAT_H - 4, true); /* terminal */
    if (split) {
        bar(BAT_Y + 2, 2, top);
        bar(BAT_Y + 5, 2, bottom);
    } else {
        bar(BAT_Y + 2, 5, top);
    }
}

static void present(void) {
    lv_draw_buf_t *dst = lv_canvas_get_draw_buf(landscape);
    lv_draw_sw_rotate(px, dst->data, PW, PH, PW, dst->header.stride,
                      IS_ENABLED(CONFIG_LILLY_STATUS_SCREEN_ROTATE_90) ? LV_DISPLAY_ROTATION_90
                                                                       : LV_DISPLAY_ROTATION_270,
                      CF);
    lv_obj_invalidate(landscape);
}

/* ----------------------------------------------------------------- state -- */

static struct {
    uint8_t bat_self;
#if LILLY_CENTRAL
    uint8_t bat_peripheral;
    struct zmk_endpoint_instance endpoint;
    int profile_index;
    bool profile_connected;
    bool caps;
    const char *layer_label;
    zmk_keymap_layer_index_t layer_index;
#else
    bool split_connected;
#endif
} st = {
    .bat_self = BAT_UNKNOWN,
#if LILLY_CENTRAL
    .bat_peripheral = BAT_UNKNOWN,
#endif
};

#if LILLY_CENTRAL
/* key log: LOG_ROWS lines of LOG_COLS chars, newest line last */
static char log_lines[LOG_ROWS][LOG_COLS + 1];
static uint8_t log_row; /* current (last used) row */
#endif

/* ---------------------------------------------------------------- render -- */

static void render(void) {
    if (landscape == NULL) {
        return;
    }
    memset(px, OFF_VAL, sizeof(px));

#if LILLY_CENTRAL
    if (st.endpoint.transport == ZMK_TRANSPORT_USB) {
        bitmap(ICON_X, ICON_Y, lilly_icon_usb, LILLY_ICON_USB_W, LILLY_ICON_USB_H, 1);
    } else {
        char prof[3] = {'1' + st.profile_index, st.profile_connected ? '\0' : '?', '\0'};
        bitmap(ICON_X, ICON_Y, lilly_icon_bt, LILLY_ICON_BT_W, LILLY_ICON_BT_H, 1);
        text(PROFILE_X, PROFILE_Y, prof, 1);
    }
    if (st.caps) {
        bitmap(CAPS_X, CAPS_Y, lilly_icon_dot, LILLY_ICON_DOT_W, LILLY_ICON_DOT_H, 1);
    }

    battery(st.bat_self, st.bat_peripheral, true);

    for (int i = 0; i <= log_row; i++) {
        text(0, LOG_Y + i * LILLY_ADV_Y, log_lines[i], 1);
    }

    char layer[5] = {0};
    if (st.layer_label != NULL && st.layer_label[0] != '\0') {
        strncpy(layer, st.layer_label, 4);
    } else {
        layer[0] = 'L';
        layer[1] = '0' + (st.layer_index % 10);
    }
    int w = strlen(layer) * LILLY_ADV_X * 2 - 2;
    text((PW - w) / 2, LAYER_Y, layer, 2);
#else
    bitmap(ICON_X, ICON_Y, lilly_icon_bt, LILLY_ICON_BT_W, LILLY_ICON_BT_H, 1);
    if (!st.split_connected) {
        text(PROFILE_X, PROFILE_Y, "?", 1);
    }
    battery(st.bat_self, BAT_UNKNOWN, false);
#endif

    present();
}

/* ------------------------------------------------------------- listeners -- */

/* own battery */
struct self_bat_state {
    uint8_t level;
};

static void self_bat_update(struct self_bat_state s) {
    st.bat_self = s.level;
    render();
}

static struct self_bat_state self_bat_get(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct self_bat_state){.level = ev ? ev->state_of_charge
                                               : zmk_battery_state_of_charge()};
}

ZMK_DISPLAY_WIDGET_LISTENER(lilly_self_bat, struct self_bat_state, self_bat_update, self_bat_get)
ZMK_SUBSCRIPTION(lilly_self_bat, zmk_battery_state_changed);

#if LILLY_CENTRAL

/* right half battery, fetched over split BLE */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
struct periph_bat_state {
    uint8_t level;
};

static void periph_bat_update(struct periph_bat_state s) {
    st.bat_peripheral = s.level;
    render();
}

static struct periph_bat_state periph_bat_get(const zmk_event_t *eh) {
    static uint8_t last = BAT_UNKNOWN;
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev != NULL && ev->source == 0) {
        last = ev->state_of_charge;
    }
    return (struct periph_bat_state){.level = last};
}

ZMK_DISPLAY_WIDGET_LISTENER(lilly_periph_bat, struct periph_bat_state, periph_bat_update,
                            periph_bat_get)
ZMK_SUBSCRIPTION(lilly_periph_bat, zmk_peripheral_battery_state_changed);
#endif

/* output / BLE profile */
struct output_state {
    struct zmk_endpoint_instance endpoint;
    int profile_index;
    bool profile_connected;
};

static void output_update(struct output_state s) {
    st.endpoint = s.endpoint;
    st.profile_index = s.profile_index;
    st.profile_connected = s.profile_connected;
    render();
}

static struct output_state output_get(const zmk_event_t *_eh) {
    return (struct output_state){
        .endpoint = zmk_endpoint_get_selected(),
#if IS_ENABLED(CONFIG_ZMK_BLE)
        .profile_index = zmk_ble_active_profile_index(),
        .profile_connected = zmk_ble_active_profile_is_connected(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(lilly_output, struct output_state, output_update, output_get)
ZMK_SUBSCRIPTION(lilly_output, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(lilly_output, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(lilly_output, zmk_ble_active_profile_changed);
#endif

/* layer */
struct layer_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

static void layer_update(struct layer_state s) {
    st.layer_index = s.index;
    st.layer_label = s.label;
    render();
}

static struct layer_state layer_get(const zmk_event_t *_eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_state){
        .index = index,
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(lilly_layer, struct layer_state, layer_update, layer_get)
ZMK_SUBSCRIPTION(lilly_layer, zmk_layer_state_changed);

/* caps lock */
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
struct caps_state {
    bool on;
};

static void caps_update(struct caps_state s) {
    st.caps = s.on;
    render();
}

static struct caps_state caps_get(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    zmk_hid_indicators_t ind = ev ? ev->indicators : zmk_hid_indicators_get_current_profile();
    return (struct caps_state){.on = (ind & HID_INDICATOR_CAPS_LOCK) != 0};
}

ZMK_DISPLAY_WIDGET_LISTENER(lilly_caps, struct caps_state, caps_update, caps_get)
ZMK_SUBSCRIPTION(lilly_caps, zmk_hid_indicators_changed);
#endif

/* ---------------------------------------------------------------- keylog -- */

#define KEY_NEWLINE '\n'
#define KEY_BACKSPACE '\b'

K_MSGQ_DEFINE(keylog_q, sizeof(char), 32, 1);

/* indexed from HID usage 0x1E (1) to 0x38 (/); 0 = ignore */
static const char unshifted[] = "1234567890\n\0\b  -=[]\\\0;'`,./";
static const char shifted[] = "!@#$%^&*()\n\0\b  _+{}|\0:\"~<>?";

static char usage_to_char(uint32_t usage, bool shift) {
    if (usage >= HID_USAGE_KEY_KEYBOARD_A && usage <= HID_USAGE_KEY_KEYBOARD_Z) {
        return 'A' + (usage - HID_USAGE_KEY_KEYBOARD_A);
    }
    if (usage >= HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION &&
        usage <= HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK) {
        int i = usage - HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION;
        return shift ? shifted[i] : unshifted[i];
    }
    return 0;
}

static void keylog_put(char c) {
    char *line = log_lines[log_row];
    size_t len = strlen(line);

    if (c == KEY_BACKSPACE) {
        if (len == 0 && log_row > 0) {
            log_row--;
            line = log_lines[log_row];
            len = strlen(line);
        }
        if (len > 0) {
            line[len - 1] = '\0';
        }
        return;
    }

    if (c == KEY_NEWLINE || len >= LOG_COLS) {
        if (log_row == LOG_ROWS - 1) {
            memmove(log_lines[0], log_lines[1], sizeof(log_lines[0]) * (LOG_ROWS - 1));
            memset(log_lines[LOG_ROWS - 1], 0, sizeof(log_lines[0]));
        } else {
            log_row++;
        }
        if (c == KEY_NEWLINE) {
            return;
        }
        line = log_lines[log_row];
        len = 0;
    }
    line[len] = c;
    line[len + 1] = '\0';
}

static void keylog_clear_cb(struct k_work *work) {
    memset(log_lines, 0, sizeof(log_lines));
    log_row = 0;
    render();
}
static K_WORK_DELAYABLE_DEFINE(keylog_clear_work, keylog_clear_cb);

static void keylog_drain_cb(struct k_work *work) {
    char c;
    bool changed = false;
    while (k_msgq_get(&keylog_q, &c, K_NO_WAIT) == 0) {
        keylog_put(c);
        changed = true;
    }
    if (changed) {
        render();
    }
}
static K_WORK_DEFINE(keylog_drain_work, keylog_drain_cb);

static int keylog_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state || ev->usage_page != HID_USAGE_KEY ||
        !zmk_display_is_initialized()) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_mod_flags_t mods =
        ev->implicit_modifiers | ev->explicit_modifiers | zmk_hid_get_explicit_mods();
    if (mods & (MOD_LCTL | MOD_RCTL | MOD_LALT | MOD_RALT | MOD_LGUI | MOD_RGUI)) {
        return ZMK_EV_EVENT_BUBBLE; /* shortcuts are not logged */
    }

    char c = usage_to_char(ev->keycode, (mods & (MOD_LSFT | MOD_RSFT)) != 0);
    if (c != 0) {
        k_msgq_put(&keylog_q, &c, K_NO_WAIT);
        k_work_submit_to_queue(zmk_display_work_q(), &keylog_drain_work);
        k_work_reschedule_for_queue(zmk_display_work_q(), &keylog_clear_work, LOG_CLEAR_AFTER);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(lilly_keylog, keylog_listener);
ZMK_SUBSCRIPTION(lilly_keylog, zmk_keycode_state_changed);

#else /* peripheral */

struct link_state {
    bool connected;
};

static void link_update(struct link_state s) {
    st.split_connected = s.connected;
    render();
}

static struct link_state link_get(const zmk_event_t *_eh) {
    return (struct link_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

ZMK_DISPLAY_WIDGET_LISTENER(lilly_link, struct link_state, link_update, link_get)
ZMK_SUBSCRIPTION(lilly_link, zmk_split_peripheral_status_changed);

#endif /* LILLY_CENTRAL */

/* ---------------------------------------------------------------- screen -- */

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen,
                              IS_ENABLED(CONFIG_LILLY_STATUS_SCREEN_INVERTED) ? lv_color_black()
                                                                              : lv_color_white(),
                              LV_PART_MAIN);

    landscape = lv_canvas_create(screen);
    lv_canvas_set_buffer(landscape, landscape_buf, PH, PW, CF);
    lv_obj_align(landscape, LV_ALIGN_TOP_LEFT, 0, 0);

    lilly_self_bat_init();
#if LILLY_CENTRAL
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    lilly_periph_bat_init();
#endif
    lilly_output_init();
    lilly_layer_init();
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    lilly_caps_init();
#endif
#else
    lilly_link_init();
#endif

    render();
    return screen;
}
