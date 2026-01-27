// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "FleshRingMeshComponent.generated.h"

/**
 * Custom StaticMeshComponent for Ring mesh
 * Provides higher picking priority than bones via custom SceneProxy in editor
 */
UCLASS()
class FLESHRINGRUNTIME_API UFleshRingMeshComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UFleshRingMeshComponent();

	/** Set Ring index (used by HitProxy) */
	void SetRingIndex(int32 InRingIndex) { RingIndex = InRingIndex; }

	/** Return Ring index */
	int32 GetRingIndex() const { return RingIndex; }

	// UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

private:
	/** Ring index */
	int32 RingIndex = INDEX_NONE;
};
