#include "fauna.h"
#include <assert.h>
#include <unistd.h>
int main(void){
    fs_init();wchar_t w[128],error[FS_ERROR]={0};char text[512];
    const char *input="한글 사진 🦊.glb";
    assert(fs_decode_utf8(0,0,input,-1,w,128));
    assert(fs_encode_utf8(0,0,w,-1,text,sizeof(text),NULL,NULL));assert(!strcmp(input,text));
    assert(!fs_decode_utf8(0,0,"\xc0\x80",2,w,128));
    assert(fs_wformat(w,128,L"%s/%08lx-%u",L"고양이",0x123UL,42)==15);
    assert(!wcscmp(w,L"고양이/00000123-42"));
    wchar_t dir[128];fs_wformat(dir,128,L"test-한글-경로-%u",(unsigned)getpid());assert(fs_mkdir(dir,NULL));
    wchar_t a[FS_PATH],b[FS_PATH];fs_join(a,dir,L"원본.txt");fs_join(b,dir,L"복사.txt");
    FILE *f=fs_wfopen(a,L"wb");assert(f);fputs("abc",f);fclose(f);
    assert(fs_copyfile(a,b,1));assert(!fs_copyfile(a,b,1));assert(!fs_move(a,b,0));assert(fs_exists(a));
    assert(fs_move(a,b,MOVEFILE_REPLACE_EXISTING));assert(!fs_exists(a));
    char hash[65];assert(fs_sha256_file(b,hash));assert(!strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(fs_unlink(b));assert(fs_rmdir(dir));
    FsCancel *cancel=fs_cancel_create(NULL,1,0,NULL);assert(cancel);assert(fs_cancel_wait(cancel,0)!=0);fs_cancel_set(cancel);assert(fs_cancel_wait(cancel,0)==0);fs_cancel_free(cancel);
#ifdef __APPLE__
    /* A two-row PPM checks ImageIO's top-to-bottom convention. */
    f=fopen("test-orientation.ppm","wb");assert(f);fputs("P6\n2 2\n255\n",f);
    unsigned char rgb[]={255,0,0,255,0,0,0,0,255,0,0,255};fwrite(rgb,1,12,f);fclose(f);
    Photo p={0};assert(photo_load(L"test-orientation.ppm",&p,error));assert(p.width==2&&p.height==2);
    assert(p.rgba[0]>240&&p.rgba[2]<15&&p.rgba[8]<15&&p.rgba[10]>240);
    photo_free(&p);unlink("test-orientation.ppm");
#endif
    (void)error;puts("PASS UTF-8 paths, atomic no-overwrite move, SHA-256, cancellation and image orientation");return 0;
}
