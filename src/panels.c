/*
 * panels.c: one descriptor + paint() per supported Inky panel, selected at
 * runtime by the model EEPROM's display_variant. paint(variant, frame) runs the
 * whole sequence for its panel: SPI/GPIO setup, reset, init, stream a frame,
 * refresh. frame == NULL streams the built-in vertical-stripe test pattern;
 * otherwise frame is the panel-native packed-4bpp buffer.
 *
 * Command sequences are ported from the Pimoroni Inky drivers
 * (github.com/pimoroni/inky):
 *   EL133UF1 (13.3 Spectra 6)  inky_el133uf1.py   -- VERIFIED on hardware
 *   E673     (7.3 Spectra 6)   inky_e673.py       -- untested port
 *   E640     (4.0 Spectra 6)   inky_e640.py       -- untested port
 *   AC073TC1A(7.3 7-colour)    inky_ac073tc1a.py  -- untested port
 *   UC8159   (7-colour)        inky_uc8159.py     -- untested port
 *
 * Shared transport lives in epd_io.c; see CLAUDE.md for the wiring story.
 */
#include "panels.h"
#include "epd_io.h"

#include <stddef.h>
#include <stdio.h>

static const uint8_t cs8       = 8;
static const uint8_t cs_m      = 26;
static const uint8_t cs_s      = 16;
static const uint8_t cs_both[] = {26, 16};

#define SETUP_SPECTRA6  300
#define SETUP_7COLOUR   0
#define MAX_ROW_BYTES   512

static const uint8_t PAL_SPECTRA6[6] = {0x0, 0x1, 0x3, 0x6, 0x5, 0x2};        /* blk wht red grn blu yel */
static const uint8_t PAL_7COLOUR[7]  = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6};   /* blk wht grn blu red yel org */

static void fill_stripe_row(uint8_t *dst, int row_bytes, int total_w,
                            int base_col, const uint8_t *pal, int n)
{
    int stripe_w = total_w / n;
    for (int b = 0; b < row_bytes; b++) {
        int c0 = base_col + b * 2;
        int c1 = c0 + 1;
        int s0 = c0 / stripe_w; if (s0 >= n) s0 = n - 1;
        int s1 = c1 / stripe_w; if (s1 >= n) s1 = n - 1;
        dst[b] = (uint8_t)((pal[s0] << 4) | (pal[s1] & 0x0F));
    }
}

/* Stream a single-controller frame to one CS. If frame is NULL, stream uniform
 * stripe rows; otherwise stream the frame row by row. */
static void stream_single(uint8_t cs, uint32_t setup, int width, int height,
                          const uint8_t *frame, const uint8_t *pal, int n)
{
    uint8_t row[MAX_ROW_BYTES];
    int row_bytes = width / 2;
    if (!frame) fill_stripe_row(row, row_bytes, width, 0, pal, n);
    epd_dtm_begin(cs, setup, 0x10);
    for (int y = 0; y < height; y++) {
        const uint8_t *src = frame ? (frame + (size_t)y * row_bytes) : row;
        epd_dtm_write(src, row_bytes);
    }
    epd_dtm_end();
}

/* ===================== EL133UF1, 13.3" Spectra 6 (verified) ===================== */

static void paint_el133uf1(uint8_t variant, const uint8_t *frame)
{
    (void)variant;
    epd_io_init(4 * 1000 * 1000);
    epd_cs_init(cs_m);
    epd_cs_init(cs_s);
    epd_reset(1, 30, 30, 300);

    const uint32_t S = SETUP_SPECTRA6;
    static const uint8_t ANTM[]  = {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55};
    static const uint8_t CMD66[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
    static const uint8_t PSR[]   = {0xDF, 0x6B};
    static const uint8_t DCDC[]  = {0x44, 0x54, 0x00};
    static const uint8_t PLL[]   = {0x08};
    static const uint8_t CDI[]   = {0x37};
    static const uint8_t TCON[]  = {0x03, 0x03};
    static const uint8_t POFS0[] = {0x00, 0xC0, 0x03, 0xA8};
    static const uint8_t POFS1[] = {0x00, 0xC0, 0x03, 0x9A};
    static const uint8_t AGID[]  = {0x10};
    static const uint8_t PWS[]   = {0x22};
    static const uint8_t CCSET[] = {0x01};
    static const uint8_t TRES[]  = {0x04, 0xB0, 0x03, 0x20};
    static const uint8_t CMDA4[] = {0x03, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t PWR[]   = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
    static const uint8_t ENBUF[] = {0x07};
    static const uint8_t BTSTP[] = {0xE0, 0x20};
    static const uint8_t BVDDP[] = {0x01};
    static const uint8_t BTSTN[] = {0xE0, 0x20};
    static const uint8_t BBVDN[] = {0x01};
    static const uint8_t VCOMP[] = {0x02};

    epd_command(&cs_m,  1, S, 0x74, ANTM,  sizeof ANTM);
    epd_command(cs_both,2, S, 0xF0, CMD66, sizeof CMD66);
    epd_command(cs_both,2, S, 0x00, PSR,   sizeof PSR);
    epd_command(&cs_m,  1, S, 0xA5, DCDC,  sizeof DCDC);
    epd_command(cs_both,2, S, 0x30, PLL,   sizeof PLL);
    epd_command(cs_both,2, S, 0x50, CDI,   sizeof CDI);
    epd_command(cs_both,2, S, 0x60, TCON,  sizeof TCON);
    epd_command(&cs_m,  1, S, 0x03, POFS0, sizeof POFS0);
    epd_command(&cs_s,  1, S, 0x03, POFS1, sizeof POFS1);
    epd_command(cs_both,2, S, 0x86, AGID,  sizeof AGID);
    epd_command(cs_both,2, S, 0xE3, PWS,   sizeof PWS);
    epd_command(cs_both,2, S, 0xE0, CCSET, sizeof CCSET);
    epd_command(cs_both,2, S, 0x61, TRES,  sizeof TRES);
    epd_command(&cs_m,  1, S, 0xA4, CMDA4, sizeof CMDA4);
    epd_command(&cs_m,  1, S, 0x01, PWR,   sizeof PWR);
    epd_command(&cs_m,  1, S, 0xB6, ENBUF, sizeof ENBUF);
    epd_command(&cs_m,  1, S, 0x06, BTSTP, sizeof BTSTP);
    epd_command(&cs_m,  1, S, 0xB7, BVDDP, sizeof BVDDP);
    epd_command(&cs_m,  1, S, 0x05, BTSTN, sizeof BTSTN);
    epd_command(&cs_m,  1, S, 0xB0, BBVDN, sizeof BBVDN);
    epd_command(&cs_m,  1, S, 0xB1, VCOMP, sizeof VCOMP);
    printf("epd: init complete\n");

    /* Per row: bytes [0..299] = left half (CS_M), [300..599] = right half (CS_S).
     * Frame stride is 600 bytes; stripe fallback generates per-half rows. */
    uint8_t row[MAX_ROW_BYTES];
    if (!frame) fill_stripe_row(row, 300, 1200, 0, PAL_SPECTRA6, 6);
    epd_dtm_begin(cs_m, S, 0x10);
    for (int y = 0; y < 1600; y++)
        epd_dtm_write(frame ? frame + (size_t)y * 600 : row, 300);
    epd_dtm_end();

    if (!frame) fill_stripe_row(row, 300, 1200, 600, PAL_SPECTRA6, 6);
    epd_dtm_begin(cs_s, S, 0x10);
    for (int y = 0; y < 1600; y++)
        epd_dtm_write(frame ? frame + (size_t)y * 600 + 300 : row, 300);
    epd_dtm_end();
    printf("epd: frame streamed, refreshing...\n");

    static const uint8_t z = 0x00;
    epd_command(cs_both, 2, S, 0x04, NULL, 0);   sleep_ms(300);
    epd_command(cs_both, 2, S, 0x12, &z, 1);     epd_wait_ready(60000);
    epd_command(cs_both, 2, S, 0x02, &z, 1);     sleep_ms(300);
    printf("epd: refresh done\n");
}

/* ===================== Spectra 6 small (E673 7.3", E640 4.0") ===================== */

static void paint_spectra6_small(int width, int height, const uint8_t tres[4],
                                 uint8_t btst2_last, uint32_t drf_timeout_ms,
                                 const uint8_t *psr_rewrite, const uint8_t *frame)
{
    epd_io_init(1 * 1000 * 1000);
    epd_cs_init(cs8);
    epd_reset(1, 30, 30, 300);

    const uint32_t S = SETUP_SPECTRA6;
    static const uint8_t UNLOCK[] = {0x49, 0x55, 0x20, 0x08, 0x09, 0x18};
    static const uint8_t PWR[]    = {0x3F};
    static const uint8_t PSR[]    = {0x5F, 0x69};
    static const uint8_t BTST1[]  = {0x40, 0x1F, 0x1F, 0x2C};
    static const uint8_t BTST3[]  = {0x6F, 0x1F, 0x1F, 0x22};
    static const uint8_t BTST2[]  = {0x6F, 0x1F, 0x17, 0x17};
    static const uint8_t POFS[]   = {0x00, 0x54, 0x00, 0x44};
    static const uint8_t TCON[]   = {0x02, 0x00};
    static const uint8_t PLL[]    = {0x08};
    static const uint8_t CDI[]    = {0x3F};
    static const uint8_t PWS[]    = {0x2F};
    static const uint8_t VDCS[]   = {0x01};

    epd_command(&cs8, 1, S, 0xAA, UNLOCK, sizeof UNLOCK);
    epd_command(&cs8, 1, S, 0x01, PWR,   sizeof PWR);
    epd_command(&cs8, 1, S, 0x00, PSR,   sizeof PSR);
    epd_command(&cs8, 1, S, 0x05, BTST1, sizeof BTST1);
    epd_command(&cs8, 1, S, 0x08, BTST3, sizeof BTST3);
    epd_command(&cs8, 1, S, 0x06, BTST2, sizeof BTST2);
    epd_command(&cs8, 1, S, 0x03, POFS,  sizeof POFS);
    epd_command(&cs8, 1, S, 0x60, TCON,  sizeof TCON);
    epd_command(&cs8, 1, S, 0x30, PLL,   sizeof PLL);
    epd_command(&cs8, 1, S, 0x50, CDI,   sizeof CDI);
    epd_command(&cs8, 1, S, 0x61, tres,  4);
    epd_command(&cs8, 1, S, 0xE3, PWS,   sizeof PWS);
    epd_command(&cs8, 1, S, 0x82, VDCS,  sizeof VDCS);
    printf("epd: init complete\n");

    stream_single(cs8, S, width, height, frame, PAL_SPECTRA6, 6);
    printf("epd: frame streamed, refreshing...\n");

    const uint8_t btst2b[4] = {0x6F, 0x1F, 0x17, btst2_last};
    static const uint8_t z = 0x00;
    epd_command(&cs8, 1, S, 0x04, NULL, 0);    sleep_ms(300);
    epd_command(&cs8, 1, S, 0x06, btst2b, 4);
    epd_command(&cs8, 1, S, 0x12, &z, 1);      epd_wait_ready(drf_timeout_ms);
    epd_command(&cs8, 1, S, 0x02, &z, 1);      sleep_ms(300);
    if (psr_rewrite) { epd_command(&cs8, 1, S, 0x00, psr_rewrite, 2); sleep_ms(300); }
    printf("epd: refresh done\n");
}

static void paint_e673(uint8_t variant, const uint8_t *frame)
{
    (void)variant;
    static const uint8_t tres[4] = {0x03, 0x20, 0x01, 0xE0};
    static const uint8_t psr_rewrite[2] = {0x4F, 0x6E};
    paint_spectra6_small(800, 480, tres, 0x49, 40000, psr_rewrite, frame);
}

static void paint_e640(uint8_t variant, const uint8_t *frame)
{
    (void)variant;
    static const uint8_t tres[4] = {0x01, 0x90, 0x02, 0x58};
    paint_spectra6_small(600, 400, tres, 0x47, 50000, NULL, frame);
}

/* ===================== AC073TC1A, 7.3" 7-colour (untested) ===================== */

static void paint_ac073(uint8_t variant, const uint8_t *frame)
{
    (void)variant;
    epd_io_init(5 * 1000 * 1000);
    epd_cs_init(cs8);
    epd_reset(2, 100, 100, 1000);

    const uint32_t S = SETUP_7COLOUR;
    static const uint8_t CMDH[]  = {0x49, 0x55, 0x20, 0x08, 0x09, 0x18};
    static const uint8_t PWR[]   = {0x3F, 0x00, 0x32, 0x2A, 0x0E, 0x2A};
    static const uint8_t PSR[]   = {0x5F, 0x69};
    static const uint8_t POFS[]  = {0x00, 0x54, 0x00, 0x44};
    static const uint8_t BTST1[] = {0x40, 0x1F, 0x1F, 0x2C};
    static const uint8_t BTST2[] = {0x6F, 0x1F, 0x16, 0x25};
    static const uint8_t BTST3[] = {0x6F, 0x1F, 0x1F, 0x22};
    static const uint8_t IPC[]   = {0x00, 0x04};
    static const uint8_t PLL[]   = {0x02};
    static const uint8_t TSE[]   = {0x00};
    static const uint8_t CDI[]   = {0x3F};
    static const uint8_t TCON[]  = {0x02, 0x00};
    static const uint8_t TRES[]  = {0x03, 0x20, 0x01, 0xE0};
    static const uint8_t VDCS[]  = {0x1E};
    static const uint8_t TVDCS[] = {0x00};
    static const uint8_t AGID[]  = {0x00};
    static const uint8_t PWS[]   = {0x2F};
    static const uint8_t CCSET[] = {0x00};
    static const uint8_t TSSET[] = {0x00};

    epd_command(&cs8, 1, S, 0xAA, CMDH,  sizeof CMDH);
    epd_command(&cs8, 1, S, 0x01, PWR,   sizeof PWR);
    epd_command(&cs8, 1, S, 0x00, PSR,   sizeof PSR);
    epd_command(&cs8, 1, S, 0x03, POFS,  sizeof POFS);
    epd_command(&cs8, 1, S, 0x05, BTST1, sizeof BTST1);
    epd_command(&cs8, 1, S, 0x06, BTST2, sizeof BTST2);
    epd_command(&cs8, 1, S, 0x08, BTST3, sizeof BTST3);
    epd_command(&cs8, 1, S, 0x13, IPC,   sizeof IPC);
    epd_command(&cs8, 1, S, 0x30, PLL,   sizeof PLL);
    epd_command(&cs8, 1, S, 0x41, TSE,   sizeof TSE);
    epd_command(&cs8, 1, S, 0x50, CDI,   sizeof CDI);
    epd_command(&cs8, 1, S, 0x60, TCON,  sizeof TCON);
    epd_command(&cs8, 1, S, 0x61, TRES,  sizeof TRES);
    epd_command(&cs8, 1, S, 0x82, VDCS,  sizeof VDCS);
    epd_command(&cs8, 1, S, 0x84, TVDCS, sizeof TVDCS);
    epd_command(&cs8, 1, S, 0x86, AGID,  sizeof AGID);
    epd_command(&cs8, 1, S, 0xE3, PWS,   sizeof PWS);
    epd_command(&cs8, 1, S, 0xE0, CCSET, sizeof CCSET);
    epd_command(&cs8, 1, S, 0xE6, TSSET, sizeof TSSET);
    printf("epd: init complete\n");

    stream_single(cs8, S, 800, 480, frame, PAL_7COLOUR, 7);
    printf("epd: frame streamed, refreshing...\n");

    static const uint8_t z = 0x00;
    epd_command(&cs8, 1, S, 0x04, NULL, 0);   sleep_ms(400);
    epd_command(&cs8, 1, S, 0x12, &z, 1);     epd_wait_ready(50000);
    epd_command(&cs8, 1, S, 0x02, &z, 1);     sleep_ms(400);
    printf("epd: refresh done\n");
}

/* ===================== UC8159, 7-colour (untested) ===================== */

static void paint_uc8159(uint8_t variant, const uint8_t *frame)
{
    int width  = (variant == 16) ? 640 : 600;
    int height = (variant == 16) ? 400 : 448;
    uint8_t res_setting = (variant == 16) ? 0b10 : 0b11;

    epd_io_init(3 * 1000 * 1000);
    epd_cs_init(cs8);
    epd_reset(1, 100, 100, 1000);

    const uint32_t S = SETUP_7COLOUR;
    const uint8_t tres[4] = {(uint8_t)(width >> 8), (uint8_t)width,
                             (uint8_t)(height >> 8), (uint8_t)height};
    const uint8_t psr[2]  = {(uint8_t)((res_setting << 6) | 0x2F), 0x08};
    static const uint8_t PWR[]  = {0x37, 0x00, 0x23, 0x23};
    static const uint8_t PLL[]  = {0x3C};
    static const uint8_t TSE[]  = {0x00};
    static const uint8_t CDI[]  = {0x37};
    static const uint8_t TCON[] = {0x22};
    static const uint8_t DAM[]  = {0x00};
    static const uint8_t PWS[]  = {0xAA};
    static const uint8_t PFS[]  = {0x00};

    epd_command(&cs8, 1, S, 0x61, tres, 4);
    epd_command(&cs8, 1, S, 0x00, psr,  2);
    epd_command(&cs8, 1, S, 0x01, PWR,  sizeof PWR);
    epd_command(&cs8, 1, S, 0x30, PLL,  sizeof PLL);
    epd_command(&cs8, 1, S, 0x41, TSE,  sizeof TSE);
    epd_command(&cs8, 1, S, 0x50, CDI,  sizeof CDI);
    epd_command(&cs8, 1, S, 0x60, TCON, sizeof TCON);
    epd_command(&cs8, 1, S, 0x65, DAM,  sizeof DAM);
    epd_command(&cs8, 1, S, 0xE3, PWS,  sizeof PWS);
    epd_command(&cs8, 1, S, 0x03, PFS,  sizeof PFS);
    printf("epd: init complete\n");

    stream_single(cs8, S, width, height, frame, PAL_7COLOUR, 7);
    printf("epd: frame streamed, refreshing...\n");

    epd_command(&cs8, 1, S, 0x04, NULL, 0);   sleep_ms(200);
    epd_command(&cs8, 1, S, 0x12, NULL, 0);   epd_wait_ready(40000);
    epd_command(&cs8, 1, S, 0x02, NULL, 0);   sleep_ms(200);
    printf("epd: refresh done\n");
}

/* ===================== registry ===================== */

static const uint8_t V_EL133[]  = {21, 27};
static const uint8_t V_E673[]   = {22, 26};
static const uint8_t V_E640[]   = {25};
static const uint8_t V_AC073[]  = {20};
static const uint8_t V_UC8159[] = {14, 15, 16};

static const panel_t k_el133  = {"EL133UF1 13.3in Spectra 6", V_EL133, 2, 1200, 1600, 1, paint_el133uf1};
static const panel_t k_e673   = {"E673 7.3in Spectra 6",      V_E673,  2,  800,  480, 0, paint_e673};
static const panel_t k_e640   = {"E640 4.0in Spectra 6",      V_E640,  1,  600,  400, 0, paint_e640};
static const panel_t k_ac073  = {"AC073TC1A 7.3in 7-colour",  V_AC073, 1,  800,  480, 0, paint_ac073};
static const panel_t k_uc8159 = {"UC8159 7-colour",           V_UC8159, 3, 640,  448, 0, paint_uc8159};

static const panel_t *const k_panels[] = {
    &k_el133, &k_e673, &k_e640, &k_ac073, &k_uc8159,
};

const panel_t *panel_for_variant(uint8_t variant)
{
    for (size_t i = 0; i < sizeof(k_panels) / sizeof(k_panels[0]); i++) {
        const panel_t *p = k_panels[i];
        for (uint8_t j = 0; j < p->n_variants; j++) {
            if (p->variants[j] == variant) return p;
        }
    }
    return NULL;
}
