/* Observed-view surface reconstruction, C17.
 * A closed mesh uses the input silhouette/colour, bounded relative-depth cues
 * on the front and the TripoSR volume on the rear. It is not measured anatomy.
 * No detached eyes, pasted mesh patches, or open seams are introduced.
 */
#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

typedef struct {float near_z,far_z,colour[3];int valid;} Pixel;
static float clamp(float v,float a,float b){return fmaxf(a,fminf(b,v));}
static int stop(Job *j){return fs_cancelled(j);}
static void sample_photo(const Photo *p,float x,float y,float c[4]){
    x=clamp(x,0,(float)p->width-1);y=clamp(y,0,(float)p->height-1);
    int a=(int)x,b=(int)y,aa=a+1<(int)p->width?a+1:a,bb=b+1<(int)p->height?b+1:b;
    float u=x-a,v=y-b;for(int k=0;k<4;k++)c[k]=(1-v)*((1-u)*p->rgba[((size_t)b*p->width+a)*4+k]+u*p->rgba[((size_t)b*p->width+aa)*4+k])+v*((1-u)*p->rgba[((size_t)bb*p->width+a)*4+k]+u*p->rgba[((size_t)bb*p->width+aa)*4+k]);
}
static float sample_depth(const float *p,int w,int h,float x,float y){
    x=clamp(x,0,(float)w-1);y=clamp(y,0,(float)h-1);int a=(int)x,b=(int)y,aa=a+1<w?a+1:a,bb=b+1<h?b+1:b;float u=x-a,v=y-b;
    return (1-v)*((1-u)*p[b*w+a]+u*p[b*w+aa])+v*((1-u)*p[bb*w+a]+u*p[bb*w+aa]);
}
static int project(Job *j,const Mesh *m,Pixel *pixels,int w,int h,const float lo[3],const float hi[3]){
    for(int i=0;i<w*h;i++){pixels[i].near_z=FLT_MAX;pixels[i].far_z=-FLT_MAX;pixels[i].valid=0;}
    for(uint32_t i=0;i<m->triangle_count;i++){
        if(!(i%4096)&&stop(j))return 0;const Triangle *t=&m->triangles[i];float x[3],y[3],z[3];
        for(int k=0;k<3;k++){const float *p=m->vertices[t->v[k]].p;x[k]=(hi[0]-p[0])/(hi[0]-lo[0])*(w-1);y[k]=(hi[1]-p[1])/(hi[1]-lo[1])*(h-1);z[k]=p[2];}
        float det=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);if(fabsf(det)<1e-9f)continue;
        int x0=(int)clamp(floorf(fminf(x[0],fminf(x[1],x[2]))),0,(float)w-1),x1=(int)clamp(ceilf(fmaxf(x[0],fmaxf(x[1],x[2]))),0,(float)w-1);
        int y0=(int)clamp(floorf(fminf(y[0],fminf(y[1],y[2]))),0,(float)h-1),y1=(int)clamp(ceilf(fmaxf(y[0],fmaxf(y[1],y[2]))),0,(float)h-1);
        for(int yy=y0;yy<=y1;yy++)for(int xx=x0;xx<=x1;xx++){
            float a=((y[1]-y[2])*(xx-x[2])+(x[2]-x[1])*(yy-y[2]))/det,b=((y[2]-y[0])*(xx-x[2])+(x[0]-x[2])*(yy-y[2]))/det,c=1-a-b;
            if(a<0||b<0||c<0)continue;float d=a*z[0]+b*z[1]+c*z[2];Pixel *p=&pixels[yy*w+xx];p->valid=1;p->near_z=fminf(p->near_z,d);
            if(d>p->far_z){p->far_z=d;for(int k=0;k<3;k++)p->colour[k]=a*m->vertices[t->v[0]].color[k]+b*m->vertices[t->v[1]].color[k]+c*m->vertices[t->v[2]].color[k];}
        }
    }return 1;
}
static int fill(Pixel *p,int w,int h){
    int *queue=malloc((size_t)w*h*sizeof(*queue));unsigned char *seen=calloc((size_t)w*h,1);if(!queue||!seen){free(queue);free(seen);return 0;}
    int first=0,last=0;for(int i=0;i<w*h;i++)if(p[i].valid){queue[last++]=i;seen[i]=1;}
    while(first<last){int a=queue[first++],x=a%w,y=a/w;int neighbors[4]={x?a-1:-1,x+1<w?a+1:-1,y?a-w:-1,y+1<h?a+w:-1};
        for(int k=0;k<4;k++){int b=neighbors[k];if(b<0||seen[b])continue;int was=p[b].valid;p[b]=p[a];p[b].valid=was;seen[b]=1;queue[last++]=b;}}
    free(queue);free(seen);return last>0;
}
static int blur(float *values,int w,int h,float sigma){
    float *temp=malloc((size_t)w*h*sizeof(float)),kernel[33];int r=(int)ceilf(3*sigma);if(r>16)r=16;if(!temp)return 0;
    float sum=0;for(int k=-r;k<=r;k++){kernel[k+r]=expf(-.5f*k*k/(sigma*sigma));sum+=kernel[k+r];}for(int k=0;k<2*r+1;k++)kernel[k]/=sum;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){float v=0;for(int k=-r;k<=r;k++){int xx=x+k;if(xx<0)xx=0;if(xx>=w)xx=w-1;v+=kernel[k+r]*values[y*w+xx];}temp[y*w+x]=v;}
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){float v=0;for(int k=-r;k<=r;k++){int yy=y+k;if(yy<0)yy=0;if(yy>=h)yy=h-1;v+=kernel[k+r]*temp[yy*w+x];}values[y*w+x]=v;}
    free(temp);return 1;
}
static void distances(const unsigned char *mask,float *d,int w,int h){
    for(int y=0;y<h;y++)for(int x=0;x<w;x++)d[y*w+x]=mask[y*w+x]?(x&&y&&x<w-1&&y<h-1?10000.f:1.f):0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){int i=y*w+x;if(x)d[i]=fminf(d[i],d[i-1]+1);if(y)d[i]=fminf(d[i],d[i-w]+1);if(x&&y)d[i]=fminf(d[i],d[i-w-1]+1.41421356f);if(x+1<w&&y)d[i]=fminf(d[i],d[i-w+1]+1.41421356f);}
    for(int y=h-1;y>=0;y--)for(int x=w-1;x>=0;x--){int i=y*w+x;if(x+1<w)d[i]=fminf(d[i],d[i+1]+1);if(y+1<h)d[i]=fminf(d[i],d[i+w]+1);if(x+1<w&&y+1<h)d[i]=fminf(d[i],d[i+w+1]+1.41421356f);if(x&&y+1<h)d[i]=fminf(d[i],d[i+w-1]+1.41421356f);}
}
static void wall(Mesh *m,uint32_t *t,uint32_t a,uint32_t b,uint32_t n){m->triangles[(*t)++]=(Triangle){{b,a,a+n}};m->triangles[(*t)++]=(Triangle){{b,a+n,b+n}};}
static int rounded(Job *j,Mesh *m){
    float (*sum)[3]=calloc(m->vertex_count,sizeof(*sum));uint32_t *degree=calloc(m->vertex_count,sizeof(*degree));if(!sum||!degree){free(sum);free(degree);return 0;}
    for(uint32_t t=0;t<m->triangle_count;t++)for(int k=0;k<3;k++){degree[m->triangles[t].v[k]]++;degree[m->triangles[t].v[(k+1)%3]]++;}
    for(int pass=0;pass<8;pass++){
        if(stop(j)){free(sum);free(degree);return 0;}memset(sum,0,(size_t)m->vertex_count*sizeof(*sum));
        for(uint32_t t=0;t<m->triangle_count;t++)for(int k=0;k<3;k++){uint32_t a=m->triangles[t].v[k],b=m->triangles[t].v[(k+1)%3];for(int c=0;c<3;c++){sum[a][c]+=m->vertices[b].p[c];sum[b][c]+=m->vertices[a].p[c];}}
        float factor=pass%2?-.52f:.5f;for(uint32_t i=0;i<m->vertex_count;i++)if(degree[i])for(int k=0;k<3;k++)m->vertices[i].p[k]+=factor*(sum[i][k]/degree[i]-m->vertices[i].p[k]);
    }free(sum);free(degree);return 1;
}
int mesh_observed_colour(Job *j,const Photo *photo,Mesh *m){
    for(uint32_t i=0;i<m->vertex_count;i++){
        if(!(i%4096)&&stop(j))return 0;Vertex *v=&m->vertices[i];if(v->face<=.001f)continue;float c[4];sample_photo(photo,v->source_uv[0]*(photo->width-1),v->source_uv[1]*(photo->height-1),c);
        for(int k=0;k<3;k++)v->color[k]=(unsigned char)clamp(c[k]*v->face+v->color[k]*(1-v->face)+.5f,0,255);
    }return 1;
}
int mesh_observed(Job *j,const Photo *photo,const wchar_t *depth_path,const Mesh *base,Mesh *out){
    Pixel *pixels=NULL;float *depth=NULL,*front=NULL,*back=NULL,*observed=NULL,*distance=NULL;unsigned char *mask=NULL,*cells=NULL;uint32_t *map=NULL;FILE *f=NULL;int ok=0;
    memset(out,0,sizeof(*out));if(stop(j)||!mesh_validate(base,j->error)||!photo->rgba||photo->width<4||photo->height<4)return 0;
    uint32_t header[3];f=fs_wfopen(depth_path,L"rb");if(!f||fread(header,4,3,f)!=3||header[0]!=0x31504446||header[1]!=photo->width||header[2]!=photo->height)goto invalid;
    size_t dn=(size_t)header[1]*header[2];depth=malloc(dn*sizeof(float));if(!depth)goto memory;
    if(fread(depth,4,dn,f)!=dn||fgetc(f)!=EOF)goto invalid;fclose(f);f=NULL;for(size_t i=0;i<dn;i++)if(!isfinite(depth[i]))goto invalid;
    int x0=photo->width,y0=photo->height,x1=-1,y1=-1;
    for(uint32_t y=0;y<photo->height;y++)for(uint32_t x=0;x<photo->width;x++)if(photo->rgba[((size_t)y*photo->width+x)*4+3]>32){if((int)x<x0)x0=x;if((int)y<y0)y0=y;if((int)x>x1)x1=x;if((int)y>y1)y1=y;}
    if(x1-x0<8||y1-y0<8)goto invalid;
    int longest=j->resolution==320?320:256,input_longest=x1-x0>y1-y0?x1-x0+1:y1-y0+1;if(input_longest<longest)longest=input_longest;
    float ratio=(float)(x1-x0+1)/(y1-y0+1);
    int w=ratio>=1?longest:(int)roundf(longest*ratio),h=ratio>=1?(int)roundf(longest/ratio):longest;if(w<32)w=32;if(h<32)h=32;int count=w*h;
    pixels=calloc(count,sizeof(*pixels));front=malloc((size_t)count*4);back=malloc((size_t)count*4);observed=malloc((size_t)count*4);distance=malloc((size_t)count*4);mask=calloc(count,1);cells=calloc(count,1);map=malloc((size_t)count*4);
    if(!pixels||!front||!back||!observed||!distance||!mask||!cells||!map)goto memory;
    float lo[3],hi[3];mesh_bounds(base,lo,hi);float span[3];for(int k=0;k<3;k++){span[k]=hi[k]-lo[k];if(span[k]<1e-6f)goto invalid;}
    if(!project(j,base,pixels,w,h,lo,hi))goto done;
    int intersection=0,united=0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){int i=y*w+x;float px=x0+(float)x/(w-1)*(x1-x0),py=y0+(float)y/(h-1)*(y1-y0),c[4];sample_photo(photo,px,py,c);mask[i]=c[3]>128;observed[i]=sample_depth(depth,photo->width,photo->height,px,py);intersection+=mask[i]&&pixels[i].valid;united+=mask[i]||pixels[i].valid;map[i]=UINT32_MAX;}
    j->observed_iou_before=united?(double)intersection/united:0;
    if(j->observed_iou_before<.60){ok=mesh_copy(base,out);goto done;}
    if(!fill(pixels,w,h))goto memory;distances(mask,distance,w,h);
    for(int i=0;i<count;i++){front[i]=pixels[i].near_z;back[i]=pixels[i].far_z;}
    if(!blur(front,w,h,2)||!blur(back,w,h,4)||!blur(observed,w,h,.7f))goto memory;
    double sx=0,sy=0,sxx=0,sxy=0,n=0;float dmin=FLT_MAX,dmax=-FLT_MAX;
    for(int i=0;i<count;i++)if(mask[i]){dmin=fminf(dmin,observed[i]);dmax=fmaxf(dmax,observed[i]);if(pixels[i].valid&&distance[i]>8){double a=observed[i],b=front[i];sx+=a;sy+=b;sxx+=a*a;sxy+=a*b;n++;}}
    if(n<100||dmax-dmin<1e-6f)goto invalid;
    double variance=sxx-sx*sx/n;float scale=variance>1e-10?(float)((sxy-sx*sy/n)/variance):0;
    scale=clamp(scale,-span[2]/(dmax-dmin),-.15f*span[2]/(dmax-dmin));float bias=(float)((sy-scale*sx)/n);j->observed_depth_scale=scale;
    for(int i=0;i<count;i++){
        float z=.3f*front[i]+.7f*(scale*observed[i]+bias),thickness=fmaxf(back[i]-z,span[2]*.10f),center=z+thickness*.5f;
        float radius=fmaxf(1,fminf(thickness*.5f/span[0]*w,40)),t=clamp(distance[i]/radius,0,1),weight=sqrtf(2*t-t*t);
        front[i]=center-thickness*.5f*weight;back[i]=center+thickness*.5f*weight;
    }
    uint32_t quads=0,edges=0,vertices=0;
    for(int y=0;y<h-1;y++)for(int x=0;x<w-1;x++){int a=y*w+x,b=a+1,d=a+w,c=d+1;if(mask[a]&&mask[b]&&mask[c]&&mask[d]){cells[a]=1;map[a]=map[b]=map[c]=map[d]=0;quads++;}}
    if(quads<100)goto invalid;
    for(int y=0;y<h-1;y++)for(int x=0;x<w-1;x++){int i=y*w+x;if(cells[i]){edges+=!x||!cells[i-1];edges+=x==w-2||!cells[i+1];edges+=!y||!cells[i-w];edges+=y==h-2||!cells[i+w];}}
    for(int i=0;i<count;i++)if(map[i]!=UINT32_MAX)map[i]=vertices++;
    out->vertex_count=2*vertices;out->triangle_count=4*quads+2*edges;out->vertices=calloc(out->vertex_count,sizeof(Vertex));out->triangles=malloc((size_t)out->triangle_count*sizeof(Triangle));if(!out->vertices||!out->triangles)goto memory;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){int i=y*w+x;if(map[i]==UINT32_MAX)continue;float u=(float)x/(w-1),v=(float)y/(h-1),px=x0+u*(x1-x0),py=y0+v*(y1-y0),c[4];sample_photo(photo,px,py,c);
        Vertex *a=&out->vertices[map[i]],*b=&out->vertices[map[i]+vertices];a->p[0]=hi[0]-u*span[0];a->p[1]=hi[1]-v*span[1];a->p[2]=front[i];a->face=1;a->source_uv[0]=px/(photo->width-1);a->source_uv[1]=py/(photo->height-1);a->smooth=1;
        *b=*a;a->observed=1;b->p[2]=back[i];b->face=0;for(int k=0;k<3;k++){a->color[k]=(unsigned char)clamp(c[k]+.5f,0,255);b->color[k]=(unsigned char)clamp(pixels[i].colour[k]+.5f,0,255);}a->color[3]=b->color[3]=255;
    }
    uint32_t triangle=0;
    for(int y=0;y<h-1;y++)for(int x=0;x<w-1;x++){int i=y*w+x;if(!cells[i])continue;uint32_t a=map[i],b=map[i+1],d=map[i+w],c=map[i+w+1];
        out->triangles[triangle++]=(Triangle){{a,d,b}};out->triangles[triangle++]=(Triangle){{b,d,c}};
        out->triangles[triangle++]=(Triangle){{a+vertices,b+vertices,d+vertices}};out->triangles[triangle++]=(Triangle){{b+vertices,c+vertices,d+vertices}};
        if(!x||!cells[i-1])wall(out,&triangle,a,d,vertices);if(x==w-2||!cells[i+1])wall(out,&triangle,c,b,vertices);if(!y||!cells[i-w])wall(out,&triangle,b,a,vertices);if(y==h-2||!cells[i+w])wall(out,&triangle,d,c,vertices);
    }
    if(triangle!=out->triangle_count)goto invalid;
    if(!rounded(j,out))goto memory;mesh_normals(out);
    if(!project(j,out,pixels,w,h,lo,hi))goto done;intersection=united=0;for(int i=0;i<count;i++){intersection+=mask[i]&&pixels[i].valid;united+=mask[i]||pixels[i].valid;}
    j->observed_iou_after=united?(double)intersection/united:0;
    if(j->observed_iou_after+.01<j->observed_iou_before){mesh_free(out);ok=mesh_copy(base,out);goto done;}
    j->observed_surface=1;ok=mesh_validate(out,j->error);goto done;
memory:if(!stop(j))fs_error(j->error,L"원본 사진 표면 보완 메모리가 부족합니다.");goto done;
invalid:fs_error(j->error,L"원본 사진의 깊이·윤곽 정보를 읽을 수 없습니다.");
done:if(f)fclose(f);free(pixels);free(depth);free(front);free(back);free(observed);free(distance);free(mask);free(cells);free(map);if(!ok)mesh_free(out);return ok;
}
