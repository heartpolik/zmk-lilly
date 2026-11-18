/*
 * Lilly custom status screen for the Lily58 128x32 SSD1306 OLED (LVGL 9).
 *
 * Everything is drawn on a portrait 32x128 canvas (hidden) and then rotated
 * into the visible 128x32 landscape canvas, so the OLED reads vertically.
 *
 * Central (left) half:
 *   - output: USB / BT profile + connection state
 *   - battery icon split lengthwise: left half = left battery, right half = right battery
 *   - L/R percentages
 *   - active layer name
 * Peripheral (right) half:
 *   - split link state
 *   - own battery
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define LILLY_CENTRAL 1
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#else
#define LILLY_CENTRAL 0
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---------------------------------------------------------------- canvas -- */

#define PW 32  /* portrait width  */
#define PH 128 /* portrait height */
#define CF LV_COLOR_FORMAT_L8

#define BPP LV_COLOR_FORMAT_GET_BPP(CF)

static uint8_t portrait_buf[LV_CANVAS_BUF_SIZE(PW, PH, BPP, LV_DRAW_BUF_STRIDE_ALIGN)] __aligned(4);
static uint8_t landscape_buf[LV_CANVAS_BUF_SIZE(PH, PW, BPP, LV_DRAW_BUF_STRIDE_ALIGN)] __aligned(4);

static lv_obj_t *portrait;  /* drawing surface, hidden */
static lv_obj_t *landscape; /* what the OLED shows */

#define FG (IS_ENABLED(CONFIG_LILLY_STATUS_SCREEN_INVERTED) ? lv_color_white() : lv_color_black())
#define BG (IS_ENABLED(CONFIG_LILLY_STATUS_SCREEN_INVERTED) ? lv_color_black() : lv_color_white())

/* ----------------------------------------------------------------- state -- */

#define BAT_UNKNOWN 0xFF

static struct {
    uint8_t bat_self;
    bool charging;
#if LILLY_CENTRAL
    uint8_t bat_peripheral;
    struct zmk_endpoint_instance endpoint;
    int profile_index;
    bool profile_connected;
    bool profile_bonded;
    zmk_keymap_layer_index_t layer_index;
    const char *layer_label;
#else
    bool split_connected;
#endif
} st = {
    .bat_self = BAT_UNKNOWN,
#if LILLY_CENTRAL
    .bat_peripheral = BAT_UNKNOWN,
#endif
};

/* --------------------------------------------------------------- drawing -- */

static void draw_rect(int x, int y, int w, int h, lv_color_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(portrait, &layer);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(&layer, &dsc, &a);

    lv_canvas_finish_layer(portrait, &layer);
}

static void draw_text(int y, const lv_font_t *font, const char *txt) {
    lv_layer_t layer;
    lv_canvas_init_layer(portrait, &layer);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = FG;
    dsc.font = font;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    dsc.text = txt;
    lv_area_t a = {0, y, PW - 1, y + lv_font_get_line_height(font) * 2};
    lv_draw_label(&layer, &dsc, &a);

    lv_canvas_finish_layer(portrait, &layer);
}

/*
 * Vertical battery, terminal on top. With `split`, the body is divided
 * lengthwise: left column = left half's charge, right column = right half's.
 */
#define BAT_X 2
#define BAT_Y 26
#define BAT_W 28
#define BAT_H 44
#define BAT_FILL_H (BAT_H - 4)

static void draw_fill(int x, int w, uint8_t level) {
    if (level == BAT_UNKNOWN) {
        /* unknown: small dash in the middle */
        draw_rect(x + w / 2 - 2, BAT_Y + BAT_H / 2 - 1, 4, 2, FG);
        return;
    }
    int h = (MIN(level, 100) * BAT_FILL_H + 50) / 100;
    if (level > 0 && h == 0) {
        h = 1;
    }
    draw_rect(x, BAT_Y + 2 + BAT_FILL_H - h, w, h, FG);
}

static void draw_battery(uint8_t left, uint8_t right, bool split) {
    /* terminal */
    draw_rect(BAT_X + BAT_W / 2 - 5, BAT_Y - 3, 10, 3, FG);
    /* outline */
    draw_rect(BAT_X, BAT_Y, BAT_W, BAT_H, FG);
    draw_rect(BAT_X + 1, BAT_Y + 1, BAT_W - 2, BAT_H - 2, BG);

    if (split) {
        /* divider */
        draw_rect(BAT_X + BAT_W / 2 - 1, BAT_Y, 2, BAT_H, FG);
        draw_fill(BAT_X + 3, BAT_W / 2 - 5, left);
        draw_fill(BAT_X + BAT_W / 2 + 2, BAT_W / 2 - 5, right);
    } else {
        draw_fill(BAT_X + 3, BAT_W - 6, left);
    }
}

static void format_level(char *buf, size_t len, char side, uint8_t level, bool charging) {
    if (level == BAT_UNKNOWN) {
        snprintf(buf, len, "%c --", side);
    } else if (level >= 100) {
        snprintf(buf, len, "%c100", side);
    } else {
        snprintf(buf, len, "%c%c%2u", side, charging ? '+' : ' ', level);
    }
}

static void present(void) {
    lv_draw_buf_t *src = lv_canvas_get_draw_buf(portrait);
    lv_draw_buf_t *dst = lv_canvas_get_draw_buf(landscape);

    lv_draw_sw_rotate(src->data, dst->data, PW, PH, src->header.stride, dst->header.stride,
                      IS_ENABLED(CONFIG_LILLY_STATUS_SCREEN_ROTATE_270) ? LV_DISPLAY_ROTATION_270
                                                                        : LV_DISPLAY_ROTATION_90,
                      CF);
    lv_obj_invalidate(landscape);
}

static void render(void) {
    static char line1[12], line2[12], line3[8], line4[8];

    lv_canvas_fill_bg(portrait, BG, LV_OPA_COVER);

#if LILLY_CENTRAL
    /* output */
    switch (st.endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        snprintf(line1, sizeof(line1), LV_SYMBOL_USB);
        snprintf(line2, sizeof(line2), "USB");
        break;
    case ZMK_TRANSPORT_BLE:
        snprintf(line1, sizeof(line1), LV_SYMBOL_BLUETOOTH "%d", st.profile_index + 1);
        snprintf(line2, sizeof(line2), "%s",
                 !st.profile_bonded ? "NEW" : (st.profile_connected ? "OK" : "--"));
        break;
    default:
        snprintf(line1, sizeof(line1), LV_SYMBOL_CLOSE);
        snprintf(line2, sizeof(line2), "OFF");
        break;
    }
    draw_text(0, &lv_font_montserrat_12, line1);
    draw_text(14, &lv_font_unscii_8, line2);

    /* battery: split lengthwise, left | right */
    draw_battery(st.bat_self, st.bat_peripheral, true);
    format_level(line3, sizeof(line3), 'L', st.bat_self, st.charging);
    format_level(line4, sizeof(line4), 'R', st.bat_peripheral, false);
    draw_text(BAT_Y + BAT_H + 5, &lv_font_unscii_8, line3);
    draw_text(BAT_Y + BAT_H + 15, &lv_font_unscii_8, line4);

    /* layer */
    draw_rect(0, 97, PW, 1, FG);
    if (st.layer_label != NULL && st.layer_label[0] != '\0') {
        draw_text(102, &lv_font_montserrat_12, st.layer_label);
    } else {
        static char layer_buf[8];
        snprintf(layer_buf, sizeof(layer_buf), "L%u", st.layer_index);
        draw_text(102, &lv_font_montserrat_12, layer_buf);
    }
#else
    /* split link */
    draw_text(0, &lv_font_montserrat_12, LV_SYMBOL_BLUETOOTH);
    draw_text(14, &lv_font_unscii_8, st.split_connected ? "OK" : "--");

    draw_battery(st.bat_self, BAT_UNKNOWN, false);
    format_level(line3, sizeof(line3), 'R', st.bat_self, st.charging);
    draw_text(BAT_Y + BAT_H + 5, &lv_font_unscii_8, line3);

    draw_rect(0, 97, PW, 1, FG);
    draw_text(102, &lv_font_montserrat_12, "Lily");
#endif

    present();
}

/* ------------------------------------------------------------- listeners -- */

/* own battery + USB power */
struct self_bat_state {
    uint8_t level;
    bool charging;
};

static void self_bat_update(struct self_bat_state s) {
    st.bat_self = s.level;
    st.charging = s.charging;
    render();
}

static struct self_bat_state self_bat_get(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct self_bat_state){
        .level = ev ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .charging = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(lilly_self_bat, struct self_bat_state, self_bat_update, self_bat_get)
ZMK_SUBSCRIPTION(lilly_self_bat, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(lilly_self_bat, zmk_usb_conn_state_changed);
#endif

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
    bool profile_bonded;
};

static void output_update(struct output_state s) {
    st.endpoint = s.endpoint;
    st.profile_index = s.profile_index;
    st.profile_connected = s.profile_connected;
    st.profile_bonded = s.profile_bonded;
    render();
}

static struct output_state output_get(const zmk_event_t *_eh) {
    return (struct output_state){
        .endpoint = zmk_endpoint_get_selected(),
#if IS_ENABLED(CONFIG_ZMK_BLE)
        .profile_index = zmk_ble_active_profile_index(),
        .profile_connected = zmk_ble_active_profile_is_connected(),
        .profile_bonded = !zmk_ble_active_profile_is_open(),
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
    lv_obj_set_style_bg_color(screen, BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    portrait = lv_canvas_create(screen);
    lv_canvas_set_buffer(portrait, portrait_buf, PW, PH, CF);
    lv_obj_add_flag(portrait, LV_OBJ_FLAG_HIDDEN);

    landscape = lv_canvas_create(screen);
    lv_canvas_set_buffer(landscape, landscape_buf, PH, PW, CF);
    lv_obj_align(landscape, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_fill_bg(landscape, BG, LV_OPA_COVER);

    lilly_self_bat_init();
#if LILLY_CENTRAL
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    lilly_periph_bat_init();
#endif
    lilly_output_init();
    lilly_layer_init();
#else
    lilly_link_init();
#endif

    return screen;
}
