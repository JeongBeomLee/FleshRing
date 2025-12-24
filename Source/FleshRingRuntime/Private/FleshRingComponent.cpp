// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/VolumeTexture.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingComponent, Log, All);

UFleshRingComponent::UFleshRingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UFleshRingComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bEnableFleshRing)
	{
		ResolveTargetMesh();
		SetupDeformer();
		GenerateSDF();
	}
}

void UFleshRingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CleanupDeformer();
	Super::EndPlay(EndPlayReason);
}

void UFleshRingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnableFleshRing)
	{
		return;
	}

	// NOTE: MarkRenderDynamicDataDirty/MarkRenderTransformDirty는 TickComponent에서 호출하지 않음
	// Optimus 방식: 엔진의 SendRenderDynamicData_Concurrent()가 자동으로 deformer의 EnqueueWork를 호출
	// 초기화 시점(SetupDeformer)에서만 MarkRenderStateDirty/MarkRenderDynamicDataDirty 호출

	// SDF 업데이트 (UpdateMode에 따라)
	if (FleshRingAsset && FleshRingAsset->SdfSettings.UpdateMode == EFleshRingSdfUpdateMode::OnTick)
	{
		GenerateSDF();
	}
}

void UFleshRingComponent::ResolveTargetMesh()
{
	// 수동 지정 모드
	if (bUseCustomTarget)
	{
		if (CustomTargetMesh)
		{
			ResolvedTargetMesh = CustomTargetMesh;
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Using custom target mesh '%s'"),
				*CustomTargetMesh->GetName());
		}
		else
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: bUseCustomTarget is true but CustomTargetMesh is null"));
		}
		return;
	}

	// 자동 탐색 모드: Owner에서 SkeletalMeshComponent 찾기
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: No owner actor found"));
		return;
	}

	// Owner의 모든 컴포넌트에서 SkeletalMeshComponent 탐색
	TArray<USkeletalMeshComponent*> SkeletalMeshComponents;
	Owner->GetComponents<USkeletalMeshComponent>(SkeletalMeshComponents);

	if (SkeletalMeshComponents.Num() == 0)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: No SkeletalMeshComponent found on owner '%s'"),
			*Owner->GetName());
		return;
	}

	// 첫 번째 SkeletalMeshComponent 사용
	ResolvedTargetMesh = SkeletalMeshComponents[0];
	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Auto-discovered target mesh '%s' on owner '%s'"),
		*SkeletalMeshComponents[0]->GetName(), *Owner->GetName());

	if (SkeletalMeshComponents.Num() > 1)
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Found %d SkeletalMeshComponents, using first one. Use bUseCustomTarget for manual selection."),
			SkeletalMeshComponents.Num());
	}
}

void UFleshRingComponent::SetupDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Cannot setup deformer - no target mesh"));
		return;
	}

	// 내부 Deformer 생성
	InternalDeformer = NewObject<UFleshRingDeformer>(this, TEXT("InternalFleshRingDeformer"));
	if (!InternalDeformer)
	{
		UE_LOG(LogFleshRingComponent, Error, TEXT("FleshRingComponent: Failed to create internal deformer"));
		return;
	}

	// SkeletalMeshComponent에 Deformer 등록
	TargetMesh->SetMeshDeformer(InternalDeformer);

	// Bounds 확장: Deformer 변형이 원래 bounds를 벗어날 수 있으므로
	// VSM(Virtual Shadow Maps) 등 bounds 기반 캐싱 시스템이 정상 작동하도록 확장
	TargetMesh->SetBoundsScale(BoundsScale);

	// Optimus와 동일하게 초기화 시점에 render state 갱신 요청
	// - MarkRenderStateDirty: PassthroughVertexFactory 생성을 위해 render state 재생성
	// - MarkRenderDynamicDataDirty: 동적 데이터 갱신 요청
	// 주의: TickComponent에서는 호출하지 않음 (엔진이 자동으로 처리)
	TargetMesh->MarkRenderStateDirty();
	TargetMesh->MarkRenderDynamicDataDirty();

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Deformer registered to target mesh '%s'"),
		*TargetMesh->GetName());
}

void UFleshRingComponent::CleanupDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (TargetMesh && InternalDeformer)
	{
		TargetMesh->SetMeshDeformer(nullptr);
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Deformer unregistered from target mesh"));
	}

	InternalDeformer = nullptr;
	ResolvedTargetMesh.Reset();
	SDFVolumeTexture = nullptr;
}

void UFleshRingComponent::GenerateSDF()
{
	if (!FleshRingAsset)
	{
		return;
	}

	// 각 Ring의 RingMesh에서 SDF 생성
	for (const FFleshRingSettings& Ring : FleshRingAsset->Rings)
	{
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (!RingMesh)
		{
			continue;
		}

		// TODO: RingMesh로부터 SDF 3D 텍스처 생성
		// 1. StaticMesh의 버텍스 데이터 추출
		// 2. GPU에서 JFA(Jump Flooding Algorithm)로 SDF 계산
		// 3. SDFVolumeTexture에 결과 저장
		// 4. InternalDeformer에 SDF 텍스처 전달
	}

	// 현재는 placeholder - 실제 SDF 생성 로직은 별도 구현 필요
}

void UFleshRingComponent::UpdateSDF()
{
	GenerateSDF();
}

void UFleshRingComponent::ApplyAsset()
{
	if (!FleshRingAsset)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyAsset called but FleshRingAsset is null"));
		return;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Applying asset '%s'"), *FleshRingAsset->GetName());

	// 기존 설정 정리 후 재설정
	CleanupDeformer();

	if (bEnableFleshRing)
	{
		ResolveTargetMesh();
		SetupDeformer();
		GenerateSDF();
	}
}
