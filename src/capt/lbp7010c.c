/* Unified header field access */
/*
 * Canon LBP7010C CAPT Color Print Filter for CUPS
 *
 * Usage (CUPS filter):
 *   lbp7010c <job> <user> <title> <copies> <options> [file]
 *
 * CUPS raster input (from PPD cupsFilter line):
 *   application/vnd.cups-raster → lbp7010c
 *
 * Color mode: cupsColorSpace=6 (CMYK) — 4 planes, 1bpp each
 * Mono  mode: cupsColorSpace=3 (K)    — 1 plane, 1bpp
 *
 * Protocol (LBP7010C, CAPT 0x0300, CNOptDevType=1):
 *   JCBeginJob → [for each page: JCPageInfo → KCMY bands → JCEndPage2] → JCEndJob
 *
 * JC commands go to ccpd socket (ccpd must be running).
 * IC/LC data goes directly to the USB device output fd provided by CUPS.
 *
 * NB: JCPageInfo / JCBeginJob / JCEndJob are ccpd IPC commands.
 *     For simplicity in this implementation, we only send IC commands
 *     (direct USB) since pi-capt targets raw /dev/usb/lp0 access.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>

#ifdef HAVE_CUPS
#  include <cups/cups.h>
#  include <cups/raster.h>
#else
/* Minimal raster types if no CUPS headers */
typedef unsigned cups_bool_t;
typedef enum {
    CUPS_CSPACE_K   = 3,
    CUPS_CSPACE_CMYK = 6
} cups_cspace_t;
typedef struct {
    unsigned       Width, Height;
    cups_cspace_t  colorspace;
    unsigned       cupsBitsPerColor;
    unsigned       cupsBytesPerLine;
    unsigned       cupsColorOrder;
    unsigned       NumColors;
} cups_page_header2_t;
#endif

#include "capt_color.h"

#ifdef HAVE_CUPS
#  define HDR_W(h)   ((int)(h)->cupsWidth)
#  define HDR_H(h)   ((int)(h)->cupsHeight)
#  define HDR_CS(h)  ((h)->cupsColorSpace)
#  define HDR_BPL(h) ((int)(h)->cupsBytesPerLine)
#else
#  define HDR_W(h)   ((int)(h)->Width)
#  define HDR_H(h)   ((int)(h)->Height)
#  define HDR_CS(h)  ((h)->colorspace)
#  define HDR_BPL(h) ((int)(h)->cupsBytesPerLine)
#endif


/* ── Constants ───────────────────────────────────────────── */
#define MEDIA_PLAIN  0x01
#define MAX_LINE_BUF 4096

/* ── JCBeginJob (IPC to ccpd, opcode 0x0001) ────────────── */
/* We skip JC commands in raw-USB mode (pi-capt style).       */

/* ── CMYK plane extraction ───────────────────────────────── */
/*
 * CUPS provides CMYK raster where each color is packed per pixel.
 * cupsColorOrder=0 (chunked): C0 M0 Y0 K0  C1 M1 Y1 K1 ... (1bpp each)
 * cupsColorOrder=1 (planar): not used here.
 *
 * For 1bpp CMYK chunked, each byte encodes 2 pixels:
 *   bit7=C0, bit6=M0, bit5=Y0, bit4=K0, bit3=C1, ...
 * We need to de-interleave into 4 separate 1bpp plane lines.
 */
static void extract_cmyk_planes(const uint8_t *cmyk_line,
                                  int width_px,
                                  uint8_t *k_out, uint8_t *c_out,
                                  uint8_t *m_out, uint8_t *y_out,
                                  int out_bytes)
{
    memset(k_out, 0, out_bytes);
    memset(c_out, 0, out_bytes);
    memset(m_out, 0, out_bytes);
    memset(y_out, 0, out_bytes);

    for (int px = 0; px < width_px; px++) {
        /* Each pixel = 4 bits in chunked 1bpp CMYK */
        int byte_idx  = px / 2;
        int bit_shift = (px % 2 == 0) ? 4 : 0;   /* high nibble = even pixel */
        uint8_t nibble = (cmyk_line[byte_idx] >> bit_shift) & 0x0F;

        /* nibble: bit3=C bit2=M bit1=Y bit0=K (CMYK order) */
        int c_bit = (nibble >> 3) & 1;
        int m_bit = (nibble >> 2) & 1;
        int y_bit = (nibble >> 1) & 1;
        int k_bit = (nibble >> 0) & 1;

        int out_byte = px / 8;
        int out_bit  = 7 - (px % 8);   /* MSB first */

        if (k_bit) k_out[out_byte] |= (1 << out_bit);
        if (c_bit) c_out[out_byte] |= (1 << out_bit);
        if (m_bit) m_out[out_byte] |= (1 << out_bit);
        if (y_bit) y_out[out_byte] |= (1 << out_bit);
    }
}

/* ── Print one page ─────────────────────────────────────── */
static int print_page(int out_fd,
                       cups_page_header2_t *hdr,
                       int page_num,
                       FILE *raster_fp)
{
    int width_px    = HDR_W(hdr);
    int height_lines= HDR_H(hdr);
    int is_color    = (HDR_CS(hdr) == CUPS_CSPACE_CMYK);
    int in_bytes    = (int)hdr->cupsBytesPerLine;
    int out_bytes   = COLOR_LINE_SIZE;   /* 592 bytes */
    int band_height = COLOR_ROWS_PER_BAND;

    uint8_t *in_buf  = malloc(in_bytes + 4);
    uint8_t *k_buf   = calloc(1, out_bytes);
    uint8_t *c_buf   = calloc(1, out_bytes);
    uint8_t *m_buf   = calloc(1, out_bytes);
    uint8_t *y_buf   = calloc(1, out_bytes);
    if (!in_buf || !k_buf || !c_buf || !m_buf || !y_buf) {
        fprintf(stderr, "ERROR: malloc failed\n");
        goto fail;
    }

    /* Create per-plane band compressors */
    capt_band_buf_t *bk = capt_band_new();
    capt_band_buf_t *bc = capt_band_new();
    capt_band_buf_t *bm = capt_band_new();
    capt_band_buf_t *by = capt_band_new();
    if (!bk || !bc || !bm || !by) {
        fprintf(stderr, "ERROR: capt_band_new failed\n");
        goto fail_bands;
    }

    /* ICBeginPage */
    if (capt_color_begin_page(out_fd, width_px, height_lines,
                               band_height, is_color, MEDIA_PLAIN) < 0) {
        fprintf(stderr, "ERROR: ICBeginPage failed\n");
        goto fail_bands;
    }

    /* ICxxxPlane headers: K → C → M → Y */
    capt_color_send_plane_header(out_fd, IC_PLANE_K);
    if (is_color) {
        capt_color_send_plane_header(out_fd, IC_PLANE_C);
        capt_color_send_plane_header(out_fd, IC_PLANE_M);
        capt_color_send_plane_header(out_fd, IC_PLANE_Y);
    }

    /* ICBeginData */
    capt_color_begin_data(out_fd);

    int total_bands = 0;

    /* Process raster data line by line */
    for (int line = 0; line < height_lines; line++) {
        int band_line = line % band_height;
        int is_last   = (band_line == band_height - 1)
                      || (line == height_lines - 1);

        /* Read one raster line from CUPS */
        size_t n = fread(in_buf, 1, in_bytes, raster_fp);
        if (n < (size_t)in_bytes)
            memset(in_buf + n, 0, in_bytes - n);

        if (is_color) {
            /* De-interleave CMYK → 4 separate 1bpp planes */
            extract_cmyk_planes(in_buf, width_px,
                                  k_buf, c_buf, m_buf, y_buf, out_bytes);
        } else {
            /* Mono: just use K plane */
            memcpy(k_buf, in_buf, (in_bytes < out_bytes) ? in_bytes : out_bytes);
            memset(c_buf, 0, out_bytes);
            memset(m_buf, 0, out_bytes);
            memset(y_buf, 0, out_bytes);
        }

        /* Compress each plane */
        /* is_final_page: 最終ページ行=最終バンド → 0x42付与 */
        int is_final = (line == height_lines - 1);
        capt_band_compress_line(bk, k_buf, in_bytes, is_last, is_final);
        if (is_color) {
            capt_band_compress_line(bc, c_buf, in_bytes, is_last, is_final);
            capt_band_compress_line(bm, m_buf, in_bytes, is_last, is_final);
            capt_band_compress_line(by, y_buf, in_bytes, is_last, is_final);
        }

        /* On band boundary: flush all planes as ICVideoData */
        if (is_last) {
            /* Send K band first, then C, M, Y */
            capt_color_send_band(out_fd, capt_band_data(bk),
                                  capt_band_size(bk));
            if (is_color) {
                capt_color_send_band(out_fd, capt_band_data(bc),
                                      capt_band_size(bc));
                capt_color_send_band(out_fd, capt_band_data(bm),
                                      capt_band_size(bm));
                capt_color_send_band(out_fd, capt_band_data(by),
                                      capt_band_size(by));
            }

            /* Reset for next band */
            capt_band_reset(bk);
            if (is_color) {
                capt_band_reset(bc);
                capt_band_reset(bm);
                capt_band_reset(by);
            }
            total_bands++;
        }
    }

    /* ICEndPage sequence */
    capt_color_end_page(out_fd, total_bands, is_color);

    fprintf(stderr, "INFO: Page %d printed (%d bands, %s)\n",
            page_num, total_bands, is_color ? "color" : "mono");

    capt_band_free(bk); capt_band_free(bc);
    capt_band_free(bm); capt_band_free(by);
    free(in_buf); free(k_buf); free(c_buf); free(m_buf); free(y_buf);
    return 0;

fail_bands:
    capt_band_free(bk); capt_band_free(bc);
    capt_band_free(bm); capt_band_free(by);
fail:
    free(in_buf); free(k_buf); free(c_buf); free(m_buf); free(y_buf);
    return -1;
}

/* ── Main ────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr,
            "Usage: %s job-id user title copies options [file]\n", argv[0]);
        return 1;
    }

    /* Open input raster file or stdin */
    FILE *raster_fp = stdin;
    if (argc == 7) {
        raster_fp = fopen(argv[6], "rb");
        if (!raster_fp) {
            fprintf(stderr, "ERROR: Cannot open %s: %s\n",
                    argv[6], strerror(errno));
            return 1;
        }
    }

    /* CUPS sends printer output to stdout (fd=1) */
    int out_fd = STDOUT_FILENO;

    /*
     * Parse CUPS raster.
     * Without libcups: read PBM P4 format (grayscale mono).
     * With libcups: use cupsRasterRead/cupsRasterReadHeader2.
     *
     * For production: link with -lcupsimage and use cups/raster.h.
     * For testing: accept raw PBM P4 binary bitmap.
     */

#ifdef HAVE_CUPS
    cups_raster_t *ras = cupsRasterOpen(fileno(raster_fp), CUPS_RASTER_READ);
    if (!ras) {
        fprintf(stderr, "ERROR: cupsRasterOpen failed\n");
        return 1;
    }
    cups_page_header2_t hdr;
    int page = 0;
    while (cupsRasterReadHeader2(ras, &hdr)) {
        page++;
        fprintf(stderr, "INFO: Page %d: %dx%d %s\n",
                page, HDR_W(&hdr), HDR_H(&hdr),
                (HDR_CS(&hdr) == CUPS_CSPACE_CMYK) ? "CMYK" : "Mono");
        if (print_page(out_fd, &hdr, page, raster_fp) < 0) {
            fprintf(stderr, "ERROR: print_page failed\n");
            cupsRasterClose(ras);
            return 1;
        }
    }
    cupsRasterClose(ras);
#else
    /*
     * Fallback: accept PBM P4 (monochrome bitmap) for testing.
     * One page per file.
     */
    char line_buf[256];
    if (!fgets(line_buf, sizeof(line_buf), raster_fp)
        || strncmp(line_buf, "P4", 2) != 0) {
        fprintf(stderr, "ERROR: Expected PBM P4 format\n");
        return 1;
    }
    /* Skip comments */
    do { fgets(line_buf, sizeof(line_buf), raster_fp); }
    while (line_buf[0] == '#');
    int bm_w = 0, bm_h = 0;
    sscanf(line_buf, "%d %d", &bm_w, &bm_h);

    cups_page_header2_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.Width = bm_w;
    hdr.Height = bm_h;
    HDR_CS(&hdr)      = CUPS_CSPACE_K;
    hdr.cupsBitsPerColor= 1;
    hdr.cupsBytesPerLine = (bm_w + 7) / 8;

    if (print_page(out_fd, &hdr, 1, raster_fp) < 0) {
        fprintf(stderr, "ERROR: print_page failed\n");
        return 1;
    }
#endif

    if (raster_fp != stdin) fclose(raster_fp);
    return 0;
}
