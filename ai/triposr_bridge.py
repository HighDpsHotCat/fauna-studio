"""Model-only adapter. The C executable owns the UI, workflow and mesh processing.

FRG1: <4sII followed by width*height*4 RGBA bytes.
FMS1: <4sII followed by float32 XYZ, uint8 RGBA, uint32 triangle indices.
FDP1: <4sII followed by width*height float32 relative depth values.
All values are little-endian. No pickle or Python objects cross this boundary.
"""
import argparse
import os
from pathlib import Path
import struct
import sys
import json

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "vendor" / "triposr"))
os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "4")
os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")

TRIPOSR = ("stabilityai/TripoSR", "model", "5b521936b01fbe1890f6f9baed0254ab6351c04a")
DINO = ("facebook/dino-vitb16", "model", "f205d5d8e640a89a2b8ef0369670dfc37cc07fc2")
BLIP = ("Salesforce/blip-image-captioning-base", "model", "82a37760796d32b1411fe092ab5d4e227313294b")
DEPTH = ("depth-anything/Depth-Anything-V2-Small-hf", "model", "5426e4f0f36572d16453bbda7a8389317b1bef99")


class Captioner:
    def __init__(self):
        import torch
        from transformers import BlipProcessor, BlipForConditionalGeneration
        from model_cache import cached_asset
        directory = None
        for name in ("config.json", "preprocessor_config.json", "special_tokens_map.json", "tokenizer.json", "tokenizer_config.json", "vocab.txt", "pytorch_model.bin"):
            directory = cached_asset(BLIP, name, progress).parent
        self.device = __import__("ai.device", fromlist=["inference_device"]).inference_device()
        self.processor = BlipProcessor.from_pretrained(directory, local_files_only=True)
        self.model = BlipForConditionalGeneration.from_pretrained(directory, local_files_only=True).to(self.device).eval()

    def describe(self, image):
        import torch
        with torch.inference_mode():
            ids = self.model.generate(**self.processor(images=image.convert("RGB"), return_tensors="pt").to(self.device), max_new_tokens=40, num_beams=3)
        return self.processor.decode(ids[0], skip_special_tokens=True)


def cat_face_box(image):
    """Pretrained OpenCV cat-face detector; C owns feature placement/colour."""
    import cv2
    import numpy as np
    small = image.convert("RGB")
    small.thumbnail((640, 640))
    xml = Path(cv2.data.haarcascades, "haarcascade_frontalcatface_extended.xml").read_text(encoding="utf-8")
    storage = cv2.FileStorage(xml, cv2.FILE_STORAGE_READ | cv2.FILE_STORAGE_MEMORY)
    detector = cv2.CascadeClassifier()
    detector.read(storage.getFirstTopLevelNode())
    faces = detector.detectMultiScale(cv2.cvtColor(np.asarray(small), cv2.COLOR_RGB2GRAY), 1.05, 3)
    storage.release()
    if len(faces) != 1:
        return None
    x, y, w, h = map(int, faces[0])
    return [x / small.width, y / small.height, w / small.width, h / small.height]


def progress(message):
    print(message, flush=True)


def read_rgba(path):
    from PIL import Image
    with open(path, "rb") as stream:
        magic, width, height = struct.unpack("<4sII", stream.read(12))
        if magic != b"FRG1" or not (0 < width <= 8192 and 0 < height <= 8192):
            raise ValueError("Invalid C image header")
        data = stream.read()
    if len(data) != width * height * 4:
        raise ValueError("Invalid C image length")
    return Image.frombytes("RGBA", (width, height), data)


def segment(args):
    import rembg
    progress("배경 분리 모델 실행 중")
    image = read_rgba(args.input)
    session = rembg.new_session("u2net", providers=["CPUExecutionProvider"])
    result = rembg.remove(image, session=session).convert("RGBA")
    with open(args.output, "wb") as stream:
        stream.write(struct.pack("<4sII", b"FRG1", *result.size))
        stream.write(result.tobytes())


def infer(args):
    import numpy as np
    import torch
    from omegaconf import OmegaConf
    from model_cache import cached_asset
    from tsr.system import TSR

    device = __import__("ai.device", fromlist=["inference_device"]).inference_device()
    torch.set_num_threads(min(6, torch.get_num_threads()))
    progress(f"TripoSR 준비 중 · {device.upper()}")
    config = OmegaConf.load(cached_asset(TRIPOSR, "config.yaml", progress))
    weights = cached_asset(TRIPOSR, "model.ckpt", progress)
    config.image_tokenizer.pretrained_model_name_or_path = str(cached_asset(DINO, "config.json", progress))
    OmegaConf.resolve(config)
    model = TSR(config)
    model.load_state_dict(torch.load(weights, map_location="cpu", weights_only=True, mmap=True), assign=True)
    model.eval().to(device)
    model.renderer.set_chunk_size(8192)
    # Keep FP32 and the selected extraction resolution on both CPU and GPU.
    # No quantization, retraining or silent quality downgrade.
    tasks = json.loads(args.input.read_text(encoding="utf-8")) if args.mode == "infer-batch" else [{"input": str(args.input), "output": str(args.output)}]
    for index, task in enumerate(tasks):
        with torch.inference_mode():
            progress(f"TripoSR 입체 형상 추론 중 · {index + 1}/{len(tasks)}")
            codes = model([read_rgba(task["input"]).convert("RGB")], device=device)
            progress(f"TripoSR 표면·색상 추출 중 · 해상도 {args.resolution}")
            mesh = model.extract_mesh(codes, has_vertex_color=True, resolution=args.resolution)[0]
        vertices = np.asarray(mesh.vertices, dtype="<f4")
        triangles = np.asarray(mesh.faces, dtype="<u4")
        colors = np.asarray(mesh.visual.vertex_colors, dtype=np.uint8)
        if not len(triangles) or not np.isfinite(vertices).all():
            raise ValueError("유효한 표면이 없습니다. 피사체가 선명한 전신 사진을 선택하세요.")
        with open(task["output"], "wb") as stream:
            stream.write(struct.pack("<4sII", b"FMS1", len(vertices), len(triangles)))
            stream.write(vertices.tobytes())
            stream.write(colors.tobytes())
            stream.write(triangles.tobytes())
        progress(f"TripoSR 완료 · 삼각형 {len(triangles):,}개")
    if args.mode == "infer-batch":
        args.output.write_text(json.dumps({"completed": len(tasks)}), encoding="utf-8")


def classify(args):
    from dataclasses import asdict
    from classifier import AnimalClassifier
    engine = AnimalClassifier(progress)
    candidates = engine.predict(read_rgba(args.input).convert("RGB"), limit=8)
    args.output.write_text(json.dumps([asdict(c) for c in candidates]), encoding="utf-8")


def depth(args):
    """Pretrained relative depth only; C owns silhouette/mesh fusion and colour."""
    import numpy as np
    import torch
    from transformers import AutoImageProcessor, AutoModelForDepthEstimation
    from model_cache import cached_asset
    progress("원본 사진의 얼굴·다리 깊이 단서 추론 중")
    directory = None
    for name in ("config.json", "preprocessor_config.json", "model.safetensors"):
        directory = cached_asset(DEPTH, name, progress).parent
    processor = AutoImageProcessor.from_pretrained(directory, local_files_only=True)
    device = __import__("ai.device", fromlist=["inference_device"]).inference_device()
    model = AutoModelForDepthEstimation.from_pretrained(directory, local_files_only=True).eval().to(device)
    image = read_rgba(args.input).convert("RGB")
    with torch.inference_mode():
        prediction = model(**processor(images=image, return_tensors="pt").to(device)).predicted_depth
        prediction = torch.nn.functional.interpolate(prediction[:, None], size=(image.height, image.width), mode="bicubic", align_corners=False)[0, 0]
    values = prediction.cpu().numpy().astype("<f4")
    if not np.isfinite(values).all() or np.ptp(values) < 1e-6:
        raise ValueError("유효한 깊이 단서를 찾지 못했습니다.")
    with args.output.open("wb") as stream:
        stream.write(struct.pack("<4sII", b"FDP1", image.width, image.height))
        stream.write(values.tobytes())


def context(args):
    from photo_context import PhotoContextEncoder
    species_path = args.input.parent / "species.json"
    species = json.loads(species_path.read_text(encoding="utf-8")) if species_path.exists() else {}
    engine = PhotoContextEncoder(progress, animal_name=species.get("common") or species.get("scientific") or "animal")
    cutout = read_rgba(args.input)
    original = args.input.parent / "input.rgba"
    result = engine.analyse(cutout, context_image=read_rgba(original) if original.exists() else cutout)
    result["caption"] = Captioner().describe(read_rgba(original) if original.exists() else cutout)
    result["captionModel"] = {"name": BLIP[0], "revision": BLIP[2]}
    try:
        from ai.animal_pose import AnimalPose
        result['animalPose'] = AnimalPose(progress).predict(read_rgba(original) if original.exists() else cutout, cutout)
    except Exception as error:  # Optional model: archive/ORT failures preserve base reconstruction.
        result['animalPoseError'] = str(error)
    if species.get("scientific") == "Felis catus":
        result["faceBox"] = cat_face_box(read_rgba(original) if original.exists() else cutout)
    args.output.write_text(json.dumps(result), encoding="utf-8")


def analyse(args):
    """Batch neural visual cues and foreground masks; C selects/uses references."""
    import rembg
    from dataclasses import asdict
    from classifier import AnimalClassifier
    from photo_context import PhotoContextEncoder
    items = json.loads(args.input.read_text(encoding="utf-8"))
    previous = json.loads(args.output.read_text(encoding="utf-8")) if args.output.exists() else []
    species_path = args.input.parent / "species.json"
    species = json.loads(species_path.read_text(encoding="utf-8")) if species_path.exists() else {}
    encoder = PhotoContextEncoder(progress, animal_name=species.get("common") or species.get("scientific") or "animal")
    classifier = AnimalClassifier(progress)
    captioner = Captioner()
    session = rembg.new_session("u2net", providers=["CPUExecutionProvider"])
    try:
        from ai.animal_pose import AnimalPose
        pose_model = AnimalPose(progress)
    except Exception:  # Optional model; base TripoSR remains usable offline.
        pose_model = None
    results = []
    for i, item in enumerate(items):
        progress(f"참고 사진 AI 분석 중 · {i + 1}/{len(items)}")
        if i < len(previous) and previous[i].get("analysisVersion") == 7 and previous[i].get("imageUrl") == item.get("url") and previous[i].get("mask") and (args.input.parent / previous[i]["mask"]).is_file():
            results.append(previous[i])
            continue
        try:
            image = read_rgba(args.input.parent / item["rgba"])
            cutout = rembg.remove(image, session=session).convert("RGBA")
            context = encoder.analyse(cutout, context_image=image)
            context["caption"] = captioner.describe(image)
            if pose_model is not None:
                try:
                    context['animalPose'] = pose_model.predict(image, cutout)
                except Exception as error:
                    context['animalPoseError'] = str(error)
            if species.get("scientific") == "Felis catus":
                context["faceBox"] = cat_face_box(image)
            mask_file = args.input.parent / f"reference-{i}-mask.rgba"
            with mask_file.open("wb") as stream:
                stream.write(struct.pack("<4sII", b"FRG1", *cutout.size))
                stream.write(cutout.tobytes())
            results.append({"context": context, "mask": mask_file.name, "imageUrl": item.get("url"), "analysisVersion": 7,
                            "speciesCandidates": [asdict(c) for c in classifier.predict(image.convert("RGB"), limit=5)]})
        except (ValueError, OSError) as error:
            results.append({"error": str(error)})
    args.output.write_text(json.dumps(results), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["segment", "infer", "infer-batch", "classify", "analyse", "context", "depth"])
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resolution", type=int, choices=[192, 256, 320], default=256)
    args = parser.parse_args()
    try:
        {"segment": segment, "infer": infer, "infer-batch": infer, "classify": classify, "analyse": analyse, "context": context, "depth": depth}[args.mode](args)
    except Exception as error:
        progress(f"{type(error).__name__}: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
