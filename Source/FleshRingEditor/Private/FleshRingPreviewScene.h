// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "FleshRingTypes.h"
#include "Animation/DebugSkelMeshComponent.h"

class USkeletalMesh;
class UStaticMeshComponent;
class UFleshRingMeshComponent;
class UFleshRingComponent;
class UFleshRingAsset;
class AActor;

/**
 * Preview scene for FleshRing editor
 * Displays actual deformation using skeletal mesh and FleshRingComponent
 */
class FFleshRingPreviewScene : public FAdvancedPreviewScene
{
public:
	FFleshRingPreviewScene(const ConstructionValues& CVS);
	virtual ~FFleshRingPreviewScene();

	/** Set FleshRing Asset (update mesh + component) */
	void SetFleshRingAsset(UFleshRingAsset* InAsset);

	/** Set skeletal mesh */
	void SetSkeletalMesh(USkeletalMesh* InMesh);

	/** Refresh Ring meshes */
	void RefreshRings(const TArray<FFleshRingSettings>& Rings);

	/** Refresh preview (called when Asset changes) */
	void RefreshPreview();

	/** Update Transform for specific Ring */
	void UpdateRingTransform(int32 Index, const FTransform& Transform);

	/** Update all Ring Transforms based on Asset (lightweight update) */
	void UpdateAllRingTransforms();

	/** Set selected Ring index */
	void SetSelectedRingIndex(int32 Index);

	/** Get selected Ring index */
	int32 GetSelectedRingIndex() const { return SelectedRingIndex; }

	/** Get skeletal mesh component (DebugSkelMesh) */
	UDebugSkelMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }

	/** Get FleshRing component */
	UFleshRingComponent* GetFleshRingComponent() const { return FleshRingComponent; }

	/** Get Ring mesh component array */
	const TArray<UFleshRingMeshComponent*>& GetRingMeshComponents() const { return RingMeshComponents; }

	/** Set Ring mesh visibility */
	void SetRingMeshesVisible(bool bVisible);

	/** Check if pending Deformer initialization (check if mesh has been rendered) */
	bool IsPendingDeformerInit() const;

	/** Execute pending Deformer initialization */
	void ExecutePendingDeformerInit();

	// =====================================
	// Preview Mesh Management (separated from Asset, excluded from transaction)
	// =====================================

	/** Generate preview mesh */
	void GeneratePreviewMesh();

	/** Clear preview mesh */
	void ClearPreviewMesh();

	/** Invalidate preview mesh cache */
	void InvalidatePreviewMeshCache();

	/** Check if preview mesh is valid (including GC'd object check) */
	bool HasValidPreviewMesh() const { return PreviewSubdividedMesh != nullptr && IsValid(PreviewSubdividedMesh); }

	/** Check if preview mesh needs regeneration */
	bool NeedsPreviewMeshRegeneration() const;

	/** Check if preview mesh cache is valid (hash comparison) */
	bool IsPreviewMeshCacheValid() const;

	/** Calculate hash for current bone configuration */
	uint32 CalculatePreviewBoneConfigHash() const;

	/** Get preview mesh */
	USkeletalMesh* GetPreviewSubdividedMesh() const { return PreviewSubdividedMesh; }

private:
	/** Create preview actor */
	void CreatePreviewActor();

	/** Preview actor */
	AActor* PreviewActor = nullptr;

	/** Target skeletal mesh component (fixed bone colors via DebugSkelMesh) */
	UDebugSkelMeshComponent* SkeletalMeshComponent = nullptr;

	/** FleshRing component (handles actual deformation) */
	UFleshRingComponent* FleshRingComponent = nullptr;

	/** Ring mesh component array (for visualization) */
	TArray<UFleshRingMeshComponent*> RingMeshComponents;

	/** Currently editing Asset */
	UFleshRingAsset* CurrentAsset = nullptr;

	/** Original mesh before PreviewSubdividedMesh applied (for restoration) */
	TWeakObjectPtr<USkeletalMesh> CachedOriginalMesh;

	// =====================================
	// Preview Subdivided Mesh (separated from Asset, excluded from transaction)
	// =====================================

	/** Subdivided mesh for preview (editor-only, excluded from transaction) */
	TObjectPtr<USkeletalMesh> PreviewSubdividedMesh;

	/** Preview mesh cache validity hash */
	uint32 LastPreviewBoneConfigHash = 0;

	/** Preview mesh cache valid flag */
	bool bPreviewMeshCacheValid = false;

	/** Currently selected Ring index (-1 = no selection) */
	int32 SelectedRingIndex = -1;

	/** Ring mesh visibility state (Show Flag) */
	bool bRingMeshesVisible = true;

	/** Asset changed delegate handle (full refresh - needed when Subdivision created/removed) */
	FDelegateHandle AssetChangedDelegateHandle;

	/** Callback when Asset changes (full refresh) */
	void OnAssetChanged(UFleshRingAsset* ChangedAsset);

	/** Delegate binding/unbinding */
	void BindToAssetDelegate();
	void UnbindFromAssetDelegate();

	/** Deformer initialization pending flag */
	bool bPendingDeformerInit = false;
};
