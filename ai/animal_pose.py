"""Fixed pretrained AP-10K model adapter; C owns reference matching/geometry."""
from pathlib import Path
import hashlib
import io
import os
import urllib.request
import zipfile

URL = ('https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/'
       'rtmpose-m_simcc-ap10k_pt-aic-coco_210e-256x256-7a041aa1_20230206.zip')
SHA256 = '1cfd1c86e0d9e5d5f95178bcd95ee9a4e8386a624cd3c57519f27ff58cac7f28'
NAMES = ['left_eye', 'right_eye', 'nose', 'neck', 'tail_root',
         'left_shoulder', 'left_elbow', 'left_front_paw',
         'right_shoulder', 'right_elbow', 'right_front_paw',
         'left_hip', 'left_knee', 'left_back_paw', 'right_hip', 'right_knee', 'right_back_paw']


class AnimalPose:
    def __init__(self, progress=print):
        import onnxruntime as ort
        cache = Path.home() / 'Library/Caches/AAAnimalStudio/models'
        cache.mkdir(parents=True, exist_ok=True)
        path = cache / 'rtmpose-ap10k-1cfd1c86.onnx'
        valid = path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == SHA256
        if not valid:
            progress('최초 다운로드 중 · 동물 눈·코·관절 위치 모델')
            request = urllib.request.Request(URL, headers={'User-Agent': 'AAAnimalStudio/1.0'})
            with urllib.request.urlopen(request, timeout=45) as response:
                archive = response.read(80_000_001)
            if len(archive) > 80_000_000:
                raise ValueError('Animal pose download exceeds expected size')
            with zipfile.ZipFile(io.BytesIO(archive)) as package:
                names = [n for n in package.namelist() if n.endswith('/end2end.onnx')]
                if len(names) != 1 or package.getinfo(names[0]).file_size != 54478120:
                    raise ValueError('Unexpected animal pose package')
                data = package.read(names[0])
            if hashlib.sha256(data).hexdigest() != SHA256:
                raise ValueError('Animal pose model checksum mismatch')
            temporary = path.with_suffix(f'.{os.getpid()}.tmp')
            temporary.write_bytes(data)
            temporary.replace(path)
        options = ort.SessionOptions()
        options.intra_op_num_threads = 4
        self.session = ort.InferenceSession(str(path), sess_options=options, providers=['CPUExecutionProvider'])

    def predict(self, image, cutout):
        import cv2
        import numpy as np
        rgb = np.asarray(image.convert('RGB'))
        alpha = np.asarray(cutout.getchannel('A'))
        ys, xs = np.where(alpha > 128)
        if len(xs) < 100:
            return None
        center = np.array([(xs.min()+xs.max())/2, (ys.min()+ys.max())/2], dtype=np.float32)
        side = max(xs.max()-xs.min()+1, ys.max()-ys.min()+1)*1.25
        scale = np.array([side, side], dtype=np.float32)
        matrix = np.array([[256/side, 0, 128-center[0]*256/side],
                           [0, 256/side, 128-center[1]*256/side]], dtype=np.float32)
        crop = cv2.warpAffine(rgb, matrix, (256, 256), flags=cv2.INTER_LINEAR)
        tensor = ((crop.astype(np.float32)-[123.675,116.28,103.53])/[58.395,57.12,57.375]).astype(np.float32)
        x, y = self.session.run(None, {self.session.get_inputs()[0].name: tensor.transpose(2,0,1)[None]})
        coordinates = np.stack((x.argmax(-1), y.argmax(-1)), axis=-1)[0]/2
        coordinates = coordinates/256*scale+center-scale/2
        confidence = np.minimum(x.max(-1), y.max(-1))[0]
        points = []
        for name, (px,py), score in zip(NAMES, coordinates, confidence):
            inside = 0 <= px < image.width and 0 <= py < image.height
            mask_support = inside and alpha[min(int(py),alpha.shape[0]-1),min(int(px),alpha.shape[1]-1)] > 64
            points.append({'name':name, 'x':float(px/image.width), 'y':float(py/image.height),
                           'score':float(max(0, score)), 'maskSupport':bool(mask_support)})
        return {'model':'RTMPose-m AP-10K', 'sha256':SHA256, 'points':points,
                'meaning':'2D estimated landmarks; confidence is not proof of visible or correct anatomy'}
