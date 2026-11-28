"""Local BioCLIP inference using the published TreeOfLife-10M text embeddings."""

from ai.device import inference_device

import json
from dataclasses import dataclass
import re

from PIL import Image, ImageOps
from model_cache import cached_asset


MODEL = ("imageomics/bioclip", "model", "ce901ab3c6a913f9e9ef94ce6d27761069f4f01c")
DATA = ("imageomics/TreeOfLife-10M", "dataset", "91debffb7146c32c89d76feb1eb575b555e2ecc7")
BINOMIAL = re.compile(r"[A-Z][a-z-]+ [a-z][a-z-]+")


@dataclass(frozen=True)
class Candidate:
    scientific: str
    common: str
    score: float


def open_photo(path, *, preserve_alpha=False):
    with Image.open(path) as image:
        if image.format not in {"JPEG", "PNG", "WEBP"}:
            raise ValueError("JPG, PNG, WEBP 사진을 선택해 주세요.")
        if image.width * image.height > 50_000_000:
            raise ValueError("사진을 5,000만 화소 이하로 줄여 주세요.")
        size = image.size
        image.thumbnail((2048, 2048), Image.Resampling.LANCZOS)
        upright = ImageOps.exif_transpose(image)
        alpha = "A" in upright.getbands() or "transparency" in upright.info
        return upright.convert("RGBA" if preserve_alpha and alpha else "RGB"), size


class AnimalClassifier:
    def __init__(self, progress):
        import numpy as np
        import open_clip
        import torch

        config = cached_asset(MODEL, "open_clip_config.json", progress)
        cached_asset(MODEL, "open_clip_pytorch_model.bin", progress)
        names_path = cached_asset(DATA, "embeddings/txt_emb_species.json", progress)
        embeddings_path = cached_asset(DATA, "embeddings/txt_emb_species.npy", progress)
        progress("BioCLIP을 메모리에 불러오는 중")
        self.device = inference_device()
        if self.device == "cpu":
            torch.set_num_threads(min(torch.get_num_threads(), 8))
        model = open_clip.create_model_from_pretrained(
            f"local-dir:{config.parent}", device=self.device, return_transform=False
        ).eval()
        self.visual = model.visual
        self.scale = model.logit_scale.detach().exp()

        labels = json.loads(names_path.read_bytes())
        vectors = np.load(embeddings_path, mmap_mode="c", allow_pickle=False)
        if vectors.shape[1] != len(labels):
            raise ValueError("종 목록과 임베딩 크기가 일치하지 않습니다.")
        indices, species_ids, species = [], [], {}
        self.names = []
        classes, class_ids = {}, []
        for index, (taxonomy, common) in enumerate(labels):
            if taxonomy[0] != "Animalia":
                continue
            scientific = " ".join(taxonomy[-2:])
            if not BINOMIAL.fullmatch(scientific):
                continue
            if scientific not in species:
                species[scientific] = len(self.names)
                self.names.append((scientific, common))
                class_name = taxonomy[2]
                classes.setdefault(class_name, len(classes))
                class_ids.append(classes[class_name])
            indices.append(index)
            species_ids.append(species[scientific])
        # Copy just the animal columns into a compact matrix, releasing the file mapping.
        self.embeddings = torch.from_numpy(vectors[:, indices].T).to(self.device)
        self.species_ids = torch.tensor(species_ids, dtype=torch.long)
        self.class_ids = torch.tensor(class_ids, dtype=torch.long)
        self.class_count = len(classes)

    def predict(self, image, limit=3):
        import torch
        from bioclip.predict import preprocess_img
        from torch.nn import functional as F

        with torch.inference_mode():
            pixels = preprocess_img(image).unsqueeze(0).to(self.device)
            features = F.normalize(self.visual(pixels), dim=-1)
            logits = self.scale * (features @ self.embeddings.T)
            probabilities = logits[0].softmax(dim=0).cpu()
            # Some published labels share a binomial. Display each species once.
            grouped = torch.zeros(len(self.names)).scatter_add_(0, self.species_ids, probabilities)
            # Wildlife datasets also label parasites visible on a host. If the image
            # strongly supports one animal class, keep the choices in that class.
            # This prevents a mite label from displacing another mammal in a fox photo.
            class_scores = torch.zeros(self.class_count).scatter_add_(0, self.class_ids, grouped)
            dominant = class_scores.argmax()
            if class_scores[dominant] >= 0.55:
                grouped = grouped.masked_fill(self.class_ids != dominant, 0)
            scores, ids = grouped.topk(min(limit, len(self.names)))
        return [Candidate(*self.names[i], score) for i, score in zip(ids.tolist(), scores.tolist())]
