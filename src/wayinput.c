/*
 * wayinput -- root-free virtual keyboard / pointer for Wayland compositors.
 *
 * Talks zwp_virtual_keyboard_manager_v1 (keyboard) and
 * zwlr_virtual_pointer_manager_v1 (absolute pointer) straight over the
 * Wayland socket, so it needs neither root nor /dev/uinput.
 *
 * Supported by Hyprland, Sway and other wlroots compositors. GNOME/Mutter
 * does not implement the virtual pointer protocol at all, and only lets
 * trusted clients use the virtual keyboard one.
 *
 * Usage:
 *   wayinput screen
 *   wayinput key <combo>                  e.g. "ctrl+shift+t", "Return"
 *   wayinput type <text>
 *   wayinput move <x> <y>
 *   wayinput click <x> <y> [left|right|middle] [count]
 *   wayinput scroll <dy> [dx]             dy in wheel notches, +down
 *
 * Exit codes: 0 ok, 2 usage, 3 protocol unavailable, 4 key lookup failure.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

/* Linux input button codes; kept local to avoid needing linux/input.h. */
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

/* xkbcommon modifier mask bits */
#define MOD_SHIFT (1u << 0)
#define MOD_CTRL  (1u << 2)
#define MOD_ALT   (1u << 3)
#define MOD_MOD3  (1u << 5)
#define MOD_LOGO  (1u << 6)

struct state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct wl_output *output;
    int out_w, out_h;
    int ext_w, ext_h;          /* extents used for absolute pointer motion */

    struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
    struct zwp_virtual_keyboard_v1 *vk;
    struct zwlr_virtual_pointer_manager_v1 *vp_mgr;
    struct zwlr_virtual_pointer_v1 *vp;

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *keymap;
    uint32_t mods;
};

static struct state S;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void sync_now(void)
{
    wl_display_flush(S.display);
    wl_display_roundtrip(S.display);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: wayinput <command> [args]\n"
        "  screen\n"
        "  key <combo>\n"
        "  type <text>\n"
        "  move <x> <y>\n"
        "  click <x> <y> [left|right|middle] [count]\n"
        "  scroll <dy> [dx]\n");
    exit(2);
}

/* ---------------------------------------------------------------- output */

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t subpixel,
                         const char *make, const char *model, int32_t transform)
{
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void out_mode(void *data, struct wl_output *o, uint32_t flags,
                     int32_t width, int32_t height, int32_t refresh)
{
    struct state *s = data;
    (void)o; (void)refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        s->out_w = width;
        s->out_h = height;
    }
}

static void out_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static void out_scale(void *data, struct wl_output *o, int32_t f) { (void)data; (void)o; (void)f; }
static void out_name(void *data, struct wl_output *o, const char *n) { (void)data; (void)o; (void)n; }
static void out_desc(void *data, struct wl_output *o, const char *d) { (void)data; (void)o; (void)d; }

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_desc,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data; (void)seat; (void)caps;
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

/* -------------------------------------------------------------- registry */

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version)
{
    struct state *s = data;

    if (strcmp(iface, wl_seat_interface.name) == 0) {
        s->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
        wl_seat_add_listener(s->seat, &seat_listener, s);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (!s->output) {
            s->output = wl_registry_bind(reg, name, &wl_output_interface,
                                         version < 3 ? version : 3);
            wl_output_add_listener(s->output, &output_listener, s);
        }
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        s->vk_mgr = wl_registry_bind(reg, name,
                                     &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        s->vp_mgr = wl_registry_bind(reg, name,
                                     &zwlr_virtual_pointer_manager_v1_interface,
                                     version < 2 ? version : 2);
    }
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static int connect_display(void)
{
    S.display = wl_display_connect(NULL);
    if (!S.display) {
        const char *d = getenv("WAYLAND_DISPLAY");
        fprintf(stderr, "wayinput: cannot connect to Wayland display (%s)\n",
                d ? d : "WAYLAND_DISPLAY unset");
        return -1;
    }
    S.registry = wl_display_get_registry(S.display);
    wl_registry_add_listener(S.registry, &registry_listener, &S);
    wl_display_roundtrip(S.display);
    wl_display_roundtrip(S.display); /* second round so outputs report modes */

    S.ext_w = S.out_w;
    S.ext_h = S.out_h;
    if (S.ext_w <= 0 || S.ext_h <= 0) {
        const char *w = getenv("DESKCTL_SCREEN_W");
        const char *h = getenv("DESKCTL_SCREEN_H");
        if (w && h) {
            S.ext_w = atoi(w);
            S.ext_h = atoi(h);
        }
    }
    return 0;
}

/* -------------------------------------------------------------- keyboard */

static int lookup_keysym(xkb_keysym_t sym, xkb_keycode_t *kc_out, int *level_out)
{
    for (xkb_keycode_t kc = 8; kc < 256; kc++) {
        int levels = xkb_keymap_num_levels_for_key(S.keymap, kc, 0);
        for (int level = 0; level < levels && level < 4; level++) {
            const xkb_keysym_t *syms = NULL;
            int n = xkb_keymap_key_get_syms_by_level(S.keymap, kc, 0, level, &syms);
            for (int i = 0; i < n; i++) {
                if (syms[i] == sym) {
                    *kc_out = kc;
                    *level_out = level;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static void set_mods(uint32_t mods)
{
    S.mods = mods;
    zwp_virtual_keyboard_v1_modifiers(S.vk, mods, 0, 0, 0);
}

static void send_key(xkb_keycode_t kc, int pressed)
{
    /* Wayland key codes are evdev codes minus 8. */
    zwp_virtual_keyboard_v1_key(S.vk, now_ms(), (uint32_t)(kc - 8),
                                pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                        : WL_KEYBOARD_KEY_STATE_RELEASED);
}

static int keycode_for(xkb_keysym_t sym, xkb_keycode_t *kc, int *level)
{
    if (lookup_keysym(sym, kc, level) < 0) {
        fprintf(stderr, "wayinput: keysym 0x%x not in keymap\n", (unsigned)sym);
        return -1;
    }
    return 0;
}

/* Extra modifier implied by the keymap level of this keysym. */
static uint32_t level_modifier(int level)
{
    if (level == 1 || level == 3) return MOD_SHIFT;
    if (level == 2) return MOD_MOD3;
    return 0;
}

static void key_down(xkb_keysym_t sym)
{
    xkb_keycode_t kc;
    int level;
    if (keycode_for(sym, &kc, &level) == 0)
        send_key(kc, 1);
}

static void key_up(xkb_keysym_t sym)
{
    xkb_keycode_t kc;
    int level;
    if (keycode_for(sym, &kc, &level) == 0)
        send_key(kc, 0);
}

static void tap_keysym(xkb_keysym_t sym)
{
    xkb_keycode_t kc;
    int level;
    if (keycode_for(sym, &kc, &level) < 0)
        return;

    uint32_t extra = level_modifier(level);
    xkb_keysym_t mod_sym = (extra == MOD_MOD3) ? XKB_KEY_ISO_Level3_Shift
                                               : XKB_KEY_Shift_L;
    uint32_t saved = S.mods;
    if (extra) {
        key_down(mod_sym);
        set_mods(saved | extra);
    }
    send_key(kc, 1);
    send_key(kc, 0);
    if (extra) {
        key_up(mod_sym);
        set_mods(saved);
    }
}

static int kb_init(void)
{
    if (!S.vk_mgr) {
        fprintf(stderr, "wayinput: compositor does not offer "
                        "zwp_virtual_keyboard_manager_v1\n");
        return -1;
    }
    const char *layout = getenv("DESKCTL_XKB_LAYOUT");
    const char *variant = getenv("DESKCTL_XKB_VARIANT");
    if (!layout || !*layout) layout = "us";
    if (!variant) variant = "";

    struct xkb_rule_names names = {
        .rules = "evdev",
        .model = "pc105",
        .layout = layout,
        .variant = variant,
        .options = "",
    };

    S.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!S.xkb_ctx) return -1;
    S.keymap = xkb_keymap_new_from_names(S.xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!S.keymap) {
        fprintf(stderr, "wayinput: cannot compile keymap for layout '%s'\n", layout);
        return -1;
    }

    char *text = xkb_keymap_get_as_string(S.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!text) return -1;
    size_t len = strlen(text) + 1;

    int fd = memfd_create("deskctl-keymap", MFD_CLOEXEC);
    if (fd < 0) { free(text); return -1; }
    if (ftruncate(fd, (off_t)len) < 0) { close(fd); free(text); return -1; }
    void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); free(text); return -1; }
    memcpy(map, text, len);
    munmap(map, len);
    free(text);

    S.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(S.vk_mgr, S.seat);
    zwp_virtual_keyboard_v1_keymap(S.vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    close(fd);
    sync_now(); /* compositor must install the keymap before any key event */
    return 0;
}

/* Decode one UTF-8 code point; returns bytes consumed (never 0). */
static size_t utf8_next(const char **p, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!*s) { *cp = 0; return 0; }
    uint32_t c = s[0];
    int extra;
    if (c < 0x80) { extra = 0; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1Fu; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0Fu; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07u; extra = 3; }
    else { *p += 1; *cp = '?'; return 1; }

    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; *cp = '?'; return 1; }
        c = (c << 6) | (s[i] & 0x3Fu);
    }
    *p += 1 + extra;
    *cp = c;
    return (size_t)(1 + extra);
}

static void type_text(const char *text)
{
    /* Some input methods (fcitx5) drop or reorder keys sent in a tight burst. */
    const char *delay_env = getenv("DESKCTL_TYPE_DELAY_MS");
    useconds_t delay_us = delay_env ? (useconds_t)(atoi(delay_env) * 1000) : 0;

    const char *p = text;
    while (*p) {
        uint32_t cp;
        if (utf8_next(&p, &cp) == 0) break;
        if (delay_us) usleep(delay_us);

        if (cp == '\n') { tap_keysym(XKB_KEY_Return); continue; }
        if (cp == '\t') { tap_keysym(XKB_KEY_Tab); continue; }

        xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
        if (sym == XKB_KEY_NoSymbol) {
            fprintf(stderr, "wayinput: no keysym for U+%04X, skipped\n", cp);
            continue;
        }
        tap_keysym(sym);
    }
}

static int modifier_for_name(const char *name, xkb_keysym_t *sym, uint32_t *mask)
{
    if (!strcmp(name, "ctrl") || !strcmp(name, "control")) {
        *sym = XKB_KEY_Control_L; *mask = MOD_CTRL; return 1;
    }
    if (!strcmp(name, "shift")) { *sym = XKB_KEY_Shift_L;   *mask = MOD_SHIFT; return 1; }
    if (!strcmp(name, "alt"))   { *sym = XKB_KEY_Alt_L;     *mask = MOD_ALT;   return 1; }
    if (!strcmp(name, "super") || !strcmp(name, "meta") ||
        !strcmp(name, "win")   || !strcmp(name, "logo")) {
        *sym = XKB_KEY_Super_L; *mask = MOD_LOGO; return 1;
    }
    if (!strcmp(name, "altgr")) { *sym = XKB_KEY_ISO_Level3_Shift; *mask = MOD_MOD3; return 1; }
    return 0;
}

static xkb_keysym_t name_to_keysym(const char *name)
{
    if (strlen(name) == 1) {
        xkb_keysym_t sym = xkb_utf32_to_keysym((uint32_t)(unsigned char)name[0]);
        if (sym != XKB_KEY_NoSymbol) return sym;
    }
    return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
}

static int send_combo(const char *combo)
{
    char *buf = strdup(combo);
    if (!buf) return -1;

    xkb_keysym_t mod_syms[8];
    uint32_t mod_masks[8];
    int nmods = 0;
    xkb_keysym_t main_sym = XKB_KEY_NoSymbol;

    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        xkb_keysym_t msym;
        uint32_t mmask;
        if (modifier_for_name(tok, &msym, &mmask)) {
            if (nmods < 8) { mod_syms[nmods] = msym; mod_masks[nmods] = mmask; nmods++; }
            continue;
        }
        main_sym = name_to_keysym(tok);
        if (main_sym == XKB_KEY_NoSymbol) {
            fprintf(stderr, "wayinput: unknown key name '%s'\n", tok);
            free(buf);
            return -1;
        }
    }
    free(buf);

    if (main_sym == XKB_KEY_NoSymbol) {
        fprintf(stderr, "wayinput: combo '%s' has no non-modifier key\n", combo);
        return -1;
    }

    uint32_t pressed = 0;
    for (int i = 0; i < nmods; i++) {
        key_down(mod_syms[i]);
        pressed |= mod_masks[i];
        set_mods(pressed);
    }

    xkb_keycode_t kc;
    int level;
    if (keycode_for(main_sym, &kc, &level) == 0) {
        uint32_t extra = level_modifier(level);
        if (extra) {
            key_down(XKB_KEY_Shift_L);
            set_mods(pressed | extra);
        }
        send_key(kc, 1);
        send_key(kc, 0);
        if (extra) {
            key_up(XKB_KEY_Shift_L);
            set_mods(pressed);
        }
    }

    for (int i = nmods - 1; i >= 0; i--) {
        key_up(mod_syms[i]);
        pressed &= ~mod_masks[i];
        set_mods(pressed);
    }
    return 0;
}

/* --------------------------------------------------------------- pointer */

static int vp_init(void)
{
    if (!S.vp_mgr) {
        fprintf(stderr, "wayinput: compositor does not offer "
                        "zwlr_virtual_pointer_manager_v1\n");
        return -1;
    }
    S.vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(S.vp_mgr, S.seat);
    if (!S.vp) return -1;
    return 0;
}

static int need_extents(void)
{
    if (S.ext_w > 0 && S.ext_h > 0) return 0;
    fprintf(stderr, "wayinput: unknown screen extents; set "
                    "DESKCTL_SCREEN_W/DESKCTL_SCREEN_H\n");
    return -1;
}

static void ptr_move(int x, int y)
{
    zwlr_virtual_pointer_v1_motion_absolute(S.vp, now_ms(),
                                            (uint32_t)x, (uint32_t)y,
                                            (uint32_t)S.ext_w, (uint32_t)S.ext_h);
    zwlr_virtual_pointer_v1_frame(S.vp);
}

static void ptr_button(uint32_t btn, int pressed)
{
    zwlr_virtual_pointer_v1_button(S.vp, now_ms(), btn,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(S.vp);
}

static int parse_button(const char *name, uint32_t *btn)
{
    if (!name) { return -1; }
    if (!strcmp(name, "left")   || !strcmp(name, "1")) { *btn = BTN_LEFT;   return 0; }
    if (!strcmp(name, "right")  || !strcmp(name, "3")) { *btn = BTN_RIGHT;  return 0; }
    if (!strcmp(name, "middle") || !strcmp(name, "2")) { *btn = BTN_MIDDLE; return 0; }
    return -1;
}

/* -------------------------------------------------------------- commands */

static int cmd_screen(void)
{
    printf("%dx%d\n", S.out_w, S.out_h);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) usage();
    const char *cmd = argv[1];

    if (connect_display() < 0) return 3;

    if (!strcmp(cmd, "screen")) {
        int rc = cmd_screen();
        wl_display_disconnect(S.display);
        return rc;
    }

    if (!strcmp(cmd, "key")) {
        if (argc < 3) usage();
        if (kb_init() < 0) return 3;
        int rc = send_combo(argv[2]) == 0 ? 0 : 4;
        sync_now();
        return rc;
    }

    if (!strcmp(cmd, "type")) {
        if (argc < 3) usage();
        if (kb_init() < 0) return 3;
        type_text(argv[2]);
        sync_now();
        return 0;
    }

    if (!strcmp(cmd, "move")) {
        if (argc < 4) usage();
        if (vp_init() < 0) return 3;
        if (need_extents() < 0) return 3;
        ptr_move(atoi(argv[2]), atoi(argv[3]));
        sync_now();
        return 0;
    }

    if (!strcmp(cmd, "click")) {
        if (argc < 4) usage();
        uint32_t btn = BTN_LEFT;
        int count = 1;
        if (argc >= 5 && parse_button(argv[4], &btn) < 0) usage();
        if (argc >= 6) count = atoi(argv[5]);
        if (count < 1) count = 1;
        if (vp_init() < 0) return 3;
        if (need_extents() < 0) return 3;
        ptr_move(atoi(argv[2]), atoi(argv[3]));
        sync_now();
        for (int i = 0; i < count; i++) {
            ptr_button(btn, 1);
            sync_now();
            usleep(20000);
            ptr_button(btn, 0);
            sync_now();
            if (i + 1 < count) usleep(40000);
        }
        return 0;
    }

    if (!strcmp(cmd, "scroll")) {
        if (argc < 3) usage();
        if (vp_init() < 0) return 3;
        double dy = atof(argv[2]);
        double dx = argc >= 4 ? atof(argv[3]) : 0.0;
        zwlr_virtual_pointer_v1_axis_source(S.vp, WL_POINTER_AXIS_SOURCE_WHEEL);
        if (dy != 0.0)
            zwlr_virtual_pointer_v1_axis(S.vp, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL,
                                         wl_fixed_from_double(dy * 15.0));
        if (dx != 0.0)
            zwlr_virtual_pointer_v1_axis(S.vp, now_ms(), WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                         wl_fixed_from_double(dx * 15.0));
        zwlr_virtual_pointer_v1_frame(S.vp);
        sync_now();
        return 0;
    }

    usage();
    return 2;
}
