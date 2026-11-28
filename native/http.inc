/* HTTPS transport for macOS libcurl. Reference selection remains in C. */
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
static uint64_t wait_until[7],last_request[7];
static const char *hosts[]={"commons.wikimedia.org","www.wikidata.org","upload.wikimedia.org","thumb.wikimedia.org","api.inaturalist.org","inaturalist-open-data.s3.amazonaws.com","api.gbif.org"};
static int cache_path(const char *url,wchar_t *out){const char *home=getenv("HOME");wchar_t base[FS_PATH],root[FS_PATH],name[80];char hash[65];if(!home||!fs_decode_utf8(0,0,home,-1,root,FS_PATH)||!fs_hash_hex(url,strlen(url),0,hash))return 0;fs_join(base,root,L"Library/Caches/AAAnimalStudio/http");if(fs_mkdirs(NULL,base,NULL))return 0;fs_decode_utf8(0,0,hash,-1,name,80);fs_wcat(name,80,L".bin");return fs_join(out,base,name);}
static char *cached_bytes(const wchar_t *p,size_t *n){FILE *f=fs_wfopen(p,L"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);char *s=NULL;if(size>0&&size<=16000000){s=malloc((size_t)size+1);if(s){if(fread(s,1,size,f)!=(size_t)size){free(s);s=NULL;}else{s[size]=0;*n=(size_t)size;}}}fclose(f);return s;}
typedef struct {char *data;size_t used;Job *job;long retry;} HttpBuffer;
static size_t receive_bytes(char *data,size_t size,size_t n,void *context){HttpBuffer *b=context;if(size&&n>SIZE_MAX/size)return 0;size_t bytes=size*n;if(fs_cancelled(b->job)||bytes>16000000-b->used)return 0;char *next=realloc(b->data,b->used+bytes+1);if(!next)return 0;b->data=next;memcpy(next+b->used,data,bytes);b->used+=bytes;next[b->used]=0;return bytes;}
static size_t receive_header(char *data,size_t size,size_t n,void *context){HttpBuffer *b=context;size_t bytes=size*n;if(bytes>12&&!strncasecmp(data,"Retry-After:",12)){char s[80];size_t k=bytes-12;if(k>79)k=79;memcpy(s,data+12,k);s[k]=0;char *end;long seconds=strtol(s,&end,10);if(end==s){time_t t=curl_getdate(s,NULL);seconds=t>time(NULL)?(long)(t-time(NULL)):60;}if(seconds>0)b->retry=seconds;}return bytes;}
static int transfer_progress(void *context,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){(void)a;(void)b;(void)c;(void)d;return fs_cancelled(context);}
static char *http_get(Job *j,const char *url,size_t *length){
    if(fs_cancelled(j))return NULL;CURLU *parts=curl_url();char *host=NULL,*scheme=NULL,*user=NULL;int index=-1;
    if(!parts||curl_url_set(parts,CURLUPART_URL,url,0)||curl_url_get(parts,CURLUPART_SCHEME,&scheme,0)||strcmp(scheme,"https")||curl_url_get(parts,CURLUPART_HOST,&host,0))goto invalid;
    if(!curl_url_get(parts,CURLUPART_USER,&user,0))goto invalid;
    for(int i=0;i<7;i++)if(!strcasecmp(host,hosts[i]))index=i;
invalid:curl_free(host);curl_free(scheme);curl_free(user);if(parts)curl_url_cleanup(parts);if(index<0)return NULL;
    wchar_t cache[FS_PATH]={0};cache_path(url,cache);char path[16384];struct stat st;
    if(cache[0]&&fs_encode_utf8(0,0,cache,-1,path,sizeof(path),NULL,NULL)&&!stat(path,&st)&&time(NULL)-st.st_mtime<7*86400){char *hit=cached_bytes(cache,length);if(hit)return hit;}
    uint64_t now=fs_milliseconds();if(now<wait_until[index])return cache[0]?cached_bytes(cache,length):NULL;
    if(index==4||index==6){if(now<last_request[index]+1100)fs_cancel_wait(j->cancel,(unsigned)(last_request[index]+1100-now));if(fs_cancelled(j))return NULL;last_request[index]=fs_milliseconds();}
    CURL *curl=curl_easy_init();if(!curl)return NULL;HttpBuffer buffer={0};buffer.job=j;buffer.retry=60;
    curl_easy_setopt(curl,CURLOPT_URL,url);curl_easy_setopt(curl,CURLOPT_USERAGENT,"AAAnimalStudio-macOS/1.0 (educational animal reference app)");curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(curl,CURLOPT_TIMEOUT,25L);curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive_bytes);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,receive_header);curl_easy_setopt(curl,CURLOPT_HEADERDATA,&buffer);curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,transfer_progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,j);curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    CURLcode code=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(curl);
    if(status==429||status==503)wait_until[index]=fs_milliseconds()+(uint64_t)buffer.retry*1000;
    if(code!=CURLE_OK||status!=200||!buffer.used){free(buffer.data);return !fs_cancelled(j)&&cache[0]?cached_bytes(cache,length):NULL;}
    *length=buffer.used;if(cache[0]){wchar_t temp[FS_PATH];fs_wformat(temp,FS_PATH,L"%s.%ld.tmp",cache,(long)getpid());FILE *f=fs_wfopen(temp,L"wb");if(f){int ok=fwrite(buffer.data,1,buffer.used,f)==buffer.used;if(fclose(f))ok=0;if(ok)fs_move(temp,cache,MOVEFILE_REPLACE_EXISTING);else fs_unlink(temp);}}
    return buffer.data;
}
