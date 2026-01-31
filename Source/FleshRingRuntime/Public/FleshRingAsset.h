// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FleshRingTypes.h"
#include "FleshRingAsset.generated.h"

class UFleshRingComponent;
struct FSkeletalMaterial;

/** Delegate broadcast when asset changes (full refresh on structural changes) */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnFleshRingAssetChanged, UFleshRingAsset*);

/** Delegate broadcast when Ring selection changes (Detail Panel -> Viewport/Tree sync) */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnRingSelectionChanged, int32 /*RingIndex*/);

/**
 * Asset storing FleshRing settings
 * Create in Content Browser and reuse across multiple characters
 */
UCLASS(BlueprintType)
class FLESHRINGRUNTIME_API UFleshRingAsset : public UObject
{
	GENERATED_BODY()

public:
	UFleshRingAsset();

	// =====================================
	// Target Mesh
	// =====================================

	/** Target skeletal mesh for this asset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	TSoftObjectPtr<USkeletalMesh> TargetSkeletalMesh;

	// =====================================
	// Subdivision Settings (Skeletal Mesh Detail)
	// =====================================

	/** Subdivision settings (editor preview + runtime) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (DisplayName = "Skeletal Mesh Detail Settings"))
	FSubdivisionSettings SubdivisionSettings;

	// =====================================
	// Ring Settings
	// =====================================

	/** Ring settings array */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Settings")
	TArray<FFleshRingSettings> Rings;

	// =====================================
	// Material Layer Settings (for penetration resolution)
	// =====================================

	/**
	 * Material-layer mapping array
	 * Auto-populated when TargetSkeletalMesh is set
	 * Only Layer Type for each slot is editable
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, EditFixedSize, Category = "Material Layer Settings",
		meta = (TitleProperty = "MaterialSlotName"))
	TArray<FMaterialLayerMapping> MaterialLayerMappings;

	/**
	 * [Reserved for future use]
	 * Enable layer penetration resolution
	 * If disabled, applies pure deformation without layer order correction
	 */
	UPROPERTY()
	bool bEnableLayerPenetrationResolution = true;

	// =====================================
	// Normals
	// =====================================

	/**
	 * Enable normal recalculation
	 * - ON: Recalculate vertex normals from deformed mesh face normal averages
	 * - OFF: Use original normals (lighting may be inaccurate)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (DisplayName = "Recalculate Normals"))
	bool bEnableNormalRecompute = true;

	/**
	 * Normal recalculation method
	 * - Surface Rotation: Rotate original smooth normal by face rotation (default, smooth result)
	 * - Geometric: Face normal average (accurate TBN, faceted result)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (DisplayName = "Normal Recompute Method", EditCondition = "bEnableNormalRecompute"))
	ENormalRecomputeMethod NormalRecomputeMethod = ENormalRecomputeMethod::Geometric;

	/**
	 * Enable depth-based blending
	 * - ON: Smoothly blend recalculated and original normals based on topology depth at deformation boundaries
	 * - OFF: Use only recalculated normals (may cause sharp lighting changes at boundaries)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (DisplayName = "Depth-Based Blending", EditCondition = "bEnableNormalRecompute"))
	bool bEnableNormalHopBlending = true;

	/**
	 * Normal blending falloff curve type
	 * - Linear: Linear falloff (sharp boundary)
	 * - Quadratic: Quadratic curve (smooth)
	 * - Hermite: S-curve (smoothest, recommended)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (DisplayName = "Depth Falloff", EditCondition = "bEnableNormalRecompute && bEnableNormalHopBlending"))
	EFalloffType NormalBlendFalloffType = EFalloffType::Hermite;

	/**
	 * Enable displacement-based blending
	 * - ON: Adjust normal blending strength based on actual vertex displacement distance
	 *   - Vertices with small displacement blend closer to original normals
	 *   - Vertices with large displacement use recalculated normals
	 *   - When used with Depth-Based blending: FinalBlend = DepthBlend * DisplacementBlend
	 * - OFF: Apply same blending regardless of displacement distance
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (DisplayName = "Displacement-Based Blending", EditCondition = "bEnableNormalRecompute"))
	bool bEnableDisplacementBlending = true;

	/**
	 * Maximum displacement distance for blending (cm)
	 * - Vertices displaced beyond this distance use 100% recalculated normals
	 * - Linear interpolation between 0 and MaxDisplacement
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (DisplayName = "Max Displacement (cm)", EditCondition = "bEnableNormalRecompute && bEnableDisplacementBlending", ClampMin = "0.01", UIMin = "0.01", UIMax = "10.0"))
	float MaxDisplacementForBlend = 1.5f;

	/**
	 * Enable tangent recalculation
	 * - ON: Orthonormalize tangents to match recalculated normals for TBN matrix consistency
	 * - OFF: Use original tangents (normal map rendering may be inaccurate)
	 * - Note: Tangent recalculation is ignored when normal recalculation is disabled
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normals", meta = (DisplayName = "Recalculate Tangents", EditCondition = "bEnableNormalRecompute"))
	bool bEnableTangentRecompute = true;

	// =====================================
	// Editor Selection State (Undo-able, reset on disk save)
	// =====================================

	/** Editor selected Ring index (-1 = no selection) */
	UPROPERTY()
	int32 EditorSelectedRingIndex = -1;

	/** Editor selection type (Gizmo/Mesh) */
	UPROPERTY()
	EFleshRingSelectionType EditorSelectionType = EFleshRingSelectionType::None;

	// =====================================
	// Utility Functions
	// =====================================

	/** Add Ring */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	int32 AddRing(const FFleshRingSettings& NewRing);

	/** Remove Ring */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	bool RemoveRing(int32 Index);

	/** Get Ring count */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	int32 GetNumRings() const { return Rings.Num(); }

	/** Check if Ring name is unique (excluding specific index) */
	bool IsRingNameUnique(FName Name, int32 ExcludeIndex = INDEX_NONE) const;

	/** Generate unique Ring name (adds suffix if duplicate) */
	FName MakeUniqueRingName(FName BaseName, int32 ExcludeIndex = INDEX_NONE) const;

	/** Validity check */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	bool IsValid() const;

	// =====================================
	// Material Layer Utilities
	// =====================================

	/**
	 * Get layer type by material slot index
	 * @param MaterialSlotIndex - Material slot index to query
	 * @return Layer type of the slot (Other if no mapping)
	 */
	UFUNCTION(BlueprintPure, Category = "Material Layer Settings")
	EFleshRingLayerType GetLayerTypeForMaterialSlot(int32 MaterialSlotIndex) const;

private:
	/**
	 * Sync Material Layer Mappings with TargetSkeletalMesh slots
	 * - Preserves existing mapping LayerTypes
	 * - Adds new slots with auto-detected LayerType
	 * - Removes deleted slots
	 * Auto-called from PostEditChangeProperty when TargetSkeletalMesh changes
	 */
	void SyncMaterialLayerMappings();

	/** Auto-detect layer type from material name */
	static EFleshRingLayerType DetectLayerTypeFromMaterialName(const FSkeletalMaterial& Material);

	/**
	 * For detecting Ring count changes during Undo/Redo (not included in transaction)
	 * Not a UPROPERTY, so not restored on Undo, enabling change detection
	 */
	int32 LastKnownRingCount = 0;

public:

	/** Check if subdivided mesh exists */
	UFUNCTION(BlueprintPure, Category = "FleshRing|Subdivision")
	bool HasSubdividedMesh() const;

	/** Check if baked mesh exists */
	UFUNCTION(BlueprintPure, Category = "FleshRing|Baked")
	bool HasBakedMesh() const;

	/** Check if subdivision regeneration needed due to parameter changes */
	UFUNCTION(BlueprintPure, Category = "FleshRing|Subdivision")
	bool NeedsSubdivisionRegeneration() const;

	/** Calculate current parameter hash */
	uint32 CalculateSubdivisionParamsHash() const;

#if WITH_EDITOR
	/**
	 * Generate Subdivided SkeletalMesh (editor only)
	 * Subdivide triangles in Ring affected area and barycentric interpolate SkinWeights
	 * For runtime - partial subdivision of Ring area only
	 * Called via button in DetailCustomization
	 *
	 * @param SourceComponent - Component providing AffectedVertices data (editor preview)
	 *                          Includes Extended/Refinement areas based on SmoothingVolumeMode
	 *                          Falls back to OBB-based area if nullptr
	 */
	void GenerateSubdividedMesh(UFleshRingComponent* SourceComponent = nullptr);

	/** Clear subdivided mesh (called via button in DetailCustomization) */
	void ClearSubdividedMesh();

	// =====================================
	// Baked Mesh (runtime mesh with deformation applied)
	// =====================================

	/**
	 * Generate baked mesh (editor only)
	 * Generate final mesh with deformations (Tightness, Bulge, Smoothing) applied
	 * At runtime, uses this mesh to operate without Deformer
	 *
	 * @param SourceComponent - FleshRingComponent providing GPU deformation results
	 * @return Success status
	 */
	bool GenerateBakedMesh(UFleshRingComponent* SourceComponent);

	/** Clear baked mesh */
	void ClearBakedMesh();

	/**
	 * Generate skinned ring meshes for runtime deformation
	 * Ring meshes are converted to SkeletalMesh with bone weights sampled from nearby skin vertices
	 * This allows ring meshes to deform with twist bones like skin vertices
	 *
	 * @param SourceMesh - Character's SkeletalMesh to sample bone weights from
	 */
	void GenerateSkinnedRingMeshes(USkeletalMesh* SourceMesh);

	/** Check if bake regeneration needed due to parameter changes */
	bool NeedsBakeRegeneration() const;

	/** Calculate bake parameter hash (includes Ring settings + deformation parameters) */
	uint32 CalculateBakeParamsHash() const;

	/**
	 * Cleanup orphaned meshes accumulated in asset
	 * Call when previous versions accumulated BakedMesh_1, BakedMesh_2... etc.
	 * Removes SkeletalMeshes except currently used SubdividedMesh and BakedMesh
	 * @return Number of orphaned meshes removed
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "FleshRing|Maintenance")
	int32 CleanupOrphanedMeshes();
#endif

	/** Called after asset load - reset editor selection state */
	virtual void PostLoad() override;

#if WITH_EDITOR
	/** Called before asset save - perform auto-bake */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif

#if WITH_EDITOR
	/** Called when property changes in editor */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Called after Undo/Redo transaction completes - recover damaged mesh */
	virtual void PostTransacted(const FTransactionObjectEvent& TransactionEvent) override;

	/** Asset change delegate - full refresh on structural changes */
	FOnFleshRingAssetChanged OnAssetChanged;

	/** Ring selection change delegate - Detail Panel -> Viewport/Tree sync */
	FOnRingSelectionChanged OnRingSelectionChanged;

	/**
	 * Set Ring selection (includes delegate call)
	 * Used for Viewport/Tree sync when Ring is clicked in Detail Panel
	 */
	void SetEditorSelectedRingIndex(int32 RingIndex, EFleshRingSelectionType SelectionType);
#endif
};
