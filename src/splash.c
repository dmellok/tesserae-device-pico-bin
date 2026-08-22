/*
 * splash.c: on-device procedural splash rendering. See splash.h.
 *
 * Draws in the panel's DISPLAY orientation (portrait for the 13.3") and maps
 * each pixel into the packed-4bpp buffer paint() consumes -- for a rotated
 * panel (descriptor width < height) the buffer is the landscape frame paint()
 * rotates back, so the splash shows upright in portrait.
 *
 * Uses the public-domain font8x8 (include/font8x8_basic.h), the MIT qrcodegen
 * (src/vendor/qrcodegen.c), and the Tesserae logo bitmap (include/
 * tesserae_logo.h, from static/brand/icon-512.png). See NOTICES.md.
 */
#include "splash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psram.h"
#include "qrcodegen.h"
#include "font8x8_basic.h"     /* char font8x8_basic[128][8] */
#include "tesserae_logo.h"     /* tesserae_logo[], TESSERAE_LOGO_W/H */

#define COL_BLK 0x0
#define COL_WHT 0x1

/* Render target, set up per call. The splash works in display coords
 * (x: 0..W-1 across, y: 0..H-1 down); px() maps to the packed buffer. */
static uint8_t *s_fb;
static int      s_W, s_H;   /* display width/height */
static bool     s_rot;      /* panel rotates (portrait-native descriptor) */
static uint8_t *s_heap;     /* malloc'd buffer to free, when not using PSRAM */

static inline void px(int x, int y, uint8_t c)
{
    if (x < 0 || y < 0 || x >= s_W || y >= s_H) return;
    int row, col, stride;
    if (s_rot) { row = s_W - 1 - x; col = y; stride = s_H / 2; }  /* landscape buffer */
    else       { row = y;           col = x; stride = s_W / 2; }
    size_t i = (size_t)row * stride + (size_t)(col >> 1);
    if (col & 1) s_fb[i] = (uint8_t)((s_fb[i] & 0xF0) | (c & 0x0F));
    else         s_fb[i] = (uint8_t)((s_fb[i] & 0x0F) | (uint8_t)(c << 4));
}

static void fill_rect(int x, int y, int w, int h, uint8_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) px(xx, yy, c);
}

/* ---------- render target ---------- */

/* Claim a frame buffer for `panel` and clear it to white. False when there is
 * nowhere to put it (a large panel on a board without PSRAM), in which case the
 * caller simply skips the splash rather than painting garbage. */
static bool fb_begin(const panel_t *panel)
{
    s_W = panel->width;
    s_H = panel->height;
    s_rot = panel->width < panel->height;   /* el133 portrait-native -> rotate */
    uint32_t bytes = (uint32_t)s_W * s_H / 2;

    s_heap = NULL;
    if (bytes <= 256u * 1024) s_fb = s_heap = malloc(bytes);
    else if (psram_init(PSRAM_CS_PIN_PLUS2) >= bytes) s_fb = (uint8_t *)PSRAM_XIP_BASE;
    else s_fb = NULL;
    if (s_fb == NULL) {
        printf("splash: no buffer for %u bytes\n", (unsigned)bytes);
        return false;
    }
    memset(s_fb, (COL_WHT << 4) | COL_WHT, bytes);   /* white background */
    return true;
}

static void fb_paint_and_end(const panel_t *panel, uint8_t variant)
{
    panel->paint(variant, s_fb);
    if (s_heap != NULL) { free(s_heap); s_heap = NULL; }
    s_fb = NULL;
}

/* ---------- logo ---------- */

static void blit_logo(int cx, int top, int size)
{
    int x0 = cx - size / 2;
    for (int dy = 0; dy < size; dy++) {
        int sy = dy * TESSERAE_LOGO_H / size;
        for (int dx = 0; dx < size; dx++) {
            int sx = dx * TESSERAE_LOGO_W / size;
            px(x0 + dx, top + dy, tesserae_logo[sy * TESSERAE_LOGO_W + sx]);
        }
    }
}

/* ---------- text (font8x8, LSB = leftmost) ---------- */

static void draw_char(int x, int y, char ch, int s, uint8_t c)
{
    const char *g = font8x8_basic[(unsigned char)ch & 0x7F];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if ((g[row] >> col) & 1) fill_rect(x + col * s, y + row * s, s, s, c);
}

static int text_w(const char *str, int s) { return (int)strlen(str) * 8 * s; }

static void draw_text_centered(int y, const char *str, int s, uint8_t c)
{
    int x = (s_W - text_w(str, s)) / 2;
    for (const char *p = str; *p; p++, x += 8 * s) draw_char(x, y, *p, s, c);
}

/* ---------- QR ---------- */

static void draw_qr(const uint8_t *qr, int x, int y, int scale)
{
    int n = qrcodegen_getSize(qr);
    fill_rect(x - 4 * scale, y - 4 * scale, (n + 8) * scale, (n + 8) * scale, COL_WHT);
    for (int qy = 0; qy < n; qy++)
        for (int qx = 0; qx < n; qx++)
            if (qrcodegen_getModule(qr, qx, qy))
                fill_rect(x + qx * scale, y + qy * scale, scale, scale, COL_BLK);
}

/* ---------- the setup splash ---------- */

void splash_show_setup(const panel_t *panel, uint8_t variant, const char *ssid)
{
    if (panel == NULL || !fb_begin(panel)) return;

    int s = s_H / 400; if (s < 1) s = 1;       /* body text scale (8px * s)   */
    int ts = 2 * s;                             /* title scale                */
    int logo = s_W / 3;                         /* logo edge, ~third of width  */

    /* QR is a WIFI: join string for the open AP; scanning connects, then the
     * captive portal pops. */
    char wifi[80];
    snprintf(wifi, sizeof wifi, "WIFI:S:%s;T:nopass;;", ssid);
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    bool have_qr = qrcodegen_encodeText(wifi, tmp, qr, qrcodegen_Ecc_MEDIUM,
                                        1, 6, qrcodegen_Mask_AUTO, true);
    int qn = have_qr ? qrcodegen_getSize(qr) : 0;
    int qscale = have_qr ? (s_H / 4) / (qn + 8) : 0;
    if (qscale < 1) qscale = 1;
    int qpix = have_qr ? qn * qscale : 0;

    char line_ssid[64];
    snprintf(line_ssid, sizeof line_ssid, "Wi-Fi:  %s", ssid);
    const char *line_url = "Open  http://192.168.4.1";

    int gap = 8 * s;
    int qz  = have_qr ? 4 * qscale : 0;        /* QR quiet-zone margin (drawn above) */
    int total = logo + gap + 8 * ts + gap + 8 * s + gap + 8 * s + gap + 8 * s
                + (have_qr ? gap + qz + qpix : 0);
    /* Bias the stack upward a little so the logo sits higher and the QR has room. */
    int y = (s_H - total) / 2 - s_H / 14; if (y < gap) y = gap;

    blit_logo(s_W / 2, y, logo);                            y += logo + gap;
    draw_text_centered(y, "Tesserae", ts, COL_BLK);         y += 8 * ts + gap;
    draw_text_centered(y, "Setup mode", s, COL_BLK);        y += 8 * s + gap;
    draw_text_centered(y, line_ssid, s, COL_BLK);           y += 8 * s + gap;
    draw_text_centered(y, line_url, s, COL_BLK);            y += 8 * s + gap;
    if (have_qr) { y += qz; draw_qr(qr, (s_W - qpix) / 2, y, qscale); }

    printf("splash: painting setup screen (display %dx%d%s)\n",
           s_W, s_H, s_rot ? ", rotated" : "");
    fb_paint_and_end(panel, variant);
}

/* ---------- status splashes ---------- */

/* Headline plus up to three body lines. font8x8 is ASCII-only, so the text is
 * deliberately plain: no apostrophes-as-quotes, no arrows, nothing that would
 * render as a blank cell on the glass. */
typedef struct {
    const char *title;
    const char *l1;
    const char *l2;
    const char *l3;
} status_copy_t;

static bool status_copy(splash_state_t st, status_copy_t *out)
{
    switch (st) {
    case SPLASH_WIFI_FAILED:
        out->title = "Wi-Fi didn't connect";
        out->l1 = "Check the network name and password.";
        out->l2 = "Setup is open again on this device:";
        out->l3 = "join its Wi-Fi and browse to 192.168.4.1";
        return true;
    case SPLASH_SERVER_UNREACHABLE:
        out->title = "Can't reach the server";
        out->l1 = "Wi-Fi is fine, but Tesserae did not answer.";
        out->l2 = "Check the server address is right and";
        out->l3 = "that the server is running.";
        return true;
    case SPLASH_AWAITING_APPROVAL:
        out->title = "Almost done";
        out->l1 = "Approve this device in Tesserae:";
        out->l2 = "Settings > Devices > Discovered.";
        out->l3 = "It will finish on its own from there.";
        return true;
    case SPLASH_RELAY_PAIRING:
        out->title = "Pairing with the relay";
        out->l1 = "Waiting for your Tesserae server to";
        out->l2 = "complete the exchange. This can take";
        out->l3 = "a few minutes; nothing to do here.";
        return true;
    case SPLASH_CONNECTED_NO_FRAME:
        out->title = "Connected!";
        out->l1 = "Waiting for your first frame.";
        out->l2 = "Assign this device a deck or page in";
        out->l3 = "Tesserae and it will appear here.";
        return true;
    case SPLASH_BATTERY_EMPTY:
        out->title = "Battery empty";
        out->l1 = "Charge or replace the cell.";
        out->l2 = "This screen stays until power returns;";
        out->l3 = "the device is asleep, not broken.";
        return true;
    default:
        return false;   /* SPLASH_NONE / SPLASH_SETUP have no status screen */
    }
}

void splash_show_status(const panel_t *panel, uint8_t variant,
                        splash_state_t state)
{
    status_copy_t c;
    if (panel == NULL || !status_copy(state, &c)) return;
    if (!fb_begin(panel)) return;

    int s = s_H / 400; if (s < 1) s = 1;
    int ts = 2 * s;
    int logo = s_W / 4;                  /* smaller than setup: the text leads */

    int gap  = 8 * s;
    int lead = 12 * s;                   /* line spacing inside the body block */
    int total = logo + gap * 2 + 8 * ts + gap + lead * 3;
    int y = (s_H - total) / 2 - s_H / 16; if (y < gap) y = gap;

    blit_logo(s_W / 2, y, logo);                       y += logo + gap * 2;
    draw_text_centered(y, c.title, ts, COL_BLK);       y += 8 * ts + gap;

    /* A rule under the headline separates it from the instructions without
     * needing a second font weight. */
    fill_rect(s_W / 4, y, s_W / 2, s, COL_BLK);        y += gap;

    if (c.l1) { draw_text_centered(y, c.l1, s, COL_BLK); y += lead; }
    if (c.l2) { draw_text_centered(y, c.l2, s, COL_BLK); y += lead; }
    if (c.l3) { draw_text_centered(y, c.l3, s, COL_BLK); y += lead; }

    printf("splash: painting status screen '%s' (state %d)\n", c.title, (int)state);
    fb_paint_and_end(panel, variant);
}
