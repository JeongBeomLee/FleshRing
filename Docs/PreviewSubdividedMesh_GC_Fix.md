# PreviewSubdividedMesh GC 문제 수정

## 날짜
2026-01-19

## 문제 요약

디테일 패널에서 Ring 배열 조작(추가/삭제/복제/드래그) 시 `/Engine/Transient.SK_*_Preview_*` 메시들이 GC되지 않고 누적되는 문제.

### 증상
- `obj list class=skeletalmesh` 명령 실행 시 `SkeletalMesh_1`, `SkeletalMesh_2` 등이 계속 쌓임
- `obj refs name=/Engine/Transient.SkeletalMesh_1` 실행 시 `TransBuffer`가 참조를 보유
- 특히 **같은 본에 링을 추가**할 때 이전 프리뷰 메시가 GC되지 않음

### 원인 분석

```
디테일 패널에서 링 추가 시:
1. 트랜잭션 시작 (UE 내부)
2. Modify() 호출 → 에셋 스냅샷 (이전 PreviewSubdividedMesh 포인터 캡처)
3. Ring 배열 변경
4. PostEditChangeProperty() → OnAssetChanged.Broadcast()
5. 트랜잭션 종료
6. 다음 틱에서 RefreshPreview() → 새 PreviewSubdividedMesh 생성
   - SetSkeletalMesh() 호출 시 이전 메시가 트랜잭션에 캡처됨!

결과: 이전 메시가 TransBuffer에 캡처되어 GC 불가
```

**핵심 원인**:
1. `UFleshRingAsset`의 `Modify()` 호출 시 `PreviewSubdividedMesh` 포인터가 캡처됨
2. `SkeletalMeshComponent::SetSkeletalMesh()` 호출 시 이전 메시가 트랜잭션에 기록됨
3. `DuplicateObject<USkeletalMesh>()` 호출이 활성 트랜잭션 중에 발생

---

## 해결책

### 1. PreviewSubdividedMesh를 에셋에서 PreviewScene으로 이동

에셋의 `Modify()` 시 PreviewSubdividedMesh가 캡처되지 않도록, `UFleshRingAsset`이 아닌 `FFleshRingPreviewScene`에서 관리.

#### 변경 전 구조
```
UFleshRingAsset
└── SubdivisionSettings (FSubdivisionSettings)
    ├── SubdividedMesh (저장됨)
    ├── BakedMesh (저장됨)
    └── PreviewSubdividedMesh (Transient) ← 문제!

FFleshRingPreviewScene
└── CachedOriginalMesh (TWeakObjectPtr)
```

#### 변경 후 구조
```
UFleshRingAsset
└── SubdivisionSettings (FSubdivisionSettings)
    ├── SubdividedMesh (저장됨)
    └── BakedMesh (저장됨)

FFleshRingPreviewScene
├── CachedOriginalMesh (TWeakObjectPtr)
├── PreviewSubdividedMesh (TObjectPtr) ← 이동!
├── LastPreviewBoneConfigHash
└── bPreviewMeshCacheValid
```

### 2. GUndo 비활성화로 트랜잭션 캡처 방지

`SetSkeletalMesh()` 및 메시 생성/삭제 시 `GUndo = nullptr`로 트랜잭션 기록 비활성화.

```cpp
// 메시 교체 시 트랜잭션 비활성화 패턴
ITransaction* OldGUndo = GUndo;
GUndo = nullptr;

SkeletalMeshComponent->SetSkeletalMesh(InMesh);

GUndo = OldGUndo;
```

**주의**: `GUndo`는 `bool`이 아닌 `ITransaction*` 타입!

---

## 수정된 파일 목록

### FleshRingEditor 모듈

#### 1. FleshRingPreviewScene.h
**경로**: `Source/FleshRingEditor/Private/FleshRingPreviewScene.h`

**추가된 멤버**:
```cpp
private:
    /** 프리뷰용 Subdivided 메시 (에디터 전용, 트랜잭션에서 제외) */
    TObjectPtr<USkeletalMesh> PreviewSubdividedMesh;

    /** 프리뷰 메시 캐시 유효성 */
    uint32 LastPreviewBoneConfigHash = 0;
    bool bPreviewMeshCacheValid = false;
```

**추가된 함수**:
```cpp
public:
    void GeneratePreviewMesh();
    void ClearPreviewMesh();
    void InvalidatePreviewMeshCache();
    bool HasValidPreviewMesh() const;
    bool IsPreviewMeshCacheValid() const;
    uint32 CalculatePreviewBoneConfigHash() const;
```

#### 2. FleshRingPreviewScene.cpp
**경로**: `Source/FleshRingEditor/Private/FleshRingPreviewScene.cpp`

**주요 변경**:

1. **SetSkeletalMesh()에 GUndo 비활성화 추가**:
```cpp
void FFleshRingPreviewScene::SetSkeletalMesh(USkeletalMesh* InMesh)
{
    if (SkeletalMeshComponent)
    {
        // ... validation ...

        // ★ Undo 비활성화
        ITransaction* OldGUndo = GUndo;
        GUndo = nullptr;

        SkeletalMeshComponent->SetSkeletalMesh(InMesh);

        GUndo = OldGUndo;
        // ...
    }
}
```

2. **GeneratePreviewMesh() 전체를 GUndo 비활성화로 감싸기**:
```cpp
void FFleshRingPreviewScene::GeneratePreviewMesh()
{
    if (!CurrentAsset) { return; }
    if (IsPreviewMeshCacheValid()) { return; }

    // ★ 전체 메시 생성/제거 과정을 Undo 시스템에서 제외
    ITransaction* OldGUndo = GUndo;
    GUndo = nullptr;

    // ... 메시 생성 로직 ...

    // ★ 모든 return 경로에서 GUndo 복원
    GUndo = OldGUndo;
}
```

3. **소멸자에서 메시 복원 시 GUndo 비활성화**:
```cpp
FFleshRingPreviewScene::~FFleshRingPreviewScene()
{
    // ...
    if (CurrentMesh != OriginalMesh)
    {
        ITransaction* OldGUndo = GUndo;
        GUndo = nullptr;
        SkeletalMeshComponent->SetSkeletalMesh(OriginalMesh);
        GUndo = OldGUndo;
    }
    // ...
}
```

4. **ExecutePendingDeformerInit()에서 GUndo 비활성화**:
```cpp
if (bUsePreviewMesh && SkeletalMeshComponent)
{
    ITransaction* OldGUndo = GUndo;
    GUndo = nullptr;
    SkeletalMeshComponent->SetSkeletalMesh(PreviewSubdividedMesh);
    GUndo = OldGUndo;
}
```

#### 3. FleshRingAssetEditor.h / .cpp
**경로**: `Source/FleshRingEditor/Private/FleshRingAssetEditor.h`, `.cpp`

**추가된 함수**:
```cpp
void FFleshRingAssetEditor::ForceRefreshPreviewMesh()
{
    if (PreviewScene.IsValid())
    {
        PreviewScene->InvalidatePreviewMeshCache();
        PreviewScene->GeneratePreviewMesh();
    }
}
```

#### 4. FleshRingAssetDetailCustomization.cpp
**경로**: `Source/FleshRingEditor/Private/FleshRingAssetDetailCustomization.cpp`

**변경**: `OnRefreshPreviewClicked()`에서 PreviewScene 함수 호출
```cpp
// 변경 전
FleshRingAsset->GeneratePreviewMesh();

// 변경 후
AssetEditor->ForceRefreshPreviewMesh();
```

#### 5. FSubdivisionSettingsCustomization.cpp
**경로**: `Source/FleshRingEditor/Private/FSubdivisionSettingsCustomization.cpp`

**변경**: 동일하게 `ForceRefreshPreviewMesh()` 사용

#### 6. FleshRingEditor.Build.cs
**경로**: `Source/FleshRingEditor/FleshRingEditor.Build.cs`

**추가된 의존성**:
```csharp
"MeshDescription",
"SkeletalMeshDescription",
"StaticMeshDescription",
```

### FleshRingRuntime 모듈

#### 7. FleshRingTypes.h
**경로**: `Source/FleshRingRuntime/Public/FleshRingTypes.h`

**제거된 멤버** (FSubdivisionSettings에서):
```cpp
// 제거됨
UPROPERTY(Transient, NonTransactional)
TObjectPtr<USkeletalMesh> PreviewSubdividedMesh;

uint32 LastPreviewBoneConfigHash = 0;
bool bPreviewMeshCacheValid = false;
```

#### 8. FleshRingAsset.h
**경로**: `Source/FleshRingRuntime/Public/FleshRingAsset.h`

**제거된 함수**:
```cpp
// 모두 제거됨
void GeneratePreviewMesh();
void ClearPreviewMesh();
bool HasValidPreviewMesh() const;
bool NeedsPreviewMeshRegeneration() const;
void InvalidatePreviewMeshCache();
uint32 CalculatePreviewBoneConfigHash() const;
bool IsPreviewMeshCacheValid() const;
```

#### 9. FleshRingAsset.cpp
**경로**: `Source/FleshRingRuntime/Private/FleshRingAsset.cpp`

**제거된 함수 구현**: 위에 나열된 모든 함수 구현 제거

**변경**: `CleanupOrphanedMeshes()`에서 PreviewSubdividedMesh 추적 제거

#### 10. FleshRingComponent.cpp
**경로**: `Source/FleshRingRuntime/Private/FleshRingComponent.cpp`

**제거된 코드**:
```cpp
// 제거됨 (에디터 프리뷰 메시 참조)
if (FleshRingAsset->SubdivisionSettings.PreviewSubdividedMesh != nullptr &&
    GetWorld() && !GetWorld()->IsGameWorld())
```

---

## 검증 방법

### 테스트 절차
1. 에디터에서 FleshRingAsset 열기
2. 디테일 패널에서 링 추가/삭제/복제/드래그 반복
3. 콘솔에서 `obj list class=skeletalmesh` 실행
4. 프리뷰 메시가 1개만 있는지 확인

### 예상 결과
```
Cmd: obj list class=skeletalmesh
Objects:
SkeletalMesh /Engine/Transient.SK_XXX_Preview_XXX  (프리뷰 메시 1개)
SkeletalMesh /Game/XXX/Mesh/SK_XXX  (게임 에셋들)
3 Objects (프리뷰 1개 + 게임 에셋들)
```

### 이전 문제 상황
```
Cmd: obj list class=skeletalmesh
Objects:
SkeletalMesh /Engine/Transient.SkeletalMesh_1  ← GC 안됨
SkeletalMesh /Engine/Transient.SkeletalMesh_2  ← GC 안됨
SkeletalMesh /Engine/Transient.SK_XXX_Preview_XXX
SkeletalMesh /Game/XXX/Mesh/SK_XXX
10+ Objects (계속 증가)
```

---

## 기술적 세부사항

### GUndo란?
- `ITransaction*` 타입의 전역 변수
- 현재 활성 트랜잭션을 가리킴
- `nullptr`이면 트랜잭션 비활성화 상태
- `Modify()` 호출 시 이 트랜잭션에 객체 상태가 기록됨

### 왜 PreviewScene으로 이동했나?
1. **이중 보호**: PreviewScene은 UObject가 아니므로 UPROPERTY 시리얼라이제이션/트랜잭션 대상이 아님
2. **Modify() 회피**: 에셋의 `Modify()` 호출 시 PreviewSubdividedMesh가 캡처되지 않음
3. **런타임/에디터 분리**: 에디터 전용 데이터가 런타임 에셋에서 제거됨
4. **쿠킹 최적화**: 패키징 시 불필요한 데이터 제외

### /Engine/Transient 패키지
- UE의 **임시 객체 저장용 특수 패키지**
- 디스크에 저장되지 않음
- 프리뷰 메시가 여기에 있는 것은 정상이며 의도된 설계
- 문제는 Transient에 있는 것이 아니라, **TransBuffer가 참조를 보유해서 GC가 안 되는 것**이었음

---

## 결론

PreviewSubdividedMesh를 에셋에서 PreviewScene으로 이동하고, 모든 `SetSkeletalMesh()` 호출 및 메시 생성 과정에서 `GUndo = nullptr`로 트랜잭션 기록을 비활성화하여 GC 누적 문제를 해결함.
