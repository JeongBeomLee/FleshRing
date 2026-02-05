// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "FleshRingSubdivisionProcessor.h"
#include "FleshRingSkinnedMeshGenerator.h"

#if WITH_EDITOR
#include "UObject/ObjectSaveContext.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "Animation/Skeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "BoneWeights.h"
#include "RenderingThread.h"
#include "FleshRingComponent.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingAffectedVertices.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/TransactionObjectEvent.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingAsset, Log, All);

UFleshRingAsset::UFleshRingAsset()
{
}

bool UFleshRingAsset::HasSubdividedMesh() const
{
	return SubdivisionSettings.SubdividedMesh.Get() != nullptr;
}

bool UFleshRingAsset::HasBakedMesh() const
{
	return SubdivisionSettings.BakedMesh.Get() != nullptr;
}

void UFleshRingAsset::PostLoad()
{
	Super::PostLoad();

	// ================================================================
	// AffectedLayerMask Other bit migration
	//
	// NOTE [Migration]: Reference this pattern when adding new layer bits
	//   1. Existing assets won't have new bits (0)
	//   2. Only add new bit if at least one layer is active
	//   3. Don't touch if all are 0 (may be intentionally disabled)
	//
	// When adding new layers in the future:
	//   - Define new bit (e.g., EFleshRingLayerMask::NewLayer = 1 << 5)
	//   - Add migration code similar to below
	//   - Include existing bits in KnownBits (including Other)
	// ================================================================
	constexpr int32 OtherBit = static_cast<int32>(EFleshRingLayerMask::Other);
	constexpr int32 KnownBitsBeforeOther =
		static_cast<int32>(EFleshRingLayerMask::Skin) |
		static_cast<int32>(EFleshRingLayerMask::Stocking) |
		static_cast<int32>(EFleshRingLayerMask::Underwear) |
		static_cast<int32>(EFleshRingLayerMask::Outerwear);

	for (int32 RingIndex = 0; RingIndex < Rings.Num(); ++RingIndex)
	{
		FFleshRingSettings& Ring = Rings[RingIndex];

		// Add Other bit if missing and at least one existing layer is active
		const bool bHasOtherBit = (Ring.AffectedLayerMask & OtherBit) != 0;
		const bool bHasAnyKnownLayer = (Ring.AffectedLayerMask & KnownBitsBeforeOther) != 0;

		if (!bHasOtherBit && bHasAnyKnownLayer)
		{
			Ring.AffectedLayerMask |= OtherBit;
			MarkPackageDirty();

			UE_LOG(LogFleshRingAsset, Log,
				TEXT("PostLoad Migration: Added Other bit to AffectedLayerMask for Ring '%s'"),
				*Ring.GetDisplayName(RingIndex));
		}
	}

	// Reset editor selection state when asset is loaded
	// (Serialized via UPROPERTY(), but always reset after load)
	EditorSelectedRingIndex = -1;
	EditorSelectionType = EFleshRingSelectionType::None;
}

#if WITH_EDITOR
void UFleshRingAsset::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);
	// TODO: Auto-bake logic to be implemented. Delete this override if not needed.
}
#endif

int32 UFleshRingAsset::AddRing(const FFleshRingSettings& NewRing)
{
	return Rings.Add(NewRing);
}

bool UFleshRingAsset::RemoveRing(int32 Index)
{
	if (Rings.IsValidIndex(Index))
	{
		Rings.RemoveAt(Index);
		return true;
	}
	return false;
}

bool UFleshRingAsset::IsRingNameUnique(FName Name, int32 ExcludeIndex) const
{
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		if (i != ExcludeIndex && Rings[i].RingName == Name)
		{
			return false;
		}
	}
	return true;
}

FName UFleshRingAsset::MakeUniqueRingName(FName BaseName, int32 ExcludeIndex) const
{
	// Return as-is if already unique
	if (IsRingNameUnique(BaseName, ExcludeIndex))
	{
		return BaseName;
	}

	// Use FName's built-in numbering system (same as Unreal sockets)
	int32 NewNumber = BaseName.GetNumber();
	while (!IsRingNameUnique(FName(BaseName, NewNumber), ExcludeIndex))
	{
		++NewNumber;
	}

	return FName(BaseName, NewNumber);
}

bool UFleshRingAsset::IsValid() const
{
	// Target mesh must be set
	if (TargetSkeletalMesh.IsNull())
	{
		return false;
	}

	// At least one Ring required
	if (Rings.Num() == 0)
	{
		return false;
	}

	// All Rings must have valid bone names
	for (const FFleshRingSettings& Ring : Rings)
	{
		if (Ring.BoneName == NAME_None)
		{
			return false;
		}
	}

	return true;
}

// =====================================
// Material Layer Utilities
// =====================================

EFleshRingLayerType UFleshRingAsset::GetLayerTypeForMaterialSlot(int32 MaterialSlotIndex) const
{
	for (const FMaterialLayerMapping& Mapping : MaterialLayerMappings)
	{
		if (Mapping.MaterialSlotIndex == MaterialSlotIndex)
		{
			return Mapping.LayerType;
		}
	}
	return EFleshRingLayerType::Other;
}

void UFleshRingAsset::SyncMaterialLayerMappings()
{
	// Clear array if no target mesh
	if (TargetSkeletalMesh.IsNull())
	{
		MaterialLayerMappings.Empty();
		return;
	}

	USkeletalMesh* Mesh = TargetSkeletalMesh.LoadSynchronous();
	if (!Mesh)
	{
		MaterialLayerMappings.Empty();
		return;
	}

	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	const int32 NumSlots = Materials.Num();

	// Create map to preserve LayerType from existing mappings
	TMap<int32, EFleshRingLayerType> ExistingLayerTypes;
	for (const FMaterialLayerMapping& Mapping : MaterialLayerMappings)
	{
		ExistingLayerTypes.Add(Mapping.MaterialSlotIndex, Mapping.LayerType);
	}

	// Rebuild array completely (Empty + Add instead of SetNum)
	// SetNum reuses existing elements if size matches → editor UI won't detect changes
	MaterialLayerMappings.Empty();
	MaterialLayerMappings.Reserve(NumSlots);

	for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
	{
		const FSkeletalMaterial& SkeletalMaterial = Materials[SlotIndex];

		// Preserve existing LayerType, auto-detect if not found
		EFleshRingLayerType LayerType;
		if (const EFleshRingLayerType* ExistingType = ExistingLayerTypes.Find(SlotIndex))
		{
			LayerType = *ExistingType;
		}
		else
		{
			LayerType = DetectLayerTypeFromMaterialName(SkeletalMaterial);
		}

		MaterialLayerMappings.Add(FMaterialLayerMapping(
			SlotIndex,
			SkeletalMaterial.MaterialSlotName,
			LayerType
		));
	}

#if WITH_EDITOR
	Modify();
#endif
}

EFleshRingLayerType UFleshRingAsset::DetectLayerTypeFromMaterialName(const FSkeletalMaterial& Material)
{
	FString MaterialName = Material.MaterialSlotName.ToString();
	if (Material.MaterialInterface)
	{
		MaterialName = Material.MaterialInterface->GetName();
	}
	FString LowerName = MaterialName.ToLower();

	// Stocking keywords (priority)
	static const TArray<FString> StockingKeywords = {
		TEXT("stocking"), TEXT("tight"), TEXT("pantyhose"),
		TEXT("hosiery"), TEXT("nylon"), TEXT("sock"), TEXT("legging")
	};
	for (const FString& Keyword : StockingKeywords)
	{
		if (LowerName.Contains(Keyword))
		{
			return EFleshRingLayerType::Stocking;
		}
	}

	// Underwear keywords
	static const TArray<FString> UnderwearKeywords = {
		TEXT("underwear"), TEXT("bra"), TEXT("panty"), TEXT("panties"),
		TEXT("lingerie"), TEXT("bikini"), TEXT("brief"), TEXT("thong")
	};
	for (const FString& Keyword : UnderwearKeywords)
	{
		if (LowerName.Contains(Keyword))
		{
			return EFleshRingLayerType::Underwear;



		}
	}

	// Outerwear keywords
	static const TArray<FString> OuterwearKeywords = {
		TEXT("cloth"), TEXT("dress"), TEXT("shirt"), TEXT("skirt"),
		TEXT("jacket"), TEXT("coat"), TEXT("pants"), TEXT("jeans")
	};
	for (const FString& Keyword : OuterwearKeywords)
	{
		if (LowerName.Contains(Keyword))
		{
			return EFleshRingLayerType::Outerwear;
		}
	}

	// Skin keywords
	static const TArray<FString> SkinKeywords = {
		TEXT("skin"), TEXT("body"), TEXT("flesh"), TEXT("face"),
		TEXT("hand"), TEXT("leg"), TEXT("arm"), TEXT("foot"), TEXT("head")
	};
	for (const FString& Keyword : SkinKeywords)
	{
		if (LowerName.Contains(Keyword))
		{
			return EFleshRingLayerType::Skin;
		}
	}

	return EFleshRingLayerType::Other;
}

bool UFleshRingAsset::NeedsSubdivisionRegeneration() const
{
	if (!SubdivisionSettings.bEnableSubdivision)
	{
		return false;
	}

	if (!SubdivisionSettings.SubdividedMesh)
	{
		return true;
	}

	return CalculateSubdivisionParamsHash() != SubdivisionSettings.SubdivisionParamsHash;
}

uint32 UFleshRingAsset::CalculateSubdivisionParamsHash() const
{
	uint32 Hash = 0;

	// Target mesh path
	if (!TargetSkeletalMesh.IsNull())
	{
		Hash = HashCombine(Hash, GetTypeHash(TargetSkeletalMesh.ToSoftObjectPath().ToString()));
	}

	// Subdivision settings
	Hash = HashCombine(Hash, GetTypeHash(SubdivisionSettings.bEnableSubdivision));
	Hash = HashCombine(Hash, GetTypeHash(SubdivisionSettings.MaxSubdivisionLevel));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(SubdivisionSettings.MinEdgeLength * 100)));

	// Ring settings (influence region related - affects subdivision target triangle selection)
	for (const FFleshRingSettings& Ring : Rings)
	{
		// Basic Ring identification
		Hash = HashCombine(Hash, GetTypeHash(Ring.BoneName.ToString()));

		// InfluenceMode (Auto vs VirtualRing)
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Ring.InfluenceMode)));

		// Auto mode: RingMesh bounds + transform affect region
		if (!Ring.RingMesh.IsNull())
		{
			Hash = HashCombine(Hash, GetTypeHash(Ring.RingMesh.ToSoftObjectPath().ToString()));
		}
		Hash = HashCombine(Hash, GetTypeHash(Ring.MeshOffset.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Ring.MeshRotation.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Ring.MeshScale.ToString()));

		// VirtualRing mode: Torus parameters affect region
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingRadius * 10)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingHeight * 10)));
		Hash = HashCombine(Hash, GetTypeHash(Ring.RingOffset.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Ring.RingRotation.ToString()));

		// Region expansion parameters (Refinement, Smoothing Volume)
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnableRefinement));
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Ring.SmoothingVolumeMode)));
		Hash = HashCombine(Hash, GetTypeHash(Ring.MaxSmoothingHops));
	}

	return Hash;
}

#if WITH_EDITOR

// ============================================
// Subdivision Region Calculation Helper Functions
// ============================================

namespace SubdivisionHelpers
{
	/** Quantize position to integer cells (for UV Seam welding) */
	FORCEINLINE FIntVector QuantizePosition(const FVector& Position, float CellSize = 0.01f)
	{
		return FIntVector(
			FMath::FloorToInt(Position.X / CellSize),
			FMath::FloorToInt(Position.Y / CellSize),
			FMath::FloorToInt(Position.Z / CellSize)
		);
	}

	/**
	 * Position-based vertex grouping (UV Seam welding)
	 * Groups vertices at the same 3D position to process together
	 * @param Positions - Vertex position array
	 * @param CellSize - Quantization cell size (cm), vertices within this range are considered same position
	 * @return Quantized position → vertex index array map
	 */
	TMap<FIntVector, TArray<uint32>> BuildPositionGroups(const TArray<FVector>& Positions, float CellSize = 0.01f)
	{
		TMap<FIntVector, TArray<uint32>> PositionGroups;
		PositionGroups.Reserve(Positions.Num());

		for (int32 i = 0; i < Positions.Num(); ++i)
		{
			FIntVector Cell = QuantizePosition(Positions[i], CellSize);
			PositionGroups.FindOrAdd(Cell).Add(static_cast<uint32>(i));
		}

		return PositionGroups;
	}

	/**
	 * Build vertex adjacency map (for HopBased expansion)
	 * Creates neighbor vertex list for each vertex from triangle indices
	 * @param Indices - Triangle index array (3 indices per triangle)
	 * @return Vertex index → neighbor vertex index set map
	 */
	TMap<uint32, TSet<uint32>> BuildAdjacencyMap(const TArray<uint32>& Indices)
	{
		TMap<uint32, TSet<uint32>> AdjacencyMap;

		const int32 NumTriangles = Indices.Num() / 3;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			const uint32 V0 = Indices[TriIdx * 3 + 0];
			const uint32 V1 = Indices[TriIdx * 3 + 1];
			const uint32 V2 = Indices[TriIdx * 3 + 2];

			// Bidirectional connection
			AdjacencyMap.FindOrAdd(V0).Add(V1);
			AdjacencyMap.FindOrAdd(V0).Add(V2);
			AdjacencyMap.FindOrAdd(V1).Add(V0);
			AdjacencyMap.FindOrAdd(V1).Add(V2);
			AdjacencyMap.FindOrAdd(V2).Add(V0);
			AdjacencyMap.FindOrAdd(V2).Add(V1);
		}

		return AdjacencyMap;
	}

	/**
	 * Position-based adjacency map expansion (UV Seam handling)
	 * Expands so vertices at same position share each other's neighbors
	 * @param AdjacencyMap - Original adjacency map (modified)
	 * @param PositionGroups - Position-based vertex groups
	 */
	void ExpandAdjacencyForUVSeams(
		TMap<uint32, TSet<uint32>>& AdjacencyMap,
		const TMap<FIntVector, TArray<uint32>>& PositionGroups)
	{
		for (const auto& Group : PositionGroups)
		{
			const TArray<uint32>& Vertices = Group.Value;
			if (Vertices.Num() <= 1)
			{
				continue;
			}

			// Collect all neighbors of vertices in group as union
			TSet<uint32> CombinedNeighbors;
			for (uint32 V : Vertices)
			{
				if (TSet<uint32>* Neighbors = AdjacencyMap.Find(V))
				{
					CombinedNeighbors.Append(*Neighbors);
				}
			}

			// Exclude vertices within group from neighbors
			for (uint32 V : Vertices)
			{
				CombinedNeighbors.Remove(V);
			}

			// Apply union neighbors to all vertices in group
			for (uint32 V : Vertices)
			{
				AdjacencyMap.FindOrAdd(V) = CombinedNeighbors;
			}
		}
	}

	/**
	 * Add UV Seam duplicate vertices to selection
	 * @param SelectedVertices - Selected vertex set (modified)
	 * @param Positions - Vertex position array
	 * @param PositionGroups - Position-based vertex groups
	 */
	void AddPositionDuplicates(
		TSet<uint32>& SelectedVertices,
		const TArray<FVector>& Positions,
		const TMap<FIntVector, TArray<uint32>>& PositionGroups,
		float CellSize = 0.01f)
	{
		TSet<uint32> Duplicates;

		for (uint32 V : SelectedVertices)
		{
			FIntVector Cell = QuantizePosition(Positions[V], CellSize);
			if (const TArray<uint32>* Group = PositionGroups.Find(Cell))
			{
				for (uint32 DupV : *Group)
				{
					if (!SelectedVertices.Contains(DupV))
					{
						Duplicates.Add(DupV);
					}
				}
			}
		}

		SelectedVertices.Append(Duplicates);
	}

	/**
	 * Calculate component space transform along bone chain
	 * @param BoneIndex - Bone index
	 * @param RefSkeleton - Reference skeleton
	 * @param RefBonePose - Reference bone pose
	 * @return Component space bone transform
	 */
	FTransform CalculateBoneTransform(
		int32 BoneIndex,
		const FReferenceSkeleton& RefSkeleton,
		const TArray<FTransform>& RefBonePose)
	{
		if (BoneIndex == INDEX_NONE || !RefBonePose.IsValidIndex(BoneIndex))
		{
			return FTransform::Identity;
		}

		FTransform BoneTransform = RefBonePose[BoneIndex];
		int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);

		while (ParentIndex != INDEX_NONE)
		{
			BoneTransform = BoneTransform * RefBonePose[ParentIndex];
			ParentIndex = RefSkeleton.GetParentIndex(ParentIndex);
		}

		return BoneTransform;
	}

	/**
	 * Select base affected vertices (Auto/VirtualRing mode)
	 * @param Ring - Ring settings
	 * @param Positions - Vertex position array
	 * @param BoneTransform - Bone's component space transform
	 * @param OutAffectedVertices - Output: affected vertex index set
	 * @param OutRingBounds - Output: Ring region's local bounds (valid only in Auto mode)
	 * @param OutRingTransform - Output: Ring local → component transform
	 * @return Success status
	 */
	bool SelectAffectedVertices(
		const FFleshRingSettings& Ring,
		const TArray<FVector>& Positions,
		const FTransform& BoneTransform,
		TSet<uint32>& OutAffectedVertices,
		FBox& OutRingBounds,
		FTransform& OutRingTransform)
	{
		OutAffectedVertices.Empty();
		OutRingBounds = FBox(EForceInit::ForceInit);
		OutRingTransform = FTransform::Identity;

		// Default margin: ensure minimum slack even when Refinement is OFF
		// Prevents deformation boundary region polygons from being too coarse
		constexpr float DefaultZMargin = 3.0f;  // cm
		constexpr float DefaultRadialMargin = 1.5f;  // cm (for VirtualRing mode)

		if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto && !Ring.RingMesh.IsNull())
		{
			// =====================================
			// Auto mode: SDF bounds-based
			// =====================================
			UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
			if (!RingMesh)
			{
				return false;
			}

			// RingMesh's local bounds
			FBox MeshBounds = RingMesh->GetBoundingBox();

			// Ring local → component space transform
			FTransform MeshTransform(Ring.MeshRotation, Ring.MeshOffset);
			MeshTransform.SetScale3D(Ring.MeshScale);
			OutRingTransform = MeshTransform * BoneTransform;

			// SDFBoundsExpandX/Y + default Z margin applied
			// Add default margin in Z direction to include upper/lower boundary regions
			FVector Expand(Ring.SDFBoundsExpandX, Ring.SDFBoundsExpandY, DefaultZMargin);
			MeshBounds.Min -= Expand;
			MeshBounds.Max += Expand;

			OutRingBounds = MeshBounds;

			// Component → Ring local inverse transform
			FTransform ComponentToLocal = OutRingTransform.Inverse();

			// Select vertices inside bounds
			for (int32 i = 0; i < Positions.Num(); ++i)
			{
				FVector LocalPos = ComponentToLocal.TransformPosition(Positions[i]);
				if (MeshBounds.IsInside(LocalPos))
				{
					OutAffectedVertices.Add(static_cast<uint32>(i));
				}
			}
		}
		else
		{
			// =====================================
			// VirtualRing mode: Torus region-based
			// =====================================
			FVector LocalOffset = Ring.RingRotation.RotateVector(Ring.RingOffset);
			FVector Center = BoneTransform.GetLocation() + LocalOffset;
			FVector Axis = BoneTransform.GetRotation().RotateVector(
				Ring.RingRotation.RotateVector(FVector::UpVector));
			Axis.Normalize();

			// Set Ring transform (used in BoundsExpand)
			OutRingTransform = FTransform(Ring.RingRotation, LocalOffset) * BoneTransform;

			// Torus parameters + default margin
			// Add margin to include boundary region vertices
			const float InnerRadius = FMath::Max(0.0f, Ring.RingRadius - DefaultRadialMargin);
			const float OuterRadius = Ring.RingRadius + Ring.RingThickness + DefaultRadialMargin;
			const float HalfHeight = Ring.RingHeight * 0.5f + DefaultZMargin;

			// Torus bounds (margin included)
			OutRingBounds = FBox(
				FVector(-OuterRadius, -OuterRadius, -HalfHeight),
				FVector(OuterRadius, OuterRadius, HalfHeight)
			);

			// Select vertices inside Torus region
			for (int32 i = 0; i < Positions.Num(); ++i)
			{
				FVector ToVertex = Positions[i] - Center;

				// Axial distance (height)
				float AxisDist = FVector::DotProduct(ToVertex, Axis);

				// Radial distance
				FVector RadialVec = ToVertex - Axis * AxisDist;
				float RadialDist = RadialVec.Size();

				// Check if inside Torus region (margin included)
				if (FMath::Abs(AxisDist) <= HalfHeight &&
					RadialDist >= InnerRadius &&
					RadialDist <= OuterRadius)
				{
					OutAffectedVertices.Add(static_cast<uint32>(i));
				}
			}
		}

		return OutAffectedVertices.Num() > 0;
	}

	/**
	 * BoundsExpand mode: Select additional vertices by expanding Z-axis bounds
	 * @param Ring - Ring settings
	 * @param Positions - Vertex position array
	 * @param RingTransform - Ring local → component transform
	 * @param OriginalBounds - Original Ring bounds (local space)
	 * @param SeedVertices - Base affected vertices
	 * @param OutExpandedVertices - Output: expanded vertex set
	 */
	void ExpandByBounds(
		const FFleshRingSettings& Ring,
		const TArray<FVector>& Positions,
		const FTransform& RingTransform,
		const FBox& OriginalBounds,
		const TSet<uint32>& SeedVertices,
		TSet<uint32>& OutExpandedVertices)
	{
		OutExpandedVertices = SeedVertices;

		// Expand bounds in Z-axis
		FBox ExpandedBounds = OriginalBounds;
		ExpandedBounds.Min.Z -= Ring.SmoothingBoundsZBottom;
		ExpandedBounds.Max.Z += Ring.SmoothingBoundsZTop;

		// Component → Ring local inverse transform
		FTransform ComponentToLocal = RingTransform.Inverse();

		// Select additional vertices inside expanded bounds
		for (int32 i = 0; i < Positions.Num(); ++i)
		{
			uint32 VertexIdx = static_cast<uint32>(i);
			if (SeedVertices.Contains(VertexIdx))
			{
				continue; // Already selected
			}

			FVector LocalPos = ComponentToLocal.TransformPosition(Positions[i]);
			if (ExpandedBounds.IsInside(LocalPos))
			{
				OutExpandedVertices.Add(VertexIdx);
			}
		}
	}

	/**
	 * HopBased mode: Expand vertices using BFS N-hop
	 * @param SeedVertices - Seed vertices (base affected)
	 * @param AdjacencyMap - Vertex adjacency map
	 * @param MaxHops - Maximum hop count
	 * @param OutExpandedVertices - Output: expanded vertex set
	 */
	void ExpandByHops(
		const TSet<uint32>& SeedVertices,
		const TMap<uint32, TSet<uint32>>& AdjacencyMap,
		int32 MaxHops,
		TSet<uint32>& OutExpandedVertices)
	{
		OutExpandedVertices = SeedVertices;

		TSet<uint32> CurrentFrontier = SeedVertices;

		for (int32 Hop = 0; Hop < MaxHops; ++Hop)
		{
			TSet<uint32> NextFrontier;

			for (uint32 V : CurrentFrontier)
			{
				if (const TSet<uint32>* Neighbors = AdjacencyMap.Find(V))
				{
					for (uint32 N : *Neighbors)
					{
						if (!OutExpandedVertices.Contains(N))
						{
							OutExpandedVertices.Add(N);
							NextFrontier.Add(N);
						}
					}
				}
			}

			CurrentFrontier = MoveTemp(NextFrontier);

			if (CurrentFrontier.Num() == 0)
			{
				break; // No more vertices to expand
			}
		}
	}

	/**
	 * Convert DI's AffectedVertices to original mesh indices via position-based matching
	 *
	 * PreviewComponent's DeformerInstance uses SubdivisionSettings.PreviewSubdividedMesh (different topology),
	 * so vertex indices differ from original mesh. However, positions are nearly identical, so we use position-based matching.
	 *
	 * @param SourceComponent - FleshRingComponent with DeformerInstance (editor preview)
	 * @param SourceMesh - Original SkeletalMesh (before subdivision)
	 * @param OutVertexIndices - Output: vertex index set of original mesh
	 * @return true if matching succeeded, false if fallback needed
	 */
	bool ExtractAffectedVerticesFromDI(
		const UFleshRingComponent* SourceComponent,
		const USkeletalMesh* SourceMesh,
		TSet<uint32>& OutVertexIndices)
	{
		OutVertexIndices.Empty();

		if (!SourceComponent || !SourceMesh)
		{
			return false;
		}

		// Get SkeletalMeshComponent (mesh that DI is bound to)
		USkeletalMeshComponent* SMC = const_cast<UFleshRingComponent*>(SourceComponent)->GetResolvedTargetSkeletalMeshComponent();
		if (!SMC)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No SkeletalMeshComponent"));
			return false;
		}

		// Get DeformerInstance (bound to SkeletalMeshComponent)
		UMeshDeformerInstance* BaseDI = SMC->GetMeshDeformerInstance();
		if (!BaseDI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No MeshDeformerInstance"));
			return false;
		}

		// Cast to FleshRingDeformerInstance
		const UFleshRingDeformerInstance* DI = Cast<UFleshRingDeformerInstance>(BaseDI);
		if (!DI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: DeformerInstance is not FleshRingDeformerInstance"));
			return false;
		}

		// Get AffectedVertices data for LOD 0
		const TArray<FRingAffectedData>* AllRingData = DI->GetAffectedRingDataForDebug(0);
		if (!AllRingData || AllRingData->Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No ring data in DI"));
			return false;
		}

		// Mesh used by DI (may be PreviewSubdividedMesh)
		USkeletalMesh* DIMesh = SMC->GetSkeletalMeshAsset();
		if (!DIMesh)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No mesh in SMC"));
			return false;
		}

		// If DI mesh is same as source mesh, use indices directly
		bool bSameMesh = (DIMesh == SourceMesh);

		// Extract DI mesh vertex positions (for position matching)
		FSkeletalMeshRenderData* DIRenderData = DIMesh->GetResourceForRendering();
		if (!DIRenderData || DIRenderData->LODRenderData.Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No DI mesh render data"));
			return false;
		}

		const FSkeletalMeshLODRenderData& DILODData = DIRenderData->LODRenderData[0];
		const uint32 DIVertexCount = DILODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		// Extract source mesh vertex positions
		FSkeletalMeshRenderData* SourceRenderData = SourceMesh->GetResourceForRendering();
		if (!SourceRenderData || SourceRenderData->LODRenderData.Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No source mesh render data"));
			return false;
		}

		const FSkeletalMeshLODRenderData& SourceLODData = SourceRenderData->LODRenderData[0];
		const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		// Collect affected vertex indices from all Rings in DI
		TSet<uint32> DIAffectedIndices;
		for (const FRingAffectedData& RingData : *AllRingData)
		{
			// Tightness region (PackedIndices)
			for (uint32 Idx : RingData.PackedIndices)
			{
				if (Idx < DIVertexCount)
				{
					DIAffectedIndices.Add(Idx);
				}
			}

			// Smoothing region (unified SmoothingRegionIndices)
			for (uint32 Idx : RingData.SmoothingRegionIndices)
			{
				if (Idx < DIVertexCount)
				{
					DIAffectedIndices.Add(Idx);
				}
			}
		}

		if (DIAffectedIndices.Num() == 0)
		{
			return false;
		}

		// If meshes are same, use indices directly
		if (bSameMesh)
		{
			OutVertexIndices = MoveTemp(DIAffectedIndices);
			return true;
		}

		// If meshes differ, perform position-based matching

		// 1. Extract positions of DI affected vertices
		TArray<FVector> DIAffectedPositions;
		DIAffectedPositions.Reserve(DIAffectedIndices.Num());
		for (uint32 DIIdx : DIAffectedIndices)
		{
			FVector Pos = FVector(DILODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(DIIdx));
			DIAffectedPositions.Add(Pos);
		}

		// 2. Build spatial hash for source mesh vertices (position → index mapping)
		// Grid size: 0.1cm (very precise)
		constexpr float GridSize = 0.1f;
		constexpr float MatchTolerance = 0.5f;  // Match if within 0.5cm

		TMap<FIntVector, TArray<uint32>> SourcePositionHash;
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			FVector Pos = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
			FIntVector GridKey(
				FMath::FloorToInt(Pos.X / GridSize),
				FMath::FloorToInt(Pos.Y / GridSize),
				FMath::FloorToInt(Pos.Z / GridSize)
			);
			SourcePositionHash.FindOrAdd(GridKey).Add(i);
		}

		// 3. Find closest vertex in source mesh for each DI affected position
		int32 MatchedCount = 0;
		for (const FVector& DIPos : DIAffectedPositions)
		{
			FIntVector CenterKey(
				FMath::FloorToInt(DIPos.X / GridSize),
				FMath::FloorToInt(DIPos.Y / GridSize),
				FMath::FloorToInt(DIPos.Z / GridSize)
			);

			float BestDistSq = MatchTolerance * MatchTolerance;
			int32 BestSourceIdx = INDEX_NONE;

			// 27-cell neighbor search (3x3x3)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					for (int32 dz = -1; dz <= 1; ++dz)
					{
						FIntVector NeighborKey = CenterKey + FIntVector(dx, dy, dz);
						if (const TArray<uint32>* Indices = SourcePositionHash.Find(NeighborKey))
						{
							for (uint32 SourceIdx : *Indices)
							{
								FVector SourcePos = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(SourceIdx));
								float DistSq = FVector::DistSquared(DIPos, SourcePos);
								if (DistSq < BestDistSq)
								{
									BestDistSq = DistSq;
									BestSourceIdx = SourceIdx;
								}
							}
						}
					}
				}
			}

			if (BestSourceIdx != INDEX_NONE)
			{
				OutVertexIndices.Add(static_cast<uint32>(BestSourceIdx));
				MatchedCount++;
			}
		}

		return OutVertexIndices.Num() > 0;
	}

	/**
	 * Calculate squared shortest distance from point to triangle
	 * @param Point - Point to check
	 * @param V0, V1, V2 - Triangle vertices
	 * @return Squared shortest distance
	 */
	float PointToTriangleDistSq(const FVector& Point, const FVector& V0, const FVector& V1, const FVector& V2)
	{
		// Project point onto triangle plane
		FVector Edge0 = V1 - V0;
		FVector Edge1 = V2 - V0;
		FVector Normal = FVector::CrossProduct(Edge0, Edge1);
		float NormalLenSq = Normal.SizeSquared();

		if (NormalLenSq < SMALL_NUMBER)
		{
			// Degenerate triangle
			return FLT_MAX;
		}

		Normal /= FMath::Sqrt(NormalLenSq);

		// Distance to plane
		FVector ToPoint = Point - V0;
		float PlaneDist = FVector::DotProduct(ToPoint, Normal);
		FVector Projected = Point - Normal * PlaneDist;

		// Calculate Barycentric coordinates
		FVector V0ToP = Projected - V0;
		float D00 = FVector::DotProduct(Edge0, Edge0);
		float D01 = FVector::DotProduct(Edge0, Edge1);
		float D11 = FVector::DotProduct(Edge1, Edge1);
		float D20 = FVector::DotProduct(V0ToP, Edge0);
		float D21 = FVector::DotProduct(V0ToP, Edge1);

		float Denom = D00 * D11 - D01 * D01;
		if (FMath::Abs(Denom) < SMALL_NUMBER)
		{
			return FLT_MAX;
		}

		float V = (D11 * D20 - D01 * D21) / Denom;
		float W = (D00 * D21 - D01 * D20) / Denom;
		float U = 1.0f - V - W;

		// Check if inside triangle
		if (U >= 0.0f && V >= 0.0f && W >= 0.0f)
		{
			// Inside: return only distance to plane
			return PlaneDist * PlaneDist;
		}

		// Outside: distance to closest edge/vertex
		auto PointToSegmentDistSq = [](const FVector& P, const FVector& A, const FVector& B) -> float
		{
			FVector AB = B - A;
			FVector AP = P - A;
			float T = FVector::DotProduct(AP, AB) / FMath::Max(FVector::DotProduct(AB, AB), SMALL_NUMBER);
			T = FMath::Clamp(T, 0.0f, 1.0f);
			FVector Closest = A + AB * T;
			return FVector::DistSquared(P, Closest);
		};

		float D0 = PointToSegmentDistSq(Point, V0, V1);
		float D1 = PointToSegmentDistSq(Point, V1, V2);
		float D2 = PointToSegmentDistSq(Point, V2, V0);

		return FMath::Min3(D0, D1, D2);
	}

	/**
	 * Find source mesh triangles containing DI's AffectedVertices positions
	 *
	 * Find which triangles in the original mesh the PreviewSubdividedMesh's
	 * AffectedVertices positions are inside/near
	 *
	 * @param SourceComponent - FleshRingComponent with DeformerInstance
	 * @param SourceMesh - Original SkeletalMesh (before subdivision)
	 * @param SourcePositions - Source mesh vertex position array
	 * @param SourceIndices - Source mesh index array
	 * @param OutTriangleIndices - Output: affected triangle index set
	 * @return true if succeeded
	 */
	bool ExtractAffectedTrianglesFromDI(
		const UFleshRingComponent* SourceComponent,
		const USkeletalMesh* SourceMesh,
		const TArray<FVector>& SourcePositions,
		const TArray<uint32>& SourceIndices,
		TSet<int32>& OutTriangleIndices)
	{
		OutTriangleIndices.Empty();

		if (!SourceComponent || !SourceMesh)
		{
			return false;
		}

		// Get SkeletalMeshComponent
		USkeletalMeshComponent* SMC = const_cast<UFleshRingComponent*>(SourceComponent)->GetResolvedTargetSkeletalMeshComponent();
		if (!SMC)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No SkeletalMeshComponent"));
			return false;
		}

		// Get DeformerInstance
		UMeshDeformerInstance* BaseDI = SMC->GetMeshDeformerInstance();
		if (!BaseDI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No MeshDeformerInstance"));
			return false;
		}

		const UFleshRingDeformerInstance* DI = Cast<UFleshRingDeformerInstance>(BaseDI);
		if (!DI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: Not FleshRingDeformerInstance"));
			return false;
		}

		// AffectedVertices data for LOD 0
		const TArray<FRingAffectedData>* AllRingData = DI->GetAffectedRingDataForDebug(0);
		if (!AllRingData || AllRingData->Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No ring data"));
			return false;
		}

		// Mesh used by DI (SubdivisionSettings.PreviewSubdividedMesh)
		USkeletalMesh* DIMesh = SMC->GetSkeletalMeshAsset();
		if (!DIMesh)
		{
			return false;
		}

		FSkeletalMeshRenderData* DIRenderData = DIMesh->GetResourceForRendering();
		if (!DIRenderData || DIRenderData->LODRenderData.Num() == 0)
		{
			return false;
		}

		const FSkeletalMeshLODRenderData& DILODData = DIRenderData->LODRenderData[0];
		const uint32 DIVertexCount = DILODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		// Get Ring settings from FleshRingAsset
		const UFleshRingAsset* Asset = SourceComponent->FleshRingAsset;
		if (!Asset)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No FleshRingAsset"));
			return false;
		}

		// Collect DI's AffectedVertices indices
		// Conditional collection based on Ring settings:
		// - bEnableRefinement == false → PackedIndices only
		// - bEnableRefinement == true → PackedIndices + SmoothingRegionIndices (unified)
		// - bEnableBulge == true → Add Bulge region vertices
		TSet<uint32> DIAffectedIndices;
		const int32 NumRings = FMath::Min(AllRingData->Num(), Asset->Rings.Num());

		for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
		{
			const FRingAffectedData& RingData = (*AllRingData)[RingIdx];
			const FFleshRingSettings& RingSettings = Asset->Rings[RingIdx];

			// 1. Base region (Tightness target) - always collect
			for (uint32 Idx : RingData.PackedIndices)
			{
				if (Idx < DIVertexCount) DIAffectedIndices.Add(Idx);
			}

			// 2. Collect smoothing region only when Refinement is enabled (unified SmoothingRegionIndices)
			if (RingSettings.bEnableRefinement)
			{
				for (uint32 Idx : RingData.SmoothingRegionIndices)
				{
					if (Idx < DIVertexCount) DIAffectedIndices.Add(Idx);
				}
			}

			// 3. Collect Bulge region vertices (use pre-calculated BulgeIndices - same as Show Bulge Heatmap)
			if (RingSettings.bEnableBulge && RingData.BulgeIndices.Num() > 0)
			{
				const int32 PrevCount = DIAffectedIndices.Num();

				for (uint32 Idx : RingData.BulgeIndices)
				{
					if (Idx < DIVertexCount) DIAffectedIndices.Add(Idx);
				}

				UE_LOG(LogFleshRingAsset, Log, TEXT("ExtractAffectedTrianglesFromDI: Ring[%d] Bulge - BulgeIndices=%d (new=%d)"),
					RingIdx,
					RingData.BulgeIndices.Num(),
					DIAffectedIndices.Num() - PrevCount);
			}
		}

		if (DIAffectedIndices.Num() == 0)
		{
			return false;
		}

		// Extract positions of AffectedVertices
		TArray<FVector> AffectedPositions;
		AffectedPositions.Reserve(DIAffectedIndices.Num());
		for (uint32 DIIdx : DIAffectedIndices)
		{
			FVector Pos = FVector(DILODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(DIIdx));
			AffectedPositions.Add(Pos);
		}

		// ============================================
		// Build source mesh triangle spatial hash
		// ============================================
		const int32 NumTriangles = SourceIndices.Num() / 3;
		constexpr float GridSize = 5.0f;  // 5cm grid

		// Triangle AABB → grid cell mapping
		TMap<FIntVector, TArray<int32>> TriangleSpatialHash;

		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			const FVector& V0 = SourcePositions[SourceIndices[TriIdx * 3 + 0]];
			const FVector& V1 = SourcePositions[SourceIndices[TriIdx * 3 + 1]];
			const FVector& V2 = SourcePositions[SourceIndices[TriIdx * 3 + 2]];

			// Triangle AABB
			FVector MinBound = V0.ComponentMin(V1.ComponentMin(V2));
			FVector MaxBound = V0.ComponentMax(V1.ComponentMax(V2));

			// Register in all grid cells that AABB overlaps
			FIntVector MinCell(
				FMath::FloorToInt(MinBound.X / GridSize),
				FMath::FloorToInt(MinBound.Y / GridSize),
				FMath::FloorToInt(MinBound.Z / GridSize)
			);
			FIntVector MaxCell(
				FMath::FloorToInt(MaxBound.X / GridSize),
				FMath::FloorToInt(MaxBound.Y / GridSize),
				FMath::FloorToInt(MaxBound.Z / GridSize)
			);

			for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
			{
				for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
				{
					for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
					{
						TriangleSpatialHash.FindOrAdd(FIntVector(X, Y, Z)).Add(TriIdx);
					}
				}
			}
		}

		// ============================================
		// Find triangle containing each AffectedPosition
		// ============================================
		constexpr float MaxDistSq = 2.0f * 2.0f;  // Within 2cm

		for (const FVector& Pos : AffectedPositions)
		{
			FIntVector CellKey(
				FMath::FloorToInt(Pos.X / GridSize),
				FMath::FloorToInt(Pos.Y / GridSize),
				FMath::FloorToInt(Pos.Z / GridSize)
			);

			float BestDistSq = MaxDistSq;
			int32 BestTriIdx = INDEX_NONE;

			// Search current cell + neighbor cells (3x3x3)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					for (int32 dz = -1; dz <= 1; ++dz)
					{
						FIntVector NeighborKey = CellKey + FIntVector(dx, dy, dz);
						if (const TArray<int32>* TriIndices = TriangleSpatialHash.Find(NeighborKey))
						{
							for (int32 TriIdx : *TriIndices)
							{
								const FVector& V0 = SourcePositions[SourceIndices[TriIdx * 3 + 0]];
								const FVector& V1 = SourcePositions[SourceIndices[TriIdx * 3 + 1]];
								const FVector& V2 = SourcePositions[SourceIndices[TriIdx * 3 + 2]];

								float DistSq = PointToTriangleDistSq(Pos, V0, V1, V2);
								if (DistSq < BestDistSq)
								{
									BestDistSq = DistSq;
									BestTriIdx = TriIdx;
								}
							}
						}
					}
				}
			}

			if (BestTriIdx != INDEX_NONE)
			{
				OutTriangleIndices.Add(BestTriIdx);
			}
		}

		return OutTriangleIndices.Num() > 0;
	}

	/**
	 * Select Bulge region vertices (VirtualRing mode calculation)
	 * Same logic as FVirtualRingBulgeProvider::CalculateBulgeRegion
	 *
	 * @param Ring - Ring settings
	 * @param Positions - All vertex positions
	 * @param BoneTransform - Bone transform in component space
	 * @param OutBulgeVertices - Output: Bulge vertex indices
	 */
	void SelectBulgeVertices(
		const FFleshRingSettings& Ring,
		const TArray<FVector>& Positions,
		const FTransform& BoneTransform,
		TSet<uint32>& OutBulgeVertices)
	{
		if (!Ring.bEnableBulge)
		{
			return;
		}

		// Calculate Ring geometry in component space
		const FVector RingCenter = BoneTransform.GetLocation() + BoneTransform.GetRotation().RotateVector(Ring.RingOffset);
		const FVector RingAxis = BoneTransform.GetRotation().RotateVector(FVector::UpVector).GetSafeNormal();
		const float RingRadiusVal = Ring.RingRadius + Ring.RingThickness * 0.5f;
		const float RingHeightVal = Ring.RingHeight;

		// Bulge parameters
		const float BulgeAxialRange = Ring.BulgeAxialRange;
		const float BulgeRadialRange = Ring.BulgeRadialRange;

		// Bulge start distance (Ring boundary)
		const float BulgeStartDist = RingHeightVal * 0.5f;

		// Range limits
		const float AxialLimit = BulgeStartDist + RingHeightVal * 0.5f * BulgeAxialRange;
		const float RadialLimit = RingRadiusVal * BulgeRadialRange;

		// Select vertices in Bulge region
		for (int32 i = 0; i < Positions.Num(); ++i)
		{
			const FVector& VertexPos = Positions[i];
			const FVector ToVertex = VertexPos - RingCenter;

			// 1. Axial distance
			const float AxialComponent = FVector::DotProduct(ToVertex, RingAxis);
			const float AxialDist = FMath::Abs(AxialComponent);

			// Exclude inside Ring boundary (Tightness region)
			if (AxialDist < BulgeStartDist)
			{
				continue;
			}

			// Exclude beyond axial limit
			if (AxialDist > AxialLimit)
			{
				continue;
			}

			// 2. Radial distance
			const FVector RadialVec = ToVertex - RingAxis * AxialComponent;
			const float RadialDist = RadialVec.Size();

			// Exclude beyond radial limit
			if (RadialDist > RadialLimit)
			{
				continue;
			}

			OutBulgeVertices.Add(static_cast<uint32>(i));
		}
	}

} // namespace SubdivisionHelpers

// ============================================
// UFleshRingAsset Editor-Only Functions
// ============================================

void UFleshRingAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Sync FQuat when EulerRotation changes
	for (FFleshRingSettings& Ring : Rings)
	{
		Ring.RingRotation = Ring.RingEulerRotation.Quaternion();
		Ring.MeshRotation = Ring.MeshEulerRotation.Quaternion();
		Ring.VirtualBand.BandRotation = Ring.VirtualBand.BandEulerRotation.Quaternion();
	}

	// Ensure RingName uniqueness (handle empty and duplicate names)
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		FName& CurrentName = Rings[i].RingName;

		// 1. Generate unique name if empty
		if (CurrentName.IsNone())
		{
			CurrentName = MakeUniqueRingName(FName(TEXT("FleshRing")), i);
		}

		// 2. Check for duplicates (compare with previous indices)
		bool bIsDuplicate = false;
		for (int32 j = 0; j < i; ++j)
		{
			if (Rings[j].RingName == CurrentName)
			{
				bIsDuplicate = true;
				break;
			}
		}

		// 3. Generate unique name using MakeUniqueRingName if duplicate
		if (bIsDuplicate)
		{
			CurrentName = MakeUniqueRingName(CurrentName, i);
		}
	}

	// Mark asset as modified
	MarkPackageDirty();

	// Check if change requires full refresh
	bool bNeedsFullRefresh = false;

	// Full update on array structure changes
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayMove)
	{
		bNeedsFullRefresh = true;
	}

	// Full update on specific property changes
	if (PropertyChangedEvent.Property)
	{
		FName PropName = PropertyChangedEvent.Property->GetFName();

		if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, bEnableSubdivision) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingMesh) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshOffset) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshScale))
		{
			bNeedsFullRefresh = true;

			// Sync Material Layer Mappings when TargetSkeletalMesh changes
			if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh))
			{
				SyncMaterialLayerMappings();
			}

		}

		// Update debug visualization when VirtualRing mode Ring parameters change
		// Recollect Affected Vertices when AffectedLayerMask changes
		if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingHeight) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, AffectedLayerMask))
		{
			bNeedsFullRefresh = true;
		}

		// Rebuild CachedVertexLayerTypes when Material Layer Mappings LayerType changes
		if (PropName == GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, LayerType) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, MaterialLayerMappings))
		{
			bNeedsFullRefresh = true;
		}

		// Rebuild AffectedVertices when hop-based smoothing parameters change
		if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MaxSmoothingHops) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingVolumeMode))
		{
			bNeedsFullRefresh = true;
		}

		// Rebuild SmoothingRegion when smoothing enable flags change
		// BuildHopDistanceData() is called conditionally on bAnySmoothingEnabled
		// so cache invalidation is needed when these flags change
		if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableRefinement) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableLaplacianSmoothing) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnablePBDEdgeConstraint) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableHeatPropagation))
		{
			bNeedsFullRefresh = true;
		}

		// Full update when preview subdivision parameters change (cache invalidated via hash comparison in PreviewScene)
		if (PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewSubdivisionLevel) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneHopCount) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneWeightThreshold) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MinEdgeLength))
		{
			bNeedsFullRefresh = true;
		}

		// Cache invalidation needed when Normal/Tangent Recompute parameters change
		// GPU normal/tangent cache must be invalidated when these properties change
		if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, bEnableNormalRecompute) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, NormalRecomputeMethod) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, bEnableNormalHopBlending) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, NormalBlendFalloffType) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, bEnableDisplacementBlending) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, MaxDisplacementForBlend) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, bEnableTangentRecompute))
		{
			bNeedsFullRefresh = true;
		}

		// Detect VirtualBand-related property changes
		// Check MemberProperty chain to determine if it's a VirtualBand sub-property
		bool bIsVirtualBandProperty = false;

		// Direct property name check (VirtualBand related)
		if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, VirtualBand) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSettings, MidUpperRadius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSettings, MidLowerRadius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSettings, BandHeight) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSettings, BandThickness) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSettings, Upper) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSettings, Lower) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSection, Radius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FVirtualBandSection, Height))
		{
			bIsVirtualBandProperty = true;
		}

		// Find VirtualBand in MemberProperty chain
		if (!bIsVirtualBandProperty && PropertyChangedEvent.MemberProperty)
		{
			FName MemberName = PropertyChangedEvent.MemberProperty->GetFName();
			if (MemberName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, VirtualBand))
			{
				bIsVirtualBandProperty = true;
			}
		}

		if (bIsVirtualBandProperty)
		{
			bNeedsFullRefresh = true;
		}

		// Clear Subdivision meshes when TargetSkeletalMesh changes (needs regeneration with new mesh)
		// (PreviewSubdividedMesh is managed in FleshRingPreviewScene - regenerated in OnAssetChanged)
		if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh))
		{
			// Also clear SubdividedMesh (needs regeneration since source mesh changed)
			if (SubdivisionSettings.SubdividedMesh)
			{
				ClearSubdividedMesh();
				// Prevent duplicate broadcast since ClearSubdividedMesh() calls OnAssetChanged.Broadcast()
				bNeedsFullRefresh = false;
			}
		}

		// Clean up SubdividedMesh when bEnableSubdivision is set to false
		// (Prevents crashes from state inconsistency)
		if (PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, bEnableSubdivision))
		{
			if (!SubdivisionSettings.bEnableSubdivision && SubdivisionSettings.SubdividedMesh)
			{
				// ClearSubdividedMesh() internally calls OnAssetChanged.Broadcast()
				ClearSubdividedMesh();
				// Prevent duplicate broadcast
				bNeedsFullRefresh = false;
			}
		}

		// Transform-related properties (Offset, Rotation, Scale, Radius, Strength, Falloff, etc.)
		// don't need full update - handled via lightweight update
	}

	// Broadcast full refresh only for structural changes
	// VirtualBand properties only update on drag end (ValueSet), excluding Interactive
	if (bNeedsFullRefresh && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		OnAssetChanged.Broadcast(this);
	}
}

// Helper: Skeletal mesh validity check (common utility wrapper)
static bool IsSkeletalMeshValidForUse(USkeletalMesh* Mesh)
{
	return FleshRingUtils::IsSkeletalMeshValid(Mesh, /*bLogWarnings=*/ false);
}

void UFleshRingAsset::PostTransacted(const FTransactionObjectEvent& TransactionEvent)
{
	Super::PostTransacted(TransactionEvent);

	// Only process Undo/Redo events
	if (TransactionEvent.GetEventType() != ETransactionObjectEventType::UndoRedo)
	{
		return;
	}

	// Detect Ring count changes (fixes hash comparison failure on Undo/Redo)
	// LastKnownRingCount is not UPROPERTY so it's not included in transaction
	// -> Not restored on Undo/Redo, enabling Ring add/delete detection
	const int32 CurrentRingCount = Rings.Num();
	LastKnownRingCount = CurrentRingCount;

	// PreviewSubdividedMesh is now managed by FleshRingPreviewScene
	// On Undo/Redo, just send asset change notification and PreviewScene handles regeneration via hash comparison

	// Asset change notification (for Deformer parameter update, etc.)
	OnAssetChanged.Broadcast(this);
}

void UFleshRingAsset::GenerateSubdividedMesh(UFleshRingComponent* SourceComponent)
{
	// Disable transaction - prevent mesh creation/cleanup from being included in Undo history
	// Setting GUndo to nullptr causes Modify() calls to be ignored, not recorded in transaction
	ITransaction* PreviousGUndo = GUndo;
	GUndo = nullptr;
	ON_SCOPE_EXIT { GUndo = PreviousGUndo; };

	// If SourceComponent exists, utilize DeformerInstance's AffectedVertices data
	// Convert accurately calculated region from DI to original mesh indices via position-based matching
	// If SourceComponent is null, fallback to direct calculation based on original mesh

	// Remove previous SubdividedMesh first if exists (prevent name collision)
	if (SubdivisionSettings.SubdividedMesh)
	{
		// Prevent memory leak: perform complete cleanup
		USkeletalMesh* OldMesh = SubdivisionSettings.SubdividedMesh;

		// 1. Release pointer
		SubdivisionSettings.SubdividedMesh = nullptr;

		// 2. Fully release render resources
		OldMesh->ReleaseResources();
		OldMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();

		// 3. Change Outer to TransientPackage
		OldMesh->Rename(nullptr, GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional);

		// 4. Clear flags
		OldMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
		OldMesh->SetFlags(RF_Transient);

		// 5. Mark as garbage collection target
		OldMesh->MarkAsGarbage();

		// Note: Don't call OnAssetChanged.Broadcast()
		// SubdividedMesh is for runtime, preview uses PreviewSubdividedMesh
		// Broadcasting would reinitialize preview DeformerInstance causing deformation data loss

		// Only directly update world FleshRingComponents (exclude preview)
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UWorld* World = Context.World())
				{
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						if (UFleshRingComponent* Comp = It->FindComponentByClass<UFleshRingComponent>())
						{
							if (Comp->FleshRingAsset == this)
							{
								// ApplyAsset() sees SubdivisionSettings.SubdividedMesh == nullptr and switches to original mesh
								Comp->ApplyAsset();
							}
						}
					}
				}
			}
		}
	}

	if (!SubdivisionSettings.bEnableSubdivision)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateSubdividedMesh: Subdivision is disabled"));
		return;
	}

	if (TargetSkeletalMesh.IsNull())
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: TargetSkeletalMesh is not set"));
		return;
	}

	USkeletalMesh* SourceMesh = TargetSkeletalMesh.LoadSynchronous();
	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Failed to load SourceMesh"));
		return;
	}

	if (Rings.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Ring is not configured"));
		return;
	}

	// ============================================
	// 1. Acquire source mesh render data
	// ============================================
	FSkeletalMeshRenderData* RenderData = SourceMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: No RenderData"));
		return;
	}

	const FSkeletalMeshLODRenderData& SourceLODData = RenderData->LODRenderData[0];
	const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// ============================================
	// 2. Extract source vertex data
	// ============================================
	TArray<FVector> SourcePositions;
	TArray<FVector> SourceNormals;
	TArray<FVector4> SourceTangents;
	TArray<FVector2D> SourceUVs;

	SourcePositions.SetNum(SourceVertexCount);
	SourceNormals.SetNum(SourceVertexCount);
	SourceTangents.SetNum(SourceVertexCount);
	SourceUVs.SetNum(SourceVertexCount);

	for (uint32 i = 0; i < SourceVertexCount; ++i)
	{
		SourcePositions[i] = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
		SourceNormals[i] = FVector(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));
		FVector4f TangentX = SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
		SourceTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);
		SourceUVs[i] = FVector2D(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
	}

	// Extract indices
	TArray<uint32> SourceIndices;
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = SourceLODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (IndexBuffer)
	{
		const int32 NumIndices = IndexBuffer->Num();
		SourceIndices.SetNum(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			SourceIndices[i] = IndexBuffer->Get(i);
		}
	}

	// Extract material indices per section (per triangle)
	TArray<int32> SourceTriangleMaterialIndices;
	{
		const int32 NumTriangles = SourceIndices.Num() / 3;
		SourceTriangleMaterialIndices.SetNum(NumTriangles);

		// Assign material indices based on triangle range of each section
		for (const FSkelMeshRenderSection& Section : SourceLODData.RenderSections)
		{
			const int32 StartTriangle = Section.BaseIndex / 3;
			const int32 EndTriangle = StartTriangle + Section.NumTriangles;

			for (int32 TriIdx = StartTriangle; TriIdx < EndTriangle && TriIdx < NumTriangles; ++TriIdx)
			{
				SourceTriangleMaterialIndices[TriIdx] = Section.MaterialIndex;
			}
		}

	}

	// Extract bone weights
	const int32 MaxBoneInfluences = SourceLODData.GetVertexBufferMaxBoneInfluences();
	TArray<TArray<uint16>> SourceBoneIndices;  // Converted to actual skeleton bone indices
	TArray<TArray<uint8>> SourceBoneWeights;

	SourceBoneIndices.SetNum(SourceVertexCount);
	SourceBoneWeights.SetNum(SourceVertexCount);

	// Create per-vertex section index map (for BoneMap conversion)
	TArray<int32> VertexToSectionIndex;
	VertexToSectionIndex.SetNum(SourceVertexCount);
	for (int32& SectionIdx : VertexToSectionIndex)
	{
		SectionIdx = INDEX_NONE;
	}

	// Iterate index buffer to determine which section each vertex belongs to
	for (int32 SectionIdx = 0; SectionIdx < SourceLODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = SourceLODData.RenderSections[SectionIdx];
		const int32 StartIndex = Section.BaseIndex;
		const int32 EndIndex = StartIndex + Section.NumTriangles * 3;

		for (int32 IdxPos = StartIndex; IdxPos < EndIndex; ++IdxPos)
		{
			uint32 VertexIdx = SourceIndices[IdxPos];
			if (VertexIdx < SourceVertexCount && VertexToSectionIndex[VertexIdx] == INDEX_NONE)
			{
				VertexToSectionIndex[VertexIdx] = SectionIdx;
			}
		}
	}

	const FSkinWeightVertexBuffer* SkinWeightBuffer = SourceLODData.GetSkinWeightVertexBuffer();
	if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
	{
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			SourceBoneIndices[i].SetNum(MaxBoneInfluences);
			SourceBoneWeights[i].SetNum(MaxBoneInfluences);

			// Find section the vertex belongs to
			int32 SectionIdx = VertexToSectionIndex[i];
			const TArray<FBoneIndexType>* BoneMap = nullptr;
			if (SectionIdx != INDEX_NONE && SectionIdx < SourceLODData.RenderSections.Num())
			{
				BoneMap = &SourceLODData.RenderSections[SectionIdx].BoneMap;
			}

			for (int32 j = 0; j < MaxBoneInfluences; ++j)
			{
				uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(i, j);
				uint8 Weight = SkinWeightBuffer->GetBoneWeight(i, j);

				// Convert to actual skeleton bone index using BoneMap
				uint16 GlobalBoneIdx = LocalBoneIdx;
				if (BoneMap && LocalBoneIdx < BoneMap->Num())
				{
					GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
				}

				SourceBoneIndices[i][j] = GlobalBoneIdx;
				SourceBoneWeights[i][j] = Weight;
			}
		}
	}

	// ============================================
	// 3. Calculate topology with Subdivision processor
	// ============================================
	FFleshRingSubdivisionProcessor Processor;

	if (!Processor.SetSourceMesh(SourcePositions, SourceIndices, SourceUVs, SourceTriangleMaterialIndices))
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: SetSourceMesh failed"));
		return;
	}

	// Processor settings
	FSubdivisionProcessorSettings Settings;
	Settings.MaxSubdivisionLevel = SubdivisionSettings.MaxSubdivisionLevel;
	Settings.MinEdgeLength = SubdivisionSettings.MinEdgeLength;
	Processor.SetSettings(Settings);

	// Set parameters for all Rings
	const USkeleton* Skeleton = SourceMesh->GetSkeleton();
	const FReferenceSkeleton& RefSkeleton = SourceMesh->GetRefSkeleton();
	const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

	Processor.ClearRingParams();

	for (int32 RingIdx = 0; RingIdx < Rings.Num(); ++RingIdx)
	{
		const FFleshRingSettings& Ring = Rings[RingIdx];
		FSubdivisionRingParams RingParams;

		int32 BoneIndex = RefSkeleton.FindBoneIndex(Ring.BoneName);

		if (BoneIndex != INDEX_NONE)
		{
			// Calculate component space transform (accumulate along parent bone chain)
			FTransform BoneTransform = RefBonePose[BoneIndex];
			int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
			while (ParentIndex != INDEX_NONE)
			{
				BoneTransform = BoneTransform * RefBonePose[ParentIndex];
				ParentIndex = RefSkeleton.GetParentIndex(ParentIndex);
			}

			// Auto mode: Use RingMesh bounds
			if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto && !Ring.RingMesh.IsNull())
			{
				UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
				if (RingMesh)
				{
					RingParams.bUseSDFBounds = true;

					// Get local bounds of RingMesh
					FBox MeshBounds = RingMesh->GetBoundingBox();

					// Calculate transform same way as FleshRingComponent::GenerateSDF
					FTransform MeshTransform = FTransform(Ring.MeshRotation, Ring.MeshOffset);
					MeshTransform.SetScale3D(Ring.MeshScale);
					FTransform LocalToComponent = MeshTransform * BoneTransform;

					RingParams.SDFBoundsMin = FVector(MeshBounds.Min);
					RingParams.SDFBoundsMax = FVector(MeshBounds.Max);
					RingParams.SDFLocalToComponent = LocalToComponent;
				}
				else
				{
					RingParams.bUseSDFBounds = false;
				}
			}
			else
			{
				// VirtualRing mode: Use Torus parameters
				RingParams.bUseSDFBounds = false;

				FVector LocalOffset = Ring.RingRotation.RotateVector(Ring.RingOffset);
				RingParams.Center = BoneTransform.GetLocation() + LocalOffset;
				RingParams.Axis = Ring.RingRotation.RotateVector(FVector::UpVector);
				RingParams.Radius = Ring.RingRadius;
				RingParams.Width = Ring.RingHeight;
			}
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("  Bone '%s' not found, using default center"),
				*Ring.BoneName.ToString());
			RingParams.bUseSDFBounds = false;
			RingParams.Center = FVector::ZeroVector;
			RingParams.Axis = FVector::UpVector;
			RingParams.Radius = Ring.RingRadius;
			RingParams.Width = Ring.RingHeight;
		}

		Processor.AddRingParams(RingParams);
	}

	// ============================================
	// 3-1. Calculate Affected region (triangle-based)
	// ============================================
	// Priority:
	// 1. Extract AffectedVertices positions from SourceComponent's DI -> Find triangles containing those positions
	//    - Use PreviewMesh's subdivided vertex positions to accurately select original mesh triangles
	//    - Includes new vertices (created by subdivision) so no region is missed
	// 2. Fallback: Calculate based on original mesh vertices -> Convert to triangles
	{
		using namespace SubdivisionHelpers;

		TSet<int32> CombinedTriangleIndices;
		bool bUsedDIData = false;

		// Method 1: Try extracting triangles from SourceComponent's DI (Point-in-Triangle)
		if (SourceComponent)
		{
			if (ExtractAffectedTrianglesFromDI(SourceComponent, SourceMesh, SourcePositions, SourceIndices, CombinedTriangleIndices))
			{
				bUsedDIData = true;
			}
		}

		// Method 2: Fallback - Calculate vertices based on original mesh then convert to triangles
		if (!bUsedDIData)
		{

			TSet<uint32> CombinedVertexIndices;

			// Position grouping for UV Seam welding
			TMap<FIntVector, TArray<uint32>> PositionGroups = BuildPositionGroups(SourcePositions);

			// Build adjacency map (for HopBased)
			TMap<uint32, TSet<uint32>> AdjacencyMap = BuildAdjacencyMap(SourceIndices);

			// UV Seam handling: Expand so same-position vertices share neighbors
			ExpandAdjacencyForUVSeams(AdjacencyMap, PositionGroups);

			for (int32 RingIdx = 0; RingIdx < Rings.Num(); ++RingIdx)
			{
				const FFleshRingSettings& Ring = Rings[RingIdx];

				// Calculate bone transform
				int32 BoneIndex = RefSkeleton.FindBoneIndex(Ring.BoneName);
				FTransform BoneTransform = CalculateBoneTransform(BoneIndex, RefSkeleton, RefBonePose);

				// 1. Select base Affected vertices
				TSet<uint32> AffectedVertices;
				FBox RingBounds;
				FTransform RingTransform;

				if (!SelectAffectedVertices(Ring, SourcePositions, BoneTransform,
					AffectedVertices, RingBounds, RingTransform))
				{
					continue;
				}

				// 2. Expansion based on SmoothingVolumeMode
				TSet<uint32> ExtendedVertices;

				if (!Ring.bEnableRefinement)
				{
					ExtendedVertices = AffectedVertices;
				}
				else if (Ring.SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand)
				{
					ExpandByBounds(Ring, SourcePositions, RingTransform, RingBounds,
						AffectedVertices, ExtendedVertices);
				}
				else // HopBased
				{
					ExpandByHops(AffectedVertices, AdjacencyMap, Ring.MaxSmoothingHops, ExtendedVertices);
				}

				// 3. Select Bulge vertices (Union with smoothing region)
				TSet<uint32> BulgeVertices;
				SelectBulgeVertices(Ring, SourcePositions, BoneTransform, BulgeVertices);
				ExtendedVertices.Append(BulgeVertices);

				// 4. UV Seam handling: Also add same-position vertices of selected vertices
				AddPositionDuplicates(ExtendedVertices, SourcePositions, PositionGroups);

				// Add to union set
				CombinedVertexIndices.Append(ExtendedVertices);
			}

			// Vertex -> Triangle conversion (for fallback case)
			const int32 NumTriangles = SourceIndices.Num() / 3;
			for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
			{
				uint32 V0 = SourceIndices[TriIdx * 3 + 0];
				uint32 V1 = SourceIndices[TriIdx * 3 + 1];
				uint32 V2 = SourceIndices[TriIdx * 3 + 2];

				if (CombinedVertexIndices.Contains(V0) ||
					CombinedVertexIndices.Contains(V1) ||
					CombinedVertexIndices.Contains(V2))
				{
					CombinedTriangleIndices.Add(TriIdx);
				}
			}
		}

		// Set triangle-based mode
		if (CombinedTriangleIndices.Num() > 0)
		{
			Processor.SetTargetTriangleIndices(CombinedTriangleIndices);
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateSubdividedMesh: No triangles selected, falling back to Ring params"));
		}
	}

	// Execute Subdivision
	FSubdivisionTopologyResult TopologyResult;
	if (!Processor.Process(TopologyResult))
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Subdivision process failed"));
		return;
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateSubdividedMesh: %d -> %d vertices, %d -> %d triangles"),
		TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount,
		TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount);

	// ============================================
	// 4. Generate new vertex data via Barycentric interpolation
	// ============================================
	const int32 NewVertexCount = TopologyResult.VertexData.Num();
	TArray<FVector> NewPositions;
	TArray<FVector> NewNormals;
	TArray<FVector4> NewTangents;
	TArray<FVector2D> NewUVs;
	TArray<TArray<uint16>> NewBoneIndices;
	TArray<TArray<uint8>> NewBoneWeights;

	NewPositions.SetNum(NewVertexCount);
	NewNormals.SetNum(NewVertexCount);
	NewTangents.SetNum(NewVertexCount);
	NewUVs.SetNum(NewVertexCount);
	NewBoneIndices.SetNum(NewVertexCount);
	NewBoneWeights.SetNum(NewVertexCount);

	// Declare outside loop for memory reuse (minimize heap allocations)
	TMap<uint16, float> BoneWeightMap;
	TArray<TPair<uint16, float>> SortedWeights;

	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FSubdivisionVertexData& VD = TopologyResult.VertexData[i];
		const float U = VD.BarycentricCoords.X;
		const float V = VD.BarycentricCoords.Y;
		const float W = VD.BarycentricCoords.Z;

		const uint32 P0 = FMath::Min(VD.ParentV0, (uint32)(SourceVertexCount - 1));
		const uint32 P1 = FMath::Min(VD.ParentV1, (uint32)(SourceVertexCount - 1));
		const uint32 P2 = FMath::Min(VD.ParentV2, (uint32)(SourceVertexCount - 1));

		// Position interpolation
		NewPositions[i] = SourcePositions[P0] * U + SourcePositions[P1] * V + SourcePositions[P2] * W;

		// Normal interpolation and normalization
		FVector InterpolatedNormal = SourceNormals[P0] * U + SourceNormals[P1] * V + SourceNormals[P2] * W;
		NewNormals[i] = InterpolatedNormal.GetSafeNormal();

		// Tangent interpolation
		FVector4 InterpTangent = SourceTangents[P0] * U + SourceTangents[P1] * V + SourceTangents[P2] * W;
		FVector TangentDir = FVector(InterpTangent.X, InterpTangent.Y, InterpTangent.Z).GetSafeNormal();
		NewTangents[i] = FVector4(TangentDir.X, TangentDir.Y, TangentDir.Z, SourceTangents[P0].W);

		// UV interpolation
		NewUVs[i] = SourceUVs[P0] * U + SourceUVs[P1] * V + SourceUVs[P2] * W;

		// Bone Weight interpolation (barycentric interpolation at byte precision)
		NewBoneIndices[i].SetNum(MaxBoneInfluences);
		NewBoneWeights[i].SetNum(MaxBoneInfluences);

		// Clear contents only with Reset() (keep memory)
		BoneWeightMap.Reset();
		SortedWeights.Reset();

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (SourceBoneWeights[P0][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P0][j]) += (SourceBoneWeights[P0][j] / 255.0f) * U;
			if (SourceBoneWeights[P1][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P1][j]) += (SourceBoneWeights[P1][j] / 255.0f) * V;
			if (SourceBoneWeights[P2][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P2][j]) += (SourceBoneWeights[P2][j] / 255.0f) * W;
		}

		for (const auto& Pair : BoneWeightMap) { SortedWeights.Add(TPair<uint16, float>(Pair.Key, Pair.Value)); }
		SortedWeights.Sort([](const TPair<uint16, float>& A, const TPair<uint16, float>& B) { return A.Value > B.Value; });

		float TotalWeight = 0.0f;
		for (int32 j = 0; j < FMath::Min(SortedWeights.Num(), MaxBoneInfluences); ++j) { TotalWeight += SortedWeights[j].Value; }

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (j < SortedWeights.Num() && TotalWeight > 0.0f)
			{
				NewBoneIndices[i][j] = SortedWeights[j].Key;
				NewBoneWeights[i][j] = FMath::Clamp<uint8>(FMath::RoundToInt((SortedWeights[j].Value / TotalWeight) * 255.0f), 0, 255);
			}
			else
			{
				NewBoneIndices[i][j] = 0;
				NewBoneWeights[i][j] = 0;
			}
		}
	}

	// ============================================
	// 5. Create new USkeletalMesh (source mesh duplication approach)
	// ============================================
	// (Previous SubdividedMesh was cleaned up at function start)

	// Duplicate source mesh to inherit all internal structures (MorphTarget, LOD data, etc.)
	// Use unique name (prevent name collision since old mesh may be pending GC)
	FString MeshName = FString::Printf(TEXT("%s_Subdivided_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	SubdivisionSettings.SubdividedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, this, FName(*MeshName));

	if (!SubdivisionSettings.SubdividedMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Source mesh duplication failed"));
		return;
	}

	// Remove RF_Transactional - prevent Undo/Redo system from referencing it
	// If TransBuffer references this mesh, it won't be GC'd even after ClearSubdividedMesh()
	SubdivisionSettings.SubdividedMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);

	// Remove existing MeshDescription from duplicated mesh
	if (SubdivisionSettings.SubdividedMesh->HasMeshDescription(0))
	{
		SubdivisionSettings.SubdividedMesh->ClearMeshDescription(0);
	}

	// ============================================
	// 6. Create MeshDescription
	// ============================================
	const int32 NumFaces = TopologyResult.Indices.Num() / 3;
	FMeshDescription MeshDescription;
	FSkeletalMeshAttributes MeshAttributes(MeshDescription);
	MeshAttributes.Register();

	// Register vertices
	MeshDescription.ReserveNewVertices(NewVertexCount);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FVertexID VertexID = MeshDescription.CreateVertex();
		MeshDescription.GetVertexPositions()[VertexID] = FVector3f(NewPositions[i]);
	}

	// Create polygon groups (material sections) - create group per MaterialIndex
	MeshDescription.PolygonGroupAttributes().RegisterAttribute<FName>(
		MeshAttribute::PolygonGroup::ImportedMaterialSlotName);

	// Collect used MaterialIndices and validate
	const int32 NumMaterials = SourceMesh ? SourceMesh->GetMaterials().Num() : 1;
	TSet<int32> UsedMaterialIndices;
	for (int32 TriIdx = 0; TriIdx < NumFaces; ++TriIdx)
	{
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(TriIdx)
			? TopologyResult.TriangleMaterialIndices[TriIdx] : 0;
		// Clamp to valid range
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		UsedMaterialIndices.Add(MatIdx);
	}

	// Create PolygonGroups in MaterialIndex order (ensure section order)
	TMap<int32, FPolygonGroupID> MaterialIndexToPolygonGroup;
	TArray<int32> SortedMaterialIndices = UsedMaterialIndices.Array();
	SortedMaterialIndices.Sort();

	for (int32 MatIdx : SortedMaterialIndices)
	{
		FPolygonGroupID GroupID = MeshDescription.CreatePolygonGroup();
		MaterialIndexToPolygonGroup.Add(MatIdx, GroupID);

		// Use exact material slot name from original mesh
		FName MaterialSlotName = NAME_None;
		if (SourceMesh && SourceMesh->GetMaterials().IsValidIndex(MatIdx))
		{
			MaterialSlotName = SourceMesh->GetMaterials()[MatIdx].ImportedMaterialSlotName;
		}
		if (MaterialSlotName.IsNone())
		{
			MaterialSlotName = *FString::Printf(TEXT("Material_%d"), MatIdx);
		}

		MeshDescription.PolygonGroupAttributes().SetAttribute(
			GroupID, MeshAttribute::PolygonGroup::ImportedMaterialSlotName, 0, MaterialSlotName);
	}

	// Create VertexInstance per index buffer entry (same as preview mesh)
	// This correctly handles UV seams and hard edges
	TArray<FVertexInstanceID> VertexInstanceIDs;
	VertexInstanceIDs.Reserve(TopologyResult.Indices.Num());

	for (int32 i = 0; i < TopologyResult.Indices.Num(); ++i)
	{
		const uint32 VertexIndex = TopologyResult.Indices[i];
		const FVertexID VertexID(VertexIndex);
		const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);
		VertexInstanceIDs.Add(VertexInstanceID);

		// UV
		MeshAttributes.GetVertexInstanceUVs().Set(VertexInstanceID, 0, FVector2f(NewUVs[VertexIndex]));

		// Normal
		MeshAttributes.GetVertexInstanceNormals().Set(VertexInstanceID, FVector3f(NewNormals[VertexIndex]));

		// Tangent
		MeshAttributes.GetVertexInstanceTangents().Set(VertexInstanceID,
			FVector3f(NewTangents[VertexIndex].X, NewTangents[VertexIndex].Y, NewTangents[VertexIndex].Z));
		MeshAttributes.GetVertexInstanceBinormalSigns().Set(VertexInstanceID, NewTangents[VertexIndex].W);
	}

	// Register triangles as polygons
	for (int32 i = 0; i < NumFaces; ++i)
	{
		TArray<FVertexInstanceID> TriangleVertexInstances;
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 0]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 1]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 2]);

		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(i)
			? TopologyResult.TriangleMaterialIndices[i] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		FPolygonGroupID* GroupID = MaterialIndexToPolygonGroup.Find(MatIdx);
		if (GroupID)
		{
			MeshDescription.CreatePolygon(*GroupID, TriangleVertexInstances);
		}
	}

	// Set SkinWeights
	FSkinWeightsVertexAttributesRef SkinWeights = MeshAttributes.GetVertexSkinWeights();
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		FVertexID VertexID(i);
		TArray<UE::AnimationCore::FBoneWeight> BoneWeightArray;

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (NewBoneWeights[i][j] > 0)
			{
				UE::AnimationCore::FBoneWeight BW;
				BW.SetBoneIndex(NewBoneIndices[i][j]);
				BW.SetWeight(NewBoneWeights[i][j] / 255.0f);
				BoneWeightArray.Add(BW);
			}
		}

		SkinWeights.Set(VertexID, BoneWeightArray);
	}

	// Save MeshDescription to SkeletalMesh
	SubdivisionSettings.SubdividedMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

	// Release existing render resources (remove data duplicated by DuplicateObject)
	SubdivisionSettings.SubdividedMesh->ReleaseResources();
	SubdivisionSettings.SubdividedMesh->ReleaseResourcesFence.Wait();

	// Commit MeshDescription to actual LOD model data
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	SubdivisionSettings.SubdividedMesh->CommitMeshDescription(0, CommitParams);

	// Build settings: Prevent vertex merging + Recompute tangents only with MikkTSpace
	if (FSkeletalMeshLODInfo* LODInfo = SubdivisionSettings.SubdividedMesh->GetLODInfo(0))
	{
		LODInfo->BuildSettings.bRecomputeNormals = false;    // Keep interpolated normals (recomputing causes faceted look)
		LODInfo->BuildSettings.bRecomputeTangents = true;    // Recompute tangents with MikkTSpace
		LODInfo->BuildSettings.bUseMikkTSpace = true;
		LODInfo->BuildSettings.bRemoveDegenerates = false;
		LODInfo->BuildSettings.ThresholdPosition = 0.0f;     // Prevent vertex merging
		LODInfo->BuildSettings.ThresholdTangentNormal = 0.0f;
		LODInfo->BuildSettings.ThresholdUV = 0.0f;
	}

	// Build mesh (LOD model -> render data)
	SubdivisionSettings.SubdividedMesh->Build();

	// Verify Build result
	FSkeletalMeshRenderData* NewRenderData = SubdivisionSettings.SubdividedMesh->GetResourceForRendering();
	if (!NewRenderData || NewRenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Build failed - no RenderData"));
		SubdivisionSettings.SubdividedMesh->ConditionalBeginDestroy();
		SubdivisionSettings.SubdividedMesh = nullptr;
		return;
	}

	// Initialize render resources
	SubdivisionSettings.SubdividedMesh->InitResources();
	FlushRenderingCommands();

	// Recalculate bounding box
	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		BoundingBox += NewPositions[i];
	}
	SubdivisionSettings.SubdividedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	SubdivisionSettings.SubdividedMesh->CalculateExtendedBounds();

	// Save parameter hash (for regeneration decision)
	SubdivisionSettings.SubdivisionParamsHash = CalculateSubdivisionParamsHash();
	MarkPackageDirty();

	// Note: SubdividedMesh is only used during bake process (editor preview)
	// World components use BakedMesh at runtime, not SubdividedMesh
	// No need to notify world components here
}

void UFleshRingAsset::ClearSubdividedMesh()
{
	// Disable transaction - prevent mesh cleanup from being included in Undo history
	// Setting GUndo to nullptr causes Modify() calls to be ignored, not recorded in transaction
	ITransaction* PreviousGUndo = GUndo;
	GUndo = nullptr;
	ON_SCOPE_EXIT { GUndo = PreviousGUndo; };

	if (SubdivisionSettings.SubdividedMesh)
	{
		// Move previous mesh to Transient package so GC can clean it up
		// Without this, Subdivided_1, Subdivided_2... accumulate in asset
		USkeletalMesh* OldMesh = SubdivisionSettings.SubdividedMesh;

		// 1. Release pointer (disconnect Asset's UPROPERTY reference)
		SubdivisionSettings.SubdividedMesh = nullptr;
		SubdivisionSettings.SubdivisionParamsHash = 0;

		// 2. Fully release render resources (ReleaseResourcesFence.Wait() required!)
		OldMesh->ReleaseResources();
		OldMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();

		// 3. Change Outer to TransientPackage (detach from Asset sub-objects)
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

		// 4. Remove RF_Transactional flag - prevent Undo/Redo system from referencing it
		OldMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
		OldMesh->SetFlags(RF_Transient);

		// 5. Mark as garbage collection target
		OldMesh->MarkAsGarbage();

		UE_LOG(LogFleshRingAsset, Log, TEXT("ClearSubdividedMesh: Cleanup complete"));

		// Note: SubdividedMesh is only used during bake process
		// World components use BakedMesh at runtime, no need to notify them
		// OnAssetChanged is for editor preview scene only
		OnAssetChanged.Broadcast(this);

		MarkPackageDirty();
	}
}

void UFleshRingAsset::SetEditorSelectedRingIndex(int32 RingIndex, EFleshRingSelectionType SelectionType)
{
	EditorSelectedRingIndex = RingIndex;
	EditorSelectionType = SelectionType;

	// Delegate broadcast (detail panel -> viewport/tree sync)
	OnRingSelectionChanged.Broadcast(RingIndex);
}

// =====================================
// Baked Mesh related functions
// =====================================

bool UFleshRingAsset::GenerateBakedMesh(UFleshRingComponent* SourceComponent)
{
	// Disable transaction - prevent mesh creation/cleanup from being included in Undo history
	// If TransBuffer references mesh, it won't be GC'd
	// Setting GUndo to nullptr causes Modify() calls to be ignored, not recorded in transaction
	ITransaction* PreviousGUndo = GUndo;
	GUndo = nullptr;
	ON_SCOPE_EXIT { GUndo = PreviousGUndo; };

	if (!SourceComponent)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: SourceComponent is null"));
		return false;
	}

	USkeletalMeshComponent* SkelMeshComp = SourceComponent->GetResolvedTargetSkeletalMeshComponent();
	if (!SkelMeshComp)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: SourceComponent has no resolved target mesh"));
		return false;
	}

	// =====================================
	// Determine source mesh: Subdivision ON -> SubdividedMesh, OFF -> original mesh
	// =====================================
	USkeletalMesh* SourceMesh = nullptr;

	if (SubdivisionSettings.bEnableSubdivision)
	{
		// Subdivision ON: Generate/use SubdividedMesh
		if (!SubdivisionSettings.SubdividedMesh || NeedsSubdivisionRegeneration())
		{
			GenerateSubdividedMesh(SourceComponent);
		}

		if (SubdivisionSettings.SubdividedMesh)
		{
			SourceMesh = SubdivisionSettings.SubdividedMesh;
			UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Using SubdividedMesh"));
		}
		else
		{
			// Subdivision generation failed -> fallback to original mesh
			SourceMesh = TargetSkeletalMesh.LoadSynchronous();
			UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: SubdividedMesh generation failed, falling back to original mesh"));
		}
	}
	else
	{
		// Subdivision OFF: Bake with deformation applied to original mesh only
		SourceMesh = TargetSkeletalMesh.LoadSynchronous();
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Subdivision disabled, using original mesh"));
	}

	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: No source mesh available"));
		return false;
	}

	UFleshRingDeformer* Deformer = SourceComponent->GetDeformer();
	if (!Deformer)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: Deformer is null"));
		return false;
	}

	UFleshRingDeformerInstance* DeformerInstance = Deformer->GetActiveInstance();
	if (!DeformerInstance)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: DeformerInstance is null"));
		return false;
	}

	// =====================================
	// GPU Baking: Render SourceMesh and Readback
	// (Subdivision ON: SubdividedMesh / OFF: original mesh)
	// =====================================
	//
	// Async bake support approach:
	// 1. If current mesh is not SourceMesh -> swap only and return false (async system waits)
	// 2. If SourceMesh and cache is valid -> proceed with Readback
	// 3. If SourceMesh but cache not yet valid -> return false (async system waits)

	USkeletalMesh* CurrentMesh = SkelMeshComp->GetSkeletalMeshAsset();
	const bool bAlreadyUsingSourceMesh = (CurrentMesh == SourceMesh);

	if (!bAlreadyUsingSourceMesh)
	{
		// Step 1: Swap to SourceMesh (first call)
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Swapping to SourceMesh..."));
		SkelMeshComp->SetSkeletalMeshAsset(SourceMesh);

		// Step 2: Complete MeshObject regeneration (synchronous)
		// RecreateRenderState_Concurrent() is async so MeshObject doesn't update immediately
		// Use UnregisterComponent/RegisterComponent for synchronous regeneration
		SkelMeshComp->UnregisterComponent();
		SkelMeshComp->RegisterComponent();
		FlushRenderingCommands();

		// Step 3: Complete Deformer reinitialization (re-register LODData/AffectedVertices for new mesh)
		// Must call after MeshObject regeneration to read new mesh's RenderData
		DeformerInstance->InvalidateForMeshChange();

		// Swap mesh only and return - async system waits until cache valid then recalls
		return false;
	}

	// SourceMesh already set - check cache
	if (!DeformerInstance->HasCachedDeformedGeometry(0))
	{
		// Cache not yet valid - async system will retry
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Waiting for cache to become valid..."));
		return false;
	}

	// Cache valid - proceed with Readback
	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Cache valid, proceeding with readback..."));

	// GPU Readback (SourceMesh basis - direct correspondence)
	TArray<FVector3f> DeformedPositions;
	TArray<FVector3f> DeformedNormals;
	TArray<FVector4f> DeformedTangents;

	if (!DeformerInstance->ReadbackDeformedGeometry(DeformedPositions, DeformedNormals, DeformedTangents, 0))
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: GPU Readback failed"));
		// Mesh restoration handled by async system (CleanupAsyncBake)
		return false;
	}

	// Readback verification
	const FSkeletalMeshRenderData* SourceRenderData = SourceMesh->GetResourceForRendering();
	if (!SourceRenderData || SourceRenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Source mesh has no render data"));
		return false;
	}

	const FSkeletalMeshLODRenderData& SourceLODData = SourceRenderData->LODRenderData[0];
	const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// Buffer size verification (must match exactly since rendering SourceMesh directly)
	if (DeformedPositions.Num() != (int32)SourceVertexCount)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Vertex count mismatch - Readback=%d, Expected=%d"),
			DeformedPositions.Num(), SourceVertexCount);
		return false;
	}

	// Fill default Normal/Tangent values (if missing)
	const bool bHasNormals = DeformedNormals.Num() == (int32)SourceVertexCount;
	const bool bHasTangents = DeformedTangents.Num() == (int32)SourceVertexCount;

	if (!bHasNormals)
	{
		DeformedNormals.SetNum(SourceVertexCount);
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			DeformedNormals[i] = FVector3f(0, 0, 1);
		}
	}

	if (!bHasTangents)
	{
		DeformedTangents.SetNum(SourceVertexCount);
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			DeformedTangents[i] = FVector4f(1, 0, 0, 1);
		}
	}

	// Fix: Perform existing BakedMesh cleanup later (after new mesh creation succeeds)
	// If cleaned up first, previous mesh is lost if new mesh creation fails

	// =====================================
	// MeshDescription-based approach (same as SubdividedMesh)
	// DuplicateObject copies MeshDescription (including skin weights)
	// Only modify vertex positions in MeshDescription then call Build()
	// This approach serializes properly and maintains skin weight mapping
	// =====================================

	// Create new SkeletalMesh with unique name (old mesh may be pending GC)
	FString MeshName = FString::Printf(TEXT("%s_Baked_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	USkeletalMesh* NewBakedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, this, FName(*MeshName));
	if (!NewBakedMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Failed to duplicate source mesh"));
		return false;
	}

	// Keep animation-related properties same as original (prevent AnimInstance reinitialization)
	NewBakedMesh->SetSkeleton(SourceMesh->GetSkeleton());
	NewBakedMesh->SetPhysicsAsset(SourceMesh->GetPhysicsAsset());
	NewBakedMesh->SetShadowPhysicsAsset(SourceMesh->GetShadowPhysicsAsset());

	// Get MeshDescription (copied by DuplicateObject, includes skin weights)
	FMeshDescription* MeshDesc = NewBakedMesh->GetMeshDescription(0);
	if (!MeshDesc)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Duplicated mesh has no MeshDescription"));
		NewBakedMesh->ConditionalBeginDestroy();
		return false;
	}

	// =====================================
	// Modify vertex positions in MeshDescription
	// Skin weights are already in MeshDescription so they're preserved
	// =====================================
	TVertexAttributesRef<FVector3f> VertexPositions = MeshDesc->GetVertexPositions();
	const int32 MeshDescVertexCount = MeshDesc->Vertices().Num();

	// Vertex count verification: RenderData vertex count and MeshDescription vertex count can differ
	// MeshDescription has unique vertices, RenderData has VertexInstances (includes duplicates)
	// GPU Readback data is RenderData-based so mapping is needed
	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: MeshDesc vertices=%d, RenderData vertices=%d"),
		MeshDescVertexCount, SourceVertexCount);

	// =====================================
	// Vertex mapping and position update (optimized hashmap approach)
	// RenderData vertex -> MeshDescription vertex mapping
	// =====================================

	// Build position-based mapping from original RenderData
	// (SourceRenderData already declared above)
	if (SourceRenderData && SourceRenderData->LODRenderData.Num() > 0)
	{
		const FPositionVertexBuffer& SrcPosBuffer = SourceRenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer;

		// Quantize position to integer grid for hash key (O(1) lookup)
		// Scale: quantize in 0.001 units (1mm precision)
		auto QuantizePosition = [](const FVector3f& Pos) -> FIntVector
		{
			const float Scale = 1000.0f;  // 0.001 units
			return FIntVector(
				FMath::RoundToInt(Pos.X * Scale),
				FMath::RoundToInt(Pos.Y * Scale),
				FMath::RoundToInt(Pos.Z * Scale)
			);
		};

		// Fix: Use TArray since multiple vertices can be at same position at UV seam
		// Index MeshDescription vertices by quantized position (allow multiple vertices per position)
		TMap<FIntVector, TArray<FVertexID>> QuantizedPosToVertices;
		QuantizedPosToVertices.Reserve(MeshDescVertexCount);

		for (const FVertexID VertexID : MeshDesc->Vertices().GetElementIDs())
		{
			FIntVector QuantizedPos = QuantizePosition(VertexPositions[VertexID]);
			QuantizedPosToVertices.FindOrAdd(QuantizedPos).Add(VertexID);
		}

		// RenderData vertex -> MeshDescription vertex mapping (O(n) complexity)
		// Map same RenderIdx to all MeshDescription vertices at same position
		TMap<FVertexID, uint32> VertexToFirstRenderIdx;
		VertexToFirstRenderIdx.Reserve(MeshDescVertexCount);

		for (uint32 RenderIdx = 0; RenderIdx < SourceVertexCount; ++RenderIdx)
		{
			FVector3f RenderPos = SrcPosBuffer.VertexPosition(RenderIdx);
			FIntVector QuantizedPos = QuantizePosition(RenderPos);

			// O(1) lookup in hashmap - map to all vertices at same position
			TArray<FVertexID>* FoundVertexIDs = QuantizedPosToVertices.Find(QuantizedPos);
			if (FoundVertexIDs)
			{
				for (const FVertexID& VertexID : *FoundVertexIDs)
				{
					// Only store first mapping (only one of multiple RenderData vertices at same position needed)
					if (!VertexToFirstRenderIdx.Contains(VertexID))
					{
						VertexToFirstRenderIdx.Add(VertexID, RenderIdx);
					}
				}
			}
		}

		// Update MeshDescription vertex positions
		for (const FVertexID VertexID : MeshDesc->Vertices().GetElementIDs())
		{
			uint32* RenderIdxPtr = VertexToFirstRenderIdx.Find(VertexID);
			if (RenderIdxPtr && *RenderIdxPtr < SourceVertexCount)
			{
				VertexPositions[VertexID] = DeformedPositions[*RenderIdxPtr];
			}
		}

		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Mapped %d/%d vertices"),
			VertexToFirstRenderIdx.Num(), MeshDescVertexCount);

		// =====================================
		// Normal/Tangent update (VertexInstance based)
		// Apply GPU-computed normals/tangents to MeshDescription
		// Normals preserved as-is (bRecomputeNormals=false), tangents recomputed by MikkTSpace
		// =====================================
		if (bHasNormals && bHasTangents)
		{
			FSkeletalMeshAttributes MeshAttributes(*MeshDesc);
			TVertexInstanceAttributesRef<FVector3f> InstanceNormals = MeshAttributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> InstanceTangents = MeshAttributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<float> InstanceBinormalSigns = MeshAttributes.GetVertexInstanceBinormalSigns();

			for (const FVertexInstanceID InstanceID : MeshDesc->VertexInstances().GetElementIDs())
			{
				// VertexID-based mapping: find RenderData index through VertexInstance's parent VertexID
				FVertexID VertexID = MeshDesc->GetVertexInstanceVertex(InstanceID);
				uint32* RenderIdxPtr = VertexToFirstRenderIdx.Find(VertexID);

				if (RenderIdxPtr && *RenderIdxPtr < SourceVertexCount)
				{
					uint32 RenderIdx = *RenderIdxPtr;
					const FVector3f& Normal = DeformedNormals[RenderIdx];
					// Only apply if GPU-recomputed normal is valid
					if (!Normal.IsNearlyZero())
					{
						FVector3f Tangent(DeformedTangents[RenderIdx].X, DeformedTangents[RenderIdx].Y, DeformedTangents[RenderIdx].Z);
						float BinormalSign = DeformedTangents[RenderIdx].W;

						// Normalize normals and tangents (GPU computed values may not be unit length)
						InstanceNormals[InstanceID] = Normal.GetSafeNormal();
						InstanceTangents[InstanceID] = Tangent.GetSafeNormal();
						InstanceBinormalSigns[InstanceID] = BinormalSign;
					}
				}
				// Unmapped VertexInstances keep original normals (non-affected areas like face)
			}
		}
	}

	// =====================================
	// Commit MeshDescription and build (same as SubdividedMesh)
	// =====================================
	// Release existing render resources
	NewBakedMesh->ReleaseResources();
	NewBakedMesh->ReleaseResourcesFence.Wait();
	FlushRenderingCommands();

	// Commit MeshDescription to LOD model
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	NewBakedMesh->CommitMeshDescription(0, CommitParams);

	// Build settings: Keep GPU normals + Recompute tangents with MikkTSpace (same as preview mesh)
	if (FSkeletalMeshLODInfo* LODInfo = NewBakedMesh->GetLODInfo(0))
	{
		LODInfo->BuildSettings.bRecomputeNormals = false;    // Keep GPU-computed normals
		LODInfo->BuildSettings.bRecomputeTangents = true;    // Recompute tangents with MikkTSpace
		LODInfo->BuildSettings.bUseMikkTSpace = true;
		LODInfo->BuildSettings.bRemoveDegenerates = false;
		LODInfo->BuildSettings.ThresholdPosition = 0.0f;     // Prevent vertex merging
		LODInfo->BuildSettings.ThresholdTangentNormal = 0.0f;
		LODInfo->BuildSettings.ThresholdUV = 0.0f;
	}

	// Build mesh (create RenderData)
	NewBakedMesh->Build();

	// Verify RenderData
	FSkeletalMeshRenderData* NewRenderData = NewBakedMesh->GetResourceForRendering();
	if (!NewRenderData || NewRenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Build failed - no RenderData"));
		NewBakedMesh->ConditionalBeginDestroy();
		return false;
	}

	// Initialize render resources
	NewBakedMesh->InitResources();
	FlushRenderingCommands();

	// Recalculate bounding box
	FBox BoundingBox(ForceInit);
	for (uint32 i = 0; i < SourceVertexCount; ++i)
	{
		BoundingBox += FVector(DeformedPositions[i]);
	}
	NewBakedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	NewBakedMesh->CalculateExtendedBounds();

	// Save Ring transforms (stored in bone-relative coordinates)
	SubdivisionSettings.BakedRingTransforms.Empty();
	for (const FFleshRingSettings& Ring : Rings)
	{
		FTransform RingRelativeTransform;
		RingRelativeTransform.SetLocation(Ring.MeshOffset);
		RingRelativeTransform.SetRotation(FQuat(Ring.MeshRotation));
		RingRelativeTransform.SetScale3D(Ring.MeshScale);
		SubdivisionSettings.BakedRingTransforms.Add(RingRelativeTransform);
	}

	// New mesh is fully ready so now clean up previous BakedMesh
	// (Previous mesh preserved if creation fails)
	if (SubdivisionSettings.BakedMesh)
	{
		USkeletalMesh* OldMesh = SubdivisionSettings.BakedMesh;

		// 1. Fully release render resources (ReleaseResourcesFence.Wait() required!)
		OldMesh->ReleaseResources();
		OldMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();

		// 2. Change Outer to TransientPackage (detach from Asset sub-objects)
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

		// 3. Remove RF_Transactional flag - prevent Undo/Redo system from referencing it
		OldMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
		OldMesh->SetFlags(RF_Transient);

		// 4. Mark as garbage collection target
		OldMesh->MarkAsGarbage();

		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Cleaned up previous BakedMesh"));
	}
	// Bug fix: Previously called BakedRingTransforms.Empty() here which immediately deleted
	// the Ring transform data filled above - removed

	// Save result
	SubdivisionSettings.BakedMesh = NewBakedMesh;
	SubdivisionSettings.BakeParamsHash = CalculateBakeParamsHash();

	// Remove RF_Transactional - prevent Undo/Redo system from referencing it
	// DuplicateObject inherits source mesh flags, so explicit removal prevents TransBuffer reference
	NewBakedMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);

	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Success - %d vertices, %d rings, Hash=%u"),
		SourceVertexCount, Rings.Num(), SubdivisionSettings.BakeParamsHash);

	// Generate skinned ring meshes for runtime deformation
	// This allows ring meshes to deform with twist bones like skin vertices
	GenerateSkinnedRingMeshes(SourceMesh);

	// SubdividedMesh cleanup is performed in CleanupAsyncBake
	// (Safely cleaned up after preview mesh is restored to original)

	// Notify asset change
	MarkPackageDirty();
	OnAssetChanged.Broadcast(this);

	return true;
}

void UFleshRingAsset::ClearBakedMesh()
{
	// Disable transaction - prevent mesh cleanup from being included in Undo history
	// Setting GUndo to nullptr causes Modify() calls to be ignored, not recorded in transaction
	ITransaction* PreviousGUndo = GUndo;
	GUndo = nullptr;
	ON_SCOPE_EXIT { GUndo = PreviousGUndo; };

	if (SubdivisionSettings.BakedMesh)
	{
		// Move previous mesh to Transient package so GC can clean it up
		// Without this, BakedMesh_1, BakedMesh_2... accumulate in asset
		USkeletalMesh* OldMesh = SubdivisionSettings.BakedMesh;

		// 1. Release pointer (disconnect Asset's UPROPERTY reference)
		SubdivisionSettings.BakedMesh = nullptr;

		// 2. Fully release render resources (ReleaseResourcesFence.Wait() required!)
		OldMesh->ReleaseResources();
		OldMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();

		// 3. Change Outer to TransientPackage (detach from Asset sub-objects)
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

		// 4. Remove RF_Transactional flag - prevent Undo/Redo system from referencing it
		OldMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
		OldMesh->SetFlags(RF_Transient);

		// 5. Mark as garbage collection target
		OldMesh->MarkAsGarbage();

		UE_LOG(LogFleshRingAsset, Log, TEXT("ClearBakedMesh: Cleanup complete"));
	}

	// Cleanup skinned ring meshes
	for (USkeletalMesh* SkinnedRingMesh : SubdivisionSettings.BakedSkinnedRingMeshes)
	{
		if (SkinnedRingMesh)
		{
			SkinnedRingMesh->ReleaseResources();
			SkinnedRingMesh->ReleaseResourcesFence.Wait();
			FlushRenderingCommands();

			SkinnedRingMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			SkinnedRingMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
			SkinnedRingMesh->SetFlags(RF_Transient);
			SkinnedRingMesh->MarkAsGarbage();
		}
	}
	SubdivisionSettings.BakedSkinnedRingMeshes.Empty();

	SubdivisionSettings.BakedRingTransforms.Empty();
	SubdivisionSettings.BakeParamsHash = 0;

	MarkPackageDirty();
}

void UFleshRingAsset::GenerateSkinnedRingMeshes(USkeletalMesh* SourceMesh)
{
	// Clear existing skinned ring meshes
	for (USkeletalMesh* OldMesh : SubdivisionSettings.BakedSkinnedRingMeshes)
	{
		if (OldMesh)
		{
			OldMesh->ReleaseResources();
			OldMesh->ReleaseResourcesFence.Wait();
			FlushRenderingCommands();
			OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			OldMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
			OldMesh->SetFlags(RF_Transient);
			OldMesh->MarkAsGarbage();
		}
	}
	SubdivisionSettings.BakedSkinnedRingMeshes.Empty();

	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateSkinnedRingMeshes: SourceMesh is null"));
		return;
	}

	// Generate skinned ring mesh for each ring
	for (int32 RingIndex = 0; RingIndex < Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = Rings[RingIndex];

		// Skip if skinned ring mesh generation is disabled
		if (!Ring.bGenerateSkinnedRingMesh)
		{
			SubdivisionSettings.BakedSkinnedRingMeshes.Add(nullptr);
			continue;
		}

		// Works with all influence modes (MeshBased, VirtualRing, VirtualBand)
		// as long as a ring mesh is set
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (!RingMesh)
		{
			SubdivisionSettings.BakedSkinnedRingMeshes.Add(nullptr);
			continue;
		}

		// Get ring's bone-relative transform from baked transforms
		FTransform RingRelativeTransform = SubdivisionSettings.BakedRingTransforms.IsValidIndex(RingIndex)
			? SubdivisionSettings.BakedRingTransforms[RingIndex]
			: FTransform::Identity;

		// Get the bone's component space transform from RefSkeleton
		// This is needed to convert ring position from bone-local to component space
		// Uses same calculation pattern as SDFBoundsSelector (line ~1838) and CalculateBoneTransform
		const FReferenceSkeleton& RefSkeleton = SourceMesh->GetRefSkeleton();
		const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();
		const int32 BoneIndex = RefSkeleton.FindBoneIndex(Ring.BoneName);
		FTransform BoneComponentTransform = SubdivisionHelpers::CalculateBoneTransform(BoneIndex, RefSkeleton, RefBonePose);

		if (BoneIndex == INDEX_NONE)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateSkinnedRingMeshes: Bone '%s' not found for Ring[%d]"),
				*Ring.BoneName.ToString(), RingIndex);
		}

		// Full transform: first RingRelative (mesh-local to bone-local), then BoneComponent (bone-local to component-space)
		// UE convention for A * B: "first A, then B" transformation
		// Same pattern as SDFBoundsSelector: LocalToComponent = MeshTransform * BoneTransform
		FTransform RingTransform = RingRelativeTransform * BoneComponentTransform;

		// Generate skinned ring mesh (with bone chain filtering to prevent sampling from unrelated bones)
		FString MeshName = FString::Printf(TEXT("%s_SkinnedRing_%d"), *GetName(), RingIndex);

		USkeletalMesh* SkinnedRingMesh = FFleshRingSkinnedMeshGenerator::GenerateSkinnedRingMesh(
			RingMesh,
			SourceMesh,
			RingTransform,
			Ring.RingSkinSamplingRadius,
			BoneIndex,  // AttachBoneIndex for bone chain filtering
			this,  // Outer = this asset for permanent storage
			MeshName
		);

		if (SkinnedRingMesh)
		{
			// Clear transactional flag to prevent undo issues
			SkinnedRingMesh->ClearFlags(RF_Transactional);
			UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateSkinnedRingMeshes: Created skinned ring mesh for Ring[%d]"), RingIndex);
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateSkinnedRingMeshes: Failed to create skinned ring mesh for Ring[%d]"), RingIndex);
		}

		SubdivisionSettings.BakedSkinnedRingMeshes.Add(SkinnedRingMesh);
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateSkinnedRingMeshes: Generated %d skinned ring meshes"),
		SubdivisionSettings.BakedSkinnedRingMeshes.Num());
}

bool UFleshRingAsset::NeedsBakeRegeneration() const
{
	// Needs regeneration if no BakedMesh
	if (!SubdivisionSettings.BakedMesh)
	{
		return true;
	}

	// Check parameter change via hash comparison
	return SubdivisionSettings.BakeParamsHash != CalculateBakeParamsHash();
}

uint32 UFleshRingAsset::CalculateBakeParamsHash() const
{
	// Base on Subdivision parameter hash
	uint32 Hash = CalculateSubdivisionParamsHash();

	// Add per-Ring deformation parameters
	for (const FFleshRingSettings& Ring : Rings)
	{
		// Position/Rotation (limited precision)
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingOffset.X * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingOffset.Y * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingOffset.Z * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingEulerRotation.Pitch * 10)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingEulerRotation.Yaw * 10)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingEulerRotation.Roll * 10)));

		// Deformation strength
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.TightnessStrength * 1000)));
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnableBulge));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.BulgeIntensity * 1000)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.BulgeAxialRange * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.BulgeRadialRange * 100)));

		// Smoothing settings
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnableRefinement));
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnableSmoothing));
		Hash = HashCombine(Hash, GetTypeHash(Ring.SmoothingIterations));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.SmoothingLambda * 1000)));
	}

	return Hash;
}

int32 UFleshRingAsset::CleanupOrphanedMeshes()
{
	int32 RemovedCount = 0;

	// Collect currently used mesh pointers
	TSet<USkeletalMesh*> ActiveMeshes;
	if (SubdivisionSettings.SubdividedMesh)
	{
		ActiveMeshes.Add(SubdivisionSettings.SubdividedMesh);
	}
	if (SubdivisionSettings.BakedMesh)
	{
		ActiveMeshes.Add(SubdivisionSettings.BakedMesh);
	}
	// Note: PreviewSubdividedMesh is managed by FleshRingPreviewScene so not tracked here

	// Collect all SkeletalMesh sub-objects of this asset
	TArray<UObject*> SubObjects;
	GetObjectsWithOuter(this, SubObjects, false);

	for (UObject* SubObj : SubObjects)
	{
		USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(SubObj);
		if (SkelMesh && !ActiveMeshes.Contains(SkelMesh))
		{
			// Orphaned mesh found - move to Transient package
			UE_LOG(LogFleshRingAsset, Log, TEXT("CleanupOrphanedMeshes: Removing orphaned mesh '%s'"), *SkelMesh->GetName());

			SkelMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			SkelMesh->ClearFlags(RF_Public | RF_Standalone);
			SkelMesh->SetFlags(RF_Transient);
			RemovedCount++;
		}
	}

	if (RemovedCount > 0)
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("CleanupOrphanedMeshes: Removed %d orphaned mesh(es)"), RemovedCount);
		MarkPackageDirty();
	}
	else
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("CleanupOrphanedMeshes: No orphaned meshes found"));
	}

	return RemovedCount;
}
#endif
