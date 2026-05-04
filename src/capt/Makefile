# Makefile of printer driver
# March 2021 -- Andrey Khrolenok
# Belfaux, July-August 2004 -- Nicolas Boichat
# Adapted from Samsung ML-85P driver by Rildo Pragana
#
# LBP7010C color extension
#   Adds lbp7010c target only; existing capt / capt-print targets unchanged.

CC      = gcc
CFLAGS  = -O2 -g
RELEASE = 0.2

# ── CUPS detection (lbp7010c のみ使用, capt には影響しない) ──────
CUPS_CFLAGS := $(shell cups-config --cflags 2>/dev/null)
CUPS_LIBS   := $(shell cups-config --libs   2>/dev/null)
ifneq ($(CUPS_CFLAGS),)
  LBP7010C_CFLAGS  = $(CFLAGS) $(CUPS_CFLAGS) -DHAVE_CUPS
  LBP7010C_LDFLAGS = $(CUPS_LIBS)
else
  LBP7010C_CFLAGS  = $(CFLAGS)
  LBP7010C_LDFLAGS =
endif

# ── 既存ターゲット (変更なし) ──────────────────────────────────────
all: capt lbp7010c

capt: capt.c capt.h Makefile
	${CC} ${INCLUDE} ${CFLAGS} capt.c -o capt

install: all
	install -s -m a=rx capt          /usr/bin
	install    -m a=rx capt-print    /usr/bin
	install    -m a=r  ppd/*         /usr/share/cups/model
	install    -m 755  lbp7010c      /usr/lib/cups/filter/
	install    -m 644  ppd/CNCUPSLBP7010CCAPTJ.ppd /usr/share/ppd/

clean:
	rm -f capt lbp7010c capt_color.o lbp7010c.o

package: clean
	rm -f *~ ppd/*~
	cp -a ../code ../capt-${RELEASE}
	tar cvfz ../capt-${RELEASE}.tar.gz ../capt-${RELEASE}

# ── LBP7010C カラーフィルタ (新規, 既存に非依存) ─────────────────
lbp7010c: lbp7010c.o capt_color.o
	$(CC) $(LBP7010C_CFLAGS) -o $@ $^ $(LBP7010C_LDFLAGS)

lbp7010c.o: lbp7010c.c capt_color.h
	$(CC) $(LBP7010C_CFLAGS) -c -o $@ $<

capt_color.o: capt_color.c capt_color.h
	$(CC) $(LBP7010C_CFLAGS) -c -o $@ $<

.PHONY: all install clean package
