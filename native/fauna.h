#ifndef FAUNA_H
#define FAUNA_H
#include "portable.h"
#include <stdint.h>
#include <stdio.h>

#define FS_PATH 4096
#define FS_ERROR 512
typedef struct {
    float p[3], n[3]; unsigned char color[4]; float detail;
    /* Reference image coordinates survive source-proportion deformation. */
    float photo_uv[2]; int photo_uv_valid;
    float limb; uint32_t limb_id;
    float face, source_uv[2], smooth;
    int observed;
} Vertex;
typedef struct { uint32_t v[3]; } Triangle;
typedef struct { Vertex *vertices; Triangle *triangles; uint32_t vertex_count, triangle_count; } Mesh;
typedef struct { unsigned char *rgba; uint32_t width, height; } Photo;
typedef struct { float xy[17][2], score[17]; } AnimalPose;
typedef struct {
    Mesh head;
    Photo head_photo, leg_photo;
    AnimalPose source_pose, head_pose, leg_pose;
    int head_index, leg_index, standing;
} VolumeGuide;
typedef struct { double silhouette_iou, depth_rmse; } Quality;
typedef struct {
    int applied, mirrored, separated_branches;
    double silhouette_before, silhouette_after, head_height_scale;
    int ground_aligned;
    double ground_tilt_degrees, foot_height_spread_before, foot_height_spread_after;
} BodyFit;
enum { EXPORT_GLB=1, EXPORT_STL=2, EXPORT_STEP=4, EXPORT_3MF=8, EXPORT_ALL=15 };
typedef void (*Progress)(void *context, const wchar_t *message);
typedef struct {
    wchar_t root[FS_PATH], input[FS_PATH], output[FS_PATH], result[FS_PATH];
    wchar_t error[FS_ERROR];
    uint32_t target, resolution;
    unsigned formats;
    double cad_size_mm;
    Mesh preview;
    FsCancel * cancel;
    Progress progress;
    void *context;
    Quality quality;
    uint32_t original_triangles, final_triangles;
    int kept_original;
    int reference_count;
    int shape_disabled, shape_used;
    double shape_iou, shape_depth_ratio;
    int face_detail_vertices, crease_detail_vertices;
    int face_projection; float face_xy[4],face_box[4];
    double face_surface_displacement;
    double face_relief_displacement, crease_displacement;
    BodyFit body_fit;
    int observed_surface;
    double observed_iou_before,observed_iou_after,observed_depth_scale;
    int volume_head,volume_legs,volume_head_index,volume_leg_index;
    double volume_head_scale,volume_paw_radius,volume_muzzle_depth;
    wchar_t species[128];
} Job;

void fs_error(wchar_t *error, const wchar_t *message);
int fs_join(wchar_t *out, const wchar_t *a, const wchar_t *b);
int fs_root(wchar_t *out);
void fs_status(Job *job, const wchar_t *message);
int fs_cancelled(Job *job);
int fs_python(Job *job, const wchar_t *mode, const wchar_t *input, const wchar_t *output, const wchar_t *log);
int fs_generate(Job *job);
int fs_save_model(Job *job, const Mesh *mesh);
int fs_default_output(wchar_t *out);
int references_collect(Job *job, const wchar_t *directory, const wchar_t *photo);
char *gbif_reference_media(const char *payload, const char *scientific, uint32_t taxon_key);
int references_volume(Job *job, const wchar_t *directory, const Photo *photo, const Mesh *base, Mesh *out);
int mesh_volume(Job *job, const Photo *photo, const Mesh *base, const VolumeGuide *guide, Mesh *out);
int mesh_source_colours(const Mesh *source, Mesh *out, FsCancel * cancel, wchar_t *error);
int references_apply(Job *job, const wchar_t *directory, Mesh *mesh);
int references_complete(Job *job, const wchar_t *directory, const Mesh *original, Mesh *out);
int shape_candidate_count(const wchar_t *directory);
int mesh_refine_details(Job *job, const wchar_t *directory, Mesh *mesh);
int mesh_finish_face(Job *job, const wchar_t *directory, Mesh *mesh);
int mesh_fit_body(Job *job, const wchar_t *directory, int reference_index,
                  Mesh *mesh, BodyFit *fit);
int mesh_limb_branches(Mesh *mesh, int protect, FsCancel * cancel, wchar_t *error);
int mesh_ground_body(Job *job, Mesh *mesh);
int mesh_observed(Job *job, const Photo *photo, const wchar_t *depth_path, const Mesh *base, Mesh *out);
int mesh_observed_colour(Job *job, const Photo *photo, Mesh *mesh);
int mesh_complete(const Mesh *original, const Mesh *prior, Mesh *out, FsCancel * cancel,
                  double *iou, double *depth_ratio, wchar_t *error);

int photo_load(const wchar_t *path, Photo *photo, wchar_t *error);
int photo_read(const wchar_t *path, Photo *photo, wchar_t *error);
int photo_write(const wchar_t *path, const Photo *photo, wchar_t *error);
int photo_prepare(const Photo *source, Photo *out, wchar_t *error);
int photo_has_alpha(const Photo *photo);
void photo_free(Photo *photo);

void mesh_free(Mesh *mesh);
int mesh_copy(const Mesh *source, Mesh *out);
int mesh_validate(const Mesh *mesh, wchar_t *error);
void mesh_normals(Mesh *mesh);
void mesh_bounds(const Mesh *mesh, float lo[3], float hi[3]);
int mesh_read_raw(const wchar_t *path, Mesh *mesh, wchar_t *error);
int mesh_read_glb(const wchar_t *path, Mesh *mesh, wchar_t *error);
int mesh_write_glb(const wchar_t *path, const Mesh *mesh, const char *extras, wchar_t *error);
int mesh_write_stl(const wchar_t *path, const Mesh *mesh, double size_mm, FsCancel * cancel, wchar_t *error);
int mesh_write_step(const wchar_t *path, const Mesh *mesh, double size_mm, FsCancel * cancel, wchar_t *error);
int mesh_write_3mf(const wchar_t *path, const Mesh *mesh, double size_mm, FsCancel * cancel, wchar_t *error);
int mesh_export(const wchar_t *directory, const Mesh *mesh, unsigned formats, double size_mm,
                const char *extras, FsCancel * cancel, wchar_t *error);
int mesh_simplify(Mesh *mesh, uint32_t target, FsCancel * cancel, wchar_t *error);
int mesh_facets(Mesh *mesh, wchar_t *error);
int mesh_lowpoly(const Mesh *source, Mesh *out, uint32_t target, FsCancel * cancel,
                 Quality *quality, int *kept_original, wchar_t *error);
int mesh_surface(const Mesh *source, Mesh *out, uint32_t target, FsCancel * cancel,
                 Quality *quality, int *kept_original, wchar_t *error);
Quality mesh_compare(const Mesh *original, const Mesh *candidate);

#endif
