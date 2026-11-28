#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <signal.h>


static FsCancel *cancel_event;
static void interrupt(int event){(void)event;fs_cancel_set(cancel_event);}
static void progress(void *context,const wchar_t *message){(void)context;wprintf(L"%ls\n",message);fflush(stdout);}
static int absolute(const wchar_t *path,wchar_t *out){uint32_t n=fs_absolute(path,FS_PATH,out,NULL);return n>0&&n<FS_PATH;}
static unsigned formats_arg(const wchar_t *value){
    wchar_t copy[64],*state=NULL;if(wcslen(value)>=64)return 0;fs_wcopy(copy,64,value);unsigned formats=0;
    for(wchar_t *part=wcstok(copy,L",",&state);part;part=wcstok(NULL,L",",&state)){
        if(!wcscasecmp(part,L"glb"))formats|=EXPORT_GLB;else if(!wcscasecmp(part,L"stl"))formats|=EXPORT_STL;else if(!wcscasecmp(part,L"step"))formats|=EXPORT_STEP;else if(!wcscasecmp(part,L"3mf"))formats|=EXPORT_3MF;else return 0;
    }return formats;
}
static int run(int argc,wchar_t **argv){

    if((argc==4||argc==6)&&(!wcscmp(argv[1],L"--export-stl")||!wcscmp(argv[1],L"--export-step")||!wcscmp(argv[1],L"--export-3mf"))){
        double mm=100;wchar_t *end=NULL,error[FS_ERROR]={0};Mesh mesh={0};
        if(argc==6){if(wcscmp(argv[4],L"--size-mm"))goto usage;mm=wcstod(argv[5],&end);if(!end||*end||end==argv[5])goto usage;}
        if(fs_attributes(argv[3])!=INVALID_FILE_ATTRIBUTES){fwprintf(stderr,L"출력 파일이 이미 있습니다.\n");return 1;}
        int ok=mesh_read_glb(argv[2],&mesh,error);
        if(ok)ok=!wcscmp(argv[1],L"--export-stl")?mesh_write_stl(argv[3],&mesh,mm,NULL,error):(!wcscmp(argv[1],L"--export-step")?mesh_write_step(argv[3],&mesh,mm,NULL,error):mesh_write_3mf(argv[3],&mesh,mm,NULL,error));
        mesh_free(&mesh);if(!ok){fwprintf(stderr,L"%ls\n",error);return 1;}wprintf(L"Saved: %ls\n",argv[3]);return 0;
    }
    if(argc>=3&&!wcscmp(argv[1],L"--inspect")){
        Mesh m={0};wchar_t error[FS_ERROR]={0};if(!mesh_read_glb(argv[2],&m,error)){fwprintf(stderr,L"%ls\n",error);return 1;}
        wprintf(L"vertices=%u triangles=%u\n",m.vertex_count,m.triangle_count);mesh_free(&m);return 0;
    }
    if(argc==6&&(!wcscmp(argv[1],L"--simplify")||!wcscmp(argv[1],L"--lowpoly")||!wcscmp(argv[1],L"--surface"))){
        Mesh a={0},b={0};wchar_t error[FS_ERROR]={0};uint32_t target=(uint32_t)fs_wtoi(argv[4]);
        int faceted=!wcscmp(argv[1],L"--lowpoly"),kept=0;Quality q={0};
        if(wcscmp(argv[3],L"--target")||target<300||target>200000||!mesh_read_glb(argv[2],&a,error))goto failed;
        if(!wcscmp(argv[1],L"--surface")){if(!mesh_surface(&a,&b,target,NULL,&q,&kept,error))goto failed;}
        else if(faceted){if(!mesh_lowpoly(&a,&b,target,NULL,&q,&kept,error))goto failed;}
        else{if(!mesh_copy(&a,&b)||!mesh_simplify(&b,target,NULL,error))goto failed;q=mesh_compare(&a,&b);}
        wprintf(L"triangles=%u -> %u, minimum IoU=%.6f, depth RMSE=%.6f\n",a.triangle_count,b.triangle_count,q.silhouette_iou,q.depth_rmse);
        if(fs_attributes(argv[5])!=INVALID_FILE_ATTRIBUTES){fs_error(error,L"출력 파일이 이미 있습니다.");goto failed;}
        char metadata[512];snprintf(metadata,sizeof(metadata),"{\"tool\":\"C lowpoly processor\",\"style\":\"%s\",\"requestedTriangles\":%u,\"resultTriangles\":%u,\"minimumSilhouetteIoU\":%.9g,\"maximumDepthRMSE\":%.9g}",faceted?"faceted-c-v1":"smooth",target,b.triangle_count,q.silhouette_iou,q.depth_rmse);
        if(!mesh_write_glb(argv[5],&b,metadata,error))goto failed;
        mesh_free(&a);mesh_free(&b);return 0;
failed:fwprintf(stderr,L"%ls\n",error);mesh_free(&a);mesh_free(&b);return 1;
    }
    if(argc==10&&!wcscmp(argv[1],L"--form")){
        Job j={0};Mesh source={0},out={0};Photo photo={0};j.progress=progress;j.target=(uint32_t)fs_wtoi(argv[8]);
        if(wcscmp(argv[3],L"--context")||wcscmp(argv[5],L"--photo")||wcscmp(argv[7],L"--target"))goto usage;
        int ok=fs_attributes(argv[9])==INVALID_FILE_ATTRIBUTES&&mesh_read_glb(argv[2],&source,j.error)&&photo_read(argv[6],&photo,j.error)&&mesh_form_model(&j,argv[4],&photo,&source,&out);
        if(ok){char meta[512];snprintf(meta,sizeof(meta),"{\"style\":\"faceted-form-no-eyes-v2\",\"eyeRegionsSuppressed\":%d,\"uniformCoatFallback\":%s,\"requestedTriangles\":%u,\"resultTriangles\":%u}",j.eye_regions,j.eye_colour_fallback?"true":"false",j.target,out.triangle_count);ok=mesh_write_glb(argv[9],&out,meta,j.error);}
        if(ok)wprintf(L"triangles=%u -> %u, IoU=%.6f, RMSE=%.6f, eye regions=%d, colour fallback=%d\n",source.triangle_count,out.triangle_count,j.quality.silhouette_iou,j.quality.depth_rmse,j.eye_regions,j.eye_colour_fallback);
        else fwprintf(stderr,L"%ls\n",j.error[0]?j.error:L"출력 파일이 이미 있거나 입력이 잘못되었습니다.");
        mesh_free(&source);mesh_free(&out);photo_free(&photo);return ok?0:1;
    }
    Job job={0};job.target=900;job.resolution=320;job.progress=progress;job.formats=EXPORT_GLB;job.cad_size_mm=100;
    if(!fs_root(job.root)){fwprintf(stderr,L"프로젝트 루트를 찾지 못했습니다.\n");return 1;}
    for(int i=1;i<argc;i++){
        if(!wcscmp(argv[i],L"--generate"))continue;
        if(!wcscmp(argv[i],L"--no-shape")){job.shape_disabled=1;continue;}
        if(i+1>=argc)goto usage;
        if(!wcscmp(argv[i],L"--input")){if(!absolute(argv[++i],job.input))goto usage;}
        else if(!wcscmp(argv[i],L"--output")){if(!absolute(argv[++i],job.output))goto usage;}
        else if(!wcscmp(argv[i],L"--target"))job.target=(uint32_t)fs_wtoi(argv[++i]);
        else if(!wcscmp(argv[i],L"--formats")){job.formats=formats_arg(argv[++i]);if(!job.formats)goto usage;}
        else if(!wcscmp(argv[i],L"--size-mm")){wchar_t *end=NULL;job.cad_size_mm=wcstod(argv[++i],&end);if(!end||*end||end==argv[i])goto usage;}
        else if(!wcscmp(argv[i],L"--resolution"))job.resolution=(uint32_t)fs_wtoi(argv[++i]);else goto usage;
    }
    if(!job.input[0])goto usage;
    cancel_event=job.cancel=fs_cancel_create(NULL,1,0,NULL);if(!cancel_event)return 1;signal(SIGINT,interrupt);
    int ok=fs_generate(&job);mesh_free(&job.preview);signal(SIGINT,SIG_DFL);fs_cancel_free(cancel_event);
    if(!ok){fwprintf(stderr,L"%ls\n",job.error);return 1;}wprintf(L"Saved: %ls\n",job.result);return 0;
usage:
    wprintf(L"aaanimal-cli --form model.glb --context RESULT_DIRECTORY --photo cutout.rgba --target 900 NEW_OUTPUT.glb\n");
    wprintf(L"aaanimal-cli --generate --input photo.jpg [--output NEW_DIRECTORY] [--target 900] [--resolution 320] [--formats glb,stl,step,3mf] [--size-mm 100]\naaanimal-cli --export-stl model.glb NEW_OUTPUT.stl [--size-mm 100]\naaanimal-cli --export-step model.glb NEW_OUTPUT.step [--size-mm 100]\naaanimal-cli --export-3mf model.glb NEW_OUTPUT.3mf [--size-mm 100]\naaanimal-cli --inspect model.glb\naaanimal-cli --surface model.glb --target 20000 NEW_OUTPUT.glb\naaanimal-cli --lowpoly model.glb --target 1200 NEW_OUTPUT.glb\naaanimal-cli --simplify model.glb --target 2400 NEW_OUTPUT.glb\n");return 2;
}

int main(int argc,char **argv){fs_init();wchar_t **wide=calloc((size_t)argc,sizeof(*wide));if(!wide)return 1;int ok=1;for(int i=0;i<argc;i++){wide[i]=calloc(strlen(argv[i])+1,sizeof(wchar_t));if(!wide[i]||!fs_decode_utf8(0,0,argv[i],-1,wide[i],(int)strlen(argv[i])+1)){ok=0;break;}}int result=ok?run(argc,wide):1;for(int i=0;i<argc;i++)free(wide[i]);free(wide);return result;}
