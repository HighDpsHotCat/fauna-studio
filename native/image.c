/* macOS ImageIO orientation, decode and resize; pixel processing stays C. */
#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void photo_free(Photo *p) { free(p->rgba); memset(p, 0, sizeof(*p)); }
int photo_has_alpha(const Photo *p) {
    size_t count = (size_t)p->width * p->height;
    for (size_t i=0; i<count; ++i) if (p->rgba[i*4+3] < 250) return 1;
    return 0;
}
int photo_write(const wchar_t *path, const Photo *p, wchar_t *error) {
    FILE *f = fs_wfopen(path, L"wb"); uint32_t header[3] = {0x31475246, p->width, p->height};
    if (!f) { fs_error(error,L"사진 임시 파일을 쓸 수 없습니다."); return 0; }
    size_t n = (size_t)p->width*p->height*4;
    int ok = fwrite(header, 4, 3, f)==3 && fwrite(p->rgba, 1, n, f)==n;
    if (fclose(f)) ok=0;
    if (!ok) fs_error(error,L"사진 파일 쓰기에 실패했습니다.");
    return ok;
}
int photo_read(const wchar_t *path, Photo *p, wchar_t *error) {
    FILE *f = fs_wfopen(path,L"rb"); uint32_t h[3]; memset(p,0,sizeof(*p));
    if (!f) goto bad;
    if (fread(h,4,3,f)!=3 || h[0]!=0x31475246 || !h[1] || !h[2] || h[1]>8192 || h[2]>8192) goto bad;
    size_t n=(size_t)h[1]*h[2]*4;
    p->rgba=malloc(n); if (!p->rgba) goto bad;
    if (fread(p->rgba,1,n,f)!=n || fgetc(f)!=EOF) goto bad;
    p->width=h[1]; p->height=h[2]; fclose(f); return 1;
bad:
    if(f) fclose(f); photo_free(p); fs_error(error,L"배경 분리 결과가 손상되었습니다."); return 0;
}

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
int photo_load(const wchar_t *path,Photo *p,wchar_t *error){
    memset(p,0,sizeof(*p));char utf8[16384];if(!fs_encode_utf8(0,0,path,-1,utf8,sizeof(utf8),NULL,NULL))return 0;
    CFURLRef url=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8*)utf8,strlen(utf8),false);if(!url)return 0;
    CGImageSourceRef source=CGImageSourceCreateWithURL(url,NULL);CFRelease(url);if(!source)goto fail;
    int max=2048;CFNumberRef size=CFNumberCreate(NULL,kCFNumberIntType,&max);
    const void *keys[]={kCGImageSourceCreateThumbnailFromImageAlways,kCGImageSourceCreateThumbnailWithTransform,kCGImageSourceThumbnailMaxPixelSize};const void *values[]={kCFBooleanTrue,kCFBooleanTrue,size};
    CFDictionaryRef options=CFDictionaryCreate(NULL,keys,values,3,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);CGImageRef image=CGImageSourceCreateThumbnailAtIndex(source,0,options);CFRelease(options);CFRelease(size);CFRelease(source);if(!image)goto fail;
    size_t w=CGImageGetWidth(image),h=CGImageGetHeight(image);p->rgba=calloc(w*h,4);CGColorSpaceRef colour=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context=p->rgba?CGBitmapContextCreate(p->rgba,w,h,8,w*4,colour,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big):NULL;CGColorSpaceRelease(colour);
    if(!context){CGImageRelease(image);goto fail;}CGContextTranslateCTM(context,0,(CGFloat)h);CGContextScaleCTM(context,1,-1);CGContextDrawImage(context,CGRectMake(0,0,w,h),image);CGContextRelease(context);CGImageRelease(image);
    for(size_t i=0;i<w*h;i++){unsigned char *c=p->rgba+4*i;if(c[3]&&c[3]!=255)for(int k=0;k<3;k++)c[k]=(unsigned char)fmin(255,(c[k]*255+c[3]/2)/c[3]);}
    p->width=(uint32_t)w;p->height=(uint32_t)h;return 1;
fail:photo_free(p);fs_error(error,L"사진을 읽을 수 없습니다. JPG 또는 PNG를 선택하세요.");return 0;
}
#else
int photo_load(const wchar_t *path,Photo *p,wchar_t *error){return photo_read(path,p,error);}
#endif

int photo_prepare(const Photo *p, Photo *out, wchar_t *error) {
    /* C owns foreground crop, padding and alpha compositing. */
    uint32_t xmin=p->width,ymin=p->height,xmax=0,ymax=0;
    memset(out,0,sizeof(*out));
    for(uint32_t y=0;y<p->height;y++)for(uint32_t x=0;x<p->width;x++) {
        if(p->rgba[((size_t)y*p->width+x)*4+3]>32){
            if(x<xmin)xmin=x;if(x>xmax)xmax=x;if(y<ymin)ymin=y;if(y>ymax)ymax=y;
        }
    }
    if(xmin>xmax||ymin>ymax){fs_error(error,L"피사체를 찾지 못했습니다. 배경과 구분되는 사진을 선택하세요.");return 0;}
    uint32_t w=xmax-xmin+1,h=ymax-ymin+1;
    uint32_t side=(uint32_t)ceil((w>h?w:h)/0.85), ox=(side-w)/2,oy=(side-h)/2;
    out->rgba=malloc((size_t)side*side*4);if(!out->rgba){fs_error(error,L"사진 처리 메모리가 부족합니다.");return 0;}
    out->width=out->height=side;
    for(size_t i=0;i<(size_t)side*side;i++){memset(out->rgba+4*i,128,3);out->rgba[4*i+3]=255;}
    for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) {
        const unsigned char *s=p->rgba+((size_t)(y+ymin)*p->width+x+xmin)*4;
        unsigned char *d=out->rgba+((size_t)(y+oy)*side+x+ox)*4;
        for(int c=0;c<3;c++)d[c]=(unsigned char)((s[c]*s[3]+128*(255-s[3])+127)/255);
    }
    return 1;
}
