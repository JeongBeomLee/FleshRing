// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingEditorViewportClient.h"
#include "FleshRingPreviewScene.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"

FFleshRingEditorViewportClient::FFleshRingEditorViewportClient(
	FFleshRingPreviewScene* InPreviewScene,
	const TWeakPtr<SFleshRingEditorViewport>& InViewportWidget)
	: FEditorViewportClient(nullptr, InPreviewScene, StaticCastSharedPtr<SEditorViewport>(InViewportWidget.Pin()))
	, PreviewScene(InPreviewScene)
	, ViewportWidget(InViewportWidget)
{
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
	EngineShowFlags.SetBones(true);

	// 조명 설정
	EngineShowFlags.SetLighting(true);
	EngineShowFlags.SetPostProcessing(true);
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

	// Ring 기즈모 그리기
	DrawRingGizmos(PDI);
}

void FFleshRingEditorViewportClient::DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas)
{
	FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);
}

bool FFleshRingEditorViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	// F키로 메시에 포커스
	if (EventArgs.Key == EKeys::F && EventArgs.Event == IE_Pressed)
	{
		FocusOnMesh();
		return true;
	}

	return FEditorViewportClient::InputKey(EventArgs);
}

void FFleshRingEditorViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);

	// TODO: Phase 2 - HitProxy를 사용한 Ring 선택 구현
}

void FFleshRingEditorViewportClient::SetAsset(UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	if (PreviewScene && InAsset)
	{
		// 스켈레탈 메시 설정
		USkeletalMesh* SkelMesh = InAsset->TargetSkeletalMesh.LoadSynchronous();
		PreviewScene->SetSkeletalMesh(SkelMesh);

		// Ring 메시 갱신
		PreviewScene->RefreshRings(InAsset->Rings);

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

		// Ring 색상 (선택됨: 노랑, 기본: 시안)
		FLinearColor RingColor = (i == SelectedIndex)
			? FLinearColor::Yellow
			: FLinearColor(0.0f, 1.0f, 1.0f, 0.8f);

		// Ring 원 그리기
		float Radius = Ring.RingRadius;
		int32 Segments = 32;

		for (int32 s = 0; s < Segments; ++s)
		{
			float Angle1 = (float)s / Segments * 2.0f * PI;
			float Angle2 = (float)(s + 1) / Segments * 2.0f * PI;

			FVector P1 = BoneLocation + BoneTransform.GetRotation().RotateVector(
				FVector(FMath::Cos(Angle1) * Radius, FMath::Sin(Angle1) * Radius, 0.0f));
			FVector P2 = BoneLocation + BoneTransform.GetRotation().RotateVector(
				FVector(FMath::Cos(Angle2) * Radius, FMath::Sin(Angle2) * Radius, 0.0f));

			PDI->DrawLine(P1, P2, RingColor, SDPG_Foreground, 2.0f);
		}

		// 본 위치에 작은 구 표시
		DrawWireSphere(PDI, BoneLocation, RingColor, 2.0f, 8, SDPG_Foreground);
	}
}
