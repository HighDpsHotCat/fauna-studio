/* GBIF media licences are independent of occurrence-data licences. */
#include "fauna.h"
#include "../vendor/cjson/cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
static const char *s(const cJSON *o,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
static double n(const cJSON *o,const char *k){return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o,k));}
static int md5(const char *text,char hex[33]){return fs_hash_hex(text,strlen(text),1,hex);}

static const char *media_license(const char *url){
    const char *p=NULL;
    if(!strncmp(url,"https://creativecommons.org/",28))p=url+28;
    else if(!strncmp(url,"http://creativecommons.org/",27))p=url+27;
    if(!p)return NULL;
    if(!strcmp(p,"publicdomain/zero/1.0/"))return "CC0 1.0";
    if(!strncmp(p,"licenses/by/",12))return "CC BY";
    if(!strncmp(p,"licenses/by-sa/",15))return "CC BY-SA";
    return NULL;
}
char *gbif_reference_media(const char *payload,const char *scientific,uint32_t taxon_key){
    cJSON *root=cJSON_Parse(payload),*out=cJSON_CreateArray();const cJSON *record=NULL;
    cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(root,"results")){
        if(cJSON_GetArraySize(out)>=16)break;
        if(strcmp(s(record,"basisOfRecord"),"HUMAN_OBSERVATION")&&strcmp(s(record,"basisOfRecord"),"MACHINE_OBSERVATION"))continue;
        if(n(record,"taxonKey")!=taxon_key&&n(record,"acceptedTaxonKey")!=taxon_key&&n(record,"speciesKey")!=taxon_key)continue;
        double key=n(record,"key");if(!isfinite(key)||key<1||key>9007199254740991.)continue;
        const cJSON *media=NULL;int used=0;
        cJSON_ArrayForEach(media,cJSON_GetObjectItemCaseSensitive(record,"media")){
            if(used>=2||cJSON_GetArraySize(out)>=16)break;
            const char *license=media_license(s(media,"license")),*identifier=s(media,"identifier"),*author=s(media,"creator");
            if(!*author)author=s(media,"rightsHolder");
            if(!license||!*author||strcmp(s(media,"type"),"StillImage")||
               (strcmp(s(media,"format"),"image/jpeg")&&strcmp(s(media,"format"),"image/png"))||
               (strncmp(identifier,"https://",8)&&strncmp(identifier,"http://",7))||strlen(identifier)>4096)continue;
            int duplicate=0;const cJSON *previous=NULL;cJSON_ArrayForEach(previous,out)if(!strcmp(identifier,s(previous,"mediaIdentifier")))duplicate=1;
            char hash[33];if(duplicate||!md5(identifier,hash))continue;
            char url[256],page[128],title[256];snprintf(url,sizeof(url),"https://api.gbif.org/v1/image/cache/640x/occurrence/%.0f/media/%s",key,hash);
            snprintf(page,sizeof(page),"https://www.gbif.org/occurrence/%.0f",key);snprintf(title,sizeof(title),"%s / GBIF occurrence %.0f",scientific,key);
            cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"provider","GBIF");cJSON_AddStringToObject(r,"title",title);
            cJSON_AddStringToObject(r,"url",url);cJSON_AddStringToObject(r,"pageUrl",page);cJSON_AddStringToObject(r,"author",author);
            cJSON_AddStringToObject(r,"license",license);cJSON_AddStringToObject(r,"licenseUrl",s(media,"license"));cJSON_AddStringToObject(r,"mediaIdentifier",identifier);
            cJSON_AddStringToObject(r,"datasetKey",s(record,"datasetKey"));cJSON_AddNumberToObject(r,"occurrenceKey",key);
            cJSON_AddNumberToObject(r,"taxonKey",taxon_key);cJSON_AddBoolToObject(r,"taxonVerified",1);
            cJSON_AddStringToObject(r,"categoryView","");cJSON_AddStringToObject(r,"searchBasis",scientific);cJSON_AddItemToArray(out,r);used++;
        }
    }
    char *result=cJSON_PrintUnformatted(out);cJSON_Delete(root);cJSON_Delete(out);return result;
}
