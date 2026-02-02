// Copyright 2026 LgThx. All Rights Reserved.

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
	// Default icon texture is lazy loaded in Draw
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

	// If BakedMesh exists, render that mesh's thumbnail
	USkeletalMesh* BakedMesh = FleshRingAsset->SubdivisionSettings.BakedMesh;
	if (BakedMesh)
	{
		DrawSkeletalMeshThumbnail(BakedMesh, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
		return;
	}

	// If no BakedMesh, render default icon
	// Lazy load default icon texture
	if (!DefaultIconTexture)
	{
		// Attempt to load from plugin Content path
		const FString IconPath = TEXT("/FleshRingPlugin/T_FleshRingAssetThumbnail");
		DefaultIconTexture = LoadObject<UTexture2D>(nullptr, *IconPath);
	}

	if (DefaultIconTexture && DefaultIconTexture->GetResource())
	{
		// Draw texture in thumbnail area
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
		// Draw simple placeholder (gray box with border)
		// NOTE: Do NOT call Super::Draw() - UDefaultSizedThumbnailRenderer doesn't implement Draw()
		// and UThumbnailRenderer::Draw() is pure virtual
		const FLinearColor BackgroundColor(0.1f, 0.1f, 0.1f, 1.0f);
		const FLinearColor BorderColor(0.3f, 0.1f, 0.15f, 1.0f);  // FleshRing theme (pinkish)

		// Background
		Canvas->DrawTile(X, Y, Width, Height, 0, 0, 1, 1, BackgroundColor);

		// Border (2 pixel)
		const int32 BorderWidth = 2;
		Canvas->DrawTile(X, Y, Width, BorderWidth, 0, 0, 1, 1, BorderColor);  // Top
		Canvas->DrawTile(X, Y + Height - BorderWidth, Width, BorderWidth, 0, 0, 1, 1, BorderColor);  // Bottom
		Canvas->DrawTile(X, Y, BorderWidth, Height, 0, 0, 1, 1, BorderColor);  // Left
		Canvas->DrawTile(X + Width - BorderWidth, Y, BorderWidth, Height, 0, 0, 1, 1, BorderColor);  // Right
	}
}

void UFleshRingAssetThumbnailRenderer::DrawSkeletalMeshThumbnail(USkeletalMesh* SkeletalMesh, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	if (!SkeletalMesh)
	{
		return;
	}

	// Directly use SkeletalMesh thumbnail renderer
	USkeletalMeshThumbnailRenderer* SkeletalMeshRenderer = GetMutableDefault<USkeletalMeshThumbnailRenderer>();
	if (SkeletalMeshRenderer)
	{
		SkeletalMeshRenderer->Draw(SkeletalMesh, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
	}
}
