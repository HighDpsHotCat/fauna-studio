/* Retrieval-assisted reconstruction in C17. Different animals are shape priors,
 * never registered photographs of the same individual. TripoSR stays unchanged.
 * A pose-compatible reference supplies a body; source photo/face proportions,
 * distinct lower branches and palette guide C processing. Hidden anatomy is
 * still an estimate, not registered multiview reconstruction.
 */
#include "fauna.h"
#include "../vendor/cjson/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define GRID 128
#define PIXELS (GRID*GRID)
typedef struct { float near_z[PIXELS],far_z[PIXELS]; } Projection;
static const char *sfield(const cJSON *o,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static const cJSON *field(const cJSON *o,const char *key){return cJSON_GetObjectItemCaseSensitive(o,key);}
static double number(const cJSON *o,const char *key){double v=cJSON_GetNumberValue(field(o,key));return isfinite(v)?v:0;}
static cJSON *read_json(const wchar_t *path){
    FILE *f=fs_wfopen(path,L"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    if(n<=0||n>20000000){fclose(f);return NULL;}char *s=malloc((size_t)n+1);if(!s){fclose(f);return NULL;}
    size_t got=fread(s,1,n,f);fclose(f);s[got]=0;cJSON *r=got==(size_t)n?cJSON_Parse(s):NULL;free(s);return r;
}
static int write_json(const wchar_t *path,const cJSON *data){
    char *s=cJSON_Print(data);if(!s)return 0;FILE *f=fs_wfopen(path,L"wb");int ok=0;
    if(f){size_t n=strlen(s);ok=fwrite(s,1,n,f)==n;if(fclose(f))ok=0;}free(s);return ok;
}
static int stopped(FsCancel * cancel,wchar_t *error){if(cancel&&fs_cancel_wait(cancel,0)==WAIT_OBJECT_0){fs_error(error,L"생성을 취소했습니다.");return 1;}return 0;}
static float clamp(float x,float a,float b){return fmaxf(a,fminf(b,x));}
static const char *canonical(const char *s){return !strcasecmp(s,"Felis domesticus")||!strcasecmp(s,"Felis silvestris catus")?"Felis catus":s;}
static double candidate_score(const cJSON *source,const cJSON *analysis,const cJSON *reference,const char *species){
    const cJSON *context=field(analysis,"context");const char *pose=sfield(source,"resolvedPose");if(!*pose)pose=sfield(field(source,"pose"),"label");
    const char *breed=sfield(field(source,"breed"),"label");
    if(*breed&&strcmp(breed,"uncertain")&&strcmp(breed,"mixed breed dog")){
        const cJSON *b=field(context,"breed");
        if(cJSON_IsTrue(field(b,"confident"))&&strcmp(sfield(b,"label"),breed))return 0;
        if(number(field(b,"scores"),breed)<.08)return 0;
    }
    if(!*pose||!strcmp(pose,"uncertain")||!strcmp(pose,"moving")||!strcmp(pose,"supine"))return 0;
    const char *p=sfield(field(context,"pose"),"bestCandidate"),*caption=sfield(context,"caption");
    int standing_caption=strstr(caption,"standing")&&!strstr(caption,"hinds")&&!strstr(caption,"hind legs")&&!strstr(caption,"rear legs")&&!strstr(caption,"two legs");
    if(standing_caption&&!strcmp(pose,"standing"))p="standing";
    if(!strcmp(p,"supine")||!strcmp(p,"moving"))return 0;
    int resting=!strcmp(pose,"resting"),lying_caption=strstr(caption,"lying")||strstr(caption,"laying")||strstr(caption,"loaf");
    if(strstr(caption,"on its back")||strstr(caption,"upside down"))return 0;
    if(resting){if(strcmp(p,"lying")&&strcmp(p,"sitting")&&!lying_caption)return 0;}
    else if(strcmp(pose,p)&&!(lying_caption&&!strcmp(pose,"lying")))return 0;
    double match=resting?number(field(field(context,"pose"),"scores"),"lying")+number(field(field(context,"pose"),"scores"),"sitting"):number(field(field(context,"pose"),"scores"),pose);
    if(lying_caption&&(!strcmp(pose,"lying")||resting))match=fmax(match,.55);
    if(standing_caption&&!strcmp(pose,"standing"))match=fmax(match,.55);
    double photo=number(field(field(context,"medium"),"scores"),"photo");
    double whole=number(field(field(context,"framing"),"scores"),"whole");
    const char *framing=sfield(field(context,"framing"),"bestCandidate");
    if(match<.38||photo<.5||whole<.3||!strcmp(framing,"head")||!strcmp(framing,"detail")||!strcmp(framing,"multiple")||(!strcmp(framing,"occluded")&&whole<.5))return 0;
    double same=0,other=0;const cJSON *item=NULL;
    cJSON_ArrayForEach(item,field(analysis,"speciesCandidates")){
        if((!strcasecmp(species,"Canis lupus familiaris")&&(!strcasecmp(sfield(item,"common"),"domestic dog")||!strcasecmp(sfield(item,"scientific"),"Canis familiaris")))||!strcasecmp(canonical(sfield(item,"scientific")),canonical(species)))same+=number(item,"score");
        else other=fmax(other,number(item,"score"));
    }
    int verified=cJSON_IsTrue(field(reference,"taxonVerified"))||strstr(sfield(reference,"searchBasis"),"haswbstatement:P180=")!=NULL;
    if(same<.25||(!verified&&same<other))return 0;
    double side=number(field(field(context,"view"),"scores"),"side"),oblique=number(field(field(context,"view"),"scores"),"oblique");
    const cJSON *limbs=field(field(context,"limb_visibility"),"scores");
    double legibility=.12*number(limbs,"clear")-.08*number(limbs,"hidden");
    int face_match=cJSON_GetArraySize(field(source,"faceBox"))==4&&cJSON_GetArraySize(field(context,"faceBox"))==4;
    return fmax(.001,.5*match+.2*whole+.15*same+.15*fmin(1,side+oblique)+legibility+.2*face_match);
}
static int read_pose(const cJSON *context,AnimalPose *pose){
    const cJSON *points=field(field(context,"animalPose"),"points");memset(pose,0,sizeof(*pose));if(cJSON_GetArraySize(points)!=17)return 0;
    for(int i=0;i<17;i++){const cJSON *p=cJSON_GetArrayItem(points,i);double x=number(p,"x"),y=number(p,"y");if(x<-.25||x>1.25||y<-.25||y>1.25)return 0;pose->xy[i][0]=(float)x;pose->xy[i][1]=(float)y;pose->score[i]=(float)number(p,"score");}return 1;
}
static double head_score(const cJSON *source,const cJSON *a,const char *species){
    const cJSON *c=field(a,"context");AnimalPose p;if(!read_pose(c,&p))return 0;
    const char *framing=sfield(field(c,"framing"),"bestCandidate");
    if(!strcmp(framing,"multiple")||!strcmp(framing,"detail")||number(field(field(c,"medium"),"scores"),"photo")<.5)return 0;
    const char *breed=sfield(field(source,"breed"),"label");double breed_match=1;
    if(cJSON_IsTrue(field(field(source,"breed"),"confident"))&&*breed&&strcmp(breed,"mixed breed dog")){
        const cJSON *b=field(c,"breed");breed_match=number(field(b,"scores"),breed);
        if(breed_match<.08||(cJSON_IsTrue(field(b,"confident"))&&strcmp(sfield(b,"label"),breed)))return 0;
    }
    double same=0,other=0;const cJSON *s=NULL;cJSON_ArrayForEach(s,field(a,"speciesCandidates")){
        int dog=!strcasecmp(species,"Canis lupus familiaris")&&(!strcasecmp(sfield(s,"common"),"domestic dog")||!strcasecmp(sfield(s,"scientific"),"Canis familiaris"));
        if(dog||!strcasecmp(canonical(sfield(s,"scientific")),canonical(species)))same+=number(s,"score");else other=fmax(other,number(s,"score"));
    }
    /* BioCLIP confuses fox-like domestic breeds with foxes. Require independent
     * confident matching-breed CLIP and a dog caption before overriding it. */
    int breed_consensus=!strcasecmp(species,"Canis lupus familiaris")&&breed_match>=.3&&
        cJSON_IsTrue(field(field(c,"breed"),"confident"))&&strstr(sfield(c,"caption"),"dog");
    if((same<.25||same<other)&&!breed_consensus)return 0;
    if(breed_consensus)same=fmax(same,.5);
    for(int i=0;i<4;i++)if(p.score[i]<(i==3?.35f:.65f))return 0;
    float eyes=hypotf(p.xy[1][0]-p.xy[0][0],p.xy[1][1]-p.xy[0][1]);if(eyes<.055f)return 0;
    double oblique=fabs(p.xy[2][0]-(p.xy[0][0]+p.xy[1][0])*.5)/eyes;
    if(oblique<.15||oblique>1.5)return 0; /* A second, visibly oblique face view. */
    /* A legible oblique close-up contributes more head geometry than a
     * confidently classified tiny face in a distant whole-body photograph. */
    double head_pixels=hypot(p.xy[3][0]-(p.xy[0][0]+p.xy[1][0])*.5,p.xy[3][1]-(p.xy[0][1]+p.xy[1][1])*.5);
    return .2*same+.2*breed_match+.3*fmin(1,eyes/.12)+.25*fmin(oblique,1)+
        number(field(field(c,"framing"),"scores"),"head")+.35*fmin(1,head_pixels/.33);
}
static int reference_photo(const wchar_t *directory,const cJSON *analysis,Photo *photo,wchar_t *error){
    const char *mask=sfield(analysis,"mask");wchar_t name[256],path[FS_PATH];if(!*mask||strchr(mask,'/')||strchr(mask,'\\')||!fs_decode_utf8(CP_UTF8,0,mask,-1,name,256))return 0;
    return fs_join(path,directory,name)&&photo_read(path,photo,error);
}
int references_volume(Job *job,const wchar_t *directory,const Photo *photo,const Mesh *base,Mesh *out){
    wchar_t path[FS_PATH],prepared[FS_PATH],raw[FS_PATH],log[FS_PATH];VolumeGuide guide={0};Photo normalized={0};int ok=0,head=-1,legs=-1;double hs=0,ls=0;
    job->volume_head=job->volume_legs=0;job->volume_head_index=job->volume_leg_index=-1;
    fs_join(path,directory,L"input-analysis.json");cJSON *source=read_json(path);fs_join(path,directory,L"reference-analysis.json");cJSON *analysis=read_json(path);
    fs_join(path,directory,L"references.json");cJSON *refs=read_json(path),*report=cJSON_CreateObject();
    char species[512];fs_encode_utf8(CP_UTF8,0,job->species,-1,species,sizeof(species),NULL,NULL);
    cJSON_AddStringToObject(report,"method","registered TripoSR oblique head prior; C signed-distance blending; source-joint legs with reference paw width");
    cJSON_AddStringToObject(report,"meaning","estimated anatomy from a different individual; not calibrated multiview or measured hidden anatomy");
    cJSON_AddBoolToObject(report,"colourUsedForSelection",0);
    if(job->shape_disabled||!read_pose(source,&guide.source_pose))goto fallback;
    int count=cJSON_GetArraySize(analysis);if(count>24)count=24;
    for(int i=0;i<count;i++){
        const cJSON *a=cJSON_GetArrayItem(analysis,i);double score=head_score(source,a,species);if(score>hs){hs=score;head=i;}
        score=candidate_score(source,a,cJSON_GetArrayItem(refs,i),species);AnimalPose p;
        if(score>ls&&read_pose(field(a,"context"),&p)){int reliable=0;for(int k=7;k<=16;k+=3)reliable+=p.score[k]>.5f;if(reliable>=3){ls=score;legs=i;}}
    }
    if(head<0){cJSON_AddStringToObject(report,"fallback","no reliable matching-breed oblique head reference");goto fallback;}
    guide.head_index=head;guide.leg_index=legs;guide.standing=!strcmp(sfield(source,"resolvedPose"),"standing");
    if(!read_pose(field(cJSON_GetArrayItem(analysis,head),"context"),&guide.head_pose)||!reference_photo(directory,cJSON_GetArrayItem(analysis,head),&guide.head_photo,job->error))goto done;
    if(legs>=0){if(!read_pose(field(cJSON_GetArrayItem(analysis,legs),"context"),&guide.leg_pose)||!reference_photo(directory,cJSON_GetArrayItem(analysis,legs),&guide.leg_photo,job->error))goto done;}
    if(!photo_prepare(&guide.head_photo,&normalized,job->error))goto done;
    fs_join(prepared,directory,L"volume-head.rgba");fs_join(raw,directory,L"volume-head.raw");fs_join(log,directory,L"inference.log");
    if(!photo_write(prepared,&normalized,job->error))goto done;
    fs_status(job,L"다른 각도의 참고 얼굴을 TripoSR로 입체화하는 중");
    if(!fs_python(job,L"infer",prepared,raw,log)){if(fs_cancelled(job))goto done;job->error[0]=0;cJSON_AddStringToObject(report,"fallback","reference head inference unavailable");goto fallback;}
    if(!mesh_read_raw(raw,&guide.head,job->error)){job->error[0]=0;cJSON_AddStringToObject(report,"fallback","invalid reference head mesh");goto fallback;}
    if(!mesh_volume(job,photo,base,&guide,out))goto done;
    goto record;
fallback:if(!mesh_copy(base,out)){fs_error(job->error,L"입체 보완 메시 메모리가 부족합니다.");goto done;}
record:
    cJSON_AddBoolToObject(report,"headApplied",job->volume_head);cJSON_AddBoolToObject(report,"legsApplied",job->volume_legs);
    cJSON_AddNumberToObject(report,"headReferenceIndex",job->volume_head_index);cJSON_AddNumberToObject(report,"legReferenceIndex",job->volume_leg_index);
    cJSON_AddNumberToObject(report,"headScale",job->volume_head_scale);cJSON_AddNumberToObject(report,"pawRadius",job->volume_paw_radius);cJSON_AddNumberToObject(report,"muzzleProjection",job->volume_muzzle_depth);
    if(job->volume_head){cJSON *r=cJSON_GetArrayItem(refs,head);cJSON_AddBoolToObject(r,"volumeHeadSelected",1);cJSON_AddStringToObject(r,"volumeHeadUse","registered oblique TripoSR head geometry; original photo colours");cJSON_AddItemToObject(report,"headReference",cJSON_Duplicate(r,1));}
    if(job->volume_legs){cJSON *r=cJSON_GetArrayItem(refs,legs);cJSON_AddBoolToObject(r,"volumeLegSelected",1);cJSON_AddStringToObject(r,"volumeLegUse","visible paw cross-section for estimated source-joint legs");cJSON_AddItemToObject(report,"legReference",cJSON_Duplicate(r,1));}
    fs_join(path,directory,L"references.json");if(refs&&!write_json(path,refs))goto done;
    fs_join(path,directory,L"volume-completion.json");if(!write_json(path,report))goto done;ok=1;
done:
    photo_free(&guide.head_photo);photo_free(&guide.leg_photo);photo_free(&normalized);mesh_free(&guide.head);cJSON_Delete(source);cJSON_Delete(analysis);cJSON_Delete(refs);cJSON_Delete(report);
    if(!ok){mesh_free(out);if(!job->error[0])fs_error(job->error,L"참고 입체 보완을 완료하지 못했습니다.");}return ok;
}
int shape_candidate_count(const wchar_t *directory){
    wchar_t p[FS_PATH];fs_join(p,directory,L"input-analysis.json");cJSON *source=read_json(p);
    fs_join(p,directory,L"reference-analysis.json");cJSON *analysis=read_json(p);
    fs_join(p,directory,L"references.json");cJSON *refs=read_json(p);
    fs_join(p,directory,L"species.json");cJSON *species=read_json(p);int n=0;const cJSON *a=NULL;
    int index=0;cJSON_ArrayForEach(a,analysis){if(candidate_score(source,a,cJSON_GetArrayItem(refs,index),sfield(species,"scientific"))>0)n++;index++;}
    cJSON_Delete(source);cJSON_Delete(analysis);cJSON_Delete(species);cJSON_Delete(refs);return n;
}
static void project(const Mesh *m,Projection *r,const float lo[3],const float hi[3]){
    for(int i=0;i<PIXELS;i++){r->near_z[i]=FLT_MAX;r->far_z[i]=-FLT_MAX;}
    for(uint32_t t=0;t<m->triangle_count;t++){
        float x[3],y[3],z[3];const Vertex *v[3];
        for(int k=0;k<3;k++){
            v[k]=&m->vertices[m->triangles[t].v[k]];
            x[k]=3+(GRID-6)*(v[k]->p[0]-lo[0])/fmaxf(hi[0]-lo[0],1e-8f);
            y[k]=3+(GRID-6)*(v[k]->p[1]-lo[1])/fmaxf(hi[1]-lo[1],1e-8f);z[k]=v[k]->p[2];
        }
        float det=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);if(fabsf(det)<1e-8f)continue;
        int xmin=(int)clamp(floorf(fminf(x[0],fminf(x[1],x[2]))),0,GRID-1),xmax=(int)clamp(ceilf(fmaxf(x[0],fmaxf(x[1],x[2]))),0,GRID-1);
        int ymin=(int)clamp(floorf(fminf(y[0],fminf(y[1],y[2]))),0,GRID-1),ymax=(int)clamp(ceilf(fmaxf(y[0],fmaxf(y[1],y[2]))),0,GRID-1);
        for(int yy=ymin;yy<=ymax;yy++)for(int xx=xmin;xx<=xmax;xx++){
            float a=((y[1]-y[2])*(xx+.5f-x[2])+(x[2]-x[1])*(yy+.5f-y[2]))/det;
            float b=((y[2]-y[0])*(xx+.5f-x[2])+(x[0]-x[2])*(yy+.5f-y[2]))/det,c=1-a-b;
            if(a<0||b<0||c<0)continue;int i=yy*GRID+xx;float d=a*z[0]+b*z[1]+c*z[2];
            if(d<r->near_z[i])r->near_z[i]=d;if(d>r->far_z[i])r->far_z[i]=d;
        }
    }
}
static double projection_iou(const Projection *a,const Projection *b){
    int intersection=0,united=0;
    for(int i=0;i<PIXELS;i++){
        int av=a->near_z[i]!=FLT_MAX,bv=b->near_z[i]!=FLT_MAX;united+=av||bv;
        intersection+=av&&bv;
    }
    return united?(double)intersection/united:0;
}
static uint32_t component_root(uint32_t *parent,uint32_t v){while(parent[v]!=v){parent[v]=parent[parent[v]];v=parent[v];}return v;}
static int largest_component(Mesh *m,wchar_t *error){
    uint32_t *parent=malloc((size_t)m->vertex_count*4),*count=calloc(m->vertex_count,4),*map=malloc((size_t)m->vertex_count*4);
    if(!parent||!count||!map){free(parent);free(count);free(map);fs_error(error,L"형상 검사 메모리가 부족합니다.");return 0;}
    for(uint32_t i=0;i<m->vertex_count;i++)parent[i]=i;
    for(uint32_t i=0;i<m->triangle_count;i++)for(int k=1;k<3;k++){uint32_t a=component_root(parent,m->triangles[i].v[0]),b=component_root(parent,m->triangles[i].v[k]);parent[b]=a;}
    uint32_t largest=0;for(uint32_t i=0;i<m->triangle_count;i++){uint32_t a=component_root(parent,m->triangles[i].v[0]);if(++count[a]>count[largest])largest=a;}
    int ok=1;for(uint32_t i=0;i<m->vertex_count;i++)if(i!=largest&&count[i]>.05*count[largest])ok=0;
    if(ok){
        for(uint32_t i=0;i<m->vertex_count;i++)map[i]=UINT32_MAX;
        uint32_t n=0;for(uint32_t i=0;i<m->triangle_count;i++)if(component_root(parent,m->triangles[i].v[0])==largest){m->triangles[n++]=m->triangles[i];for(int k=0;k<3;k++)map[m->triangles[i].v[k]]=0;}m->triangle_count=n;
        n=0;for(uint32_t i=0;i<m->vertex_count;i++)if(map[i]!=UINT32_MAX){map[i]=n;m->vertices[n++]=m->vertices[i];}m->vertex_count=n;
        for(uint32_t i=0;i<m->triangle_count;i++)for(int k=0;k<3;k++)m->triangles[i].v[k]=map[m->triangles[i].v[k]];
    }
    free(parent);free(count);free(map);return ok;
}
static double fullness(const Mesh *m){
    float lo[3],hi[3];mesh_bounds(m,lo,hi);double volume=0;
    for(uint32_t i=0;i<m->triangle_count;i++){const Triangle *t=&m->triangles[i];const float *a=m->vertices[t->v[0]].p,*b=m->vertices[t->v[1]].p,*c=m->vertices[t->v[2]].p;
        volume+=a[0]*(b[1]*c[2]-b[2]*c[1])+a[1]*(b[2]*c[0]-b[0]*c[2])+a[2]*(b[0]*c[1]-b[1]*c[0]);}
    return fabs(volume/6)/fmax(1e-12,(hi[0]-lo[0])*(hi[1]-lo[1])*(hi[2]-lo[2]));
}
int mesh_complete(const Mesh *original,const Mesh *prior,Mesh *out,FsCancel * cancel,double *iou,double *depth_ratio,wchar_t *error){
    Projection *a=NULL,*b=NULL;int ok=0;*iou=*depth_ratio=0;memset(out,0,sizeof(*out));
    if(stopped(cancel,error)||!mesh_validate(original,error)||!mesh_validate(prior,error))return 0;
    if(!mesh_copy(prior,out)){fs_error(error,L"참고 형상 메모리가 부족합니다.");return 0;}
    if(!largest_component(out,error)||fullness(out)<.17)goto done;
    float lo[3],hi[3],pl[3],ph[3];mesh_bounds(original,lo,hi);mesh_bounds(out,pl,ph);
    float scale=(hi[1]-lo[1])/fmaxf(ph[1]-pl[1],1e-8f);
    /* A reference photo establishes its own viewing direction. Keep it rather
     * than rotating a thin mesh sideways just to score a large silhouette. */
    for(uint32_t i=0;i<out->vertex_count;i++)for(int k=0;k<3;k++)out->vertices[i].p[k]=(out->vertices[i].p[k]-(pl[k]+ph[k])*.5f)*scale+(lo[k]+hi[k])*.5f;
    a=malloc(sizeof(*a));b=malloc(sizeof(*b));if(!a||!b){fs_error(error,L"형상 비교 메모리가 부족합니다.");goto done;}
    float boundlo[3],boundhi[3];mesh_bounds(out,boundlo,boundhi);for(int k=0;k<3;k++){boundlo[k]=fminf(boundlo[k],lo[k]);boundhi[k]=fmaxf(boundhi[k],hi[k]);}
    project(original,a,boundlo,boundhi);project(out,b,boundlo,boundhi);*iou=projection_iou(a,b);if(*iou<.45)goto done;
    /* The reference supplies bounded rear depth only. Preserve every source
     * X/Y coordinate, all front vertices, face identity, feet and source colour.
     * A reference animal must never replace the photographed animal's head. */
    mesh_free(out);if(!mesh_copy(original,out)){fs_error(error,L"원본 형상 보존 메모리가 부족합니다.");goto done;}
    float middle=(lo[2]+hi[2])*.5f,height=hi[1]-lo[1];
    for(uint32_t i=0;i<out->vertex_count;i++){
        if(!(i%4096)&&stopped(cancel,error))goto done;Vertex *v=&out->vertices[i];if(v->p[2]<=middle)continue;
        float x=3+(GRID-6)*(v->p[0]-boundlo[0])/(boundhi[0]-boundlo[0]),y=3+(GRID-6)*(v->p[1]-boundlo[1])/(boundhi[1]-boundlo[1]);
        float sum=0,weight=0;int ix=(int)x,iy=(int)y;
        for(int dy=-3;dy<=3;dy++)for(int dx=-3;dx<=3;dx++)if(ix+dx>=0&&ix+dx<GRID&&iy+dy>=0&&iy+dy<GRID){
            float z=b->far_z[(iy+dy)*GRID+ix+dx],w=1.f/(1+dx*dx+dy*dy);weight+=w;
            if(z!=-FLT_MAX)sum+=w*clamp(z-hi[2],0,height*.25f);
        }
        float t=clamp((v->p[2]-middle)/fmaxf(hi[2]-middle,1e-6f),0,1);v->p[2]+=(weight?sum/weight:0)*t*t;
    }
    mesh_normals(out);mesh_bounds(out,pl,ph);*depth_ratio=(ph[2]-pl[2])/fmaxf(hi[2]-lo[2],1e-8f);ok=mesh_validate(out,error);
 done:free(a);free(b);if(!ok)mesh_free(out);return ok;
}
int references_complete(Job *job,const wchar_t *directory,const Mesh *original,Mesh *out){
    wchar_t path[FS_PATH],manifest[FS_PATH],done_path[FS_PATH],log[FS_PATH];int ok=0;
    fs_join(path,directory,L"input-analysis.json");cJSON *source=read_json(path);
    fs_join(path,directory,L"reference-analysis.json");cJSON *analysis=read_json(path);
    fs_join(path,directory,L"references.json");cJSON *refs=read_json(path);
    cJSON *report=cJSON_CreateObject(),*tasks=cJSON_CreateArray(),*selection=cJSON_AddArrayToObject(report,"candidates");
    char scientific[512];fs_encode_utf8(CP_UTF8,0,job->species,-1,scientific,sizeof(scientific),NULL,NULL);
    cJSON_AddStringToObject(report,"method","pose-compatible TripoSR reference for bounded rear depth only; input face, photo-plane outline, feet and colours retained");
    cJSON_AddStringToObject(report,"sourcePose",sfield(source,"resolvedPose"));
    cJSON_AddBoolToObject(report,"measuredReconstruction",0);cJSON_AddBoolToObject(report,"colourUsedForSelection",0);
    int selected[5]={-1,-1,-1,-1,-1};double scores[24]={0};
    int count=cJSON_GetArraySize(analysis);if(count>24)count=24;
    for(int i=0;i<count;i++)scores[i]=candidate_score(source,cJSON_GetArrayItem(analysis,i),cJSON_GetArrayItem(refs,i),scientific);
    if(!job->shape_disabled)for(int slot=0;slot<5;slot++){
        int best=-1;for(int i=0;i<count;i++)if(scores[i]>0&&(best<0||scores[i]>scores[best]))best=i;if(best<0)break;
        const cJSON *a=cJSON_GetArrayItem(analysis,best);const char *mask=sfield(a,"mask");double score=scores[best];scores[best]=0;
        if(!*mask||strchr(mask,'/')||strchr(mask,'\\'))continue;
        wchar_t name[256],prepared[FS_PATH],raw[FS_PATH];Photo cutout={0},normalized={0};
        if(!fs_decode_utf8(CP_UTF8,0,mask,-1,name,256)||!fs_join(path,directory,name))continue;
        if(!photo_read(path,&cutout,job->error))goto done;
        int valid=photo_prepare(&cutout,&normalized,job->error);photo_free(&cutout);if(!valid)goto done;
        fs_wformat(name,256,L"shape-%d.rgba",slot);fs_join(prepared,directory,name);
        valid=photo_write(prepared,&normalized,job->error);photo_free(&normalized);if(!valid)goto done;
        fs_wformat(name,256,L"shape-%d.raw",slot);fs_join(raw,directory,name);
        char utf8[FS_PATH*3];cJSON *task=cJSON_CreateObject();fs_encode_utf8(CP_UTF8,0,prepared,-1,utf8,sizeof(utf8),NULL,NULL);cJSON_AddStringToObject(task,"input",utf8);
        fs_encode_utf8(CP_UTF8,0,raw,-1,utf8,sizeof(utf8),NULL,NULL);cJSON_AddStringToObject(task,"output",utf8);cJSON_AddItemToArray(tasks,task);
        cJSON *entry=cJSON_CreateObject();cJSON_AddNumberToObject(entry,"slot",slot);cJSON_AddNumberToObject(entry,"referenceIndex",best);cJSON_AddNumberToObject(entry,"poseScore",score);
        cJSON_AddStringToObject(entry,"pageUrl",sfield(cJSON_GetArrayItem(refs,best),"pageUrl"));cJSON_AddItemToArray(selection,entry);selected[slot]=best;
        cJSON_AddStringToObject(entry,"author",sfield(cJSON_GetArrayItem(refs,best),"author"));cJSON_AddStringToObject(entry,"license",sfield(cJSON_GetArrayItem(refs,best),"license"));
    }
    if(cJSON_GetArraySize(tasks)){
        fs_status(job,L"같은 종·자세의 참고 사진에서 가려진 몸통 형상을 추정하는 중");
        fs_join(manifest,directory,L"shape-inference.json");fs_join(done_path,directory,L"shape-inference-done.json");fs_join(log,directory,L"inference.log");
        if(!write_json(manifest,tasks))goto done;
        if(!fs_python(job,L"infer-batch",manifest,done_path,log)){
            if(fs_cancelled(job))goto done;job->error[0]=0;cJSON_AddStringToObject(report,"fallback","reference inference unavailable");
        }else{
            double best=-1;for(int slot=0;slot<5;slot++)if(selected[slot]>=0){
                fs_status(job,L"C에서 참고 형상의 연결·부피를 검사하고 원본 색감을 적용하는 중");
                wchar_t name[128];fs_wformat(name,128,L"shape-%d.raw",slot);fs_join(path,directory,name);Mesh prior={0},candidate={0};
                if(!mesh_read_raw(path,&prior,job->error)){
                    /* One defective optional reference must not discard the
                     * valid input reconstruction or prevent later candidates. */
                    cJSON *entry=NULL;cJSON_ArrayForEach(entry,selection)if(number(entry,"slot")==slot){cJSON_AddBoolToObject(entry,"accepted",0);cJSON_AddStringToObject(entry,"rejectedReason","invalid reference mesh");}
                    job->error[0]=0;continue;
                }
                double iou=0,depth=0;int accepted=mesh_complete(original,&prior,&candidate,job->cancel,&iou,&depth,job->error);mesh_free(&prior);
                cJSON *entry=NULL,*search=NULL;cJSON_ArrayForEach(search,selection)if(number(search,"slot")==slot)entry=search;
                cJSON_AddBoolToObject(entry,"accepted",accepted);cJSON_AddNumberToObject(entry,"frontSilhouetteIoU",iou);cJSON_AddNumberToObject(entry,"depthRatio",depth);
                if(fs_cancelled(job)){mesh_free(&candidate);goto done;}
                if(job->error[0]){mesh_free(&candidate);goto done;}
                BodyFit fit={0};if(accepted){fit.separated_branches=mesh_limb_branches(&candidate,1,job->cancel,job->error);if(fit.separated_branches<0){mesh_free(&candidate);goto done;}}
                /* Once a volume is plausible, extra bulk earns no bonus.
                 * Real open lower-limb branches outrank a single round mass. */
                const cJSON *context=field(cJSON_GetArrayItem(analysis,selected[slot]),"context");
                int face_match=cJSON_GetArraySize(field(source,"faceBox"))==4&&cJSON_GetArraySize(field(context,"faceBox"))==4;
                double photo_match=fit.applied?fit.silhouette_after:iou;
                double rank=accepted?.45*photo_match+.15*iou+.1*fmin(1,fullness(&candidate)/.2)+.12*fit.separated_branches+.15*face_match:0;
                cJSON_AddBoolToObject(entry,"sourceProportionFit",fit.applied);cJSON_AddNumberToObject(entry,"sourcePhotoIoUBefore",fit.silhouette_before);cJSON_AddNumberToObject(entry,"sourcePhotoIoUAfter",fit.silhouette_after);
                cJSON_AddNumberToObject(entry,"separatedLowerBranches",fit.separated_branches);
                cJSON_AddNumberToObject(entry,"geometryRank",rank);
                if(accepted&&rank>best){mesh_free(out);*out=candidate;memset(&candidate,0,sizeof(candidate));best=rank;job->shape_used=1;job->shape_iou=iou;job->shape_depth_ratio=depth;
                    job->body_fit=fit;
                    cJSON_DeleteItemFromObjectCaseSensitive(report,"referenceIndex");cJSON_AddNumberToObject(report,"referenceIndex",selected[slot]);}
                mesh_free(&candidate);
            }
        }
    }
    if(!job->shape_used){
        fs_status(job,job->shape_disabled?L"원본 사진의 TripoSR 형상을 사용합니다.":L"자세·형상이 맞는 참고가 없어 원본 TripoSR 형상을 유지합니다.");
        if(!mesh_copy(original,out)){fs_error(job->error,L"메시 복사 메모리가 부족합니다.");goto done;}
    }
    if(job->shape_used){
        int chosen=(int)number(report,"referenceIndex");cJSON *r=cJSON_GetArrayItem(refs,chosen);
        cJSON_AddBoolToObject(r,"shapeSelected",1);cJSON_AddStringToObject(r,"shapeUse","bounded rear-depth guidance; source front surface and all photo-plane positions retained");
        fs_join(path,directory,L"references.json");if(!write_json(path,refs))goto done;
    }
    cJSON_AddStringToObject(report,"geometryMode",job->shape_used?"source_with_rear_prior":"source_only");
    cJSON_AddBoolToObject(report,"applied",job->shape_used);cJSON_AddNumberToObject(report,"frontSilhouetteIoU",job->shape_iou);cJSON_AddNumberToObject(report,"depthRatio",job->shape_depth_ratio);
    fs_join(path,directory,L"shape-completion.json");if(!write_json(path,report))goto done;
    ok=1;
done:cJSON_Delete(source);cJSON_Delete(analysis);cJSON_Delete(refs);cJSON_Delete(report);cJSON_Delete(tasks);if(!ok&&!job->error[0])fs_error(job->error,L"형태 보완 기록을 저장하지 못했습니다.");return ok;
}
