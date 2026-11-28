#define _GNU_SOURCE
#include "fauna.h"
#include <stdarg.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>
#include <curl/curl.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <CommonCrypto/CommonDigest.h>
#else
#include <sys/syscall.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#endif
extern char **environ;
void fs_init(void){if(!setlocale(LC_CTYPE,"en_US.UTF-8"))setlocale(LC_CTYPE,"C.UTF-8");setlocale(LC_NUMERIC,"C");curl_global_init(CURL_GLOBAL_DEFAULT);}
int fs_wcopy(wchar_t *d,size_t n,const wchar_t *s){if(!n||wcslen(s)>=n)return EINVAL;wcscpy(d,s);return 0;}
int fs_wncopy(wchar_t *d,size_t n,const wchar_t *s,size_t count){if(!n)return EINVAL;size_t k=wcslen(s);if(k>count)k=count;if(k>=n)k=n-1;wmemcpy(d,s,k);d[k]=0;return 0;}
int fs_wcat(wchar_t *d,size_t n,const wchar_t *s){size_t k=wcslen(d);return k<n?fs_wcopy(d+k,n-k,s):EINVAL;}
int fs_wformat(wchar_t *out,size_t n,const wchar_t *fmt,...){
    /* The inherited C core uses wide %s, as on its original CRT. */
    wchar_t converted[8192];size_t j=0;
    for(size_t i=0;fmt[i]&&j+4<8192;i++){converted[j++]=fmt[i];if(fmt[i]!='%')continue;i++;if(fmt[i]=='%'){converted[j++]=fmt[i];continue;}
        int is_long=0;while(fmt[i]&&wcschr(L"-+ #0.123456789*hljztL",fmt[i])){if(fmt[i]=='l')is_long=1;converted[j++]=fmt[i++];if(j+4>=8192)return -1;}
        if(fmt[i]=='s'&&!is_long)converted[j++]=L'l';if(fmt[i])converted[j++]=fmt[i];else break;}
    converted[j]=0;va_list a;va_start(a,fmt);int r=vswprintf(out,n,converted,a);va_end(a);return r;
}
int fs_decode_utf8(unsigned cp,unsigned flags,const char *s,int len,wchar_t *out,int cap){
    (void)cp;(void)flags;if(!s||!out||cap<=0)return 0;size_t n=len<0?strlen(s)+1:(size_t)len,j=0;
    for(size_t i=0;i<n;){uint32_t c=(unsigned char)s[i++];int extra=0;if(c>=0x80){if((c&0xe0)==0xc0){c&=31;extra=1;}else if((c&0xf0)==0xe0){c&=15;extra=2;}else if((c&0xf8)==0xf0){c&=7;extra=3;}else return 0;
        uint32_t minimum=extra==1?0x80:extra==2?0x800:0x10000;for(int k=0;k<extra;k++){if(i>=n||((unsigned char)s[i]&0xc0)!=0x80)return 0;c=(c<<6)|((unsigned char)s[i++]&63);}if(c<minimum||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return 0;}
        if(j>=(size_t)cap)return 0;out[j++]=(wchar_t)c;}return (int)j;
}
int fs_encode_utf8(unsigned cp,unsigned flags,const wchar_t *s,int len,char *out,int cap,void *a,void *b){
    (void)cp;(void)flags;(void)a;(void)b;if(!s||!out||cap<=0)return 0;size_t n=len<0?wcslen(s)+1:(size_t)len,j=0;
    for(size_t i=0;i<n;i++){uint32_t c=(uint32_t)s[i];if(c>0x10ffff||(c>=0xd800&&c<=0xdfff))return 0;int bytes=c<0x80?1:c<0x800?2:c<0x10000?3:4;if(j+bytes>(size_t)cap)return 0;
        if(bytes==1)out[j++]=(char)c;else{out[j++]=(char)((bytes==2?0xc0:bytes==3?0xe0:0xf0)|(c>>(6*(bytes-1))));for(int k=bytes-2;k>=0;k--)out[j++]=(char)(0x80|((c>>(6*k))&63));}}return (int)j;
}
static int path8(const wchar_t *p,char b[16384]){return fs_encode_utf8(0,0,p,-1,b,16384,NULL,NULL)>0;}
FILE *fs_wfopen(const wchar_t *p,const wchar_t *m){char path[16384],mode[32];if(!path8(p,path)||!fs_encode_utf8(0,0,m,-1,mode,32,NULL,NULL))return NULL;return fopen(path,mode);}
int fs_exists(const wchar_t *p){char b[16384];struct stat s;return path8(p,b)&&lstat(b,&s)==0;}
uint32_t fs_attributes(const wchar_t *p){return fs_exists(p)?0:INVALID_FILE_ATTRIBUTES;}
int fs_parent(wchar_t *p){wchar_t *s=wcsrchr(p,L'/');if(!s)return 0;if(s==p){if(!s[1])return 0;s[1]=0;}else *s=0;return 1;}
int fs_mkdir(const wchar_t *p,void *unused){(void)unused;char b[16384];return path8(p,b)&&mkdir(b,0700)==0;}
int fs_mkdirs(void *a,const wchar_t *p,void *c){(void)a;(void)c;char b[16384];if(!path8(p,b))return EINVAL;for(char *s=b+1;*s;s++)if(*s=='/'){*s=0;if(mkdir(b,0700)&&errno!=EEXIST)return errno;*s='/';}if(mkdir(b,0700)&&errno!=EEXIST)return errno;return 0;}
int fs_rmdir(const wchar_t *p){char b[16384];return path8(p,b)&&rmdir(b)==0;}
int fs_unlink(const wchar_t *p){char b[16384];return path8(p,b)&&unlink(b)==0;}
int fs_copyfile(const wchar_t *from,const wchar_t *to,int fail_if_exists){
    char dest[16384];if(!path8(to,dest))return 0;FILE *in=fs_wfopen(from,L"rb");if(!in)return 0;
    int fd=open(dest,O_WRONLY|O_CREAT|(fail_if_exists?O_EXCL:O_TRUNC),0600);if(fd<0){fclose(in);return 0;}
    FILE *out=fdopen(fd,"wb");if(!out){close(fd);fclose(in);return 0;}char b[65536];size_t n;int ok=1;
    while((n=fread(b,1,sizeof(b),in)))if(fwrite(b,1,n,out)!=n){ok=0;break;}
    if(ferror(in))ok=0;fclose(in);if(fclose(out))ok=0;if(!ok)unlink(dest);return ok;
}
int fs_move(const wchar_t *from,const wchar_t *to,unsigned flags){char a[16384],b[16384];if(!path8(from,a)||!path8(to,b))return 0;if(flags&MOVEFILE_REPLACE_EXISTING)return rename(a,b)==0;
#ifdef __APPLE__
    return renamex_np(a,b,RENAME_EXCL)==0;
#else
    return syscall(SYS_renameat2,AT_FDCWD,a,AT_FDCWD,b,1)==0;
#endif
}
uint32_t fs_absolute(const wchar_t *p,uint32_t n,wchar_t *out,void *unused){(void)unused;if(p[0]=='/')return fs_wcopy(out,n,p)?0:(uint32_t)wcslen(out);char cwd[16384];wchar_t w[4096];if(!getcwd(cwd,sizeof(cwd))||!fs_decode_utf8(0,0,cwd,-1,w,4096))return 0;int k=fs_wformat(out,n,L"%s/%s",w,p);return k>0?(uint32_t)k:0;}
int fs_create_id(FsId *id){uint32_t n;int fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;ssize_t got=read(fd,&n,sizeof(n));close(fd);if(got!=sizeof(n))return -1;id->value=n;return 0;}
void fs_utc_time(FsTime *t){time_t now=time(NULL);struct tm v;gmtime_r(&now,&v);*t=(FsTime){v.tm_year+1900,v.tm_mon+1,v.tm_mday,v.tm_hour,v.tm_min,v.tm_sec};}
FsCancel *fs_cancel_create(void *a,int manual,int signaled,void *b){(void)a;(void)b;(void)manual;FsCancel *c=malloc(sizeof(*c));if(c)atomic_init(&c->signaled,signaled);return c;}
void fs_cancel_set(FsCancel *c){if(c)atomic_store(&c->signaled,1);}
void fs_cancel_free(FsCancel *c){free(c);}
uint64_t fs_milliseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int fs_cancel_wait(FsCancel *c,unsigned ms){uint64_t end=fs_milliseconds()+ms;do{if(c&&atomic_load(&c->signaled))return 0;if(!ms)break;struct timespec t={0,10000000};nanosleep(&t,NULL);}while(fs_milliseconds()<end);return 258;}
static void hex(const unsigned char *d,int n,char *out){for(int i=0;i<n;i++)snprintf(out+2*i,3,"%02x",d[i]);}
int fs_hash_hex(const void *s,size_t n,int md5,char *out){unsigned char d[32];
#ifdef __APPLE__
    if(md5)CC_MD5(s,(CC_LONG)n,d);else CC_SHA256(s,(CC_LONG)n,d);
#else
    if(md5)MD5(s,n,d);else SHA256(s,n,d);
#endif
    hex(d,md5?16:32,out);return 1;
}
int fs_sha256_file(const wchar_t *path,char out[65]){FILE *f=fs_wfopen(path,L"rb");if(!f)return 0;unsigned char b[65536],d[32];size_t n;
#ifdef __APPLE__
    CC_SHA256_CTX c;CC_SHA256_Init(&c);while((n=fread(b,1,sizeof(b),f)))CC_SHA256_Update(&c,b,(CC_LONG)n);CC_SHA256_Final(d,&c);
#else
    SHA256_CTX c;SHA256_Init(&c);while((n=fread(b,1,sizeof(b),f)))SHA256_Update(&c,b,n);SHA256_Final(d,&c);
#endif
    int ok=!ferror(f);fclose(f);if(ok)hex(d,32,out);return ok;
}
void fs_error(wchar_t *out,const wchar_t *s){fs_wncopy(out,FS_ERROR,s,_TRUNCATE);}
int fs_join(wchar_t *out,const wchar_t *a,const wchar_t *b){if(fs_wformat(out,FS_PATH,L"%s/%s",a,b)<0)return 0;for(wchar_t *p=out;*p;p++)if(*p==L'\\')*p=L'/';return 1;}
int fs_root(wchar_t *out){char path[16384];const char *env=getenv("AAANIMAL_ROOT");if(env&&fs_decode_utf8(0,0,env,-1,out,FS_PATH))return 1;
#ifdef __APPLE__
    uint32_t n=sizeof(path);if(_NSGetExecutablePath(path,&n))return 0;
#else
    ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);if(n<0)return 0;path[n]=0;
#endif
    char resolved[16384];if(realpath(path,resolved))strcpy(path,resolved);if(!fs_decode_utf8(0,0,path,-1,out,FS_PATH))return 0;fs_parent(out);wchar_t p[FS_PATH];for(int i=0;i<8;i++){fs_join(p,out,L"ai/triposr_bridge.py");if(fs_exists(p))return 1;if(!fs_parent(out))break;}return 0;
}
void fs_status(Job *j,const wchar_t *s){if(j->progress)j->progress(j->context,s);}
int fs_cancelled(Job *j){if(j->cancel&&fs_cancel_wait(j->cancel,0)==0){fs_error(j->error,L"생성을 취소했습니다.");return 1;}return 0;}
int fs_default_output(wchar_t *out){const char *home=getenv("HOME");wchar_t w[FS_PATH],name[128],base[FS_PATH];FsId id;if(!home||!fs_decode_utf8(0,0,home,-1,w,FS_PATH)||fs_create_id(&id)<0)return 0;fs_join(base,w,L"Documents/AAAnimal Studio");time_t t=time(NULL);struct tm local;localtime_r(&t,&local);fs_wformat(name,128,L"%04d%02d%02d-%02d%02d%02d-%08lx",local.tm_year+1900,local.tm_mon+1,local.tm_mday,local.tm_hour,local.tm_min,local.tm_sec,id.value);return fs_join(out,base,name);}
static void emit(Job *j,char *b,size_t *used,int flush){size_t start=0;for(size_t i=0;i<*used;i++)if(b[i]=='\n'||(flush&&i+1==*used)){size_t n=i-start+(b[i]!='\n');wchar_t w[FS_ERROR];if(n&&b[start+n-1]=='\r')n--;int k=fs_decode_utf8(0,0,b+start,(int)n,w,FS_ERROR-1);if(k>0){w[k]=0;fs_status(j,w);}start=i+1;}if(start){memmove(b,b+start,*used-start);*used-=start;}}
int fs_python(Job *j,const wchar_t *mode,const wchar_t *input,const wchar_t *output,const wchar_t *log){
    wchar_t py[FS_PATH],script[FS_PATH];fs_join(py,j->root,L".venv/bin/python");fs_join(script,j->root,L"ai/triposr_bridge.py");if(!fs_exists(py)){fs_error(j->error,L"AI 환경이 없습니다. setup_macos.sh를 먼저 실행하세요.");return 0;}
    char p[16384],s[16384],in[16384],out[16384],m[128],res[32];if(!path8(py,p)||!path8(script,s)||!path8(input,in)||!path8(output,out)||!fs_encode_utf8(0,0,mode,-1,m,128,NULL,NULL))return 0;snprintf(res,sizeof(res),"%u",j->resolution);
    char *args[]={p,"-X","utf8","-u",s,m,"--input",in,"--output",out,"--resolution",res,NULL};int pipefd[2];if(pipe(pipefd))return 0;
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO);posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDERR_FILENO);posix_spawn_file_actions_addclose(&actions,pipefd[0]);posix_spawn_file_actions_addclose(&actions,pipefd[1]);posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    posix_spawnattr_t attr;posix_spawnattr_init(&attr);posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attr,0);pid_t pid;int code=posix_spawn(&pid,p,&actions,&attr,args,environ);posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attr);close(pipefd[1]);if(code){close(pipefd[0]);fs_error(j->error,L"AI 프로세스를 시작하지 못했습니다.");return 0;}
    fcntl(pipefd[0],F_SETFL,O_NONBLOCK);FILE *f=fs_wfopen(log,L"ab");char b[8192];size_t used=0;int status=0,ended=0,eof=0,cancelled=0;uint64_t deadline=0;
    while(!ended||!eof){if(fs_cancelled(j)&&!cancelled){kill(-pid,SIGTERM);cancelled=1;deadline=fs_milliseconds()+2000;}if(cancelled&&!ended&&fs_milliseconds()>deadline)kill(-pid,SIGKILL);
        struct pollfd pollfd={pipefd[0],POLLIN|POLLHUP,0};poll(&pollfd,1,80);ssize_t n=read(pipefd[0],b+used,sizeof(b)-used);if(n>0){if(f){fwrite(b+used,1,(size_t)n,f);fflush(f);}used+=(size_t)n;emit(j,b,&used,used==sizeof(b));}else if(n==0)eof=1;
        if(!ended){pid_t w=waitpid(pid,&status,WNOHANG);if(w==pid)ended=1;else if(w<0&&errno!=EINTR){ended=1;status=256;}}
    }emit(j,b,&used,1);if(f)fclose(f);close(pipefd[0]);int ok=!cancelled&&WIFEXITED(status)&&WEXITSTATUS(status)==0;if(!ok&&!j->error[0])fs_error(j->error,L"AI 실행에 실패했습니다. inference.log를 확인하세요.");return ok;
}
