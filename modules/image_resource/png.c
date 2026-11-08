#include "dunlin/image_resource.h"
#include "dunlin/content_hash.h"
#include <zlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static int legal_depth(unsigned color, unsigned depth) {
    switch (color) {
    case 0: return depth==1 || depth==2 || depth==4 || depth==8 || depth==16;
    case 2: case 4: case 6: return depth==8 || depth==16;
    case 3: return depth==1 || depth==2 || depth==4 || depth==8;
    default: return 0;
    }
}
static size_t rows_size(unsigned w,unsigned h,unsigned bits,unsigned interlace) {
    static const unsigned x[]={0,4,0,2,0,1,0},y[]={0,0,4,0,2,0,1};
    static const unsigned dx[]={8,8,4,4,2,2,1},dy[]={8,8,8,4,4,2,2};
    size_t size=0;
    for(unsigned pass=0;pass<(interlace?7u:1u);++pass) {
        unsigned width=interlace?(w>x[pass]?(w-x[pass]+dx[pass]-1)/dx[pass]:0):w;
        unsigned height=interlace?(h>y[pass]?(h-y[pass]+dy[pass]-1)/dy[pass]:0):h;
        if(width && height) size+=((width*bits+7)/8+1)*height;
    }
    return size;
}
static int filters(const unsigned char *p,unsigned w,unsigned h,unsigned bits,unsigned interlace) {
    static const unsigned x[]={0,4,0,2,0,1,0},y[]={0,0,4,0,2,0,1};
    static const unsigned dx[]={8,8,4,4,2,2,1},dy[]={8,8,8,4,4,2,2};
    size_t offset=0;
    for(unsigned pass=0;pass<(interlace?7u:1u);++pass) {
        unsigned width=interlace?(w>x[pass]?(w-x[pass]+dx[pass]-1)/dx[pass]:0):w;
        unsigned height=interlace?(h>y[pass]?(h-y[pass]+dy[pass]-1)/dy[pass]:0):h;
        if(width && height) for(unsigned row=0;row<height;++row) {
            if(p[offset]>4) return 0;
            offset+=(width*bits+7)/8+1;
        }
    }
    return 1;
}
dl_image_status dl_png_inspect(const void *bytes,size_t n,dl_image_info *info) {
    if(info) memset(info,0,sizeof *info);
    if(!bytes || !info) return DL_IMAGE_INVALID;
    if(n>DL_IMAGE_MAX_BYTES) return DL_IMAGE_LIMIT;
    const unsigned char *p=bytes;
    static const unsigned char signature[]={137,80,78,71,13,10,26,10};
    if(n<57 || memcmp(p,signature,8)) return DL_IMAGE_INVALID;
    unsigned width=0,height=0,depth=0,color=0,interlace=0;
    int header=0,palette=0,idat=0,idat_end=0,end=0;
    size_t compressed_size=0;
    unsigned char *compressed=malloc(n);
    if(!compressed) return DL_IMAGE_NOMEM;
    dl_image_status status=DL_IMAGE_INVALID;
    for(size_t off=8;off<n;) {
        if(n-off<12) goto done;
        uint32_t length=be32(p+off);const unsigned char *type=p+off+4,*data=p+off+8;
        if(length>n-off-12 || (length&UINT32_C(0x80000000))) goto done;
        for(unsigned i=0;i<4;i++) if(!((type[i]>='A'&&type[i]<='Z')||(type[i]>='a'&&type[i]<='z'))) goto done;
        if(type[2]&32) goto done; /* reserved chunk type bit */
        uLong crc=crc32(0L,Z_NULL,0);crc=crc32(crc,type,length+4);
        if((uint32_t)crc!=be32(data+length)) goto done;
        if(!header && memcmp(type,"IHDR",4)) goto done;
        if(!memcmp(type,"IHDR",4)) {
            if(header || length!=13 || off!=8) goto done;
            width=be32(data);height=be32(data+4);depth=data[8];color=data[9];interlace=data[12];
            if(!width||!height||width>DL_IMAGE_MAX_DIMENSION||height>DL_IMAGE_MAX_DIMENSION) {status=DL_IMAGE_LIMIT;goto done;}
            if(!legal_depth(color,depth) || data[10] || data[11] || interlace>1) goto done;
            header=1;
        } else if(!memcmp(type,"PLTE",4)) {
            if(palette||idat||!length||length%3||length>768||color==0||color==4||(color==3&&length/3>(1u<<depth))) goto done;
            palette=1;
        } else if(!memcmp(type,"IDAT",4)) {
            if(idat_end || (color==3&&!palette)) goto done;
            memcpy(compressed+compressed_size,data,length);compressed_size+=length;idat=1;
        } else if(!memcmp(type,"IEND",4)) {
            if(length||!idat||off+12!=n) goto done;
            end=1;off+=12;break;
        } else {
            if(!(type[0]&32) || !memcmp(type,"acTL",4)||!memcmp(type,"fcTL",4)||!memcmp(type,"fdAT",4)) goto done;
            if(idat) idat_end=1;
        }
        off+=(size_t)length+12;
    }
    if(!end || !compressed_size) goto done;
    unsigned channels=color==2?3:color==4?2:color==6?4:1;
    size_t expected=rows_size(width,height,channels*depth,interlace);
    /* Maximum decoded scanline storage is bounded by dimensions, <=8.4 MiB.
     * One extra byte distinguishes exact completion from oversized output. */
    unsigned char *raw=malloc(expected+1);
    if(!raw) {status=DL_IMAGE_NOMEM;goto done;}
    z_stream z={0};z.next_in=compressed;z.avail_in=(uInt)compressed_size;
    z.next_out=raw;z.avail_out=(uInt)(expected+1);
    int init=inflateInit(&z),result=init==Z_OK?inflate(&z,Z_FINISH):init;
    int correct=result==Z_STREAM_END && z.total_out==expected && !z.avail_in
        && filters(raw,width,height,channels*depth,interlace);
    if(init==Z_OK) inflateEnd(&z);
    free(raw);
    if(!correct) goto done;
    info->width=width;info->height=height;info->bytes=n;dc_sha256(bytes,n,info->sha256);
    status=DL_IMAGE_OK;
done:
    free(compressed);return status;
}
dl_image_status dl_png_data_url(const void *bytes,size_t n,char **url) {
    if(url) *url=NULL;
    if(!url) return DL_IMAGE_INVALID;
    dl_image_info info;dl_image_status status=dl_png_inspect(bytes,n,&info);
    if(status!=DL_IMAGE_OK) return status;
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char prefix[]="data:image/png;base64,";
    size_t encoded=(n+2)/3*4,offset=sizeof prefix-1;
    char *out=malloc(offset+encoded+1);
    if(!out) return DL_IMAGE_NOMEM;
    memcpy(out,prefix,offset);const unsigned char *p=bytes;
    for(size_t i=0;i<n;i+=3) {
        unsigned value=(unsigned)p[i]<<16;
        if(i+1<n)value|=(unsigned)p[i+1]<<8;
        if(i+2<n)value|=p[i+2];
        out[offset++]=alphabet[value>>18];out[offset++]=alphabet[(value>>12)&63];
        out[offset++]=i+1<n?alphabet[(value>>6)&63]:'=';
        out[offset++]=i+2<n?alphabet[value&63]:'=';
    }
    out[offset]=0;*url=out;return DL_IMAGE_OK;
}
void dl_image_free(void *p) {free(p);}
