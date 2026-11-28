/* Quadric error edge collapse, implemented in C17.
 * Closed manifold edges only; preserve boundaries, topology and face orientation.
 * Disjoint one-rings per pass make adjacency stable during each pass.
 */
#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

typedef struct { double q[10]; } Quadric;
typedef struct { uint32_t a,b; float p[3], t; double cost; } Edge;
static void plane(const Mesh *m,const Triangle *t,double n[4]){
    const float *a=m->vertices[t->v[0]].p,*b=m->vertices[t->v[1]].p,*c=m->vertices[t->v[2]].p;
    double u[3],v[3];for(int k=0;k<3;k++){u[k]=b[k]-a[k];v[k]=c[k]-a[k];}
    n[0]=u[1]*v[2]-u[2]*v[1];n[1]=u[2]*v[0]-u[0]*v[2];n[2]=u[0]*v[1]-u[1]*v[0];
    double len=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    if(len>1e-20)for(int k=0;k<3;k++)n[k]/=len;
    n[3]=-(n[0]*a[0]+n[1]*a[1]+n[2]*a[2]);
}
static double evaluate(const double *q,const float *p){
    double x=p[0],y=p[1],z=p[2];
    return q[0]*x*x+2*q[1]*x*y+2*q[2]*x*z+2*q[3]*x+q[4]*y*y+2*q[5]*y*z+2*q[6]*y+q[7]*z*z+2*q[8]*z+q[9];
}
static int by_vertex(const void *x,const void *y){
    const Edge *a=x,*b=y;if(a->a!=b->a)return a->a<b->a?-1:1;return a->b==b->b?0:(a->b<b->b?-1:1);
}
static int by_cost(const void *x,const void *y){
    const Edge *a=x,*b=y;if(a->cost!=b->cost)return a->cost<b->cost?-1:1;return by_vertex(x,y);
}
static void candidate(Edge *e,const Mesh *m,const Quadric *quadrics,double extent){
    double q[10];const float *a=m->vertices[e->a].p,*b=m->vertices[e->b].p;
    for(int k=0;k<10;k++)q[k]=quadrics[e->a].q[k]+quadrics[e->b].q[k];
    /* Minimize the quadric along the original edge. No off-surface spikes. */
    float mid[3];for(int k=0;k<3;k++)mid[k]=(a[k]+b[k])*.5f;
    double f0=evaluate(q,a),f1=evaluate(q,b),fm=evaluate(q,mid);
    double aa=2*(f0+f1-2*fm),bb=f1-f0-aa;
    e->t=(float)(aa>1e-20?fmin(1,fmax(0,-bb/(2*aa))):.5);
    for(int k=0;k<3;k++)e->p[k]=a[k]+e->t*(b[k]-a[k]);
    double length=0,color=0;
    for(int k=0;k<3;k++){
        double d=a[k]-b[k],dc=(m->vertices[e->a].color[k]-m->vertices[e->b].color[k])/255.;
        length+=d*d;color+=dc*dc;
    }
    double detail=fmax(m->vertices[e->a].detail,m->vertices[e->b].detail);
    double protection=(detail>.5?60*detail:6*detail)+30*fmax(m->vertices[e->a].limb,m->vertices[e->b].limb);
    e->cost=(fmax(0,evaluate(q,e->p))+length*(0.002+0.8*color))*(1+protection)+length*.02*detail+extent*1e-15;
}
int mesh_simplify(Mesh *m,uint32_t target,FsCancel * cancel,wchar_t *error){
    uint32_t nv=m->vertex_count,nt=m->triangle_count;int ok=0;
    if(!mesh_validate(m,error)||target<4)return 0;
    if(nt<=target)return 1;
    Quadric *q=calloc(nv,sizeof(*q));Edge *edges=malloc((size_t)nt*3*sizeof(*edges));
    uint32_t *offset=calloc((size_t)nv+1,sizeof(*offset)),*cursor=calloc(nv,sizeof(*cursor));
    uint32_t *refs=malloc((size_t)nt*3*sizeof(*refs)),*map=malloc((size_t)nv*sizeof(*map));
    unsigned char *locked=calloc(nv,1),*boundary=calloc(nv,1),*dead=calloc(nt,1),*neighbor=calloc(nv,1);
    if(!q||!edges||!offset||!cursor||!refs||!map||!locked||!boundary||!dead||!neighbor){fs_error(error,L"메시 단순화 메모리가 부족합니다.");goto done;}
    for(uint32_t i=0;i<nt;i++){
        double n[4];plane(m,&m->triangles[i],n);
        for(int v=0;v<3;v++){double *d=q[m->triangles[i].v[v]].q;int k=0;
            for(int a=0;a<4;a++)for(int b=a;b<4;b++)d[k++]+=n[a]*n[b];}
    }
    float lo[3],hi[3];mesh_bounds(m,lo,hi);double extent=0;for(int k=0;k<3;k++)extent+=pow(hi[k]-lo[k],2);
    int has_photo=0;for(uint32_t i=0;i<nv;i++)if(m->vertices[i].observed){has_photo=1;break;}
    for(int pass=0;pass<(has_photo?32:160)&&m->triangle_count>target;pass++){
        if(cancel&&fs_cancel_wait(cancel,0)==WAIT_OBJECT_0){fs_error(error,L"생성을 취소했습니다.");goto done;}
        memset(offset,0,((size_t)nv+1)*sizeof(*offset));memset(locked,0,nv);memset(boundary,0,nv);memset(dead,0,nt);
        uint32_t ne=0;
        for(uint32_t t=0;t<m->triangle_count;t++)for(int k=0;k<3;k++){
            uint32_t a=m->triangles[t].v[k],b=m->triangles[t].v[(k+1)%3];offset[a+1]++;
            edges[ne].a=a<b?a:b;edges[ne++].b=a<b?b:a;
        }
        for(uint32_t i=1;i<=nv;i++)offset[i]+=offset[i-1];memcpy(cursor,offset,nv*sizeof(*cursor));
        for(uint32_t t=0;t<m->triangle_count;t++)for(int k=0;k<3;k++)refs[cursor[m->triangles[t].v[k]]++]=t;
        qsort(edges,ne,sizeof(*edges),by_vertex);
        uint32_t count=0;
        for(uint32_t i=0;i<ne;){uint32_t end=i+1;while(end<ne&&edges[end].a==edges[i].a&&edges[end].b==edges[i].b)end++;
            if(end-i!=2){boundary[edges[i].a]=boundary[edges[i].b]=1;}else edges[count++]=edges[i];i=end;}
        for(uint32_t i=0;i<count;i++)candidate(&edges[i],m,q,extent);
        qsort(edges,count,sizeof(*edges),by_cost);
        uint32_t removed=0;
        for(uint32_t i=0;i<count&&m->triangle_count-removed>target;i++){
            Edge *e=&edges[i];uint32_t a=e->a,b=e->b;
            if(locked[a]||locked[b]||boundary[a]||boundary[b])continue;
            if(m->vertices[a].limb_id&&m->vertices[b].limb_id&&m->vertices[a].limb_id!=m->vertices[b].limb_id)continue;
            /* Link condition: exactly the two edge-opposite vertices are shared. */
            for(uint32_t r=offset[a];r<offset[a+1];r++)for(int k=0;k<3;k++)neighbor[m->triangles[refs[r]].v[k]]=1;
            neighbor[a]=neighbor[b]=0;
            unsigned common=0;
            for(uint32_t r=offset[b];r<offset[b+1];r++)for(int k=0;k<3;k++){
                uint32_t v=m->triangles[refs[r]].v[k];if(neighbor[v]==1){common++;neighbor[v]=2;}}
            for(uint32_t r=offset[a];r<offset[a+1];r++)for(int k=0;k<3;k++)neighbor[m->triangles[refs[r]].v[k]]=0;
            if(common!=2)continue;
            /* A tetrahedron may satisfy the vertex-neighbour test but would
             * collapse into two coincident opposite faces with zero volume. */
            if(offset[a+1]-offset[a]==3&&offset[b+1]-offset[b]==3)continue;
            int valid=1;float photo_uv[2]={0};
            int protect_photo=m->vertices[a].observed&&m->vertices[b].observed&&m->vertices[a].face>.5f&&m->vertices[b].face>.5f;
            if(protect_photo)for(int k=0;k<2;k++)photo_uv[k]=(1-e->t)*m->vertices[a].source_uv[k]+e->t*m->vertices[b].source_uv[k];
            for(int side=0;side<2&&valid;side++){
                uint32_t v=side?b:a;
                for(uint32_t r=offset[v];r<offset[v+1];r++){
                    Triangle *t=&m->triangles[refs[r]];int both=0;
                    for(int k=0;k<3;k++)if(t->v[k]==a||t->v[k]==b)both++;
                    if(both==2)continue;
                    /* Vertex colours need enough samples in photo space. A
                     * shape-only error metric misses a tiny eye disappearing. */
                    if(protect_photo)for(int k=0;k<3;k++){
                        Vertex *neighbor_vertex=&m->vertices[t->v[k]];if(t->v[k]==a||t->v[k]==b||!neighbor_vertex->observed)continue;
                        float du=photo_uv[0]-neighbor_vertex->source_uv[0],dv=photo_uv[1]-neighbor_vertex->source_uv[1];
                        float limit=fmaxf(m->vertices[a].detail,m->vertices[b].detail)>.5f?.0065f:.010f;
                        if(du*du+dv*dv>limit*limit){valid=0;break;}
                    }
                    if(!valid)break;
                    double n[4];plane(m,t,n);float p[3][3];
                    for(int k=0;k<3;k++)memcpy(p[k],(t->v[k]==a||t->v[k]==b)?e->p:m->vertices[t->v[k]].p,12);
                    double u[3],w[3],nn[3];for(int k=0;k<3;k++){u[k]=p[1][k]-p[0][k];w[k]=p[2][k]-p[0][k];}
                    nn[0]=u[1]*w[2]-u[2]*w[1];nn[1]=u[2]*w[0]-u[0]*w[2];nn[2]=u[0]*w[1]-u[1]*w[0];
                    double len=sqrt(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]);
                    if(len<extent*1e-14||nn[0]*n[0]+nn[1]*n[1]+nn[2]*n[2]<.25*len){valid=0;break;}
                }
            }
            if(!valid)continue;
            for(int side=0;side<2;side++){
                uint32_t v=side?b:a;
                for(uint32_t r=offset[v];r<offset[v+1];r++){
                    uint32_t ti=refs[r];Triangle *t=&m->triangles[ti];if(dead[ti])continue;
                    for(int k=0;k<3;k++){locked[t->v[k]]=1;if(t->v[k]==b)t->v[k]=a;}
                    if(t->v[0]==t->v[1]||t->v[1]==t->v[2]||t->v[2]==t->v[0]){dead[ti]=1;removed++;}
                }
            }
            memcpy(m->vertices[a].p,e->p,12);
            float wa=(1-e->t)*m->vertices[a].face,wb=e->t*m->vertices[b].face;
            if(wa+wb>1e-8f)for(int k=0;k<2;k++)m->vertices[a].source_uv[k]=(wa*m->vertices[a].source_uv[k]+wb*m->vertices[b].source_uv[k])/(wa+wb);
            m->vertices[a].face=wa+wb;
            m->vertices[a].observed|=m->vertices[b].observed;
            m->vertices[a].smooth=fmaxf(m->vertices[a].smooth,m->vertices[b].smooth);
            m->vertices[a].detail=fmaxf(m->vertices[a].detail,m->vertices[b].detail);
            m->vertices[a].limb=fmaxf(m->vertices[a].limb,m->vertices[b].limb);
            if(!m->vertices[a].limb_id)m->vertices[a].limb_id=m->vertices[b].limb_id;
            for(int k=0;k<4;k++)m->vertices[a].color[k]=(unsigned char)((1-e->t)*m->vertices[a].color[k]+e->t*m->vertices[b].color[k]+.5f);
            for(int k=0;k<10;k++)q[a].q[k]+=q[b].q[k];
        }
        if(!removed)break;
        uint32_t live=0;for(uint32_t i=0;i<m->triangle_count;i++)if(!dead[i])m->triangles[live++]=m->triangles[i];m->triangle_count=live;
    }
    for(uint32_t i=0;i<nv;i++)map[i]=UINT32_MAX;
    for(uint32_t i=0;i<m->triangle_count;i++)for(int k=0;k<3;k++)map[m->triangles[i].v[k]]=0;
    uint32_t live=0;for(uint32_t i=0;i<nv;i++)if(map[i]!=UINT32_MAX){map[i]=live;m->vertices[live++]=m->vertices[i];}
    m->vertex_count=live;
    for(uint32_t i=0;i<m->triangle_count;i++)for(int k=0;k<3;k++)m->triangles[i].v[k]=map[m->triangles[i].v[k]];
    mesh_normals(m);ok=mesh_validate(m,error);
done:
    free(q);free(edges);free(offset);free(cursor);free(refs);free(map);free(locked);free(boundary);free(dead);free(neighbor);return ok;
}

#define SIDE 128
static void raster(const Mesh *m,float *depth,int axis,int sign,const float lo[3],const float hi[3]){
    int xaxis=(axis+1)%3,yaxis=(axis+2)%3;
    for(int i=0;i<SIDE*SIDE;i++)depth[i]=-FLT_MAX;
    for(uint32_t t=0;t<m->triangle_count;t++){
        float x[3],y[3],z[3];
        for(int k=0;k<3;k++){
            const float *p=m->vertices[m->triangles[t].v[k]].p;
            x[k]=2+(SIDE-4)*(p[xaxis]-lo[xaxis])/fmaxf(1e-8f,hi[xaxis]-lo[xaxis]);
            y[k]=2+(SIDE-4)*(p[yaxis]-lo[yaxis])/fmaxf(1e-8f,hi[yaxis]-lo[yaxis]);
            z[k]=sign*(p[axis]-lo[axis])/fmaxf(1e-8f,hi[axis]-lo[axis]);
        }
        float det=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);if(fabsf(det)<1e-8f)continue;
        int xmin=(int)fmaxf(0,floorf(fminf(x[0],fminf(x[1],x[2])))),xmax=(int)fminf(SIDE-1,ceilf(fmaxf(x[0],fmaxf(x[1],x[2]))));
        int ymin=(int)fmaxf(0,floorf(fminf(y[0],fminf(y[1],y[2])))),ymax=(int)fminf(SIDE-1,ceilf(fmaxf(y[0],fmaxf(y[1],y[2]))));
        for(int yy=ymin;yy<=ymax;yy++)for(int xx=xmin;xx<=xmax;xx++){
            float a=((y[1]-y[2])*(xx+.5f-x[2])+(x[2]-x[1])*(yy+.5f-y[2]))/det;
            float b=((y[2]-y[0])*(xx+.5f-x[2])+(x[0]-x[2])*(yy+.5f-y[2]))/det,c=1-a-b;
            if(a>=0&&b>=0&&c>=0){float d=a*z[0]+b*z[1]+c*z[2];if(d>depth[yy*SIDE+xx])depth[yy*SIDE+xx]=d;}
        }
    }
}
Quality mesh_compare(const Mesh *original,const Mesh *candidate){
    Quality result={1,0};float lo[3],hi[3];mesh_bounds(original,lo,hi);
    float *a=malloc(SIDE*SIDE*sizeof(float)),*b=malloc(SIDE*SIDE*sizeof(float));
    if(!a||!b){free(a);free(b);result.silhouette_iou=0;result.depth_rmse=1;return result;}
    for(int axis=0;axis<3;axis++)for(int sign=-1;sign<=1;sign+=2){
        raster(original,a,axis,sign,lo,hi);raster(candidate,b,axis,sign,lo,hi);
        unsigned intersection=0,union_count=0;double sum=0;
        for(int i=0;i<SIDE*SIDE;i++){
            int aa=a[i]!=-FLT_MAX,bb=b[i]!=-FLT_MAX;union_count+=aa||bb;
            if(aa&&bb){intersection++;double d=a[i]-b[i];sum+=d*d;}
        }
        double iou=union_count?(double)intersection/union_count:0,rmse=intersection?sqrt(sum/intersection):1;
        if(iou<result.silhouette_iou)result.silhouette_iou=iou;if(rmse>result.depth_rmse)result.depth_rmse=rmse;
    }
    free(a);free(b);return result;
}
