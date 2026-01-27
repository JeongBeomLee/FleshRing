// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformer.h"
#include "FleshRingDeformer.generated.h"

class UFleshRingDeformerInstance;
class UFleshRingComponent;

UCLASS(Blueprintable, BlueprintType, Meta = (DisplayName = "Flesh Ring Deformer"))
class FLESHRINGRUNTIME_API UFleshRingDeformer : public UMeshDeformer
{
	GENERATED_BODY()

public:
	UFleshRingDeformer();

	/** Return currently active DeformerInstance (nullptr if none) */
	UFleshRingDeformerInstance* GetActiveInstance() const { return ActiveInstance.Get(); }

	// UMeshDeformer interface
	virtual UMeshDeformerInstanceSettings* CreateSettingsInstance(UMeshComponent* InMeshComponent) override;
	virtual UMeshDeformerInstance* CreateInstance(UMeshComponent* InMeshComponent, UMeshDeformerInstanceSettings* InSettings) override;

private:
	/** Cache created DeformerInstance (access via GetActiveInstance()) */
	UPROPERTY(Transient)
	TWeakObjectPtr<UFleshRingDeformerInstance> ActiveInstance;

	/** FleshRingComponent that creates/manages this Deformer (supports multi-component environment) */
	UPROPERTY(Transient)
	TWeakObjectPtr<UFleshRingComponent> OwnerFleshRingComponent;

public:
	/** Set Owner FleshRingComponent (called from SetupDeformer) */
	void SetOwnerFleshRingComponent(UFleshRingComponent* InComponent);

	/** Return Owner FleshRingComponent */
	UFleshRingComponent* GetOwnerFleshRingComponent() const;
};
