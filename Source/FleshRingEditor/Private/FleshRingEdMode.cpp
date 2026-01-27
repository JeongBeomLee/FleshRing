// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingEdMode.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingPreviewScene.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "Components/SkeletalMeshComponent.h"

const FEditorModeID FFleshRingEdMode::EM_FleshRingEdModeId = TEXT("EM_FleshRingEdMode");
FFleshRingEdMode* FFleshRingEdMode::CurrentInstance = nullptr;

FFleshRingEdMode::FFleshRingEdMode()
{
	Info = FEditorModeInfo(
		EM_FleshRingEdModeId,
		NSLOCTEXT("FleshRingEdMode", "ModeName", "FleshRing"),
		FSlateIcon(),
		false);  // bVisible = false (don't show in UI)

	// Store current instance
	CurrentInstance = this;
}

FFleshRingEdMode::~FFleshRingEdMode()
{
	// Clean up current instance
	if (CurrentInstance == this)
	{
		CurrentInstance = nullptr;
	}
}

bool FFleshRingEdMode::ShouldDrawWidget() const
{
	// Only show Widget when Ring is selected
	if (ViewportClient)
	{
		EFleshRingSelectionType SelectionType = ViewportClient->GetSelectionType();
		if (SelectionType != EFleshRingSelectionType::None)
		{
			FFleshRingPreviewScene* PreviewScene = ViewportClient->GetPreviewScene();
			if (PreviewScene && PreviewScene->GetSelectedRingIndex() >= 0)
			{
				return true;
			}
		}
	}
	return false;
}

bool FFleshRingEdMode::UsesTransformWidget() const
{
	return true;
}

bool FFleshRingEdMode::UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const
{
	// All modes available (including None)
	return (CheckMode == UE::Widget::WM_None ||
			CheckMode == UE::Widget::WM_Translate ||
			CheckMode == UE::Widget::WM_Rotate ||
			CheckMode == UE::Widget::WM_Scale);
}

FVector FFleshRingEdMode::GetWidgetLocation() const
{
	// Use ViewportClient's GetWidgetLocation
	if (ViewportClient)
	{
		return ViewportClient->GetWidgetLocation();
	}
	return FVector::ZeroVector;
}

bool FFleshRingEdMode::GetCustomDrawingCoordinateSystem(FMatrix& InMatrix, void* InData)
{
	// Display gizmo aligned to AlignRotation
	if (ViewportClient)
	{
		InMatrix = ViewportClient->GetSelectedRingAlignMatrix();
		return true;
	}
	return false;
}

bool FFleshRingEdMode::GetCustomInputCoordinateSystem(FMatrix& InMatrix, void* InData)
{
	// Input also uses AlignRotation coordinate system
	if (ViewportClient)
	{
		InMatrix = ViewportClient->GetSelectedRingAlignMatrix();
		return true;
	}
	return false;
}
