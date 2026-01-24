// Copyright 2026 LgThx. All Rights Reserved.

// AdaptiveSubdivisionActor.h
// Easy-to-use test actor for adaptive subdivision

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "AdaptiveSubdivisionComponent.h"
#include "AdaptiveSubdivisionActor.generated.h"

/**
 * Actor with built-in AdaptiveSubdivisionComponent for easy testing.
 *
 * Usage:
 * 1. Place this actor in the level
 * 2. Adjust ring parameters in the details panel
 * 3. Watch the mesh subdivide and deform in real-time
 *
 * For testing ring interaction:
 * - Set RingFollowActor to make the ring follow another actor
 * - Or move the ring manually via RingCenter property
 */
UCLASS()
class FLESHRINGRUNTIME_API AAdaptiveSubdivisionActor : public AActor
{
	GENERATED_BODY()

public:
	AAdaptiveSubdivisionActor();

	// Procedural mesh component (owned by Actor for proper editor integration)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> MeshComponent;

	// Adaptive subdivision component
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Components")
	TObjectPtr<UAdaptiveSubdivisionComponent> SubdivisionComponent;

	// Optional actor for ring to follow
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Follow")
	TObjectPtr<AActor> RingFollowActor;

	// Offset from follow actor
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Follow")
	FVector RingFollowOffset = FVector::ZeroVector;

	// Enable ring following
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Follow")
	bool bEnableRingFollow = false;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
#endif
};
