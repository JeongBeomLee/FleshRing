// Copyright 2026 LgThx. All Rights Reserved.

// AdaptiveSubdivisionActor.cpp

#include "AdaptiveSubdivisionActor.h"

AAdaptiveSubdivisionActor::AAdaptiveSubdivisionActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Create mesh component as root (so gizmo is at mesh center)
	MeshComponent = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MeshComponent"));
	SetRootComponent(MeshComponent);
	MeshComponent->bUseAsyncCooking = true;

	// Create subdivision component and link the mesh
	SubdivisionComponent = CreateDefaultSubobject<UAdaptiveSubdivisionComponent>(TEXT("SubdivisionComponent"));
	SubdivisionComponent->ProceduralMesh = MeshComponent;
}

void AAdaptiveSubdivisionActor::BeginPlay()
{
	Super::BeginPlay();
}

void AAdaptiveSubdivisionActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Follow ring actor if enabled
	if (bEnableRingFollow && RingFollowActor && SubdivisionComponent)
	{
		FVector WorldCenter = RingFollowActor->GetActorLocation() + RingFollowOffset;
		FVector WorldDirection = RingFollowActor->GetActorForwardVector();

		SubdivisionComponent->SetRingFromWorldTransform(WorldCenter, WorldDirection);
	}
}

#if WITH_EDITOR
void AAdaptiveSubdivisionActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Ensure ProceduralMesh reference is set
	if (SubdivisionComponent && MeshComponent)
	{
		SubdivisionComponent->ProceduralMesh = MeshComponent;
		SubdivisionComponent->GenerateMesh();
	}
}
#endif
