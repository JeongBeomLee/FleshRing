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

	// UMeshDeformer interface
	virtual UMeshDeformerInstanceSettings* CreateSettingsInstance(UMeshComponent* InMeshComponent) override;
	virtual UMeshDeformerInstance* CreateInstance(UMeshComponent* InMeshComponent, UMeshDeformerInstanceSettings* InSettings) override;
};
