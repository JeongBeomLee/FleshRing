// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;
class IPropertyHandle;
class UFleshRingComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class FAssetThumbnailPool;
struct FAssetData;

/**
 * Detail Panel customizer for UFleshRingComponent
 * Handles property grouping, category organization, custom widgets, etc.
 */
class FFleshRingDetailCustomization : public IDetailCustomization
{
public:
	/** Create DetailCustomization instance (factory function) */
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** IDetailCustomization interface implementation */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** Cache selected components */
	TArray<TWeakObjectPtr<UObject>> SelectedObjects;

	/** Cached property handle for TargetSkeletalMeshComponent */
	TSharedPtr<IPropertyHandle> TargetSkeletalMeshPropertyHandle;

	/** FleshRingAsset filtering: Only show Assets matching Owner's SkeletalMesh */
	bool OnShouldFilterAsset(const FAssetData& AssetData) const;

	/** Get Owner's SkeletalMesh */
	USkeletalMesh* GetOwnerSkeletalMesh() const;

	/** Get first selected FleshRingComponent */
	UFleshRingComponent* GetFirstSelectedComponent() const;

	/** Get all SkeletalMeshComponents from Owner Actor */
	TArray<USkeletalMeshComponent*> GetOwnerSkeletalMeshComponents() const;

	/** Option item: DisplayName + ComponentPropertyName */
	struct FTargetMeshOption
	{
		FString DisplayName;      // User-friendly name (shown in UI)
		FName ComponentProperty;  // FName for FComponentReference
	};

	/** Refresh combo box options */
	void RefreshTargetMeshOptions();

	/** ComboBox callbacks */
	TSharedRef<SWidget> GenerateTargetMeshComboItem(TSharedPtr<FTargetMeshOption> InItem);
	void OnTargetMeshSelectionChanged(TSharedPtr<FTargetMeshOption> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurrentTargetMeshText() const;

	/** Cached combo box options */
	TArray<TSharedPtr<FTargetMeshOption>> TargetMeshOptions;

	/** Currently selected option */
	TSharedPtr<FTargetMeshOption> CurrentTargetMeshSelection;

	/** True if current selection is invalid (component was renamed/removed) */
	bool bCurrentSelectionInvalid = false;

	/** Invalid component name (for display only) */
	FName InvalidComponentName;

	/** ComboBox widget reference for refresh */
	TSharedPtr<SComboBox<TSharedPtr<FTargetMeshOption>>> TargetMeshComboBox;
};
