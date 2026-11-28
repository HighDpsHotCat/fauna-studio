"""Keep FP32 inference portable; MPS is an explicit development option."""
import os


def inference_device():
    import torch

    requested = os.environ.get("AAANIMAL_DEVICE", "cpu").lower()
    if requested == "cpu":
        return "cpu"
    if requested == "mps" and torch.backends.mps.is_available():
        return "mps"
    raise RuntimeError("AAANIMAL_DEVICE는 cpu 또는 사용 가능한 mps로 설정하세요.")
