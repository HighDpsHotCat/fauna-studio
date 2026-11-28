/* C17 source-proportion fitting. A monotone photo-plane warp preserves the
 * reference's open limb gaps and mesh connectivity. These are inferred shapes;
 * neither the warp nor the branch count identifies a hidden anatomical joint. */
#include "fauna.h"
#include "../vendor/cjson/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

typedef struct { int x0,y0,x1,y1; float face[4]; } Profile;
static float clamp(float x,float a,float b){return fmaxf(a,fminf(b,x));}
static float smooth(float x){x=clamp(x,0,1);return x*x*(3-2*x);}
static cJSON *read_json(const wchar_t *dir,const wchar_t *name){
    wchar_t path[FS_PATH];fs_join(path,dir,name);FILE *f=fs_wfopen(path,L"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<1||n>20000000){fclose(f);return NULL;}
    char *s=malloc((size_t)n+1);if(!s){fclose(f);return NULL;}size_t got=fread(s,1,n,f);fclose(f);s[got]=0;
    cJSON *r=got==(size_t)n?cJSON_Parse(s):NULL;free(s);return r;
}
static int profile(const Photo *photo,const cJSON *context,Profile *p){
    memset(p,0,sizeof(*p));p->x0=photo->width;p->y0=photo->height;
    for(uint32_t y=0;y<photo->height;y++)for(uint32_t x=0;x<photo->width;x++)if(photo->rgba[((size_t)y*photo->width+x)*4+3]>64){
        p->x0=(int)fmin(p->x0,x);p->x1=(int)fmax(p->x1,x);p->y0=(int)fmin(p->y0,y);p->y1=(int)fmax(p->y1,y);
    }
    if(p->x1-p->x0<12||p->y1-p->y0<12)return 0;
    const cJSON *face=cJSON_GetObjectItemCaseSensitive(context,"faceBox");if(cJSON_GetArraySize(face)!=4)return 0;
    for(int k=0;k<4;k++){double v=cJSON_GetNumberValue(cJSON_GetArrayItem(face,k));if(!isfinite(v)||v<0||v>1)return 0;p->face[k]=(float)v;}
    p->face[0]=(p->face[0]*photo->width-p->x0)/(p->x1-p->x0);
    p->face[1]=(p->face[1]*photo->height-p->y0)/(p->y1-p->y0);
    p->face[2]*=(float)photo->width/(p->x1-p->x0);p->face[3]*=(float)photo->height/(p->y1-p->y0);
    if(p->face[1]<.03f||p->face[1]+p->face[3]>.93f||p->face[2]<.08f||p->face[3]<.08f)return 0;
    return 1;
}
/* Monotone cubic Hermite interpolation keeps vertical ordering and has no
 * horizontal seams at the head/neck anchors. Positive X/Z scales preserve a
 * one-to-one deformation; there is no cutting or overlapping duplicate leg. */
static float vertical(float v,const float a[4],const float b[4]){
    float d[3],m[4];for(int i=0;i<3;i++)d[i]=(b[i+1]-b[i])/(a[i+1]-a[i]);
    m[0]=d[0];m[3]=d[2];for(int i=1;i<3;i++)m[i]=2*d[i-1]*d[i]/(d[i-1]+d[i]);
    int i=v<a[1]?0:(v<a[2]?1:2);float h=a[i+1]-a[i],t=clamp((v-a[i])/h,0,1),t2=t*t,t3=t2*t;
    return (2*t3-3*t2+1)*b[i]+(t3-2*t2+t)*h*m[i]+(-2*t3+3*t2)*b[i+1]+(t3-t2)*h*m[i+1];
}
static uint32_t root(uint32_t *p,uint32_t i){while(p[i]!=i){p[i]=p[p[i]];i=p[i];}return i;}
static int limb_scan(Mesh *m,int protect,FsCancel * cancel,wchar_t *error,float feet[4][3]){
    uint32_t n=m->vertex_count,*p=malloc((size_t)n*4),*count=calloc(n,4);float (*bounds)[6]=malloc((size_t)n*sizeof(*bounds));
    if(!p||!count||!bounds){free(p);free(count);free(bounds);fs_error(error,L"다리 간격 검사 메모리가 부족합니다.");return -1;}
    float lo[3],hi[3];mesh_bounds(m,lo,hi);float height=hi[1]-lo[1],span=fmaxf(height,fmaxf(hi[0]-lo[0],hi[2]-lo[2]));int best=0;
    for(int level=0;level<15;level++){
        if(cancel&&fs_cancel_wait(cancel,0)==WAIT_OBJECT_0){fs_error(error,L"생성을 취소했습니다.");best=-1;break;}
        float cut=lo[1]+height*(.08f+level*.025f);
        memset(count,0,(size_t)n*4);for(uint32_t i=0;i<n;i++){p[i]=i;for(int k=0;k<3;k++){bounds[i][k]=FLT_MAX;bounds[i][k+3]=-FLT_MAX;}}
        for(uint32_t t=0;t<m->triangle_count;t++)for(int k=0;k<3;k++){
            uint32_t a=m->triangles[t].v[k],b=m->triangles[t].v[(k+1)%3];
            if(m->vertices[a].p[1]<cut&&m->vertices[b].p[1]<cut)p[root(p,b)]=root(p,a);
        }
        for(uint32_t i=0;i<n;i++)if(m->vertices[i].p[1]<cut){uint32_t r=root(p,i);count[r]++;
            for(int k=0;k<3;k++){bounds[r][k]=fminf(bounds[r][k],m->vertices[i].p[k]);bounds[r][k+3]=fmaxf(bounds[r][k+3],m->vertices[i].p[k]);}}
        int branches=0;for(uint32_t i=0;i<n;i++)if(count[i]>=(m->triangle_count>10000?12u:4u)&&bounds[i][4]-bounds[i][1]>height*.02f&&
            bounds[i][3]-bounds[i][0]<span*.32f&&bounds[i][5]-bounds[i][2]<span*.48f){count[i]|=0x80000000u;branches++;}
        if(branches>=2&&branches<=4&&branches>=best){best=branches;
            if(branches==4&&feet){
                uint32_t roots[4],samples[4]={0};int k=0;for(uint32_t i=0;i<n;i++)if(count[i]&0x80000000u)roots[k++]=i;
                memset(feet,0,4*3*sizeof(float));
                for(uint32_t i=0;i<n;i++)if(m->vertices[i].p[1]<cut){uint32_t r=root(p,i);for(k=0;k<4;k++)if(r==roots[k]&&m->vertices[i].p[1]<bounds[r][1]+span*.01f){samples[k]++;for(int c=0;c<3;c++)feet[k][c]+=m->vertices[i].p[c];}}
                for(k=0;k<4;k++)for(int c=0;c<3;c++)feet[k][c]/=samples[k]?samples[k]:1;
            }
            if(protect){uint32_t roots[4],k=0;for(uint32_t i=0;i<n;i++)if(count[i]&0x80000000u)roots[k++]=i;
                for(uint32_t i=0;i<n;i++){m->vertices[i].limb=0;m->vertices[i].limb_id=0;
                    if(m->vertices[i].p[1]<cut)for(k=0;k<(uint32_t)branches;k++)if(root(p,i)==roots[k]){m->vertices[i].limb=1;m->vertices[i].limb_id=k+1;}}
            }
        }
    }
    free(p);free(count);free(bounds);return best;
}
int mesh_limb_branches(Mesh *m,int protect,FsCancel * cancel,wchar_t *error){return limb_scan(m,protect,cancel,error,NULL);}
int mesh_ground_body(Job *job,Mesh *mesh){
    if(!job->body_fit.applied||job->body_fit.separated_branches!=4)return 1;
    float feet[4][3],lo[3],hi[3];int count=limb_scan(mesh,1,job->cancel,job->error,feet);if(count<0)return 0;if(count!=4)return 1;
    mesh_bounds(mesh,lo,hi);double center[3]={0};for(int i=0;i<4;i++)for(int k=0;k<3;k++)center[k]+=feet[i][k]*.25;
    double xx=0,zz=0,xz=0,xy=0,zy=0;for(int i=0;i<4;i++){double x=feet[i][0]-center[0],y=feet[i][1]-center[1],z=feet[i][2]-center[2];xx+=x*x;zz+=z*z;xz+=x*z;xy+=x*y;zy+=z*y;}
    double determinant=xx*zz-xz*xz;if(determinant<1e-8)return 1;
    double a=(xy*zz-zy*xz)/determinant,b=(zy*xx-xy*xz)/determinant,angle=atan(hypot(a,b))*180/3.141592653589793;
    /* Four usable feet are evidence of a ground plane, not an instruction to
     * synthesize four feet. Reject ill-conditioned/extreme camera estimates. */
    if(angle<3||angle>40)return 1;
    double norm=sqrt(1+a*a+b*b),side=sqrt(1+a*a),basis[3][3]={{1/side,a/side,0},{-a/norm,1/norm,-b/norm},{-a*b/(side*norm),b/(side*norm),side/norm}};
    double before_lo=DBL_MAX,before_hi=-DBL_MAX,after_lo=DBL_MAX,after_hi=-DBL_MAX;
    for(int i=0;i<4;i++){double y=0;for(int k=0;k<3;k++)y+=basis[1][k]*(feet[i][k]-center[k]);before_lo=fmin(before_lo,feet[i][1]);before_hi=fmax(before_hi,feet[i][1]);after_lo=fmin(after_lo,y);after_hi=fmax(after_hi,y);}
    if(after_hi-after_lo>(before_hi-before_lo)*.55||after_hi-after_lo>(hi[1]-lo[1])*.08)return 1;
    for(uint32_t i=0;i<mesh->vertex_count;i++){float p[3];memcpy(p,mesh->vertices[i].p,12);for(int k=0;k<3;k++){double v=center[k];for(int c=0;c<3;c++)v+=basis[k][c]*(p[c]-center[c]);mesh->vertices[i].p[k]=(float)v;}}
    mesh_normals(mesh);job->body_fit.ground_aligned=1;job->body_fit.ground_tilt_degrees=angle;
    job->body_fit.foot_height_spread_before=before_hi-before_lo;job->body_fit.foot_height_spread_after=after_hi-after_lo;return mesh_validate(mesh,job->error);
}
static double photo_iou(const Mesh *m,const Photo *photo,const Profile *p,const float lo[3],const float hi[3]){
    enum{N=192};unsigned char mask[N*N]={0};
    for(uint32_t i=0;i<m->triangle_count;i++){
        float x[3],y[3];for(int k=0;k<3;k++){const float *v=m->vertices[m->triangles[i].v[k]].p;x[k]=(hi[0]-v[0])/(hi[0]-lo[0])*(N-1);y[k]=(hi[1]-v[1])/(hi[1]-lo[1])*(N-1);}
        float det=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);if(fabsf(det)<1e-10f)continue;
        int xl=(int)clamp(floorf(fminf(x[0],fminf(x[1],x[2]))),0,N-1),xr=(int)clamp(ceilf(fmaxf(x[0],fmaxf(x[1],x[2]))),0,N-1);
        int yl=(int)clamp(floorf(fminf(y[0],fminf(y[1],y[2]))),0,N-1),yr=(int)clamp(ceilf(fmaxf(y[0],fmaxf(y[1],y[2]))),0,N-1);
        for(int yy=yl;yy<=yr;yy++)for(int xx=xl;xx<=xr;xx++){float a=((y[1]-y[2])*(xx-x[2])+(x[2]-x[1])*(yy-y[2]))/det,b=((y[2]-y[0])*(xx-x[2])+(x[0]-x[2])*(yy-y[2]))/det;if(a>=0&&b>=0&&a+b<=1)mask[yy*N+xx]=1;}
    }
    int both=0,either=0;for(int y=0;y<N;y++)for(int x=0;x<N;x++){
        int px=p->x0+x*(p->x1-p->x0)/(N-1),py=p->y0+y*(p->y1-p->y0)/(N-1);
        int a=photo->rgba[((size_t)py*photo->width+px)*4+3]>64,b=mask[y*N+x];both+=a&&b;either+=a||b;
    }return either?(double)both/either:0;
}
int mesh_fit_body(Job *job,const wchar_t *directory,int reference_index,Mesh *mesh,BodyFit *fit){
    cJSON *source_info=read_json(directory,L"input-analysis.json"),*analysis=read_json(directory,L"reference-analysis.json");Photo source={0},reference={0};Mesh candidate={0};int ok=0;
    memset(fit,0,sizeof(*fit));if(fs_cancelled(job)){fs_error(job->error,L"생성을 취소했습니다.");goto done;}
    const cJSON *ref=cJSON_GetArrayItem(analysis,reference_index),*ctx=cJSON_GetObjectItemCaseSensitive(ref,"context");
    const char *mask=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ref,"mask"));
    if(!cJSON_HasObjectItem(source_info,"faceBox")||!cJSON_HasObjectItem(ctx,"faceBox")||!mask)goto success;
    wchar_t path[FS_PATH],name[256];if(strchr(mask,'/')||strchr(mask,'\\')||!fs_decode_utf8(CP_UTF8,0,mask,-1,name,256))goto success;
    fs_join(path,directory,name);if(!photo_read(path,&reference,job->error))goto done;fs_join(path,directory,L"cutout.rgba");if(!photo_read(path,&source,job->error))goto done;
    Profile s,r;if(!profile(&source,source_info,&s)||!profile(&reference,ctx,&r))goto success;
    float a[4]={0,r.face[1],r.face[1]+r.face[3],1},b[4]={0,s.face[1],s.face[1]+s.face[3],1};
    float scale=s.face[3]/r.face[3];if(scale<.45f||scale>2.6f)goto success;
    float lo[3],hi[3];mesh_bounds(mesh,lo,hi);float target_lo[3],target_hi[3];memcpy(target_lo,lo,12);memcpy(target_hi,hi,12);
    /* Detection boxes include some cheek/chest. Limit the fit so a large box
     * cannot crush the feet into a plate or erase a naturally open gap. */
    for(int k=1;k<3;k++)b[k]=.55f*a[k]+.45f*b[k];
    float height=hi[1]-lo[1],width=height*(s.x1-s.x0)/(s.y1-s.y0),center=(lo[0]+hi[0])*.5f;
    width=clamp(width,.85f*(hi[0]-lo[0]),1.15f*(hi[0]-lo[0]));
    target_lo[0]=center-width*.5f;target_hi[0]=center+width*.5f;
    fit->silhouette_before=photo_iou(mesh,&source,&s,target_lo,target_hi);
    float sf=s.face[0]+s.face[2]*.5f,rf=r.face[0]+r.face[2]*.5f;
    int mirror=(sf-.5f)*(rf-.5f)<-.008f;
    if(!mesh_copy(mesh,&candidate)){fs_error(job->error,L"체형 보정 메모리가 부족합니다.");goto done;}
    for(uint32_t i=0;i<candidate.vertex_count;i++){
        Vertex *v=&candidate.vertices[i];float u=(hi[0]-v->p[0])/(hi[0]-lo[0]),vv=(hi[1]-v->p[1])/height;
        v->photo_uv[0]=u;v->photo_uv[1]=vv;v->photo_uv_valid=1;
        float y=vertical(vv,a,b);
        if(mirror)u=1-u;
        /* Do not align individual silhouette rows: an ear tip or a single
         * paw can jump between sides in a row, producing spikes. A broad
         * head-to-body transition preserves all small limb cross-sections. */
        float head=smooth((a[2]+.16f-vv)/.24f);
        float rc=.5f,sc=.5f,xscale=1.f;
        float face_u=mirror?1-rf:rf,face_scale=clamp(1+.45f*(s.face[2]/r.face[2]-1),.8f,1.3f);
        float face_center=.55f*face_u+.45f*sf;
        float mapped=(u-rc)*xscale+sc;
        mapped=mapped*(1-head)+((u-face_u)*face_scale+face_center)*head;
        v->p[0]=target_hi[0]-mapped*width;v->p[1]=hi[1]-y*height;
        /* Scale depth with the local width, bounded so a large kitten face
         * does not inflate the adult prior's entire abdomen. */
        float depth_scale=clamp(((1-head)*xscale+head*face_scale)*width/(hi[0]-lo[0]),.7f,1.65f);
        v->p[2]=(lo[2]+hi[2])*.5f+(v->p[2]-(lo[2]+hi[2])*.5f)*sqrtf(depth_scale);
    }
    if(mirror)for(uint32_t t=0;t<candidate.triangle_count;t++){uint32_t tmp=candidate.triangles[t].v[1];candidate.triangles[t].v[1]=candidate.triangles[t].v[2];candidate.triangles[t].v[2]=tmp;}
    mesh_normals(&candidate);if(!mesh_validate(&candidate,job->error))goto done;
    fit->silhouette_after=photo_iou(&candidate,&source,&s,target_lo,target_hi);
    if(fit->silhouette_after<fit->silhouette_before+.025||fit->silhouette_after<.55)goto success;
    fit->applied=1;fit->mirrored=mirror;fit->head_height_scale=(b[2]-b[1])/(a[2]-a[1]);mesh_free(mesh);*mesh=candidate;memset(&candidate,0,sizeof(candidate));
success:
    fit->separated_branches=mesh_limb_branches(mesh,1,job->cancel,job->error);if(fit->separated_branches<0)goto done;ok=1;
done:cJSON_Delete(source_info);cJSON_Delete(analysis);photo_free(&source);photo_free(&reference);mesh_free(&candidate);return ok;
}
