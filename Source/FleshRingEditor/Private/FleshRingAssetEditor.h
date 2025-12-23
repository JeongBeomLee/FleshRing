// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"

class UFleshRingAsset;
class SFleshRingEditorViewport;
class IDetailsView;

/**
 * FleshRing Asset 전용 에디터
 * Physics Asset Editor처럼 3D 뷰포트와 Details 패널 제공
 */
class FFleshRingAssetEditor : public FAssetEditorToolkit
{
public:
	FFleshRingAssetEditor();
	virtual ~FFleshRingAssetEditor();

	/** 에디터 초기화 */
	void InitFleshRingAssetEditor(
		const EToolkitMode::Type Mode,
		const TSharedPtr<IToolkitHost>& InitToolkitHost,
		UFleshRingAsset* InAsset);

	// FAssetEditorToolkit interface
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FText GetToolkitName() const override;
	virtual FText GetToolkitToolTipText() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FString GetDocumentationLink() const override;

	// IToolkit interface
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;

	/** 편집 중인 Asset 반환 */
	UFleshRingAsset* GetEditingAsset() const { return EditingAsset; }

	/** 뷰포트 갱신 */
	void RefreshViewport();

private:
	/** Viewport 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Details 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

	/** Details View 생성 */
	void CreateDetailsView();

private:
	/** 편집 중인 Asset */
	UFleshRingAsset* EditingAsset = nullptr;

	/** 뷰포트 위젯 */
	TSharedPtr<SFleshRingEditorViewport> ViewportWidget;

	/** Details View */
	TSharedPtr<IDetailsView> DetailsView;
};
