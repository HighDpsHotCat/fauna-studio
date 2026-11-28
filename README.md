# AAAnimal Studio · macOS

동물 사진을 TripoSR로 3D 모델로 만들고, C로 형태·색상·표면을 보완하는 macOS 개발용 프로젝트입니다. 현재까지 작업한 C 버전을 Mac용으로 분리했습니다. 설치 패키지(DMG/PKG)는 포함하지 않습니다.

## Mac에서 시작하기

macOS 13 이상, Python 3.11, Xcode Command Line Tools, CMake가 필요합니다. Apple Silicon과 Intel을 각각 빌드합니다. 메모리는 16 GB 이상을 권장하며, 최초 AI 실행에는 모델 다운로드용 인터넷과 수 GB의 여유 공간이 필요합니다.

터미널에서 개발 도구를 준비합니다. `brew` 명령은 [Homebrew](https://brew.sh)가 설치된 환경을 기준으로 합니다.

```bash
xcode-select --install
brew install cmake python@3.11
git clone https://github.com/HighDpsHotCat/aaanimal-studio.git AAAnimal-Studio-macOS
cd AAAnimal-Studio-macOS
bash setup_macos.sh
./run.command
```

Xcode 도구가 이미 설치되어 있으면 첫 명령은 생략합니다. GitHub에서 ZIP으로 받았다면 `bash run.command`로 실행해도 됩니다. `setup_macos.sh`는 프로젝트 안에 `.venv`를 만들고 AI 라이브러리를 설치한 뒤 앱을 빌드합니다. 모델 가중치는 필요한 기능을 처음 실행할 때 내려받습니다.

AI 없이 화면·기존 GLB 열기·다른 형식으로 저장만 확인할 때는 다음 두 명령으로 시작할 수 있습니다.

```bash
bash build_macos.sh
bash run.command
```

빌드 결과는 `build/AAAnimal Studio.app`, C 명령행 프로그램은 `build/aaanimal-cli`입니다. 앱은 프로젝트의 `ai/`, `vendor/`, `.venv/`를 사용하므로 개발 중에는 프로젝트 폴더에서 실행하세요. `.app` 하나만 다른 Mac으로 옮기는 배포 방식은 아직 지원하지 않습니다.

## 사용

1. 사진을 첨부하거나 예제 사진을 선택합니다.
2. 목표 폴리곤 수와 품질을 정합니다. 기본 목표는 900면입니다. 귀·주둥이·발·윤곽 보호에 필요한 경우 목표보다 많은 삼각형을 유지합니다.
3. GLB, STL, STEP, 3MF 중 필요한 형식을 체크하고 **3D 생성하기**를 누릅니다.
4. 이미 만들어진 모델은 **GLB 열기… → 형식 체크 → 선택 형식으로 저장**으로 재생성 없이 변환합니다.

결과는 `~/Documents/AAAnimal Studio/날짜-고유번호/`에 저장합니다. 원본을 덮어쓰지 않고 매번 새 폴더에 저장합니다. **저장 폴더 열기**로 확인할 수 있습니다. 작업 취소 시 진단을 위해 `.partial-…` 작업 폴더를 남깁니다.

미리보기는 마우스 드래그로 회전하고 스크롤로 확대·축소합니다. 바닥 격자는 항상 표시합니다. 품질과 저장 형식의 자세한 설명은 각각 `?` 버튼에 있습니다.

| 형식 | 용도 |
|---|---|
| GLB | 색상·법선이 포함된 모델, 다시 열기 |
| STL | 색상 없는 삼각형 메시, 3D 인쇄 |
| STEP | 삼각면 기반 CAD 형상. 설계 이력이나 매끈한 NURBS 모델이 아닙니다 |
| 3MF | 인쇄 크기와 색상 포함. 색상 사용 여부는 슬라이서·프린터에 따라 다릅니다 |

STL·STEP·3MF의 긴 변 길이는 mm로 지정합니다. 실제 프린터용 G-code는 별도의 슬라이서에서 만듭니다. STEP 솔리드에 부적합한 메시가 들어오면 오류로 알리며, 조용히 잘못된 솔리드로 저장하지 않습니다.

## 눈 없는 로우폴리 스타일

몸체와 얼굴 모두 평평한 삼각형 면으로 표현합니다. 귀·이마·콧대·코·입·턱의 큰 입체를 유지하며, 눈의 색 무늬는 주변 털색으로 지웁니다. 미세한 눈·입 표현을 위한 면 추가나 사진 색 재투영은 하지 않습니다. 각 삼각형에 하나의 법선과 하나의 색을 저장하므로 GLB를 다른 뷰어에서 열어도 각진 형태가 유지됩니다.

얼굴 위치를 확실히 찾지 못하면 눈 무늬가 남지 않도록 단색 털색을 사용하고 `report.json`의 `uniformCoatFallback`에 기록합니다. 모델 가중치와 TripoSR 추론은 바꾸지 않습니다. 최종 파일은 `model.glb`이고, `original.glb`·`completed.glb`는 스타일 적용 전 중간 결과입니다. 기존 GLB 열기·재저장은 원래 파일의 모습을 그대로 유지합니다.

이미 만든 모델에 새 스타일을 적용하는 개발 명령(결과 폴더의 `input-analysis.json`과 원본 배경 분리 사진 필요):

```bash
build/aaanimal-cli --form model.glb --context RESULT_DIRECTORY --photo cutout.rgba --target 900 NEW_OUTPUT.glb
```

## 구성과 발표 설명

**“TripoSR이 기본 형상을 추론하고, C 프로그램이 사진 전처리·인터넷 참고 자료 선택·형태 보완·메시 처리·파일 저장을 수행한다.”**

| 부분 | 구현 |
|---|---|
| 처리 흐름·전처리·자료 수집·메시·4개 형식 저장 | `native/`의 C17 |
| Mac 창·파일 선택·3D 화면 | `macos/App.m`의 Objective-C Cocoa/SceneKit 연결 코드 |
| 사전학습 AI 실행 | Python/PyTorch 모델 어댑터. TripoSR 원본 가중치 유지 |
| C와 AI의 전달 | RGBA·깊이·메시의 단순 이진 파일, 별도 프로세스 |

TripoSR을 ONNX로 바꾸지 않으며 양자화하거나 재학습하지 않습니다. 관절 위치 추론용 RTMPose만 공개 ONNX 모델을 ONNX Runtime으로 실행합니다. 이는 TripoSR 변환 모델이 아닙니다.

기존 기능인 종 자동 판별, 유사 자세 참고 검색, 원본 사진의 윤곽·상대 깊이 보완, 머리와 다리의 참고 형상 결합, 색상 보호를 포함합니다. 참고 자료는 Wikimedia Commons, iNaturalist, GBIF에서 자동 수집합니다. 저작자·라이선스·출처는 결과의 `references.json`에 남깁니다. 다른 개체의 참고 사진은 형태의 단서이며 실제 가려진 부위를 측정한 자료는 아닙니다. 유튜브 영상 수집 기능은 없습니다.

## Mac의 AI 실행

기본은 두 종류의 Mac에서 동일한 FP32 **CPU 추론**입니다. Apple Silicon은 PyTorch 2.9.1, Intel은 배포 휠이 제공되는 PyTorch 2.2.2를 사용하도록 의존성을 나눴습니다. 두 환경 모두 Python 3.11과 NumPy 1.26.4를 사용합니다. 기준 배포 목록: [PyTorch 2.2.2](https://pypi.org/project/torch/2.2.2/), [ONNX Runtime 1.19.2](https://pypi.org/project/onnxruntime/1.19.2/).

Apple Silicon에서 MPS 가속을 시험하려면 다음과 같이 실행합니다. 이 경로는 기기별 추가 확인이 필요하며, 문제가 생기면 기본 CPU 실행을 사용하세요.

```bash
AAANIMAL_DEVICE=mps bash run.command
```

CPU 추론은 오래 걸릴 수 있습니다. 품질 320에서 메모리가 부족하면 256을 선택하세요. 앱이 임의로 해상도를 낮추지는 않습니다.

## 확인 범위와 남은 작업

GitHub Actions는 Apple Silicon·Intel macOS에서 앱과 C CLI를 빌드하고 네이티브 창 실행·예제 표시, C 회귀 테스트, 예제 GLB 읽기, STL·STEP·3MF 변환, AI 의존성 설치·가져오기를 검사합니다. 실행 기록은 [Actions](https://github.com/HighDpsHotCat/aaanimal-studio/actions)에 있습니다. 모델 가중치를 내려받는 전체 사진→3D 생성 및 실제 Mac 화면의 사용성은 이 자동 검사에 포함하지 않습니다.

현재 모델링 보완은 개발 중입니다. 한 장의 사진만으로 눈·코·입의 측면 깊이, 겹친 발가락, 가려진 다리를 정확히 복원할 수 없으며 참고 사진을 결합해도 목 접합부와 측면 색상 늘어짐 등이 남을 수 있습니다. 이번 macOS 분리는 기존 형상 처리 코드를 옮긴 작업이며 이런 품질 문제가 모두 해결됐다는 의미는 아닙니다.

## 개발 명령

```bash
build/aaanimal-cli --inspect examples/red-fox-lowpoly-3000.glb
build/aaanimal-cli --export-stl examples/red-fox-lowpoly-3000.glb /tmp/animal.stl --size-mm 100
build/aaanimal-cli --export-step examples/red-fox-lowpoly-3000.glb /tmp/animal.step
build/aaanimal-cli --export-3mf examples/red-fox-lowpoly-3000.glb /tmp/animal.3mf
build/aaanimal-cli --generate --input assets/red-fox.jpg --target 900 --resolution 320 --formats glb,stl,step,3mf
ctest --test-dir build --output-on-failure
.venv/bin/python tools/check_runtime.py
```

CLI의 출력 파일·지정 출력 폴더는 아직 존재하지 않는 이름을 사용합니다. `--no-shape`는 참고 자료를 통한 형태 보완을 끄며, 종 판별·색상 참고 수집은 유지합니다.

## 외부 소스·예제 출처

외부 라이브러리와 모델의 고지는 [vendor/C-NOTICES.md](vendor/C-NOTICES.md)에 있습니다. TripoSR 및 관련 공개 모델 가중치는 저장소에 포함하지 않고 지정한 리비전을 받아 사용합니다.

- 입력 예제: Uoaei1, [Vulpes vulpes Mallnitz 01.jpg](https://commons.wikimedia.org/wiki/File:Vulpes_vulpes_Mallnitz_01.jpg), [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
- `examples/red-fox-lowpoly-3000.glb`: 위 예제를 바탕으로 생성·가공한 미리보기 예제. 같은 CC BY-SA 4.0 조건으로 제공합니다.

개인 입력 사진, 생성 작업 기록, 가중치, 가상환경, 플랫폼별 빌드 결과는 버전 관리에 포함하지 않습니다.

## 더 큰 면의 로우폴리

현재 `faceted-form-no-eyes-v2`의 기본 목표는 900면입니다. 600~1,200면에서 큰 평면이 강조됩니다. 여섯 방향의 윤곽 IoU 0.94를 유지하면서 깊이 RMSE 한도를 0.05로 조정해 곡면을 촘촘하게 복원하던 면 증가를 줄였습니다. 눈 없는 표현과 면별 평면 법선은 유지하며, GLB뿐 아니라 STL·STEP·3MF에도 실제로 줄인 형상을 저장합니다. 사진 추론을 다시 실행하지 않고 코기 기존 형상을 처리한 결과는 v1 2,700면 → v2 1,350면입니다(요청 900, 최소 윤곽 IoU 0.966887 / 최대 깊이 RMSE 0.045720). 이 지표는 원래 메시 대비 보존도입니다.
