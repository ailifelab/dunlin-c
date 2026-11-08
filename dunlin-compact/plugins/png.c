#include "png.h"
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32(const unsigned char *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}

struct json_object *ah_png_content(const char *prompt,const ah_png *image) {
    const unsigned char *p=image->data;
    size_t n=image->size;
    if(!p || n<45 || n>1024*1024 || memcmp(p,"\x89PNG\r\n\x1a\n",8) ||
       read32(p+8)!=13 || memcmp(p+12,"IHDR",4) ||
       !read32(p+16) || read32(p+16)>1024 || !read32(p+20) || read32(p+20)>1024 ||
       read32(p+n-12)!=0 || memcmp(p+n-8,"IEND",4)) return NULL;
    static const char prefix[]="data:image/png;base64,";
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *url=malloc(sizeof(prefix)+4*((n+2)/3));
    if(!url) return NULL;
    memcpy(url,prefix,sizeof(prefix)-1);
    char *out=url+sizeof(prefix)-1;
    for(size_t i=0;i<n;i+=3) {
        unsigned bits=(unsigned)p[i]<<16;
        if(i+1<n) bits|=(unsigned)p[i+1]<<8;
        if(i+2<n) bits|=p[i+2];
        *out++=alphabet[(bits>>18)&63];
        *out++=alphabet[(bits>>12)&63];
        *out++=i+1<n?alphabet[(bits>>6)&63]:'=';
        *out++=i+2<n?alphabet[bits&63]:'=';
    }
    *out=0;
    struct json_object *parts=json_object_new_array(),*text=json_object_new_object();
    struct json_object *info=json_object_new_object(),*picture=json_object_new_object();
    json_object_object_add(text,"type",json_object_new_string("text"));
    json_object_object_add(text,"text",json_object_new_string(prompt));
    json_object_object_add(info,"url",json_object_new_string(url));
    json_object_object_add(picture,"type",json_object_new_string("image_url"));
    json_object_object_add(picture,"image_url",info);
    json_object_array_add(parts,text);
    json_object_array_add(parts,picture);
    free(url);
    return parts;
}
