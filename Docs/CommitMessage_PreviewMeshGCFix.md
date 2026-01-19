# 커밋 메시지

## 제목
PreviewSubdividedMesh GC 누적 문제 수정 - 에셋에서 PreviewScene으로 이동 및 GUndo 비활성화

## 설명
```
[수정] PreviewSubdividedMesh GC 누적 문제 해결

문제:
- 디테일 패널에서 Ring 추가/삭제/복제/드래그 시 프리뷰 메시가 GC되지 않고 /Engine/Transient에 누적
- 원인: SetSkeletalMesh() 호출 및 에셋 Modify() 시 이전 메시가 TransBuffer에 캡처됨

해결:
1. PreviewSubdividedMesh를 UFleshRingAsset에서 FFleshRingPreviewScene으로 이동
   - 에셋 Modify() 시 프리뷰 메시 포인터가 캡처되지 않음
   - 에디터 전용 데이터가 런타임 에셋에서 분리됨

2. 모든 SetSkeletalMesh() 호출에 GUndo 비활성화 적용
   - ITransaction* OldGUndo = GUndo; GUndo = nullptr; 패턴 사용
   - GeneratePreviewMesh() 전체를 GUndo 비활성화로 감싸기
   - 소멸자, ExecutePendingDeformerInit() 등에도 적용

수정 파일:
- FleshRingPreviewScene.h/cpp: PreviewSubdividedMesh 및 관련 함수 이동, GUndo 비활성화 적용
- FleshRingAssetEditor.h/cpp: ForceRefreshPreviewMesh() 추가
- FleshRingAssetDetailCustomization.cpp: ForceRefreshPreviewMesh() 사용
- FSubdivisionSettingsCustomization.cpp: ForceRefreshPreviewMesh() 사용
- FleshRingTypes.h: PreviewSubdividedMesh 멤버 제거
- FleshRingAsset.h/cpp: 프리뷰 메시 관련 함수 제거
- FleshRingComponent.cpp: PreviewSubdividedMesh 참조 제거
- FleshRingEditor.Build.cs: MeshDescription 의존성 추가
```

---

## Perforce 제출용 (한 줄 요약)

```
[Fix] PreviewSubdividedMesh GC 누적 문제 수정 - PreviewScene으로 이동 및 GUndo 비활성화
```

## Perforce 제출용 (상세)

```
[Fix] PreviewSubdividedMesh GC 누적 문제 수정

- PreviewSubdividedMesh를 UFleshRingAsset에서 FFleshRingPreviewScene으로 이동
- 모든 SetSkeletalMesh() 호출에 GUndo = nullptr 적용하여 트랜잭션 캡처 방지
- 디테일 패널에서 Ring 조작 시 이전 프리뷰 메시가 TransBuffer에 캡처되어 GC 안되던 문제 해결
- 에디터 전용 프리뷰 데이터가 런타임 에셋에서 분리되어 더 깔끔한 구조
```
