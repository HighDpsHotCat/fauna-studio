/* C17 detail refinement. Photo colours are observed; the shallow facial relief
 * and accentuated existing creases are artistic estimates, not measured anatomy.
 * Never opens holes, creates disconnected eyes, or invents hidden limb joints. */
#include "fauna.h"
#include "../vendor/cjson/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define DSIDE 512
typedef struct { int x0,y0,x1,y1; } Crop;
static float clip(float x,float a,float b){return fmaxf(a,fminf(b,x));}
static float smooth(float x){x=clip(x,0,1);return x*x*(3-2*x);}
static int stopped(Job *j){if(fs_cancelled(j)){fs_error(j->error,L"생성을 취소했습니다.");return 1;}return 0;}
static cJSON *read_json(const wchar_t *directory,const wchar_t *name){
    wchar_t path[FS_PATH];if(!fs_join(path,directory,name))return NULL;FILE *f=fs_wfopen(path,L"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<1||n>20000000){fclose(f);return NULL;}
    char *s=malloc((size_t)n+1);if(!s){fclose(f);return NULL;}size_t got=fread(s,1,n,f);fclose(f);s[got]=0;
    cJSON *r=got==(size_t)n?cJSON_Parse(s):NULL;free(s);return r;
}
static int face_box(const cJSON *context,float box[4]){
    const cJSON *a=cJSON_GetObjectItemCaseSensitive(context,"faceBox");if(cJSON_GetArraySize(a)!=4)return 0;
    for(int k=0;k<4;k++){double d=cJSON_GetNumberValue(cJSON_GetArrayItem(a,k));if(!isfinite(d)||d<0||d>1)return 0;box[k]=(float)d;}
    return box[2]>.025f&&box[3]>.025f&&box[0]+box[2]<=1.01f&&box[1]+box[3]<=1.01f;
}
static int crop(const Photo *p,Crop *c){
    *c=(Crop){(int)p->width,(int)p->height,0,0};
    for(uint32_t y=0;y<p->height;y++)for(uint32_t x=0;x<p->width;x++)if(p->rgba[((size_t)y*p->width+x)*4+3]>32){
        if((int)x<c->x0)c->x0=x;if((int)x>c->x1)c->x1=x;if((int)y<c->y0)c->y0=y;if((int)y>c->y1)c->y1=y;}
    return c->x1>c->x0&&c->y1>c->y0;
}
static int sample(const Photo *p,float x,float y,float rgb[3]){
    if(x<0||y<0||x>p->width-1||y>p->height-1)return 0;
    int x0=(int)x,y0=(int)y,x1=x0+1<(int)p->width?x0+1:x0,y1=y0+1<(int)p->height?y0+1:y0;
    float weights[4]={(1-(x-x0))*(1-(y-y0)),(x-x0)*(1-(y-y0)),(1-(x-x0))*(y-y0),(x-x0)*(y-y0)};
    int xs[4]={x0,x1,x0,x1},ys[4]={y0,y0,y1,y1};float alpha=0;memset(rgb,0,12);
    for(int i=0;i<4;i++){const unsigned char *q=p->rgba+((size_t)ys[i]*p->width+xs[i])*4;alpha+=weights[i]*q[3];for(int k=0;k<3;k++)rgb[k]+=weights[i]*q[k];}
    return alpha>220;
}
static float luma(const float c[3]){return .2126f*c[0]+.7152f*c[1]+.0722f*c[2];}
static void near_depth(const Mesh *m,float *depth,const float lo[3],const float hi[3],float *far_depth){
    for(int i=0;i<DSIDE*DSIDE;i++){depth[i]=FLT_MAX;if(far_depth)far_depth[i]=-FLT_MAX;}
    for(uint32_t i=0;i<m->triangle_count;i++){
        float x[3],y[3],z[3];for(int k=0;k<3;k++){const float *p=m->vertices[m->triangles[i].v[k]].p;
            x[k]=(DSIDE-1)*(p[0]-lo[0])/(hi[0]-lo[0]);y[k]=(DSIDE-1)*(p[1]-lo[1])/(hi[1]-lo[1]);z[k]=p[2];}
        float det=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);if(fabsf(det)<1e-9f)continue;
        int x0=(int)clip(floorf(fminf(x[0],fminf(x[1],x[2]))),0,DSIDE-1),x1=(int)clip(ceilf(fmaxf(x[0],fmaxf(x[1],x[2]))),0,DSIDE-1);
        int y0=(int)clip(floorf(fminf(y[0],fminf(y[1],y[2]))),0,DSIDE-1),y1=(int)clip(ceilf(fmaxf(y[0],fmaxf(y[1],y[2]))),0,DSIDE-1);
        for(int yy=y0;yy<=y1;yy++)for(int xx=x0;xx<=x1;xx++){
            float a=((y[1]-y[2])*(xx-x[2])+(x[2]-x[1])*(yy-y[2]))/det;
            float b=((y[2]-y[0])*(xx-x[2])+(x[0]-x[2])*(yy-y[2]))/det,c=1-a-b;
            if(a>=0&&b>=0&&c>=0){float d=a*z[0]+b*z[1]+c*z[2];depth[yy*DSIDE+xx]=fminf(depth[yy*DSIDE+xx],d);if(far_depth)far_depth[yy*DSIDE+xx]=fmaxf(far_depth[yy*DSIDE+xx],d);}
        }
    }
}
static int visible(const Vertex *v,const float *depth,const float lo[3],const float hi[3],float tolerance){
    if(v->n[2]>-.15f)return 0;
    int x=(int)clip((DSIDE-1)*(v->p[0]-lo[0])/(hi[0]-lo[0]),0,DSIDE-1),y=(int)clip((DSIDE-1)*(v->p[1]-lo[1])/(hi[1]-lo[1]),0,DSIDE-1);
    float nearest=FLT_MAX;
    for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)if(x+dx>=0&&x+dx<DSIDE&&y+dy>=0&&y+dy<DSIDE)nearest=fminf(nearest,fabsf(v->p[2]-depth[(y+dy)*DSIDE+x+dx]));
    return nearest<tolerance;
}
static void normal_at(const Mesh *m,const Triangle *t,const float (*delta)[3],float scale,float n[3]){
    float p[3][3],u[3],v[3];for(int k=0;k<3;k++)for(int c=0;c<3;c++)p[k][c]=m->vertices[t->v[k]].p[c]+scale*delta[t->v[k]][c];
    for(int k=0;k<3;k++){u[k]=p[1][k]-p[0][k];v[k]=p[2][k]-p[0][k];}
    n[0]=u[1]*v[2]-u[2]*v[1];n[1]=u[2]*v[0]-u[0]*v[2];n[2]=u[0]*v[1]-u[1]*v[0];
}
static float safe_move(Mesh *m,float (*delta)[3]){
    /* Attenuate only offending triangle neighbourhoods. A near-zero marching
     * cubes triangle must not cancel all useful relief across the animal. */
    for(int attempt=0;attempt<24;attempt++){
        int valid=1;for(uint32_t i=0;i<m->triangle_count&&valid;i++){
            float a[3],b[3];normal_at(m,&m->triangles[i],delta,0,a);normal_at(m,&m->triangles[i],delta,1,b);
            double aa=0,bb=0,ab=0;for(int k=0;k<3;k++){aa+=a[k]*a[k];bb+=b[k]*b[k];ab+=a[k]*b[k];}
            if(aa>1e-24&&(bb<aa*.1||ab<.35*sqrt(aa*bb))){valid=0;break;}
        }
        if(valid){for(uint32_t i=0;i<m->vertex_count;i++)for(int k=0;k<3;k++)m->vertices[i].p[k]+=delta[i][k];mesh_normals(m);return 1;}
        for(uint32_t i=0;i<m->triangle_count;i++){
            float a[3],b[3];normal_at(m,&m->triangles[i],delta,0,a);normal_at(m,&m->triangles[i],delta,1,b);
            double aa=0,bb=0,ab=0;for(int k=0;k<3;k++){aa+=a[k]*a[k];bb+=b[k]*b[k];ab+=a[k]*b[k];}
            if(aa>1e-24&&(bb<aa*.12||ab<.4*sqrt(aa*bb)))for(int v=0;v<3;v++)for(int k=0;k<3;k++)delta[m->triangles[i].v[v]][k]*=.5f;
        }
    }
    return 0;
}
static int creases(Job *job,Mesh *mesh){
    uint32_t n=mesh->vertex_count;float (*low)[3]=malloc((size_t)n*sizeof(*low)),(*sum)[3]=calloc(n,sizeof(*sum)),(*delta)[3]=calloc(n,sizeof(*delta));
    uint32_t *degree=calloc(n,sizeof(*degree));int ok=0;
    if(!low||!sum||!delta||!degree){fs_error(job->error,L"세부 형상 처리 메모리가 부족합니다.");goto done;}
    for(uint32_t i=0;i<n;i++)memcpy(low[i],mesh->vertices[i].p,12);
    for(int pass=0;pass<12;pass++){
        if(stopped(job))goto done;memset(sum,0,(size_t)n*sizeof(*sum));memset(degree,0,(size_t)n*sizeof(*degree));
        for(uint32_t t=0;t<mesh->triangle_count;t++)for(int k=0;k<3;k++){
            uint32_t a=mesh->triangles[t].v[k],b=mesh->triangles[t].v[(k+1)%3];degree[a]++;degree[b]++;
            for(int c=0;c<3;c++){sum[a][c]+=low[b][c];sum[b][c]+=low[a][c];}}
        for(uint32_t i=0;i<n;i++)if(degree[i])for(int k=0;k<3;k++)low[i][k]=.5f*low[i][k]+.5f*sum[i][k]/degree[i];
    }
    float lo[3],hi[3];mesh_bounds(mesh,lo,hi);float span=fmaxf(hi[0]-lo[0],fmaxf(hi[1]-lo[1],hi[2]-lo[2]));mesh_normals(mesh);
    for(uint32_t i=0;i<n;i++){
        Vertex *v=&mesh->vertices[i];float concavity=0;for(int k=0;k<3;k++)concavity+=(low[i][k]-v->p[k])*v->n[k];
        /* Concave ridges already in TripoSR: emphasize them, never cut a new leg. */
        if(v->p[1]<lo[1]+(hi[1]-lo[1])*.52f&&concavity>span*.0015f){float amount=fminf((concavity-span*.0015f)*.65f,span*.004f);
            for(int k=0;k<3;k++)delta[i][k]=-amount*v->n[k];v->detail=fmaxf(v->detail,clip(concavity/(span*.015f),0,.45f));job->crease_detail_vertices++;}
    }
    if(safe_move(mesh,delta)==0)job->crease_detail_vertices=0;
    else for(uint32_t i=0;i<n;i++){double d=0;for(int k=0;k<3;k++)d+=delta[i][k]*delta[i][k];job->crease_displacement=fmax(job->crease_displacement,sqrt(d));}ok=1;
done:free(low);free(sum);free(delta);free(degree);return ok;
}
static float gauss(float u,float v,float x,float y,float sx,float sy){float a=(u-x)/sx,b=(v-y)/sy;return expf(-.5f*(a*a+b*b));}
static void find_features(const Photo *source,const float box[4],float feature[3][2]){
    /* Search only inside an independently detected frontal cat face. */
    const float ranges[3][4]={{.1f,.46f,.24f,.58f},{.54f,.9f,.24f,.58f},{.33f,.67f,.52f,.8f}};
    const float defaults[3][2]={{.28f,.43f},{.72f,.43f},{.5f,.67f}};
    for(int f=0;f<3;f++){
        float best=FLT_MAX;memcpy(feature[f],defaults[f],8);
        for(float v=ranges[f][2];v<ranges[f][3];v+=.0125f)for(float u=ranges[f][0];u<ranges[f][1];u+=.0125f){
            float rgb[3];if(!sample(source,(box[0]+u*box[2])*source->width,(box[1]+v*box[3])*source->height,rgb))continue;
            float d=(u-defaults[f][0])*(u-defaults[f][0])+(v-defaults[f][1])*(v-defaults[f][1]);
            float score=luma(rgb)+150*d;if(f==2)score-=1.6f*fmaxf(0,rgb[0]-fmaxf(rgb[1],rgb[2]));
            if(score<best){best=score;feature[f][0]=u;feature[f][1]=v;}
        }
    }
}
static void photo_palette(const Photo *source,const Photo *reference,float palette[256][3]){
    double hist[256]={0},other[256]={0},sums[256][3]={{0}},total=0,rtotal=0;
    for(size_t i=0;i<(size_t)source->width*source->height;i++)if(source->rgba[4*i+3]>240){const unsigned char *p=source->rgba+4*i;int b=(54*p[0]+183*p[1]+19*p[2])/256;hist[b]++;total++;for(int k=0;k<3;k++)sums[b][k]+=p[k];}
    for(size_t i=0;i<(size_t)reference->width*reference->height;i++)if(reference->rgba[4*i+3]>240){const unsigned char *p=reference->rgba+4*i;other[(54*p[0]+183*p[1]+19*p[2])/256]++;rtotal++;}
    int src=0;while(src<255&&!hist[src])src++;double cdf=hist[src]/fmax(1,total),rcdf=0;
    for(int i=0;i<256;i++){rcdf+=other[i]/fmax(1,rtotal);while(src<255&&cdf<rcdf){src++;cdf+=hist[src]/fmax(1,total);}
        int b=src;while(b>0&&!hist[b])b--;for(int k=0;k<3;k++)palette[i][k]=(float)(sums[b][k]/fmax(1,hist[b]));}
}
static int smooth_face_surface(Job *job,Mesh *m,const float *depth,const float lo[3],const float hi[3],float span){
    if(!job->face_projection)return 1;
    uint32_t n=m->vertex_count;float *start=malloc((size_t)n*4),*sum=malloc((size_t)n*4),*weight=calloc(n,4);uint32_t *degree=calloc(n,4);
    if(!start||!sum||!weight||!degree){free(start);free(sum);free(weight);free(degree);fs_error(job->error,L"얼굴 표면 처리 메모리가 부족합니다.");return 0;}
    for(uint32_t i=0;i<n;i++){Vertex *v=&m->vertices[i];start[i]=v->p[2];
        float u=(job->face_xy[1]-v->p[0])/(job->face_xy[1]-job->face_xy[0]),vv=(job->face_xy[3]-v->p[1])/(job->face_xy[3]-job->face_xy[2]);
        if(visible(v,depth,lo,hi,span*.02f))weight[i]=smooth((1.3f-sqrtf(powf((u-.5f)/.62f,2)+powf((vv-.5f)/.78f,2)))/.3f);
    }
    for(uint32_t t=0;t<m->triangle_count;t++)for(int k=0;k<3;k++){degree[m->triangles[t].v[k]]++;degree[m->triangles[t].v[(k+1)%3]]++;}
    int ok=1;
    for(int pass=0;pass<120;pass++){
        if(stopped(job)){ok=0;break;}memset(sum,0,(size_t)n*4);
        for(uint32_t t=0;t<m->triangle_count;t++)for(int k=0;k<3;k++){uint32_t a=m->triangles[t].v[k],b=m->triangles[t].v[(k+1)%3];sum[a]+=m->vertices[b].p[2];sum[b]+=m->vertices[a].p[2];}
        for(uint32_t i=0;i<n;i++)if(degree[i]&&weight[i]>.001f){float z=m->vertices[i].p[2];m->vertices[i].p[2]=clip(z+.55f*weight[i]*(sum[i]/degree[i]-z),start[i]-span*.035f,start[i]+span*.035f);}
    }
    for(uint32_t i=0;i<n;i++)job->face_surface_displacement=fmax(job->face_surface_displacement,fabs(m->vertices[i].p[2]-start[i]));
    free(start);free(sum);free(weight);free(degree);mesh_normals(m);return ok;
}
int mesh_refine_details(Job *job,const wchar_t *directory,Mesh *mesh){
    cJSON *source_info=NULL,*analysis=NULL,*completion=NULL;Photo source={0},reference={0};float *depth=NULL,(*delta)[3]=NULL;int ok=0;
    if(stopped(job)||!creases(job,mesh))goto done;
    source_info=read_json(directory,L"input-analysis.json");analysis=read_json(directory,L"reference-analysis.json");completion=read_json(directory,L"shape-completion.json");
    float sb[4]={0},rb[4]={0};int has_source_face=face_box(source_info,sb),has_reference_face=0;wchar_t path[FS_PATH],name[256];
    const cJSON *reference_info=NULL;
    if(job->shape_used){
        const cJSON *index=cJSON_GetObjectItemCaseSensitive(completion,"referenceIndex");if(!cJSON_IsNumber(index))goto success;
        reference_info=cJSON_GetArrayItem(analysis,index->valueint);has_reference_face=face_box(cJSON_GetObjectItemCaseSensitive(reference_info,"context"),rb);
        const char *mask=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reference_info,"mask"));
        if(!mask||strchr(mask,'/')||strchr(mask,'\\')||!fs_decode_utf8(CP_UTF8,0,mask,-1,name,256)||!fs_join(path,directory,name))goto success;
        if(!photo_read(path,&reference,job->error))goto done;
    }
    fs_join(path,directory,L"cutout.rgba");if(!photo_read(path,&source,job->error))goto done;
    if(!job->shape_used){reference=source;memset(&source,0,sizeof(source));if(!photo_read(path,&source,job->error))goto done;memcpy(rb,sb,sizeof(rb));has_reference_face=has_source_face;}
    Crop bounds;if(!crop(&reference,&bounds))goto success;
    float lo[3],hi[3];mesh_bounds(mesh,lo,hi);float span=fmaxf(hi[0]-lo[0],fmaxf(hi[1]-lo[1],hi[2]-lo[2]));
    if(hi[0]<=lo[0]||hi[1]<=lo[1])goto success;
    depth=malloc(DSIDE*DSIDE*sizeof(*depth));delta=calloc(mesh->vertex_count,sizeof(*delta));if(!depth||!delta){fs_error(job->error,L"사진 세부 표현 메모리가 부족합니다.");goto done;}
    near_depth(mesh,depth,lo,hi,NULL);float palette[256][3],features[3][2]={{0}};photo_palette(&source,&reference,palette);
    if(has_source_face&&has_reference_face)find_features(&source,sb,features);
    float projection[4]={FLT_MAX,-FLT_MAX,FLT_MAX,-FLT_MAX};
    if(has_source_face&&has_reference_face){
        for(uint32_t i=0;i<mesh->vertex_count;i++){const Vertex *v=&mesh->vertices[i];if(!visible(v,depth,lo,hi,span*.02f))continue;
            float pu=v->photo_uv_valid?v->photo_uv[0]:(hi[0]-v->p[0])/(hi[0]-lo[0]),pv=v->photo_uv_valid?v->photo_uv[1]:(hi[1]-v->p[1])/(hi[1]-lo[1]);
            float u=((bounds.x0+pu*(bounds.x1-bounds.x0))/reference.width-rb[0])/rb[2],vv=((bounds.y0+pv*(bounds.y1-bounds.y0))/reference.height-rb[1])/rb[3];
            if(u<0||u>1||vv<0||vv>1)continue;
            projection[0]=fminf(projection[0],v->p[0]);projection[1]=fmaxf(projection[1],v->p[0]);projection[2]=fminf(projection[2],v->p[1]);projection[3]=fmaxf(projection[3],v->p[1]);
        }
        /* Keep the input face aspect ratio; a reference's long cheek/chest
         * box must not stretch the kitten's eyes and mouth vertically. */
        if(projection[1]>projection[0]&&projection[3]>projection[2]){
            float center=(projection[2]+projection[3])*.5f;
            float half=.5f*(projection[1]-projection[0])*(sb[3]*source.height)/(sb[2]*source.width);
            half=clip(half,.35f*(projection[3]-projection[2]),.60f*(projection[3]-projection[2]));
            projection[2]=center-half;projection[3]=center+half;
        }
        job->face_projection=projection[1]-projection[0]>span*.05f&&projection[3]-projection[2]>span*.05f;
        if(job->face_projection){memcpy(job->face_xy,projection,16);memcpy(job->face_box,sb,16);}
    }
    if(!smooth_face_surface(job,mesh,depth,lo,hi,span))goto done;
    near_depth(mesh,depth,lo,hi,NULL);
    float coat[3]={0};int coat_count=0;
    if(has_source_face)for(int side=0;side<2;side++)for(int row=0;row<5;row++){
        float rgb[3];if(sample(&source,(sb[0]+sb[2]*(side?.92f:.08f))*source.width,(sb[1]+sb[3]*(.15f+row*.14f))*source.height,rgb)&&luma(rgb)>55){for(int k=0;k<3;k++)coat[k]+=rgb[k];coat_count++;}}
    if(coat_count)for(int k=0;k<3;k++)coat[k]/=coat_count;
    for(uint32_t i=0;i<mesh->vertex_count;i++){
        if(!(i%4096)&&stopped(job))goto done;Vertex *v=&mesh->vertices[i];
        float px=bounds.x0+(v->photo_uv_valid?v->photo_uv[0]:(hi[0]-v->p[0])/(hi[0]-lo[0]))*(bounds.x1-bounds.x0);
        float py=bounds.y0+(v->photo_uv_valid?v->photo_uv[1]:(hi[1]-v->p[1])/(hi[1]-lo[1]))*(bounds.y1-bounds.y0),ref_rgb[3];
        float head_weight=0;
        if(has_source_face&&has_reference_face){float hu=(px/reference.width-rb[0])/rb[2],hv=(py/reference.height-rb[1])/rb[3];
            head_weight=smooth((1.35f-sqrtf(powf((hu-.5f)/.75f,2)+powf((hv-.42f)/.92f,2)))/.3f);v->smooth=head_weight;
            /* Reference eyes/nose must not survive beside the input face or
             * on the back of the head. Only the source supplies face features. */
            if(job->shape_used&&coat_count)for(int k=0;k<3;k++)v->color[k]=(unsigned char)clip((1-head_weight)*v->color[k]+head_weight*coat[k],0,255);
        }
        if(!visible(v,depth,lo,hi,span*.012f))continue;
        if(sample(&reference,px,py,ref_rgb)){
            int bin=(int)clip(luma(ref_rgb),0,255);for(int k=0;k<3;k++){float c=job->shape_used?palette[bin][k]:ref_rgb[k];if(job->shape_used&&coat_count)c=c*(1-head_weight)+coat[k]*head_weight;v->color[k]=(unsigned char)clip(c,0,255);}
        }
        if(!has_source_face||!has_reference_face)continue;
        float u=(px/reference.width-rb[0])/rb[2],vv=(py/reference.height-rb[1])/rb[3];
        if(job->body_fit.mirrored)u=1-u;
        if(job->face_projection){u=(projection[1]-v->p[0])/(projection[1]-projection[0]);vv=(projection[3]-v->p[1])/(projection[3]-projection[2]);}
        float weight=smooth((u+.16f)/.16f)*smooth((1.16f-u)/.16f)*smooth((vv+.4f)/.2f)*smooth((1.12f-vv)/.16f);
        if(weight<.01f)continue;float rgb[3];if(!sample(&source,(sb[0]+u*sb[2])*source.width,(sb[1]+vv*sb[3])*source.height,rgb))continue;
        v->face=weight;v->source_uv[0]=sb[0]+u*sb[2];v->source_uv[1]=sb[1]+vv*sb[3];
        for(int k=0;k<3;k++)v->color[k]=(unsigned char)clip(v->color[k]*(1-weight)+rgb[k]*weight,0,255);
        float feature_weight=0;
        for(int eye=0;eye<2;eye++)feature_weight+=8*gauss(u,vv,features[eye][0],features[eye][1],.13f,.085f);
        feature_weight+=5*gauss(u,vv,features[2][0],features[2][1]+.055f,.14f,.11f);
        v->detail=fmaxf(v->detail,weight*(1+feature_weight));job->face_detail_vertices++;
        float width=rb[2]*reference.width/(bounds.x1-bounds.x0)*(hi[0]-lo[0]);
        float nx=features[2][0],ny=features[2][1];
        float relief=.045f*gauss(u,vv,nx,ny,.105f,.085f);
        relief+=.018f*(gauss(u,vv,nx-.14f,ny+.10f,.15f,.10f)+gauss(u,vv,nx+.14f,ny+.10f,.15f,.10f));
        for(int eye=0;eye<2;eye++)relief-=.012f*gauss(u,vv,features[eye][0],features[eye][1],.11f,.065f);
        /* Dark observed lip pixels form a shallow groove below the nose. */
        if(vv>ny+.035f&&vv<ny+.24f&&fabsf(u-nx)<.22f)relief-=.007f*smooth((100-luma(rgb))/65)*gauss(u,vv,nx,ny+.12f,.2f,.08f);
        delta[i][2]=-width*clip(relief,-.015f,.055f)*weight;
    }
    if(safe_move(mesh,delta)>0)for(uint32_t i=0;i<mesh->vertex_count;i++)job->face_relief_displacement=fmax(job->face_relief_displacement,fabs(delta[i][2]));
success:ok=1;
done:cJSON_Delete(source_info);cJSON_Delete(analysis);cJSON_Delete(completion);photo_free(&source);photo_free(&reference);free(depth);free(delta);return ok;
}
int mesh_finish_face(Job *job,const wchar_t *directory,Mesh *mesh){
    int any=0;for(uint32_t i=0;i<mesh->vertex_count;i++)if(mesh->vertices[i].face>.01f){any=1;break;}if(!any)return 1;
    wchar_t path[FS_PATH];Photo source={0};fs_join(path,directory,L"cutout.rgba");if(!photo_read(path,&source,job->error))return 0;
    for(uint32_t i=0;i<mesh->vertex_count;i++){if(!(i%4096)&&stopped(job)){photo_free(&source);return 0;}Vertex *v=&mesh->vertices[i];float rgb[3];
        if(v->face>.01f&&job->face_projection){v->source_uv[0]=job->face_box[0]+(job->face_xy[1]-v->p[0])/(job->face_xy[1]-job->face_xy[0])*job->face_box[2];v->source_uv[1]=job->face_box[1]+(job->face_xy[3]-v->p[1])/(job->face_xy[3]-job->face_xy[2])*job->face_box[3];}
        if(v->face>.01f&&sample(&source,v->source_uv[0]*source.width,v->source_uv[1]*source.height,rgb))for(int k=0;k<3;k++)v->color[k]=(unsigned char)clip((1-v->face)*v->color[k]+v->face*rgb[k],0,255);
    }photo_free(&source);return 1;
}
