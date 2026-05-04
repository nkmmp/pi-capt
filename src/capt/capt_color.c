/*
 * Canon LBP7010C Color CAPT Protocol
 * 官製captfilter出力との比較で確認済み:
 *   Hi-SCoA: 行頭0x40なし、未変化行0x41のみ、バンド終端0x42のみ
 *   buf[4]=0x02 定数
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "capt_color.h"

/* tMediaTypeTbl@@Base (0x2ef00) */
static uint8_t media_type_byte(uint8_t key)
{
    static const struct { uint8_t k, b; } t[]={
        {1,0x01},{3,0x02},{17,0x09},{11,0x10},{2,0x13},{13,0x14},{0,0x18}
    };
    for(int i=0;i<7;i++) if(t[i].k==key||t[i].k==0) return t[i].b;
    return 0x01;
}

/* capt_send_cmd */
int capt_send_cmd(int fd,uint16_t op,const void *pl,uint16_t plen)
{
    uint16_t tot=(uint16_t)(4+plen);
    uint8_t h[4]={(uint8_t)(op&0xFF),(uint8_t)(op>>8),
                  (uint8_t)(tot&0xFF),(uint8_t)(tot>>8)};
    if(write(fd,h,4)!=4)return -1;
    if(plen>0&&write(fd,pl,plen)!=(ssize_t)plen)return -1;
    return 0;
}

/* Hi-SCoA バンド圧縮器
 *   [行データ or 0x41(未変化)] × N行 + 0x42(バンド終端)
 *   ※ 行頭0x40マーカーなし (pi-captの0x40は旧プロトコル向け)
 */
#define BMAX 65536
struct capt_band_buf {
    uint8_t pkt[BMAX]; int pkt_pos;
    uint8_t prev[768]; int has_prev; int total;
};

capt_band_buf_t *capt_band_new(void)
{
    capt_band_buf_t *b=calloc(1,sizeof(*b));
    if(b){b->pkt[0]=0xa0;b->pkt[1]=0xc0;b->pkt_pos=4;}
    return b;
}
void capt_band_free(capt_band_buf_t *b){free(b);}
void capt_band_reset(capt_band_buf_t *b)
{
    b->pkt[0]=0xa0;b->pkt[1]=0xc0;b->pkt[2]=0;b->pkt[3]=0;
    b->pkt_pos=4;b->has_prev=0;b->total=0;
}
const uint8_t *capt_band_data(const capt_band_buf_t *b){return b->pkt;}
int capt_band_size(const capt_band_buf_t *b){return b->pkt_pos;}

static int ld(const uint8_t *a,const uint8_t *b,int n)
{int d=-1;for(int i=0;i<n;i++)if(a[i]!=b[i])d=i;return d;}
static int bw(capt_band_buf_t *b,const uint8_t *s,int n)
{if(b->pkt_pos+n>BMAX-4)return -1;memcpy(b->pkt+b->pkt_pos,s,n);b->pkt_pos+=n;return 0;}
static int by(capt_band_buf_t *b,uint8_t c){return bw(b,&c,1);}

int capt_band_compress_line(capt_band_buf_t *b,const uint8_t *line,
                             int lb,int last,int final_page)
{
    int diff=b->has_prev?ld(line,b->prev,lb)+1:lb;
    if(diff<0)diff=0;

    /* 未変化行: 0x41のみ*/
    if(diff==0&&b->has_prev){
        if(by(b,0x41)<0)return -1;
    } else {
        /* 変化部分をエンコード */
        for(int lp=0;lp<diff;){
            if(lp+1<diff&&line[lp]==line[lp+1]){
                int r=2;
                while(r<255&&lp+r<diff&&line[lp+r]==line[lp])r++;
                if(r<8){
                    if(by(b,(uint8_t)(0x40|(r<<3)))<0)return -1;
                    if(by(b,line[lp])<0)return -1;
                }else{
                    if(by(b,(uint8_t)(0xa0|(r>>3)))<0)return -1;
                    if(by(b,(uint8_t)(0x80|(r&7)<<3))<0)return -1;
                    if(by(b,line[lp])<0)return -1;
                }
                lp+=r;
            }else{
                const uint8_t *s=line+lp;int r=1;
                while(r<255&&lp+r+2<diff){
                    if(s[r]!=s[r+1]||s[r+1]!=s[r+2])r++;else break;}
                if(lp+r>=diff)r=diff-lp;
                if(r<8){if(by(b,(uint8_t)(r<<3))<0)return -1;}
                else{if(by(b,(uint8_t)(0xa0|(r>>3)))<0)return -1;
                     if(by(b,(uint8_t)(0xc0|(r&7)<<3))<0)return -1;}
                if(bw(b,s,r)<0)return -1;
                lp+=r;
            }
        }
        /* 短行終端: 変化部がline_bytes未満の場合 */
        if(diff<lb&&by(b,0x41)<0)return -1;
    }
    memcpy(b->prev,line,lb);b->has_prev=1;

    if(last){
        /* 0x42はページ最終バンドにのみ付与
         * bands[0..N-2]: 0x42なし、bands[N-1]: 0x42あり */
        if(final_page && by(b,0x42)<0)return -1;
        int sz=b->pkt_pos;
        b->pkt[2]=(uint8_t)(sz&0xFF);b->pkt[3]=(uint8_t)(sz>>8);
        b->total+=sz;
    }
    return 0;
}

/* ICBeginPage (0xD0A0) — 40B payload 全フィールド確定
 *
 * buf[4]=0x02: pi-capt LBP810参照値と一致
 *              color flagではなく定数。カラー/モノは ICxxxPlane の送信数で区別。
 * buf[5]=media_key: CUPS media type code (PlainPaper=0x01)
 * buf[36]=tMediaTypeTbl.byte8: PlainPaper=0x01確定
 * buf[37-39]=0x00: optDevType≤10確定
 */
int capt_color_begin_page(int fd,int wpx,int hlines,int bh,int color,uint8_t mk)
{
    (void)wpx;(void)bh;(void)color;  /* buf[4]は0x02定数のためcolorフラグ不使用 */
    uint8_t buf[40];
    memset(buf,0,40);
    buf[2]=0xa4;buf[3]=0x01;          /* [2-3] 0x01A4=420 */
    buf[4]=0x02;                       /* [4]   0x02 定数 */
    buf[5]=mk;                         /* [5]   media_key */
    buf[8]=buf[9]=buf[10]=buf[11]=0x1f; /* margins */
    buf[13]=0x11;buf[14]=0x03;buf[15]=0x01;
    buf[16]=0x01;buf[17]=0x01;buf[18]=0x02;
    buf[22]=0x70;                      /* [22-23] 0x0070=112 */
    buf[24]=0x78;                      /* [24-25] 0x0078=120 */
    buf[26]=(uint8_t)(COLOR_LINE_SIZE&0xFF);
    buf[27]=(uint8_t)(COLOR_LINE_SIZE>>8);
    buf[28]=(uint8_t)(hlines&0xFF);
    buf[29]=(uint8_t)(hlines>>8);
    buf[30]=(uint8_t)(COLOR_PAGE_WIDTH_PX&0xFF);
    buf[31]=(uint8_t)(COLOR_PAGE_WIDTH_PX>>8);
    buf[32]=(uint8_t)(COLOR_TOTAL_HEIGHT&0xFF);
    buf[33]=(uint8_t)(COLOR_TOTAL_HEIGHT>>8);
    /* [34-35] FLAGS=0x0000 */
    buf[36]=media_type_byte(mk);       /* tMediaTypeTbl確定 */
    /* [37-39] 0x00 */
    return capt_send_cmd(fd,IC_BEGIN_PAGE,buf,40);
}

/* ICxxxPlane — 確定値 {0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00} */
int capt_color_send_plane_header(int fd,uint16_t op)
{
    static const uint8_t buf[8]={0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00};
    return capt_send_cmd(fd,op,buf,8);
}

int capt_color_begin_data(int fd){return capt_send_cmd(fd,IC_BEGIN_DATA,NULL,0);}
int capt_color_send_band(int fd,const uint8_t *d,int s)
{if(write(fd,d,s)!=s)return -1;return 0;}

/* ICVideoCount — divisor=4確定 */
static int send_vcnt(int fd,int k,int c,int m,int y)
{
    uint8_t buf[16];
    uint32_t kv=(uint32_t)(k/4),cv=(uint32_t)(c/4),
             mv=(uint32_t)(m/4),yv=(uint32_t)(y/4);
    memcpy(buf+0,&kv,4);memcpy(buf+4,&cv,4);
    memcpy(buf+8,&mv,4);memcpy(buf+12,&yv,4);
    return capt_send_cmd(fd,IC_VIDEO_COUNT,buf,16);
}

int capt_color_end_page(int fd,int total_bands,int is_color)
{
    if(capt_send_cmd(fd,IC_END_K,NULL,0)<0)return -1;
    if(is_color){
        if(capt_send_cmd(fd,IC_END_C,NULL,0)<0)return -1;
        if(capt_send_cmd(fd,IC_END_M,NULL,0)<0)return -1;
        if(capt_send_cmd(fd,IC_END_Y,NULL,0)<0)return -1;
    }
    int pb=total_bands*COLOR_LINE_SIZE*COLOR_ROWS_PER_BAND;
    if(send_vcnt(fd,pb,is_color?pb:0,is_color?pb:0,is_color?pb:0)<0)return -1;
    return capt_send_cmd(fd,IC_END_PAGE,NULL,0);
}
