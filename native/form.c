/* Faceted form style. Eye landmarks locate paint to REMOVE, never to draw.
 * TripoSR retains the ears, forehead, muzzle, nose, mouth and jaw volumes.
 */
#include "fauna.h"
#include "../vendor/cjson/cJSON.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define form_open _wfopen
#else
#define form_open fs_wfopen
#endif

typedef struct {float x,y,rx,ry,colour[3];} Eye;
static float clampf(float x,float a,float b){return fmaxf(a,fminf(b,x));}
static cJSON *field(const cJSON *j,const char *key){return cJSON_GetObjectItemCaseSensitive(j,key);}
static cJSON *analysis(const wchar_t *directory){
    wchar_t path[FS_PATH];if(!directory||!directory[0]||!fs_join(path,directory,L"input-analysis.json"))return NULL;
    FILE *f=form_open(path,L"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    if(n<1||n>2000000){fclose(f);return NULL;}char *s=calloc((size_t)n+1,1);if(!s){fclose(f);return NULL;}
    size_t got=fread(s,1,(size_t)n,f);fclose(f);cJSON *j=got==(size_t)n?cJSON_Parse(s):NULL;free(s);return j;
}
static int landmark(const cJSON *points,int index,float p[2]){
    const cJSON *v=cJSON_GetArrayItem(points,index);double x=cJSON_GetNumberValue(field(v,"x")),y=cJSON_GetNumberValue(field(v,"y")),score=cJSON_GetNumberValue(field(v,"score"));
    if(!isfinite(x)||!isfinite(y)||!isfinite(score)||score<.4||x<0||y<0||x>1||y>1||cJSON_IsFalse(field(v,"maskSupport")))return 0;
    p[0]=(float)x;p[1]=(float)y;return 1;
}
static float eye_distance(const Eye *e,float x,float y){float dx=(x-e->x)/e->rx,dy=(y-e->y)/e->ry;return sqrtf(dx*dx+dy*dy);}
static int byte_order(const void *a,const void *b){return (int)*(const unsigned char*)a-(int)*(const unsigned char*)b;}
static int locate_eyes(const cJSON *context,const Photo *p,Eye eyes[2]){
    const cJSON *points=field(field(context,"animalPose"),"points");float xy[3][2];int valid[3];
    for(int k=0;k<3;k++){valid[k]=landmark(points,k,xy[k]);if(valid[k]){xy[k][0]*=p->width;xy[k][1]*=p->height;}}
    float spacing=valid[0]&&valid[1]?hypotf(xy[0][0]-xy[1][0],xy[0][1]-xy[1][1]):0;
    int count=0;for(int k=0;k<2;k++)if(valid[k]){
        float size=spacing;if(size<3&&valid[2])size=.75f*hypotf(xy[k][0]-xy[2][0],xy[k][1]-xy[2][1]);
        if(size<3||size>fmaxf((float)p->width,(float)p->height)*.5f)continue;
        eyes[count++]=(Eye){xy[k][0],xy[k][1],fmaxf(3,size*.45f),fmaxf(3,size*.33f),{0}};
    }
    if(!count){
        const cJSON *box=field(context,"faceBox");float b[4];int valid_box=cJSON_GetArraySize(box)==4;
        for(int k=0;k<4&&valid_box;k++){double x=cJSON_GetNumberValue(cJSON_GetArrayItem(box,k));if(!isfinite(x)||x<0||x>1)valid_box=0;else b[k]=(float)x;}
        if(valid_box&&b[2]>.025f&&b[3]>.025f&&b[0]+b[2]<=1.01f&&b[1]+b[3]<=1.01f){
            for(int k=0;k<2;k++)eyes[count++]=(Eye){(b[0]+b[2]*(k?.7f:.3f))*p->width,(b[1]+b[3]*.46f)*p->height,b[2]*p->width*.23f,b[3]*p->height*.16f,{0}};
        }
    }
    /* Robust surrounding-coat median. Neither the eye nor a second eye can
     * contribute samples, so dark irises cannot be repainted after reduction. */
    for(int e=0;e<count;e++){
        unsigned char samples[3][2048];int n=0;Eye *eye=&eyes[e];
        for(int yy=-22;yy<=22;yy++)for(int xx=-22;xx<=22;xx++){
            float x=eye->x+eye->rx*xx/8.f,y=eye->y+eye->ry*yy/8.f;
            float d=eye_distance(eye,x,y);if(d<1.7f||d>2.6f||x<0||y<0||x>=p->width||y>=p->height)continue;
            int excluded=0;for(int k=0;k<count;k++)if(eye_distance(&eyes[k],x,y)<1.7f)excluded=1;
            if(valid[2]&&hypotf(x-xy[2][0],y-xy[2][1])<eye->rx*.9f)excluded=1;if(excluded)continue;
            const unsigned char *c=p->rgba+((size_t)(uint32_t)y*p->width+(uint32_t)x)*4;if(c[3]<220)continue;
            if(n<2048){for(int k=0;k<3;k++)samples[k][n]=c[k];n++;}
        }
        if(n<8)return 0;
        for(int k=0;k<3;k++){qsort(samples[k],(size_t)n,1,byte_order);eye->colour[k]=samples[k][n/2];}
    }
    return count;
}

int mesh_form_prepare(Job *j,const wchar_t *directory,const Photo *photo,const Mesh *source,Mesh *out){
    memset(out,0,sizeof(*out));j->eye_regions=0;j->eye_colour_vertices=0;j->eye_colour_fallback=0;
    if(fs_cancelled(j)||!mesh_validate(source,j->error)||!mesh_copy(source,out))return 0;
    Eye eyes[2];int count=0;float crop[4]={0,0,1,1};
    if(photo&&photo->rgba&&photo->width>3&&photo->height>3){
        cJSON *context=analysis(directory);count=locate_eyes(context,photo,eyes);cJSON_Delete(context);
        crop[0]=(float)photo->width;crop[1]=(float)photo->height;crop[2]=crop[3]=0;
        for(uint32_t y=0;y<photo->height;y++)for(uint32_t x=0;x<photo->width;x++)if(photo->rgba[((size_t)y*photo->width+x)*4+3]>128){
            crop[0]=fminf(crop[0],(float)x);crop[1]=fminf(crop[1],(float)y);crop[2]=fmaxf(crop[2],(float)x);crop[3]=fmaxf(crop[3],(float)y);}
        if(crop[2]<=crop[0]||crop[3]<=crop[1])count=0;
    }
    float lo[3],hi[3];mesh_bounds(source,lo,hi);double mean[3]={0};
    for(uint32_t i=0;i<source->vertex_count;i++)for(int c=0;c<3;c++)mean[c]+=source->vertices[i].color[c]/(double)source->vertex_count;
    if(!count){j->eye_colour_fallback=1;fs_status(j,L"얼굴 위치가 불확실해 눈 무늬 없는 단색 로우폴리로 정리합니다.");}
    for(uint32_t i=0;i<out->vertex_count;i++){
        if(!(i%4096)&&fs_cancelled(j)){mesh_free(out);return 0;}Vertex *v=&out->vertices[i];
        if(!count){for(int c=0;c<3;c++)v->color[c]=(unsigned char)(mean[c]+.5);}
        else{
            float x=crop[0]+(hi[0]-v->p[0])/fmaxf(hi[0]-lo[0],1e-8f)*(crop[2]-crop[0]);
            float y=crop[1]+(hi[1]-v->p[1])/fmaxf(hi[1]-lo[1],1e-8f)*(crop[3]-crop[1]);
            if(v->face>.01f&&v->observed){x=v->source_uv[0]*(photo->width-1);y=v->source_uv[1]*(photo->height-1);}
            float best=0;int selected=0;
            for(int e=0;e<count;e++){float d=eye_distance(&eyes[e],x,y),w=clampf((1.65f-d)/.55f,0,1);w=w*w*(3-2*w);if(w>best){best=w;selected=e;}}
            if(best>0){for(int c=0;c<3;c++)v->color[c]=(unsigned char)(v->color[c]*(1-best)+eyes[selected].colour[c]*best+.5f);j->eye_colour_vertices++;}
        }
        /* Shape-only reduction: do not keep tiny image-space edges for eyes,
         * invent iris vertices, or interpolate smooth face normals. */
        v->face=v->smooth=v->detail=0;v->observed=0;
        v->limb=fminf(v->limb,.2f);
        if(v->p[1]<lo[1]+.2f*(hi[1]-lo[1]))v->limb=fmaxf(v->limb,.15f);
    }
    j->eye_regions=count;return 1;
}

int mesh_form_model(Job *j,const wchar_t *directory,const Photo *photo,const Mesh *source,Mesh *out){
    Mesh prepared={0};if(!mesh_form_prepare(j,directory,photo,source,&prepared))return 0;
    int ok=mesh_form_surface(&prepared,out,j);mesh_free(&prepared);return ok;
}
