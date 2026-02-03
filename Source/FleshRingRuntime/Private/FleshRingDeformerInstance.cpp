// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingDeformerInstance.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingTightnessShader.h"
#include "FleshRingSkinningShader.h"
#include "FleshRingComputeWorker.h"
#include "FleshRingBulgeProviders.h"
#include "FleshRingBulgeTypes.h"
#include "Components/SkinnedMeshComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"


DEFINE_LOG_CATEGORY_STATIC(LogFleshRing, Log, All);

#include UE_INLINE_GENERATED_CPP_BY_NAME(FleshRingDeformerInstance)

UFleshRingDeformerInstance::UFleshRingDeformerInstance()
{
}

void UFleshRingDeformerInstance::BeginDestroy()
{
	// Cancel pending work items on render thread
	// Prevent MeshObject dangling pointer crash on PIE shutdown
	if (Scene)
	{
		if (FFleshRingComputeWorker* Worker = FFleshRingComputeSystem::Get().GetWorker(Scene))
		{
			Worker->AbortWork(this);
		}
	}
	Scene = nullptr;

	// Wait for render thread to complete current work
	// Need flush as already queued work may be in progress
	FlushRenderingCommands();

	// ★ Explicitly release GPU buffers and caches (prevent memory leaks)
	ReleaseResources();

	// ★ Explicitly release DeformerGeometry
	DeformerGeometry.Reset();

	// Fully clean LODData array (ClearAll already called in ReleaseResources)
	LODData.Empty();

	// ★ Explicitly release weak references as well
	Deformer.Reset();
	MeshComponent.Reset();
	FleshRingComponent.Reset();

	Super::BeginDestroy();
}

void UFleshRingDeformerInstance::SetupFromDeformer(
	UFleshRingDeformer* InDeformer,
	UMeshComponent* InMeshComponent,
	UFleshRingComponent* InOwnerFleshRingComponent)
{
	Deformer = InDeformer;
	MeshComponent = InMeshComponent;
	Scene = InMeshComponent ? InMeshComponent->GetScene() : nullptr;
	LastLodIndex = INDEX_NONE;

	// NOTE: Use explicitly passed FleshRingComponent (support multi-component environments)
	if (InOwnerFleshRingComponent)
	{
		FleshRingComponent = InOwnerFleshRingComponent;
	}
	else if (AActor* Owner = InMeshComponent->GetOwner())
	{
		// Backward compatibility: Use old method when not passed (single component environment)
		FleshRingComponent = Owner->FindComponentByClass<UFleshRingComponent>();
	}

	// If FleshRingComponent is valid, register AffectedVertices for all LODs
	if (FleshRingComponent.IsValid())
	{
		USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(InMeshComponent);
		if (SkelMesh)
		{
			// Determine LOD count
			USkeletalMesh* Mesh = SkelMesh->GetSkeletalMeshAsset();
			if (Mesh)
			{
				const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
				if (RenderData)
				{
					NumLODs = RenderData->LODRenderData.Num();
					LODData.SetNum(NumLODs);

					// Register AffectedVertices for each LOD
					// Selector is automatically determined by Ring's InfluenceMode (inside RegisterAffectedVertices)
					int32 SuccessCount = 0;
					for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
					{
						LODData[LODIndex].bAffectedVerticesRegistered =
							LODData[LODIndex].AffectedVerticesManager.RegisterAffectedVertices(
								FleshRingComponent.Get(), SkelMesh, LODIndex);

						if (LODData[LODIndex].bAffectedVerticesRegistered)
						{
							SuccessCount++;
						}
					}
				}
			}
		}
	}
}

void UFleshRingDeformerInstance::AllocateResources()
{
	// Resources are allocated on-demand in EnqueueWork
}

void UFleshRingDeformerInstance::ReleaseResources()
{
	// Release cached resources for all LODs
	// ★ CL 320 restore: Keep AffectedVerticesManager and bAffectedVerticesRegistered
	//    (AffectedVertices data is needed when Deformer is reused)
	for (FLODDeformationData& Data : LODData)
	{
		// Release TightenedBindPose buffer
		if (Data.CachedTightenedBindPoseShared.IsValid())
		{
			Data.CachedTightenedBindPoseShared->SafeRelease();
			Data.CachedTightenedBindPoseShared.Reset();
		}
		Data.bTightenedBindPoseCached = false;
		Data.CachedTightnessVertexCount = 0;

		// Release recomputed normals buffer
		if (Data.CachedNormalsShared.IsValid())
		{
			Data.CachedNormalsShared->SafeRelease();
			Data.CachedNormalsShared.Reset();
		}

		// Release recomputed tangents buffer
		if (Data.CachedTangentsShared.IsValid())
		{
			Data.CachedTangentsShared->SafeRelease();
			Data.CachedTangentsShared.Reset();
		}

		// Release debug Influence buffer
		if (Data.CachedDebugInfluencesShared.IsValid())
		{
			Data.CachedDebugInfluencesShared->SafeRelease();
			Data.CachedDebugInfluencesShared.Reset();
		}

		// Release debug point buffer
		if (Data.CachedDebugPointBufferShared.IsValid())
		{
			Data.CachedDebugPointBufferShared->SafeRelease();
			Data.CachedDebugPointBufferShared.Reset();
		}

		// Release Bulge debug point buffer
		if (Data.CachedDebugBulgePointBufferShared.IsValid())
		{
			Data.CachedDebugBulgePointBufferShared->SafeRelease();
			Data.CachedDebugBulgePointBufferShared.Reset();
		}

		// Release Readback-related SharedPtr
		Data.DebugInfluenceReadbackResult.Reset();
		Data.bDebugInfluenceReadbackComplete.Reset();

		// Release source positions
		Data.CachedSourcePositions.Empty();
		Data.bSourcePositionsCached = false;
	}
}

void UFleshRingDeformerInstance::EnqueueWork(FEnqueueWorkDesc const& InDesc)
{
	// Only process during Update workload, skip Setup/Trigger phases
	if (InDesc.WorkLoadType != EWorkLoad::WorkLoad_Update)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			InDesc.FallbackDelegate.ExecuteIfBound();
		}
		return;
	}

	UFleshRingDeformer* DeformerPtr = Deformer.Get();
	USkinnedMeshComponent* SkinnedMeshComp = Cast<USkinnedMeshComponent>(MeshComponent.Get());


	if (!DeformerPtr || !SkinnedMeshComp)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	const int32 LODIndex = SkinnedMeshComp->GetPredictedLODLevel();

	// LOD validity check
	if (LODIndex < 0 || LODIndex >= NumLODs)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// Reference to current LOD data
	FLODDeformationData& CurrentLODData = LODData[LODIndex];

	// Fallback or Passthrough if AffectedVertices not registered
	const int32 TotalAffectedCount = CurrentLODData.AffectedVerticesManager.GetTotalAffectedCount();
	if (!CurrentLODData.bAffectedVerticesRegistered || TotalAffectedCount == 0)
	{
		// Check if there was previous deformation (judged by cache buffer validity)
		const bool bHadPreviousDeformation =
			CurrentLODData.CachedTightenedBindPoseShared.IsValid() &&
			CurrentLODData.CachedTightenedBindPoseShared->IsValid();

		if (bHadPreviousDeformation)
		{
			// ===== Passthrough Mode =====
			// Previous deformation existed but AffectedVertices became 0
			// → Run SkinningCS once with original data to remove tangent residue
			FSkeletalMeshObject* MeshObjectForPassthrough = SkinnedMeshComp->MeshObject;
			if (MeshObjectForPassthrough && !MeshObjectForPassthrough->IsCPUSkinned())
			{
				FFleshRingWorkItem PassthroughWorkItem;
				PassthroughWorkItem.DeformerInstance = this;
				PassthroughWorkItem.MeshObject = MeshObjectForPassthrough;
				PassthroughWorkItem.LODIndex = LODIndex;
				PassthroughWorkItem.bPassthroughMode = true;
				PassthroughWorkItem.FallbackDelegate = InDesc.FallbackDelegate;

				// Set vertex count
				const FSkeletalMeshRenderData& RenderData = MeshObjectForPassthrough->GetSkeletalMeshRenderData();
				const FSkeletalMeshLODRenderData& LODData_Render = RenderData.LODRenderData[LODIndex];
				PassthroughWorkItem.TotalVertexCount = LODData_Render.GetNumVertices();

				// Pass original source positions (for original tangent output in SkinningCS)
				if (CurrentLODData.CachedSourcePositions.Num() > 0)
				{
					PassthroughWorkItem.SourceDataPtr = MakeShared<TArray<float>>(CurrentLODData.CachedSourcePositions);
				}

				FFleshRingComputeWorker* Worker = FFleshRingComputeSystem::Get().GetWorker(Scene);
				if (Worker)
				{
					Worker->EnqueueWork(MoveTemp(PassthroughWorkItem));
				}
			}

			// Clear cache (prevent re-execution after Passthrough operation)
			CurrentLODData.CachedTightenedBindPoseShared.Reset();
			CurrentLODData.bTightenedBindPoseCached = false;
			CurrentLODData.CachedTightnessVertexCount = 0;

			// Also clear normal/tangent cache
			if (CurrentLODData.CachedNormalsShared.IsValid())
			{
				CurrentLODData.CachedNormalsShared->SafeRelease();
				CurrentLODData.CachedNormalsShared.Reset();
			}
			if (CurrentLODData.CachedTangentsShared.IsValid())
			{
				CurrentLODData.CachedTangentsShared->SafeRelease();
				CurrentLODData.CachedTangentsShared.Reset();
			}
		}
		else
		{
			// No previous deformation → existing Fallback
			if (InDesc.FallbackDelegate.IsBound())
			{
				ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
				{
					FallbackDelegate.ExecuteIfBound();
				});
			}
		}

		// Clear GPU debug buffers
		if (CurrentLODData.CachedDebugInfluencesShared.IsValid())
		{
			CurrentLODData.CachedDebugInfluencesShared->SafeRelease();
			CurrentLODData.CachedDebugInfluencesShared.Reset();
		}
		if (CurrentLODData.CachedDebugPointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugPointBufferShared->SafeRelease();
			CurrentLODData.CachedDebugPointBufferShared.Reset();
		}
		if (CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugBulgePointBufferShared->SafeRelease();
			CurrentLODData.CachedDebugBulgePointBufferShared.Reset();
		}

		return;
	}

	FSkeletalMeshObject* MeshObject = SkinnedMeshComp->MeshObject;
	if (!MeshObject || MeshObject->IsCPUSkinned())
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// Check if MeshObject has been updated at least once
	if (!MeshObject->bHasBeenUpdatedAtLeastOnce)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// Get FleshRingComputeWorker
	FFleshRingComputeWorker* Worker = FFleshRingComputeSystem::Get().GetWorker(Scene);
	if (!Worker)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("FleshRing: Cannot find ComputeWorker"));
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// Track LOD changes for invalidating previous position
	// Each LOD has separate cache so cache invalidation is not needed
	bool bInvalidatePreviousPosition = false;
	if (LODIndex != LastLodIndex)
	{
		bInvalidatePreviousPosition = true;
		LastLodIndex = LODIndex;
	}

	// ================================================================
	// Source vertex caching (only first frame of this LOD)
	// ================================================================
	if (!CurrentLODData.bSourcePositionsCached)
	{
		USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(SkinnedMeshComp);
		USkeletalMesh* SkelMesh = SkelMeshComp ? SkelMeshComp->GetSkeletalMeshAsset() : nullptr;
		if (SkelMesh)
		{
			const FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
			if (RenderData && RenderData->LODRenderData.Num() > LODIndex)
			{
				const FSkeletalMeshLODRenderData& RenderLODData = RenderData->LODRenderData[LODIndex];
				const uint32 NumVerts = RenderLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

				CurrentLODData.CachedSourcePositions.SetNum(NumVerts * 3);
				for (uint32 i = 0; i < NumVerts; ++i)
				{
					const FVector3f& Pos = RenderLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i);
					CurrentLODData.CachedSourcePositions[i * 3 + 0] = Pos.X;
					CurrentLODData.CachedSourcePositions[i * 3 + 1] = Pos.Y;
					CurrentLODData.CachedSourcePositions[i * 3 + 2] = Pos.Z;
				}
				CurrentLODData.bSourcePositionsCached = true;
			}
		}
	}

	if (!CurrentLODData.bSourcePositionsCached)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// ================================================================
	// Create and queue work item
	// ================================================================
	const TArray<FRingAffectedData>& AllRingData = CurrentLODData.AffectedVerticesManager.GetAllRingData();
	const uint32 TotalVertexCount = CurrentLODData.CachedSourcePositions.Num() / 3;

	// Prepare Ring data
	TSharedPtr<TArray<FFleshRingWorkItem::FRingDispatchData>> RingDispatchDataPtr =
		MakeShared<TArray<FFleshRingWorkItem::FRingDispatchData>>();
	RingDispatchDataPtr->Reserve(AllRingData.Num());

	// Get Ring settings from FleshRingAsset
	const TArray<FFleshRingSettings>* RingSettingsPtr = nullptr;
	if (FleshRingComponent.IsValid() && FleshRingComponent->FleshRingAsset)
	{
		RingSettingsPtr = &FleshRingComponent->FleshRingAsset->Rings;
	}

	// ===== Full mesh LayerTypes conversion (once only, shared by all Rings) =====
	// EFleshRingLayerType -> uint32 conversion
	// Lookup table directly accessible by VertexIndex on GPU
	TArray<uint32> FullMeshLayerTypes;
	{
		const TArray<EFleshRingLayerType>& CachedLayerTypes = CurrentLODData.AffectedVerticesManager.GetCachedVertexLayerTypes();
		FullMeshLayerTypes.SetNum(CachedLayerTypes.Num());
		for (int32 i = 0; i < CachedLayerTypes.Num(); ++i)
		{
			FullMeshLayerTypes[i] = static_cast<uint32>(CachedLayerTypes[i]);
		}
	}

	for (int32 RingIndex = 0; RingIndex < AllRingData.Num(); ++RingIndex)
	{
		const FRingAffectedData& RingData = AllRingData[RingIndex];
		if (RingData.Vertices.Num() == 0)
		{
			continue;
		}

		// Skip this ring if deformation is disabled
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			if (!(*RingSettingsPtr)[RingIndex].bEnableDeformation)
			{
				continue;
			}
		}

		FFleshRingWorkItem::FRingDispatchData DispatchData;
		DispatchData.OriginalRingIndex = RingIndex;  // Store original index (for settings lookup)
		DispatchData.Params = CreateTightnessParams(RingData, TotalVertexCount);

		// SmoothingBoundsZTop/Bottom settings (smoothing region Z expansion)
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& RingSettings = (*RingSettingsPtr)[RingIndex];
			DispatchData.Params.BoundsZTop = RingSettings.SmoothingBoundsZTop;
			DispatchData.Params.BoundsZBottom = RingSettings.SmoothingBoundsZBottom;
		}

		DispatchData.Indices = RingData.PackedIndices;
		DispatchData.Influences = RingData.PackedInfluences;
		DispatchData.LayerTypes = RingData.PackedLayerTypes;
		DispatchData.FullMeshLayerTypes = FullMeshLayerTypes;  // Full mesh LayerTypes (for direct GPU upload)
		DispatchData.RepresentativeIndices = RingData.RepresentativeIndices;  // For UV seam welding
		DispatchData.bHasUVDuplicates = RingData.bHasUVDuplicates;  // UV Sync skip optimization

		// ===== Smoothing region data copy (unified SmoothingRegion*) =====
		// Design: Indices = for Tightness (original SDF AABB)
		//         SmoothingRegion* = for smoothing/penetration resolution (BoundsExpand or HopBased mode)
		// Note: Same variables used regardless of BoundsExpand/HopBased mode
		DispatchData.SmoothingRegionIndices = RingData.SmoothingRegionIndices;
		DispatchData.SmoothingRegionInfluences = RingData.SmoothingRegionInfluences;
		DispatchData.SmoothingRegionIsAnchor = RingData.SmoothingRegionIsAnchor;  // Anchor flags
		DispatchData.SmoothingRegionRepresentativeIndices = RingData.SmoothingRegionRepresentativeIndices;  // For UV seam welding
		DispatchData.bSmoothingRegionHasUVDuplicates = RingData.bSmoothingRegionHasUVDuplicates;  // UV Sync skip optimization
		DispatchData.SmoothingRegionLaplacianAdjacency = RingData.SmoothingRegionLaplacianAdjacency;
		DispatchData.SmoothingRegionPBDAdjacency = RingData.SmoothingRegionPBDAdjacency;
		DispatchData.SmoothingRegionAdjacencyOffsets = RingData.SmoothingRegionAdjacencyOffsets;
		DispatchData.SmoothingRegionAdjacencyTriangles = RingData.SmoothingRegionAdjacencyTriangles;
		DispatchData.SmoothingRegionHopDistances = RingData.SmoothingRegionHopDistances;
		DispatchData.MaxSmoothingHops = RingData.MaxSmoothingHops;

		// Normal blend falloff type copy (global setting)
		if (FleshRingComponent.IsValid() && FleshRingComponent->FleshRingAsset)
		{
			DispatchData.NormalBlendFalloffType = static_cast<uint32>(FleshRingComponent->FleshRingAsset->NormalBlendFalloffType);
		}

		// SkinSDF layer separation data copy
		DispatchData.SkinVertexIndices = RingData.SkinVertexIndices;
		DispatchData.SkinVertexNormals = RingData.SkinVertexNormals;
		DispatchData.StockingVertexIndices = RingData.StockingVertexIndices;

		// Normal Recomputation adjacency data copy
		DispatchData.AdjacencyOffsets = RingData.AdjacencyOffsets;
		DispatchData.AdjacencyTriangles = RingData.AdjacencyTriangles;

		// Laplacian Smoothing adjacency data copy
		DispatchData.LaplacianAdjacencyData = RingData.LaplacianAdjacencyData;

		// Bone Ratio Preserve slice data copy
		DispatchData.OriginalBoneDistances = RingData.OriginalBoneDistances;
		DispatchData.AxisHeights = RingData.AxisHeights;
		DispatchData.SlicePackedData = RingData.SlicePackedData;

		// ===== DeformAmounts calculation (for reducing smoothing in Bulge region during Laplacian Smoothing) =====
		// Distinguish Bulge/Tightness based on AxisHeight:
		//   - Ring center (AxisHeight ≈ 0): Tightness (negative) → Apply smoothing
		//   - Ring edge (|AxisHeight| > threshold): Bulge (positive) → Reduce smoothing
		{
			const int32 NumAffected = DispatchData.Indices.Num();
			DispatchData.DeformAmounts.Reset(NumAffected);
			DispatchData.DeformAmounts.AddZeroed(NumAffected);

			// Use half the Ring height as threshold (inside this is tightness zone)
			const float RingHalfWidth = RingData.RingHeight * 0.5f;

			for (int32 i = 0; i < NumAffected; ++i)
			{
				const float AxisHeight = RingData.AxisHeights.IsValidIndex(i) ? RingData.AxisHeights[i] : 0.0f;
				const float Influence = DispatchData.Influences.IsValidIndex(i) ? DispatchData.Influences[i] : 0.0f;

				// Distance ratio from Ring center (0 = center, 1 = edge)
				const float EdgeRatio = FMath::Clamp(FMath::Abs(AxisHeight) / FMath::Max(RingHalfWidth, 0.01f), 0.0f, 2.0f);

				// EdgeRatio > 1 means Bulge region (positive)
				// EdgeRatio < 1 means Tightness region (negative)
				// Multiply by Influence to reflect actual influence amount
				DispatchData.DeformAmounts[i] = (EdgeRatio - 1.0f) * Influence;
			}
		}

		// Per-Ring RadialSmoothing settings copy
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// Disable all smoothing if bEnableRefinement/bEnableSmoothing is false
			DispatchData.bEnableRadialSmoothing = Settings.bEnableRefinement && Settings.bEnableSmoothing && Settings.bEnableRadialSmoothing;
			DispatchData.RadialBlendStrength = Settings.RadialBlendStrength;
			DispatchData.RadialSliceHeight = Settings.RadialSliceHeight;
		}

		// Per-Ring Laplacian/Taubin Smoothing settings copy
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// Disable all smoothing if bEnableRefinement/bEnableSmoothing is false
			DispatchData.bEnableLaplacianSmoothing = Settings.bEnableRefinement && Settings.bEnableSmoothing && Settings.bEnableLaplacianSmoothing;
			DispatchData.bUseTaubinSmoothing = (Settings.LaplacianSmoothingType == ELaplacianSmoothingType::Taubin);
			DispatchData.SmoothingLambda = Settings.SmoothingLambda;
			DispatchData.TaubinMu = Settings.TaubinMu;
			DispatchData.SmoothingIterations = Settings.SmoothingIterations;

			// Anchor Mode: Fix original Affected Vertices as anchors
			DispatchData.bAnchorDeformedVertices = Settings.bAnchorDeformedVertices;

			// Smoothing expansion mode settings
			// NOTE: Data is always copied (runtime toggle support)
			DispatchData.SmoothingExpandMode = Settings.SmoothingVolumeMode;
			DispatchData.HopBasedInfluences = RingData.HopBasedInfluences;

			// Note: SmoothingRegion* data is already copied above (unified variables)
			// HopBased exclusive data: HopDistances, SeedThreadIndices accessed directly from RingData

			// Heat Propagation settings copy (only valid in HopBased mode)
			DispatchData.bEnableHeatPropagation = Settings.bEnableRefinement &&
				Settings.SmoothingVolumeMode == ESmoothingVolumeMode::HopBased &&
				Settings.bEnableHeatPropagation;
			DispatchData.HeatPropagationIterations = Settings.HeatPropagationIterations;
			DispatchData.HeatPropagationLambda = Settings.HeatPropagationLambda;
			DispatchData.bIncludeBulgeVerticesAsSeeds = Settings.bIncludeBulgeVerticesAsSeeds;
		}

		// Per-Ring PBD Edge Constraint settings copy (Tolerance based)
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// Disable all refinement if bEnableRefinement is false
			DispatchData.bEnablePBDEdgeConstraint = Settings.bEnableRefinement && Settings.bEnablePBDEdgeConstraint;
			DispatchData.PBDStiffness = Settings.PBDStiffness;
			DispatchData.PBDIterations = Settings.PBDIterations;
			DispatchData.PBDTolerance = Settings.PBDTolerance;
			DispatchData.bPBDAnchorAffectedVertices = Settings.bPBDAnchorAffectedVertices;
		}

		// PBD adjacency data and full map copy
		DispatchData.PBDAdjacencyWithRestLengths = RingData.PBDAdjacencyWithRestLengths;
		DispatchData.FullInfluenceMap = RingData.FullInfluenceMap;
		DispatchData.FullDeformAmountMap = RingData.FullDeformAmountMap;
		DispatchData.FullVertexAnchorFlags = RingData.FullVertexAnchorFlags;

		// Zero array cache for when bPBDAnchorAffectedVertices=false (prevent per-tick allocation)
		if (!DispatchData.bPBDAnchorAffectedVertices && DispatchData.bEnablePBDEdgeConstraint)
		{
			// PBD target vertex count (using unified SmoothingRegion)
			const int32 NumPBDVertices = DispatchData.SmoothingRegionIndices.Num();
			const int32 NumTotalVertices = DispatchData.FullVertexAnchorFlags.Num();

			if (NumPBDVertices > 0 && NumTotalVertices > 0)
			{
				DispatchData.CachedZeroIsAnchorFlags.SetNumZeroed(NumPBDVertices);
				DispatchData.CachedZeroFullVertexAnchorFlags.SetNumZeroed(NumTotalVertices);
			}
		}

		// Per-Ring InfluenceMode check
		EFleshRingInfluenceMode RingInfluenceMode = EFleshRingInfluenceMode::Auto;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			RingInfluenceMode = (*RingSettingsPtr)[RingIndex].InfluenceMode;
		}

		// ===== VirtualBand parameter settings (always set regardless of SDF) =====
		// GPU InfluenceMode: 0=Auto/SDF, 1=VirtualRing, 2=VirtualBand
		// Note: If bUseSDFInfluence is 1, use SDF mode; if 0, branch based on InfluenceMode
		switch (RingInfluenceMode)
		{
		case EFleshRingInfluenceMode::Auto:
			DispatchData.Params.InfluenceMode = 0;
			break;
		case EFleshRingInfluenceMode::VirtualRing:
			DispatchData.Params.InfluenceMode = 1;
			break;
		case EFleshRingInfluenceMode::VirtualBand:
			DispatchData.Params.InfluenceMode = 2;
			// VirtualBand variable radius parameter settings
			if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
			{
				const FVirtualBandSettings& BandSettings = (*RingSettingsPtr)[RingIndex].VirtualBand;
				DispatchData.Params.LowerRadius = BandSettings.Lower.Radius;
				DispatchData.Params.MidLowerRadius = BandSettings.MidLowerRadius;
				DispatchData.Params.MidUpperRadius = BandSettings.MidUpperRadius;
				DispatchData.Params.UpperRadius = BandSettings.Upper.Radius;
				DispatchData.Params.LowerHeight = BandSettings.Lower.Height;
				DispatchData.Params.BandSectionHeight = BandSettings.BandHeight;
				DispatchData.Params.UpperHeight = BandSettings.Upper.Height;
			}
			break;
		}

		// Pass SDF cache data (safely copy to render thread)
		// Use SDF mode only in Auto mode + when SDF is valid (VirtualBand doesn't generate SDF)
		if (FleshRingComponent.IsValid())
		{
			const FRingSDFCache* SDFCache = FleshRingComponent->GetRingSDFCache(RingIndex);
			const bool bUseSDFForThisRing =
				(RingInfluenceMode == EFleshRingInfluenceMode::Auto) &&
				(SDFCache && SDFCache->IsValid());

			if (bUseSDFForThisRing)
			{
				DispatchData.SDFPooledTexture = SDFCache->PooledTexture;
				DispatchData.SDFBoundsMin = SDFCache->BoundsMin;
				DispatchData.SDFBoundsMax = SDFCache->BoundsMax;
				DispatchData.bHasValidSDF = true;

				// OBB support: Copy LocalToComponent transform
				DispatchData.SDFLocalToComponent = SDFCache->LocalToComponent;

				// Also set SDF bounds in Params
				DispatchData.Params.SDFBoundsMin = SDFCache->BoundsMin;
				DispatchData.Params.SDFBoundsMax = SDFCache->BoundsMax;
				DispatchData.Params.bUseSDFInfluence = 1;

				// SDF Falloff distance calculation: Based on minimum axis size of SDF volume
				// Deformation amount decreases smoothly as distance from surface increases
				FVector3f SDFExtent = SDFCache->BoundsMax - SDFCache->BoundsMin;
				float MinAxisSize = FMath::Min3(SDFExtent.X, SDFExtent.Y, SDFExtent.Z);
				DispatchData.Params.SDFInfluenceFalloffDistance = FMath::Max(MinAxisSize * 0.5f, 1.0f);

				// Ring Center: Use SDF bounds center (more accurate ring mesh center than bone position)
				// Bone position may differ from ring mesh center (MeshOffset, etc.)
				DispatchData.SDFLocalRingCenter = (SDFCache->BoundsMin + SDFCache->BoundsMax) * 0.5f;

				// Ring Axis: Ring mesh hole direction in SDF Local Space (shortest axis)
				// Uses same logic as CPU's FSDFBulgeProvider::DetectRingAxis()
				// Mismatch causes incorrect BulgeAxisDirection filtering
				if (SDFExtent.X <= SDFExtent.Y && SDFExtent.X <= SDFExtent.Z)
					DispatchData.SDFLocalRingAxis = FVector3f(1, 0, 0);
				else if (SDFExtent.Y <= SDFExtent.X && SDFExtent.Y <= SDFExtent.Z)
					DispatchData.SDFLocalRingAxis = FVector3f(0, 1, 0);
				else
					DispatchData.SDFLocalRingAxis = FVector3f(0, 0, 1);

				}
		}

		RingDispatchDataPtr->Add(MoveTemp(DispatchData));
	}

	if (RingDispatchDataPtr->Num() == 0)
	{
		// Clear normal/tangent caches (one-time cleanup, safe to call repeatedly)
		if (CurrentLODData.CachedNormalsShared.IsValid())
		{
			CurrentLODData.CachedNormalsShared->SafeRelease();
			CurrentLODData.CachedNormalsShared.Reset();
		}
		if (CurrentLODData.CachedTangentsShared.IsValid())
		{
			CurrentLODData.CachedTangentsShared->SafeRelease();
			CurrentLODData.CachedTangentsShared.Reset();
		}

		// ===== Continuous Passthrough Mode =====
		// Keep running SkinningCS with bPassthroughSkinning=true every frame
		// to avoid shader binary switch (FleshRingSkinningCS ↔ GpuSkinCacheComputeShader)
		// which causes visible FP drift on transition frames.
		if (CurrentLODData.CachedSourcePositions.Num() > 0)
		{
			FSkeletalMeshObject* MeshObjectForPassthrough = SkinnedMeshComp->MeshObject;
			if (MeshObjectForPassthrough && !MeshObjectForPassthrough->IsCPUSkinned())
			{
				FFleshRingWorkItem PassthroughWorkItem;
				PassthroughWorkItem.DeformerInstance = this;
				PassthroughWorkItem.MeshObject = MeshObjectForPassthrough;
				PassthroughWorkItem.LODIndex = LODIndex;
				PassthroughWorkItem.bPassthroughMode = true;
				PassthroughWorkItem.FallbackDelegate = InDesc.FallbackDelegate;
				PassthroughWorkItem.TotalVertexCount = TotalVertexCount;
				PassthroughWorkItem.SourceDataPtr = MakeShared<TArray<float>>(CurrentLODData.CachedSourcePositions);

				FFleshRingComputeWorker* PassthroughWorker = FFleshRingComputeSystem::Get().GetWorker(Scene);
				if (PassthroughWorker)
				{
					PassthroughWorker->EnqueueWork(MoveTemp(PassthroughWorkItem));
				}
			}
		}
		else
		{
			// No source data (never computed) → Fallback to UE default skinning
			if (InDesc.FallbackDelegate.IsBound())
			{
				ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
				{
					FallbackDelegate.ExecuteIfBound();
				});
			}
		}
		return;
	}

	// ================================================================
	// Prepare Bulge data for each Ring (SDF mode only)
	// ================================================================
	bool bAnyRingHasBulge = false;

	// Convert source positions to FVector3f array (shared by all Rings)
	TArray<FVector3f> AllVertexPositions;
	AllVertexPositions.SetNum(TotalVertexCount);
	for (uint32 i = 0; i < TotalVertexCount; ++i)
	{
		AllVertexPositions[i] = FVector3f(
			CurrentLODData.CachedSourcePositions[i * 3 + 0],
			CurrentLODData.CachedSourcePositions[i * 3 + 1],
			CurrentLODData.CachedSourcePositions[i * 3 + 2]);
	}

	// Calculate Bulge data for each Ring
	for (int32 RingIdx = 0; RingIdx < RingDispatchDataPtr->Num(); ++RingIdx)
	{
		FFleshRingWorkItem::FRingDispatchData& DispatchData = (*RingDispatchDataPtr)[RingIdx];

		// Get per-Ring Bulge settings (using OriginalRingIndex)
		const int32 OriginalIdx = DispatchData.OriginalRingIndex;
		bool bBulgeEnabledInSettings = true;
		float RingBulgeStrength = 1.0f;
		float RingMaxBulgeDistance = 10.0f;
		float RingBulgeAxialRange = 3.0f;
		float RingBulgeRadialRange = 1.5f;
		float RingBulgeRadialTaper = 0.5f;
		float RingBulgeRadialRatio = 0.7f;
		float RingUpperBulgeStrength = 1.0f;
		float RingLowerBulgeStrength = 1.0f;
		EFleshRingFalloffType RingBulgeFalloff = EFleshRingFalloffType::WendlandC2;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
		{
			bBulgeEnabledInSettings = (*RingSettingsPtr)[OriginalIdx].bEnableBulge;
			RingBulgeStrength = (*RingSettingsPtr)[OriginalIdx].BulgeIntensity;
			RingBulgeAxialRange = (*RingSettingsPtr)[OriginalIdx].BulgeAxialRange;
			RingBulgeRadialRange = (*RingSettingsPtr)[OriginalIdx].BulgeRadialRange;
			RingBulgeRadialTaper = (*RingSettingsPtr)[OriginalIdx].BulgeRadialTaper;
			RingBulgeRadialRatio = (*RingSettingsPtr)[OriginalIdx].BulgeRadialRatio;
			RingUpperBulgeStrength = (*RingSettingsPtr)[OriginalIdx].UpperBulgeStrength;
			RingLowerBulgeStrength = (*RingSettingsPtr)[OriginalIdx].LowerBulgeStrength;
			RingBulgeFalloff = (*RingSettingsPtr)[OriginalIdx].BulgeFalloff;
		}

		// Enable Bulge if bEnableBulge is true and BulgeIntensity > 0
		if (!bBulgeEnabledInSettings || RingBulgeStrength <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		// Calculate Bulge region (optimized from O(N) to O(candidates) via Spatial Hash)
		TArray<uint32> BulgeIndices;
		TArray<float> BulgeInfluences;
		TArray<FVector3f> BulgeDirections;  // Empty as GPU calculates this

		// Get Spatial Hash from AffectedVerticesManager
		const FVertexSpatialHash* SpatialHash = &CurrentLODData.AffectedVerticesManager.GetSpatialHash();

		// ===== Select Bulge Provider: Branch based on SDF availability and InfluenceMode =====
		// Get Ring InfluenceMode
		EFleshRingInfluenceMode BulgeRingInfluenceMode = EFleshRingInfluenceMode::Auto;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
		{
			BulgeRingInfluenceMode = (*RingSettingsPtr)[OriginalIdx].InfluenceMode;
		}

		if (DispatchData.bHasValidSDF)
		{
			// Auto/VirtualBand mode + SDF valid: SDF bounds-based Bulge
			FSDFBulgeProvider BulgeProvider;
			BulgeProvider.InitFromSDFCache(
				DispatchData.SDFBoundsMin,
				DispatchData.SDFBoundsMax,
				DispatchData.SDFLocalToComponent,
				RingBulgeAxialRange,
				RingBulgeRadialRange);
			BulgeProvider.RadialTaper = RingBulgeRadialTaper;
			BulgeProvider.FalloffType = RingBulgeFalloff;

			BulgeProvider.CalculateBulgeRegion(
				AllVertexPositions,
				SpatialHash,
				BulgeIndices,
				BulgeInfluences,
				BulgeDirections);
		}
		else if (BulgeRingInfluenceMode == EFleshRingInfluenceMode::VirtualBand &&
				 RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
		{
			// VirtualBand mode + SDF invalid: Variable radius-based Bulge
			const FVirtualBandSettings& BandSettings = (*RingSettingsPtr)[OriginalIdx].VirtualBand;

			// Calculate Band center/axis (from DispatchData)
			FVector3f BandCenter = FVector3f(DispatchData.Params.RingCenter);
			FVector3f BandAxis = FVector3f(DispatchData.Params.RingAxis);

			FVirtualBandInfluenceProvider BulgeProvider;
			BulgeProvider.InitFromBandSettings(
				BandSettings.Lower.Radius,
				BandSettings.MidLowerRadius,
				BandSettings.MidUpperRadius,
				BandSettings.Upper.Radius,
				BandSettings.Lower.Height,
				BandSettings.BandHeight,
				BandSettings.Upper.Height,
				BandCenter,
				BandAxis,
				RingBulgeAxialRange,
				RingBulgeRadialRange);
			BulgeProvider.FalloffType = RingBulgeFalloff;

			BulgeProvider.CalculateBulgeRegion(
				AllVertexPositions,
				SpatialHash,
				BulgeIndices,
				BulgeInfluences,
				BulgeDirections);
		}
		else
		{
			// VirtualRing mode: Fixed radius-based Bulge
			FVirtualRingBulgeProvider BulgeProvider;
			BulgeProvider.InitFromRingParams(
				FVector3f(DispatchData.Params.RingCenter),
				FVector3f(DispatchData.Params.RingAxis),
				DispatchData.Params.RingRadius,
				DispatchData.Params.RingHeight,
				RingBulgeAxialRange,
				RingBulgeRadialRange);
			BulgeProvider.RadialTaper = RingBulgeRadialTaper;
			BulgeProvider.FalloffType = RingBulgeFalloff;

			BulgeProvider.CalculateBulgeRegion(
				AllVertexPositions,
				SpatialHash,
				BulgeIndices,
				BulgeInfluences,
				BulgeDirections);
		}

		if (BulgeIndices.Num() > 0)
		{
			DispatchData.bEnableBulge = true;
			DispatchData.BulgeIndices = MoveTemp(BulgeIndices);
			DispatchData.BulgeInfluences = MoveTemp(BulgeInfluences);
			DispatchData.BulgeStrength = RingBulgeStrength;
			DispatchData.MaxBulgeDistance = RingMaxBulgeDistance;
			DispatchData.BulgeRadialRatio = RingBulgeRadialRatio;
			DispatchData.UpperBulgeStrength = RingUpperBulgeStrength;
			DispatchData.LowerBulgeStrength = RingLowerBulgeStrength;
			bAnyRingHasBulge = true;

			// ===== Set Bulge direction data =====
			// Get detected direction from SDF cache (using OriginalRingIndex)
			if (FleshRingComponent.IsValid())
			{
				const FRingSDFCache* SDFCache = FleshRingComponent->GetRingSDFCache(OriginalIdx);
				int32 DetectedDirection = SDFCache ? SDFCache->DetectedBulgeDirection : 0;
				DispatchData.DetectedBulgeDirection = DetectedDirection;

				// Get BulgeDirection mode from Ring settings
				EBulgeDirectionMode BulgeDirectionMode = EBulgeDirectionMode::Auto;
				if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
				{
					BulgeDirectionMode = (*RingSettingsPtr)[OriginalIdx].BulgeDirection;
				}

				// Calculate final direction (detected direction for Auto mode, manual otherwise)
				switch (BulgeDirectionMode)
				{
				case EBulgeDirectionMode::Auto:
					// If DetectedDirection == 0, closed mesh (Torus) → bidirectional Bulge
					DispatchData.BulgeAxisDirection = DetectedDirection;  // 0, +1, or -1
					break;
				case EBulgeDirectionMode::Bidirectional:
					DispatchData.BulgeAxisDirection = 0;  // Bidirectional
					break;
				case EBulgeDirectionMode::Positive:
					DispatchData.BulgeAxisDirection = 1;
					break;
				case EBulgeDirectionMode::Negative:
					DispatchData.BulgeAxisDirection = -1;
					break;
				}
			}

			}
	}

	// Determine whether to cache TightenedBindPose
	bool bNeedTightnessCaching = !CurrentLODData.bTightenedBindPoseCached;

	if (bNeedTightnessCaching)
	{
		CurrentLODData.bTightenedBindPoseCached = true;
		CurrentLODData.CachedTightnessVertexCount = TotalVertexCount;
		bInvalidatePreviousPosition = true;

		// Create TSharedPtr (on first cache)
		if (!CurrentLODData.CachedTightenedBindPoseShared.IsValid())
		{
			CurrentLODData.CachedTightenedBindPoseShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Create normal buffer TSharedPtr (on first cache)
		if (!CurrentLODData.CachedNormalsShared.IsValid())
		{
			CurrentLODData.CachedNormalsShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Create tangent buffer TSharedPtr (on first cache)
		if (!CurrentLODData.CachedTangentsShared.IsValid())
		{
			CurrentLODData.CachedTangentsShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Create debug Influence buffer TSharedPtr (on first cache)
		if (!CurrentLODData.CachedDebugInfluencesShared.IsValid())
		{
			CurrentLODData.CachedDebugInfluencesShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Create debug point buffer TSharedPtr (on first cache)
		if (!CurrentLODData.CachedDebugPointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugPointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Create Bulge debug point buffer TSharedPtr (on first cache)
		if (!CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugBulgePointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}
	}

	// Determine whether debug Influence output is needed
	// Only output when bShowDebugVisualization && bShowAffectedVertices are enabled in editor
	bool bOutputDebugInfluences = false;
	bool bOutputDebugPoints = false;  // Debug point output for GPU rendering
	bool bOutputDebugBulgePoints = false;  // Bulge debug point output for GPU rendering
	uint32 MaxAffectedVertexCount = 0;
	uint32 MaxBulgeVertexCount = 0;
#if WITH_EDITORONLY_DATA
	if (FleshRingComponent.IsValid() && FleshRingComponent->bShowDebugVisualization && FleshRingComponent->bShowAffectedVertices)
	{
		bOutputDebugInfluences = true;

		// Output DebugPointBuffer in GPU rendering mode
		if (FleshRingComponent->IsGPUDebugRenderingEnabled())
		{
			bOutputDebugPoints = true;
		}

		// Calculate max affected vertex count for Readback
		if (RingDispatchDataPtr.IsValid())
		{
			for (const auto& RingData : *RingDispatchDataPtr)
			{
				MaxAffectedVertexCount = FMath::Max(MaxAffectedVertexCount, RingData.Params.NumAffectedVertices);
			}
		}

		// Initialize Readback-related pointers (on first use)
		if (MaxAffectedVertexCount > 0)
		{
			if (!CurrentLODData.DebugInfluenceReadbackResult.IsValid())
			{
				CurrentLODData.DebugInfluenceReadbackResult = MakeShared<TArray<float>>();
			}
			if (!CurrentLODData.bDebugInfluenceReadbackComplete.IsValid())
			{
				CurrentLODData.bDebugInfluenceReadbackComplete = MakeShared<std::atomic<bool>>(false);
			}
			CurrentLODData.DebugInfluenceCount = MaxAffectedVertexCount;
		}
	}

	// Enable Bulge debug point output
	// When bShowDebugVisualization && bShowBulgeHeatmap && GPU rendering mode
	if (FleshRingComponent.IsValid() && FleshRingComponent->bShowDebugVisualization && FleshRingComponent->bShowBulgeHeatmap)
	{
		if (FleshRingComponent->IsGPUDebugRenderingEnabled())
		{
			bOutputDebugBulgePoints = true;

			// Calculate Bulge vertex count
			if (RingDispatchDataPtr.IsValid())
			{
				for (const auto& RingData : *RingDispatchDataPtr)
				{
					MaxBulgeVertexCount += RingData.BulgeIndices.Num();
				}
			}

			// ★ Clear existing cache buffer if MaxBulgeVertexCount == 0
			// (Fixes issue where previous frame's buffer remains when bEnableBulge is disabled)
			if (MaxBulgeVertexCount == 0 && CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
			{
				CurrentLODData.CachedDebugBulgePointBufferShared->SafeRelease();
				CurrentLODData.CachedDebugBulgePointBufferShared.Reset();
			}
		}
	}
#endif

	// Initialize buffer for GPU debug rendering
	// ★ DrawDebug method: Recalculate every frame without caching (accuracy > performance)
	// Performance degradation is acceptable for debugging purposes
	if (bOutputDebugPoints || bOutputDebugBulgePoints)
	{
		// Re-run TightnessCS/BulgeCS every frame when debug rendering is enabled
		bNeedTightnessCaching = true;

		// Create Affected debug point buffer TSharedPtr
		if (bOutputDebugPoints && !CurrentLODData.CachedDebugPointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugPointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Create Bulge debug point buffer TSharedPtr
		if (bOutputDebugBulgePoints && !CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugBulgePointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}
	}

	// Create work item
	FFleshRingWorkItem WorkItem;
	WorkItem.DeformerInstance = this;
	WorkItem.MeshObject = MeshObject;
	WorkItem.LODIndex = LODIndex;
	WorkItem.TotalVertexCount = TotalVertexCount;
	WorkItem.SourceDataPtr = MakeShared<TArray<float>>(CurrentLODData.CachedSourcePositions);
	WorkItem.RingDispatchDataPtr = RingDispatchDataPtr;

	// Pass mesh indices for Normal Recomputation
	const TArray<uint32>& MeshIndices = CurrentLODData.AffectedVerticesManager.GetCachedMeshIndices();
	if (MeshIndices.Num() > 0)
	{
		WorkItem.MeshIndicesPtr = MakeShared<TArray<uint32>>(MeshIndices);
	}

	// ===== Build unified Normal/Tangent Recompute data (merged from all Rings) =====
	// Merge all Ring indices to run NormalRecomputeCS/TangentRecomputeCS ONCE
	// This prevents overlapping regions from being overwritten by the last Ring's results
	if (RingDispatchDataPtr.IsValid() && RingDispatchDataPtr->Num() > 0 && MeshIndices.Num() > 0)
	{
		// Collect all indices from all Rings
		TSet<uint32> UnionIndexSet;
		TMap<uint32, int32> VertexToMaxHop;  // Track max hop distance per vertex
		TMap<uint32, uint32> VertexToRepresentative;  // Track representative per vertex
		int32 UnionMaxHops = 0;

		for (const FFleshRingWorkItem::FRingDispatchData& DispatchData : *RingDispatchDataPtr)
		{
			// Skip if no actual deformation
			const bool bHasDeformation =
				DispatchData.Params.TightnessStrength > KINDA_SMALL_NUMBER ||
				(DispatchData.bEnableBulge && DispatchData.BulgeStrength > KINDA_SMALL_NUMBER && DispatchData.BulgeIndices.Num() > 0);
			if (!bHasDeformation)
			{
				continue;
			}

			// Determine which indices to use (SmoothingRegion > Original)
			const bool bAnySmoothingEnabled =
				DispatchData.bEnableRadialSmoothing ||
				DispatchData.bEnableLaplacianSmoothing ||
				DispatchData.bEnablePBDEdgeConstraint;

			const bool bUseSmoothingRegion = bAnySmoothingEnabled &&
				DispatchData.SmoothingRegionIndices.Num() > 0 &&
				DispatchData.SmoothingRegionAdjacencyOffsets.Num() > 0;

			const TArray<uint32>& IndicesSource = bUseSmoothingRegion
				? DispatchData.SmoothingRegionIndices : DispatchData.Indices;
			const TArray<uint32>& RepSource = bUseSmoothingRegion
				? DispatchData.SmoothingRegionRepresentativeIndices : DispatchData.RepresentativeIndices;
			const TArray<int32>& HopSource = bUseSmoothingRegion
				? DispatchData.SmoothingRegionHopDistances : TArray<int32>();

			const bool bIsHopBased = (DispatchData.SmoothingExpandMode == ESmoothingVolumeMode::HopBased);

			// Only consider MaxSmoothingHops from HopBased rings
			if (bIsHopBased)
			{
				UnionMaxHops = FMath::Max(UnionMaxHops, DispatchData.MaxSmoothingHops);
			}

			// ===== BoundsExpand depth calculation =====
			// For BoundsExpand mode, calculate pseudo-hop based on distance from SDF bounds
			// Vertices inside SDF → depth=0 (100% recomputed normal)
			// Vertices at expanded boundary → depth=1 (100% original normal)
			constexpr int32 BoundsExpandVirtualMaxHops = 10;
			TMap<uint32, int32> BoundsExpandDepthMap;

			if (!bIsHopBased && bUseSmoothingRegion && DispatchData.bHasValidSDF && IndicesSource.Num() > 0)
			{
				UnionMaxHops = FMath::Max(UnionMaxHops, BoundsExpandVirtualMaxHops);

				const FVector SDFMin(DispatchData.SDFBoundsMin);
				const FVector SDFMax(DispatchData.SDFBoundsMax);

				// Component to SDF local transform
				const FTransform ComponentToSDFLocal = DispatchData.SDFLocalToComponent.Inverse();

				const TArray<float>& Positions = CurrentLODData.CachedSourcePositions;

				// Lambda: check if point is inside SDF box
				auto IsInsideSDFBox = [&SDFMin, &SDFMax](const FVector& Point) -> bool
				{
					return Point.X >= SDFMin.X && Point.X <= SDFMax.X &&
						   Point.Y >= SDFMin.Y && Point.Y <= SDFMax.Y &&
						   Point.Z >= SDFMin.Z && Point.Z <= SDFMax.Z;
				};

				// Lambda: compute distance to SDF box
				auto DistanceToSDFBox = [&SDFMin, &SDFMax](const FVector& Point) -> float
				{
					FVector Clamped;
					Clamped.X = FMath::Clamp(Point.X, SDFMin.X, SDFMax.X);
					Clamped.Y = FMath::Clamp(Point.Y, SDFMin.Y, SDFMax.Y);
					Clamped.Z = FMath::Clamp(Point.Z, SDFMin.Z, SDFMax.Z);
					return FVector::Dist(Point, Clamped);
				};

				// First pass: find max distance to SDF box
				float MaxDistanceToBox = 0.0f;
				for (int32 i = 0; i < IndicesSource.Num(); ++i)
				{
					const uint32 VertexIndex = IndicesSource[i];
					if (VertexIndex * 3 + 2 >= static_cast<uint32>(Positions.Num())) continue;

					FVector VertexPosComponent(
						Positions[VertexIndex * 3 + 0],
						Positions[VertexIndex * 3 + 1],
						Positions[VertexIndex * 3 + 2]
					);

					// Transform to SDF local space
					FVector VertexPosLocal = ComponentToSDFLocal.TransformPosition(VertexPosComponent);

					if (!IsInsideSDFBox(VertexPosLocal))
					{
						float Dist = DistanceToSDFBox(VertexPosLocal);
						MaxDistanceToBox = FMath::Max(MaxDistanceToBox, Dist);
					}
				}

				// Second pass: calculate depth for each vertex
				if (MaxDistanceToBox > KINDA_SMALL_NUMBER)
				{
					for (int32 i = 0; i < IndicesSource.Num(); ++i)
					{
						const uint32 VertexIndex = IndicesSource[i];
						if (VertexIndex * 3 + 2 >= static_cast<uint32>(Positions.Num())) continue;

						FVector VertexPosComponent(
							Positions[VertexIndex * 3 + 0],
							Positions[VertexIndex * 3 + 1],
							Positions[VertexIndex * 3 + 2]
						);

						FVector VertexPosLocal = ComponentToSDFLocal.TransformPosition(VertexPosComponent);

						float Depth = 0.0f;
						if (!IsInsideSDFBox(VertexPosLocal))
						{
							float Dist = DistanceToSDFBox(VertexPosLocal);
							Depth = FMath::Clamp(Dist / MaxDistanceToBox, 0.0f, 1.0f);
						}

						int32 PseudoHop = FMath::RoundToInt(Depth * BoundsExpandVirtualMaxHops);
						BoundsExpandDepthMap.Add(VertexIndex, PseudoHop);
					}
				}
			}

			for (int32 i = 0; i < IndicesSource.Num(); ++i)
			{
				const uint32 VertexIndex = IndicesSource[i];
				UnionIndexSet.Add(VertexIndex);

				// Track representative (first encountered wins)
				if (RepSource.IsValidIndex(i) && !VertexToRepresentative.Contains(VertexIndex))
				{
					VertexToRepresentative.Add(VertexIndex, RepSource[i]);
				}

				// Track hop distance (minimum hop wins for blending)
				int32 HopValue = INDEX_NONE;

				if (bIsHopBased && HopSource.IsValidIndex(i))
				{
					HopValue = HopSource[i];
				}
				else if (const int32* DepthPtr = BoundsExpandDepthMap.Find(VertexIndex))
				{
					HopValue = *DepthPtr;
				}

				if (HopValue != INDEX_NONE)
				{
					if (int32* ExistingHop = VertexToMaxHop.Find(VertexIndex))
					{
						*ExistingHop = FMath::Min(*ExistingHop, HopValue);
					}
					else
					{
						VertexToMaxHop.Add(VertexIndex, HopValue);
					}
				}
			}
		}

		// Build unified arrays
		if (UnionIndexSet.Num() > 0)
		{
			TArray<uint32> UnionIndices = UnionIndexSet.Array();
			UnionIndices.Sort();  // Sorted for consistent ordering

			TArray<uint32> UnionRepresentatives;
			TArray<int32> UnionHopDistances;
			bool bHasUVDuplicates = false;

			UnionRepresentatives.Reserve(UnionIndices.Num());
			if (VertexToMaxHop.Num() > 0)
			{
				UnionHopDistances.Reserve(UnionIndices.Num());
			}

			for (const uint32 VertexIndex : UnionIndices)
			{
				// Representative
				if (const uint32* RepPtr = VertexToRepresentative.Find(VertexIndex))
				{
					UnionRepresentatives.Add(*RepPtr);
					if (*RepPtr != VertexIndex)
					{
						bHasUVDuplicates = true;
					}
				}
				else
				{
					UnionRepresentatives.Add(VertexIndex);
				}

				// Hop distance
				if (VertexToMaxHop.Num() > 0)
				{
					if (const int32* HopPtr = VertexToMaxHop.Find(VertexIndex))
					{
						UnionHopDistances.Add(*HopPtr);
					}
					else
					{
						UnionHopDistances.Add(0);  // Default to seed (hop=0)
					}
				}
			}

			// Build unified adjacency data
			TArray<uint32> UnionAdjacencyOffsets;
			TArray<uint32> UnionAdjacencyTriangles;
			CurrentLODData.AffectedVerticesManager.BuildAdjacencyDataFromIndices(
				UnionIndices,
				MeshIndices,
				UnionAdjacencyOffsets,
				UnionAdjacencyTriangles
			);

			// Store in WorkItem
			WorkItem.UnionAffectedIndicesPtr = MakeShared<TArray<uint32>>(MoveTemp(UnionIndices));
			WorkItem.UnionAdjacencyOffsetsPtr = MakeShared<TArray<uint32>>(MoveTemp(UnionAdjacencyOffsets));
			WorkItem.UnionAdjacencyTrianglesPtr = MakeShared<TArray<uint32>>(MoveTemp(UnionAdjacencyTriangles));
			WorkItem.UnionRepresentativeIndicesPtr = MakeShared<TArray<uint32>>(MoveTemp(UnionRepresentatives));
			WorkItem.bUnionHasUVDuplicates = bHasUVDuplicates;
			WorkItem.UnionMaxHops = UnionMaxHops;

			if (UnionHopDistances.Num() > 0)
			{
				WorkItem.UnionHopDistancesPtr = MakeShared<TArray<int32>>(MoveTemp(UnionHopDistances));
			}

			UE_LOG(LogFleshRing, Verbose,
				TEXT("Unified NormalRecompute data: %d vertices from %d Rings"),
				WorkItem.UnionAffectedIndicesPtr->Num(), RingDispatchDataPtr->Num());
		}
	}

	WorkItem.bNeedTightnessCaching = bNeedTightnessCaching;
	WorkItem.bInvalidatePreviousPosition = bInvalidatePreviousPosition;
	WorkItem.CachedBufferSharedPtr = CurrentLODData.CachedTightenedBindPoseShared;  // TSharedPtr copy (ref count increase)
	WorkItem.CachedNormalsBufferSharedPtr = CurrentLODData.CachedNormalsShared;  // Normal cache buffer (ref count increase)
	WorkItem.CachedTangentsBufferSharedPtr = CurrentLODData.CachedTangentsShared;  // Tangent cache buffer (ref count increase)
	WorkItem.CachedDebugInfluencesBufferSharedPtr = CurrentLODData.CachedDebugInfluencesShared;  // Debug Influence cache buffer
	WorkItem.bOutputDebugInfluences = bOutputDebugInfluences;  // Enable debug Influence output
	WorkItem.DebugInfluenceReadbackResultPtr = CurrentLODData.DebugInfluenceReadbackResult;  // Readback result storage array
	WorkItem.bDebugInfluenceReadbackComplete = CurrentLODData.bDebugInfluenceReadbackComplete;  // Readback completion flag
	WorkItem.DebugInfluenceCount = CurrentLODData.DebugInfluenceCount;  // Vertex count for Readback

	// DebugPointBuffer fields for GPU debug rendering
	WorkItem.CachedDebugPointBufferSharedPtr = CurrentLODData.CachedDebugPointBufferShared;
	WorkItem.bOutputDebugPoints = bOutputDebugPoints;

	// Bulge DebugPointBuffer fields for GPU debug rendering
	WorkItem.CachedDebugBulgePointBufferSharedPtr = CurrentLODData.CachedDebugBulgePointBufferShared;
	WorkItem.bOutputDebugBulgePoints = bOutputDebugBulgePoints;
	WorkItem.DebugBulgePointCount = MaxBulgeVertexCount;


	// Set LocalToWorld matrix - prioritize ResolvedTargetMesh
	USkeletalMeshComponent* TargetMeshComp = nullptr;
	if (FleshRingComponent.IsValid())
	{
		TargetMeshComp = FleshRingComponent->GetResolvedTargetMesh();
	}
	if (!TargetMeshComp && MeshComponent.IsValid())
	{
		TargetMeshComp = Cast<USkeletalMeshComponent>(MeshComponent.Get());
	}

	if (TargetMeshComp)
	{
		FTransform WorldTransform = TargetMeshComp->GetComponentTransform();
		WorkItem.LocalToWorldMatrix = FMatrix44f(WorldTransform.ToMatrixWithScale());
	}

	WorkItem.FallbackDelegate = InDesc.FallbackDelegate;

	// Set Bulge global flag (for determining VolumeAccumBuffer creation)
	WorkItem.bAnyRingHasBulge = bAnyRingHasBulge;

	// Set Layer Penetration Resolution flag
	if (FleshRingComponent.IsValid() && FleshRingComponent->FleshRingAsset)
	{
		WorkItem.bEnableLayerPenetrationResolution =
			FleshRingComponent->FleshRingAsset->bEnableLayerPenetrationResolution;

		// Set Normal/Tangent Recompute flags
		WorkItem.bEnableNormalRecompute =
			FleshRingComponent->FleshRingAsset->bEnableNormalRecompute;
		WorkItem.NormalRecomputeMode =
			static_cast<uint32>(FleshRingComponent->FleshRingAsset->NormalRecomputeMethod);
		WorkItem.bEnableNormalHopBlending =
			FleshRingComponent->FleshRingAsset->bEnableNormalHopBlending;
		WorkItem.NormalBlendFalloffType =
			static_cast<uint32>(FleshRingComponent->FleshRingAsset->NormalBlendFalloffType);
		WorkItem.bEnableDisplacementBlending =
			FleshRingComponent->FleshRingAsset->bEnableDisplacementBlending;
		WorkItem.MaxDisplacementForBlend =
			FleshRingComponent->FleshRingAsset->MaxDisplacementForBlend;
		WorkItem.bEnableTangentRecompute =
			FleshRingComponent->FleshRingAsset->bEnableTangentRecompute;
	}

	// Queue work to Worker on render thread
	// ENQUEUE_RENDER_COMMAND only queues the work, actual execution happens
	// when renderer calls SubmitWork in EndOfFrameUpdate
	ENQUEUE_RENDER_COMMAND(FleshRingEnqueueWork)(
		[Worker, WorkItem = MoveTemp(WorkItem)](FRHICommandListImmediate& RHICmdList) mutable
		{
			Worker->EnqueueWork(MoveTemp(WorkItem));
		});
}

EMeshDeformerOutputBuffer UFleshRingDeformerInstance::GetOutputBuffers() const
{
	// Both Position + Tangent must be output for lighting consistency
	// Outputting only Position causes mismatch with engine's default skinning Tangent → ghosting artifacts
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition | EMeshDeformerOutputBuffer::SkinnedMeshTangents;
}

#if WITH_EDITORONLY_DATA
bool UFleshRingDeformerInstance::HasCachedDeformedGeometry(int32 LODIndex) const
{
	if (!LODData.IsValidIndex(LODIndex))
	{
		return false;
	}

	const FLODDeformationData& Data = LODData[LODIndex];
	return Data.bTightenedBindPoseCached &&
		Data.CachedTightenedBindPoseShared.IsValid() &&
		Data.CachedTightenedBindPoseShared->IsValid();
}

bool UFleshRingDeformerInstance::ReadbackDeformedGeometry(
	TArray<FVector3f>& OutPositions,
	TArray<FVector3f>& OutNormals,
	TArray<FVector4f>& OutTangents,
	int32 LODIndex)
{
	if (!HasCachedDeformedGeometry(LODIndex))
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: No cached deformed geometry for LOD %d"), LODIndex);
		return false;
	}

	const FLODDeformationData& Data = LODData[LODIndex];
	const uint32 NumVertices = Data.CachedTightnessVertexCount;

	if (NumVertices == 0)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: NumVertices is 0"));
		return false;
	}

	// Wait for GPU work completion
	FlushRenderingCommands();

	// ===== Position Readback =====
	bool bPositionSuccess = false;
	if (Data.CachedTightenedBindPoseShared.IsValid() && Data.CachedTightenedBindPoseShared->IsValid())
	{
		TRefCountPtr<FRDGPooledBuffer> PooledBuffer = *Data.CachedTightenedBindPoseShared;
		FBufferRHIRef BufferRHI = PooledBuffer->GetRHI();

		if (BufferRHI.IsValid())
		{
			// ★ Buffer size may be larger than requested due to RDG buffer pooling
			// Must use CachedTightnessVertexCount, not BufferRHI->GetSize()
			const uint32 ActualBufferSize = BufferRHI->GetSize();
			const uint32 AllocatedVertexCount = ActualBufferSize / (3 * sizeof(float));

			// Meaningful data count is the value stored at caching time
			const uint32 CachedVertexCount = Data.CachedTightnessVertexCount;

			// Check if buffer is sufficient
			if (AllocatedVertexCount < CachedVertexCount)
			{
				UE_LOG(LogFleshRing, Error, TEXT("ReadbackDeformedGeometry: Buffer too small! Allocated=%u, Cached=%u"),
					AllocatedVertexCount, CachedVertexCount);
				return false;
			}

			// Debug log (only when sizes differ)
			if (CachedVertexCount != NumVertices)
			{
				UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: CachedVertexCount (%u) != expected (%u)"),
					CachedVertexCount, NumVertices);
			}

			// ★ Read only cached vertex count (ignore RDG pooled extra data)
			const uint32 VertexCountToRead = CachedVertexCount;
			const uint32 SizeToRead = VertexCountToRead * 3 * sizeof(float);

			TArray<float> TempPositions;
			TempPositions.SetNumUninitialized(VertexCountToRead * 3);

			// UE5.7 synchronous Readback: Lock/Unlock performed on RenderThread
			TArray<float>* DestPtr = &TempPositions;
			uint32 ReadSize = SizeToRead;
			ENQUEUE_RENDER_COMMAND(ReadbackPositions)(
				[BufferRHI, ReadSize, DestPtr](FRHICommandListImmediate& RHICmdList)
				{
					void* MappedData = RHICmdList.LockBuffer(BufferRHI, 0, ReadSize, RLM_ReadOnly);
					if (MappedData)
					{
						FMemory::Memcpy(DestPtr->GetData(), MappedData, ReadSize);
						RHICmdList.UnlockBuffer(BufferRHI);
					}
				});
			FlushRenderingCommands();

			OutPositions.SetNum(VertexCountToRead);
			for (uint32 i = 0; i < VertexCountToRead; ++i)
			{
				OutPositions[i] = FVector3f(
					TempPositions[i * 3 + 0],
					TempPositions[i * 3 + 1],
					TempPositions[i * 3 + 2]);
			}
			bPositionSuccess = true;
		}
	}

	if (!bPositionSuccess)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Position readback failed"));
		return false;
	}

	// ===== Normal Readback =====
	// ★ Normal buffer is float3 format! (shader stores 3 floats per vertex)
	bool bNormalSuccess = false;
	if (Data.CachedNormalsShared.IsValid() && Data.CachedNormalsShared->IsValid())
	{
		TRefCountPtr<FRDGPooledBuffer> PooledBuffer = *Data.CachedNormalsShared;
		FBufferRHIRef BufferRHI = PooledBuffer->GetRHI();

		if (BufferRHI.IsValid())
		{
			// ★ Normal buffer is float3 format (3 floats per vertex)
			const uint32 ActualBufferSize = BufferRHI->GetSize();
			const uint32 AllocatedVertexCount = ActualBufferSize / (3 * sizeof(float));  // float3!
			const uint32 CachedVertexCount = Data.CachedTightnessVertexCount;

			// Check if buffer is sufficient
			if (AllocatedVertexCount < CachedVertexCount)
			{
				UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Normal buffer too small! Allocated=%u, Cached=%u"),
					AllocatedVertexCount, CachedVertexCount);
				// Normal is optional, not an error
			}

			// ★ Read only cached vertex count
			const uint32 VertexCountToRead = FMath::Min(CachedVertexCount, AllocatedVertexCount);
			const uint32 SizeToRead = VertexCountToRead * 3 * sizeof(float);  // float3!

			TArray<float> TempNormals;
			TempNormals.SetNumUninitialized(VertexCountToRead * 3);  // float3!

			// UE5.7 synchronous Readback method
			TArray<float>* DestPtr = &TempNormals;
			uint32 ReadSize = SizeToRead;
			ENQUEUE_RENDER_COMMAND(ReadbackNormals)(
				[BufferRHI, ReadSize, DestPtr](FRHICommandListImmediate& RHICmdList)
				{
					void* MappedData = RHICmdList.LockBuffer(BufferRHI, 0, ReadSize, RLM_ReadOnly);
					if (MappedData)
					{
						FMemory::Memcpy(DestPtr->GetData(), MappedData, ReadSize);
						RHICmdList.UnlockBuffer(BufferRHI);
					}
				});
			FlushRenderingCommands();

			OutNormals.SetNum(VertexCountToRead);
			for (uint32 i = 0; i < VertexCountToRead; ++i)
			{
				OutNormals[i] = FVector3f(
					TempNormals[i * 3 + 0],  // float3!
					TempNormals[i * 3 + 1],
					TempNormals[i * 3 + 2]);
			}
			bNormalSuccess = true;
		}
	}

	if (!bNormalSuccess)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Normal readback failed (may be disabled)"));
		// Normal is optional, not an error, return empty array
		OutNormals.Empty();
	}

	// ===== Tangent Readback =====
	bool bTangentSuccess = false;
	if (Data.CachedTangentsShared.IsValid() && Data.CachedTangentsShared->IsValid())
	{
		TRefCountPtr<FRDGPooledBuffer> PooledBuffer = *Data.CachedTangentsShared;
		FBufferRHIRef BufferRHI = PooledBuffer->GetRHI();

		if (BufferRHI.IsValid())
		{
			// ★ Use CachedTightnessVertexCount like Position (handle RDG buffer pooling)
			const uint32 ActualBufferSize = BufferRHI->GetSize();
			const uint32 AllocatedVertexCount = ActualBufferSize / (4 * sizeof(float));
			const uint32 CachedVertexCount = Data.CachedTightnessVertexCount;

			// Check if buffer is sufficient
			if (AllocatedVertexCount < CachedVertexCount)
			{
				UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Tangent buffer too small! Allocated=%u, Cached=%u"),
					AllocatedVertexCount, CachedVertexCount);
				// Tangent is optional, not an error
			}

			// ★ Read only cached vertex count
			const uint32 VertexCountToRead = FMath::Min(CachedVertexCount, AllocatedVertexCount);
			const uint32 SizeToRead = VertexCountToRead * 4 * sizeof(float);

			TArray<float> TempTangents;
			TempTangents.SetNumUninitialized(VertexCountToRead * 4);

			// UE5.7 synchronous Readback method
			TArray<float>* DestPtr = &TempTangents;
			uint32 ReadSize = SizeToRead;
			ENQUEUE_RENDER_COMMAND(ReadbackTangents)(
				[BufferRHI, ReadSize, DestPtr](FRHICommandListImmediate& RHICmdList)
				{
					void* MappedData = RHICmdList.LockBuffer(BufferRHI, 0, ReadSize, RLM_ReadOnly);
					if (MappedData)
					{
						FMemory::Memcpy(DestPtr->GetData(), MappedData, ReadSize);
						RHICmdList.UnlockBuffer(BufferRHI);
					}
				});
			FlushRenderingCommands();

			OutTangents.SetNum(VertexCountToRead);
			for (uint32 i = 0; i < VertexCountToRead; ++i)
			{
				OutTangents[i] = FVector4f(
					TempTangents[i * 4 + 0],
					TempTangents[i * 4 + 1],
					TempTangents[i * 4 + 2],
					TempTangents[i * 4 + 3]);
			}
			bTangentSuccess = true;
		}
	}

	if (!bTangentSuccess)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Tangent readback failed (may be disabled)"));
		// Tangent is also optional, not an error, return empty array
		OutTangents.Empty();
	}

	UE_LOG(LogFleshRing, Log, TEXT("ReadbackDeformedGeometry: Success - %d vertices, Normals=%d, Tangents=%d"),
		OutPositions.Num(), OutNormals.Num(), OutTangents.Num());

	return true;
}
#endif

void UFleshRingDeformerInstance::InvalidateTightnessCache(int32 DirtyRingIndex)
{
    // 1. Re-register AffectedVertices (affected vertices may change when Ring transform changes)
    if (FleshRingComponent.IsValid())
    {
        USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(MeshComponent.Get());
        if (SkelMesh)
        {
            for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
            {
                // Set Dirty Flag: specific Ring only or all
                if (DirtyRingIndex == INDEX_NONE)
                {
                    // Invalidate all
                    LODData[LODIndex].AffectedVerticesManager.MarkAllRingsDirty();
                }
                else
                {
                    // Invalidate specific Ring only
                    LODData[LODIndex].AffectedVerticesManager.MarkRingDirty(DirtyRingIndex);
                }

                // RegisterAffectedVertices only processes dirty Rings
                LODData[LODIndex].bAffectedVerticesRegistered =
                    LODData[LODIndex].AffectedVerticesManager.RegisterAffectedVertices(
                        FleshRingComponent.Get(), SkelMesh, LODIndex);
            }
        }
    }

    // 2. Invalidate TightenedBindPose cache for all LODs
    // TightnessCS will recalculate with new transform in next frame
    for (FLODDeformationData& Data : LODData)
    {
        Data.bTightenedBindPoseCached = false;

        // Note: CachedTightenedBindPoseShared/CachedNormalsShared/CachedTangentsShared
        // are not released here! When AffectedVertices == 0 in EnqueueWork(),
        // buffer validity is needed for Passthrough Skinning.
        // Released in EnqueueWork() after Passthrough completes.

        // GPU debug point buffer is not cleared here
        // Points should be visible during drag, so clear only when AffectedCount == 0
        // in EnqueueWork Fallback

        // 3. Also invalidate GPU Influence Readback cache
        // Use CPU fallback until new TightnessCS result is Readback
        if (Data.bDebugInfluenceReadbackComplete.IsValid())
        {
            Data.bDebugInfluenceReadbackComplete->store(false);
        }
        if (Data.DebugInfluenceReadbackResult.IsValid())
        {
            Data.DebugInfluenceReadbackResult->Empty();
        }
    }

    // 4. Also invalidate CPU debug cache (synchronize with GPU recalculation)
#if WITH_EDITORONLY_DATA
    if (FleshRingComponent.IsValid())
    {
        FleshRingComponent->InvalidateDebugCaches(DirtyRingIndex);
    }
#endif
}

void UFleshRingDeformerInstance::InvalidateForMeshChange()
{
	// ★ Complete reinitialization on mesh change
	// Release existing GPU buffers + Reset NumLODs/LODData + Re-register AffectedVertices

	// Step 1: Completely release existing resources
	ReleaseResources();

	// Step 2: Reinitialize LOD structure from new mesh
	USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(MeshComponent.Get());
	if (SkelMesh)
	{
		USkeletalMesh* Mesh = SkelMesh->GetSkeletalMeshAsset();
		if (Mesh)
		{
			const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
			if (RenderData)
			{
				const int32 NewNumLODs = RenderData->LODRenderData.Num();

				// Recreate array if LOD count differs
				if (NewNumLODs != NumLODs)
				{
					LODData.Empty();
					NumLODs = NewNumLODs;
					LODData.SetNum(NumLODs);
				}
				else
				{
					// Initialize all data even if LOD count is same
					for (FLODDeformationData& Data : LODData)
					{
						Data.CachedSourcePositions.Empty();
						Data.bSourcePositionsCached = false;
						Data.bTightenedBindPoseCached = false;
						Data.CachedTightnessVertexCount = 0;
						Data.bAffectedVerticesRegistered = false;
						Data.AffectedVerticesManager.MarkAllRingsDirty();
					}
				}

				// Step 3: Re-register AffectedVertices for each LOD
				if (FleshRingComponent.IsValid())
				{
					for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
					{
						LODData[LODIndex].bAffectedVerticesRegistered =
							LODData[LODIndex].AffectedVerticesManager.RegisterAffectedVertices(
								FleshRingComponent.Get(), SkelMesh, LODIndex);
					}
				}
			}
		}
	}

	// Step 4: Flush GPU commands to ensure buffer release is complete
	FlushRenderingCommands();

	// Reset LOD change tracking
	LastLodIndex = INDEX_NONE;
}