// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingEditorViewportClient.h"
#include "FleshRingEdMode.h"
#include "FleshRingPreviewScene.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingHitProxy.h"
#include "FleshRingTypes.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "EngineUtils.h"
#include "SkeletalDebugRendering.h"
#include "Preferences/PersonaOptions.h"
#include "UnrealWidget.h"
#include "Editor.h"
#include "Settings/LevelEditorViewportSettings.h"

IMPLEMENT_HIT_PROXY(HFleshRingGizmoHitProxy, HHitProxy);
IMPLEMENT_HIT_PROXY(HFleshRingMeshHitProxy, HHitProxy);
IMPLEMENT_HIT_PROXY(HFleshRingAxisHitProxy, HHitProxy);

FFleshRingEditorViewportClient::FFleshRingEditorViewportClient(
	FEditorModeTools* InModeTools,
	FFleshRingPreviewScene* InPreviewScene,
	const TWeakPtr<SFleshRingEditorViewport>& InViewportWidget)
	: FEditorViewportClient(InModeTools, InPreviewScene, StaticCastSharedPtr<SEditorViewport>(InViewportWidget.Pin()))
	, PreviewScene(InPreviewScene)
	, ViewportWidget(InViewportWidget)
{
	// Widget에 ModeTools 연결 (ShouldDrawWidget 호출을 위해 필요)
	if (Widget && ModeTools)
	{
		Widget->SetUsesEditorModeTools(ModeTools.Get());
	}

	// 기본 카메라 설정
	SetViewLocation(FVector(-300, 200, 150));
	SetViewRotation(FRotator(-15, -30, 0));

	// 뷰포트 설정
	SetRealtime(true);
	DrawHelper.bDrawGrid = true;
	DrawHelper.bDrawPivot = false;
	DrawHelper.AxesLineThickness = 2.0f;
	DrawHelper.PivotSize = 5.0f;

	// 배경 설정
	EngineShowFlags.SetGrid(true);
	EngineShowFlags.SetBones(false);  // 직접 그리기 위해 기본 본 렌더링 비활성화

	// 조명 설정
	EngineShowFlags.SetLighting(true);
	EngineShowFlags.SetPostProcessing(true);

	// Stats 표시 활성화 (FPS 등)
	SetShowStats(true);
}

FFleshRingEditorViewportClient::~FFleshRingEditorViewportClient()
{
}

void FFleshRingEditorViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// 프리뷰 씬 틱
	if (PreviewScene)
	{
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
	}
}

void FFleshRingEditorViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	FEditorViewportClient::Draw(View, PDI);

	// 본 렌더링 (Persona 스타일)
	if (bShowBones)
	{
		DrawMeshBones(PDI);
	}

	// Ring 기즈모 그리기
	if (bShowRingGizmos)
	{
		DrawRingGizmos(PDI);
	}
}

void FFleshRingEditorViewportClient::DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas)
{
	FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);
}

bool FFleshRingEditorViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	if (EventArgs.Event == IE_Pressed)
	{
		// F키로 메시에 포커스
		if (EventArgs.Key == EKeys::F)
		{
			FocusOnMesh();
			return true;
		}

		// 카메라 조작 중에는 QWER 키 무시 (우클릭 드래그 등)
		if (!IsTracking())
		{
			// QWER 키로 위젯 모드 전환 (ModeTools 사용)
			if (ModeTools)
			{
				if (EventArgs.Key == EKeys::Q)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_None);
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::W)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_Translate);
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::E)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_Rotate);
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::R)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_Scale);
					Invalidate();
					return true;
				}
			}
		}
	}

	return FEditorViewportClient::InputKey(EventArgs);
}

void FFleshRingEditorViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	if (Key == EKeys::LeftMouseButton && Event == IE_Released)
	{
		if (HitProxy)
		{
			// Ring 기즈모 클릭
			if (HitProxy->IsA(HFleshRingGizmoHitProxy::StaticGetType()))
			{
				HFleshRingGizmoHitProxy* GizmoProxy = static_cast<HFleshRingGizmoHitProxy*>(HitProxy);
				if (PreviewScene)
				{
					PreviewScene->SetSelectedRingIndex(GizmoProxy->RingIndex);
					SelectionType = EFleshRingSelectionType::Gizmo;
					Invalidate();
				}
				return;
			}
			// Ring 메시 클릭 (HitProxy 직접 등록된 경우)
			else if (HitProxy->IsA(HFleshRingMeshHitProxy::StaticGetType()))
			{
				HFleshRingMeshHitProxy* MeshProxy = static_cast<HFleshRingMeshHitProxy*>(HitProxy);
				if (PreviewScene)
				{
					PreviewScene->SetSelectedRingIndex(MeshProxy->RingIndex);
					SelectionType = EFleshRingSelectionType::Mesh;
					Invalidate();
				}
				return;
			}
			// StaticMeshComponent 클릭 (HActor HitProxy)
			else if (HitProxy->IsA(HActor::StaticGetType()))
			{
				HActor* ActorProxy = static_cast<HActor*>(HitProxy);
				if (PreviewScene && ActorProxy->PrimComponent)
				{
					// RingMeshComponents에서 해당 컴포넌트 찾기
					const TArray<UStaticMeshComponent*>& RingMeshComponents = PreviewScene->GetRingMeshComponents();
					for (int32 i = 0; i < RingMeshComponents.Num(); ++i)
					{
						if (RingMeshComponents[i] == ActorProxy->PrimComponent)
						{
							PreviewScene->SetSelectedRingIndex(i);
							SelectionType = EFleshRingSelectionType::Mesh;
							Invalidate();
							return;
						}
					}
				}
			}
		}

		// 빈 공간 클릭 - 선택 해제
		ClearSelection();
	}

	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

void FFleshRingEditorViewportClient::ClearSelection()
{
	if (PreviewScene)
	{
		PreviewScene->SetSelectedRingIndex(-1);
	}
	SelectionType = EFleshRingSelectionType::None;
	Invalidate();
}

void FFleshRingEditorViewportClient::SetAsset(UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	if (PreviewScene && InAsset)
	{
		// Asset 설정 (메시 + 컴포넌트 + Ring 시각화)
		PreviewScene->SetFleshRingAsset(InAsset);

		// 카메라 포커스
		FocusOnMesh();
	}
}

void FFleshRingEditorViewportClient::FocusOnMesh()
{
	if (!PreviewScene)
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset())
	{
		// 메시 바운드로 카메라 포커스
		FBoxSphereBounds Bounds = SkelMeshComp->Bounds;
		FVector Center = Bounds.Origin;
		float Radius = Bounds.SphereRadius;

		// 카메라 위치 계산
		float Distance = Radius * 2.5f;
		FVector NewLocation = Center - GetViewRotation().Vector() * Distance;
		SetViewLocation(NewLocation);

		Invalidate();
	}
}

void FFleshRingEditorViewportClient::DrawMeshBones(FPrimitiveDrawInterface* PDI)
{
	if (!PreviewScene)
	{
		return;
	}

	UDebugSkelMeshComponent* MeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	// 컴포넌트가 등록되지 않았으면 스킵
	if (!MeshComponent->IsRegistered())
	{
		return;
	}

	if (MeshComponent->SkeletonDrawMode == ESkeletonDrawMode::Hidden)
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = MeshComponent->GetReferenceSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	// 본이 없으면 리턴
	if (NumBones == 0)
	{
		return;
	}

	// 스켈레탈 메시가 완전히 로드되었는지 확인
	const TArray<FTransform>& ComponentSpaceTransforms = MeshComponent->GetComponentSpaceTransforms();
	if (ComponentSpaceTransforms.Num() < NumBones)
	{
		return;
	}

	// 월드 Transform 배열 생성 (직접 ComponentSpaceTransforms 사용)
	TArray<FTransform> WorldTransforms;
	WorldTransforms.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		WorldTransforms[i] = ComponentSpaceTransforms[i] * MeshComponent->GetComponentTransform();
	}

	// 그릴 본 인덱스 (모든 본)
	TArray<FBoneIndexType> AllBoneIndices;
	AllBoneIndices.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		AllBoneIndices[i] = static_cast<FBoneIndexType>(i);
	}

	// 본 색상 배열 생성 (UDebugSkelMeshComponent의 고정 색상 사용)
	TArray<FLinearColor> BoneColors;
	BoneColors.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		BoneColors[i] = MeshComponent->GetBoneColor(i);
	}

	// DrawConfig 설정
	FSkelDebugDrawConfig DrawConfig;
	DrawConfig.BoneDrawMode = EBoneDrawMode::All;
	DrawConfig.BoneDrawSize = 1.0f;
	DrawConfig.bForceDraw = true;
	DrawConfig.bAddHitProxy = false;
	DrawConfig.bUseMultiColorAsDefaultColor = GetDefault<UPersonaOptions>()->bShowBoneColors;
	DrawConfig.DefaultBoneColor = GetDefault<UPersonaOptions>()->DefaultBoneColor;
	DrawConfig.SelectedBoneColor = GetDefault<UPersonaOptions>()->SelectedBoneColor;
	DrawConfig.AffectedBoneColor = GetDefault<UPersonaOptions>()->AffectedBoneColor;
	DrawConfig.ParentOfSelectedBoneColor = GetDefault<UPersonaOptions>()->ParentOfSelectedBoneColor;

	// 그릴 본 인덱스 비트 배열 (모든 본 그리기)
	TBitArray<> BonesToDraw;
	BonesToDraw.Init(true, NumBones);

	// 본 렌더링
	SkeletalDebugRendering::DrawBones(
		PDI,
		MeshComponent->GetComponentLocation(),
		AllBoneIndices,
		RefSkeleton,
		WorldTransforms,
		TArray<int32>(),  // 선택된 본 없음
		BoneColors,
		TArray<TRefCountPtr<HHitProxy>>(),  // HitProxy 없음
		DrawConfig,
		BonesToDraw
	);
}

void FFleshRingEditorViewportClient::DrawRingGizmos(FPrimitiveDrawInterface* PDI)
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();

	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		const FFleshRingSettings& Ring = Rings[i];

		// 본 Transform 가져오기
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
		FVector BoneLocation = BoneTransform.GetLocation();
		FQuat BoneRotation = BoneTransform.GetRotation();

		// 본 회전 * 링 회전 = 월드 회전 (RingRotation 기본값 90,0,0으로 본의 X축과 링의 Z축이 일치)
		FQuat RingWorldRotation = BoneRotation * FQuat(Ring.RingRotation);

		// RingOffset 적용
		FVector GizmoLocation = BoneLocation + BoneRotation.RotateVector(Ring.RingOffset);

		// Ring 색상 결정
		FLinearColor GizmoColor;
		if (i == SelectedIndex)
		{
			// 선택된 Ring: Gizmo=노랑, Mesh=마젠타
			GizmoColor = (SelectionType == EFleshRingSelectionType::Gizmo)
				? FLinearColor::Yellow
				: FLinearColor(1.0f, 0.0f, 1.0f, 1.0f); // 마젠타 (Mesh 선택 시)
		}
		else
		{
			// 선택 안 된 Ring: 시안
			GizmoColor = FLinearColor(0.0f, 1.0f, 1.0f, 0.8f);
		}

		// HitProxy 설정 (Ring 기즈모용)
		PDI->SetHitProxy(new HFleshRingGizmoHitProxy(i));

		// Ring 원 그리기 (RingRotation 적용)
		float Radius = Ring.RingRadius;
		int32 Segments = 32;

		for (int32 s = 0; s < Segments; ++s)
		{
			float Angle1 = (float)s / Segments * 2.0f * PI;
			float Angle2 = (float)(s + 1) / Segments * 2.0f * PI;

			// 로컬 XY 평면에서 원을 그린 후 RingWorldRotation으로 회전
			FVector P1 = GizmoLocation + RingWorldRotation.RotateVector(
				FVector(FMath::Cos(Angle1) * Radius, FMath::Sin(Angle1) * Radius, 0.0f));
			FVector P2 = GizmoLocation + RingWorldRotation.RotateVector(
				FVector(FMath::Cos(Angle2) * Radius, FMath::Sin(Angle2) * Radius, 0.0f));

			PDI->DrawLine(P1, P2, GizmoColor, SDPG_Foreground, 1.0f);
		}

		// 본 위치에 작은 구 표시
		DrawWireSphere(PDI, GizmoLocation, GizmoColor, 2.0f, 8, SDPG_Foreground);

		// HitProxy 해제
		PDI->SetHitProxy(nullptr);
	}
}

FVector FFleshRingEditorViewportClient::GetWidgetLocation() const
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FVector::ZeroVector;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return FVector::ZeroVector;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return FVector::ZeroVector;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return FVector::ZeroVector;
	}

	const FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FVector BoneLocation = BoneTransform.GetLocation();

	// 선택 타입에 따라 다른 오프셋 적용
	if (SelectionType == EFleshRingSelectionType::Gizmo)
	{
		return BoneLocation + BoneTransform.GetRotation().RotateVector(Ring.RingOffset);
	}
	else if (SelectionType == EFleshRingSelectionType::Mesh)
	{
		return BoneLocation + BoneTransform.GetRotation().RotateVector(Ring.MeshOffset);
	}

	return FVector::ZeroVector;
}

FMatrix FFleshRingEditorViewportClient::GetWidgetCoordSystem() const
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FMatrix::Identity;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return FMatrix::Identity;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return FMatrix::Identity;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return FMatrix::Identity;
	}

	const FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FMatrix::Identity;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);

	// AlignRotation 좌표계 반환 (GetSelectedRingAlignMatrix와 동일)
	return GetSelectedRingAlignMatrix();
}

FMatrix FFleshRingEditorViewportClient::GetSelectedRingAlignMatrix() const
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FMatrix::Identity;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return FMatrix::Identity;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return FMatrix::Identity;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return FMatrix::Identity;
	}

	const FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FMatrix::Identity;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FQuat BoneRotation = BoneTransform.GetRotation();

	// 좌표계 모드 확인 (World vs Local)
	ECoordSystem CoordSystem = ModeTools ? ModeTools->GetCoordSystem() : COORD_World;

	if (CoordSystem == COORD_Local)
	{
		// 로컬 모드: 본 회전 * 링/메시 회전 = 현재 월드 회전
		FQuat CurrentRotation;
		if (SelectionType == EFleshRingSelectionType::Gizmo)
		{
			CurrentRotation = FQuat(Ring.RingRotation);
		}
		else // Mesh
		{
			CurrentRotation = FQuat(Ring.MeshRotation);
		}
		FQuat WorldRotation = BoneRotation * CurrentRotation;
		return FRotationMatrix(WorldRotation.Rotator());
	}
	else
	{
		// 월드 모드: 순수 월드 축 기준
		return FMatrix::Identity;
	}
}

UE::Widget::EWidgetMode FFleshRingEditorViewportClient::GetWidgetMode() const
{
	// ModeTools의 위젯 모드 사용 (툴바 하이라이트 동작)
	if (ModeTools)
	{
		return ModeTools->GetWidgetMode();
	}
	return UE::Widget::WM_Translate;
}

bool FFleshRingEditorViewportClient::InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale)
{
	// Widget 축이 선택되지 않았으면 처리 안 함
	if (CurrentAxis == EAxisList::None)
	{
		return false;
	}

	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return false;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return false;
	}

	TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return false;
	}

	// 본 Transform 가져오기 (로컬 좌표 변환용)
	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return false;
	}

	FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return false;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FQuat BoneRotation = BoneTransform.GetRotation();

	// 스냅 적용
	const ULevelEditorViewportSettings* ViewportSettings = GetDefault<ULevelEditorViewportSettings>();

	// 이동 스냅
	FVector SnappedDrag = Drag;
	if (ViewportSettings->GridEnabled && GEditor)
	{
		float GridSize = GEditor->GetGridSize();
		SnappedDrag.X = FMath::GridSnap(Drag.X, GridSize);
		SnappedDrag.Y = FMath::GridSnap(Drag.Y, GridSize);
		SnappedDrag.Z = FMath::GridSnap(Drag.Z, GridSize);
	}

	// 회전 스냅
	FRotator SnappedRot = Rot;
	if (ViewportSettings->RotGridEnabled && GEditor)
	{
		FRotator RotGridSize = GEditor->GetRotGridSize();
		SnappedRot.Pitch = FMath::GridSnap(Rot.Pitch, RotGridSize.Pitch);
		SnappedRot.Yaw = FMath::GridSnap(Rot.Yaw, RotGridSize.Yaw);
		SnappedRot.Roll = FMath::GridSnap(Rot.Roll, RotGridSize.Roll);
	}

	// 스케일 스냅
	FVector SnappedScale = Scale;
	if (ViewportSettings->SnapScaleEnabled && GEditor)
	{
		float ScaleGridSize = GEditor->GetScaleGridSize();
		SnappedScale.X = FMath::GridSnap(Scale.X, ScaleGridSize);
		SnappedScale.Y = FMath::GridSnap(Scale.Y, ScaleGridSize);
		SnappedScale.Z = FMath::GridSnap(Scale.Z, ScaleGridSize);
	}

	// 좌표계 모드 확인 (World vs Local)
	ECoordSystem CoordSystem = ModeTools ? ModeTools->GetCoordSystem() : COORD_World;

	// 월드 드래그를 본 로컬 좌표로 변환
	FVector LocalDrag = BoneRotation.UnrotateVector(SnappedDrag);

	// 선택 타입에 따라 다른 오프셋 업데이트
	if (SelectionType == EFleshRingSelectionType::Gizmo)
	{
		// 이동 -> RingOffset 업데이트
		Ring.RingOffset += LocalDrag;

		// 회전 -> RingRotation 업데이트
		if (!SnappedRot.IsZero())
		{
			FQuat DeltaRotation = FQuat(SnappedRot);
			// 현재 월드 회전 = 본 회전 * 링 회전
			FQuat CurrentWorldRotation = BoneRotation * FQuat(Ring.RingRotation);

			FQuat NewWorldRotation;
			if (CoordSystem == COORD_Local)
			{
				// 로컬 기준: 현재 회전 상태의 로컬 축 기준으로 회전
				NewWorldRotation = CurrentWorldRotation * DeltaRotation;
			}
			else
			{
				// 월드 기준: 월드 축 기준으로 회전
				NewWorldRotation = DeltaRotation * CurrentWorldRotation;
			}

			// 월드 회전을 본 로컬 회전으로 변환: 링 회전 = 본 회전^-1 * 월드 회전
			FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
			Ring.RingRotation = NewLocalRotation.Rotator();
		}

		// 스케일 -> RingRadius 조절 (Manual 모드에서만)
		if (!SnappedScale.IsZero() && Ring.InfluenceMode == EFleshRingInfluenceMode::Manual)
		{
			// 균일 스케일 사용 (X, Y, Z 중 가장 큰 값)
			float ScaleDelta = FMath::Max3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
			if (ScaleDelta == 0.0f)
			{
				ScaleDelta = FMath::Min3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
			}
			// 스케일을 반경 변화량으로 변환
			Ring.RingRadius = FMath::Clamp(Ring.RingRadius * (1.0f + ScaleDelta), 0.1f, 100.0f);
		}
	}
	else if (SelectionType == EFleshRingSelectionType::Mesh)
	{
		// Ring 메시 이동 -> MeshOffset 업데이트
		Ring.MeshOffset += LocalDrag;

		// 회전도 적용
		if (!SnappedRot.IsZero())
		{
			FQuat DeltaRotation = FQuat(SnappedRot);
			// 현재 월드 회전 = 본 회전 * 메시 회전
			FQuat CurrentWorldRotation = BoneRotation * FQuat(Ring.MeshRotation);

			FQuat NewWorldRotation;
			if (CoordSystem == COORD_Local)
			{
				// 로컬 기준: 현재 회전 상태의 로컬 축 기준으로 회전
				NewWorldRotation = CurrentWorldRotation * DeltaRotation;
			}
			else
			{
				// 월드 기준: 월드 축 기준으로 회전
				NewWorldRotation = DeltaRotation * CurrentWorldRotation;
			}

			// 월드 회전을 본 로컬 회전으로 변환: 메시 회전 = 본 회전^-1 * 월드 회전
			FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
			Ring.MeshRotation = NewLocalRotation.Rotator();
		}

		// 스케일 적용
		if (!SnappedScale.IsZero())
		{
			Ring.MeshScale += SnappedScale;
			// 최소값만 클램프
			Ring.MeshScale.X = FMath::Max(Ring.MeshScale.X, 0.01f);
			Ring.MeshScale.Y = FMath::Max(Ring.MeshScale.Y, 0.01f);
			Ring.MeshScale.Z = FMath::Max(Ring.MeshScale.Z, 0.01f);
		}

		// StaticMeshComponent Transform 업데이트
		const TArray<UStaticMeshComponent*>& RingMeshComponents = PreviewScene->GetRingMeshComponents();
		if (RingMeshComponents.IsValidIndex(SelectedIndex) && RingMeshComponents[SelectedIndex])
		{
			FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(Ring.MeshOffset);
			FQuat WorldRotation = BoneRotation * FQuat(Ring.MeshRotation);
			RingMeshComponents[SelectedIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation.Rotator());
			RingMeshComponents[SelectedIndex]->SetWorldScale3D(Ring.MeshScale);
		}
	}

	// Asset 변경 알림
	EditingAsset->MarkPackageDirty();

	// 뷰포트 갱신
	Invalidate();

	return true;
}

void FFleshRingEditorViewportClient::TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge)
{
	// 드래그 시작 시 트랜잭션 시작
	if (bIsDraggingWidget && EditingAsset.IsValid() && SelectionType != EFleshRingSelectionType::None)
	{
		ScopedTransaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("FleshRingEditor", "ModifyRingTransform", "Modify Ring Transform"));
		EditingAsset->Modify();
	}

	FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
}

void FFleshRingEditorViewportClient::TrackingStopped()
{
	// 드래그 종료 시 트랜잭션 종료
	ScopedTransaction.Reset();

	FEditorViewportClient::TrackingStopped();
}

void FFleshRingEditorViewportClient::ToggleShowRingMeshes()
{
	bShowRingMeshes = !bShowRingMeshes;

	// Ring 메시 컴포넌트 Visibility 토글
	if (PreviewScene)
	{
		const TArray<UStaticMeshComponent*>& RingMeshComponents = PreviewScene->GetRingMeshComponents();
		for (UStaticMeshComponent* RingComp : RingMeshComponents)
		{
			if (RingComp)
			{
				RingComp->SetVisibility(bShowRingMeshes);
			}
		}
	}

	Invalidate();
}
