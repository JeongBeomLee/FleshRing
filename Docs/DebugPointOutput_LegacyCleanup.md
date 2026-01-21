# 디버그 포인트 출력 통합 및 레거시 코드 정리

## 개요

TightnessCS와 BulgeCS에서 개별적으로 수행하던 디버그 포인트 출력을
**모든 변형 패스 완료 후** 새로운 **DebugPointOutputCS**에서 통합 출력하도록 변경.

### 변경 이유

기존에는 TightnessCS(1단계)와 BulgeCS(2단계)에서 각각 디버그 포인트를 출력했으나,
이후 스무딩, PBD, Laplacian 등 추가 변형 패스가 적용되면서
**최종 메시 표면과 디버그 포인트 위치가 불일치**하는 문제가 있었음.

```
[기존 문제]
1. TightnessCS      ← Tightness 디버그 포인트 출력 (여기!)
2. BulgeCS          ← Bulge 디버그 포인트 출력 (여기!)
3. HeatPropagationCS
4. PBD Edge Constraint
5. LaplacianCS      ← 스무딩 적용 (디버그 포인트에 미반영)
6. LayerPenetrationCS
7. NormalRecomputeCS
8. TangentRecomputeCS

결과: 디버그 포인트가 메시 표면에서 떠있거나 안쪽에 위치함
```

### 변경 후 파이프라인

```
1. TightnessCS        (디버그 포인트 출력 제거)
2. BulgeCS            (디버그 포인트 출력 제거)
3. HeatPropagationCS
4. PBD Edge Constraint
5. LaplacianCS
6. LayerPenetrationCS
7. NormalRecomputeCS
8. TangentRecomputeCS
9. DebugPointOutputCS  ★ 최종 위치로 Tightness/Bulge 통합 디버그 포인트 출력

결과: 디버그 포인트가 최종 스무딩된 메시 표면에 정확히 위치함
```

---

## 신규 파일

### 1. FleshRingDebugPointOutputCS.usf (신규)

모든 변형 패스 완료 후 최종 위치에서 디버그 포인트를 출력하는 컴퓨트 셰이더.

**주요 특징:**
- `FinalPositions` 버퍼에서 최종 변형된 버텍스 위치 읽기
- `Influences` 버퍼에서 GPU에서 계산된 Influence 값 읽기
- `LocalToWorld` 행렬로 월드 좌표 변환
- 다중 Ring 지원을 위한 `BaseOffset`, `InfluenceBaseOffset` 파라미터

```hlsl
// 핵심 구조
struct FDebugPoint
{
    float3 WorldPosition;   // 월드 공간 위치
    float Influence;        // Influence 값 (0~1)
    uint RingIndex;         // Ring 인덱스 (가시성 필터링용)
    uint Padding;           // 정렬 패딩
};

// Input
Buffer<float> FinalPositions;           // 최종 변형된 위치 (모든 CS 패스 완료 후)
StructuredBuffer<uint> VertexIndices;   // 출력할 버텍스 인덱스
Buffer<float> Influences;               // GPU 계산된 Influence 값

// Output
RWStructuredBuffer<FDebugPoint> DebugPointBuffer;

// Parameters
uint NumVertices;           // 처리할 버텍스 수
uint NumTotalVertices;      // 전체 메시 버텍스 수 (범위 검사용)
uint RingIndex;             // Ring 인덱스
uint BaseOffset;            // 출력 버퍼 오프셋 (다중 Ring 지원)
uint InfluenceBaseOffset;   // Influence 버퍼 오프셋 (다중 Ring 지원)
float4x4 LocalToWorld;      // 로컬 → 월드 변환 행렬
```

---

### 2. FleshRingDebugPointOutputShader.h (신규)

셰이더 클래스 및 디스패치 파라미터 정의.

```cpp
class FFleshRingDebugPointOutputCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingDebugPointOutputCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingDebugPointOutputCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Input Buffers
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, FinalPositions)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VertexIndices)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, Influences)

        // Output Buffer
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FFleshRingDebugPoint>, DebugPointBuffer)

        // Parameters
        SHADER_PARAMETER(uint32, NumVertices)
        SHADER_PARAMETER(uint32, NumTotalVertices)
        SHADER_PARAMETER(uint32, RingIndex)
        SHADER_PARAMETER(uint32, BaseOffset)
        SHADER_PARAMETER(uint32, InfluenceBaseOffset)
        SHADER_PARAMETER(FMatrix44f, LocalToWorld)
    END_SHADER_PARAMETER_STRUCT()
};

struct FDebugPointOutputDispatchParams
{
    uint32 NumVertices = 0;
    uint32 NumTotalVertices = 0;
    uint32 RingIndex = 0;
    uint32 BaseOffset = 0;
    uint32 InfluenceBaseOffset = 0;
    FMatrix44f LocalToWorld = FMatrix44f::Identity;
};
```

---

### 3. FleshRingDebugPointOutputShader.cpp (신규)

셰이더 디스패치 함수 구현.

```cpp
void DispatchFleshRingDebugPointOutputCS(
    FRDGBuilder& GraphBuilder,
    const FDebugPointOutputDispatchParams& Params,
    FRDGBufferRef FinalPositionsBuffer,
    FRDGBufferRef VertexIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef DebugPointBuffer);
```

---

## 수정된 파일

### 4. FleshRingComputeWorker.cpp

**추가된 코드 - Debug Point Output Pass (약 100줄):**

```cpp
// ===== Debug Point Output Pass (모든 CS 완료 후 최종 변형 위치 기반) =====
// TightnessCS, BulgeCS에서 출력하면 중간 위치가 출력되므로,
// 모든 변형 패스(스무딩 포함) 완료 후 여기서 통합 출력
if (WorkItem.RingDispatchDataPtr.IsValid())
{
    uint32 DebugCumulativeOffset = 0;

    // Tightness 디버그 포인트 출력 (최종 위치)
    if (WorkItem.bOutputDebugPoints && DebugPointBuffer && DebugInfluencesBuffer)
    {
        for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
        {
            const auto& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];
            uint32 NumAffectedVertices = DispatchData.NumAffectedVertices;

            if (NumAffectedVertices > 0)
            {
                FDebugPointOutputDispatchParams DebugParams;
                DebugParams.NumVertices = NumAffectedVertices;
                DebugParams.NumTotalVertices = ActualNumVertices;
                DebugParams.RingIndex = DispatchData.OriginalRingIndex;
                DebugParams.BaseOffset = DebugCumulativeOffset;
                DebugParams.InfluenceBaseOffset = DebugCumulativeOffset;
                DebugParams.LocalToWorld = WorkItem.LocalToWorldMatrix;

                DispatchFleshRingDebugPointOutputCS(
                    GraphBuilder,
                    DebugParams,
                    TightenedBindPoseBuffer,  // 최종 변형된 위치
                    DebugIndicesBuffer,
                    DebugInfluencesBuffer,    // GPU에서 계산된 Influence
                    DebugPointBuffer
                );

                DebugCumulativeOffset += NumAffectedVertices;
            }
        }
    }

    // Bulge 디버그 포인트 출력 (최종 위치)
    if (WorkItem.bOutputDebugBulgePoints && DebugBulgePointBuffer)
    {
        // ... 유사한 구조로 Bulge 디버그 포인트 출력
    }
}
```

**제거된 코드:**
- TightnessCS 호출에서 `nullptr` (DebugPointBuffer) 인자 제거
- BulgeCS 호출에서 `nullptr` (DebugBulgePointBuffer) 인자 제거
- `BulgeParams.bOutputDebugBulgePoints = false;` 설정 제거

---

### 5. FleshRingTightnessCS.usf

**제거된 코드:**
- `FDebugPoint` 구조체 정의
- `RWStructuredBuffer<FDebugPoint> DebugPointBuffer` 선언
- `uint bOutputDebugPoints` 파라미터
- `float4x4 LocalToWorld` 파라미터
- 디버그 포인트 출력 블록 (`if (bOutputDebugPoints != 0) { ... }`)

**유지된 코드:**
- `RWBuffer<float> DebugInfluences` - Influence 히트맵 출력용 (DebugPointOutputCS에서 사용)
- `uint bOutputDebugInfluences` - Influence 출력 플래그
- `uint DebugPointBaseOffset` - DebugInfluences 버퍼 오프셋용

---

### 6. FleshRingTightnessShader.h

**FParameters 구조체에서 제거:**
```cpp
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FFleshRingDebugPoint>, DebugPointBuffer)
SHADER_PARAMETER(uint32, bOutputDebugPoints)
SHADER_PARAMETER(FMatrix44f, LocalToWorld)
```

**FTightnessDispatchParams 구조체에서 제거:**
```cpp
bool bOutputDebugPoints = false;
FMatrix44f LocalToWorld = FMatrix44f::Identity;
```

**함수 시그니처 변경:**
```cpp
// Before
void DispatchFleshRingTightnessCS(..., FRDGBufferRef DebugInfluencesBuffer, FRDGBufferRef DebugPointBuffer);

// After
void DispatchFleshRingTightnessCS(..., FRDGBufferRef DebugInfluencesBuffer = nullptr);
```

---

### 7. FleshRingTightnessShader.cpp

**제거된 코드:**
- `DebugPointBuffer` UAV 바인딩 코드 (약 40줄)
- `bOutputDebugPoints`, `LocalToWorld` 파라미터 바인딩
- Deprecated 함수에서 더미 `DebugPointBuffer` 생성 코드

**`DispatchFleshRingTightnessCS_WithReadback` 함수:**
- `DebugPointBuffer` 파라미터 제거

---

### 8. FleshRingBulgeCS.usf

**제거된 코드:**
- `FDebugPoint` 구조체 정의
- `RWStructuredBuffer<FDebugPoint> DebugBulgePointBuffer` 선언
- `uint bOutputDebugBulgePoints` 파라미터
- `uint DebugBulgePointBaseOffset` 파라미터
- `float4x4 BulgeLocalToWorld` 파라미터
- `WriteInvalidDebugPoint()` 함수 정의
- 모든 `WriteInvalidDebugPoint(ThreadIndex)` 호출 (6개소)
- 디버그 포인트 출력 블록

---

### 9. FleshRingBulgeShader.h

**FParameters 구조체에서 제거:**
```cpp
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FFleshRingDebugPoint>, DebugBulgePointBuffer)
SHADER_PARAMETER(uint32, bOutputDebugBulgePoints)
SHADER_PARAMETER(uint32, DebugBulgePointBaseOffset)
SHADER_PARAMETER(FMatrix44f, BulgeLocalToWorld)
```

**FBulgeDispatchParams 구조체에서 제거:**
```cpp
bool bOutputDebugBulgePoints = false;
uint32 DebugBulgePointBaseOffset = 0;
FMatrix44f BulgeLocalToWorld = FMatrix44f::Identity;
```

**함수 시그니처 변경:**
```cpp
// Before
void DispatchFleshRingBulgeCS(..., FRDGTextureRef SDFTexture, FRDGBufferRef DebugBulgePointBuffer);

// After
void DispatchFleshRingBulgeCS(..., FRDGTextureRef SDFTexture);
```

---

### 10. FleshRingBulgeShader.cpp

**제거된 코드:**
- Debug Point Output 파라미터 바인딩 코드 (약 16줄)
- `DebugBulgePointBuffer` UAV 바인딩 및 더미 버퍼 생성

**함수 시그니처 변경:**
- `DispatchFleshRingBulgeCS`: `DebugBulgePointBuffer` 파라미터 제거
- `DispatchFleshRingBulgeCS_WithReadback`: `DebugBulgePointBuffer` 파라미터 제거

---

## 파일 변경 요약

| 파일 | 변경 유형 | 설명 |
|------|----------|------|
| `FleshRingDebugPointOutputCS.usf` | **신규** | 통합 디버그 포인트 출력 셰이더 |
| `FleshRingDebugPointOutputShader.h` | **신규** | 셰이더 클래스 및 파라미터 정의 |
| `FleshRingDebugPointOutputShader.cpp` | **신규** | 디스패치 함수 구현 |
| `FleshRingComputeWorker.cpp` | 수정 | Debug Point Output Pass 추가, 레거시 호출 정리 |
| `FleshRingTightnessCS.usf` | 수정 | 디버그 포인트 출력 코드 제거 |
| `FleshRingTightnessShader.h` | 수정 | 디버그 포인트 파라미터 제거 |
| `FleshRingTightnessShader.cpp` | 수정 | 디버그 포인트 바인딩 코드 제거 |
| `FleshRingBulgeCS.usf` | 수정 | 디버그 포인트 출력 코드 및 WriteInvalidDebugPoint 제거 |
| `FleshRingBulgeShader.h` | 수정 | 디버그 포인트 파라미터 제거 |
| `FleshRingBulgeShader.cpp` | 수정 | 디버그 포인트 바인딩 코드 제거 |

---

## 유지되는 디버그 기능

### WorkItem 플래그 (유지)

```cpp
bool bOutputDebugPoints = false;       // Tightness 디버그 포인트 활성화
bool bOutputDebugBulgePoints = false;  // Bulge 디버그 포인트 활성화
```

### DebugInfluences 버퍼 (유지)

TightnessCS에서 GPU로 계산된 Influence 값을 `DebugInfluences` 버퍼에 출력.
DebugPointOutputCS에서 이 값을 읽어 디버그 포인트의 색상/크기를 결정.

### GPU 렌더링 파이프라인 (유지)

- `DebugPointBuffer` → `FleshRingDebugPointVS/PS` → 화면 렌더링
- `DebugBulgePointBuffer` → `FleshRingDebugPointVS/PS` → 화면 렌더링

---

## 테스트 방법

1. 에디터에서 FleshRing 컴포넌트가 있는 레벨 열기
2. FleshRing 컴포넌트 선택 → Details 패널
3. Debug 섹션에서 "Show Affected Vertices" 또는 "Show Bulge Heatmap" 활성화
4. 디버그 포인트가 **최종 스무딩된 메시 표면**에 정확히 위치하는지 확인
5. Smoothing Iterations, Laplacian Lambda 등 변경 시 디버그 포인트도 함께 이동하는지 확인

---

## 관련 문서

- `Phase3_BulgeGPUDebugRendering.md` - 이전 BulgeCS 디버그 출력 구조 (참조용)
