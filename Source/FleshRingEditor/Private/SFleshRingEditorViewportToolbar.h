// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SCommonEditorViewportToolbarBase.h"

class SFleshRingEditorViewport;

/**
 * FleshRing 에디터 뷰포트 툴바
 * 뷰 모드, Show 플래그, 카메라 설정 등 제공
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
	/** 뷰포트 참조 */
	TWeakPtr<SFleshRingEditorViewport> Viewport;
};
