// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformer.h"
#include "FleshRingDeformer.generated.h"

class UFleshRingDeformerInstance;

UCLASS(Blueprintable, BlueprintType, Meta = (DisplayName = "Flesh Ring Deformer"))
class FLESHRINGRUNTIME_API UFleshRingDeformer : public UMeshDeformer
{
	GENERATED_BODY()

public:
	UFleshRingDeformer();

	// Wave deformation parameters for feasibility test
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Settings")
	float WaveAmplitude = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Settings")
	float WaveFrequency = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Settings")
	float WaveSpeed = 1.0f;

	// Inertia settings - makes mesh react to movement velocity
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inertia Settings", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float InertiaStrength = 1.0f;

	// Bounds 확장 배율 (VSM 등 캐싱 시스템의 정상 작동을 위해 Deformer 변형량에 맞게 조정)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounds Settings", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float BoundsScale = 2.0f;

	// 매 프레임 Bounds Invalidation 강제 (VSM 캐시 문제 완전 해결, 약간의 성능 비용)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounds Settings")
	bool bForceBoundsUpdate = false;

	// UMeshDeformer interface
	virtual UMeshDeformerInstanceSettings* CreateSettingsInstance(UMeshComponent* InMeshComponent) override;
	virtual UMeshDeformerInstance* CreateInstance(UMeshComponent* InMeshComponent, UMeshDeformerInstanceSettings* InSettings) override;
};
