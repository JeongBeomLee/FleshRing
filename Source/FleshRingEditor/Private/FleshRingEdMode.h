// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"

class FFleshRingEditorViewportClient;

/**
 * FleshRing 에디터 전용 EdMode
 * Widget 표시/숨기기 제어
 */
class FFleshRingEdMode : public FEdMode
{
public:
	static const FEditorModeID EM_FleshRingEdModeId;

	/** 현재 활성 인스턴스 (Asset Editor당 하나) */
	static FFleshRingEdMode* CurrentInstance;

	FFleshRingEdMode();
	virtual ~FFleshRingEdMode();

	// FEdMode interface
	virtual bool ShouldDrawWidget() const override;
	virtual bool UsesTransformWidget() const override;
	virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override;
	virtual FVector GetWidgetLocation() const override;
	virtual bool GetCustomDrawingCoordinateSystem(FMatrix& InMatrix, void* InData) override;
	virtual bool GetCustomInputCoordinateSystem(FMatrix& InMatrix, void* InData) override;

	/** ViewportClient 설정 */
	void SetViewportClient(FFleshRingEditorViewportClient* InClient) { ViewportClient = InClient; }

private:
	/** 연결된 ViewportClient */
	FFleshRingEditorViewportClient* ViewportClient = nullptr;
};
