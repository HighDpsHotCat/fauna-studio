#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#define CGLTF_IMPLEMENTATION
#include "../vendor/cgltf/cgltf.h"

void mesh_free(Mesh *m){free(m->vertices);free(m->triangles);memset(m,0,sizeof(*m));}
int mesh_copy(const Mesh *s,Mesh *d){
    memset(d,0,sizeof(*d));
    d->vertices=malloc((size_t)s->vertex_count*sizeof(Vertex));
    d->triangles=malloc((size_t)s->triangle_count*sizeof(Triangle));
    if(!d->vertices||!d->triangles){mesh_free(d);return 0;}
    d->vertex_count=s->vertex_count;d->triangle_count=s->triangle_count;
    memcpy(d->vertices,s->vertices,(size_t)s->vertex_count*sizeof(Vertex));
    memcpy(d->triangles,s->triangles,(size_t)s->triangle_count*sizeof(Triangle));return 1;
}
void mesh_bounds(const Mesh *m,float lo[3],float hi[3]){
    for(int k=0;k<3;k++){lo[k]=FLT_MAX;hi[k]=-FLT_MAX;}
    for(uint32_t i=0;i<m->vertex_count;i++)for(int k=0;k<3;k++){
        float v=m->vertices[i].p[k];if(v<lo[k])lo[k]=v;if(v>hi[k])hi[k]=v;
    }
}
int mesh_validate(const Mesh *m,wchar_t *error){
    if(!m->vertices||!m->triangles||!m->vertex_count||!m->triangle_count||m->vertex_count>4000000||m->triangle_count>8000000)goto bad;
    for(uint32_t i=0;i<m->vertex_count;i++)for(int k=0;k<3;k++)if(!isfinite(m->vertices[i].p[k])||fabsf(m->vertices[i].p[k])>1e6f)goto bad;
    for(uint32_t i=0;i<m->triangle_count;i++){
        const uint32_t *v=m->triangles[i].v;
        if(v[0]>=m->vertex_count||v[1]>=m->vertex_count||v[2]>=m->vertex_count||v[0]==v[1]||v[1]==v[2]||v[0]==v[2])goto bad;
    }
    return 1;
bad:fs_error(error,L"유효하지 않거나 너무 큰 3D 메시입니다.");return 0;
}
void mesh_normals(Mesh *m){
    for(uint32_t i=0;i<m->vertex_count;i++)memset(m->vertices[i].n,0,3*sizeof(float));
    for(uint32_t i=0;i<m->triangle_count;i++){
        float a[3],b[3],n[3];const Triangle *t=&m->triangles[i];
        for(int k=0;k<3;k++){a[k]=m->vertices[t->v[1]].p[k]-m->vertices[t->v[0]].p[k];b[k]=m->vertices[t->v[2]].p[k]-m->vertices[t->v[0]].p[k];}
        n[0]=a[1]*b[2]-a[2]*b[1];n[1]=a[2]*b[0]-a[0]*b[2];n[2]=a[0]*b[1]-a[1]*b[0];
        for(int j=0;j<3;j++)for(int k=0;k<3;k++)m->vertices[t->v[j]].n[k]+=n[k];
    }
    for(uint32_t i=0;i<m->vertex_count;i++){
        float *n=m->vertices[i].n;float length=sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if(length>1e-20f)for(int k=0;k<3;k++)n[k]/=length;
    }
}
int mesh_read_raw(const wchar_t *path,Mesh *m,wchar_t *error){
    FILE *f=fs_wfopen(path,L"rb");uint32_t h[3];memset(m,0,sizeof(*m));
    if(!f)goto bad;
    if(fread(h,4,3,f)!=3||h[0]!=0x31534d46||!h[1]||!h[2]||h[1]>4000000||h[2]>8000000)goto bad;
    m->vertex_count=h[1];m->triangle_count=h[2];
    m->vertices=calloc(h[1],sizeof(Vertex));m->triangles=malloc((size_t)h[2]*sizeof(Triangle));
    if(!m->vertices||!m->triangles)goto bad;
    for(uint32_t i=0;i<h[1];i++){
        float p[3];if(fread(p,4,3,f)!=3)goto bad;
        /* Match the official TripoSR viewer orientation, using C. */
        m->vertices[i].p[0]=-p[1];m->vertices[i].p[1]=p[2];m->vertices[i].p[2]=-p[0];
    }
    for(uint32_t i=0;i<h[1];i++)if(fread(m->vertices[i].color,1,4,f)!=4)goto bad;
    if(fread(m->triangles,sizeof(Triangle),h[2],f)!=h[2]||fgetc(f)!=EOF)goto bad;
    /* Marching cubes can emit a collapsed face after vertex welding. It has
     * no surface area; discard it without weakening index/position checks. */
    uint32_t kept=0;
    for(uint32_t i=0;i<m->triangle_count;i++){
        Triangle t=m->triangles[i];
        if(t.v[0]>=m->vertex_count||t.v[1]>=m->vertex_count||t.v[2]>=m->vertex_count)goto bad;
        if(t.v[0]==t.v[1]||t.v[1]==t.v[2]||t.v[0]==t.v[2])continue;
        m->triangles[kept++]=t;
    }
    m->triangle_count=kept;
    fclose(f);if(!mesh_validate(m,error)){mesh_free(m);return 0;}
    mesh_normals(m);return 1;
bad:if(f)fclose(f);mesh_free(m);fs_error(error,L"TripoSR 메시 결과를 읽을 수 없습니다.");return 0;
}

int mesh_read_glb(const wchar_t *path,Mesh *m,wchar_t *error){
    FILE *f=fs_wfopen(path,L"rb");void *bytes=NULL;cgltf_data *data=NULL;cgltf_options opts={0};
    int ok=0,all_normals=1;memset(m,0,sizeof(*m));if(!f)goto done;
    if(fseeko(f,0,SEEK_END))goto done;
    int64_t size=ftello(f);if(size<20||size>512*1024*1024)goto done;
    rewind(f);bytes=malloc((size_t)size);if(!bytes||fread(bytes,1,(size_t)size,f)!=(size_t)size)goto done;
    if(cgltf_parse(&opts,bytes,(size_t)size,&data)!=cgltf_result_success)goto done;
    /* Only self-contained GLB: do not resolve external paths or URLs. */
    if(data->file_type!=cgltf_file_type_glb||data->buffers_count!=1||data->buffers[0].uri||!data->bin||data->buffers[0].size>data->bin_size)goto done;
    data->buffers[0].data=(void*)data->bin;
    if(cgltf_validate(data)!=cgltf_result_success)goto done;
    uint64_t nv=0,nt=0;
    for(cgltf_size i=0;i<data->nodes_count;i++)if(data->nodes[i].mesh){
        cgltf_mesh *msh=data->nodes[i].mesh;
        for(cgltf_size j=0;j<msh->primitives_count;j++){
            cgltf_primitive *p=&msh->primitives[j];
            const cgltf_accessor *pos=cgltf_find_accessor(p,cgltf_attribute_type_position,0);
            if(!pos||p->type!=cgltf_primitive_type_triangles||p->has_draco_mesh_compression||pos->type!=cgltf_type_vec3)goto done;
            if(p->material&&p->material->pbr_metallic_roughness.base_color_texture.texture){
                fs_error(error,L"이 C 뷰어는 정점 색상 GLB를 지원합니다. 텍스처 GLB는 별도 뷰어로 열어 주세요.");goto done;
            }
            cgltf_size count=p->indices?p->indices->count:pos->count;if(count%3)goto done;
            nv+=pos->count;nt+=count/3;
        }
    }
    if(!nv||!nt||nv>4000000||nt>8000000)goto done;
    m->vertices=calloc((size_t)nv,sizeof(Vertex));m->triangles=malloc((size_t)nt*sizeof(Triangle));if(!m->vertices||!m->triangles)goto done;
    for(cgltf_size i=0;i<data->nodes_count;i++)if(data->nodes[i].mesh){
        float transform[16];cgltf_node_transform_world(&data->nodes[i],transform);
        cgltf_mesh *msh=data->nodes[i].mesh;
        for(cgltf_size j=0;j<msh->primitives_count;j++){
            cgltf_primitive *p=&msh->primitives[j];uint32_t base=m->vertex_count;
            const cgltf_accessor *pos=cgltf_find_accessor(p,cgltf_attribute_type_position,0);
            const cgltf_accessor *col=cgltf_find_accessor(p,cgltf_attribute_type_color,0);
            const cgltf_accessor *normal=cgltf_find_accessor(p,cgltf_attribute_type_normal,0);if(!normal)all_normals=0;
            for(cgltf_size v=0;v<pos->count;v++){
                float point[3],color[4]={1,1,1,1};Vertex *d=&m->vertices[m->vertex_count++];
                if(!cgltf_accessor_read_float(pos,v,point,3))goto done;
                for(int k=0;k<3;k++)d->p[k]=transform[k]*point[0]+transform[4+k]*point[1]+transform[8+k]*point[2]+transform[12+k];
                if(normal){float n[3];if(!cgltf_accessor_read_float(normal,v,n,3))goto done;
                    float a=transform[0],b=transform[4],c=transform[8],e=transform[1],g=transform[5],h=transform[9],m20=transform[2],m21=transform[6],m22=transform[10];
                    float determinant=a*(g*m22-h*m21)-b*(e*m22-h*m20)+c*(e*m21-g*m20);if(fabsf(determinant)<1e-15f)goto done;
                    d->n[0]=((g*m22-h*m21)*n[0]+(h*m20-e*m22)*n[1]+(e*m21-g*m20)*n[2])/determinant;
                    d->n[1]=((c*m21-b*m22)*n[0]+(a*m22-c*m20)*n[1]+(b*m20-a*m21)*n[2])/determinant;
                    d->n[2]=((b*h-c*g)*n[0]+(c*e-a*h)*n[1]+(a*g-b*e)*n[2])/determinant;
                    float len=sqrtf(d->n[0]*d->n[0]+d->n[1]*d->n[1]+d->n[2]*d->n[2]);if(!isfinite(len)||len<1e-10f)goto done;for(int q=0;q<3;q++)d->n[q]/=len;
                }
                if(col&&!cgltf_accessor_read_float(col,v,color,4))goto done;
                for(int k=0;k<4;k++){
                    float factor=p->material?p->material->pbr_metallic_roughness.base_color_factor[k]:1;
                    float linear=fminf(1,fmaxf(0,color[k]*factor));
                    float srgb=k==3?linear:(linear<=0.0031308f?12.92f*linear:1.055f*powf(linear,1/2.4f)-0.055f);
                    d->color[k]=(unsigned char)(srgb*255+0.5f);
                }
            }
            cgltf_size count=p->indices?p->indices->count:pos->count;
            for(cgltf_size t=0;t<count/3;t++){
                Triangle *d=&m->triangles[m->triangle_count++];
                for(int k=0;k<3;k++){
                    cgltf_size index=p->indices?cgltf_accessor_read_index(p->indices,t*3+k):t*3+k;
                    if(index>=pos->count)goto done;d->v[k]=base+(uint32_t)index;
                }
            }
        }
    }
    ok=mesh_validate(m,error);if(ok&&!all_normals)mesh_normals(m);
done:
    if(f)fclose(f);if(data){if(data->buffers_count)data->buffers[0].data=NULL;cgltf_free(data);}free(bytes);
    if(!ok){mesh_free(m);if(!error[0])fs_error(error,L"지원하지 않거나 손상된 GLB 파일입니다.");}return ok;
}

int mesh_write_glb(const wchar_t *path,const Mesh *m,const char *extras,wchar_t *error){
    if(!mesh_validate(m,error))return 0;
    char json[8192];float lo[3],hi[3];mesh_bounds(m,lo,hi);
    uint32_t pos=m->vertex_count*12,norm=pos,color=m->vertex_count*16,idx=m->triangle_count*12;
    uint32_t bin=pos+norm+color+idx;
    int length=snprintf(json,sizeof(json),
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"AAAnimal Studio / TripoSR\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0],\"extras\":%s}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"COLOR_0\":2},\"indices\":3,\"material\":0}]}],"
        "\"materials\":[{\"doubleSided\":true,\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0,\"roughnessFactor\":0.9}}],"
        "\"buffers\":[{\"byteLength\":%u}],\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":%u,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}]}",
        extras?extras:"{}",bin,pos,pos,norm,pos+norm,color,pos+norm+color,idx,
        m->vertex_count,lo[0],lo[1],lo[2],hi[0],hi[1],hi[2],m->vertex_count,m->vertex_count,m->triangle_count*3);
    if(length<0||length>(int)sizeof(json)-4){fs_error(error,L"GLB 메타데이터가 너무 큽니다.");return 0;}
    while(length%4)json[length++]=' ';
    uint32_t header[5]={0x46546c67,2,12+8+(uint32_t)length+8+bin,(uint32_t)length,0x4e4f534a};
    FILE *f=fs_wfopen(path,L"wb");if(!f){fs_error(error,L"GLB 파일을 저장할 수 없습니다.");return 0;}
    int ok=fwrite(header,4,5,f)==5 && fwrite(json,1,length,f)==(size_t)length;
    uint32_t chunk[2]={bin,0x004e4942};ok=ok&&fwrite(chunk,4,2,f)==2;
    for(uint32_t i=0;ok&&i<m->vertex_count;i++)ok=fwrite(m->vertices[i].p,4,3,f)==3;
    for(uint32_t i=0;ok&&i<m->vertex_count;i++)ok=fwrite(m->vertices[i].n,4,3,f)==3;
    for(uint32_t i=0;ok&&i<m->vertex_count;i++){
        float c[4];for(int k=0;k<4;k++){float s=m->vertices[i].color[k]/255.f;c[k]=k==3?s:(s<=.04045f?s/12.92f:powf((s+.055f)/1.055f,2.4f));}
        ok=fwrite(c,4,4,f)==4;
    }
    ok=ok&&fwrite(m->triangles,sizeof(Triangle),m->triangle_count,f)==m->triangle_count;
    if(fclose(f))ok=0;if(!ok)fs_error(error,L"GLB 쓰기에 실패했습니다. 저장 공간을 확인하세요.");return ok;
}
