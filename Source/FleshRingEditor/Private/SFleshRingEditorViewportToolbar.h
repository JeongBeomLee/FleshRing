// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SCommonEditorViewportToolbarBase.h"

class SFleshRingEditorViewport;

/**
 * FleshRing editor viewport toolbar
 * Provides view mode, show flags, camera settings, etc.
 */
class SFleshRingEditorViewportToolbar : public SCommonEditorViewportToolbarBase
{
public:
	SLATE_BEGIN_ARGS(SFleshRingEditorViewportToolbar) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedPtr<SFleshRingEditorViewport> InViewport);

	// SCommonEditorViewportToolbarBase interface
	virtual TSharedRef<SWidget> GenerateShowMenu() const override;

private:
	/** Viewport reference */
	TWeakPtr<SFleshRingEditorViewport> Viewport;
};
