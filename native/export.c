/* Native C exporters. CAD output uses millimetres, Z up, and a grounded base.
 * STEP AP214 uses planar FACE_SURFACEs / POLY_LOOPs in FACETED_BREPs.
 * There is no CAD library or Python conversion in the application path.
 */
#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../vendor/cjson/cJSON.h"

typedef struct { double scale, center_x, center_z, floor; } CadFrame;
static int stopped(FsCancel * cancel,wchar_t *error){
    if(cancel&&fs_cancel_wait(cancel,0)==WAIT_OBJECT_0){fs_error(error,L"생성을 취소했습니다.");return 1;}return 0;
}
static int frame(const Mesh *m,double mm,CadFrame *out,wchar_t *error){
    if(!mesh_validate(m,error))return 0;
    if(!isfinite(mm)||mm<1||mm>10000){fs_error(error,L"인쇄 파일 크기를 1~10,000 mm 사이로 입력하세요.");return 0;}
    float lo[3],hi[3];mesh_bounds(m,lo,hi);double span=0;
    for(int k=0;k<3;k++)span=fmax(span,(double)hi[k]-lo[k]);
    if(span<1e-12){fs_error(error,L"크기가 없는 모델은 내보낼 수 없습니다.");return 0;}
    *out=(CadFrame){mm/span,((double)lo[0]+hi[0])/2,((double)lo[2]+hi[2])/2,lo[1]};return 1;
}
static void point(const Vertex *v,const CadFrame *f,double p[3]){
    p[0]=(v->p[0]-f->center_x)*f->scale;
    p[1]=-(v->p[2]-f->center_z)*f->scale;
    p[2]=(v->p[1]-f->floor)*f->scale;
}
static int basis(const double a[3],const double b[3],const double c[3],double n[3],double u[3]){
    double v[3],length=0;for(int k=0;k<3;k++){u[k]=b[k]-a[k];v[k]=c[k]-a[k];length+=u[k]*u[k];}
    n[0]=u[1]*v[2]-u[2]*v[1];n[1]=u[2]*v[0]-u[0]*v[2];n[2]=u[0]*v[1]-u[1]*v[0];
    double area=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);length=sqrt(length);
    if(area<1e-16||length<1e-12)return 0;
    for(int k=0;k<3;k++){n[k]/=area;u[k]/=length;}return 1;
}
int mesh_write_stl(const wchar_t *path,const Mesh *m,double mm,FsCancel * cancel,wchar_t *error){
    CadFrame f;if(!frame(m,mm,&f,error)||stopped(cancel,error))return 0;
    FILE *out=fs_wfopen(path,L"wb");if(!out){fs_error(error,L"STL 파일을 열 수 없습니다.");return 0;}
    char header[80]={0};snprintf(header,sizeof(header),"AAAnimal Studio | millimetres | Z up | longest side %.6g mm",mm);
    int ok=fwrite(header,1,80,out)==80&&fwrite(&m->triangle_count,4,1,out)==1;
    for(uint32_t i=0;ok&&i<m->triangle_count;i++){
        if(!(i%1024)&&stopped(cancel,error)){ok=0;break;}
        double p[3][3],n[3],u[3];for(int k=0;k<3;k++)point(&m->vertices[m->triangles[i].v[k]],&f,p[k]);
        if(!basis(p[0],p[1],p[2],n,u)){fs_error(error,L"면적이 없는 삼각형 때문에 STL을 저장할 수 없습니다.");ok=0;break;}
        float record[12];for(int k=0;k<3;k++)record[k]=(float)n[k];
        for(int j=0;j<3;j++)for(int k=0;k<3;k++)record[3+j*3+k]=(float)p[j][k];
        uint16_t attributes=0;ok=fwrite(record,4,12,out)==12&&fwrite(&attributes,2,1,out)==1;
    }
    if(fclose(out))ok=0;
    if(!ok){fs_unlink(path);if(!error[0])fs_error(error,L"STL 저장에 실패했습니다. 저장 공간을 확인하세요.");}return ok;
}

typedef struct {float p[3];uint32_t index;} Position;
typedef struct {uint32_t a,b,face;int direction;} Edge;
static int compare_position(const void *a,const void *b){
    const Position *x=a,*y=b;for(int k=0;k<3;k++)if(x->p[k]!=y->p[k])return x->p[k]<y->p[k]?-1:1;return 0;
}
static int compare_edge(const void *a,const void *b){
    const Edge *x=a,*y=b;if(x->a!=y->a)return x->a<y->a?-1:1;if(x->b!=y->b)return x->b<y->b?-1:1;return 0;
}
static uint32_t root(uint32_t *parent,uint32_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;}

int mesh_write_step(const wchar_t *path,const Mesh *m,double mm,FsCancel * cancel,wchar_t *error){
    CadFrame f;if(!frame(m,mm,&f,error)||stopped(cancel,error))return 0;
    uint32_t nv=m->vertex_count,nt=m->triangle_count,np=0,nc=0;
    Position *positions=malloc((size_t)nv*sizeof(*positions));
    uint32_t *mapping=malloc((size_t)nv*sizeof(*mapping)),*representative=malloc((size_t)nv*sizeof(*representative));
    uint32_t *parent=malloc((size_t)nt*sizeof(*parent)),*component=malloc((size_t)nt*sizeof(*component));
    uint32_t *head=malloc((size_t)nt*sizeof(*head)),*next=malloc((size_t)nt*sizeof(*next));
    double *volume=calloc(nt,sizeof(*volume));Edge *edges=malloc((size_t)nt*3*sizeof(*edges));
    FILE *out=NULL;int ok=0;
    if(!positions||!mapping||!representative||!parent||!component||!head||!next||!volume||!edges){fs_error(error,L"STEP 변환 메모리가 부족합니다.");goto done;}
    /* Weld exactly coincident positions, not colour/normal seams. */
    for(uint32_t i=0;i<nv;i++){memcpy(positions[i].p,m->vertices[i].p,12);positions[i].index=i;}
    qsort(positions,nv,sizeof(*positions),compare_position);
    for(uint32_t i=0;i<nv;i++){
        if(!i||compare_position(positions+i,positions+i-1)){representative[np++]=positions[i].index;}
        mapping[positions[i].index]=np-1;
    }
    for(uint32_t i=0;i<nt;i++){
        parent[i]=i;component[i]=UINT32_MAX;head[i]=UINT32_MAX;
        for(int k=0;k<3;k++){
            uint32_t a=mapping[m->triangles[i].v[k]],b=mapping[m->triangles[i].v[(k+1)%3]];
            if(a==b)goto topology;
            edges[i*3+k]=(Edge){a<b?a:b,a<b?b:a,i,a<b?1:-1};
        }
    }
    qsort(edges,(size_t)nt*3,sizeof(*edges),compare_edge);
    for(uint32_t i=0;i<nt*3;i+=2){
        if(i+1>=nt*3||compare_edge(edges+i,edges+i+1)||edges[i].direction==edges[i+1].direction||
            (i+2<nt*3&&!compare_edge(edges+i,edges+i+2)))goto topology;
        uint32_t a=root(parent,edges[i].face),b=root(parent,edges[i+1].face);if(a!=b)parent[b]=a;
    }
    for(uint32_t i=0;i<nt;i++){
        uint32_t r=root(parent,i);if(component[r]==UINT32_MAX)component[r]=nc++;
        uint32_t c=component[r];next[i]=head[c];head[c]=i;
        double p[3][3],n[3],u[3];for(int k=0;k<3;k++)point(&m->vertices[m->triangles[i].v[k]],&f,p[k]);
        if(!basis(p[0],p[1],p[2],n,u))goto topology;
        volume[c]+=p[0][0]*(p[1][1]*p[2][2]-p[1][2]*p[2][1])+p[0][1]*(p[1][2]*p[2][0]-p[1][0]*p[2][2])+p[0][2]*(p[1][0]*p[2][1]-p[1][1]*p[2][0]);
    }
    for(uint32_t c=0;c<nc;c++)if(volume[c]<=1e-12)goto topology;
    if(stopped(cancel,error))goto done;
    out=fs_wfopen(path,L"wb");if(!out){fs_error(error,L"STEP 파일을 열 수 없습니다.");goto done;}
    FsTime now;fs_utc_time(&now);
    fprintf(out,"ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('AAAnimal Studio planar triangle solids'),'2;1');\n"
        "FILE_NAME('model.step','%04u-%02u-%02uT%02u:%02u:%02u',(''),(''),'AAAnimal Studio','AAAnimal Studio','');\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\nENDSEC;\nDATA;\n",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
    fputs("#1=APPLICATION_CONTEXT('core data for automotive mechanical design processes');\n"
        "#2=APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000,#1);\n"
        "#3=PRODUCT_CONTEXT('',#1,'mechanical');\n#4=PRODUCT('AAAnimal','AAAnimal','Faceted model',(#3));\n"
        "#5=PRODUCT_DEFINITION_FORMATION('','',#4);\n#6=PRODUCT_DEFINITION_CONTEXT('part definition',#1,'design');\n"
        "#7=PRODUCT_DEFINITION('design','',#5,#6);\n#8=PRODUCT_DEFINITION_SHAPE('','',#7);\n"
        "#9=(LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.));\n"
        "#10=(NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.));\n"
        "#11=(NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT());\n"
        "#12=UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-7),#9,'distance_accuracy_value','');\n"
        "#13=(GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((#12)) GLOBAL_UNIT_ASSIGNED_CONTEXT((#9,#10,#11)) REPRESENTATION_CONTEXT('','3D'));\n",out);
    const uint32_t points=20,faces=points+np,shells=faces+nt*7,representation=shells+nc*2;
    for(uint32_t i=0;i<np;i++){
        double p[3];point(&m->vertices[representative[i]],&f,p);
        fprintf(out,"#%u=CARTESIAN_POINT('',(%.15E,%.15E,%.15E));\n",points+i,p[0],p[1],p[2]);
    }
    for(uint32_t i=0;i<nt;i++){
        if(!(i%512)&&stopped(cancel,error))goto done;
        uint32_t base=faces+i*7,ids[3];double p[3][3],n[3],u[3];
        for(int k=0;k<3;k++){ids[k]=points+mapping[m->triangles[i].v[k]];point(&m->vertices[m->triangles[i].v[k]],&f,p[k]);}
        basis(p[0],p[1],p[2],n,u);
        fprintf(out,"#%u=POLY_LOOP('',(#%u,#%u,#%u));\n#%u=FACE_OUTER_BOUND('',#%u,.T.);\n",base,ids[0],ids[1],ids[2],base+1,base);
        fprintf(out,"#%u=DIRECTION('',(%.15E,%.15E,%.15E));\n#%u=DIRECTION('',(%.15E,%.15E,%.15E));\n",base+2,n[0],n[1],n[2],base+3,u[0],u[1],u[2]);
        fprintf(out,"#%u=AXIS2_PLACEMENT_3D('',#%u,#%u,#%u);\n#%u=PLANE('',#%u);\n#%u=FACE_SURFACE('',(#%u),#%u,.T.);\n",base+4,ids[0],base+2,base+3,base+5,base+4,base+6,base+1,base+5);
    }
    for(uint32_t c=0;c<nc;c++){
        fprintf(out,"#%u=CLOSED_SHELL('',(",shells+c*2);int first=1;
        for(uint32_t i=head[c];i!=UINT32_MAX;i=next[i]){fprintf(out,"%s#%u",first?"":",",faces+i*7+6);first=0;}
        fprintf(out,"));\n#%u=FACETED_BREP('AAAnimal solid %u',#%u);\n",shells+c*2+1,c+1,shells+c*2);
    }
    fprintf(out,"#%u=FACETED_BREP_SHAPE_REPRESENTATION('AAAnimal',(",representation);
    for(uint32_t c=0;c<nc;c++)fprintf(out,"%s#%u",c?",":"",shells+c*2+1);
    fprintf(out,"),#13);\n#%u=SHAPE_DEFINITION_REPRESENTATION(#8,#%u);\nENDSEC;\nEND-ISO-10303-21;\n",representation+1,representation);
    ok=!ferror(out);goto done;
topology:fs_error(error,L"STEP에는 닫힌 면과 일관된 바깥 방향이 필요합니다. 이 모델은 STL 또는 GLB로 저장하세요.");
done:
    if(out){if(fclose(out))ok=0;if(!ok)fs_unlink(path);}
    free(positions);free(mapping);free(representative);free(parent);free(component);free(head);free(next);free(volume);free(edges);
    if(!ok&&!error[0])fs_error(error,L"STEP 저장에 실패했습니다. 저장 공간을 확인하세요.");return ok;
}

typedef struct {const char *name,*text;FILE *file;uint32_t length,crc,offset;} ZipPart;
static void zip16(FILE *f,uint16_t v){unsigned char b[2]={(unsigned char)v,(unsigned char)(v>>8)};fwrite(b,1,2,f);}
static void zip32(FILE *f,uint32_t v){zip16(f,(uint16_t)v);zip16(f,(uint16_t)(v>>16));}
static uint32_t crc_update(uint32_t crc,const unsigned char *p,size_t n){
    uint32_t table[256];for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c>>1)^(0xedb88320u&-(int32_t)(c&1));table[i]=c;}
    for(size_t i=0;i<n;i++)crc=table[(crc^p[i])&255]^(crc>>8);return crc;
}
int mesh_write_3mf(const wchar_t *path,const Mesh *m,double mm,FsCancel * cancel,wchar_t *error){
    CadFrame f;FILE *xml=NULL,*out=NULL;Position *positions=NULL;uint32_t *mapping=NULL,*representative=NULL,np=0;int ok=0;
    if(!frame(m,mm,&f,error)||stopped(cancel,error))return 0;
    positions=malloc((size_t)m->vertex_count*sizeof(*positions));mapping=malloc((size_t)m->vertex_count*4);representative=malloc((size_t)m->vertex_count*4);
    if(!positions||!mapping||!representative||tmpfile_s(&xml)||!xml){fs_error(error,L"3MF 저장 준비에 실패했습니다.");goto done;}
    /* Weld geometry seams while preserving independent triangle-corner colour
     * indices. Surface colour is optional; core-only readers get a base colour. */
    for(uint32_t i=0;i<m->vertex_count;i++){memcpy(positions[i].p,m->vertices[i].p,12);positions[i].index=i;}
    qsort(positions,m->vertex_count,sizeof(*positions),compare_position);
    for(uint32_t i=0;i<m->vertex_count;i++){if(!i||compare_position(positions+i,positions+i-1))representative[np++]=positions[i].index;mapping[positions[i].index]=np-1;}
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<model unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" xmlns:m=\"http://schemas.microsoft.com/3dmanufacturing/material/2015/02\">\n<metadata name=\"Application\">AAAnimal Studio</metadata>\n<resources><basematerials id=\"1\"><base name=\"Model\" displaycolor=\"#B98758FF\"/></basematerials><m:colorgroup id=\"2\">\n",xml);
    for(uint32_t i=0;i<m->vertex_count;i++){if(!(i%1024)&&stopped(cancel,error))goto done;const unsigned char *c=m->vertices[i].color;fprintf(xml,"<m:color color=\"#%02X%02X%02XFF\"/>\n",c[0],c[1],c[2]);}
    fputs("</m:colorgroup><object id=\"3\" type=\"model\" name=\"AAAnimal Studio model\" pid=\"1\" pindex=\"0\"><mesh><vertices>\n",xml);
    for(uint32_t i=0;i<np;i++){double p[3];point(&m->vertices[representative[i]],&f,p);fprintf(xml,"<vertex x=\"%.15g\" y=\"%.15g\" z=\"%.15g\"/>\n",p[0],p[1],p[2]);}
    fputs("</vertices><triangles>\n",xml);
    for(uint32_t i=0;i<m->triangle_count;i++){if(!(i%512)&&stopped(cancel,error))goto done;const Triangle *t=&m->triangles[i];
        fprintf(xml,"<triangle v1=\"%u\" v2=\"%u\" v3=\"%u\" pid=\"2\" p1=\"%u\" p2=\"%u\" p3=\"%u\"/>\n",mapping[t->v[0]],mapping[t->v[1]],mapping[t->v[2]],t->v[0],t->v[1],t->v[2]);}
    fputs("</triangles></mesh></object></resources><build><item objectid=\"3\"/></build></model>\n",xml);
    if(fflush(xml)||ferror(xml))goto done;
    int64_t xml_length=ftello(xml);if(xml_length<1||xml_length>0x7fffffffu){fs_error(error,L"3MF 파일이 처리 한도를 초과했습니다.");goto done;}
    ZipPart parts[3]={
        {"[Content_Types].xml","<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>",NULL,0,0,0},
        {"_rels/.rels","<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>",NULL,0,0,0},
        {"3D/3dmodel.model",NULL,xml,(uint32_t)xml_length,0,0}};
    unsigned char buffer[65536];for(int i=0;i<3;i++){uint32_t crc=~0u;
        if(parts[i].text){parts[i].length=(uint32_t)strlen(parts[i].text);crc=crc_update(crc,(const unsigned char*)parts[i].text,parts[i].length);}
        else{rewind(xml);size_t n;while((n=fread(buffer,1,sizeof(buffer),xml))>0){if(stopped(cancel,error))goto done;crc=crc_update(crc,buffer,n);}if(ferror(xml))goto done;}
        parts[i].crc=~crc;
    }
    out=fs_wfopen(path,L"wb");if(!out)goto done;
    for(int i=0;i<3;i++){ZipPart *p=&parts[i];p->offset=(uint32_t)ftello(out);uint16_t name_length=(uint16_t)strlen(p->name);
        zip32(out,0x04034b50);zip16(out,20);zip16(out,0);zip16(out,0);zip16(out,0);zip16(out,33);zip32(out,p->crc);zip32(out,p->length);zip32(out,p->length);zip16(out,name_length);zip16(out,0);fwrite(p->name,1,name_length,out);
        if(p->text)fwrite(p->text,1,p->length,out);else{rewind(xml);size_t n;while((n=fread(buffer,1,sizeof(buffer),xml))>0){if(stopped(cancel,error))goto done;fwrite(buffer,1,n,out);}if(ferror(xml))goto done;}
    }
    uint32_t central=(uint32_t)ftello(out);
    for(int i=0;i<3;i++){ZipPart *p=&parts[i];uint16_t n=(uint16_t)strlen(p->name);zip32(out,0x02014b50);zip16(out,20);zip16(out,20);zip16(out,0);zip16(out,0);zip16(out,0);zip16(out,33);zip32(out,p->crc);zip32(out,p->length);zip32(out,p->length);zip16(out,n);zip16(out,0);zip16(out,0);zip16(out,0);zip16(out,0);zip32(out,0);zip32(out,p->offset);fwrite(p->name,1,n,out);}
    uint32_t central_length=(uint32_t)ftello(out)-central;zip32(out,0x06054b50);zip16(out,0);zip16(out,0);zip16(out,3);zip16(out,3);zip32(out,central_length);zip32(out,central);zip16(out,0);ok=!ferror(out)&&!stopped(cancel,error);
done:if(xml)fclose(xml);if(out){if(fclose(out))ok=0;if(!ok)fs_unlink(path);}free(positions);free(mapping);free(representative);if(!ok&&!error[0])fs_error(error,L"3MF 저장에 실패했습니다.");return ok;
}
int mesh_export(const wchar_t *directory,const Mesh *m,unsigned formats,double mm,const char *extras,FsCancel * cancel,wchar_t *error){
    if(!formats||(formats&~EXPORT_ALL)){fs_error(error,L"저장할 형식을 하나 이상 선택하세요.");return 0;}
    const wchar_t *names[]={L"model.glb",L"model.stl",L"model.step",L"model.3mf"};wchar_t path[FS_PATH];
    for(int i=0;i<4;i++)if(formats&(1u<<i)){
        if(!fs_join(path,directory,names[i])||fs_attributes(path)!=INVALID_FILE_ATTRIBUTES){fs_error(error,L"저장 파일이 이미 있거나 경로가 너무 깁니다.");return 0;}
    }
    for(int i=0;i<4;i++)if(formats&(1u<<i)){
        if(stopped(cancel,error))return 0;fs_join(path,directory,names[i]);
        int ok=i==0?mesh_write_glb(path,m,extras,error):(i==1?mesh_write_stl(path,m,mm,cancel,error):(i==2?mesh_write_step(path,m,mm,cancel,error):mesh_write_3mf(path,m,mm,cancel,error)));
        if(!ok)return 0;
    }
    return !stopped(cancel,error);
}

/* Export an existing in-memory model. No photo, AI environment, network,
 * simplifier or style processor is consulted on this path. */
int fs_save_model(Job *job,const Mesh *mesh){
    wchar_t parent[FS_PATH],directory[FS_PATH],path[FS_PATH];FsId id;
    cJSON *report=NULL;char *metadata=NULL;int ok=0;job->error[0]=0;job->result[0]=0;
    if(!job->formats||(job->formats&~EXPORT_ALL)){fs_error(job->error,L"저장할 형식을 하나 이상 선택하세요.");return 0;}
    if(!mesh_validate(mesh,job->error)||stopped(job->cancel,job->error))return 0;
    if(job->formats&(EXPORT_STL|EXPORT_STEP|EXPORT_3MF)){CadFrame transform;if(!frame(mesh,job->cad_size_mm,&transform,job->error))return 0;}
    if(!job->output[0]&&!fs_default_output(job->output)){fs_error(job->error,L"저장 위치를 정할 수 없습니다.");return 0;}
    if(fs_exists(job->output)){fs_error(job->error,L"결과 폴더가 이미 있습니다. 새 폴더 이름을 사용하세요.");return 0;}
    fs_wcopy(parent,FS_PATH,job->output);fs_parent(parent);
    int created=fs_mkdirs(NULL,parent,NULL);
    if(created!=ERROR_SUCCESS&&created!=ERROR_ALREADY_EXISTS&&created!=ERROR_FILE_EXISTS){fs_error(job->error,L"저장 위치를 만들 수 없습니다.");return 0;}
    if(FS_FAILED(fs_create_id(&id))||fs_wformat(directory,FS_PATH,L"%s.partial-%08lx",job->output,id.value)<0||!fs_mkdir(directory,NULL)){
        fs_error(job->error,L"저장 작업 폴더를 만들 수 없습니다.");return 0;
    }
    fs_status(job,L"현재 모델을 선택한 형식으로 저장하는 중");
    report=cJSON_CreateObject();
    cJSON_AddStringToObject(report,"application","AAAnimal Studio");
    cJSON_AddStringToObject(report,"operation","export_existing");
    cJSON_AddBoolToObject(report,"regenerated",0);
    cJSON_AddStringToObject(report,"geometryProcessing","existing in-memory mesh; no inference, simplification or restyling");
    cJSON_AddNumberToObject(report,"resultTriangles",mesh->triangle_count);
    cJSON_AddNumberToObject(report,"sourceVertices",mesh->vertex_count);
    cJSON *formats=cJSON_AddArrayToObject(report,"exportFormats");
    if(job->formats&EXPORT_GLB)cJSON_AddItemToArray(formats,cJSON_CreateString("glb"));
    if(job->formats&EXPORT_STL)cJSON_AddItemToArray(formats,cJSON_CreateString("stl"));
    if(job->formats&EXPORT_STEP)cJSON_AddItemToArray(formats,cJSON_CreateString("step"));
    if(job->formats&EXPORT_3MF)cJSON_AddItemToArray(formats,cJSON_CreateString("3mf"));
    if(job->formats&(EXPORT_STL|EXPORT_STEP|EXPORT_3MF)){
        cJSON_AddNumberToObject(report,"cadLongestSideMm",job->cad_size_mm);
        cJSON_AddStringToObject(report,"cadCoordinates","millimetres; Z up; center X/Y and place lowest point on Z=0; STL readers must use mm");
        cJSON_AddStringToObject(report,"cadAppearance","STL/STEP: geometry only, STEP planar faceted solids; 3MF: surface corner colours, slicer dependent");
    }
    metadata=cJSON_PrintUnformatted(report);if(!metadata){fs_error(job->error,L"저장 기록 메모리가 부족합니다.");goto done;}
    if(!mesh_export(directory,mesh,job->formats,job->cad_size_mm,metadata,job->cancel,job->error))goto done;
    fs_join(path,directory,L"report.json");FILE *out=fs_wfopen(path,L"wb");if(!out){fs_error(job->error,L"저장 기록 파일을 열 수 없습니다.");goto done;}
    size_t length=strlen(metadata);int written=fwrite(metadata,1,length,out)==length;if(fclose(out))written=0;
    if(!written){fs_error(job->error,L"저장 기록 쓰기에 실패했습니다.");goto done;}
    if(stopped(job->cancel,job->error))goto done;
    if(!fs_move(directory,job->output,MOVEFILE_WRITE_THROUGH)){fs_error(job->error,L"저장 폴더를 확정하지 못했습니다.");goto done;}
    fs_join(job->result,job->output,(job->formats&EXPORT_GLB)?L"model.glb":((job->formats&EXPORT_STL)?L"model.stl":((job->formats&EXPORT_STEP)?L"model.step":L"model.3mf")));
    job->final_triangles=mesh->triangle_count;ok=1;fs_status(job,L"현재 모델 저장 완료");
done:
    free(metadata);cJSON_Delete(report);return ok;
}
