// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "FleshRingAssetThumbnailRenderer.generated.h"

class UFleshRingAsset;
class UTexture2D;

/**
 * FleshRingAsset용 썸네일 렌더러
 * - BakedMesh 있음: BakedMesh 썸네일 렌더링
 * - BakedMesh 없음: 기본 아이콘 이미지 렌더링
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
	/** 기본 아이콘 텍스처 (BakedMesh 없을 때 사용) */
	UPROPERTY()
	TObjectPtr<UTexture2D> DefaultIconTexture;

	/** SkeletalMesh 썸네일 렌더링용 */
	void DrawSkeletalMeshThumbnail(class USkeletalMesh* SkeletalMesh, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily);
};
