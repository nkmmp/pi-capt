/*
 * Canon LBP7010C Color CAPT Protocol
 *
 * CAPT command packet format: [opcode:2B LE][length:2B LE][payload]
 * length field INCLUDES the 4-byte header.
 *
 * For LBP7010C: CNOptDevType=1 (<=10) → ICBeginPage NON-_30 variant
 *   Total packet size 44B (0x2C) = 4B header + 40B payload
 *
 * Plane ordering (start):  K(0xD0A4) → C(0xD0A7) → M(0xD0A6) → Y(0xD0A5)
 * Plane ordering (end):    C(0xC0A7) → M(0xC0A6) → Y(0xC0A5) → K(0xC0A4)
 */

#ifndef CAPT_COLOR_H
#define CAPT_COLOR_H

#include <stdint.h>
#include <stddef.h>

/* ── IC系コマンドオペコード (カラー印刷、直接USB送信) ──────────── */
#define IC_BEGIN_PAGE    0xD0A0  /* ICBeginPage: ペイロード40B (optDevType≤10) */
#define IC_BEGIN_DATA    0xD0A1  /* ICBeginData: 4B total (ペイロードなし)    */
#define IC_END_PAGE      0xD0A2  /* ICEndPage:   4B total                     */
#define IC_PLANE_K       0xD0A4  /* ICxxxPlane K(黒): ペイロード8B            */
#define IC_PLANE_Y       0xD0A5  /* ICxxxPlane Y(黄): ペイロード8B            */
#define IC_PLANE_M       0xD0A6  /* ICxxxPlane M(マゼンタ): ペイロード8B      */
#define IC_PLANE_C       0xD0A7  /* ICxxxPlane C(シアン): ペイロード8B        */
#define IC_VIDEO_COUNT   0xD0A8  /* ICVideoCount: ペイロード16B               */
#define IC_VIDEO_DATA    0xC0A0  /* ICVideoData: 可変長 (Hi-SCoA圧縮データ)   */
#define IC_END_K         0xC0A4  /* ICBlackEnd: 4B total                      */
#define IC_END_Y         0xC0A5  /* ICxxxEnd Y: 4B total                      */
#define IC_END_M         0xC0A6  /* ICxxxEnd M: 4B total                      */
#define IC_END_C         0xC0A7  /* ICxxxEnd C: 4B total                      */

/* ── LCコマンド (モノクロ) ──────────────────────────────────────── */
#define LC_BLACK_DATA    0x8000  /* LCBlackData: モノクロバンドデータ         */

/* ── LBP7010C / A4 / 600dpi 固定値 (pi-capt参照) ─ */
#define COLOR_LINE_SIZE      592   /* 印刷幅バイト数 (592*8=4736px ≈ 200mm)  */
#define COLOR_ROWS_PER_BAND   70   /* バンド高さ (CAPT 0x0300クラス)          */
#define COLOR_PRINT_HEIGHT  6778   /* A4印刷高さライン数 (~287mm @ 600dpi)    */
#define COLOR_TOTAL_HEIGHT  7015
#define COLOR_PRINT_WIDTH_PX 4736  /* 印刷幅ピクセル (592*8)                  */
#define COLOR_PAGE_WIDTH_PX  4960

/* ── ICBeginPage 40バイトペイロード構造 (pi-capt LBP810参照から推定) */
/* Ref: pi-capt/src/capt/capt.c print_page() 34バイトバッファ       */
/* LBP7010C は 6バイト追加 (total 40B)                               */
typedef struct __attribute__((packed)) {
    uint16_t job_id;           /* 0x00: 0x0000 (ジョブID or padding)         */
    uint16_t unk_01a4;         /* 0x02: 0x01A4 = 420 (不明)                   */
    uint8_t  color_flags;      /* 0x04: 0x02 (カラーフラグ)                   */
    uint8_t  media_type;       /* 0x05: 0x01 (PlainPaper)                    */
    uint8_t  pad0[2];          /* 0x06: 0x00 0x00                             */
    uint8_t  margin_top;       /* 0x08: 0x1f = 31 (上マージン px @ 600dpi)   */
    uint8_t  margin_bottom;    /* 0x09: 0x1f = 31 (下マージン px)            */
    uint8_t  margin_left;      /* 0x0A: 0x1f = 31 (左マージン px)            */
    uint8_t  margin_right;     /* 0x0B: 0x1f = 31 (右マージン px)            */
    uint8_t  opt[10];          /* 0x0C-0x15: 各種オプション (0x00 0x11 …)    */
    uint16_t band_h_hi;        /* 0x16: 0x0070 = 112 (バンド高さ関連?)        */
    uint16_t band_h_lo;        /* 0x18: 0x0078 = 120                          */
    uint16_t line_bytes;       /* 0x1A: LINE_SIZE (印刷幅バイト)              */
    uint16_t print_height;     /* 0x1C: 印刷高さ (ライン数)                   */
    uint16_t print_width_px;   /* 0x1E: 印刷幅 (ピクセル)                    */
    uint16_t total_height;     /* 0x20: 総ページ高さ (ピクセル)               */
    uint16_t unk_22;           /* 0x22: 0x???                                 */
    uint16_t unk_24;           /* 0x24: 0x???                                 */
    uint8_t  color_ext[3];     /* 0x26: カラー拡張 (LBP7010C追加6B中の3B)     */
    uint8_t  pad1;             /* 0x27: 0x00                                  */
    uint8_t  color_ext2[2];    /* 0x28: カラー拡張続き                        */
    uint8_t  pad2;             /* 0x2A: 0x00 (const)             */
    uint8_t  pad3;             /* 0x2B: 0x00 (const)                          */
} capt_ic_begin_page_t;        /* 計 44B = header(4) + payload(40)            */

/* ── ICxxxPlane 8バイトペイロード ────────────────────────────────── */
/* SetOptDevType1 より: bytes[0-3]=ctx->0x1e8, bytes[4-7]=ctx->0x1ec */
/* 初期実装ではゼロ (デジタルレジスト無効)        */
typedef struct __attribute__((packed)) {
    uint8_t  params[6];        /* 0x00-0x05: プレーン設定パラメータ           */
    uint16_t stride;           /* 0x06-0x07: ストライド (LINE_SIZE?)          */
} capt_ic_plane_t;             /* 計 12B = header(4) + payload(8)             */

/* ── ICVideoCount 16バイトペイロード ─────────────────────────────── */
/* SetVideoCount より: 各プレーンの圧縮バイト数 / divisor(=4 for 1bpp) */
typedef struct __attribute__((packed)) {
    uint32_t k_count;          /* Kプレーン バンドカウント / 4               */
    uint32_t c_count;          /* Cプレーン バンドカウント / 4               */
    uint32_t m_count;          /* Mプレーン バンドカウント / 4               */
    uint32_t y_count;          /* Yプレーン バンドカウント / 4               */
} capt_ic_video_count_t;       /* 計 20B = header(4) + payload(16)            */

/* ── 関数プロトタイプ ─────────────────────────────────────────────── */

/* 低レベル: CAPTコマンド送信 */
int capt_send_cmd(int fd, uint16_t opcode, const void *payload, uint16_t payload_len);

/* Hi-SCoAライン圧縮 (capt.cのcompress_bitmap相当、1プレーン分) */
typedef struct capt_band_buf capt_band_buf_t;
capt_band_buf_t *capt_band_new(void);
void             capt_band_free(capt_band_buf_t *b);
void             capt_band_reset(capt_band_buf_t *b);
int              capt_band_compress_line(capt_band_buf_t *b,
                     const uint8_t *line, int line_bytes,
                     int is_last_in_band, int is_final_page);
const uint8_t   *capt_band_data(const capt_band_buf_t *b);
int              capt_band_size(const capt_band_buf_t *b);

/* カラーページ送信 */
int capt_color_begin_page(int fd, int width_px, int height_lines,
                           int band_height, int is_color,
                           uint8_t media_type);
int capt_color_send_plane_header(int fd, uint16_t plane_opcode);
int capt_color_send_band(int fd, const uint8_t *data, int size);
int capt_color_end_page(int fd, int total_bands, int is_color);

#endif /* CAPT_COLOR_H */

/* ICBeginData (0xD0A1) — 4B total */
int capt_color_begin_data(int fd);
