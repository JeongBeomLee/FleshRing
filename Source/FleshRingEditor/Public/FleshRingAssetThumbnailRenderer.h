// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "FleshRingAssetThumbnailRenderer.generated.h"

class UFleshRingAsset;
class UTexture2D;

/**
 * Thumbnail renderer for FleshRingAsset
 * - With BakedMesh: Render BakedMesh thumbnail
 * - Without BakedMesh: Render default icon image
 */
UCLASS()
class FLESHRINGEDITOR_API UFleshRingAssetThumbnailRenderer : public UDefaultSizedThumbnailRenderer
{
	GENERATED_BODY()

public:
	UFleshRingAssetThumbnailRenderer();

	// UThumbnailRenderer interface
	virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
	virtual bool CanVisualizeAsset(UObject* Object) override;

private:
	/** Default icon texture (used when BakedMesh is missing) */
	UPROPERTY()
	TObjectPtr<UTexture2D> DefaultIconTexture;

	/** For SkeletalMesh thumbnail rendering */
	void DrawSkeletalMeshThumbnail(class USkeletalMesh* SkeletalMesh, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily);
};
