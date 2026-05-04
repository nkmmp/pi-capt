/*
 *  Linux Canon CAPT driver
 *  Copyright (C) 2004 Nicolas Boichat <nicolas@boichat.ch>
 *  Some functions adapted from Samsung ML-85G driver by Rildo Pragana
 *
 *  2026: CUPS filter interface, error messages, device selection — pi-capt

 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>

#include "capt.h"

#define WAIT 8000

int fd = -1;

struct timeval lasttv, newtv;

unsigned char cmdbuffer[8][256];

FILE *bitmapf = NULL;
int   cbmbuf  = 0;
unsigned char *bmptr[2];
unsigned char *bmbuf[2] = {NULL, NULL};
int bmwidth = 0, bmheight = 0;

unsigned char  band[65536];
unsigned char *bandptr = band;
int bsize = 0;

int  ccbm = 0;
unsigned char *cbm[100];

unsigned char  garbage[600];
unsigned char *cbmp;
int csize   = 0;
int linecnt = 0;
int pktcnt;
int topskip  = 0;
int leftskip = 0;

void errorexit(void);

void bitmap_seek(int offset)
{
    while (offset > (int)sizeof(garbage)) {
        if (fread(bmbuf[cbmbuf], 1, sizeof(garbage), bitmapf) < 0) break;
        offset -= (int)sizeof(garbage);
    }
    if (offset > 0)
        (void)fread(bmbuf[cbmbuf], 1, offset, bitmapf);
}

void next_page(int page)
{
    (void)page;
    int skip = (bmheight - topskip - linecnt) * bmwidth;
    CUPS_DEBUG("bmheight=%d bmwidth=%d topskip=%d linecnt=%d skip=%d",
               bmheight, bmwidth, topskip, linecnt, skip);
    if (skip > 0)
        bitmap_seek(skip);
    linecnt = 0;
}

unsigned char *get_line(void)
{
    cbmbuf = (cbmbuf == 1) ? 0 : 1;
    memset(bmbuf[cbmbuf], 0, 800);

    if (linecnt < (bmheight - topskip)) {
        if (bmwidth > 800) {
            (void)fread(bmbuf[cbmbuf], 1, 800, bitmapf);
            bitmap_seek(bmwidth - 800);
        } else {
            (void)fread(bmbuf[cbmbuf], 1, bmwidth, bitmapf);
        }
    }
    bmptr[cbmbuf] = bmbuf[cbmbuf] + (leftskip / 8);
    linecnt++;
    return bmptr[cbmbuf];
}

int last_difference(void)
{
    int diff = -1, i;
    unsigned char *p1 = bmptr[cbmbuf];
    unsigned char *p2 = bmptr[(cbmbuf == 1) ? 0 : 1];
    for (i = 0; i < LINE_SIZE; i++, p1++, p2++)
        if (*p1 != *p2) diff = i;
    return diff;
}

void out_packet_buf(int cnt, unsigned char *c)
{
    int i;
    for (i = 0; i < cnt; i++) *(bandptr++) = c[i];
    bsize += cnt;
    if (bsize > 65535) {
        CUPS_ERROR("Band overflow (bsize=%d)", bsize);
        errorexit();
    }
}

int out_packet(int cnt, unsigned char c1, unsigned char c2,
               unsigned char c3, unsigned char c4)
{
    if (cnt == 0) {
        if (c1 == 1) {  /* band flush */
            if (csize + bsize > 65500)
                out_packet(0, 0, 0, 0, 0);
            csize += bsize;
            CUPS_DEBUG("Band flushed: %d (%d)", bsize, csize);
            bandptr = band;
            while (bsize > 0) { *(cbmp++) = *(bandptr++); bsize--; }
            bandptr = band;
            bsize   = 0;
        } else if (c1 == 0) {  /* packet flush */
            if (c2 == 1) *(cbmp++) = 0x42;  /* final band marker */
            cbm[ccbm][2] = (unsigned char)(csize & 0xFF);
            cbm[ccbm][3] = (unsigned char)((csize >> 8) & 0xFF);
            CUPS_DEBUG("Packet %d flushed: %d bytes (final=%d)", ccbm, csize, c2);
            ccbm++;
            if (ccbm >= 99) {
                CUPS_ERROR("Too many packets (%d)", ccbm);
                errorexit();
            }
            if (c2 != 1) {
                cbm[ccbm] = malloc(65536);
                cbm[ccbm+1] = NULL;
                if (!cbm[ccbm]) {
                    CUPS_ERROR("malloc failed for packet %d", ccbm);
                    errorexit();
                }
                cbmp = cbm[ccbm];
                csize = 4;
                *(cbmp++) = 0xa0; *(cbmp++) = 0xc0;
                *(cbmp++) = 0x00; *(cbmp++) = 0x00;
            }
        }
        return 1;
    }

    switch (cnt) {
    case 4: *(bandptr++) = c1; *(bandptr++) = c2;
            *(bandptr++) = c3; *(bandptr++) = c4; break;
    case 3: *(bandptr++) = c1; *(bandptr++) = c2; *(bandptr++) = c3; break;
    case 2: *(bandptr++) = c1; *(bandptr++) = c2; break;
    case 1: *(bandptr++) = c1; break;
    default:
        CUPS_ERROR("Wrong packet size (%d)", cnt);
        errorexit();
    }
    bsize += cnt;
    DPRINTF("->%d %x %x %x %x", cnt, c1, c2, c3, c4);
    if (bsize > 65535) {
        CUPS_ERROR("Band overflow (bsize=%d)", bsize);
        errorexit();
    }
    return 0;
}

int compress_bitmap(void)
{
    int  band_idx, diff;
    int  state, rep, lpos, cnt;
    unsigned char c1, c2, *c;
    unsigned char *cline;
    char hdr[200];

    ccbm = 0;
    cbm[0] = malloc(65536);
    cbm[1] = NULL;
    cbmp = cbm[0];
    if (!cbm[0]) {
        CUPS_ERROR("malloc failed for packet 0");
        errorexit();
    }

    if (!fgets(hdr, sizeof(hdr), bitmapf)) return 0;
    if (strncmp(hdr, "P4", 2)) {
        CUPS_ERROR("Wrong file format (expected P4, got: %s)", hdr);
        errorexit();
    }
    if (!bmbuf[0]) { bmbuf[0] = malloc(1024); bmbuf[1] = malloc(1024); }

    do { (void)fgets(hdr, sizeof(hdr), bitmapf); } while (hdr[0] == '#');

    if (sscanf(hdr, "%d %d", &bmwidth, &bmheight) < 2) {
        CUPS_ERROR("Bitmap size fields invalid");
        errorexit();
    }
    bmwidth = (bmwidth + 7) / 8;
    if (topskip) bitmap_seek(bmwidth * topskip);

    cline = get_line();
    diff  = LINE_SIZE;

    csize = 4;
    *(cbmp++) = 0xa0; *(cbmp++) = 0xc0;
    *(cbmp++) = 0x00; *(cbmp++) = 0x00;

    for (band_idx = 0; linecnt < LINES_BY_PAGE; band_idx++) {
        out_packet(1, 0x40, 0, 0, 0);
        pktcnt = 0;
        cnt = ((LINES_BY_PAGE - linecnt) > ROWS_BY_BAND)
              ? ROWS_BY_BAND : (LINES_BY_PAGE - linecnt);

        CUPS_DEBUG("band=%d cnt=%d linecnt=%d diff=%d",
                   band_idx, cnt, linecnt, diff);

        while (1) {
            rep = 0; state = 0; lpos = 0;

            for (lpos = 0; lpos < diff; lpos++) {
                if ((cline[lpos] == cline[lpos+1]) && ((lpos+1) < diff)) {
                    if (state == -1) {
                        if (rep == 255) {
                            c1 = 0xa0 | (rep >> 3);
                            c2 = 0x80 | (rep & 0x07) << 3;
                            out_packet(3, c1, c2, cline[lpos], 0);
                            rep = 0; state = 0;
                        } else { rep++; }
                    } else { state = -1; rep = 2; }
                } else {
                    if (state == 0) {
                        rep = 1;
                        c = &cline[lpos];
                        while (rep < 255) {
                            if (lpos+rep+2 >= diff) {
                                rep += diff - (lpos+rep); break;
                            } else if ((c[rep] != c[rep+1]) || (c[rep+1] != c[rep+2])) {
                                rep++;
                            } else { break; }
                        }
                        if (rep > 255) rep = 255;
                        lpos += rep - 1;
                        if (rep < 8) {
                            out_packet(1, (unsigned char)(rep << 3), 0, 0, 0);
                        } else {
                            c1 = 0xa0 | (rep >> 3);
                            c2 = 0xc0 | ((rep & 0x07) << 3);
                            out_packet(2, c1, c2, 0, 0);
                        }
                        out_packet_buf(rep, c);
                        rep = 0;
                    } else { /* state == -1 */
                        if (rep < 8) {
                            if (((cline[lpos+1] != cline[lpos+2]) || ((lpos+2) >= diff))
                                && ((lpos+1) < diff)) {
                                c1 = 0xc1 | (rep << 3);
                                out_packet(3, c1, cline[lpos], cline[lpos+1], 0);
                                lpos++;
                            } else {
                                c1 = 0x40 | (rep << 3);
                                out_packet(2, c1, cline[lpos], 0, 0);
                            }
                        } else {
                            if (((cline[lpos+1] != cline[lpos+2]) || ((lpos+2) >= diff))
                                && ((lpos+1) < diff)) {
                                if (((cline[lpos+2] != cline[lpos+3]) || ((lpos+3) >= diff))
                                    && ((lpos+2) < diff)) {
                                    c1 = 0xa0 | (rep >> 3);
                                    c2 = 0x02 | ((rep & 0x07) << 3);
                                    out_packet(3, c1, c2, cline[lpos], 0);
                                    out_packet(2, cline[lpos+1], cline[lpos+2], 0, 0);
                                    lpos += 2;
                                } else {
                                    c1 = 0xa0 | (rep >> 3);
                                    c2 = 0x01 | ((rep & 0x07) << 3);
                                    out_packet(4, c1, c2, cline[lpos], cline[lpos+1]);
                                    lpos++;
                                }
                            } else {
                                c1 = 0xa0 | (rep >> 3);
                                c2 = 0x80 | (rep & 0x07) << 3;
                                out_packet(3, c1, c2, cline[lpos], 0);
                            }
                        }
                        state = 0;
                    }
                }
            } /* for lpos */

            if (diff < LINE_SIZE)
                out_packet(1, 0x41, 0, 0, 0);

            cline = get_line();
            if (cnt > 0) {
                diff = last_difference() + 1;
            } else {
                diff = LINE_SIZE;
                break;
            }
            cnt--;
        } /* while */

        out_packet(1, 0x40, 0, 0, 0);
        out_packet(0, 1, 0, 0, 0);
    } /* for band_idx */

    out_packet(0, 0, 1, 0, 0);
    return 1;
}

INLINE void errorexit(void)
{
#ifdef DEBUG
    int *p = NULL; (*p)++;  
#endif
    exit(1);
}

INLINE void ssleep(const int usec)
{
    gettimeofday(&lasttv, NULL);
    while (1) {
        gettimeofday(&newtv, NULL);
        long elapsed = (newtv.tv_usec - lasttv.tv_usec)
                     + (newtv.tv_sec  - lasttv.tv_sec) * 1000000L;
        if (elapsed > usec) break;
    }
}

void write_command_packet_buf(unsigned char one, unsigned char two,
                               int uwait, int nread,
                               unsigned char *buf, int len)
{
    unsigned char buffer[256];
    int n, i, j;

    buffer[0] = one; buffer[1] = two;
    buffer[2] = (unsigned char)(0x04 + len); buffer[3] = 0x00;
    for (i = 0; i < len; i++) buffer[i+4] = buf[i];

    n = write(fd, buffer, 4 + len);
    if (n < 0) {
        CUPS_ERROR("USB write failed: %s", strerror(errno));
        errorexit();
    }
    CUPS_DEBUG("write %d bytes: %02x %02x ...", 4+len, one, two);

    ssleep(WAIT);

    for (j = 0; j < nread; j++) {
        memset(&cmdbuffer[j], 0, 256);
        n = read(fd, &cmdbuffer[j], 256);
        if (n < 0 && errno != EAGAIN) {
            CUPS_WARN("USB read failed: %s", strerror(errno));
        }
        CUPS_DEBUG("read %d bytes", n);
        ssleep(WAIT);
    }
    if (uwait > 0) usleep(uwait);
}

INLINE void write_command_packet(unsigned char one, unsigned char two,
                                  int uwait, int nread)
{
    write_command_packet_buf(one, two, uwait, nread, NULL, 0);
}

int waitforpaper(void)
{
    int i;
    for (i = 0; i < 120; i++) {
        write_command_packet(0xa0, 0xa0, 0, 2);
        write_command_packet(0xa1, 0xa0, 0, 2);
        if (cmdbuffer[1][10] == 0xfd) return 1;
        if (i == 0) CUPS_INFO("Waiting for paper...");
        usleep(1000000);
    }
    CUPS_ERROR("Timeout waiting for paper");
    return 0;
}

int waitforready(void) { return 1; }

int print_page(int page)
{
    unsigned int size;
    int i, w, c;

    if (page == 0) {
        write_command_packet(0xa1, 0xa1, 0, 2);
        if ((cmdbuffer[0][0] != 0xa1) || (cmdbuffer[0][1] != 0xa1)) {
            CUPS_ERROR("Invalid printer state — printer not connected?");
            return 0;
        }
    }

    if (!waitforpaper()) return 0;

    if (page == 0) {
        write_command_packet(0xa0, 0xa2, 0, 1);
        write_command_packet(0xa0, 0xe0, 0, 1);
        write_command_packet(0xa1, 0xa0, 0, 2);
        write_command_packet(0xa4, 0xe0, 0, 1);
        {
            unsigned char buf[] = {0xee, 0xdb, 0xea, 0xad,
                                   0x00, 0x00, 0x00, 0x00};
            write_command_packet_buf(0xa5, 0xe0, 0, 1, buf, 8);
        }
        write_command_packet(0xa0, 0xe0, 0, 1);
        write_command_packet(0xa0, 0xa0, 0, 2);
        write_command_packet(0xa1, 0xa0, 0, 2);
        write_command_packet(0xa0, 0xe0, 0, 1);
    }

    {
        unsigned char buf[] = {
            0x00, 0x00, 0xa4, 0x01, 0x02, 0x01, 0x00, 0x00,
            0x1f, 0x1f, 0x1f, 0x1f, 0x00, 0x11, 0x03, 0x01,
            0x01, 0x01, 0x02, 0x00, 0x00, 0x00, 0x70, 0x00,
            0x78, 0x00, 0x50, 0x02, 0x7a, 0x1a, 0x60, 0x13,
            0x67, 0x1b};
        write_command_packet_buf(0xa0, 0xd0, 0, 0, buf, 34);
    }

    write_command_packet(0xa0, 0xe0, 0, 1);
    write_command_packet(0xa1, 0xd0, 0, 0);
    write_command_packet(0xa0, 0xe0, 0, 1);
    write_command_packet(0xa0, 0xa0, 0, 2);
    write_command_packet(0xa1, 0xa0, 0, 2);

    for (ccbm = 0; cbm[ccbm] != NULL; ccbm++) {
        write_command_packet(0xa0, 0xe0, 0, 1);
        while (((cmdbuffer[0][4] & 0x08) == 0x08) || (cmdbuffer[0][0] == 0x00)) {
            ssleep(100000);
            write_command_packet(0xa0, 0xe0, 0, 1);
        }
        ssleep(2 * WAIT);

        size = ((unsigned int)cbm[ccbm][3] << 8) | cbm[ccbm][2];
        CUPS_DEBUG("Sending packet %d: %u bytes", ccbm, size);

        for (i = 0; (i + 4096) < (int)size; i += 4096) {
            c = 0;
            do {
                w = write(fd, &cbm[ccbm][i], 4096);
                if (w < 0) {
                    CUPS_WARN("USB write retry (i=%d/%u): %s", i, size, strerror(errno));
                }
                ssleep(2 * WAIT);
                if (++c > 100) {
                    CUPS_ERROR("USB write timeout at offset %d", i);
                    return 0;
                }
            } while (w < 0);
        }

        c = 0;
        do {
            w = write(fd, &cbm[ccbm][i], size - i);
            if (w < 0) CUPS_WARN("USB write retry (tail): %s", strerror(errno));
            ssleep(2 * WAIT);
            if (++c > 100) {
                CUPS_ERROR("USB write timeout (tail)");
                return 0;
            }
        } while (w < 0);

        free(cbm[ccbm]);
        cbm[ccbm] = NULL;
        ssleep(5 * WAIT);
    }

    write_command_packet(0xa0, 0xe0, 0, 1);
    write_command_packet(0xa2, 0xd0, 0, 0);
    write_command_packet(0xa0, 0xe0, 0, 1);
    write_command_packet(0xa1, 0xa0, 0, 2);

    return waitforready();
}

int main(int argc, char **argv)
{
    int  simulate = 0;
    int  page     = 0;
    int  c;
    const char *device = NULL;  

    device = getenv("DEVICE_URI");
    if (device && strncmp(device, "usb://", 6) == 0) {
        device = NULL;
    }

    bitmapf = stdin;
    while ((c = getopt(argc, argv, "t:l:sd:f:")) != -1) {
        switch (c) {
        case 't':
            sscanf(optarg, "%d", &topskip);
            break;
        case 'l':
            sscanf(optarg, "%d", &leftskip);
            break;
        case 's':
            simulate = 1;
            break;
        case 'd':

            device = optarg;
            break;
        case 'f':
            bitmapf = fopen(optarg, "r");
            if (!bitmapf) {
                CUPS_ERROR("Cannot open input file '%s': %s",
                           optarg, strerror(errno));
                errorexit();
            }
            break;  
        default:
            fprintf(stderr,
                "Usage: %s [-t topskip] [-l leftskip] [-s] "
                "[-d device] [-f file]\n", argv[0]);
            return 1;
        }
    }

    if (!simulate) {
        const char *dev = device ? device : "/dev/usb/lp0";
        fd = open(dev, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            CUPS_ERROR("Cannot open device '%s': %s", dev, strerror(errno));
            return 1;
        }
        CUPS_INFO("Opened device %s", dev);
    }

    while (1) {
        if (!compress_bitmap()) break;
        CUPS_INFO("Printing page %d", page + 1);
        if (!simulate) {
            if (!print_page(page)) {
                CUPS_ERROR("Failed to print page %d", page + 1);
                errorexit();
            }
        }
        next_page(page++);
    }

    if (bitmapf && bitmapf != stdin) fclose(bitmapf);
    if (fd >= 0) close(fd);

    CUPS_INFO("Done — %d page(s) printed", page);
    return 0;
}
