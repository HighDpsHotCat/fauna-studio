/* Detailed source-preserving surfaces and the retained legacy low-poly path.
 * Smooth normals and continuous colours are stored in GLB, not viewer effects.
 */
#include "fauna.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#define PALETTE_SIZE 24
static int cancelled(FsCancel * cancel,wchar_t *error){
    if(cancel&&fs_cancel_wait(cancel,0)==WAIT_OBJECT_0){
        fs_error(error,L"생성을 취소했습니다.");return 1;
    }
    return 0;
}

static int condition_surface(Mesh *mesh,FsCancel * cancel,wchar_t *error){
    /* Mild non-shrinking Taubin smoothing removes tiny reconstructed fur bumps.
     * Extremities move less; every vertex stays within 0.6% of the model span.
     * A face-flip check rejects a pass instead of changing the mesh topology.
     */
    const uint32_t nv=mesh->vertex_count;
    float (*start)[3]=malloc((size_t)nv*sizeof(*start));
    float (*next)[3]=malloc((size_t)nv*sizeof(*next));
    double (*sum)[3]=calloc(nv,sizeof(*sum));
    uint32_t *degree=calloc(nv,sizeof(*degree));
    if(!start||!next||!sum||!degree){
        free(start);free(next);free(sum);free(degree);
        fs_error(error,L"표면 정리 메모리가 부족합니다.");return 0;
    }
    float lo[3],hi[3];mesh_bounds(mesh,lo,hi);float span=0;
    for(int k=0;k<3;k++)span=fmaxf(span,hi[k]-lo[k]);
    for(uint32_t i=0;i<nv;i++)memcpy(start[i],mesh->vertices[i].p,sizeof(start[i]));
    int ok=1;
    for(int pass=0;pass<12;pass++){
        if(cancelled(cancel,error)){ok=0;break;}
        memset(sum,0,(size_t)nv*sizeof(*sum));memset(degree,0,(size_t)nv*sizeof(*degree));
        for(uint32_t t=0;t<mesh->triangle_count;t++)for(int k=0;k<3;k++){
            uint32_t a=mesh->triangles[t].v[k],b=mesh->triangles[t].v[(k+1)%3];
            degree[a]++;degree[b]++;
            for(int c=0;c<3;c++){sum[a][c]+=mesh->vertices[b].p[c];sum[b][c]+=mesh->vertices[a].p[c];}
        }
        for(uint32_t i=0;i<nv;i++){
            const float *p=mesh->vertices[i].p;
            float y=(start[i][1]-lo[1])/fmaxf(hi[1]-lo[1],1e-8f);
            float x=(start[i][0]-lo[0])/fmaxf(hi[0]-lo[0],1e-8f);
            float protection=(y<.2f||y>.72f||x<.1f||x>.9f)?.3f:1.f;
            protection*=1-.95f*fminf(1,fmaxf(mesh->vertices[i].detail,mesh->vertices[i].limb));
            float factor=(pass%2?-.53f:.5f)*protection;double displacement=0;
            for(int c=0;c<3;c++){
                next[i][c]=p[c]+factor*(float)(sum[i][c]/(degree[i]?degree[i]:1)-p[c]);
                displacement+=(next[i][c]-start[i][c])*(next[i][c]-start[i][c]);
            }
            double limit=span*.006;
            if(displacement>limit*limit){float scale=(float)(limit/sqrt(displacement));
                for(int c=0;c<3;c++)next[i][c]=start[i][c]+scale*(next[i][c]-start[i][c]);}
        }
        int safe=1;
        for(uint32_t t=0;t<mesh->triangle_count&&safe;t++){
            const Triangle *tri=&mesh->triangles[t];double normals[2][3];
            for(int state=0;state<2;state++){
                const float *a=state?next[tri->v[0]]:mesh->vertices[tri->v[0]].p;
                const float *b=state?next[tri->v[1]]:mesh->vertices[tri->v[1]].p;
                const float *c=state?next[tri->v[2]]:mesh->vertices[tri->v[2]].p;
                double u[3],v[3];for(int k=0;k<3;k++){u[k]=b[k]-a[k];v[k]=c[k]-a[k];}
                normals[state][0]=u[1]*v[2]-u[2]*v[1];normals[state][1]=u[2]*v[0]-u[0]*v[2];normals[state][2]=u[0]*v[1]-u[1]*v[0];
            }
            double dot=0,length=0;for(int k=0;k<3;k++){dot+=normals[0][k]*normals[1][k];length+=normals[0][k]*normals[0][k];}
            if(length>1e-24&&dot<=length*.05)safe=0;
        }
        if(!safe)break;
        for(uint32_t i=0;i<nv;i++)memcpy(mesh->vertices[i].p,next[i],sizeof(next[i]));
    }
    free(start);free(next);free(sum);free(degree);return ok;
}

static double color_distance(const float a[3],const float b[3]){
    double d=0;for(int k=0;k<3;k++)d+=(a[k]-b[k])*(a[k]-b[k]);return d;
}

/* A k-d tree samples the dense mesh at each final face centre. Repeated edge
 * averaging otherwise erases small eyes/noses even when their shape survives. */
static void colour_tree(const Mesh *source,uint32_t *ids,int first,int end,int axis){
    if(end-first<2)return;int middle=(first+end)/2,left=first,right=end-1;
    while(left<right){
        float pivot=source->vertices[ids[(left+right)/2]].p[axis];int i=left,j=right;
        while(i<=j){while(source->vertices[ids[i]].p[axis]<pivot)i++;while(source->vertices[ids[j]].p[axis]>pivot)j--;
            if(i<=j){uint32_t temp=ids[i];ids[i++]=ids[j];ids[j--]=temp;}}
        if(middle<=j)right=j;else if(middle>=i)left=i;else break;
    }
    colour_tree(source,ids,first,middle,(axis+1)%3);colour_tree(source,ids,middle+1,end,(axis+1)%3);
}
static void nearest_colour(const Mesh *source,const uint32_t *ids,int first,int end,int axis,const float point[3],double *distance,uint32_t *index){
    if(first>=end)return;int mid=(first+end)/2;uint32_t id=ids[mid];const float *p=source->vertices[id].p;double d=0;
    for(int k=0;k<3;k++)d+=(point[k]-p[k])*(point[k]-p[k]);if(d<*distance){*distance=d;*index=id;}
    float delta=point[axis]-p[axis];int next=(axis+1)%3;
    if(delta<0){nearest_colour(source,ids,first,mid,next,point,distance,index);if(delta*delta<*distance)nearest_colour(source,ids,mid+1,end,next,point,distance,index);}
    else{nearest_colour(source,ids,mid+1,end,next,point,distance,index);if(delta*delta<*distance)nearest_colour(source,ids,first,mid,next,point,distance,index);}
}
typedef struct {uint32_t a,b,triangle,side;} DetailEdge;
static int detail_edge_order(const void *a,const void *b){const DetailEdge *x=a,*y=b;if(x->a!=y->a)return x->a<y->a?-1:1;return x->b==y->b?0:(x->b<y->b?-1:1);}
static int child_orientation(const Mesh *mesh,const Triangle *old,const Triangle *child){
    double normal[2][3],dot=0,aa=0,bb=0;const Triangle *t[2]={old,child};
    for(int i=0;i<2;i++){const float *a=mesh->vertices[t[i]->v[0]].p,*b=mesh->vertices[t[i]->v[1]].p,*c=mesh->vertices[t[i]->v[2]].p;double u[3],v[3];
        for(int k=0;k<3;k++){u[k]=b[k]-a[k];v[k]=c[k]-a[k];}
        normal[i][0]=u[1]*v[2]-u[2]*v[1];normal[i][1]=u[2]*v[0]-u[0]*v[2];normal[i][2]=u[0]*v[1]-u[1]*v[0];}
    for(int k=0;k<3;k++){dot+=normal[0][k]*normal[1][k];aa+=normal[0][k]*normal[0][k];bb+=normal[1][k]*normal[1][k];}
    return aa<1e-24||(bb>1e-24&&dot>.4*sqrt(aa*bb));
}
/* Shared-edge splits keep a single closed surface. New facial vertices sample
 * the dense TripoSR/refined surface, so irises, lips and the nose get real faces
 * without increasing the tessellation of every patch of coat. */
static int refine_detail_triangles(Mesh *mesh,const Mesh *source,FsCancel * cancel,wchar_t *error){
    int any=0;for(uint32_t i=0;i<source->vertex_count;i++)if(source->vertices[i].detail>.15f){any=1;break;}if(!any)return 1;
    uint32_t *tree=malloc((size_t)source->vertex_count*sizeof(*tree));if(!tree){fs_error(error,L"얼굴 면 처리 메모리가 부족합니다.");return 0;}
    for(uint32_t i=0;i<source->vertex_count;i++)tree[i]=i;colour_tree(source,tree,0,(int)source->vertex_count,0);
    float lo[3],hi[3];mesh_bounds(source,lo,hi);float span=fmaxf(hi[0]-lo[0],fmaxf(hi[1]-lo[1],hi[2]-lo[2]));int ok=0;
    for(int pass=0;pass<7;pass++){
        if(cancelled(cancel,error))goto done;
        uint32_t nv=mesh->vertex_count,nt=mesh->triangle_count,ne=3*nt,added=0;
        DetailEdge *edges=malloc((size_t)ne*sizeof(*edges));uint32_t *mid=malloc((size_t)ne*sizeof(*mid));
        Vertex *vertices=malloc((size_t)(nv+ne)*sizeof(*vertices));float (*linear)[3]=malloc((size_t)ne*sizeof(*linear));
        Triangle *triangles=malloc((size_t)nt*4*sizeof(*triangles));uint32_t *parent=malloc((size_t)nt*4*sizeof(*parent));
        if(!edges||!mid||!vertices||!linear||!triangles||!parent){free(edges);free(mid);free(vertices);free(linear);free(triangles);free(parent);fs_error(error,L"얼굴 면 처리 메모리가 부족합니다.");goto done;}
        memcpy(vertices,mesh->vertices,(size_t)nv*sizeof(*vertices));
        for(uint32_t i=0;i<nt;i++)for(int k=0;k<3;k++){
            uint32_t a=mesh->triangles[i].v[k],b=mesh->triangles[i].v[(k+1)%3];edges[3*i+k]=(DetailEdge){a<b?a:b,a<b?b:a,i,(uint32_t)k};mid[3*i+k]=UINT32_MAX;}
        qsort(edges,ne,sizeof(*edges),detail_edge_order);
        for(uint32_t i=0;i<ne;){
            uint32_t end=i+1;while(end<ne&&edges[i].a==edges[end].a&&edges[i].b==edges[end].b)end++;
            const Vertex *a=&mesh->vertices[edges[i].a],*b=&mesh->vertices[edges[i].b];float point[3];double length=0;
            for(int k=0;k<3;k++){point[k]=(a->p[k]+b->p[k])*.5f;length+=(a->p[k]-b->p[k])*(a->p[k]-b->p[k]);}
            double distance=DBL_MAX;uint32_t nearest=0;nearest_colour(source,tree,0,(int)source->vertex_count,0,point,&distance,&nearest);
            const Vertex *sample=&source->vertices[nearest];float limit=span*(sample->detail>3.f?.004f:.009f);
            int boundary=sample->detail>.15f&&point[1]<lo[1]+.58f*(hi[1]-lo[1]);
            if(boundary&&sample->detail<=.65f)limit=span*.022f;
            if((sample->detail>.65f||boundary)&&length>limit*limit&&distance<length*.25&&nv+added<1000000){
                uint32_t index=nv+added;vertices[index]=*sample;memcpy(linear[added],point,12);
                float offset=0;for(int k=0;k<3;k++)offset+=(sample->p[k]-point[k])*sample->n[k];
                offset=fmaxf(-span*.006f,fminf(span*.006f,offset));
                for(int k=0;k<3;k++)vertices[index].p[k]=point[k]+offset*sample->n[k];
                for(uint32_t j=i;j<end;j++)mid[edges[j].triangle*3+edges[j].side]=index;added++;
            }
            i=end;
        }
        uint32_t count=0;
        for(uint32_t i=0;i<nt;i++){
            const uint32_t *v=mesh->triangles[i].v,*m=&mid[i*3];int n=0;for(int k=0;k<3;k++)n+=m[k]!=UINT32_MAX;
            uint32_t before=count;
            if(!n)triangles[count++]=mesh->triangles[i];
            else if(n==3){triangles[count++]=(Triangle){{v[0],m[0],m[2]}};triangles[count++]=(Triangle){{m[0],v[1],m[1]}};triangles[count++]=(Triangle){{m[2],m[1],v[2]}};triangles[count++]=(Triangle){{m[0],m[1],m[2]}};}
            else{
                int start=0;if(n==1){while(m[start]==UINT32_MAX)start++;}else{int missing=0;while(m[missing]!=UINT32_MAX)missing++;start=(missing+1)%3;}
                uint32_t a=v[start],b=v[(start+1)%3],c=v[(start+2)%3],ab=m[start],bc=m[(start+1)%3];
                if(n==1){triangles[count++]=(Triangle){{a,ab,c}};triangles[count++]=(Triangle){{ab,b,c}};}
                else{triangles[count++]=(Triangle){{b,bc,ab}};triangles[count++]=(Triangle){{a,ab,c}};triangles[count++]=(Triangle){{ab,bc,c}};}
            }
            for(uint32_t t=before;t<count;t++)parent[t]=i;
        }
        Mesh next={vertices,triangles,nv+added,count};
        for(int attempt=0;attempt<3;attempt++){
            int valid=1;for(uint32_t i=0;i<count;i++)if(!child_orientation(&next,&mesh->triangles[parent[i]],&triangles[i])){
                valid=0;for(int k=0;k<3;k++){uint32_t id=triangles[i].v[k];if(id>=nv)memcpy(vertices[id].p,linear[id-nv],12);}}
            if(valid)break;
            if(attempt==2)for(uint32_t i=0;i<added;i++)memcpy(vertices[nv+i].p,linear[i],12);
        }
        free(edges);free(mid);free(linear);free(parent);free(mesh->vertices);free(mesh->triangles);*mesh=next;
        if(!added)break;
    }
    mesh_normals(mesh);ok=mesh_validate(mesh,error);
done:free(tree);return ok;
}
static int facets_from(Mesh *mesh,const Mesh *source,wchar_t *error){
    if(!mesh_validate(mesh,error))return 0;
    if(mesh->triangle_count>4000000/3){fs_error(error,L"로우폴리 면 수가 처리 한도를 초과했습니다.");return 0;}
    uint32_t count=mesh->triangle_count;
    float (*face_color)[3]=calloc(count,sizeof(*face_color));
    double *area=calloc(count,sizeof(*area));
    Vertex *vertices=calloc((size_t)count*3,sizeof(*vertices));
    Triangle *triangles=malloc((size_t)count*sizeof(*triangles));
    uint32_t *tree=source?malloc((size_t)source->vertex_count*sizeof(*tree)):NULL;
    if(!face_color||!area||!vertices||!triangles||(source&&!tree)){
        free(face_color);free(area);free(vertices);free(triangles);
        free(tree);
        fs_error(error,L"로우폴리 색상 처리 메모리가 부족합니다.");return 0;
    }
    if(source){for(uint32_t i=0;i<source->vertex_count;i++)tree[i]=i;colour_tree(source,tree,0,(int)source->vertex_count,0);}
    float palette[PALETTE_SIZE][3]={{0}};double total_area=0;
    for(uint32_t i=0;i<count;i++){
        const Triangle *t=&mesh->triangles[i];float u[3],v[3],normal[3];
        for(int k=0;k<3;k++){
            const Vertex *a=&mesh->vertices[t->v[0]],*b=&mesh->vertices[t->v[1]],*c=&mesh->vertices[t->v[2]];
            u[k]=b->p[k]-a->p[k];v[k]=c->p[k]-a->p[k];
            face_color[i][k]=(a->color[k]+b->color[k]+c->color[k])/(3.f*255);
        }
        if(source){float center[3];for(int k=0;k<3;k++)center[k]=(mesh->vertices[t->v[0]].p[k]+mesh->vertices[t->v[1]].p[k]+mesh->vertices[t->v[2]].p[k])/3;
            double distance=DBL_MAX;uint32_t nearest=0;nearest_colour(source,tree,0,(int)source->vertex_count,0,center,&distance,&nearest);
            for(int k=0;k<3;k++)face_color[i][k]=source->vertices[nearest].color[k]/255.f;
        }
        normal[0]=u[1]*v[2]-u[2]*v[1];normal[1]=u[2]*v[0]-u[0]*v[2];normal[2]=u[0]*v[1]-u[1]*v[0];
        area[i]=sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);total_area+=area[i];
        for(int k=0;k<3;k++){
            Vertex *d=&vertices[i*3+k];*d=mesh->vertices[t->v[k]];
            if(source&&d->face>.01f){double distance=DBL_MAX;uint32_t nearest=0;nearest_colour(source,tree,0,(int)source->vertex_count,0,d->p,&distance,&nearest);
                const Vertex *sample=&source->vertices[nearest];if(sample->face>.01f){memcpy(d->source_uv,sample->source_uv,sizeof(d->source_uv));d->face=sample->face;}}
            float w=source?fminf(1,fmaxf(d->smooth,d->face*2)):0,length=0;
            for(int c=0;c<3;c++){d->n[c]=w*d->n[c]+(1-w)*(area[i]>1e-20?(float)(normal[c]/area[i]):0);length+=d->n[c]*d->n[c];}
            if(length>1e-20f)for(int c=0;c<3;c++)d->n[c]/=sqrtf(length);
            triangles[i].v[k]=i*3+k;
        }
    }
    /* Area weighting prevents tiny fur triangles from dominating the palette. */
    double center[3]={0};
    for(uint32_t i=0;i<count;i++)for(int k=0;k<3;k++)center[k]+=area[i]*face_color[i][k];
    for(int k=0;k<3;k++)palette[0][k]=(float)(center[k]/fmax(total_area,1e-20));
    for(int p=1;p<PALETTE_SIZE;p++){
        uint32_t farthest=0;double maximum=-1;
        for(uint32_t i=0;i<count;i++){
            double distance=DBL_MAX;for(int j=0;j<p;j++)distance=fmin(distance,color_distance(face_color[i],palette[j]));
            if(distance>maximum){maximum=distance;farthest=i;}
        }
        memcpy(palette[p],face_color[farthest],sizeof(palette[p]));
    }
    for(int iteration=0;iteration<16;iteration++){
        double sums[PALETTE_SIZE][3]={{0}},weights[PALETTE_SIZE]={0};
        for(uint32_t i=0;i<count;i++){
            int nearest=0;double distance=DBL_MAX;
            for(int p=0;p<PALETTE_SIZE;p++){double d=color_distance(face_color[i],palette[p]);if(d<distance){distance=d;nearest=p;}}
            weights[nearest]+=area[i];for(int k=0;k<3;k++)sums[nearest][k]+=face_color[i][k]*area[i];
        }
        for(int p=0;p<PALETTE_SIZE;p++)if(weights[p]>1e-20)for(int k=0;k<3;k++)palette[p][k]=(float)(sums[p][k]/weights[p]);
    }
    for(uint32_t i=0;i<count;i++){
        int nearest=0;double distance=DBL_MAX;
        for(int p=0;p<PALETTE_SIZE;p++){double d=color_distance(face_color[i],palette[p]);if(d<distance){distance=d;nearest=p;}}
        /* Keep source-photo face colours instead of quantizing tiny iris/nose
         * regions into the much larger coat palette. Flat normals show planes. */
        float detail=0;for(int k=0;k<3;k++)detail=fmaxf(detail,vertices[i*3+k].detail);
        float photo_weight=detail>.5f?1.f:0.f;unsigned char color[4]={0,0,0,255};
        for(int k=0;k<3;k++){
            float c=palette[nearest][k]*(1-photo_weight)+face_color[i][k]*photo_weight;
            color[k]=(unsigned char)(fmaxf(0,fminf(1,c))*255+.5f);
        }
        for(int k=0;k<3;k++){Vertex *v=&vertices[i*3+k];float w=source?v->face:0;
            for(int c=0;c<3;c++)v->color[c]=(unsigned char)(w*v->color[c]+(1-w)*color[c]+.5f);v->color[3]=255;}
    }
    free(tree);free(face_color);free(area);free(mesh->vertices);free(mesh->triangles);
    mesh->vertices=vertices;mesh->triangles=triangles;mesh->vertex_count=count*3;return 1;
}
int mesh_facets(Mesh *mesh,wchar_t *error){return facets_from(mesh,NULL,error);}

typedef struct {float lo[3],hi[3];double area;} SurfaceIsland;
static uint32_t island_root(uint32_t *parent,uint32_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;}
/* Marching cubes can produce microscopic, inverted closed cavities. Remove
 * only disconnected specks with BOTH tiny area and tiny spatial extent. Never
 * discard an isolated paw or another substantial component as "not largest". */
static int remove_surface_specks(Mesh *m,FsCancel * cancel,wchar_t *error){
    uint32_t *parent=malloc((size_t)m->vertex_count*sizeof(*parent));
    SurfaceIsland *islands=calloc(m->vertex_count,sizeof(*islands));
    if(!parent||!islands){free(parent);free(islands);fs_error(error,L"표면 조각 검사 메모리가 부족합니다.");return 0;}
    for(uint32_t i=0;i<m->vertex_count;i++){parent[i]=i;for(int k=0;k<3;k++){islands[i].lo[k]=FLT_MAX;islands[i].hi[k]=-FLT_MAX;}}
    for(uint32_t t=0;t<m->triangle_count;t++){
        if(!(t%4096)&&cancelled(cancel,error)){free(parent);free(islands);return 0;}
        uint32_t a=island_root(parent,m->triangles[t].v[0]);
        for(int k=1;k<3;k++){uint32_t b=island_root(parent,m->triangles[t].v[k]);if(a!=b)parent[b]=a;}
    }
    float lo[3],hi[3],span=0;mesh_bounds(m,lo,hi);for(int k=0;k<3;k++)span=fmaxf(span,hi[k]-lo[k]);
    double total=0;
    for(uint32_t t=0;t<m->triangle_count;t++){
        const Triangle *triangle=&m->triangles[t];SurfaceIsland *island=&islands[island_root(parent,triangle->v[0])];
        double u[3],v[3],n[3];for(int k=0;k<3;k++){u[k]=(double)m->vertices[triangle->v[1]].p[k]-m->vertices[triangle->v[0]].p[k];v[k]=(double)m->vertices[triangle->v[2]].p[k]-m->vertices[triangle->v[0]].p[k];}
        n[0]=u[1]*v[2]-u[2]*v[1];n[1]=u[2]*v[0]-u[0]*v[2];n[2]=u[0]*v[1]-u[1]*v[0];double area=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);island->area+=area;total+=area;
        for(int j=0;j<3;j++)for(int k=0;k<3;k++){float p=m->vertices[triangle->v[j]].p[k];island->lo[k]=fminf(island->lo[k],p);island->hi[k]=fmaxf(island->hi[k],p);}
    }
    uint32_t live=0;
    for(uint32_t t=0;t<m->triangle_count;t++){
        SurfaceIsland *island=&islands[island_root(parent,m->triangles[t].v[0])];float extent=0;for(int k=0;k<3;k++)extent=fmaxf(extent,island->hi[k]-island->lo[k]);
        if(island->area<total*1e-5&&extent<span*.005f)continue;
        m->triangles[live++]=m->triangles[t];
    }
    if(live!=m->triangle_count){
        m->triangle_count=live;for(uint32_t i=0;i<m->vertex_count;i++)parent[i]=UINT32_MAX;
        for(uint32_t t=0;t<live;t++)for(int k=0;k<3;k++)parent[m->triangles[t].v[k]]=0;
        uint32_t vertices=0;for(uint32_t i=0;i<m->vertex_count;i++)if(parent[i]!=UINT32_MAX){parent[i]=vertices;m->vertices[vertices++]=m->vertices[i];}
        m->vertex_count=vertices;for(uint32_t t=0;t<live;t++)for(int k=0;k<3;k++)m->triangles[t].v[k]=parent[m->triangles[t].v[k]];
    }
    free(parent);free(islands);return !cancelled(cancel,error);
}

/* Preserve the reconstructed face and feet for every species. A low polygon
 * target is a preference, not permission to erase eyes or the end of a paw.
 * Continuous colours and smooth normals cover the entire animal consistently. */
int mesh_source_colours(const Mesh *source,Mesh *out,FsCancel * cancel,wchar_t *error){
    uint32_t *tree=malloc((size_t)source->vertex_count*sizeof(*tree));if(!tree){fs_error(error,L"원본 색상 처리 메모리가 부족합니다.");return 0;}
    for(uint32_t i=0;i<source->vertex_count;i++)tree[i]=i;colour_tree(source,tree,0,(int)source->vertex_count,0);
    for(uint32_t i=0;i<out->vertex_count;i++){
        if(!(i%4096)&&cancelled(cancel,error)){free(tree);return 0;}double distance=DBL_MAX;uint32_t nearest=0;
        nearest_colour(source,tree,0,(int)source->vertex_count,0,out->vertices[i].p,&distance,&nearest);
        memcpy(out->vertices[i].color,source->vertices[nearest].color,4);
    }free(tree);return 1;
}
int mesh_surface(const Mesh *source,Mesh *out,uint32_t target,FsCancel * cancel,
                 Quality *quality,int *kept_original,wchar_t *error){
    Mesh protected={0};uint32_t *tree=NULL;memset(out,0,sizeof(*out));*kept_original=0;
    if(target<300||target>200000||!mesh_validate(source,error))return 0;
    if(!mesh_copy(source,&protected)){fs_error(error,L"세부 보존 메모리가 부족합니다.");return 0;}
    if(!remove_surface_specks(&protected,cancel,error))goto fail;
    float lo[3],hi[3];mesh_bounds(source,lo,hi);
    for(uint32_t i=0;i<protected.vertex_count;i++){Vertex *v=&protected.vertices[i];
        v->smooth=1;v->detail=.15f;
        if(v->p[1]<lo[1]+.25f*(hi[1]-lo[1])){v->limb=1;v->detail=2;}
    }
    for(uint32_t t=0;t<protected.triangle_count;t++)for(int k=0;k<3;k++){
        uint32_t a=protected.triangles[t].v[k],b=protected.triangles[t].v[(k+1)%3];float contrast=0;
        for(int c=0;c<3;c++)contrast+=fabsf((float)protected.vertices[a].color[c]-protected.vertices[b].color[c])/255.f;
        protected.vertices[a].detail=fmaxf(protected.vertices[a].detail,contrast*12);
        protected.vertices[b].detail=fmaxf(protected.vertices[b].detail,contrast*12);
    }
    uint32_t budget=(uint32_t)fmin(source->triangle_count,fmax(target,20000));
    for(int attempt=0;attempt<6;attempt++){
        if(cancelled(cancel,error))goto fail;
        if(!mesh_copy(&protected,out)){fs_error(error,L"세부 메시 복사 메모리가 부족합니다.");goto fail;}
        if(!mesh_simplify(out,budget,cancel,error))goto fail;
        *quality=mesh_compare(source,out);
        if(quality->silhouette_iou>=.985&&quality->depth_rmse<=.012)break;
        mesh_free(out);budget=(uint32_t)fmin(source->triangle_count,ceil(budget*1.5));
        if(budget>=source->triangle_count||attempt==5){
            if(!mesh_copy(&protected,out)){fs_error(error,L"원본 세부 형상을 보존할 수 없습니다.");goto fail;}
            *kept_original=1;*quality=mesh_compare(source,out);break;
        }
    }
    tree=malloc((size_t)source->vertex_count*sizeof(*tree));if(!tree){fs_error(error,L"원본 색상 처리 메모리가 부족합니다.");goto fail;}
    for(uint32_t i=0;i<source->vertex_count;i++)tree[i]=i;colour_tree(source,tree,0,(int)source->vertex_count,0);
    for(uint32_t i=0;i<out->vertex_count;i++){
        if(!(i%4096)&&cancelled(cancel,error))goto fail;double distance=DBL_MAX;uint32_t nearest=0;
        nearest_colour(source,tree,0,(int)source->vertex_count,0,out->vertices[i].p,&distance,&nearest);
        memcpy(out->vertices[i].color,source->vertices[nearest].color,4);
    }
    mesh_normals(out);free(tree);mesh_free(&protected);return mesh_validate(out,error);
fail:free(tree);mesh_free(&protected);mesh_free(out);return 0;
}

int mesh_lowpoly(const Mesh *source,Mesh *out,uint32_t target,FsCancel * cancel,
                 Quality *quality,int *kept_original,wchar_t *error){
    Mesh conditioned={0};memset(out,0,sizeof(*out));*kept_original=0;
    if(target<300||target>200000||!mesh_validate(source,error))return 0;
    if(!mesh_copy(source,&conditioned)){fs_error(error,L"표면 정리 메모리가 부족합니다.");return 0;}
    if(!condition_surface(&conditioned,cancel,error)){mesh_free(&conditioned);return 0;}
    uint32_t budget=target;
    for(int attempt=0;attempt<8;attempt++){
        if(cancelled(cancel,error))goto failed;
        if(!mesh_copy(&conditioned,out)){fs_error(error,L"로우폴리 처리 메모리가 부족합니다.");goto failed;}
        if(!mesh_simplify(out,budget,cancel,error))goto failed;
        *quality=mesh_compare(source,out);
        if(quality->silhouette_iou>=.94&&quality->depth_rmse<=.035)break;
        mesh_free(out);
        if(budget>=source->triangle_count||attempt==7){
            if(!mesh_copy(source,out)){fs_error(error,L"원본 보존 메모리가 부족합니다.");goto failed;}
            *kept_original=1;*quality=mesh_compare(source,out);break;
        }
        budget=(uint32_t)fmin(source->triangle_count,ceil(budget*1.5));
    }
    if(cancelled(cancel,error)||!refine_detail_triangles(out,source,cancel,error))goto failed;
    *quality=mesh_compare(source,out);
    if(quality->silhouette_iou<.94||quality->depth_rmse>.035){
        mesh_free(out);if(!mesh_copy(source,out)){fs_error(error,L"얼굴 형상 보존 메모리가 부족합니다.");goto failed;}*kept_original=1;*quality=mesh_compare(source,out);
    }
    if(!facets_from(out,source,error))goto failed;
    mesh_free(&conditioned);return 1;
failed:mesh_free(&conditioned);mesh_free(out);return 0;
}
/* Broad, planar forms without the legacy eye refinement or 20k face floor. */
int mesh_form_surface(const Mesh *source,Mesh *out,Job *j){
    Mesh conditioned={0};memset(out,0,sizeof(*out));j->kept_original=0;
    if(j->target<300||j->target>200000||!mesh_validate(source,j->error))return 0;
    if(!mesh_copy(source,&conditioned)){fs_error(j->error,L"로우폴리 메모리가 부족합니다.");return 0;}
    if(!remove_surface_specks(&conditioned,j->cancel,j->error)||!condition_surface(&conditioned,j->cancel,j->error))goto fail;
    /* Coarser planes may deviate in depth while six-view outlines still
     * retain the ears, muzzle and feet. Do not restore dense fur geometry. */
    uint32_t budget=j->target;
    for(int attempt=0;attempt<8;attempt++){
        if(fs_cancelled(j)||!mesh_copy(&conditioned,out))goto fail;
        if(!mesh_simplify(out,budget,j->cancel,j->error))goto fail;
        j->quality=mesh_compare(&conditioned,out);
        if(j->quality.silhouette_iou>=.94&&j->quality.depth_rmse<=.05)break;
        mesh_free(out);
        if(budget>=conditioned.triangle_count||attempt==7){
            if(!mesh_copy(&conditioned,out))goto fail;j->kept_original=1;j->quality=mesh_compare(&conditioned,out);break;
        }
        budget=(uint32_t)fmin(conditioned.triangle_count,ceil(budget*1.5));
    }
    /* Each triangle stores one colour and one geometric normal. No original
     * photo reprojection may follow this step: it would restore the eyes. */
    if(fs_cancelled(j)||!mesh_facets(out,j->error))goto fail;
    mesh_free(&conditioned);return 1;
fail:mesh_free(&conditioned);mesh_free(out);return 0;
}
