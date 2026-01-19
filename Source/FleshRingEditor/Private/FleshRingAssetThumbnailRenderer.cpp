// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetThumbnailRenderer.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ThumbnailRendering/SkeletalMeshThumbnailRenderer.h"

UFleshRingAssetThumbnailRenderer::UFleshRingAssetThumbnailRenderer()
{
	// 기본 아이콘 텍스처는 Draw에서 lazy load
	DefaultIconTexture = nullptr;
}

bool UFleshRingAssetThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
	return Object && Object->IsA<UFleshRingAsset>();
}

void UFleshRingAssetThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	UFleshRingAsset* FleshRingAsset = Cast<UFleshRingAsset>(Object);
	if (!FleshRingAsset)
	{
		return;
	}

	// BakedMesh가 있으면 해당 메시 썸네일 렌더링
	USkeletalMesh* BakedMesh = FleshRingAsset->SubdivisionSettings.BakedMesh;
	if (BakedMesh)
	{
		DrawSkeletalMeshThumbnail(BakedMesh, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
		return;
	}

	// BakedMesh 없으면 기본 아이콘 렌더링
	// Lazy load 기본 아이콘 텍스처
	if (!DefaultIconTexture)
	{
		// 플러그인 Content 경로에서 로드 시도
		const FString IconPath = TEXT("/FleshRingPlugin/FleshRingAssetThumbnail");
		DefaultIconTexture = LoadObject<UTexture2D>(nullptr, *IconPath);
	}

	if (DefaultIconTexture && DefaultIconTexture->GetResource())
	{
		// 텍스처를 썸네일 영역에 그리기
		FCanvasTileItem TileItem(
			FVector2D(X, Y),
			DefaultIconTexture->GetResource(),
			FVector2D(Width, Height),
			FLinearColor::White
		);
		TileItem.BlendMode = SE_BLEND_Opaque;
		Canvas->DrawItem(TileItem);
	}
	else
	{
		// 아이콘도 없으면 기본 에셋 썸네일 (큐브 아이콘)
		Super::Draw(Object, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
	}
}

void UFleshRingAssetThumbnailRenderer::DrawSkeletalMeshThumbnail(USkeletalMesh* SkeletalMesh, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	if (!SkeletalMesh)
	{
		return;
	}

	// SkeletalMesh 썸네일 렌더러 직접 사용
	USkeletalMeshThumbnailRenderer* SkeletalMeshRenderer = GetMutableDefault<USkeletalMeshThumbnailRenderer>();
	if (SkeletalMeshRenderer)
	{
		SkeletalMeshRenderer->Draw(SkeletalMesh, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
	}
}
