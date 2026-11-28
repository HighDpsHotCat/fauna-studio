#include "fauna.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../vendor/cjson/cJSON.h"

static int sha256(const wchar_t *path,char hex[65]){return fs_sha256_file(path,hex);}

int fs_generate(Job *job){
    wchar_t directory[FS_PATH],parent[FS_PATH],input[FS_PATH],mask[FS_PATH],prepared[FS_PATH],raw[FS_PATH],log[FS_PATH],path[FS_PATH];
    Photo photo={0},cutout={0},normalized={0},depth_photo={0};Mesh original={0},colored={0},reduced={0},observed={0};int ok=0;
    cJSON *report=NULL;char *metadata=NULL;FsId id;job->error[0]=0;
    if(!job->formats||(job->formats&~EXPORT_ALL)){fs_error(job->error,L"저장할 형식을 하나 이상 선택하세요.");return 0;}
    if((job->formats&(EXPORT_STL|EXPORT_STEP|EXPORT_3MF))&&(!isfinite(job->cad_size_mm)||job->cad_size_mm<1||job->cad_size_mm>10000)){fs_error(job->error,L"인쇄 파일 크기를 1~10,000 mm 사이로 입력하세요.");return 0;}
    if(job->target<300||job->target>200000||(job->resolution!=192&&job->resolution!=256&&job->resolution!=320)){fs_error(job->error,L"삼각형 수 또는 추출 해상도가 잘못되었습니다.");return 0;}
    if(!job->output[0]&&!fs_default_output(job->output))return 0;
    if(fs_exists(job->output)){fs_error(job->error,L"결과 폴더가 이미 있습니다. 새 폴더 이름을 사용하세요.");return 0;}
    fs_wcopy(parent,FS_PATH,job->output);fs_parent(parent);
    int created=fs_mkdirs(NULL,parent,NULL);if(created!=ERROR_SUCCESS&&created!=ERROR_ALREADY_EXISTS&&created!=ERROR_FILE_EXISTS){fs_error(job->error,L"저장 위치를 만들 수 없습니다.");return 0;}
    if(FS_FAILED(fs_create_id(&id)))return 0;
    if(fs_wformat(directory,FS_PATH,L"%s.partial-%08lx",job->output,id.value)<0||!fs_mkdir(directory,NULL)){fs_error(job->error,L"작업 폴더를 만들 수 없습니다.");return 0;}
    fs_join(input,directory,L"input.rgba");fs_join(mask,directory,L"cutout.rgba");fs_join(prepared,directory,L"prepared.rgba");
    fs_join(raw,directory,L"mesh.raw");fs_join(log,directory,L"inference.log");
    fs_status(job,L"C에서 사진을 읽고 방향·크기를 정리하는 중");
    if(fs_cancelled(job)||!photo_load(job->input,&photo,job->error)||!photo_write(input,&photo,job->error))goto done;
    if(photo_has_alpha(&photo)){cutout=photo;memset(&photo,0,sizeof(photo));}
    else {
        if(!fs_python(job,L"segment",input,mask,log)||!photo_read(mask,&cutout,job->error))goto done;
    }
    if(!photo_write(mask,&cutout,job->error))goto done;
    if(!references_collect(job,directory,input))goto done;
    fs_status(job,L"C에서 피사체 여백·배경을 전처리하는 중");
    if(fs_cancelled(job)||!photo_prepare(&cutout,&normalized,job->error)||!photo_write(prepared,&normalized,job->error))goto done;
    if(!fs_python(job,L"infer",prepared,raw,log)||fs_cancelled(job)||!mesh_read_raw(raw,&original,job->error))goto done;
    job->original_triangles=original.triangle_count;
    if(!references_complete(job,directory,&original,&colored))goto done;
    if(!references_apply(job,directory,&colored))goto done;
    wchar_t depth_input[FS_PATH],depth_path[FS_PATH];fs_join(depth_input,directory,L"depth-input.rgba");fs_join(depth_path,directory,L"observed-depth.bin");
    depth_photo.width=cutout.width;depth_photo.height=cutout.height;depth_photo.rgba=malloc((size_t)cutout.width*cutout.height*4);
    if(!depth_photo.rgba){fs_error(job->error,L"깊이 입력 메모리가 부족합니다.");goto done;}
    for(size_t i=0;i<(size_t)cutout.width*cutout.height;i++){const unsigned char *s=cutout.rgba+4*i;unsigned char *d=depth_photo.rgba+4*i;for(int k=0;k<3;k++)d[k]=(unsigned char)((s[k]*s[3]+128*(255-s[3])+127)/255);d[3]=255;}
    if(!photo_write(depth_input,&depth_photo,job->error))goto done;
    if(fs_python(job,L"depth",depth_input,depth_path,log)){
        fs_status(job,L"C에서 원본의 머리·주둥이와 발 윤곽을 보완하는 중");
        if(!mesh_observed(job,&cutout,depth_path,&colored,&observed))goto done;
        mesh_free(&colored);colored=observed;memset(&observed,0,sizeof(observed));
    }else{if(fs_cancelled(job))goto done;job->error[0]=0;fs_status(job,L"깊이 보완을 사용할 수 없어 TripoSR 원형을 유지합니다.");}
    if(!references_volume(job,directory,&cutout,&colored,&observed))goto done;
    mesh_free(&colored);colored=observed;memset(&observed,0,sizeof(observed));
    fs_status(job,L"C에서 눈 무늬를 지우고 각진 로우폴리 면을 만드는 중");
    if(!mesh_form_model(job,directory,&cutout,&colored,&reduced)||fs_cancelled(job))goto done;
    const Mesh *result=&reduced;job->final_triangles=result->triangle_count;
    if(result->triangle_count>job->target)fs_status(job,L"귀·주둥이·턱과 발 윤곽에 필요한 면을 유지합니다.");
    char digest[65];if(!sha256(job->input,digest)){fs_error(job->error,L"원본 사진 해시를 계산할 수 없습니다.");goto done;}
    report=cJSON_CreateObject();
    cJSON_AddStringToObject(report,"application","AAAnimal Studio");cJSON_AddStringToObject(report,"model","stabilityai/TripoSR");
    cJSON_AddStringToObject(report,"revision","5b521936b01fbe1890f6f9baed0254ab6351c04a");
    cJSON_AddStringToObject(report,"inputSha256",digest);
    cJSON_AddStringToObject(report,"geometrySource",job->volume_head?"C volume fusion: source body, registered oblique TripoSR head prior, estimated source-joint legs when supported":(job->observed_surface?"C closed surface: input silhouette and relative depth on observed front, TripoSR rear volume":"TripoSR surface; observed-front reconstruction unavailable"));
    cJSON_AddBoolToObject(report,"referenceHeadVolumeApplied",job->volume_head);cJSON_AddBoolToObject(report,"referenceLegVolumeApplied",job->volume_legs);
    cJSON_AddStringToObject(report,"volumeDetails","volume-completion.json; anatomy prior from another individual, not measured hidden anatomy");
    cJSON_AddStringToObject(report,"observedIoUMeaning","source silhouette fitting before optional reference volume fusion; not final anatomy accuracy");
    cJSON_AddBoolToObject(report,"observedSurfaceApplied",job->observed_surface);
    cJSON_AddNumberToObject(report,"observedPhotoIoUBefore",job->observed_iou_before);
    cJSON_AddNumberToObject(report,"observedPhotoIoUAfter",job->observed_iou_after);
    cJSON_AddNumberToObject(report,"relativeDepthScale",job->observed_depth_scale);
    cJSON_AddStringToObject(report,"depthModel","depth-anything/Depth-Anything-V2-Small-hf");
    cJSON_AddStringToObject(report,"depthRevision","5426e4f0f36572d16453bbda7a8389317b1bef99");
    cJSON_AddStringToObject(report,"depthMeaning","relative single-image depth cue, not metric measurement; hidden anatomy remains inferred");
    cJSON_AddBoolToObject(report,"sourceProportionFit",job->body_fit.applied);
    cJSON_AddNumberToObject(report,"sourcePhotoIoUBeforeFit",job->body_fit.silhouette_before);
    cJSON_AddNumberToObject(report,"sourcePhotoIoUAfterFit",job->body_fit.silhouette_after);
    cJSON_AddNumberToObject(report,"headHeightScale",job->body_fit.head_height_scale);
    cJSON_AddNumberToObject(report,"separatedLowerLimbBranches",job->body_fit.separated_branches);
    cJSON_AddBoolToObject(report,"groundPlaneAligned",job->body_fit.ground_aligned);
    cJSON_AddNumberToObject(report,"groundTiltDegrees",job->body_fit.ground_tilt_degrees);
    cJSON_AddNumberToObject(report,"footHeightSpreadBefore",job->body_fit.foot_height_spread_before);
    cJSON_AddNumberToObject(report,"footHeightSpreadAfter",job->body_fit.foot_height_spread_after);
    cJSON_AddStringToObject(report,"bodyFitMeaning","photo IoU compares cropped source silhouette before ground rotation; lower branches are a geometry proxy, not confirmed anatomical labels");
    cJSON_AddStringToObject(report,"geometryMode",job->volume_head?"source_with_anatomy_prior":(job->shape_used?"source_with_rear_prior":"source_only"));
    cJSON_AddBoolToObject(report,"shapeReferenceUsed",job->shape_used);
    cJSON_AddNumberToObject(report,"shapeFrontSilhouetteIoU",job->shape_iou);
    cJSON_AddNumberToObject(report,"shapeDepthRatio",job->shape_depth_ratio);
    cJSON_AddStringToObject(report,"referenceUse","C-selected palette, up to 15 percent blend on unseen rear only; original front colours preserved; see references.json");
    cJSON_AddStringToObject(report,"style","faceted-form-no-eyes-v1");
    cJSON_AddNumberToObject(report,"eyeRegionsSuppressed",job->eye_regions);
    cJSON_AddNumberToObject(report,"eyeColourVerticesChanged",job->eye_colour_vertices);
    cJSON_AddBoolToObject(report,"uniformCoatFallback",job->eye_colour_fallback);
    cJSON_AddStringToObject(report,"faceStyle","ears, forehead, muzzle, nose, mouth and jaw volumes; no painted eyes or fine facial refinement");
    cJSON_AddStringToObject(report,"normals","one geometric normal per triangle across face and body; stored in GLB");
    cJSON_AddStringToObject(report,"colors","24-colour coat palette; one colour per triangle; eye regions replaced with surrounding coat before simplification; uniform coat if landmarks unavailable");
    cJSON_AddNumberToObject(report,"faceDetailVertices",job->face_detail_vertices);
    cJSON_AddBoolToObject(report,"finalViewFaceProjection",job->face_projection);
    cJSON_AddNumberToObject(report,"faceSurfaceMaxDisplacement",job->face_surface_displacement);
    cJSON_AddNumberToObject(report,"creaseDetailVertices",job->crease_detail_vertices);
    cJSON_AddNumberToObject(report,"faceReliefMaxDisplacement",job->face_relief_displacement);
    cJSON_AddNumberToObject(report,"creaseMaxDisplacement",job->crease_displacement);
    /* Protection weights are colour/lower-surface heuristics, not anatomical
     * landmark detections. Keep legacy face counters at zero for old readers. */
    unsigned protected_triangles=0;
    for(uint32_t i=0;i<result->triangle_count;i++){int detail=0;for(int k=0;k<3;k++)detail+=result->vertices[result->triangles[i].v[k]].detail>.65f;protected_triangles+=detail>=2;}
    cJSON_AddNumberToObject(report,"faceDetailTriangles",0);
    cJSON_AddNumberToObject(report,"eyeNoseMouthTriangles",0);
    cJSON_AddNumberToObject(report,"detailProtectedTriangles",protected_triangles);
    cJSON_AddBoolToObject(report,"sourceFrontGeometryPreserved",!job->observed_surface&&!job->volume_head);
    cJSON_AddStringToObject(report,"detailMethod","closed front/rear surface fusion; geometry-focused C QEM; no iris subdivisions or final photo projection; six-view outline/depth checks");
    cJSON_AddBoolToObject(report,"budgetAdjusted",result->triangle_count>job->target);
    char species[512];fs_encode_utf8(CP_UTF8,0,job->species,-1,species,sizeof(species),NULL,NULL);
    cJSON_AddStringToObject(report,"species",species);cJSON_AddNumberToObject(report,"referenceCount",job->reference_count);
    cJSON_AddStringToObject(report,"referenceStatus",job->reference_count>=2?"ready":(job->reference_count?"limited":"input_only"));
    cJSON_AddNumberToObject(report,"referenceColourBlend",job->reference_count>=2?.15:(job->reference_count?.075:0));
    cJSON_AddNumberToObject(report,"extractionResolution",job->resolution);
    cJSON_AddNumberToObject(report,"requestedTriangles",job->target);cJSON_AddNumberToObject(report,"originalTriangles",original.triangle_count);
    cJSON_AddNumberToObject(report,"resultTriangles",result->triangle_count);cJSON_AddNumberToObject(report,"minimumSilhouetteIoU",job->quality.silhouette_iou);
    cJSON_AddNumberToObject(report,"maximumDepthRMSE",job->quality.depth_rmse);cJSON_AddBoolToObject(report,"keptOriginal",job->kept_original);
    cJSON_AddStringToObject(report,"qualityMeaning","simplification preservation of completed geometry, not accuracy against a real animal");
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
    metadata=cJSON_PrintUnformatted(report);if(!metadata){fs_error(job->error,L"메타데이터 메모리가 부족합니다.");goto done;}
    fs_status(job,L"선택한 형식으로 최종 모델을 저장하는 중");
    if(job->formats&EXPORT_GLB){fs_join(path,directory,L"original.glb");if(!mesh_write_glb(path,&original,"{\"model\":\"TripoSR\",\"unmodifiedInference\":true}",job->error))goto done;}
    if(job->formats&EXPORT_GLB){fs_join(path,directory,L"completed.glb");if(!mesh_write_glb(path,&colored,"{\"stage\":\"before_faceted_style\",\"finalOutput\":\"model.glb\"}",job->error))goto done;}
    if(!mesh_export(directory,result,job->formats,job->cad_size_mm,metadata,job->cancel,job->error))goto done;
    fs_join(path,directory,L"report.json");FILE *f=fs_wfopen(path,L"wb");if(!f)goto done;
    size_t length=strlen(metadata);int written=fwrite(metadata,1,length,f)==length;if(fclose(f))written=0;if(!written)goto done;
    if(fs_cancelled(job))goto done;
    /* Only complete jobs become visible under their final name. Never overwrite. */
    fs_unlink(input);fs_unlink(mask);fs_unlink(prepared);fs_unlink(raw);fs_unlink(depth_input);
    if(!fs_move(directory,job->output,MOVEFILE_WRITE_THROUGH)){fs_error(job->error,L"결과 폴더를 확정하지 못했습니다. 저장 위치를 확인하세요.");goto done;}
    fs_join(job->result,job->output,(job->formats&EXPORT_GLB)?L"model.glb":((job->formats&EXPORT_STL)?L"model.stl":((job->formats&EXPORT_STEP)?L"model.step":L"model.3mf")));
    job->preview=reduced;memset(&reduced,0,sizeof(reduced));ok=1;fs_status(job,L"생성 완료");
done:
    if(!ok){
        wchar_t diagnostic[FS_PATH+40];fs_wformat(diagnostic,FS_PATH+40,L"작업 기록: %s",directory);fs_status(job,diagnostic);
        if(!job->error[0])fs_error(job->error,L"결과 저장에 실패했습니다. 작업 폴더를 확인하세요.");
    }
    photo_free(&photo);photo_free(&cutout);photo_free(&normalized);photo_free(&depth_photo);mesh_free(&original);mesh_free(&colored);mesh_free(&reduced);mesh_free(&observed);free(metadata);cJSON_Delete(report);return ok;
}
