/*
 *  Linux Canon CAPT driver, header file
 *  Copyright (C) 2004 Nicolas Boichat <nicolas@boichat.ch>
 * 
 *  Adapted from a printer driver for Samsung ML-85G laser printer
 *  (C) Copyleft, 2000 Rildo Pragana <rpragana@acm.org>
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

#define LINE_SIZE       592
#define LINES_BY_PAGE   6968
#define ROWS_BY_BAND    104

#define MAX_PACKET_COUNT 3072
#define PAGE_DELAY       3000000  /* usec between pages */

#define CUPS_ERROR(fmt, ...)   fprintf(stderr, "ERROR: "   fmt "\n", ##__VA_ARGS__)
#define CUPS_WARN(fmt, ...)    fprintf(stderr, "WARNING: " fmt "\n", ##__VA_ARGS__)
#define CUPS_INFO(fmt, ...)    fprintf(stderr, "INFO: "    fmt "\n", ##__VA_ARGS__)
#define CUPS_DEBUG(fmt, ...)   fprintf(stderr, "DEBUG: "   fmt "\n", ##__VA_ARGS__)

#ifdef DEBUG
#  define INLINE
#  define DPRINTF(fmt, ...)    fprintf(stderr, "DEBUG: " fmt "\n", ##__VA_ARGS__)
#else
#  define INLINE   inline
#  define DPRINTF(fmt, ...)    do {} while(0)
#endif

/* end of file */
