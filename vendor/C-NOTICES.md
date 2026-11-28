# C 버전 외부 소스

## cgltf

- 원본: https://github.com/jkuhlmann/cgltf
- 버전: v1.15, 커밋 `360db1a95480fe102ae9c69b27c5d101167ff5ba`
- 파일: `cgltf/cgltf.h`, `cgltf/LICENSE`
- cgltf.h SHA-256: `e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec`
- MIT. 소스를 수정하지 않고 C17로 컴파일합니다. GLB 읽기에 사용합니다.

## cJSON

- 원본: https://github.com/DaveGamble/cJSON
- 버전: v1.7.19, 커밋 `c859b25da02955fef659d658b8f324b5cde87be3`
- 파일: `cjson/cJSON.c`, `cjson/cJSON.h`, `cjson/LICENSE`
- MIT. 소스를 수정하지 않고 C17로 컴파일합니다. API 응답과 메타데이터 처리에 사용합니다.

## 기존 AI 소스

`triposr/LICENSE`, `triposr/LICENSE-APACHE-2.0`와 기존 저작권 고지를 유지합니다. TripoSR 가중치 리비전은 `5b521936b01fbe1890f6f9baed0254ab6351c04a`입니다. 소스 기준 리비전은 `107cefdc244c39106fa830359024f6a2f1c78871`이며, 이 프로젝트에 이미 있던 CPU marching cubes 및 로컬 DINO 설정 호환 변경을 그대로 사용합니다. 이번 전환에서 사전학습 가중치를 수정하거나 새 학습을 수행하지 않았습니다.

BioCLIP, OpenAI CLIP ViT-B/32, rembg/U²-Net 및 PyTorch는 `ai/requirements.txt`와 모델 래퍼의 고정 리비전을 따릅니다.

메시 단순화·품질 검사·GLB/STL/STEP/3MF 쓰기는 C 구현입니다. macOS 화면과 미리보기는 Objective-C로 Cocoa/SceneKit에 연결합니다. HTTPS는 시스템 libcurl, 이미지 읽기는 ImageIO, 해시는 CommonCrypto를 사용합니다. 외부 C++ 단순화 라이브러리나 Blender를 실행하지 않습니다.

## BLIP 사진 설명 모델

- Salesforce/blip-image-captioning-base, 리비전 `82a37760796d32b1411fe092ab5d4e227313294b`.
- 원본: https://huggingface.co/Salesforce/blip-image-captioning-base
- BSD-3-Clause. Transformers의 모델 클래스만 사용하며 원격 Python 코드를 실행하지 않습니다. 자세 판별 보조 설명을 만들며 3D 추론은 TripoSR을 유지합니다.

## OpenCV 고양이 얼굴 검출

- `opencv-python-headless==4.11.0.86`, OpenCV 4.11, Apache-2.0 및 배포 패키지의 제3자 고지.
- 원본: https://github.com/opencv/opencv/tree/4.11.0
- 배포 패키지의 `haarcascade_frontalcatface_extended.xml`을 사용합니다. 원본·참고 사진에서 한 고양이 얼굴이 검출되면 얼굴 영역을 기록합니다. C가 영역을 대응시켜 원본 얼굴 색과 제한된 입체 표현을 적용합니다. 얼굴 복원이나 관절 검출 모델은 아닙니다.
- 해당 검출 데이터: Copyright (c) 2014–2016, Joseph Howse (Nummist Media Corporation Limited), XML 내부 BSD 계열 3조건 라이선스와 고지를 유지합니다.

## Depth Anything V2 Small

- 저자: Lihe Yang, Bingyi Kang, Zilong Huang, Zhen Zhao, Xiaogang Xu, Jiashi Feng, Hengshuang Zhao.
- 공식 소스: https://github.com/DepthAnything/Depth-Anything-V2
- Transformers 가중치: https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf
- 고정 리비전: `5426e4f0f36572d16453bbda7a8389317b1bef99`.
- Small 가중치와 공식 코드의 Apache-2.0 라이선스: `depth-anything-v2/LICENSE`. 더 큰 모델의 라이선스는 별개이며 이 앱은 Small만 사용합니다.
- 가중치를 변경하거나 재학습하지 않습니다. 기존 Transformers 모델 클래스로 로컬 상대 깊이만 추론합니다. 사진 윤곽·깊이 결합과 메시 작성은 `native/observed.c`의 C 구현입니다. 실측 깊이·해부학 복원 모델로 설명하지 않습니다.

## RTMPose AP-10K

- OpenMMLab MMPose / RTMPose-m AP-10K, 공식 공개 ONNX 모델. https://github.com/open-mmlab/mmpose/tree/main/projects/rtmpose
- MMPose 코드 라이선스: Apache-2.0, Copyright 2018–2020 Open-MMLab. 원문은 `rtmpose/LICENSE`에 보관합니다. 학습 데이터와 사전학습 모델의 이용 조건은 해당 원 출처를 따릅니다.
- 공식 패키지: `rtmpose-m_simcc-ap10k_pt-aic-coco_210e-256x256-7a041aa1_20230206.zip` (download.openmmlab.com).
- ONNX SHA-256: `1cfd1c86e0d9e5d5f95178bcd95ee9a4e8386a624cd3c57519f27ff58cac7f28`. 압축 내 모델 크기와 해시를 확인한 뒤 로컬 캐시에 저장합니다.
- `ai/animal_pose.py`는 기존 ONNX Runtime으로 눈·코·목·팔다리 17개 2D 위치만 추론합니다. 가중치를 수정하지 않습니다. 신뢰 점수는 확률이 아니며 가려진 관절도 추정치입니다. 머리 정렬, 자료 선별, 표면·다리 작성은 C에서 수행합니다. TripoSR은 기존 PyTorch 원본 모델로 유지합니다.

## GBIF 관찰 사진

- 공식 API: https://techdocs.gbif.org/en/openapi/images
- 사진마다 `references.json`에 관찰 페이지, 저작자, 미디어 라이선스와 원본 사진 URL을 보관합니다. 관찰 레코드의 데이터 라이선스를 사진 이용 허락으로 간주하지 않습니다.
- 한 연결에서 직렬 요청하며 CC0·CC BY·CC BY-SA 미디어만 수집합니다. 종이 일치해도 발자국이나 여러 개체가 보이는 사진은 입체 참고에서 제외합니다.
