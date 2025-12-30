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

// 에셋별 설정 저장용 Config 섹션 베이스
static const FString FleshRingViewportConfigSectionBase = TEXT("FleshRingEditorViewport");

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

	// 정적 인스턴스 레지스트리에 등록 (타입 안전 확인용)
	GetAllInstances().Add(this);
}

FFleshRingEditorViewportClient::~FFleshRingEditorViewportClient()
{
	// 설정 저장
	SaveSettings();

	// 정적 인스턴스 레지스트리에서 제거
	GetAllInstances().Remove(this);
}

void FFleshRingEditorViewportClient::ToggleLocalCoordSystem()
{
	// 스케일 모드일 때는 토글하지 않음 (항상 로컬 유지)
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return;
	}

	bUseLocalCoordSystem = !bUseLocalCoordSystem;
	Invalidate();
}

bool FFleshRingEditorViewportClient::IsUsingLocalCoordSystem() const
{
	// 스케일 모드일 때는 항상 로컬
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return true;
	}

	return bUseLocalCoordSystem;
}

void FFleshRingEditorViewportClient::SetLocalCoordSystem(bool bLocal)
{
	// 스케일 모드일 때는 변경하지 않음 (항상 로컬 유지)
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return;
	}

	bUseLocalCoordSystem = bLocal;
	Invalidate();
}

void FFleshRingEditorViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// 첫 Tick에서 저장된 설정 로드 (생성자에서는 뷰포트가 아직 준비되지 않음)
	if (!bSettingsLoaded)
	{
		LoadSettings();
		bSettingsLoaded = true;
	}

	// 프리뷰 씬 틱
	if (PreviewScene)
	{
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
	}

	// 선택된 링이 삭제되었는지 확인하고 선택 해제
	if (SelectionType != EFleshRingSelectionType::None && PreviewScene)
	{
		int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
		bool bSelectionValid = false;

		if (EditingAsset.IsValid() && SelectedIndex >= 0)
		{
			bSelectionValid = EditingAsset->Rings.IsValidIndex(SelectedIndex);
		}

		if (!bSelectionValid)
		{
			ClearSelection();
		}
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

		// Ctrl+` 로 Local/World 좌표계 전환
		if (EventArgs.Key == EKeys::Tilde &&
			(EventArgs.Viewport->KeyState(EKeys::LeftControl) || EventArgs.Viewport->KeyState(EKeys::RightControl)))
		{
			ToggleLocalCoordSystem();
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
					// 1. PreviewScene의 RingMeshComponents에서 찾기 (Deformer 비활성화 시)
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

					// 2. FleshRingComponent의 RingMeshComponents에서 찾기 (Deformer 활성화 시)
					UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
					if (FleshRingComp)
					{
						const TArray<TObjectPtr<UStaticMeshComponent>>& ComponentRingMeshes = FleshRingComp->GetRingMeshComponents();
						for (int32 i = 0; i < ComponentRingMeshes.Num(); ++i)
						{
							if (ComponentRingMeshes[i] == ActorProxy->PrimComponent)
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

		// Manual 모드일 때만 Ring 기즈모 표시 (SDF 모드에서는 Radius가 의미 없음)
		if (Ring.InfluenceMode != EFleshRingInfluenceMode::Manual)
		{
			continue;
		}

		// 본 Transform 가져오기
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
		FVector BoneLocation = BoneTransform.GetLocation();
		FQuat BoneRotation = BoneTransform.GetRotation();

		// 본 회전 * 링 회전 = 월드 회전 (기본값으로 본의 X축과 링의 Z축이 일치)
		FQuat RingWorldRotation = BoneRotation * Ring.RingRotation;

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

	// 좌표계 모드 확인 (World vs Local) - 커스텀 플래그 사용
	if (bUseLocalCoordSystem)
	{
		FQuat TargetRotation;
		if (bIsDraggingRotation)
		{
			// 드래그 중이면 드래그 시작 시점의 회전 사용 (기즈모 고정)
			TargetRotation = DragStartWorldRotation;
		}
		else
		{
			// 로컬 모드: 본 회전 * 링/메시 회전 = 현재 월드 회전
			FQuat CurrentRotation;
			if (SelectionType == EFleshRingSelectionType::Gizmo)
			{
				CurrentRotation = Ring.RingRotation;
			}
			else // Mesh
			{
				CurrentRotation = Ring.MeshRotation;
			}
			TargetRotation = BoneRotation * CurrentRotation;
		}

		// FQuatRotationMatrix 사용 (FRotator 변환 시 Gimbal lock 문제 방지)
		return FQuatRotationMatrix(TargetRotation);
	}
	else
	{
		// 월드 모드: 순수 월드 축 기준
		return FMatrix::Identity;
	}
}

ECoordSystem FFleshRingEditorViewportClient::GetWidgetCoordSystemSpace() const
{
	// 항상 COORD_World를 반환하여 Widget 시스템의 Local Space 회전 반전 로직을 비활성화
	// GetWidgetCoordSystem()에서 이미 회전된 좌표계를 반환하고 있으므로,
	// 추가적인 Local Space 처리가 필요 없음
	return COORD_World;
}

void FFleshRingEditorViewportClient::SetWidgetCoordSystemSpace(ECoordSystem NewCoordSystem)
{
	// 기본 툴바 버튼 클릭 시 호출됨 - 커스텀 플래그 토글
	bUseLocalCoordSystem = (NewCoordSystem == COORD_Local);
	Invalidate();
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

	// 이동 스냅 - 기즈모 축 기준으로 적용
	// Widget 시스템은 월드 좌표로 Drag를 전달하므로, 기즈모 로컬로 변환 후 스냅 적용
	FVector SnappedDrag = Drag;
	if (ViewportSettings->GridEnabled && GEditor && !Drag.IsZero())
	{
		float GridSize = GEditor->GetGridSize();

		// 기즈모 좌표계 행렬 가져오기
		FMatrix GizmoMatrix = GetSelectedRingAlignMatrix();
		FMatrix GizmoMatrixInverse = GizmoMatrix.Inverse();

		// 월드 Drag를 기즈모 로컬 좌표로 변환
		FVector LocalDragForSnap = GizmoMatrixInverse.TransformVector(Drag);

		// 기즈모 로컬 좌표에서 스냅 적용
		FVector SnappedLocalDrag;
		SnappedLocalDrag.X = FMath::GridSnap(LocalDragForSnap.X, GridSize);
		SnappedLocalDrag.Y = FMath::GridSnap(LocalDragForSnap.Y, GridSize);
		SnappedLocalDrag.Z = FMath::GridSnap(LocalDragForSnap.Z, GridSize);

		// 다시 월드 좌표로 변환
		SnappedDrag = GizmoMatrix.TransformVector(SnappedLocalDrag);
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

	// 월드 드래그를 본 로컬 좌표로 변환
	// (Widget 시스템은 World/Local 모드 상관없이 항상 월드 좌표로 Drag를 전달함)
	FVector LocalDrag = BoneRotation.UnrotateVector(SnappedDrag);

	// 선택 타입에 따라 다른 오프셋 업데이트
	if (SelectionType == EFleshRingSelectionType::Gizmo)
	{
		// 이동 -> RingOffset 업데이트
		Ring.RingOffset += LocalDrag;

		// 회전 -> RingRotation 업데이트
		if (bIsDraggingRotation)
		{
			// Widget이 주는 Rot는 월드 좌표계로 분해된 값
			// 로컬 축 회전이 월드 FRotator로 변환되면서 Pitch/Yaw/Roll에 분산됨
			// 따라서 Rot 전체를 쿼터니언으로 변환하여 사용해야 회전이 정확히 360도 누적됨
			FQuat FrameDeltaRotation = Rot.Quaternion();

			if (!FrameDeltaRotation.IsIdentity())
			{
				// 누적 델타에 추가 (짐벌락 방지: FRotator를 다시 읽지 않고 쿼터니언으로 누적)
				AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
				AccumulatedDeltaRotation.Normalize();

				// 드래그 시작 회전에 누적 델타 적용
				FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
				NewWorldRotation.Normalize();

				// 월드 회전을 본 로컬 회전으로 변환 후 저장
				FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
				Ring.RingRotation = NewLocalRotation;
				Ring.RingEulerRotation = NewLocalRotation.Rotator();  // EulerRotation 동기화
			}
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
		if (bIsDraggingRotation)
		{
			// Widget이 주는 Rot는 월드 좌표계로 분해된 값
			// 로컬 축 회전이 월드 FRotator로 변환되면서 Pitch/Yaw/Roll에 분산됨
			// 따라서 Rot 전체를 쿼터니언으로 변환하여 사용해야 회전이 정확히 360도 누적됨
			FQuat FrameDeltaRotation = Rot.Quaternion();

			if (!FrameDeltaRotation.IsIdentity())
			{
				// 누적 델타에 추가 (짐벌락 방지: FRotator를 다시 읽지 않고 쿼터니언으로 누적)
				AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
				AccumulatedDeltaRotation.Normalize();

				// 드래그 시작 회전에 누적 델타 적용
				FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
				NewWorldRotation.Normalize();

				// 월드 회전을 본 로컬 회전으로 변환 후 저장
				FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
				Ring.MeshRotation = NewLocalRotation;
				Ring.MeshEulerRotation = NewLocalRotation.Rotator();  // EulerRotation 동기화
			}
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
		FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(Ring.MeshOffset);
		FQuat WorldRotation = BoneRotation * Ring.MeshRotation;

		// 1. PreviewScene의 RingMeshComponents 업데이트 (Deformer 비활성화 시)
		const TArray<UStaticMeshComponent*>& RingMeshComponents = PreviewScene->GetRingMeshComponents();
		if (RingMeshComponents.IsValidIndex(SelectedIndex) && RingMeshComponents[SelectedIndex])
		{
			RingMeshComponents[SelectedIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
			RingMeshComponents[SelectedIndex]->SetWorldScale3D(Ring.MeshScale);
		}

		// 2. FleshRingComponent의 RingMeshComponents 업데이트 (Deformer 활성화 시)
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			const TArray<TObjectPtr<UStaticMeshComponent>>& ComponentRingMeshes = FleshRingComp->GetRingMeshComponents();
			if (ComponentRingMeshes.IsValidIndex(SelectedIndex) && ComponentRingMeshes[SelectedIndex])
			{
				ComponentRingMeshes[SelectedIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
				ComponentRingMeshes[SelectedIndex]->SetWorldScale3D(Ring.MeshScale);
			}
		}
	}

	// Asset 변경 알림
	EditingAsset->MarkPackageDirty();

	// 트랜스폼만 업데이트 (Deformer 유지, 깜빡임 방지)
	if (PreviewScene)
	{
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			FleshRingComp->UpdateRingTransforms();
		}
	}

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

		// 회전 모드일 때만 회전 관련 초기화 수행
		bool bIsRotationMode = ModeTools && ModeTools->GetWidgetMode() == UE::Widget::WM_Rotate;
		if (bIsRotationMode)
		{
			// 드래그 시작 시 초기 회전 저장 (기즈모 좌표계 고정용)
			USkeletalMeshComponent* SkelMeshComp = PreviewScene ? PreviewScene->GetSkeletalMeshComponent() : nullptr;
			int32 SelectedIndex = PreviewScene ? PreviewScene->GetSelectedRingIndex() : -1;

			if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() && EditingAsset->Rings.IsValidIndex(SelectedIndex))
			{
				const FFleshRingSettings& Ring = EditingAsset->Rings[SelectedIndex];
				int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
				if (BoneIndex != INDEX_NONE)
				{
					FQuat BoneRotation = SkelMeshComp->GetBoneTransform(BoneIndex).GetRotation();
					FQuat CurrentRotation = (SelectionType == EFleshRingSelectionType::Gizmo)
						? Ring.RingRotation
						: Ring.MeshRotation;
					DragStartWorldRotation = BoneRotation * CurrentRotation;
					DragStartWorldRotation.Normalize();  // 정규화

					// 누적 델타 회전 초기화
					AccumulatedDeltaRotation = FQuat::Identity;

					bIsDraggingRotation = true;
				}
			}
		}
	}

	FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
}

void FFleshRingEditorViewportClient::TrackingStopped()
{
	// 드래그 종료 시 트랜잭션 종료
	ScopedTransaction.Reset();
	bIsDraggingRotation = false;

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

FString FFleshRingEditorViewportClient::GetConfigSectionName() const
{
	if (EditingAsset.IsValid())
	{
		// 에셋 경로를 섹션 이름에 포함 (예: FleshRingEditorViewport:/Game/FleshRings/MyAsset)
		return FString::Printf(TEXT("%s:%s"), *FleshRingViewportConfigSectionBase, *EditingAsset->GetPathName());
	}
	return FleshRingViewportConfigSectionBase;
}

void FFleshRingEditorViewportClient::SaveSettings()
{
	const FString SectionName = GetConfigSectionName();

	// 카메라 설정 저장
	GConfig->SetVector(*SectionName, TEXT("ViewLocation"), GetViewLocation(), GEditorPerProjectIni);
	GConfig->SetRotator(*SectionName, TEXT("ViewRotation"), GetViewRotation(), GEditorPerProjectIni);

	// 커스텀 쇼플래그 저장
	GConfig->SetBool(*SectionName, TEXT("ShowRingGizmos"), bShowRingGizmos, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowRingMeshes"), bShowRingMeshes, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBones"), bShowBones, GEditorPerProjectIni);

	// Config 파일에 즉시 저장
	GConfig->Flush(false, GEditorPerProjectIni);
}

void FFleshRingEditorViewportClient::LoadSettings()
{
	const FString SectionName = GetConfigSectionName();

	// 카메라 설정 로드
	FVector SavedLocation;
	if (GConfig->GetVector(*SectionName, TEXT("ViewLocation"), SavedLocation, GEditorPerProjectIni))
	{
		SetViewLocation(SavedLocation);
	}

	FRotator SavedRotation;
	if (GConfig->GetRotator(*SectionName, TEXT("ViewRotation"), SavedRotation, GEditorPerProjectIni))
	{
		SetViewRotation(SavedRotation);
	}

	// 커스텀 쇼플래그 로드
	GConfig->GetBool(*SectionName, TEXT("ShowRingGizmos"), bShowRingGizmos, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowRingMeshes"), bShowRingMeshes, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBones"), bShowBones, GEditorPerProjectIni);
}

void FFleshRingEditorViewportClient::ToggleShowDebugVisualization()
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowDebugVisualization = !Comp->bShowDebugVisualization;
			// 평면 액터 즉시 숨기기/보이기
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
			Invalidate();
		}
	}
}

void FFleshRingEditorViewportClient::ToggleShowSdfVolume()
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowSdfVolume = !Comp->bShowSdfVolume;
			Invalidate();
		}
	}
}

void FFleshRingEditorViewportClient::ToggleShowAffectedVertices()
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowAffectedVertices = !Comp->bShowAffectedVertices;
			Invalidate();
		}
	}
}

bool FFleshRingEditorViewportClient::ShouldShowDebugVisualization() const
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			return Comp->bShowDebugVisualization;
		}
	}
	return false;
}

bool FFleshRingEditorViewportClient::ShouldShowSdfVolume() const
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			return Comp->bShowSdfVolume;
		}
	}
	return false;
}

bool FFleshRingEditorViewportClient::ShouldShowAffectedVertices() const
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			return Comp->bShowAffectedVertices;
		}
	}
	return false;
}

void FFleshRingEditorViewportClient::ToggleShowSDFSlice()
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowSDFSlice = !Comp->bShowSDFSlice;
			// 평면 액터 즉시 숨기기/보이기
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
			Invalidate();
		}
	}
}

bool FFleshRingEditorViewportClient::ShouldShowSDFSlice() const
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			return Comp->bShowSDFSlice;
		}
	}
	return false;
}

int32 FFleshRingEditorViewportClient::GetDebugSliceZ() const
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			return Comp->DebugSliceZ;
		}
	}
	return 32;
}

void FFleshRingEditorViewportClient::SetDebugSliceZ(int32 NewValue)
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->DebugSliceZ = NewValue;
			Invalidate();
		}
	}
}

void FFleshRingEditorViewportClient::ToggleShowBulgeHeatmap()
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowBulgeHeatmap = !Comp->bShowBulgeHeatmap;
			Invalidate();
		}
	}
}

bool FFleshRingEditorViewportClient::ShouldShowBulgeHeatmap() const
{
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			return Comp->bShowBulgeHeatmap;
		}
	}
	return false;
}
