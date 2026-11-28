/* C17 volumetric reference completion. A registered TripoSR reference head
 * replaces the photo-depth relief locally. Standing legs use source landmarks
 * and the reference's visible paw cross-section. This is an anatomical prior,
 * not measured reconstruction of the hidden individual. */
#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>

typedef struct {int nx,ny,nz;size_t count;float origin[3],step;} Grid;
typedef struct {float z;int direction;} Crossing;
typedef struct {uint64_t key;uint32_t vertex;} EdgeEntry;
typedef struct {Mesh mesh;uint32_t vc,tc;EdgeEntry *edges;size_t capacity,used;} Builder;
static float clamp(float x,float a,float b){return fmaxf(a,fminf(b,x));}
static float ease(float x){x=clamp(x,0,1);return x*x*(3-2*x);}
static float dot(const float a[3],const float b[3]){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static float length(const float p[3]){return sqrtf(dot(p,p));}
static void cross(const float a[3],const float b[3],float n[3]){n[0]=a[1]*b[2]-a[2]*b[1];n[1]=a[2]*b[0]-a[0]*b[2];n[2]=a[0]*b[1]-a[1]*b[0];}
static size_t cell(const Grid *g,int x,int y,int z){return (size_t)x+(size_t)g->nx*(y+(size_t)g->ny*z);}
static int crop(const Photo *p,int bounds[4]){
    bounds[0]=p->width;bounds[1]=p->height;bounds[2]=bounds[3]=-1;if(!p->rgba)return 0;
    for(uint32_t y=0;y<p->height;y++)for(uint32_t x=0;x<p->width;x++)if(p->rgba[((size_t)y*p->width+x)*4+3]>32){
        if((int)x<bounds[0])bounds[0]=x;if((int)y<bounds[1])bounds[1]=y;if((int)x>bounds[2])bounds[2]=x;if((int)y>bounds[3])bounds[3]=y;}
    return bounds[2]-bounds[0]>8&&bounds[3]-bounds[1]>8;
}
/* A ray through actual triangles lifts photo landmarks onto the inferred mesh. */
static int ray(const Mesh *m,float x,float y,float z[2]){
    z[0]=FLT_MAX;z[1]=-FLT_MAX;
    for(uint32_t i=0;i<m->triangle_count;i++){
        const float *a=m->vertices[m->triangles[i].v[0]].p,*b=m->vertices[m->triangles[i].v[1]].p,*c=m->vertices[m->triangles[i].v[2]].p;
        float d=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1]);if(fabsf(d)<1e-12f)continue;
        float u=((b[1]-c[1])*(x-c[0])+(c[0]-b[0])*(y-c[1]))/d,v=((c[1]-a[1])*(x-c[0])+(a[0]-c[0])*(y-c[1]))/d;
        if(u<0||v<0||u+v>1)continue;float value=u*a[2]+v*b[2]+(1-u-v)*c[2];z[0]=fminf(z[0],value);z[1]=fmaxf(z[1],value);
    }
    if(z[0]!=FLT_MAX)return 1;
    float distance=FLT_MAX;for(uint32_t i=0;i<m->vertex_count;i++){const float *p=m->vertices[i].p;float d=(p[0]-x)*(p[0]-x)+(p[1]-y)*(p[1]-y);if(d<distance){distance=d;z[0]=z[1]=p[2];}}
    return 0;
}
static int lift(const Mesh *m,const Photo *p,const AnimalPose *pose,float points[17][3]){
    int b[4];if(!crop(p,b))return 0;float lo[3],hi[3];mesh_bounds(m,lo,hi);
    for(int i=0;i<17;i++){
        float u=(pose->xy[i][0]*p->width-b[0])/(b[2]-b[0]),v=(pose->xy[i][1]*p->height-b[1])/(b[3]-b[1]);
        if(!isfinite(u)||!isfinite(v))return 0;
        if(i<4&&(u<-.2f||u>1.2f||v<-.2f||v>1.2f))return 0;
        points[i][0]=hi[0]-u*(hi[0]-lo[0]);points[i][1]=hi[1]-v*(hi[1]-lo[1]);float z[2];ray(m,points[i][0],points[i][1],z);points[i][2]=z[0];
    }return 1;
}
static int frame(const float p[17][3],float center[3],float basis[3][3],float *eye_distance){
    for(int k=0;k<3;k++){center[k]=(p[0][k]+p[1][k])*.5f;basis[0][k]=p[1][k]-p[0][k];basis[1][k]=center[k]-p[3][k];}
    *eye_distance=length(basis[0]);if(*eye_distance<1e-5f)return 0;
    for(int k=0;k<3;k++)basis[0][k]/=*eye_distance;float along=dot(basis[0],basis[1]);for(int k=0;k<3;k++)basis[1][k]-=along*basis[0][k];
    float up=length(basis[1]);if(up<1e-5f)return 0;for(int k=0;k<3;k++)basis[1][k]/=up;cross(basis[0],basis[1],basis[2]);return 1;
}
static int crossing_order(const void *a,const void *b){float x=((const Crossing*)a)->z,y=((const Crossing*)b)->z;return x==y?0:(x<y?-1:1);}
static int blur(Job *j,const Grid *g,float *field,float sigma){
    float *temp=malloc(g->count*sizeof(float)),kernel[17],sum=0;int r=(int)ceilf(3*sigma);if(r>8)r=8;if(!temp)return 0;
    for(int k=-r;k<=r;k++){kernel[k+r]=expf(-.5f*k*k/(sigma*sigma));sum+=kernel[k+r];}for(int k=0;k<=2*r;k++)kernel[k]/=sum;
    for(int axis=0;axis<3;axis++){
        for(int z=0;z<g->nz;z++){if(fs_cancelled(j)){free(temp);return 0;}for(int y=0;y<g->ny;y++)for(int x=0;x<g->nx;x++){
            float value=0;for(int k=-r;k<=r;k++){int xx=x,yy=y,zz=z;if(axis==0)xx=(int)clamp((float)(x+k),0,(float)g->nx-1);if(axis==1)yy=(int)clamp((float)(y+k),0,(float)g->ny-1);if(axis==2)zz=(int)clamp((float)(z+k),0,(float)g->nz-1);value+=kernel[k+r]*field[cell(g,xx,yy,zz)];}temp[cell(g,x,y,z)]=value;
        }}memcpy(field,temp,g->count*sizeof(float));
    }free(temp);return 1;
}
/* Multiple ray crossings preserve separate front/rear legs and cavities. */
static int mesh_field(Job *j,const Grid *g,const Mesh *m,float *field){
    enum{MAX_CROSS=48};size_t columns=(size_t)g->nx*g->ny;Crossing *rays=calloc(columns*MAX_CROSS,sizeof(*rays));
    unsigned char *counts=calloc(columns,1),*inside=calloc(g->count,1);int ok=0;if(!rays||!counts||!inside)goto done;
    for(uint32_t t=0;t<m->triangle_count;t++){
        if(!(t%4096)&&fs_cancelled(j))goto done;float x[3],y[3],z[3];
        for(int k=0;k<3;k++){const float *p=m->vertices[m->triangles[t].v[k]].p;x[k]=(p[0]-g->origin[0])/g->step;y[k]=(p[1]-g->origin[1])/g->step;z[k]=p[2];}
        float d=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);if(fabsf(d)<1e-10f)continue;
        int xl=(int)clamp(ceilf(fminf(x[0],fminf(x[1],x[2]))),0,(float)g->nx-1),xr=(int)clamp(floorf(fmaxf(x[0],fmaxf(x[1],x[2]))),0,(float)g->nx-1);
        int yl=(int)clamp(ceilf(fminf(y[0],fminf(y[1],y[2]))),0,(float)g->ny-1),yr=(int)clamp(floorf(fmaxf(y[0],fmaxf(y[1],y[2]))),0,(float)g->ny-1);
        for(int yy=yl;yy<=yr;yy++)for(int xx=xl;xx<=xr;xx++){
            float a=((y[1]-y[2])*(xx-x[2])+(x[2]-x[1])*(yy-y[2]))/d,b=((y[2]-y[0])*(xx-x[2])+(x[0]-x[2])*(yy-y[2]))/d;
            if(a< -1e-6f||b< -1e-6f||a+b>1.000001f)continue;size_t col=(size_t)yy*g->nx+xx;if(counts[col]>=MAX_CROSS)goto done;
            rays[col*MAX_CROSS+counts[col]++]=(Crossing){a*z[0]+b*z[1]+(1-a-b)*z[2],d<0?1:-1};
        }
    }
    size_t filled=0;
    for(int y=0;y<g->ny;y++)for(int x=0;x<g->nx;x++){
        size_t col=(size_t)y*g->nx+x;Crossing *r=rays+col*MAX_CROSS;int n=counts[col];qsort(r,n,sizeof(*r),crossing_order);
        int live=0;for(int a=0;a<n;){int b=a+1,sign=r[a].direction;while(b<n&&fabsf(r[b].z-r[a].z)<g->step*1e-4f)sign+=r[b++].direction;if(sign)r[live++]=(Crossing){r[a].z,sign>0?1:-1};a=b;}
        int at=0,winding=0;for(int z=0;z<g->nz;z++){float value=g->origin[2]+z*g->step;while(at<live&&r[at].z<=value)winding+=r[at++].direction;if(winding){inside[cell(g,x,y,z)]=1;filled++;}}
    }
    if(filled<100||filled>g->count*.9)goto done;
    for(int z=0;z<g->nz;z++)for(int y=0;y<g->ny;y++)for(int x=0;x<g->nx;x++){
        size_t i=cell(g,x,y,z);int edge=(x&&inside[i]!=inside[i-1])||(x+1<g->nx&&inside[i]!=inside[i+1])||(y&&inside[i]!=inside[i-g->nx])||(y+1<g->ny&&inside[i]!=inside[i+g->nx])||(z&&inside[i]!=inside[i-columns])||(z+1<g->nz&&inside[i]!=inside[i+columns]);
        field[i]=edge?.5f:1e6f;
    }
    for(int pass=0;pass<2;pass++)for(int zz=0;zz<g->nz;zz++){
        if(fs_cancelled(j))goto done;int z=pass?g->nz-1-zz:zz;
        for(int yy=0;yy<g->ny;yy++)for(int xx=0;xx<g->nx;xx++){
            int y=pass?g->ny-1-yy:yy,x=pass?g->nx-1-xx:xx;size_t i=cell(g,x,y,z);float best=field[i];
            for(int dz=-1;dz<=0;dz++)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
                if(dz==0&&(dy>0||(dy==0&&dx>=0)))continue;int a=x+(pass?-dx:dx),b=y+(pass?-dy:dy),c=z+(pass?-dz:dz);
                if(a<0||b<0||c<0||a>=g->nx||b>=g->ny||c>=g->nz)continue;float value=field[cell(g,a,b,c)]+sqrtf((float)(dx*dx+dy*dy+dz*dz));best=fminf(best,value);
            }field[i]=best;
        }
    }
    for(size_t i=0;i<g->count;i++)field[i]*=g->step*(inside[i]?-1.f:1.f);ok=1;
done:free(rays);free(counts);free(inside);return ok;
}
static float smin(float a,float b,float k){float h=clamp(.5f+.5f*(b-a)/k,0,1);return b*(1-h)+a*h-k*h*(1-h);}
static float capsule(const float p[3],const float a[3],const float b[3],float ra,float rb){
    float v[3],d[3];for(int k=0;k<3;k++){v[k]=p[k]-a[k];d[k]=b[k]-a[k];}float t=clamp(dot(v,d)/fmaxf(dot(d,d),1e-10f),0,1);for(int k=0;k<3;k++)v[k]-=t*d[k];return length(v)-(ra*(1-t)+rb*t);
}
/* Estimate visible paw width from the reference mask; retain its uncertainty
 * through conservative scale limits. No measurement in real-world units. */
static float paw_radius(const Photo *p,const AnimalPose *pose,float target_height,float unit){
    int bounds[4];if(!crop(p,bounds))return 0;const int ids[4]={7,10,13,16};float radii[4];int n=0;
    for(int k=0;k<4;k++){
        int id=ids[k];if(pose->score[id]<.4f)continue;int px=(int)(pose->xy[id][0]*p->width),py=(int)(pose->xy[id][1]*p->height),best=INT_MAX,bx=-1,by=-1;
        int radius=(int)fmaxf(3,.04f*(bounds[3]-bounds[1]));
        for(int dy=-radius;dy<=radius;dy++)for(int dx=-radius;dx<=radius;dx++){int x=px+dx,y=py+dy;if(x<0||y<0||x>=(int)p->width||y>=(int)p->height)continue;
            int d=dx*dx+dy*dy;if(d<best&&p->rgba[((size_t)y*p->width+x)*4+3]>128){best=d;bx=x;by=y;}}
        if(bx<0)continue;int left=bx,right=bx;while(left>0&&p->rgba[((size_t)by*p->width+left-1)*4+3]>128)left--;while(right+1<(int)p->width&&p->rgba[((size_t)by*p->width+right+1)*4+3]>128)right++;
        if(right-left>2&&right-left<(bounds[2]-bounds[0])*.22f)radii[n++]=.5f*(right-left+1)/(bounds[3]-bounds[1])*target_height;
    }
    if(!n)return 0;for(int a=0;a<n;a++)for(int b=a+1;b<n;b++)if(radii[a]>radii[b]){float t=radii[a];radii[a]=radii[b];radii[b]=t;}
    return clamp(radii[n/2],.018f*unit,.043f*unit);
}
static size_t edge_slot(uint64_t key,size_t capacity){key^=key>>33;key*=0xff51afd7ed558ccdULL;key^=key>>33;return (size_t)key&(capacity-1);}
static int grow_edges(Builder *b){
    size_t capacity=b->capacity?b->capacity*2:262144;EdgeEntry *next=calloc(capacity,sizeof(*next));if(!next)return 0;
    for(size_t i=0;i<b->capacity;i++)if(b->edges[i].key){size_t k=edge_slot(b->edges[i].key,capacity);while(next[k].key)k=(k+1)&(capacity-1);next[k]=b->edges[i];}
    free(b->edges);b->edges=next;b->capacity=capacity;return 1;
}
static uint32_t edge_vertex(Builder *b,const Grid *g,uint32_t a,uint32_t c,float va,float vc){
    if(a>c){uint32_t t=a;a=c;c=t;float f=va;va=vc;vc=f;}uint64_t key=((uint64_t)a<<32)|c;
    if(!b->capacity||b->used*10>b->capacity*7)if(!grow_edges(b))return UINT32_MAX;
    size_t slot=edge_slot(key,b->capacity);while(b->edges[slot].key&&b->edges[slot].key!=key)slot=(slot+1)&(b->capacity-1);
    if(b->edges[slot].key)return b->edges[slot].vertex;
    if(b->mesh.vertex_count==b->vc){uint32_t capacity=b->vc?b->vc*2:131072;if(capacity>4000000)return UINT32_MAX;Vertex *next=realloc(b->mesh.vertices,(size_t)capacity*sizeof(*next));if(!next)return UINT32_MAX;b->mesh.vertices=next;b->vc=capacity;}
    uint32_t index=b->mesh.vertex_count++;Vertex *v=&b->mesh.vertices[index];memset(v,0,sizeof(*v));float t=clamp(va/(va-vc),0,1);
    uint32_t ax=a%g->nx,ay=(a/g->nx)%g->ny,az=a/(g->nx*g->ny),cx=c%g->nx,cy=(c/g->nx)%g->ny,cz=c/(g->nx*g->ny);
    float p[3]={(float)ax,(float)ay,(float)az},q[3]={(float)cx,(float)cy,(float)cz};for(int k=0;k<3;k++)v->p[k]=g->origin[k]+g->step*(p[k]+t*(q[k]-p[k]));v->color[3]=255;v->smooth=1;
    b->edges[slot]=(EdgeEntry){key,index};b->used++;return index;
}
static int triangle(Builder *b,uint32_t a,uint32_t c,uint32_t d,const float outward[3]){
    if(a==UINT32_MAX||c==UINT32_MAX||d==UINT32_MAX)return 0;
    if(b->mesh.triangle_count==b->tc){uint32_t capacity=b->tc?b->tc*2:262144;if(capacity>8000000)return 0;Triangle *next=realloc(b->mesh.triangles,(size_t)capacity*sizeof(*next));if(!next)return 0;b->mesh.triangles=next;b->tc=capacity;}
    float u[3],v[3],normal[3];for(int k=0;k<3;k++){u[k]=b->mesh.vertices[c].p[k]-b->mesh.vertices[a].p[k];v[k]=b->mesh.vertices[d].p[k]-b->mesh.vertices[a].p[k];}cross(u,v,normal);
    if(dot(normal,outward)<0){uint32_t t=c;c=d;d=t;}b->mesh.triangles[b->mesh.triangle_count++]=(Triangle){{a,c,d}};return 1;
}
static int extract(Job *j,const Grid *g,const float *field,Mesh *out){
    const int tetra[6][4]={{0,5,1,6},{0,1,2,6},{0,2,3,6},{0,3,7,6},{0,7,4,6},{0,4,5,6}};
    const int xyz[8][3]={{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}};Builder b={0};int ok=0;
    for(int z=0;z<g->nz-1;z++){if(fs_cancelled(j))goto done;for(int y=0;y<g->ny-1;y++)for(int x=0;x<g->nx-1;x++){
        /* Keep crossings away from grid corners in float32. Near-zero values
         * otherwise create coincident vertices and zero-area STL facets. */
        float epsilon=g->step*.02f;
        uint32_t ids[8];float values[8];int sum=0;for(int k=0;k<8;k++){ids[k]=(uint32_t)cell(g,x+xyz[k][0],y+xyz[k][1],z+xyz[k][2]);values[k]=field[ids[k]];if(fabsf(values[k])<epsilon)values[k]=epsilon;sum+=values[k]<0;}if(!sum||sum==8)continue;
        for(int t=0;t<6;t++){
            int in[4],outside[4],ni=0,no=0;float outward[3]={0};for(int k=0;k<4;k++){int c=tetra[t][k];if(values[c]<0)in[ni++]=c;else outside[no++]=c;}
            if(!ni||!no)continue;for(int k=0;k<3;k++){for(int a=0;a<no;a++)outward[k]+=(float)xyz[outside[a]][k]/no;for(int a=0;a<ni;a++)outward[k]-=(float)xyz[in[a]][k]/ni;}
            if(ni==1||no==1){int a=ni==1?in[0]:outside[0],*others=ni==1?outside:in;uint32_t v[3];for(int k=0;k<3;k++)v[k]=edge_vertex(&b,g,ids[a],ids[others[k]],values[a],values[others[k]]);if(!triangle(&b,v[0],v[1],v[2],outward))goto done;}
            else{uint32_t v[4];for(int a=0;a<2;a++)for(int c=0;c<2;c++)v[a*2+c]=edge_vertex(&b,g,ids[in[a]],ids[outside[c]],values[in[a]],values[outside[c]]);if(!triangle(&b,v[0],v[1],v[2],outward)||!triangle(&b,v[1],v[3],v[2],outward))goto done;}
        }
    }}*out=b.mesh;memset(&b.mesh,0,sizeof(b.mesh));mesh_normals(out);ok=mesh_validate(out,j->error);
done:free(b.edges);mesh_free(&b.mesh);return ok;
}
static int photo_colours(Job *j,const Photo *p,const Grid *g,const float *field,const float lo[3],const float hi[3],float neck,Mesh *m){
    int bounds[4];if(!crop(p,bounds))return 0;size_t count=(size_t)p->width*p->height;
    uint32_t *nearest=malloc(count*4),*queue=malloc(count*4);float *near_z=malloc((size_t)g->nx*g->ny*4);if(!nearest||!queue||!near_z){free(nearest);free(queue);free(near_z);return 0;}
    /* Grass left at the segmentation edge must not colour newly completed paws.
     * Apply this colour rule only to domestic dogs/cats, never green animals. */
    int pet=wcsstr(j->species,L"Canis")||wcsstr(j->species,L"Felis");
    size_t first=0,last=0;for(uint32_t i=0;i<count;i++){const unsigned char *c=p->rgba+4*i;nearest[i]=UINT32_MAX;int grass=pet&&c[1]>c[0]&&c[1]>c[2]*1.02f;if(c[3]>230&&!grass){nearest[i]=i;queue[last++]=i;}}
    while(first<last){uint32_t a=queue[first++],x=a%p->width,y=a/p->width,n[4]={x?a-1:UINT32_MAX,x+1<p->width?a+1:UINT32_MAX,y?a-p->width:UINT32_MAX,y+1<p->height?a+p->width:UINT32_MAX};for(int k=0;k<4;k++)if(n[k]!=UINT32_MAX&&nearest[n[k]]==UINT32_MAX){nearest[n[k]]=nearest[a];queue[last++]=n[k];}}
    for(int y=0;y<g->ny;y++)for(int x=0;x<g->nx;x++){float zvalue=FLT_MAX;for(int z=0;z<g->nz;z++)if(field[cell(g,x,y,z)]<0){zvalue=g->origin[2]+z*g->step;break;}near_z[y*g->nx+x]=zvalue;}
    for(uint32_t i=0;i<m->vertex_count;i++){
        if(!(i%4096)&&fs_cancelled(j)){free(nearest);free(queue);free(near_z);return 0;}Vertex *v=&m->vertices[i];
        int gx=(int)clamp(roundf((v->p[0]-g->origin[0])/g->step),0,(float)g->nx-1),gy=(int)clamp(roundf((v->p[1]-g->origin[1])/g->step),0,(float)g->ny-1);
        float body=ease((neck+.03f*(hi[1]-lo[1])-v->p[1])/(.12f*(hi[1]-lo[1])));
        float front=v->p[2]<=near_z[gy*g->nx+gx]+g->step*3?ease((-v->n[2]-.05f)/.4f):0;
        float weight=fmaxf(body,front);
        int grass=pet&&v->color[1]>v->color[0]&&v->color[1]>v->color[2]*1.02f;
        if(grass)weight=1;if(weight<.001f)continue;
        float u=(hi[0]-v->p[0])/(hi[0]-lo[0]),vv=(hi[1]-v->p[1])/(hi[1]-lo[1]);
        int x=(int)clamp(roundf(bounds[0]+u*(bounds[2]-bounds[0])),0,(float)p->width-1),y=(int)clamp(roundf(bounds[1]+vv*(bounds[3]-bounds[1])),0,(float)p->height-1);
        uint32_t pixel=nearest[(size_t)y*p->width+x];if(pixel==UINT32_MAX)continue;x=pixel%p->width;y=pixel/p->width;
        v->face=weight;v->observed=1;v->source_uv[0]=(float)x/(p->width-1);v->source_uv[1]=(float)y/(p->height-1);
        for(int k=0;k<3;k++)v->color[k]=(unsigned char)(v->color[k]*(1-v->face)+p->rgba[pixel*4+k]*v->face+.5f);
    }free(nearest);free(queue);free(near_z);return 1;
}
int mesh_volume(Job *j,const Photo *photo,const Mesh *base,const VolumeGuide *guide,Mesh *out){
    Mesh head={0};float *body_field=NULL,*head_field=NULL;int ok=0;memset(out,0,sizeof(*out));if(fs_cancelled(j))return 0;
    if(!guide->head.vertices||!guide->head_photo.rgba||guide->source_pose.score[0]<.4f||guide->source_pose.score[1]<.4f||guide->source_pose.score[3]<.3f||guide->head_pose.score[0]<.4f||guide->head_pose.score[1]<.4f)goto fallback;
    float sp[17][3],hp[17][3],sc[3],hc[3],sb[3][3],hb[3][3],se,he;
    if(!lift(base,photo,&guide->source_pose,sp)||!lift(&guide->head,&guide->head_photo,&guide->head_pose,hp)||!frame(sp,sc,sb,&se)||!frame(hp,hc,hb,&he))goto fallback;
    float lo[3],hi[3],span[3],unit=0;mesh_bounds(base,lo,hi);for(int k=0;k<3;k++){span[k]=hi[k]-lo[k];unit=fmaxf(unit,span[k]);}
    float scale=se/he;if(scale<.15f||scale>3||se<unit*.035f||se>unit*.3f)goto fallback;
    if(!mesh_copy(&guide->head,&head))goto memory;
    for(uint32_t i=0;i<head.vertex_count;i++){float v[3],local[3];for(int k=0;k<3;k++)v[k]=head.vertices[i].p[k]-hc[k];for(int k=0;k<3;k++)local[k]=dot(v,hb[k]);for(int k=0;k<3;k++)head.vertices[i].p[k]=sc[k]+scale*(local[0]*sb[0][k]+local[1]*sb[1][k]+local[2]*sb[2][k]);}
    float neck=sp[3][1]+.02f*unit,ground=0,feet[4];const int paw_ids[4]={7,10,13,16};int legs=guide->standing&&guide->leg_photo.rgba;
    for(int k=0;k<4;k++){feet[k]=sp[paw_ids[k]][1];if(guide->source_pose.score[paw_ids[k]]<.35f)legs=0;}
    for(int a=0;a<4;a++)for(int b=a+1;b<4;b++)if(feet[a]>feet[b]){float t=feet[a];feet[a]=feet[b];feet[b]=t;}ground=(feet[1]+feet[2])*.5f-.025f*unit;
    if(ground<lo[1]-.12f*unit||ground>lo[1]+.12f*unit||feet[3]-feet[0]>.12f*unit)legs=0;
    float headlo[3],headhi[3];mesh_bounds(&head,headlo,headhi);Grid g={0};g.step=unit/(j->resolution==320?224.f:192.f);
    int sizes[3];for(int k=0;k<3;k++){float lower=fminf(lo[k],headlo[k]),upper=fmaxf(hi[k],headhi[k]);if(k==1&&legs)lower=fminf(lower,ground-.04f*unit);if(upper-lower>unit*2.5f)goto fallback;g.origin[k]=floorf((lower-g.step*5)/g.step)*g.step;sizes[k]=(int)ceilf((upper+g.step*5-g.origin[k])/g.step)+1;}
    g.nx=sizes[0];g.ny=sizes[1];g.nz=sizes[2];g.count=(size_t)g.nx*g.ny*g.nz;if(g.count>24000000)goto fallback;
    body_field=malloc(g.count*4);head_field=malloc(g.count*4);if(!body_field||!head_field)goto memory;
    fs_status(j,L"C에서 참고 머리와 다리를 실제 입체 표면으로 연결하는 중");
    if(!mesh_field(j,&g,base,body_field)||!mesh_field(j,&g,&head,head_field)){if(fs_cancelled(j))goto done;goto fallback;}
    if(!blur(j,&g,body_field,3.2f)||!blur(j,&g,head_field,.65f))goto memory;
    float radius=paw_radius(&guide->leg_photo,&guide->leg_pose,span[1],unit),joints[4][3][3];
    if(radius<=0)legs=0;
    const int bones[4][3]={{5,6,7},{8,9,10},{11,12,13},{14,15,16}};
    for(int leg=0;leg<4;leg++){
        for(int k=0;k<3;k++)memcpy(joints[leg][k],sp[bones[leg][k]],12);float z[2];ray(base,joints[leg][0][0],joints[leg][0][1],z);float thickness=fmaxf(z[1]-z[0],.16f*unit);
        float depth=(z[0]+z[1])*.5f+(leg%2?1:-1)*thickness*.33f;for(int k=0;k<3;k++)joints[leg][k][2]=depth;joints[leg][2][1]=ground+radius*.85f;
    }
    float belly=(sp[6][1]+sp[9][1]+sp[12][1]+sp[15][1])*.25f+.035f*unit;
    for(int z=0;z<g.nz;z++){if(fs_cancelled(j))goto done;for(int y=0;y<g.ny;y++)for(int x=0;x<g.nx;x++){
        size_t i=cell(&g,x,y,z);float p[3]={g.origin[0]+x*g.step,g.origin[1]+y*g.step,g.origin[2]+z*g.step};
        float body=body_field[i],top=fmaxf(head_field[i],neck-.09f*unit-p[1]);
        /* The reference may include its torso. Never transplant that torso as
         * an extra appendage after aligning the head to a different direction. */
        top=fmaxf(top,fabsf(p[0]-sc[0])-se*2.2f);top=fmaxf(top,fabsf(p[2]-sc[2])-se*3.f);
        if(legs)body=fmaxf(body,belly-p[1]);
        float value=smin(body,top,.045f*unit),blend=ease((p[1]-(neck-.035f*unit))/(.16f*unit))*ease((se*2.3f-fabsf(p[0]-sc[0]))/(.10f*unit));
        value=value*(1-blend)+top*blend;
        if(legs){for(int leg=0;leg<4;leg++){
            value=smin(value,capsule(p,joints[leg][0],joints[leg][1],radius*1.5f,radius*.85f),.02f*unit);
            value=smin(value,capsule(p,joints[leg][1],joints[leg][2],radius*.85f,radius),.02f*unit);
            float delta[3]={p[0]-joints[leg][2][0]-.007f*unit,p[1]-joints[leg][2][1],p[2]-joints[leg][2][2]+.006f*unit};float rr[3]={radius*1.25f,radius*.8f,radius*1.1f};for(int k=0;k<3;k++)delta[k]/=rr[k];value=smin(value,(length(delta)-1)*rr[1],.012f*unit);
        }value=fmaxf(value,ground-p[1]);}body_field[i]=value;
    }}
    if(!blur(j,&g,body_field,.65f)||!extract(j,&g,body_field,out))goto memory;
    if(!mesh_source_colours(base,out,j->cancel,j->error)||!photo_colours(j,photo,&g,body_field,lo,hi,neck,out))goto memory;
    j->volume_head=1;j->volume_legs=legs;j->volume_head_index=guide->head_index;j->volume_leg_index=legs?guide->leg_index:-1;
    j->volume_head_scale=scale;j->volume_paw_radius=legs?radius:0;
    float nose_local[3],nose_world[3];for(int k=0;k<3;k++)nose_local[k]=hp[2][k]-hc[k];for(int k=0;k<3;k++)nose_world[k]=sc[k]+scale*(dot(nose_local,hb[0])*sb[0][k]+dot(nose_local,hb[1])*sb[1][k]+dot(nose_local,hb[2])*sb[2][k]);j->volume_muzzle_depth=sc[2]-nose_world[2];
    ok=1;goto done;
fallback:mesh_free(out);ok=mesh_copy(base,out);goto done;
memory:if(!fs_cancelled(j)&&!j->error[0])fs_error(j->error,L"참고 입체 표면 처리에 필요한 메모리를 확보하지 못했습니다.");
done:mesh_free(&head);free(body_field);free(head_field);if(!ok)mesh_free(out);return ok;
}
