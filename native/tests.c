#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "../vendor/cjson/cJSON.h"

static void torus(Mesh *m){
    const unsigned u=64,v=32;m->vertex_count=u*v;m->triangle_count=2*u*v;
    m->vertices=calloc(m->vertex_count,sizeof(Vertex));m->triangles=calloc(m->triangle_count,sizeof(Triangle));
    assert(m->vertices&&m->triangles);
    for(unsigned i=0;i<u;i++)for(unsigned j=0;j<v;j++){
        double a=6.283185307179586*i/u,b=6.283185307179586*j/v;Vertex *p=&m->vertices[i*v+j];
        p->p[0]=(float)((1+.3*cos(b))*cos(a));p->p[1]=(float)(.3*sin(b));p->p[2]=(float)((1+.3*cos(b))*sin(a));
        p->color[0]=(unsigned char)(100+80*cos(a));p->color[1]=130;p->color[2]=70;p->color[3]=255;
        unsigned x=i*v+j,y=((i+1)%u)*v+j,z=((i+1)%u)*v+(j+1)%v,w=i*v+(j+1)%v;
        m->triangles[2*x]=(Triangle){{x,z,y}};m->triangles[2*x+1]=(Triangle){{x,w,z}};
    }
    mesh_normals(m);
}
typedef struct{uint32_t a,b;} TestEdge;
static void add_tetrahedron(Mesh *m,float size){
    uint32_t nv=m->vertex_count,nt=m->triangle_count;
    m->vertices=realloc(m->vertices,(size_t)(nv+4)*sizeof(Vertex));m->triangles=realloc(m->triangles,(size_t)(nt+4)*sizeof(Triangle));assert(m->vertices&&m->triangles);
    const float p[4][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    const uint32_t f[4][3]={{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    for(int i=0;i<4;i++){Vertex *v=&m->vertices[nv+i];memset(v,0,sizeof(*v));for(int k=0;k<3;k++)v->p[k]=size*p[i][k];memset(v->color,128,3);v->color[3]=255;
        for(int k=0;k<3;k++)m->triangles[nt+i].v[k]=nv+f[i][k];}
    m->vertex_count+=4;m->triangle_count+=4;mesh_normals(m);
}
static int compare_edges(const void *x,const void *y){const TestEdge *a=x,*b=y;if(a->a!=b->a)return a->a<b->a?-1:1;return a->b==b->b?0:(a->b<b->b?-1:1);}
static void closed(const Mesh *m){
    uint32_t n=m->triangle_count*3;TestEdge *edges=malloc(n*sizeof(*edges));assert(edges);
    for(uint32_t i=0;i<m->triangle_count;i++)for(int k=0;k<3;k++){uint32_t a=m->triangles[i].v[k],b=m->triangles[i].v[(k+1)%3];edges[i*3+k]=(TestEdge){a<b?a:b,a<b?b:a};}
    qsort(edges,n,sizeof(*edges),compare_edges);
    for(uint32_t i=0;i<n;i+=2){assert(i+1<n);assert(!compare_edges(edges+i,edges+i+1));assert(i+2==n||compare_edges(edges+i,edges+i+2));}free(edges);
}
static void raw_collapsed_faces(void){
    const uint32_t h[3]={0x31534d46,4,6};
    const float points[12]={0,0,0,1,0,0,0,1,0,0,0,1};
    const unsigned char colors[16]={80,40,20,255,90,50,30,255,100,60,40,255,110,70,50,255};
    const Triangle valid[6]={{{0,0,1}},{{0,2,1}},{{0,1,3}},{{2,2,2}},{{0,3,2}},{{1,2,3}}};
    for(int scenario=0;scenario<4;scenario++){
        Triangle faces[6];float positions[12];memcpy(faces,valid,sizeof(faces));memcpy(positions,points,sizeof(positions));
        if(scenario==1)faces[0]=(Triangle){{99,99,0}};
        if(scenario==2)for(int i=0;i<6;i++)faces[i]=(Triangle){{0,0,0}};
        if(scenario==3)positions[0]=NAN;
        FILE *f=fs_wfopen(L"test-collapsed.raw",L"wb");assert(f);
        assert(fwrite(h,4,3,f)==3&&fwrite(positions,4,12,f)==12&&fwrite(colors,1,16,f)==16&&fwrite(faces,sizeof(Triangle),6,f)==6);assert(!fclose(f));
        Mesh m={0};wchar_t error[FS_ERROR]={0};int ok=mesh_read_raw(L"test-collapsed.raw",&m,error);
        if(scenario){assert(!ok&&!m.vertices&&!m.triangles&&error[0]);}
        else{assert(ok&&m.triangle_count==4&&m.vertex_count==4);closed(&m);assert(!memcmp(m.vertices[1].color,colors+4,4));assert(m.vertices[1].p[2]==-1);}
        mesh_free(&m);assert(fs_unlink(L"test-collapsed.raw"));
    }
    puts("PASS raw collapsed-face cleanup; invalid indices, empty surface and NaN remain rejected");
}
static void exports(const Mesh *mesh){
    wchar_t error[FS_ERROR]={0},directory[FS_PATH],path[FS_PATH];FsId id;assert(FS_SUCCEEDED(fs_create_id(&id)));
    fs_wformat(directory,FS_PATH,L"test-export-%08lx",id.value);assert(fs_mkdir(directory,NULL));
    const wchar_t *names[]={L"model.glb",L"model.stl",L"model.step",L"model.3mf"};
    for(unsigned formats=1;formats<=EXPORT_ALL;formats++){
        assert(mesh_export(directory,mesh,formats,100,"{}",NULL,error));
        for(int i=0;i<4;i++){
            assert(fs_join(path,directory,names[i]));assert((fs_attributes(path)!=INVALID_FILE_ATTRIBUTES)==!!(formats&(1u<<i)));
            if(formats&(1u<<i))assert(fs_unlink(path));
        }
    }
    assert(!mesh_export(directory,mesh,0,100,NULL,NULL,error));
    FsCancel * cancelled=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_export(directory,mesh,EXPORT_ALL,100,NULL,cancelled,error));fs_cancel_free(cancelled);
    for(int i=0;i<4;i++){fs_join(path,directory,names[i]);assert(fs_attributes(path)==INVALID_FILE_ATTRIBUTES);}
    error[0]=0;assert(!mesh_export(directory,mesh,EXPORT_STL,0,NULL,NULL,error));
    assert(!mesh_export(directory,mesh,EXPORT_STEP,NAN,NULL,NULL,error));
    Mesh open=*mesh;open.triangle_count--;assert(!mesh_export(directory,&open,EXPORT_STEP,100,NULL,NULL,error));
    fs_join(path,directory,L"model.step");assert(fs_attributes(path)==INVALID_FILE_ATTRIBUTES);
    assert(fs_rmdir(directory));
    puts("PASS all 15 export selections, no selection, invalid size, cancellation and non-solid STEP rejection");
    error[0]=0;assert(mesh_write_stl(L"test-export.stl",mesh,100,NULL,error));
    FILE *f=fs_wfopen(L"test-export.stl",L"rb");assert(f);assert(!fseeko(f,0,SEEK_END));assert(ftello(f)==84+50LL*mesh->triangle_count);
    assert(!fseeko(f,80,SEEK_SET));uint32_t triangles=0;assert(fread(&triangles,4,1,f)==1&&triangles==mesh->triangle_count);
    float record[12];assert(fread(record,4,12,f)==12);assert(fabs(sqrt(record[0]*record[0]+record[1]*record[1]+record[2]*record[2])-1)<1e-5);
    fclose(f);assert(fs_unlink(L"test-export.stl"));
}
static void save_existing(const Mesh *mesh){
    wchar_t directory[FS_PATH],relative[128],path[FS_PATH],output[FS_PATH],error[FS_ERROR]={0};FsId id;
    assert(FS_SUCCEEDED(fs_create_id(&id)));fs_wformat(relative,128,L"test-save-existing-%08lx",id.value);
    assert(fs_absolute(relative,FS_PATH,directory,NULL));assert(fs_mkdir(directory,NULL));
    Mesh before={0};assert(mesh_copy(mesh,&before));
    const unsigned selections[]={EXPORT_GLB,EXPORT_STL|EXPORT_STEP|EXPORT_3MF,EXPORT_3MF};
    for(int i=0;i<3;i++){
        Job job={0}; /* No photo, AI root, quality setting or generation target. */
        job.formats=selections[i];job.cad_size_mm=75;
        assert(fs_join(job.output,directory,i==2?L"3mf":(i?L"cad":L"glb")));
        assert(fs_save_model(&job,mesh));assert(job.final_triangles==mesh->triangle_count);
        assert(!memcmp(mesh->vertices,before.vertices,(size_t)mesh->vertex_count*sizeof(Vertex)));
        assert(!memcmp(mesh->triangles,before.triangles,(size_t)mesh->triangle_count*sizeof(Triangle)));
        assert(fs_join(path,job.output,L"report.json"));FILE *f=fs_wfopen(path,L"rb");assert(f);
        char report[4096]={0};size_t n=fread(report,1,sizeof(report)-1,f);assert(n>0);fclose(f);
        assert(strstr(report,"\"operation\":\"export_existing\""));assert(strstr(report,"\"regenerated\":false"));
        if(!i){Mesh loaded={0};assert(mesh_read_glb(job.result,&loaded,error));
            assert(loaded.vertex_count==mesh->vertex_count&&loaded.triangle_count==mesh->triangle_count);
            assert(!memcmp(loaded.triangles,mesh->triangles,(size_t)mesh->triangle_count*sizeof(Triangle)));
            for(uint32_t v=0;v<mesh->vertex_count;v++){assert(!memcmp(loaded.vertices[v].p,mesh->vertices[v].p,12));assert(!memcmp(loaded.vertices[v].color,mesh->vertices[v].color,4));}
            mesh_free(&loaded);
        }
        assert(!fs_save_model(&job,mesh)); /* Existing folder is not overwritten. */
        const wchar_t *names[]={L"model.glb",L"model.stl",L"model.step",L"model.3mf"};
        for(int k=0;k<4;k++){fs_join(path,job.output,names[k]);assert((fs_attributes(path)!=INVALID_FILE_ATTRIBUTES)==!!(selections[i]&(1u<<k)));if(selections[i]&(1u<<k))assert(fs_unlink(path));}
        fs_join(path,job.output,L"report.json");assert(fs_unlink(path));assert(fs_rmdir(job.output));
    }
    Job cancelled={0};cancelled.formats=EXPORT_STL;cancelled.cad_size_mm=100;
    cancelled.cancel=fs_cancel_create(NULL,1,1,NULL);assert(fs_join(output,directory,L"cancelled"));fs_wcopy(cancelled.output,FS_PATH,output);
    assert(!fs_save_model(&cancelled,mesh));assert(fs_attributes(output)==INVALID_FILE_ATTRIBUTES);fs_cancel_free(cancelled.cancel);
    mesh_free(&before);assert(fs_rmdir(directory));
    puts("PASS existing model save without AI/photo, unchanged geometry/colours, selected files, overwrite protection and cancellation");
}
static void test_json_file(const wchar_t *directory,const wchar_t *name,const char *json){
    wchar_t path[FS_PATH];assert(fs_join(path,directory,name));FILE *out=fs_wfopen(path,L"wb");assert(out);
    assert(fwrite(json,1,strlen(json),out)==strlen(json));assert(!fclose(out));
}
static void sparse_references(const Mesh *source){
    wchar_t directory[FS_PATH],path[FS_PATH],error[FS_ERROR]={0};FsId id;assert(FS_SUCCEEDED(fs_create_id(&id)));
    fs_wformat(directory,FS_PATH,L"test-references-%08lx",id.value);assert(fs_mkdir(directory,NULL));
    Photo mask={0};mask.width=mask.height=16;mask.rgba=malloc(16*16*4);assert(mask.rgba);
    for(int i=0;i<256;i++){mask.rgba[i*4]=140;mask.rgba[i*4+1]=130;mask.rgba[i*4+2]=70;mask.rgba[i*4+3]=255;}
    fs_join(path,directory,L"mask.rgba");assert(photo_write(path,&mask,error));photo_free(&mask);
    for(int scenario=0;scenario<4;scenario++){
        test_json_file(directory,L"references.json",scenario?"[{\"categoryView\":\"side\"}]":"[]");
        const char *analysis=scenario==0?"[]":(scenario==1?
            "[{\"context\":{\"medium\":{\"label\":\"photo\"},\"framing\":{\"label\":\"whole\"}},\"mask\":\"mask.rgba\"}]":(scenario==2?
            "[{\"context\":{\"medium\":{\"label\":\"photo\"},\"framing\":{\"label\":\"multiple\"}},\"mask\":\"mask.rgba\"}]":
            "[{\"context\":{},\"mask\":\"mask.rgba\"}]"));
        test_json_file(directory,L"reference-analysis.json",analysis);
        Job job={0};Mesh output={0};assert(mesh_copy(source,&output));assert(references_apply(&job,directory,&output));
        assert(job.reference_count==(scenario==1?1:0));
        assert(!memcmp(output.triangles,source->triangles,(size_t)source->triangle_count*sizeof(Triangle)));
        int changes=0;for(uint32_t i=0;i<source->vertex_count;i++){
            assert(!memcmp(output.vertices[i].p,source->vertices[i].p,12));
            for(int k=0;k<3;k++){int difference=abs((int)output.vertices[i].color[k]-source->vertices[i].color[k]);assert(difference<20);changes+=difference;}
        }
        if(scenario==1)assert(changes>0);else assert(!changes);mesh_free(&output);
    }
    const wchar_t *files[]={L"references.json",L"reference-analysis.json",L"mask.rgba"};for(int i=0;i<3;i++){fs_join(path,directory,files[i]);assert(fs_unlink(path));}
    assert(fs_rmdir(directory));puts("PASS zero/one reference continues, unusable group/invalid photos stay excluded, geometry and zero-reference colours preserved");
}
static void shape_completion(const Mesh *source){
    Mesh flat={0},prior={0},completed={0},unchanged={0};wchar_t error[FS_ERROR]={0};double iou=0,depth=0;
    assert(mesh_copy(source,&flat)&&mesh_copy(source,&prior)&&mesh_copy(source,&unchanged));
    for(uint32_t i=0;i<flat.vertex_count;i++)flat.vertices[i].p[2]*=.22f;
    for(uint32_t i=0;i<prior.vertex_count;i++){prior.vertices[i].color[0]=10;prior.vertices[i].color[1]=10;prior.vertices[i].color[2]=240;}
    assert(mesh_complete(&flat,&prior,&completed,NULL,&iou,&depth,error));assert(iou>.8&&depth>1.1&&depth<2.3);
    assert(completed.vertex_count==flat.vertex_count&&completed.triangle_count==flat.triangle_count);
    assert(!memcmp(completed.triangles,flat.triangles,(size_t)flat.triangle_count*sizeof(Triangle)));
    unsigned changed_rear=0;for(uint32_t i=0;i<flat.vertex_count;i++){
        assert(!memcmp(completed.vertices[i].p,flat.vertices[i].p,8));
        assert(!memcmp(completed.vertices[i].color,flat.vertices[i].color,4));
        if(flat.vertices[i].p[2]<=0)assert(completed.vertices[i].p[2]==flat.vertices[i].p[2]);
        else changed_rear+=completed.vertices[i].p[2]>flat.vertices[i].p[2];
    }assert(changed_rear>0);
    closed(&completed);assert(completed.triangle_count>0);
    double blue=0;for(uint32_t i=0;i<completed.vertex_count;i++)blue+=completed.vertices[i].color[2];assert(blue/completed.vertex_count<130);
    assert(!memcmp(prior.triangles,unchanged.triangles,(size_t)prior.triangle_count*sizeof(Triangle)));
    for(uint32_t i=0;i<prior.vertex_count;i++)assert(!memcmp(prior.vertices[i].p,unchanged.vertices[i].p,12));
    mesh_free(&completed);FsCancel * cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_complete(&flat,&prior,&completed,cancel,&iou,&depth,error));fs_cancel_free(cancel);
    mesh_free(&flat);mesh_free(&prior);mesh_free(&unchanged);
    puts("PASS reference geometry changes hidden depth, preserves closed topology/source data, rejects reference coat colours and honors cancellation");
    wchar_t directory[FS_PATH],path[FS_PATH];FsId id;assert(FS_SUCCEEDED(fs_create_id(&id)));fs_wformat(directory,FS_PATH,L"test-shape-%08lx",id.value);assert(fs_mkdir(directory,NULL));
    test_json_file(directory,L"input-analysis.json","{\"resolvedPose\":\"lying\"}");
    test_json_file(directory,L"species.json","{\"scientific\":\"Felis catus\"}");
    const char *matching="[{\"context\":{\"pose\":{\"bestCandidate\":\"lying\",\"scores\":{\"lying\":0.8}},\"medium\":{\"scores\":{\"photo\":0.9}},\"framing\":{\"bestCandidate\":\"whole\",\"scores\":{\"whole\":0.8}}},\"speciesCandidates\":[{\"scientific\":\"Felis catus\",\"score\":0.9}]}]";
    test_json_file(directory,L"reference-analysis.json",matching);assert(shape_candidate_count(directory)==1);
    test_json_file(directory,L"input-analysis.json","{\"resolvedPose\":\"standing\"}");assert(shape_candidate_count(directory)==0);
    test_json_file(directory,L"input-analysis.json","{\"resolvedPose\":\"lying\"}");test_json_file(directory,L"species.json","{\"scientific\":\"Vulpes vulpes\"}");assert(shape_candidate_count(directory)==0);
    /* Disabled or unsuitable references must keep the original geometry and
     * report source_only, without launching an inference process. */
    for(int disabled=0;disabled<2;disabled++){
        Job job={0};job.shape_disabled=disabled;fs_wcopy(job.species,128,L"Vulpes vulpes");
        Mesh fallback={0};assert(references_complete(&job,directory,source,&fallback));
        assert(!job.shape_used&&fallback.triangle_count==source->triangle_count);
        assert(!memcmp(fallback.vertices,source->vertices,(size_t)source->vertex_count*sizeof(Vertex)));
        mesh_free(&fallback);fs_join(path,directory,L"shape-completion.json");FILE *f=fs_wfopen(path,L"rb");assert(f);
        char data[4096];size_t n=fread(data,1,sizeof(data)-1,f);fclose(f);data[n]=0;cJSON *report=cJSON_Parse(data);assert(report);
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(report,"applied")));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(report,"geometryMode")),"source_only"));cJSON_Delete(report);
    }
    const wchar_t *files[]={L"input-analysis.json",L"reference-analysis.json",L"species.json",L"shape-completion.json"};for(int i=0;i<4;i++){fs_join(path,directory,files[i]);assert(fs_unlink(path));}assert(fs_rmdir(directory));
    puts("PASS same-species/same-pose selection; mismatched posture and wrong species excluded");
}
static void detail_refinement(const Mesh *source){
    wchar_t directory[FS_PATH],path[FS_PATH];FsId id;assert(FS_SUCCEEDED(fs_create_id(&id)));fs_wformat(directory,FS_PATH,L"test-detail-%08lx",id.value);assert(fs_mkdir(directory,NULL));
    Photo photo={0};photo.width=photo.height=128;photo.rgba=malloc(128*128*4);assert(photo.rgba);
    for(int y=0;y<128;y++)for(int x=0;x<128;x++){unsigned char *c=photo.rgba+(y*128+x)*4;c[0]=x<64?220:80;c[1]=y<64?110:180;c[2]=40;c[3]=255;}
    Job job={0};fs_join(path,directory,L"cutout.rgba");assert(photo_write(path,&photo,job.error));photo_free(&photo);
    test_json_file(directory,L"input-analysis.json","{\"faceBox\":[0.1,0.05,0.8,0.9]}");
    Mesh detail={0};assert(mesh_copy(source,&detail));assert(mesh_refine_details(&job,directory,&detail));
    assert(job.face_detail_vertices>0);closed(&detail);
    float lo[3],hi[3];mesh_bounds(source,lo,hi);float span=fmaxf(hi[0]-lo[0],fmaxf(hi[1]-lo[1],hi[2]-lo[2]));
    int colors=0,geometry=0,rear=0;for(uint32_t i=0;i<source->vertex_count;i++){
        double displacement=0;for(int k=0;k<3;k++){double d=detail.vertices[i].p[k]-source->vertices[i].p[k];displacement+=d*d;}
        assert(displacement<.065*.065*span*span);geometry+=displacement>1e-12;
        colors+=memcmp(detail.vertices[i].color,source->vertices[i].color,4)!=0;
        if(source->vertices[i].p[2]>lo[2]+.85f*(hi[2]-lo[2])){assert(!memcmp(detail.vertices[i].color,source->vertices[i].color,4));rear++;}
    }
    assert(colors>0&&geometry>0&&rear>0);
    Mesh reduced={0};Quality quality={0};int kept=0;assert(mesh_lowpoly(&detail,&reduced,300,NULL,&quality,&kept,job.error));
    unsigned face=0,features=0;for(uint32_t i=0;i<reduced.triangle_count;i++){int a=0,b=0;for(int k=0;k<3;k++){float value=reduced.vertices[reduced.triangles[i].v[k]].detail;a+=value>.65f;b+=value>3.f;}face+=a>=2;features+=b>=2;}
    assert(face>=220&&features>=48&&reduced.triangle_count>300);
    assert(mesh_finish_face(&job,directory,&reduced));
    int smooth_faces=0;for(uint32_t i=0;i<reduced.triangle_count;i++){const Triangle *t=&reduced.triangles[i];if(memcmp(reduced.vertices[t->v[0]].n,reduced.vertices[t->v[1]].n,12))smooth_faces++;}assert(smooth_faces>20);
    Mesh reopened={0};fs_join(path,directory,L"smooth.glb");assert(mesh_write_glb(path,&reduced,"{}",job.error));assert(mesh_read_glb(path,&reopened,job.error));
    for(uint32_t i=0;i<reduced.vertex_count;i++){for(int k=0;k<3;k++)assert(fabsf(reopened.vertices[i].n[k]-reduced.vertices[i].n[k])<1e-5f);assert(!memcmp(reopened.vertices[i].color,reduced.vertices[i].color,4));}
    mesh_free(&reopened);assert(fs_unlink(path));mesh_free(&reduced);mesh_free(&detail);
    assert(mesh_copy(source,&detail));job.cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_refine_details(&job,directory,&detail));
    assert(!memcmp(detail.vertices,source->vertices,(size_t)source->vertex_count*sizeof(Vertex)));fs_cancel_free(job.cancel);mesh_free(&detail);
    const wchar_t *files[]={L"input-analysis.json",L"cutout.rgba"};for(int i=0;i<2;i++){fs_join(path,directory,files[i]);assert(fs_unlink(path));}assert(fs_rmdir(directory));
    puts("PASS bounded face/crease relief, front-only photo colour, closed topology, adaptive facial detail and cancellation");
}
static int animal_cell(int x,int y,int z){return x>=0&&x<5&&y>=0&&y<4&&z>=0&&z<7&&(y>=2||((x==0||x==4)&&(z==0||z==6)));}
static void body_branches(void){
    /* One closed solid with a torso and four genuinely separate legs; no
     * overlapping primitives. It catches accidental leg-ID merging in QEM. */
    Mesh m={0},coarse={0},before={0};m.vertex_count=6*5*8;m.vertices=calloc(m.vertex_count,sizeof(Vertex));m.triangles=malloc(2000*sizeof(Triangle));assert(m.vertices&&m.triangles);
    for(int z=0;z<8;z++)for(int y=0;y<5;y++)for(int x=0;x<6;x++){Vertex *v=&m.vertices[x+6*(y+5*z)];v->p[0]=(float)x;v->p[1]=(float)y;v->p[2]=(float)z;v->color[0]=180;v->color[1]=100;v->color[2]=50;v->color[3]=255;}
    const int neighbor[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const int corner[6][4][3]={{{1,0,0},{1,1,0},{1,1,1},{1,0,1}},{{0,0,0},{0,0,1},{0,1,1},{0,1,0}},{{0,1,0},{0,1,1},{1,1,1},{1,1,0}},{{0,0,0},{1,0,0},{1,0,1},{0,0,1}},{{0,0,1},{1,0,1},{1,1,1},{0,1,1}},{{0,0,0},{0,1,0},{1,1,0},{1,0,0}}};
    for(int z=0;z<7;z++)for(int y=0;y<4;y++)for(int x=0;x<5;x++)if(animal_cell(x,y,z))for(int side=0;side<6;side++)if(!animal_cell(x+neighbor[side][0],y+neighbor[side][1],z+neighbor[side][2])){
        uint32_t q[4];for(int k=0;k<4;k++)q[k]=(x+corner[side][k][0])+6*((y+corner[side][k][1])+5*(z+corner[side][k][2]));
        m.triangles[m.triangle_count++]=(Triangle){{q[0],q[1],q[2]}};m.triangles[m.triangle_count++]=(Triangle){{q[0],q[2],q[3]}};
    }
    mesh_normals(&m);wchar_t error[FS_ERROR]={0};assert(mesh_validate(&m,error));closed(&m);
    assert(mesh_limb_branches(&m,1,NULL,error)==4);assert(mesh_copy(&m,&coarse));assert(mesh_simplify(&coarse,160,NULL,error));closed(&coarse);
    unsigned ids=0;for(uint32_t i=0;i<coarse.vertex_count;i++)if(coarse.vertices[i].limb_id)ids|=1u<<coarse.vertices[i].limb_id;assert(ids==30);mesh_free(&coarse);
    for(uint32_t i=0;i<m.vertex_count;i++)m.vertices[i].p[1]+=.05f*m.vertices[i].p[0]+.08f*m.vertices[i].p[2];mesh_normals(&m);
    Job job={0};job.body_fit.applied=1;job.body_fit.separated_branches=4;assert(mesh_copy(&m,&before));
    assert(mesh_ground_body(&job,&m));assert(job.body_fit.ground_aligned&&job.body_fit.foot_height_spread_after<1e-5);closed(&m);
    for(uint32_t i=1;i<m.vertex_count;i++){double a=0,b=0;for(int k=0;k<3;k++){double x=before.vertices[i].p[k]-before.vertices[0].p[k],y=m.vertices[i].p[k]-m.vertices[0].p[k];a+=x*x;b+=y*y;}assert(fabs(a-b)<1e-4);}
    job.cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_ground_body(&job,&m));fs_cancel_free(job.cancel);mesh_free(&before);mesh_free(&m);
    puts("PASS four separated leg branches, no cross-leg QEM collapse, rigid ground alignment and cancellation");
}
static void observed_surface(void){
    Job job={0};job.resolution=256;Mesh base={0},result={0};Photo photo={0};
    const float points[8][3]={{-1,-1,-.5f},{1,-1,-.5f},{1,1,-.5f},{-1,1,-.5f},{-1,-1,.5f},{1,-1,.5f},{1,1,.5f},{-1,1,.5f}};
    const Triangle triangles[12]={{{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},{{0,4,7}},{{0,7,3}},{{1,2,6}},{{1,6,5}},{{0,1,5}},{{0,5,4}},{{3,7,6}},{{3,6,2}}};
    base.vertex_count=8;base.triangle_count=12;base.vertices=calloc(8,sizeof(Vertex));base.triangles=malloc(sizeof(triangles));assert(base.vertices&&base.triangles);memcpy(base.triangles,triangles,sizeof(triangles));
    for(int i=0;i<8;i++){memcpy(base.vertices[i].p,points[i],12);memset(base.vertices[i].color,128,3);base.vertices[i].color[3]=255;}mesh_normals(&base);
    photo.width=96;photo.height=80;photo.rgba=calloc(96*80,4);float values[96*80];assert(photo.rgba);
    for(int y=0;y<80;y++)for(int x=0;x<96;x++){int i=y*96+x;values[i]=2.f+.002f*x+.001f*y;
        photo.rgba[i*4]=180;photo.rgba[i*4+1]=100;photo.rgba[i*4+2]=50;
        photo.rgba[i*4+3]=(x>=8&&x<88&&y>=8&&y<72&&!(x>32&&x<64&&y>48))?255:0;}
    FILE *f=fs_wfopen(L"test-depth.bin",L"wb");assert(f);uint32_t header[3]={0x31504446,96,80};assert(fwrite(header,4,3,f)==3&&fwrite(values,4,96*80,f)==96*80);fclose(f);
    assert(mesh_observed(&job,&photo,L"test-depth.bin",&base,&result));printf("Observed test: applied=%d photo IoU %.6f -> %.6f\n",job.observed_surface,job.observed_iou_before,job.observed_iou_after);fflush(stdout);
    assert(job.observed_surface&&job.observed_iou_after>.92&&job.observed_iou_after>job.observed_iou_before+.05);closed(&result);
    int front=0,rear=0;for(uint32_t i=0;i<result.vertex_count;i++){const Vertex *v=&result.vertices[i];if(v->observed){front++;assert(v->face==1&&v->source_uv[0]>=0&&v->source_uv[0]<=1);assert(v->color[0]==180);}else{rear++;assert(v->face==0&&v->color[0]==128);}}
    assert(front==rear&&front>100);assert(mesh_write_step(L"test-observed.step",&result,100,NULL,job.error));assert(fs_unlink(L"test-observed.step"));mesh_free(&result);
    job.cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_observed(&job,&photo,L"test-depth.bin",&base,&result));fs_cancel_free(job.cancel);job.cancel=NULL;job.error[0]=0;
    f=fs_wfopen(L"test-depth.bin",L"wb");assert(f);header[1]=95;fwrite(header,4,3,f);fclose(f);assert(!mesh_observed(&job,&photo,L"test-depth.bin",&base,&result));assert(!result.vertices);
    assert(fs_unlink(L"test-depth.bin"));mesh_free(&base);photo_free(&photo);
    puts("PASS photo silhouette/depth reconstruction: closed surface, source front colour, rear separation, STEP, cancellation and corrupt depth rejection");
}
static void gbif_media_filter(void){
    const char *payload="{\"results\":[{\"key\":123,\"taxonKey\":7,\"basisOfRecord\":\"HUMAN_OBSERVATION\",\"license\":\"https://creativecommons.org/publicdomain/zero/1.0/\",\"media\":[{\"type\":\"StillImage\",\"format\":\"image/jpeg\",\"identifier\":\"https://example.org/dog.jpg\",\"creator\":\"Photographer\",\"license\":\"https://creativecommons.org/licenses/by/4.0/\"}]}]}";
    char *text=gbif_reference_media(payload,"Dog",7);cJSON *a=cJSON_Parse(text);free(text);assert(cJSON_GetArraySize(a)==1);cJSON *r=cJSON_GetArrayItem(a,0);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(r,"url")->valuestring,"api.gbif.org/v1/image/cache/640x/occurrence/123/media/"));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"taxonVerified")));cJSON_Delete(a);
    text=gbif_reference_media(payload,"Wrong species",8);a=cJSON_Parse(text);free(text);assert(cJSON_GetArraySize(a)==0);cJSON_Delete(a);
    cJSON *root=cJSON_Parse(payload),*record=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"results"),0),*media=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(record,"media"),0);
    const char *invalid[]={"https://creativecommons.org/licenses/by-nc/4.0/","https://creativecommons.org.evil.example/licenses/by/4.0/","","https://creativecommons.org/licenses/by-nd/4.0/"};
    for(int i=0;i<4;i++){cJSON_ReplaceItemInObjectCaseSensitive(media,"license",cJSON_CreateString(invalid[i]));char *p=cJSON_PrintUnformatted(root);text=gbif_reference_media(p,"Dog",7);free(p);a=cJSON_Parse(text);free(text);assert(cJSON_GetArraySize(a)==0);cJSON_Delete(a);}
    cJSON_DeleteItemFromObjectCaseSensitive(media,"license");char *p=cJSON_PrintUnformatted(root);text=gbif_reference_media(p,"Dog",7);free(p);a=cJSON_Parse(text);free(text);assert(cJSON_GetArraySize(a)==0);cJSON_Delete(a);cJSON_Delete(root);
    text=gbif_reference_media("invalid JSON","Dog",7);a=cJSON_Parse(text);free(text);assert(cJSON_GetArraySize(a)==0);cJSON_Delete(a);
    puts("PASS GBIF exact taxon, media-level licence, attribution, malformed input and restricted licence filtering");
}
static void volume_fallback(const Mesh *source){
    Job job={0};VolumeGuide guide={0};Photo photo={0};Mesh out={0};assert(mesh_volume(&job,&photo,source,&guide,&out));assert(!job.volume_head&&!job.volume_legs);assert(out.vertex_count==source->vertex_count);
    for(uint32_t i=0;i<out.vertex_count;i++)assert(!memcmp(out.vertices[i].p,source->vertices[i].p,12));mesh_free(&out);
    job.cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_volume(&job,&photo,source,&guide,&out));fs_cancel_free(job.cancel);puts("PASS unavailable anatomical evidence preserves source geometry; cancellation honored");
}

static void form_style(const Mesh *mesh){
    Job j={0};j.target=200000;Mesh input={0},prepared={0},out={0};Photo photo={0};assert(mesh_copy(mesh,&input));
    photo.width=photo.height=100;photo.rgba=malloc(40000);assert(photo.rgba);
    for(int y=0;y<100;y++)for(int x=0;x<100;x++){unsigned char *c=photo.rgba+4*(y*100+x);c[0]=170;c[1]=120;c[2]=70;c[3]=255;if((abs(x-35)<5||abs(x-65)<5)&&abs(y-40)<4)c[0]=c[1]=c[2]=5;}
    for(uint32_t i=0;i<input.vertex_count;i++){Vertex *v=&input.vertices[i];v->face=v->smooth=1;v->observed=1;v->detail=8;v->source_uv[0]=.1f;v->source_uv[1]=.9f;}
    input.vertices[0].source_uv[0]=35.f/99;input.vertices[0].source_uv[1]=40.f/99;memset(input.vertices[0].color,5,3);
    input.vertices[1].source_uv[0]=65.f/99;input.vertices[1].source_uv[1]=40.f/99;memset(input.vertices[1].color,5,3);
    wchar_t directory[FS_PATH],path[FS_PATH];FsId id;assert(FS_SUCCEEDED(fs_create_id(&id)));fs_wformat(directory,FS_PATH,L"test-form-%08lx",id.value);assert(fs_mkdir(directory,NULL));fs_join(path,directory,L"input-analysis.json");
    FILE *f=fs_wfopen(path,L"wb");assert(f);fputs("{\"animalPose\":{\"points\":[{\"x\":0.35,\"y\":0.4,\"score\":1},{\"x\":0.65,\"y\":0.4,\"score\":1},{\"x\":0.5,\"y\":0.6,\"score\":1}]}}",f);fclose(f);
    assert(mesh_form_prepare(&j,directory,&photo,&input,&prepared));assert(j.eye_regions==2&&!j.eye_colour_fallback&&j.eye_colour_vertices>=2);
    for(int i=0;i<2;i++){assert(prepared.vertices[i].color[0]==170&&prepared.vertices[i].color[1]==120&&prepared.vertices[i].color[2]==70);assert(!memcmp(input.vertices[i].p,prepared.vertices[i].p,12));}
    assert(input.vertices[0].color[0]==5);assert(!memcmp(input.vertices[2].color,prepared.vertices[2].color,4));
    for(uint32_t i=0;i<prepared.vertex_count;i++)assert(!prepared.vertices[i].observed&&!prepared.vertices[i].face&&!prepared.vertices[i].smooth&&!prepared.vertices[i].detail);
    assert(mesh_form_surface(&prepared,&out,&j));assert(j.target==FS_FORM_BASE_TRIANGLES);assert(out.triangle_count>=j.target&&out.triangle_count<=FS_FORM_MAX_TRIANGLES);assert(out.vertex_count==3*out.triangle_count);
    for(uint32_t i=0;i<out.triangle_count;i++){const Vertex *a=&out.vertices[out.triangles[i].v[0]];for(int k=1;k<3;k++){const Vertex *b=&out.vertices[out.triangles[i].v[k]];assert(!memcmp(a->color,b->color,4));assert(!memcmp(a->n,b->n,12));}}
    Job automatic={0};Mesh repeat={0};assert(mesh_form_surface(&prepared,&repeat,&automatic));
    assert(automatic.target==j.target&&repeat.triangle_count==out.triangle_count);
    assert(!memcmp(repeat.vertices,out.vertices,(size_t)out.vertex_count*sizeof(Vertex)));mesh_free(&repeat);
    assert(mesh_write_step(L"test-form.step",&out,100,NULL,j.error));assert(fs_unlink(L"test-form.step"));
    assert(mesh_write_glb(L"test-form.glb",&out,"{}",j.error));Mesh read={0};assert(mesh_read_glb(L"test-form.glb",&read,j.error));
    for(uint32_t i=0;i<out.vertex_count;i++)assert(!memcmp(out.vertices[i].color,read.vertices[i].color,4));mesh_free(&read);assert(fs_unlink(L"test-form.glb"));
    mesh_free(&out);mesh_free(&prepared);assert(mesh_form_prepare(&j,NULL,NULL,&input,&prepared));assert(j.eye_colour_fallback);
    for(uint32_t i=1;i<prepared.vertex_count;i++)assert(!memcmp(prepared.vertices[0].color,prepared.vertices[i].color,3));mesh_free(&prepared);
    j.cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_form_model(&j,directory,&photo,&input,&out));fs_cancel_free(j.cancel);
    assert(fs_unlink(path));assert(fs_rmdir(directory));photo_free(&photo);mesh_free(&input);
    puts("PASS eye paint removal, unchanged source/coat outside mask, explicit no-landmark fallback, automatic bounded face budget regardless of caller target, flat stored normals/colours, STEP and cancellation");
}

int main(void){fs_init();
    gbif_media_filter();
    observed_surface();
    wchar_t error[FS_ERROR]={0};Mesh a={0},b={0},loaded={0};torus(&a);assert(mesh_validate(&a,error));closed(&a);assert(mesh_copy(&a,&b));
    Quality same=mesh_compare(&a,&b);assert(same.silhouette_iou==1&&same.depth_rmse==0);
    assert(mesh_simplify(&b,1600,NULL,error));assert(b.triangle_count<=1600);closed(&b);
    Quality q=mesh_compare(&a,&b);printf("QEM quality: IoU %.6f, depth %.6f\n",q.silhouette_iou,q.depth_rmse);fflush(stdout);assert(q.silhouette_iou>.94&&q.depth_rmse<.035);
    printf("PASS C QEM: %u -> %u, six-view IoU %.6f, depth %.6f, closed topology\n",a.triangle_count,b.triangle_count,q.silhouette_iou,q.depth_rmse);
    assert(mesh_write_glb(L"test-roundtrip.glb",&b,"{\"test\":true}",error));assert(mesh_read_glb(L"test-roundtrip.glb",&loaded,error));
    assert(loaded.vertex_count==b.vertex_count&&loaded.triangle_count==b.triangle_count);
    for(uint32_t i=0;i<b.vertex_count;i++){for(int k=0;k<3;k++)assert(fabs(b.vertices[i].p[k]-loaded.vertices[i].p[k])<1e-6);for(int k=0;k<4;k++)assert(b.vertices[i].color[k]==loaded.vertices[i].color[k]);}
    puts("PASS GLB roundtrip: geometry, linear/sRGB vertex colors, Unicode-capable I/O");
    FsCancel * cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_simplify(&loaded,300,cancel,error));fs_cancel_free(cancel);
    puts("PASS cancellation is honored");
    /* Per-face attributes must survive a real GLB save/reopen, without moving
     * geometry or turning the style into a viewport-only shading setting. */
    Mesh faceted={0},faceted_read={0};assert(mesh_copy(&b,&faceted));assert(mesh_facets(&faceted,error));
    assert(faceted.vertex_count==faceted.triangle_count*3&&faceted.triangle_count==b.triangle_count);
    for(uint32_t i=0;i<faceted.triangle_count;i++){
        const Vertex *v=&faceted.vertices[i*3];
        for(int k=0;k<3;k++){
            assert(!memcmp(v[0].color,v[k].color,4));assert(!memcmp(v[0].n,v[k].n,12));
            assert(!memcmp(v[k].p,b.vertices[b.triangles[i].v[k]].p,12));
        }
        float u[3],w[3],n[3];for(int k=0;k<3;k++){u[k]=v[1].p[k]-v[0].p[k];w[k]=v[2].p[k]-v[0].p[k];}
        n[0]=u[1]*w[2]-u[2]*w[1];n[1]=u[2]*w[0]-u[0]*w[2];n[2]=u[0]*w[1]-u[1]*w[0];
        float length=sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);for(int k=0;k<3;k++)assert(fabsf(n[k]/length-v[0].n[k])<1e-5f);
    }
    assert(mesh_write_glb(L"test-faceted.glb",&faceted,"{\"style\":\"faceted-c-v1\"}",error));
    exports(&faceted);
    save_existing(&faceted);
    sparse_references(&faceted);
    shape_completion(&a);
    volume_fallback(&a);
    detail_refinement(&a);
    body_branches();
    assert(mesh_read_glb(L"test-faceted.glb",&faceted_read,error));
    for(uint32_t i=0;i<faceted.vertex_count;i++){
        assert(!memcmp(faceted.vertices[i].color,faceted_read.vertices[i].color,4));
        for(int k=0;k<3;k++)assert(fabsf(faceted.vertices[i].n[k]-faceted_read.vertices[i].n[k])<1e-5f);
    }
    mesh_free(&faceted);mesh_free(&faceted_read);fs_unlink(L"test-faceted.glb");
    puts("PASS flat face normals and constant face colors are exported and reopened without geometry changes");
    form_style(&a);
    raw_collapsed_faces();
    Mesh adaptive={0};Quality preservation={0};int kept=0;
    assert(mesh_lowpoly(&a,&adaptive,300,NULL,&preservation,&kept,error));
    assert(preservation.silhouette_iou>=.94&&preservation.depth_rmse<=.035);
    assert(adaptive.triangle_count>300&&adaptive.triangle_count<a.triangle_count);
    mesh_free(&adaptive);puts("PASS adaptive low-poly budget preserves shape instead of silently dropping the style");
    Mesh surface={0};assert(mesh_surface(&a,&surface,300,NULL,&preservation,&kept,error));
    assert(surface.triangle_count==a.triangle_count);closed(&surface);
    for(uint32_t i=0;i<a.vertex_count;i++){assert(!memcmp(a.vertices[i].p,surface.vertices[i].p,12));assert(!memcmp(a.vertices[i].color,surface.vertices[i].color,4));}
    mesh_free(&surface);FsCancel * surface_cancel=fs_cancel_create(NULL,1,1,NULL);assert(!mesh_surface(&a,&surface,300,surface_cancel,&preservation,&kept,error));fs_cancel_free(surface_cancel);
    puts("PASS detailed surface retains low-count geometry and colours without faceting; cancellation honored");
    Mesh specks={0};assert(mesh_copy(&a,&specks));add_tetrahedron(&specks,.0001f);error[0]=0;
    assert(mesh_surface(&specks,&surface,300,NULL,&preservation,&kept,error));assert(surface.triangle_count==a.triangle_count&&surface.vertex_count==a.vertex_count);closed(&surface);
    for(uint32_t i=0;i<a.vertex_count;i++)assert(!memcmp(a.vertices[i].p,surface.vertices[i].p,12));
    assert(mesh_write_step(L"test-clean-surface.step",&surface,100,NULL,error));assert(fs_unlink(L"test-clean-surface.step"));mesh_free(&surface);mesh_free(&specks);
    assert(mesh_copy(&a,&specks));add_tetrahedron(&specks,.1f);
    assert(mesh_surface(&specks,&surface,300,NULL,&preservation,&kept,error));assert(surface.triangle_count==specks.triangle_count);mesh_free(&surface);
    assert(mesh_simplify(&specks,300,NULL,error));closed(&specks);assert(mesh_write_step(L"test-tetra-preserved.step",&specks,100,NULL,error));assert(fs_unlink(L"test-tetra-preserved.step"));mesh_free(&specks);
    puts("PASS microscopic islands removed without trimming substantial parts; tetrahedra retain volume during QEM");
    FILE *bad=fs_wfopen(L"test-corrupt.raw",L"wb");assert(bad);uint32_t header[3]={0x31534d46,UINT32_MAX,1};fwrite(header,4,3,bad);fclose(bad);
    Mesh invalid={0};assert(!mesh_read_raw(L"test-corrupt.raw",&invalid,error));
    Photo p={0},prepared={0},roundtrip={0};p.width=p.height=4;p.rgba=calloc(16,4);assert(p.rgba);
    for(int y=1;y<=2;y++)for(int x=1;x<=2;x++){unsigned char *c=p.rgba+(y*4+x)*4;c[0]=255;c[3]=255;}
    assert(photo_prepare(&p,&prepared,error));assert(prepared.width==3&&prepared.height==3);assert(prepared.rgba[3]==255);
    assert(photo_write(L"test-image.rgba",&prepared,error));assert(photo_read(L"test-image.rgba",&roundtrip,error));assert(!memcmp(prepared.rgba,roundtrip.rgba,36));
    memset(p.rgba,0,64);Photo empty={0};assert(!photo_prepare(&p,&empty,error));
    puts("PASS image crop/compositing, binary roundtrip, malformed data and empty foreground rejection");
    photo_free(&p);photo_free(&prepared);photo_free(&roundtrip);mesh_free(&a);mesh_free(&b);mesh_free(&loaded);
    fs_unlink(L"test-roundtrip.glb");fs_unlink(L"test-corrupt.raw");fs_unlink(L"test-image.rgba");puts("All native tests passed.");return 0;
}
