# FleshRing - Unreal Engine Mesh Deformation Plugin

[English](#english) | [한국어](#korean)

---

# English

A GPU-accelerated mesh deformation plugin for Unreal Engine that simulates realistic flesh compression effects. Using Compute Shaders and Signed Distance Fields (SDF), it creates believable skin deformation when bands, straps, or tight clothing press against character meshes.

---

## Available on Fab

You can download this plugin directly from Fab Marketplace:
👉 [FleshRing on Fab](https://www.fab.com/ko/portal/listings/4aa9ceae-7c7d-4e70-b6fb-3d5f28dcf2f6/preview)

---

## Features

- **GPU-Accelerated Deformation** - All calculations run on GPU via Compute Shaders
- **SDF-Based Influence** - Accurate distance-based deformation using Signed Distance Fields
- **Tightness Effect** - Pulls mesh vertices toward Ring center for compression
- **Bulge Effect** - Creates realistic flesh displacement around compressed areas
- **Laplacian Smoothing** - Removes deformation artifacts with Taubin algorithm option
- **PBD Edge Constraints** - Maintains mesh volume during heavy compression
- **Layer Penetration Resolution** - Prevents clothing clipping through skin
- **Multiple Ring Support** - Unlimited Rings per component with independent settings
- **Dedicated Asset Editor** - 3D preview viewport with interactive gizmos
- **Blueprint Integration** - Full runtime manipulation support
- **Modular Character Support** - Leader Pose, Copy Pose, Skeletal Merging compatible

---

## Usage Example

### 1. Create FleshRing Asset
Right-click in Content Browser > FleshRing > FleshRing Asset

### 2. Configure Rings in Asset Editor
- Set target Skeletal Mesh
- Add Rings and attach to bones
- Adjust Tightness, Bulge, Smoothing parameters

### 3. Generate Bake (Required for Runtime)
Click "Generate Bake" button to create the deformed mesh

> **Important**: Without baking, deformation will NOT appear in-game!

### 4. Add Component to Character
Add FleshRing Component and assign the Asset

---

## Installation & Setup

### Requirements
- Unreal Engine 5.5 or later
- Windows (DirectX 11/12)
- GPU with Compute Shader support (SM 5.0+)

### Installation Steps
1. Download the FleshRing plugin package
2. Extract to: `YourProject/Plugins/FleshRingPlugin/`
3. Open your Unreal Engine project
4. Enable the plugin: `Edit > Plugins > FleshRing`
5. Restart the editor

---

## Basic Structure

```plaintext
FleshRingPlugin/
├── Source/
│   ├── FleshRingRuntime/    # Runtime module
│   └── FleshRingEditor/     # Editor module
├── Shaders/                 # HLSL Compute Shaders
│   ├── FleshRingTightnessCS.usf
│   ├── FleshRingBulgeCS.usf
│   ├── FleshRingLaplacianCS.usf
│   └── ...
├── Resources/
│   └── Icon128.png
└── FleshRingPlugin.uplugin
```

---

## License

MIT License - Free for personal and commercial use.

---

# Korean

GPU 기반 메시 변형 플러그인으로, 밴드나 타이트한 의류가 캐릭터 메시를 누를 때 발생하는 사실적인 살 압축 효과를 시뮬레이션합니다. Compute Shader와 SDF(Signed Distance Field)를 사용하여 실시간으로 자연스러운 피부 변형을 생성합니다.

---

## Fab에서 다운로드 가능

이 플러그인은 Fab 마켓에서 바로 다운로드할 수 있습니다.
👉 [Fab에서 FleshRing 보기](https://www.fab.com/ko/portal/listings/4aa9ceae-7c7d-4e70-b6fb-3d5f28dcf2f6/preview)

---

## 기능 요약

- **GPU 가속 변형** - 모든 계산이 Compute Shader를 통해 GPU에서 실행
- **SDF 기반 영향도** - Signed Distance Field를 사용한 정확한 거리 기반 변형
- **Tightness 효과** - 메시 버텍스를 Ring 중심으로 당겨 압축 표현
- **Bulge 효과** - 압축된 영역 주변으로 사실적인 살 밀림 생성
- **Laplacian 스무딩** - Taubin 알고리즘으로 변형 아티팩트 제거
- **PBD 엣지 제약** - 강한 압축에서도 메시 볼륨 유지
- **레이어 침투 해결** - 의류가 피부를 뚫는 현상 방지
- **다중 Ring 지원** - 컴포넌트당 무제한 Ring, 각각 독립 설정
- **전용 에셋 에디터** - 인터랙티브 기즈모가 있는 3D 프리뷰 뷰포트
- **블루프린트 통합** - 런타임 조작 완벽 지원
- **모듈러 캐릭터 지원** - Leader Pose, Copy Pose, Skeletal Merging 호환

---

## 사용 예시

### 1. FleshRing Asset 생성
콘텐츠 브라우저 우클릭 > FleshRing > FleshRing Asset

### 2. Asset Editor에서 Ring 설정
- 타겟 스켈레탈 메시 지정
- Ring 추가 및 본에 부착
- Tightness, Bulge, Smoothing 파라미터 조정

### 3. Bake 생성 (런타임 필수)
"Generate Bake" 버튼 클릭하여 변형된 메시 생성

> **중요**: Bake를 하지 않으면 게임에서 변형이 나타나지 않습니다!

### 4. 캐릭터에 컴포넌트 추가
FleshRing Component를 추가하고 Asset 할당

---

## 설치 및 설정

### 요구 사항
- Unreal Engine 5.5 이상
- Windows (DirectX 11/12)
- Compute Shader 지원 GPU (SM 5.0+)

### 설치 방법
1. FleshRing 플러그인 패키지 다운로드
2. 압축 해제: `YourProject/Plugins/FleshRingPlugin/`
3. 언리얼 엔진 프로젝트 열기
4. 플러그인 활성화: `Edit > Plugins > FleshRing`
5. 에디터 재시작

---

## 기본 구조

```plaintext
FleshRingPlugin/
├── Source/
│   ├── FleshRingRuntime/    # 런타임 모듈
│   └── FleshRingEditor/     # 에디터 모듈
├── Shaders/                 # HLSL Compute Shader
│   ├── FleshRingTightnessCS.usf
│   ├── FleshRingBulgeCS.usf
│   ├── FleshRingLaplacianCS.usf
│   └── ...
├── Resources/
│   └── Icon128.png
└── FleshRingPlugin.uplugin
```

---

## 라이선스

MIT License - 개인 및 상업적 사용 모두 무료
