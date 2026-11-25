"""Run inside a background Blender process, with arguments after --."""

from pathlib import Path
import shutil
import sys

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from glb_info import triangle_count


def progress(message):
    print("FAUNA " + message, flush=True)


def fit_budget(objects, target, original):
    modifiers = []
    for obj in objects:
        mod = obj.modifiers.new("Fauna · Decimate", "DECIMATE")
        mod.decimate_type = "COLLAPSE"
        mod.use_collapse_triangulate = True
        modifiers.append(mod)

    def evaluate(ratio):
        for mod in modifiers:
            mod.ratio = ratio
        depsgraph = bpy.context.evaluated_depsgraph_get()
        count = 0
        for obj in objects:
            mesh = obj.evaluated_get(depsgraph).data
            mesh.calc_loop_triangles()
            count += len(mesh.loop_triangles)
        return count

    # Collapse removes whole edges. A closed surface can skip an odd target.
    low, high = 0.0, 1.0
    ratio = min(1.0, (target + 0.25) / original)
    best = None
    for _ in range(24):
        count = evaluate(ratio)
        if 0 < count <= target:
            best = ratio
            if count == target:
                return
        if count <= target:
            low = ratio
        else:
            high = ratio
        if (high - low) * original < 0.25:
            break
        ratio = (low + high) / 2
    if best is None:
        raise ValueError("이 메시를 지정한 수 이하의 삼각형으로 유지할 수 없습니다. 목표를 높여 주세요.")
    for mod in modifiers:
        mod.ratio = best


def convert(source, target, destination):
    original = triangle_count(source)
    if target < 1:
        raise ValueError("목표 삼각형 수는 1 이상이어야 합니다.")
    if original <= target:
        shutil.copyfile(source, destination)
        progress(f"원본이 목표 이하 · {original:,}개 그대로 저장")
        return
    progress("Blender에서 원본 메시를 읽는 중")
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(source))
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH" and len(o.data.polygons)]
    if not meshes:
        raise ValueError("Blender에서 변환할 메시를 찾을 수 없습니다.")
    progress(f"삼각형 줄이는 중 · 목표 {target:,}개")
    fit_budget(meshes, target, original)
    progress("정점 색상과 메시를 GLB로 저장하는 중")
    bpy.ops.export_scene.gltf(
        filepath=str(destination), export_format="GLB", export_apply=True,
        export_extras=True, use_active_scene=True, export_vertex_color="MATERIAL",
        export_active_vertex_color_when_no_material=True,
    )
    actual = triangle_count(destination)
    if actual > target:
        raise ValueError(f"내보낸 메시가 목표를 초과합니다 ({actual:,}개).")
    progress(f"{original:,}개 → {actual:,}개 · 목표 {target:,}개")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    source, target, destination = sys.argv[sys.argv.index("--") + 1:]
    try:
        convert(Path(source), int(target), Path(destination))
    except Exception as error:
        print(str(error), file=sys.stderr, flush=True)
        sys.exit(1)
