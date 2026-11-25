"""Reconstruct one selected photo in a dedicated, cancellable local process."""

import json
import os
from pathlib import Path
import sys

os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "4")
sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor" / "triposr"))

from PIL import Image

from model_cache import cached_asset


TRIPOSR = ("stabilityai/TripoSR", "model", "5b521936b01fbe1890f6f9baed0254ab6351c04a")
DINO = ("facebook/dino-vitb16", "model", "f205d5d8e640a89a2b8ef0369670dfc37cc07fc2")


def progress(message):
    print(message, flush=True)


def prepare_foreground(image):
    image = image.convert("RGBA")
    if image.getchannel("A").getextrema()[0] == 255:
        progress("동물과 배경 분리 중 · 최초 실행 시 U²-Net 다운로드")
        import rembg

        session = rembg.new_session("u2net", providers=["CPUExecutionProvider"])
        image = rembg.remove(image.convert("RGB"), session=session)
    alpha = image.getchannel("A")
    box = alpha.point(lambda v: 255 if v > 12 else 0).getbbox()
    if box is None:
        raise ValueError("동물을 찾지 못했습니다. 배경과 동물의 경계가 뚜렷한 사진을 선택해 주세요.")
    cutout = image.crop(box)
    side = max(2, round(max(cutout.size) / 0.85))
    background = Image.new("RGB", (side, side), (128, 128, 128))
    background.paste(cutout, ((side - cutout.width) // 2, (side - cutout.height) // 2),
                     cutout.getchannel("A"))
    return background


def reconstruct(image, source, destination, device):
    import numpy as np
    import torch
    import trimesh
    from omegaconf import OmegaConf
    from tsr.system import TSR
    from tsr.utils import to_gradio_3d_orientation

    hardware = torch.cuda.get_device_name() if device == "cuda" else "CPU"
    torch.set_num_threads(min(6, torch.get_num_threads()))
    progress(f"TripoSR 모델 준비 중 · {hardware}")
    config = OmegaConf.load(cached_asset(TRIPOSR, "config.yaml", progress))
    weights = cached_asset(TRIPOSR, "model.ckpt", progress)
    config.image_tokenizer.pretrained_model_name_or_path = str(cached_asset(DINO, "config.json", progress))
    OmegaConf.resolve(config)
    model = TSR(config)
    model.load_state_dict(torch.load(weights, map_location="cpu", weights_only=True, mmap=True), assign=True)
    model.eval().to(device)
    model.renderer.set_chunk_size(32768)
    with torch.inference_mode():
        progress(f"입체 형태 추론 중 · {hardware}")
        codes = model([image], device=device)
        progress("표면과 정점 색상 추출 중")
        try:
            mesh = model.extract_mesh(codes, has_vertex_color=True,
                                      resolution=224 if device == "cuda" else 160)[0]
        except (ValueError, RuntimeError) as error:
            if "surface" in str(error).lower():
                raise ValueError("표면을 찾지 못했습니다. 동물이 크게 보이는 사진으로 다시 시도해 주세요.") from error
            raise
    if not len(mesh.faces) or not np.isfinite(mesh.vertices).all():
        raise ValueError("유효한 3D 표면을 만들지 못했습니다. 다른 사진을 선택해 주세요.")
    to_gradio_3d_orientation(mesh)
    progress("GLB 파일 저장 중")
    scene = trimesh.Scene(mesh)
    scene.metadata.update({"generator": "TripoSR", "sourceImage": source})
    scene.export(destination, file_type="glb", include_normals=True)
    progress(f"{hardware} · 삼각형 {len(mesh.faces):,}개")


def main(directory):
    source = json.loads((directory / "source.json").read_text(encoding="utf-8"))
    if source.get("page_id"):
        from commons import reference_image

        progress("선택한 커먼스 사진 다운로드 중")
        image = reference_image(source["page_id"])
    else:
        with Image.open(directory / "input.png") as file:
            image = file.copy()
    progress("입력 사진 준비 중")
    image = prepare_foreground(image)
    import torch

    output = directory / "mesh.glb"
    if torch.cuda.is_available():
        try:
            reconstruct(image, source, output, "cuda")
            return
        except torch.cuda.OutOfMemoryError:
            progress("GPU 메모리가 부족해 CPU로 다시 생성합니다.")
        # Leave the exception scope before releasing its tensors.
        import gc
        gc.collect()
        torch.cuda.empty_cache()
    reconstruct(image, source, output, "cpu")


if __name__ == "__main__":
    try:
        main(Path(sys.argv[1]))
    except Exception as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
