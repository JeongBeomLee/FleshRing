// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"

class FFleshRingEditorViewportClient;

/**
 * FleshRing editor-only EdMode
 * Controls Widget show/hide
 */
class FFleshRingEdMode : public FEdMode
{
public:
	static const FEditorModeID EM_FleshRingEdModeId;

	/** Current active instance (one per Asset Editor) */
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

	/** Set ViewportClient */
	void SetViewportClient(FFleshRingEditorViewportClient* InClient) { ViewportClient = InClient; }

private:
	/** Connected ViewportClient */
	FFleshRingEditorViewportClient* ViewportClient = nullptr;
};
