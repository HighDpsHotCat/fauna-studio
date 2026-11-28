"""Offline dependency check; does not download or initialize model weights."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(root), str(root / "vendor/triposr")]
import torch
import torchvision
import numpy as np
import onnxruntime
import rembg
import bioclip
from transformers import CLIPModel, BlipForConditionalGeneration, AutoModelForDepthEstimation
from tsr.system import TSR
from ai.animal_pose import AnimalPose
from ai.device import inference_device

assert np.array([1], dtype=np.float32).dtype == torch.ones(1).numpy().dtype
assert "CPUExecutionProvider" in onnxruntime.get_available_providers()
print(f"AI imports OK · torch {torch.__version__} · {inference_device()}")
