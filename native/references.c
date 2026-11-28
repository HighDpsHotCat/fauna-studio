/* Species resolution, HTTPS, reference selection and palette use are C code. */
#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../vendor/cjson/cJSON.h"

#define MAX_REFS 24
#define MAX_CANDIDATES 64
static const char *str(const cJSON *o,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static cJSON *field(const cJSON *o,const char *key){return cJSON_GetObjectItemCaseSensitive(o,key);}
static cJSON *read_json(const wchar_t *path){
    FILE *f=fs_wfopen(path,L"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<1||n>20000000){fclose(f);return NULL;}
    char *bytes=malloc((size_t)n+1);if(!bytes){fclose(f);return NULL;}
    size_t got=fread(bytes,1,n,f);fclose(f);bytes[got]=0;cJSON *data=got==(size_t)n?cJSON_Parse(bytes):NULL;free(bytes);return data;
}
static int write_json(const wchar_t *path,const cJSON *data){
    char *s=cJSON_Print(data);if(!s)return 0;FILE *f=fs_wfopen(path,L"wb");int ok=0;
    if(f){size_t n=strlen(s);ok=fwrite(s,1,n,f)==n;if(fclose(f))ok=0;}free(s);return ok;
}
static void encode(const char *s,char *out,size_t capacity){
    const char hex[]="0123456789ABCDEF";size_t j=0;
    for(size_t i=0;s[i]&&j+4<capacity;i++){unsigned char c=(unsigned char)s[i];
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')out[j++]=(char)c;
        else{out[j++]='%';out[j++]=hex[c>>4];out[j++]=hex[c&15];}}
    out[j]=0;
}
#include "http.inc"
static cJSON *api(Job *job,const char *url){size_t length=0;char *s=http_get(job,url,&length);if(!s)return NULL;cJSON *o=cJSON_Parse(s);free(s);if(field(o,"error")){cJSON_Delete(o);return NULL;}return o;}
static cJSON *entity(Job *job,const char *qid){
    char url[1024];snprintf(url,sizeof(url),"https://www.wikidata.org/w/api.php?action=wbgetentities&format=json&props=claims%%7Clabels&languages=ko&ids=%s",qid);
    cJSON *root=api(job,url),*value=cJSON_Duplicate(field(field(root,"entities"),qid),1);cJSON_Delete(root);return value;
}
static cJSON *claim(const cJSON *e,const char *property){
    cJSON *list=field(field(e,"claims"),property),*item=NULL;
    cJSON_ArrayForEach(item,list){if(strcmp(str(item,"rank"),"deprecated")){cJSON *v=field(field(field(item,"mainsnak"),"datavalue"),"value");if(v)return v;}}
    return NULL;
}
static cJSON *taxon(Job *job,const char *name){
    char encoded[512],url[2048];encode(name,encoded,sizeof(encoded));
    snprintf(url,sizeof(url),"https://www.wikidata.org/w/api.php?action=wbsearchentities&format=json&search=%s&language=en&type=item&limit=5",encoded);
    cJSON *hits=api(job,url),*hit=NULL,*result=NULL;
    cJSON_ArrayForEach(hit,field(hits,"search")){
        cJSON *e=entity(job,str(hit,"id")),*scientific=claim(e,"P225");
        if(cJSON_IsString(scientific)&&!strcasecmp(scientific->valuestring,name)){
            cJSON *current=claim(e,"P12765");
            if(current&&str(current,"id")[0]){cJSON *next=entity(job,str(current,"id"));if(cJSON_IsString(claim(next,"P225"))){cJSON_Delete(e);e=next;}else cJSON_Delete(next);}
            result=cJSON_CreateObject();cJSON_AddStringToObject(result,"scientific",claim(e,"P225")->valuestring);
            cJSON_AddStringToObject(result,"qid",str(e,"id"));
            cJSON *category=claim(e,"P373");cJSON_AddStringToObject(result,"category",cJSON_IsString(category)?category->valuestring:claim(e,"P225")->valuestring);
            cJSON_AddStringToObject(result,"korean",str(field(field(e,"labels"),"ko"),"value"));
            cJSON_Delete(e);break;
        }
        cJSON_Delete(e);if(fs_cancelled(job))break;
    }
    cJSON_Delete(hits);return result;
}
static int contains(const char *s,const char *term){
    size_t n=strlen(term);for(;*s;s++)if(!strncasecmp(s,term,n))return 1;return 0;
}
static int excluded(const char *title,const char *categories){
    const char *terms[]={"painting","drawing","illustration","taxidermy","sculpture","skeleton","close-up","skull","postage","diagram","genital"};
    for(size_t i=0;i<sizeof(terms)/sizeof(*terms);i++)if(contains(title,terms[i])||contains(categories,terms[i]))return 1;return 0;
}
static void plain(const char *source,char *dest,size_t size){
    size_t n=0;int tag=0;for(;*source&&n+1<size;source++){if(*source=='<'){tag=1;continue;}if(*source=='>'){tag=0;continue;}if(!tag)dest[n++]=*source;}dest[n]=0;
}
static void collect_query(Job *job,cJSON *refs,const char *query,const char *view,int limit){
    char encoded[4096],url[8192];encode(query,encoded,sizeof(encoded));
    snprintf(url,sizeof(url),"https://commons.wikimedia.org/w/api.php?action=query&format=json&formatversion=2&generator=search&gsrnamespace=6&gsrsearch=%s%%20filetype%%3Abitmap&gsrlimit=16&prop=imageinfo&iiprop=url%%7Cmime%%7Cextmetadata&iiurlwidth=640&iiextmetadatalanguage=en&iiextmetadatafilter=Artist%%7CLicenseShortName%%7CCategories",encoded);
    cJSON *root=api(job,url),*page=NULL;int added=0;
    cJSON_ArrayForEach(page,field(field(root,"query"),"pages")){
        if(cJSON_GetArraySize(refs)>=MAX_REFS||added>=limit)break;
        cJSON *info=cJSON_GetArrayItem(field(page,"imageinfo"),0),*meta=field(info,"extmetadata");
        if(strcmp(str(info,"mime"),"image/jpeg")&&strcmp(str(info,"mime"),"image/png"))continue;
        if(!str(info,"thumburl")[0]||excluded(str(page,"title"),str(field(meta,"Categories"),"value")))continue;
        double id=cJSON_GetNumberValue(field(page,"pageid"));int duplicate=0;cJSON *previous=NULL;
        cJSON_ArrayForEach(previous,refs)if(cJSON_GetNumberValue(field(previous,"pageId"))==id)duplicate=1;
        if(duplicate)continue;
        cJSON *r=cJSON_CreateObject();char author[1024],license[256];
        plain(str(field(meta,"Artist"),"value"),author,sizeof(author));plain(str(field(meta,"LicenseShortName"),"value"),license,sizeof(license));
        if(!author[0]||!license[0]||!str(info,"descriptionurl")[0]){cJSON_Delete(r);continue;}
        cJSON_AddNumberToObject(r,"pageId",id);cJSON_AddStringToObject(r,"title",str(page,"title"));cJSON_AddStringToObject(r,"url",str(info,"thumburl"));
        cJSON_AddStringToObject(r,"pageUrl",str(info,"descriptionurl"));cJSON_AddStringToObject(r,"author",author);cJSON_AddStringToObject(r,"license",license);
        cJSON_AddStringToObject(r,"categoryView",view);cJSON_AddStringToObject(r,"searchBasis",query);cJSON_AddItemToArray(refs,r);added++;
        cJSON_AddStringToObject(r,"provider","Wikimedia Commons");
    }
    cJSON_Delete(root);
}
static int same_image(const char *a,const char *b){
    if(!strcmp(a,b))return 1;const char *prefix="https://inaturalist-open-data.s3.amazonaws.com/photos/";size_t n=strlen(prefix);
    if(!strncmp(a,prefix,n)&&!strncmp(b,prefix,n)){const char *ae=strchr(a+n,'/'),*be=strchr(b+n,'/');return ae&&be&&ae-a==be-b&&!strncmp(a,b,(size_t)(ae-a));}return 0;
}
static int has_url(const cJSON *refs,const char *url){const cJSON *r=NULL;cJSON_ArrayForEach(r,refs)if(same_image(str(r,"url"),url)||(*str(r,"mediaIdentifier")&&same_image(str(r,"mediaIdentifier"),url)))return 1;return 0;}
static void collect_gbif(Job *job,cJSON *refs,const char *scientific){
    char encoded[512],url[2048];encode(scientific,encoded,sizeof(encoded));
    snprintf(url,sizeof(url),"https://api.gbif.org/v1/species/match?name=%s&strict=true",encoded);
    cJSON *match=api(job,url);double key=cJSON_GetNumberValue(field(match,"usageKey"));
    int exact=!strcmp(str(match,"matchType"),"EXACT")&&!strcasecmp(str(match,"canonicalName"),scientific)&&(!strcmp(str(match,"rank"),"SPECIES")||!strcmp(str(match,"rank"),"SUBSPECIES"));
    cJSON_Delete(match);if(!exact||!isfinite(key)||key<=0||key>UINT32_MAX)return;
    for(int page=0;page<2&&cJSON_GetArraySize(refs)<MAX_CANDIDATES;page++){
        if(fs_cancelled(job))return;
        snprintf(url,sizeof(url),"https://api.gbif.org/v1/occurrence/search?taxonKey=%.0f&mediaType=StillImage&basisOfRecord=HUMAN_OBSERVATION&limit=100&offset=%d",key,page*100);
        cJSON *root=api(job,url);char *payload=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!payload)break;
        char *decoded=gbif_reference_media(payload,scientific,(uint32_t)key);free(payload);cJSON *media=decoded?cJSON_Parse(decoded):NULL;free(decoded);
        cJSON *r=NULL;int added=0;cJSON_ArrayForEach(r,media){if(cJSON_GetArraySize(refs)>=MAX_CANDIDATES)break;if(!has_url(refs,str(r,"url"))&&!has_url(refs,str(r,"mediaIdentifier"))){cJSON_AddItemToArray(refs,cJSON_Duplicate(r,1));added++;}}
        cJSON_Delete(media);if(added>=8)break;
    }
}
static const char *canonical_name(const char *name){
    if(!strcasecmp(name,"Felis silvestris catus")||!strcasecmp(name,"Felis domesticus"))return "Felis catus";
    return name;
}
static void collect_inaturalist(Job *job,cJSON *refs,const char *scientific){
    char encoded[512],url[2048];const char *name=!strcasecmp(scientific,"Canis lupus familiaris")?"Canis familiaris":canonical_name(scientific);encode(name,encoded,sizeof(encoded));
    snprintf(url,sizeof(url),"https://api.inaturalist.org/v1/taxa?q=%s&is_active=true&per_page=5",encoded);
    cJSON *root=api(job,url),*taxon=NULL;double id=0;
    cJSON_ArrayForEach(taxon,field(root,"results"))if(!strcasecmp(str(taxon,"name"),name)&&(!strcmp(str(taxon,"rank"),"species")||!strcmp(str(taxon,"rank"),"subspecies"))){id=cJSON_GetNumberValue(field(taxon,"id"));break;}
    cJSON_Delete(root);if(!isfinite(id)||id<=0||fs_cancelled(job))return;
    snprintf(url,sizeof(url),"https://api.inaturalist.org/v1/observations?taxon_id=%.0f&photos=true&photo_license=cc0,cc-by,cc-by-sa&per_page=30&order_by=votes&order=desc",id);
    root=api(job,url);cJSON *observation=NULL;
    cJSON_ArrayForEach(observation,field(root,"results")){
        if(cJSON_GetArraySize(refs)>=MAX_CANDIDATES||fs_cancelled(job))break;
        cJSON *observed=field(observation,"taxon"),*ancestor=NULL;int same=cJSON_GetNumberValue(field(observed,"id"))==id;
        cJSON_ArrayForEach(ancestor,field(observed,"ancestor_ids"))if(cJSON_GetNumberValue(ancestor)==id)same=1;
        if(!same)continue;cJSON *photo=NULL;int added=0;
        cJSON_ArrayForEach(photo,field(observation,"photos")){
            if(added>=2||cJSON_GetArraySize(refs)>=MAX_CANDIDATES)break;
            const char *license=str(photo,"license_code"),*source=str(photo,"url");
            if(strcmp(license,"cc0")&&strcmp(license,"cc-by")&&strcmp(license,"cc-by-sa"))continue;
            if(cJSON_IsTrue(field(photo,"hidden"))||cJSON_GetArraySize(field(photo,"flags")))continue;
            const char *host="https://inaturalist-open-data.s3.amazonaws.com/photos/";
            if(strncmp(source,host,strlen(host)))continue;
            const char *size=strstr(source,"/square.");if(!size)continue;const char *extension=size+8;
            if(strcmp(extension,"jpg")&&strcmp(extension,"jpeg")&&strcmp(extension,"png"))continue;
            char image_url[2048];int prefix=(int)(size-source);snprintf(image_url,sizeof(image_url),"%.*s/medium.%s",prefix,source,extension);
            if(has_url(refs,image_url)||!str(photo,"attribution")[0])continue;
            char page[160],title[256];snprintf(page,sizeof(page),"https://www.inaturalist.org/observations/%.0f",cJSON_GetNumberValue(field(observation,"id")));
            snprintf(title,sizeof(title),"%s / iNaturalist observation %.0f",name,cJSON_GetNumberValue(field(observation,"id")));
            cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"provider","iNaturalist");cJSON_AddNumberToObject(r,"photoId",cJSON_GetNumberValue(field(photo,"id")));
            cJSON_AddNumberToObject(r,"observationId",cJSON_GetNumberValue(field(observation,"id")));
            cJSON_AddStringToObject(r,"title",title);cJSON_AddStringToObject(r,"url",image_url);cJSON_AddStringToObject(r,"pageUrl",page);
            cJSON_AddStringToObject(r,"author",str(photo,"attribution"));cJSON_AddStringToObject(r,"license",license);
            cJSON_AddStringToObject(r,"categoryView","");cJSON_AddStringToObject(r,"searchBasis",name);cJSON_AddItemToArray(refs,r);added++;
            cJSON_AddBoolToObject(r,"taxonVerified",1);
        }
    }cJSON_Delete(root);
}
static void download_candidates(Job *job,const wchar_t *directory,cJSON *refs,cJSON *downloaded,int *cursor,int limit){
    wchar_t path[FS_PATH],rgba[FS_PATH];
    for(;*cursor<cJSON_GetArraySize(refs)&&cJSON_GetArraySize(downloaded)<limit;(*cursor)++){
        if(fs_cancelled(job))return;cJSON *r=cJSON_GetArrayItem(refs,*cursor);
        if(has_url(downloaded,str(r,"url")))continue;
        size_t size=0;char *bytes=http_get(job,str(r,"url"),&size);if(!bytes)continue;
        int index=cJSON_GetArraySize(downloaded);wchar_t name[80];fs_wformat(name,80,L"reference-%d.jpg",index);fs_join(path,directory,name);
        FILE *f=fs_wfopen(path,L"wb");int written=f&&fwrite(bytes,1,size,f)==size;if(f&&fclose(f))written=0;free(bytes);if(!written)continue;
        Photo p={0};wchar_t error[FS_ERROR]={0};if(!photo_load(path,&p,error))continue;
        fs_wformat(name,80,L"reference-%d.rgba",index);fs_join(rgba,directory,name);written=photo_write(rgba,&p,error);photo_free(&p);if(!written)continue;
        cJSON *item=cJSON_Duplicate(r,1);char filename[80];snprintf(filename,sizeof(filename),"reference-%d.rgba",index);cJSON_AddStringToObject(item,"rgba",filename);
        snprintf(filename,sizeof(filename),"reference-%d.jpg",index);cJSON_AddStringToObject(item,"file",filename);cJSON_AddItemToArray(downloaded,item);
    }
}
static int context_usable(const cJSON *context){
    if(!cJSON_IsObject(context))return 0;
    const char *medium=str(field(context,"medium"),"label"),*framing=str(field(context,"framing"),"label");
    if(!*medium||!*framing)return 0;
    return strcmp(medium,"art")&&strcmp(medium,"object")&&strcmp(framing,"head")&&strcmp(framing,"detail")&&strcmp(framing,"multiple");
}
static int usable_count(const wchar_t *directory,const wchar_t *analysis_path){
    cJSON *data=read_json(analysis_path),*item=NULL;int n=0;
    cJSON_ArrayForEach(item,data){
        const char *file=str(item,"mask");if(!context_usable(field(item,"context"))||!*file||strchr(file,'/')||strchr(file,'\\'))continue;
        wchar_t path[FS_PATH],name[256],error[FS_ERROR]={0};Photo p={0};
        if(!fs_decode_utf8(CP_UTF8,0,file,-1,name,256)||!fs_join(path,directory,name)||!photo_read(path,&p,error))continue;
        int foreground=0;for(size_t i=0;i<(size_t)p.width*p.height&&foreground<100;i++)if(p.rgba[i*4+3]>=230)foreground++;
        photo_free(&p);if(foreground>=100)n++;
    }
    cJSON_Delete(data);return n;
}
int references_collect(Job *job,const wchar_t *directory,const wchar_t *photo){
    wchar_t candidates_path[FS_PATH],manifest[FS_PATH],analysis[FS_PATH],log[FS_PATH],path[FS_PATH],rgba[FS_PATH];
    cJSON *candidates=NULL,*resolved=NULL,*refs=cJSON_CreateArray(),*downloaded=cJSON_CreateArray();int ok=0;
    fs_join(candidates_path,directory,L"species-candidates.json");fs_join(log,directory,L"inference.log");
    fs_join(manifest,directory,L"references.json");fs_join(analysis,directory,L"reference-analysis.json");
    fs_status(job,L"BioCLIP으로 동물 종을 자동 판별하는 중");
    if(!fs_python(job,L"classify",photo,candidates_path,log))goto done;
    candidates=read_json(candidates_path);if(!cJSON_IsArray(candidates))goto done;
    fs_status(job,L"C에서 Wikidata 학명·별칭을 확인하는 중");
    cJSON *groups=cJSON_CreateArray();
    for(int i=0;i<cJSON_GetArraySize(candidates)&&i<8;i++){
        if(fs_cancelled(job)){cJSON_Delete(groups);goto done;}
        cJSON *candidate=cJSON_GetArrayItem(candidates,i);
        const char *name=contains(str(candidate,"common"),"domestic dog")?"Canis lupus familiaris":str(candidate,"scientific");
        cJSON *r=taxon(job,name);
        if(!str(candidate,"scientific")[0])continue;
        if(!r){
            r=cJSON_CreateObject();cJSON_AddStringToObject(r,"scientific",name);
            cJSON_AddStringToObject(r,"qid","");cJSON_AddStringToObject(r,"category",name);
            cJSON_AddBoolToObject(r,"wikidataUnavailable",1);
        }
        cJSON_AddStringToObject(r,"common",str(candidate,"common"));
        if(!strcasecmp(canonical_name(str(r,"scientific")),"Felis catus")){
            cJSON_ReplaceItemInObjectCaseSensitive(r,"scientific",cJSON_CreateString("Felis catus"));
            cJSON_ReplaceItemInObjectCaseSensitive(r,"qid",cJSON_CreateString("Q146"));
            cJSON_ReplaceItemInObjectCaseSensitive(r,"category",cJSON_CreateString("Cats"));
        }
        cJSON *same=NULL,*g=NULL;cJSON_ArrayForEach(g,groups)if(!strcmp(str(g,"scientific"),str(r,"scientific")))same=g;
        double score=cJSON_GetNumberValue(field(candidate,"score"));
        if(same){cJSON_SetNumberValue(field(same,"score"),cJSON_GetNumberValue(field(same,"score"))+score);cJSON_Delete(r);}
        else{cJSON_AddNumberToObject(r,"score",score);cJSON_AddItemToArray(groups,r);}
        if(cJSON_GetArraySize(groups)>=3)break;
    }
    cJSON *best=NULL,*g=NULL;cJSON_ArrayForEach(g,groups)if(!best||cJSON_GetNumberValue(field(g,"score"))>cJSON_GetNumberValue(field(best,"score")))best=g;
    resolved=cJSON_Duplicate(best,1);cJSON_Delete(groups);
    if(!resolved){fs_error(job->error,L"동물 학명을 확인하지 못했습니다. 인터넷 연결과 입력 사진을 확인하세요.");goto done;}
    fs_decode_utf8(CP_UTF8,0,str(resolved,"scientific"),-1,job->species,128);
    wchar_t status[256];fs_wformat(status,256,L"%s · C에서 같은 종의 참고 사진을 수집하는 중",job->species);fs_status(job,status);
    fs_join(path,directory,L"species.json");if(!write_json(path,resolved))goto done;
    wchar_t cutout_path[FS_PATH],context_path[FS_PATH];fs_join(cutout_path,directory,L"cutout.rgba");fs_join(context_path,directory,L"input-analysis.json");
    if(!fs_python(job,L"context",cutout_path,context_path,log)){
        if(fs_cancelled(job))goto done;job->error[0]=0;fs_status(job,L"자세 판독이 어려워 원본 중심으로 진행합니다.");
    }
    char restriction[1024],query[2048];const char *subject=str(resolved,"category"),*qid=str(resolved,"qid");
    if(!strcasecmp(str(resolved,"scientific"),"Felis catus")||!strcasecmp(str(resolved,"scientific"),"Felis silvestris catus")||!strcasecmp(str(resolved,"scientific"),"Felis domesticus")){subject="cats";qid="Q146";}
    if(contains(str(resolved,"common"),"domestic dog")){subject="dogs";qid="";}
    if(*qid)snprintf(restriction,sizeof(restriction),"haswbstatement:P180=%s",qid);
    else snprintf(restriction,sizeof(restriction),"\"%s\"",str(resolved,"scientific"));
    fs_join(path,directory,L"input-analysis.json");cJSON *input_context=read_json(path);
    const char *pose=str(field(input_context,"pose"),"label");
    if(!*pose||!strcmp(pose,"uncertain")){
        const char *caption=str(input_context,"caption");
        if(contains(caption,"lying")||contains(caption,"laying")||contains(caption,"loaf"))pose="lying";
        else if(contains(caption,"sitting"))pose="resting";
        else if(contains(caption,"standing"))pose="standing";
    }
    if(input_context){cJSON_AddStringToObject(input_context,"resolvedPose",pose);if(!write_json(path,input_context)){cJSON_Delete(input_context);goto done;}}
    const cJSON *breed=field(input_context,"breed");const char *breed_name=str(breed,"label");
    if(*breed_name&&strcmp(breed_name,"uncertain")&&strcmp(breed_name,"mixed breed dog")){
        /* Same species is insufficient when breeds have very different legs. */
        if(contains(breed_name,"corgi")){collect_query(job,refs,"\"Pembroke Welsh Corgi\" -puppy -puppies -crossbreed -mix","",5);}
        snprintf(query,sizeof(query),"\"%s\" %s",breed_name,(!strcmp(pose,"standing")||!strcmp(pose,"sitting")||!strcmp(pose,"lying"))?pose:"");collect_query(job,refs,query,"",4);
        snprintf(query,sizeof(query),"\"%s\" profile",breed_name);collect_query(job,refs,query,"side",4);
        snprintf(query,sizeof(query),"\"%s\"",breed_name);collect_query(job,refs,query,"",5);
        cJSON_AddStringToObject(resolved,"breedSearchHint",breed_name);fs_join(path,directory,L"species.json");write_json(path,resolved);
    }
    const char *pose_term=!strcmp(pose,"lying")?"lying":(!strcmp(pose,"sitting")?"sitting":(!strcmp(pose,"standing")?"standing":(!strcmp(pose,"curled")?"curled":"")));
    if(!strcmp(pose,"resting"))pose_term="lying";
    if(*pose_term&&!job->shape_disabled){
        snprintf(query,sizeof(query),"%s %s",restriction,pose_term);collect_query(job,refs,query,"",8);
        snprintf(query,sizeof(query),"\"%s\" %s",str(resolved,"scientific"),pose_term);collect_query(job,refs,query,"",4);
        if(!strcmp(pose,"lying")||!strcmp(pose,"resting")){snprintf(query,sizeof(query),"%s loaf",restriction);collect_query(job,refs,query,"",4);}
    }
    cJSON_Delete(input_context);
    const char *prefix[]={"Side","Rear","Top"},*views[]={"side","rear","top"},*terms[]={"profile","rear","above"};
    for(int i=0;i<3;i++){
        if(fs_cancelled(job))goto done;
        int before=cJSON_GetArraySize(refs);snprintf(query,sizeof(query),"incategory:\"%s views of %s\"",prefix[i],subject);collect_query(job,refs,query,views[i],3);
        if(cJSON_GetArraySize(refs)==before){snprintf(query,sizeof(query),"%s %s",restriction,terms[i]);collect_query(job,refs,query,"",3);}
    }
    collect_query(job,refs,restriction,"",MAX_REFS);
    if(cJSON_GetArraySize(refs)<MAX_REFS){snprintf(query,sizeof(query),"incategory:\"%s\"",str(resolved,"category"));collect_query(job,refs,query,"",MAX_REFS);}
    if(cJSON_GetArraySize(refs)<MAX_REFS){snprintf(query,sizeof(query),"\"%s\"",str(resolved,"scientific"));collect_query(job,refs,query,"",MAX_REFS);}
    if(cJSON_GetArraySize(refs)<MAX_REFS&&str(resolved,"common")[0]){snprintf(query,sizeof(query),"\"%s\"",str(resolved,"common"));collect_query(job,refs,query,"",MAX_REFS);}
    int cursor=0;download_candidates(job,directory,refs,downloaded,&cursor,12);
    if(cJSON_GetArraySize(downloaded)<2){
        /* A small attributed example cache can be shipped with source for demos. */
        wchar_t seed_root[FS_PATH],seed[FS_PATH],qid_w[80];fs_decode_utf8(CP_UTF8,0,str(resolved,"qid"),-1,qid_w,80);
        fs_join(seed_root,job->root,L"assets\\reference-cache");fs_join(seed,seed_root,qid_w);fs_join(path,seed,L"references.json");cJSON *saved=read_json(path);
        if(cJSON_IsArray(saved)){
            fs_status(job,L"참고 사이트 응답 제한 · 출처가 보관된 예제 참고 사진을 사용합니다.");
            for(int i=0;i<cJSON_GetArraySize(saved)&&cJSON_GetArraySize(downloaded)<MAX_REFS;i++){
                cJSON *r=cJSON_GetArrayItem(saved,i);const char *file=str(r,"cacheFile");if(!*file||strchr(file,'/')||strchr(file,'\\'))continue;
                if(has_url(downloaded,str(r,"url")))continue;
                wchar_t filename[128];if(!fs_decode_utf8(CP_UTF8,0,file,-1,filename,128))continue;fs_join(path,seed,filename);
                int index=cJSON_GetArraySize(downloaded);wchar_t dest[FS_PATH],name[80];fs_wformat(name,80,L"reference-%d.png",index);fs_join(dest,directory,name);
                if(!fs_copyfile(path,dest,0))continue;Photo p={0};wchar_t error[FS_ERROR]={0};if(!photo_load(dest,&p,error))continue;
                fs_wformat(name,80,L"reference-%d.rgba",index);fs_join(rgba,directory,name);int written=photo_write(rgba,&p,error);photo_free(&p);if(!written)continue;
                cJSON *item=cJSON_Duplicate(r,1);char filename8[80];snprintf(filename8,sizeof(filename8),"reference-%d.rgba",index);cJSON_AddStringToObject(item,"rgba",filename8);
                snprintf(filename8,sizeof(filename8),"reference-%d.png",index);cJSON_AddStringToObject(item,"file",filename8);cJSON_AddBoolToObject(item,"fromExampleCache",1);cJSON_AddItemToArray(downloaded,item);
            }
        }cJSON_Delete(saved);
    }
    for(int round=0;round<3;round++){
        if(fs_cancelled(job)||!write_json(manifest,downloaded))goto done;
        if(cJSON_GetArraySize(downloaded)){
            if(!fs_python(job,L"analyse",manifest,analysis,log)){
                if(fs_cancelled(job))goto done;job->error[0]=0;
                fs_status(job,L"참고 사진을 분석하지 못해 입력 사진 중심으로 계속합니다.");
                cJSON *empty=cJSON_CreateArray();int written=write_json(analysis,empty);cJSON_Delete(empty);if(!written)goto done;break;
            }
        }else{cJSON *empty=cJSON_CreateArray();int written=write_json(analysis,empty);cJSON_Delete(empty);if(!written)goto done;}
        if((usable_count(directory,analysis)>=4&&(job->shape_disabled||shape_candidate_count(directory)>=3))||round==2)break;
        fs_status(job,round?L"GBIF에서 같은 분류의 다른 각도 사진을 추가 검색합니다.":L"적합한 참고가 부족해 iNaturalist에서 같은 종 사진을 추가 검색합니다.");
        int supplemental=cJSON_GetArraySize(refs);if(round)collect_gbif(job,refs,str(resolved,"scientific"));else collect_inaturalist(job,refs,str(resolved,"scientific"));
        int before=cJSON_GetArraySize(downloaded),supplemental_cursor=supplemental;
        download_candidates(job,directory,refs,downloaded,&supplemental_cursor,(before+8<MAX_REFS)?before+8:MAX_REFS);
        download_candidates(job,directory,refs,downloaded,&cursor,round?MAX_REFS:18);
        if(cJSON_GetArraySize(downloaded)==before&&round)break;
    }
    if(fs_cancelled(job))goto done;
    if(usable_count(directory,analysis)<2)fs_status(job,L"참고 자료가 제한적입니다. 입력 사진의 형상을 유지하며 생성을 계속합니다.");
    ok=1;
done:
    cJSON_Delete(candidates);cJSON_Delete(resolved);cJSON_Delete(refs);cJSON_Delete(downloaded);
    if(!ok&&!job->error[0])fs_error(job->error,L"참고 사진 준비에 실패했습니다. 작업 기록을 확인하세요.");return ok;
}

typedef struct {double score,mean[3],sum[512][3];uint32_t bins[512];int valid,selected;const char *view;} Measurement;
int references_apply(Job *job,const wchar_t *directory,Mesh *mesh){
    wchar_t path[FS_PATH],analysis_path[FS_PATH],mask[FS_PATH],name[256];
    fs_join(path,directory,L"references.json");fs_join(analysis_path,directory,L"reference-analysis.json");
    cJSON *refs=read_json(path),*analyses=read_json(analysis_path);Measurement *items=calloc(MAX_REFS,sizeof(*items));int ok=0;
    if(!refs||!analyses||!items)goto done;
    double original[3]={0};for(uint32_t i=0;i<mesh->vertex_count;i++)for(int k=0;k<3;k++)original[k]+=mesh->vertices[i].color[k]/(255.*mesh->vertex_count);
    int n=cJSON_GetArraySize(refs);if(n>MAX_REFS)n=MAX_REFS;
    for(int i=0;i<n;i++){
        cJSON *analysis=cJSON_GetArrayItem(analyses,i),*context=field(analysis,"context"),*r=cJSON_GetArrayItem(refs,i);
        if(!context)continue;
        cJSON_DeleteItemFromObjectCaseSensitive(r,"photoContext");
        cJSON_AddItemToObject(r,"photoContext",cJSON_Duplicate(context,1));
        if(!context_usable(context))continue;
        const char *file=str(analysis,"mask");if(!file[0]||strchr(file,'/')||strchr(file,'\\')||!fs_decode_utf8(CP_UTF8,0,file,-1,name,256))continue;
        fs_join(mask,directory,name);Photo p={0};wchar_t error[FS_ERROR]={0};if(!photo_read(mask,&p,error))continue;
        unsigned total=0;
        for(size_t pixel=0;pixel<(size_t)p.width*p.height;pixel++){
            const unsigned char *c=p.rgba+pixel*4;if(c[3]<230)continue;
            unsigned bin=(c[0]/32)*64+(c[1]/32)*8+c[2]/32;items[i].bins[bin]++;total++;
            for(int k=0;k<3;k++){items[i].sum[bin][k]+=c[k]/255.;items[i].mean[k]+=c[k]/255.;}
        }
        photo_free(&p);if(total<100)continue;
        for(int k=0;k<3;k++){items[i].mean[k]/=total;items[i].score+=pow(items[i].mean[k]-original[k],2);}
        items[i].view=str(r,"categoryView");if(!items[i].view[0])items[i].view=str(field(context,"view"),"label");
        items[i].valid=1;
    }
    const char *wanted[]={"side","rear","top","any","any","any","any"};int selected=0;
    for(int pass=0;pass<7&&selected<4;pass++){
        int best=-1;for(int i=0;i<n;i++)if(items[i].valid&&!items[i].selected&&(!strcmp(wanted[pass],"any")||!strcmp(items[i].view,wanted[pass])))
            if(best<0||items[i].score<items[best].score)best=i;
        if(best>=0){items[best].selected=1;selected++;}
    }
    if(selected<2)fs_status(job,selected?L"참고 1장만 적합하여 색상 반영을 줄이고 생성을 계속합니다.":L"적합한 참고가 없어 입력 사진의 형상·색상으로 생성을 계속합니다.");
    double sums[512][3]={{0}};uint32_t bins[512]={0};float palette[16][3];int palette_count=0;
    for(int i=0;i<n;i++){
        cJSON *r=cJSON_GetArrayItem(refs,i);cJSON_DeleteItemFromObjectCaseSensitive(r,"selected");cJSON_DeleteItemFromObjectCaseSensitive(r,"use");cJSON_AddBoolToObject(r,"selected",items[i].selected);
        cJSON_AddStringToObject(r,"use",items[i].selected?"rear colour palette only; source front colours retained":"not used");
        if(items[i].selected)for(int b=0;b<512;b++){bins[b]+=items[i].bins[b];for(int k=0;k<3;k++)sums[b][k]+=items[i].sum[b][k];}
    }
    for(int i=0;i<16;i++){
        int best=0;for(int b=1;b<512;b++)if(bins[b]>bins[best])best=b;if(!bins[best])break;
        for(int k=0;k<3;k++)palette[palette_count][k]=(float)(sums[best][k]/bins[best]);bins[best]=0;palette_count++;
    }
    float mesh_lo[3],mesh_hi[3];mesh_bounds(mesh,mesh_lo,mesh_hi);float rear_start=(mesh_lo[2]+mesh_hi[2])*.5f;
    for(uint32_t i=0;i<mesh->vertex_count;i++){
        if(mesh->vertices[i].p[2]<=rear_start||mesh->vertices[i].n[2]<-.05f)continue;
        unsigned char *c=mesh->vertices[i].color;int best=0;double distance=1e9;
        for(int p=0;p<palette_count;p++){double d=0;for(int k=0;k<3;k++)d+=pow(c[k]/255.-palette[p][k],2);if(d<distance){distance=d;best=p;}}
        double blend=selected==1?.075:.15;
        if(distance<.16)for(int k=0;k<3;k++)c[k]=(unsigned char)((1-blend)*c[k]+blend*255*palette[best][k]+.5);
    }
    if(!write_json(path,refs))goto done;job->reference_count=selected;ok=1;
done:
    free(items);cJSON_Delete(refs);cJSON_Delete(analyses);if(!ok&&!job->error[0])fs_error(job->error,L"참고 사진 색상 처리에 실패했습니다.");return ok;
}
