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
		false);  // bVisible = false (UI에 표시 안 함)

	// 현재 인스턴스 저장
	CurrentInstance = this;
}

FFleshRingEdMode::~FFleshRingEdMode()
{
	// 현재 인스턴스 정리
	if (CurrentInstance == this)
	{
		CurrentInstance = nullptr;
	}
}

bool FFleshRingEdMode::ShouldDrawWidget() const
{
	// Ring이 선택된 경우에만 Widget 표시
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
	// 모든 모드 사용 가능 (None 포함)
	return (CheckMode == UE::Widget::WM_None ||
			CheckMode == UE::Widget::WM_Translate ||
			CheckMode == UE::Widget::WM_Rotate ||
			CheckMode == UE::Widget::WM_Scale);
}

FVector FFleshRingEdMode::GetWidgetLocation() const
{
	// ViewportClient의 GetWidgetLocation 사용
	if (ViewportClient)
	{
		return ViewportClient->GetWidgetLocation();
	}
	return FVector::ZeroVector;
}

bool FFleshRingEdMode::GetCustomDrawingCoordinateSystem(FMatrix& InMatrix, void* InData)
{
	// 기즈모를 AlignRotation에 맞춰 표시
	if (ViewportClient)
	{
		InMatrix = ViewportClient->GetSelectedRingAlignMatrix();
		return true;
	}
	return false;
}

bool FFleshRingEdMode::GetCustomInputCoordinateSystem(FMatrix& InMatrix, void* InData)
{
	// 입력도 AlignRotation 좌표계 사용
	if (ViewportClient)
	{
		InMatrix = ViewportClient->GetSelectedRingAlignMatrix();
		return true;
	}
	return false;
}
