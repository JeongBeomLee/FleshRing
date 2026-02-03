// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingComputeWorker.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingSkinningShader.h"
#include "FleshRingHeatPropagationShader.h"
#include "FleshRingUVSyncShader.h"
#include "FleshRingDebugPointOutputShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "SkeletalMeshUpdater.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "RHIGPUReadback.h"
#include "FleshRingDebugTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingWorker, Log, All);

// ============================================================================
// FFleshRingComputeSystem - Singleton instance
// ============================================================================
FFleshRingComputeSystem* FFleshRingComputeSystem::Instance = nullptr;
bool FFleshRingComputeSystem::bIsRegistered = false;

// ============================================================================
// FFleshRingComputeWorker implementation
// ============================================================================

FFleshRingComputeWorker::FFleshRingComputeWorker(FSceneInterface const* InScene)
	: Scene(InScene)
{
}

FFleshRingComputeWorker::~FFleshRingComputeWorker()
{
}

bool FFleshRingComputeWorker::HasWork(FName InExecutionGroupName) const
{
	// Process work only in EndOfFrameUpdate execution group
	if (InExecutionGroupName != ComputeTaskExecutionGroup::EndOfFrameUpdate)
	{
		return false;
	}

	FScopeLock Lock(&WorkItemsLock);
	return PendingWorkItems.Num() > 0;
}

void FFleshRingComputeWorker::SubmitWork(FComputeContext& Context)
{
	// Process only in EndOfFrameUpdate execution group
	if (Context.ExecutionGroupName != ComputeTaskExecutionGroup::EndOfFrameUpdate)
	{
		return;
	}

	// Get pending work items
	TArray<FFleshRingWorkItem> WorkItemsToProcess;
	{
		FScopeLock Lock(&WorkItemsLock);
		WorkItemsToProcess = MoveTemp(PendingWorkItems);
		PendingWorkItems.Reset();
	}

	if (WorkItemsToProcess.Num() == 0)
	{
		return;
	}

	// Wait for MeshDeformer stage - this is critical!
	// Ensures execution after UpdatedFrameNumber is properly set
	FSkeletalMeshUpdater::WaitForStage(Context.GraphBuilder, ESkeletalMeshUpdateStage::MeshDeformer);

	// Execute each work item
	for (FFleshRingWorkItem& WorkItem : WorkItemsToProcess)
	{
		ExecuteWorkItem(Context.GraphBuilder, WorkItem);
	}
}

void FFleshRingComputeWorker::EnqueueWork(FFleshRingWorkItem&& InWorkItem)
{
	FScopeLock Lock(&WorkItemsLock);
	PendingWorkItems.Add(MoveTemp(InWorkItem));
}

void FFleshRingComputeWorker::AbortWork(UFleshRingDeformerInstance* InDeformerInstance)
{
	FScopeLock Lock(&WorkItemsLock);

	for (int32 i = PendingWorkItems.Num() - 1; i >= 0; --i)
	{
		if (PendingWorkItems[i].DeformerInstance.Get() == InDeformerInstance)
		{
			// Execute Fallback
			if (PendingWorkItems[i].FallbackDelegate.IsBound())
			{
				PendingWorkItems[i].FallbackDelegate.Execute();
			}
			PendingWorkItems.RemoveAt(i);
		}
	}
}

void FFleshRingComputeWorker::ExecuteWorkItem(FRDGBuilder& GraphBuilder, FFleshRingWorkItem& WorkItem)
{
	// DeformerInstance validity check (prevent dangling pointer on PIE exit)
	// MeshObject depends on DeformerInstance lifetime, so if DeformerInstance is invalidated
	// MeshObject is likely in dangling state as well
	if (!WorkItem.DeformerInstance.IsValid())
	{
		UE_LOG(LogFleshRingWorker, Verbose, TEXT("FleshRing: DeformerInstance invalidated - skipping work"));
		return;
	}

	FSkeletalMeshObject* MeshObject = WorkItem.MeshObject;
	const int32 LODIndex = WorkItem.LODIndex;
	const uint32 TotalVertexCount = WorkItem.TotalVertexCount;

	// Validity check
	if (!MeshObject || LODIndex < 0)
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	FSkeletalMeshRenderData const& RenderData = MeshObject->GetSkeletalMeshRenderData();
	if (LODIndex >= RenderData.LODRenderData.Num())
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData.LODRenderData[LODIndex];
	if (LODData.RenderSections.Num() == 0 || !LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices())
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	const int32 FirstAvailableSection = FSkeletalMeshDeformerHelpers::GetIndexOfFirstAvailableSection(MeshObject, LODIndex);
	if (FirstAvailableSection == INDEX_NONE)
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	const uint32 ActualNumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
	const uint32 ActualBufferSize = ActualNumVertices * 3;

	if (TotalVertexCount != ActualNumVertices)
	{
		UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Vertex count mismatch - cached:%d, actual:%d"),
			TotalVertexCount, ActualNumVertices);
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	FRDGExternalAccessQueue ExternalAccessQueue;

	// Allocate Position output buffer (auto ping-pong handled)
	FRDGBuffer* OutputPositionBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(
		GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingOutput"));

	if (!OutputPositionBuffer)
	{
		UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Position buffer allocation failed"));
		ExternalAccessQueue.Submit(GraphBuilder);
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	// ===== Passthrough Mode =====
	// When AffectedVertices becomes 0, run SkinningCS once with original data
	// Needed to remove tangent residue from previous deformation
	if (WorkItem.bPassthroughMode)
	{
		// Fallback if no original source positions
		if (!WorkItem.SourceDataPtr.IsValid() || WorkItem.SourceDataPtr->Num() == 0)
		{
			UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Passthrough mode but SourceDataPtr is null"));
			ExternalAccessQueue.Submit(GraphBuilder);
			WorkItem.FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Create original bind pose buffer
		FRDGBufferRef PassthroughPositionBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
			TEXT("FleshRing_PassthroughPositions")
		);
		GraphBuilder.QueueBufferUpload(
			PassthroughPositionBuffer,
			WorkItem.SourceDataPtr->GetData(),
			ActualBufferSize * sizeof(float),
			ERDGInitialDataFlags::None
		);

		// Execute SkinningCS (use original tangents - RecomputedNormals/Tangents = nullptr)
		const FSkinWeightVertexBuffer* WeightBuffer = LODData.GetSkinWeightVertexBuffer();
		FRHIShaderResourceView* InputWeightStreamSRV = WeightBuffer ?
			WeightBuffer->GetDataVertexBuffer()->GetSRV() : nullptr;

		FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

		if (!InputWeightStreamSRV)
		{
			// Just copy if no weights
			AddCopyBufferPass(GraphBuilder, OutputPositionBuffer, PassthroughPositionBuffer);
		}
		else
		{
			// Allocate Tangent output buffer
			FRDGBuffer* OutputTangentBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryTangentBuffer(
				GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingPassthroughTangent"));

			const int32 NumSections = LODData.RenderSections.Num();

			for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
			{
				const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];

				FRHIShaderResourceView* BoneMatricesSRV = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(
					MeshObject, LODIndex, SectionIndex, false);
				if (!BoneMatricesSRV) continue;

				FSkinningDispatchParams SkinParams;
				SkinParams.BaseVertexIndex = Section.BaseVertexIndex;
				SkinParams.NumVertices = Section.NumVertices;
				SkinParams.InputWeightStride = WeightBuffer->GetConstantInfluencesVertexStride();
				SkinParams.InputWeightIndexSize = WeightBuffer->GetBoneIndexByteSize() |
					(WeightBuffer->GetBoneWeightByteSize() << 8);
				SkinParams.NumBoneInfluences = WeightBuffer->GetMaxBoneInfluences();
				SkinParams.bPassthroughSkinning = true; // Skip bone skinning to avoid FP drift

				// RecomputedNormalsBuffer and RecomputedTangentsBuffer are nullptr
				// → SkinningCS uses original tangents
				DispatchFleshRingSkinningCS(GraphBuilder, SkinParams, PassthroughPositionBuffer,
					SourceTangentsSRV, OutputPositionBuffer, nullptr,
					OutputTangentBuffer, BoneMatricesSRV, nullptr, InputWeightStreamSRV,
					nullptr, nullptr);  // RecomputedNormalsBuffer, RecomputedTangentsBuffer = nullptr
			}
		}

		// Update VertexFactory buffer (invalidate previous position)
		FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(
			GraphBuilder, MeshObject, LODIndex, true);

		ExternalAccessQueue.Submit(GraphBuilder);
		return;
	}

	// TightenedBindPose buffer handling
	FRDGBufferRef TightenedBindPoseBuffer = nullptr;

	// NormalRecomputeCS output buffer (used in SkinningCS)
	FRDGBufferRef RecomputedNormalsBuffer = nullptr;

	// TangentRecomputeCS output buffer (used in SkinningCS)
	FRDGBufferRef RecomputedTangentsBuffer = nullptr;

	// DebugPointBuffer (for GPU debug rendering)
	FRDGBufferRef DebugPointBuffer = nullptr;

	// DebugBulgePointBuffer (for Bulge GPU debug rendering)
	FRDGBufferRef DebugBulgePointBuffer = nullptr;

	if (WorkItem.bNeedTightnessCaching)
	{
		// Create source buffer
		FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
			TEXT("FleshRing_SourcePositions")
		);
		GraphBuilder.QueueBufferUpload(
			SourceBuffer,
			WorkItem.SourceDataPtr->GetData(),
			ActualBufferSize * sizeof(float),
			ERDGInitialDataFlags::None
		);

		// Create TightenedBindPose buffer
		TightenedBindPoseBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
			TEXT("FleshRing_TightenedBindPose")
		);

		// Copy source
		AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, SourceBuffer);

		// ===== Create VolumeAccumBuffer (when Bulge is enabled on one or more Rings) =====
		// Each Ring uses independent VolumeAccum slot (based on OriginalRingIndex)
		FRDGBufferRef VolumeAccumBuffer = nullptr;
		const int32 NumRings = WorkItem.RingDispatchDataPtr.IsValid() ? WorkItem.RingDispatchDataPtr->Num() : 0;

		if (WorkItem.bAnyRingHasBulge && NumRings > 0)
		{
			// Calculate max OriginalRingIndex (ensure accurate buffer size even with skipped Rings)
			int32 MaxOriginalRingIndex = 0;
			for (const FFleshRingWorkItem::FRingDispatchData& DispatchData : *WorkItem.RingDispatchDataPtr)
			{
				MaxOriginalRingIndex = FMath::Max(MaxOriginalRingIndex, DispatchData.OriginalRingIndex);
			}
			const int32 VolumeBufferSize = MaxOriginalRingIndex + 1;

			VolumeAccumBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VolumeBufferSize),
				TEXT("FleshRing_VolumeAccum")
			);
			// Initialize to 0 (before Atomic operations)
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VolumeAccumBuffer, PF_R32_UINT), 0u);
		}

		// ===== Create DebugInfluencesBuffer (when debug Influence output is enabled) =====
		// Cache GPU-computed Influence values for visualization in DrawDebugPoint
		// Buffer size is summed since InfluenceCumulativeOffset accumulates across multiple Rings
		FRDGBufferRef DebugInfluencesBuffer = nullptr;
		uint32 TotalInfluenceVertices = 0;

		if (WorkItem.bOutputDebugInfluences && NumRings > 0)
		{
			// Sum NumAffectedVertices from all Rings (multi-Ring support)
			for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& Data = (*WorkItem.RingDispatchDataPtr)[RingIdx];
				TotalInfluenceVertices += Data.Params.NumAffectedVertices;
			}

			if (TotalInfluenceVertices > 0)
			{
				DebugInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalInfluenceVertices),
					TEXT("FleshRing_DebugInfluences")
				);
				// Initialize to 0
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DebugInfluencesBuffer, PF_R32_FLOAT), 0.0f);
			}
		}

		// ===== Create DebugPointBuffer (for GPU rendering) =====
		// Buffer size is summed since DebugPointCumulativeOffset accumulates across multiple Rings
		uint32 TotalAffectedVertices = 0;
		if (WorkItem.bOutputDebugPoints && NumRings > 0)
		{
			for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& Data = (*WorkItem.RingDispatchDataPtr)[RingIdx];
				TotalAffectedVertices += Data.Params.NumAffectedVertices;
			}

			if (TotalAffectedVertices > 0)
			{
				DebugPointBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FFleshRingDebugPoint), TotalAffectedVertices),
					TEXT("FleshRing_DebugPointBuffer")
				);
				// Initialize to 0
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DebugPointBuffer), 0u);
			}
		}

		// ===== Create DebugBulgePointBuffer (for Bulge GPU rendering) =====
		uint32 MaxBulgeVertices = 0;
		if (WorkItem.bOutputDebugBulgePoints && WorkItem.bAnyRingHasBulge && NumRings > 0)
		{
			// Sum total Bulge vertices (must contain all Bulge points from multiple Rings)
			for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& Data = (*WorkItem.RingDispatchDataPtr)[RingIdx];
				if (Data.bEnableBulge)
				{
					// Using FMath::Max only calculates points for the largest Ring, missing points from other Rings
					MaxBulgeVertices += Data.BulgeIndices.Num();
				}
			}

			if (MaxBulgeVertices > 0)
			{
				DebugBulgePointBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FFleshRingDebugPoint), MaxBulgeVertices),
					TEXT("FleshRing_DebugBulgePointBuffer")
				);
				// Initialize to 0
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DebugBulgePointBuffer), 0u);
			}
		}

		// Apply TightnessCS
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			// Debug point/Influence buffer offset (multi-Ring support)
			// DebugPointBaseOffset and DebugInfluenceBaseOffset are identical (same NumAffectedVertices unit)
			uint32 DebugPointCumulativeOffset = 0;

			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Create local copy (for setting inverse transform matrix)
				FTightnessDispatchParams Params = DispatchData.Params;
				if (Params.NumAffectedVertices == 0) continue;

				FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.NumAffectedVertices),
					TEXT("FleshRing_AffectedIndices")
				);
				GraphBuilder.QueueBufferUpload(
					IndicesBuffer,
					DispatchData.Indices.GetData(),
					DispatchData.Indices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Influence is computed directly on GPU

				// ===== UV Seam Welding: Create RepresentativeIndices buffer =====
				// Ensure UV duplicate vertices at the same position are deformed identically
				FRDGBufferRef RepresentativeIndicesBuffer = nullptr;
				if (DispatchData.RepresentativeIndices.Num() > 0)
				{
					RepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.RepresentativeIndices.Num()),
						TEXT("FleshRing_RepresentativeIndices")
					);
					GraphBuilder.QueueBufferUpload(
						RepresentativeIndicesBuffer,
						DispatchData.RepresentativeIndices.GetData(),
						DispatchData.RepresentativeIndices.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// Register SDF texture (Pooled → RDG)
				FRDGTextureRef SDFTextureRDG = nullptr;
				if (DispatchData.bHasValidSDF && DispatchData.SDFPooledTexture.IsValid())
				{
					SDFTextureRDG = GraphBuilder.RegisterExternalTexture(DispatchData.SDFPooledTexture);

					// OBB support: Calculate inverse of LocalToComponent
					// Used in shader to transform vertices (component space) to local space
					// Note: FTransform::Inverse() loses Shear with non-uniform scale+rotation
					// Solution: Convert to FMatrix then use FMatrix::Inverse() (preserves Shear)
					FMatrix ForwardMatrix = DispatchData.SDFLocalToComponent.ToMatrixWithScale();
					FMatrix InverseMatrix = ForwardMatrix.Inverse();
					Params.ComponentToSDFLocal = FMatrix44f(InverseMatrix);

					// Local → Component transform matrix (for accurate inverse transform with scale)
					Params.SDFLocalToComponent = FMatrix44f(DispatchData.SDFLocalToComponent.ToMatrixWithScale());

					// Ring Center/Axis (SDF Local Space) - pass accurate position even with bound expansion
					Params.SDFLocalRingCenter = DispatchData.SDFLocalRingCenter;
					Params.SDFLocalRingAxis = DispatchData.SDFLocalRingAxis;

				}
				else
				{
				}

				// Enable volume accumulation when Bulge is active (Bulge used by this Ring or other Rings)
				if (WorkItem.bAnyRingHasBulge && VolumeAccumBuffer)
				{
					Params.bAccumulateVolume = 1;
					Params.FixedPointScale = 1000.0f;  // float → uint conversion scale
					Params.RingIndex = DispatchData.OriginalRingIndex;  // Actual Ring array index (for visibility filtering)
				}

				// Enable debug Influence output
				// DebugInfluences buffer also uses DebugPointBaseOffset (same offset)
				if (WorkItem.bOutputDebugInfluences && DebugInfluencesBuffer)
				{
					Params.bOutputDebugInfluences = 1;
					Params.DebugPointBaseOffset = DebugPointCumulativeOffset;
				}

				// DebugPointBuffer is processed based on final positions in DebugPointOutputCS

				DispatchFleshRingTightnessCS(
					GraphBuilder,
					Params,
					SourceBuffer,
					IndicesBuffer,
					// Influence is computed directly on GPU
					RepresentativeIndicesBuffer,  // Representative vertex indices for UV seam welding
					TightenedBindPoseBuffer,
					SDFTextureRDG,
					VolumeAccumBuffer,
					DebugInfluencesBuffer
				);

				// Accumulate debug point/Influence offset (for next Ring)
				DebugPointCumulativeOffset += Params.NumAffectedVertices;
			}
		}

		// ===== BulgeCS Dispatch (after TightnessCS, per Ring) =====
		if (WorkItem.bAnyRingHasBulge && VolumeAccumBuffer && WorkItem.RingDispatchDataPtr.IsValid())
		{
			// Debug Bulge point buffer offset (multi-Ring support)
			uint32 DebugBulgePointCumulativeOffset = 0;

			// Dispatch BulgeCS per Ring
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Skip if Bulge is disabled or no data for this Ring
				if (!DispatchData.bEnableBulge || DispatchData.BulgeIndices.Num() == 0)
				{
					continue;
				}

				const uint32 NumBulgeVertices = DispatchData.BulgeIndices.Num();

				// Create Bulge vertex index buffer
				FRDGBufferRef BulgeIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumBulgeVertices),
					*FString::Printf(TEXT("FleshRing_BulgeVertexIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BulgeIndicesBuffer,
					DispatchData.BulgeIndices.GetData(),
					NumBulgeVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Create Bulge influence buffer
				FRDGBufferRef BulgeInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumBulgeVertices),
					*FString::Printf(TEXT("FleshRing_BulgeInfluences_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BulgeInfluencesBuffer,
					DispatchData.BulgeInfluences.GetData(),
					NumBulgeVertices * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// ===== Create separate output buffer (prevent SRV/UAV conflict) =====
				FRDGBufferRef BulgeOutputBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					*FString::Printf(TEXT("FleshRing_BulgeOutput_Ring%d"), RingIdx)
				);
				// Copy TightenedBindPose first (preserve vertices not targeted by Bulge)
				AddCopyBufferPass(GraphBuilder, BulgeOutputBuffer, TightenedBindPoseBuffer);

				// Register SDF texture for this Ring
				FRDGTextureRef RingSDFTextureRDG = nullptr;
				FMatrix44f RingComponentToSDFLocal = FMatrix44f::Identity;
				if (DispatchData.bHasValidSDF && DispatchData.SDFPooledTexture.IsValid())
				{
					RingSDFTextureRDG = GraphBuilder.RegisterExternalTexture(DispatchData.SDFPooledTexture);
					// NOTE: Using FMatrix::Inverse() instead of FTransform::Inverse() (preserves Shear with non-uniform scale+rotation),
					// When Ring rotated, some vertices in Positive Bulge region were caught in Negative direction or vice versa!
					FMatrix ForwardMatrix = DispatchData.SDFLocalToComponent.ToMatrixWithScale();
					FMatrix InverseMatrix = ForwardMatrix.Inverse();
					// This approach doesn't work!
					//FMatrix InverseMatrix = DispatchData.SDFLocalToComponent.Inverse().ToMatrixWithScale();
					RingComponentToSDFLocal = FMatrix44f(InverseMatrix);
				}

				// Set Bulge dispatch parameters
				FBulgeDispatchParams BulgeParams;
				BulgeParams.NumBulgeVertices = NumBulgeVertices;
				BulgeParams.NumTotalVertices = ActualNumVertices;
				BulgeParams.BulgeStrength = DispatchData.BulgeStrength;
				BulgeParams.MaxBulgeDistance = DispatchData.MaxBulgeDistance;
				BulgeParams.FixedPointScale = 0.001f;  // uint → float conversion scale (1/1000)
				BulgeParams.BulgeAxisDirection = DispatchData.BulgeAxisDirection;  // Direction filtering
				BulgeParams.RingIndex = DispatchData.OriginalRingIndex;  // Actual Ring array index (for visibility filtering)
				BulgeParams.BulgeRadialRatio = DispatchData.BulgeRadialRatio;  // Radial vs Axial ratio
				BulgeParams.UpperBulgeStrength = DispatchData.UpperBulgeStrength;  // Upper strength multiplier
				BulgeParams.LowerBulgeStrength = DispatchData.LowerBulgeStrength;  // Lower strength multiplier

				// SDF mode vs VirtualRing mode branching
				BulgeParams.bUseSDFInfluence = DispatchData.bHasValidSDF ? 1 : 0;

				if (DispatchData.bHasValidSDF)
				{
					// SDF mode: Set SDF-related parameters
					BulgeParams.SDFBoundsMin = DispatchData.SDFBoundsMin;
					BulgeParams.SDFBoundsMax = DispatchData.SDFBoundsMax;
					BulgeParams.ComponentToSDFLocal = RingComponentToSDFLocal;
					BulgeParams.SDFLocalRingCenter = DispatchData.SDFLocalRingCenter;
					BulgeParams.SDFLocalRingAxis = DispatchData.SDFLocalRingAxis;
				}
				else
				{
					// VirtualRing mode: Set Component Space parameters
					BulgeParams.RingCenter = DispatchData.Params.RingCenter;
					BulgeParams.RingAxis = DispatchData.Params.RingAxis;
					BulgeParams.RingHeight = DispatchData.Params.RingHeight;
				}

				// NOTE: Debug point output is handled with final positions in DebugPointOutputCS

				DispatchFleshRingBulgeCS(
					GraphBuilder,
					BulgeParams,
					TightenedBindPoseBuffer,  // INPUT (SRV) - includes Bulge results from previous Ring
					BulgeIndicesBuffer,
					BulgeInfluencesBuffer,
					VolumeAccumBuffer,
					BulgeOutputBuffer,        // OUTPUT (UAV) - separate output buffer
					RingSDFTextureRDG
				);

				// Copy result to TightenedBindPoseBuffer (next Ring accumulates on top of this result)
				AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, BulgeOutputBuffer);
			}
		}

		// ===== BoneRatioCS Dispatch (after BulgeCS, before NormalRecomputeCS) =====
		// Equalize vertices at same height (slice) to have uniform radius
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Skip if radial smoothing is disabled
				if (!DispatchData.bEnableRadialSmoothing)
				{
					continue;
				}

				// Skip if no actual deformation (TightnessStrength=0 and no effective Bulge)
				const bool bHasDeformation =
					DispatchData.Params.TightnessStrength > KINDA_SMALL_NUMBER ||
					(DispatchData.bEnableBulge && DispatchData.BulgeStrength > KINDA_SMALL_NUMBER && DispatchData.BulgeIndices.Num() > 0);
				if (!bHasDeformation)
				{
					continue;
				}

				// Skip if no slice data
				if (DispatchData.SlicePackedData.Num() == 0 || DispatchData.OriginalBoneDistances.Num() == 0)
				{
					continue;
				}

				// Skip if no axis height data (needed for Gaussian weights)
				if (DispatchData.AxisHeights.Num() == 0)
				{
					continue;
				}

				const uint32 NumAffected = DispatchData.Indices.Num();
				if (NumAffected == 0) continue;

				// Affected vertex index buffer
				FRDGBufferRef BoneRatioIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
					*FString::Printf(TEXT("FleshRing_BoneRatioIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BoneRatioIndicesBuffer,
					DispatchData.Indices.GetData(),
					NumAffected * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Influence buffer
				FRDGBufferRef BoneRatioInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumAffected),
					*FString::Printf(TEXT("FleshRing_BoneRatioInfluences_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BoneRatioInfluencesBuffer,
					DispatchData.Influences.GetData(),
					NumAffected * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// Original bone distance buffer
				FRDGBufferRef OriginalBoneDistancesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumAffected),
					*FString::Printf(TEXT("FleshRing_OriginalBoneDistances_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					OriginalBoneDistancesBuffer,
					DispatchData.OriginalBoneDistances.GetData(),
					NumAffected * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// Axis height buffer (for Gaussian weights)
				FRDGBufferRef AxisHeightsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumAffected),
					*FString::Printf(TEXT("FleshRing_AxisHeights_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					AxisHeightsBuffer,
					DispatchData.AxisHeights.GetData(),
					NumAffected * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// Slice data buffer
				FRDGBufferRef SliceDataBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.SlicePackedData.Num()),
					*FString::Printf(TEXT("FleshRing_SliceData_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SliceDataBuffer,
					DispatchData.SlicePackedData.GetData(),
					DispatchData.SlicePackedData.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Create and initialize output buffer
				// Important: Since shader only writes affected vertices,
				// must initialize with input data to preserve remaining vertices
				FRDGBufferRef BoneRatioOutputBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualNumVertices * 3),
					*FString::Printf(TEXT("FleshRing_BoneRatioOutput_Ring%d"), RingIdx)
				);
				AddCopyBufferPass(GraphBuilder, BoneRatioOutputBuffer, TightenedBindPoseBuffer);

				// BoneRatio dispatch parameters
				FBoneRatioDispatchParams BoneRatioParams;
				BoneRatioParams.NumAffectedVertices = NumAffected;
				BoneRatioParams.NumTotalVertices = ActualNumVertices;
				BoneRatioParams.RingAxis = FVector3f(DispatchData.Params.RingAxis);
				BoneRatioParams.RingCenter = FVector3f(DispatchData.Params.RingCenter);
				BoneRatioParams.BlendStrength = DispatchData.RadialBlendStrength;
				BoneRatioParams.HeightSigma = DispatchData.RadialSliceHeight;  // Sigma equal to slice height

				// BoneRatio dispatch
				DispatchFleshRingBoneRatioCS(
					GraphBuilder,
					BoneRatioParams,
					TightenedBindPoseBuffer,
					BoneRatioOutputBuffer,
					BoneRatioIndicesBuffer,
					BoneRatioInfluencesBuffer,
					OriginalBoneDistancesBuffer,
					AxisHeightsBuffer,
					SliceDataBuffer
				);

				// Copy result to TightenedBindPoseBuffer
				AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, BoneRatioOutputBuffer);

			}
		}

		// ===== HeatPropagationCS Dispatch (after BoneRatioCS, before LaplacianCS) =====
		// Delta-based Heat Propagation: Propagate deformation delta from Seed to SmoothingRegion area
		// Algorithm: Init → Diffuse × N → Apply
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Heat Propagation enable condition: bEnableHeatPropagation && HopBased mode && SmoothingRegion data exists
				if (!DispatchData.bEnableHeatPropagation || DispatchData.SmoothingExpandMode != ESmoothingVolumeMode::HopBased)
				{
					continue;
				}

				// Skip if no actual deformation (TightnessStrength=0 and no Bulge)
				const bool bHasDeformation =
					DispatchData.Params.TightnessStrength > KINDA_SMALL_NUMBER ||
					(DispatchData.bEnableBulge && DispatchData.BulgeStrength > KINDA_SMALL_NUMBER && DispatchData.BulgeIndices.Num() > 0);
				if (!bHasDeformation)
				{
					continue;
				}

				// SmoothingRegion data validation
				if (DispatchData.SmoothingRegionIndices.Num() == 0 ||
					DispatchData.SmoothingRegionIsAnchor.Num() == 0 ||
					DispatchData.SmoothingRegionLaplacianAdjacency.Num() == 0)
				{
					continue;
				}

				const uint32 NumSmoothingRegionVertices = DispatchData.SmoothingRegionIndices.Num();

				// ★ Array size consistency validation (prevent size mismatch when Smoothing Expand changes)
				// SmoothingRegionIsAnchor must have same size as SmoothingRegionIndices
				if (DispatchData.SmoothingRegionIsAnchor.Num() != (int32)NumSmoothingRegionVertices)
				{
					UE_LOG(LogFleshRingWorker, Warning,
						TEXT("FleshRing: SmoothingRegionIsAnchor size mismatch - IsAnchor:%d, Expected:%d (Ring %d). Cache regeneration required."),
						DispatchData.SmoothingRegionIsAnchor.Num(), NumSmoothingRegionVertices, RingIdx);
					continue;
				}

				// ========================================
				// 1. Original Positions buffer (bind pose)
				// ========================================
				FRDGBufferRef OriginalPositionsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					*FString::Printf(TEXT("FleshRing_HeatProp_OriginalPos_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					OriginalPositionsBuffer,
					WorkItem.SourceDataPtr->GetData(),
					ActualBufferSize * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 2. Output Positions buffer
				// Copy TightenedBindPose first (preserve non-extended vertices)
				// ========================================
				FRDGBufferRef HeatPropOutputBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					*FString::Printf(TEXT("FleshRing_HeatProp_Output_Ring%d"), RingIdx)
				);
				AddCopyBufferPass(GraphBuilder, HeatPropOutputBuffer, TightenedBindPoseBuffer);

				// ========================================
				// 3. SmoothingRegion Indices buffer
				// ========================================
				FRDGBufferRef SmoothingRegionIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_ExtIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SmoothingRegionIndicesBuffer,
					DispatchData.SmoothingRegionIndices.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 4. Seed/Barrier Flags separation
				// ========================================
				// Structure expected by shader:
				//   - IsSeedFlags: 1 = Bulge (delta propagation source), 0 = others
				//   - IsBarrierFlags: 1 = Tightness (propagation barrier), 0 = others
				//
				// SmoothingRegionIsAnchor data: 1 = Tightness, 0 = Non-Seed
				// If bIncludeBulgeVerticesAsSeeds is true, Bulge is also included
				// ========================================

				// First load original data (0=Non-Seed, 1=Tightness)
				TArray<uint32> SeedTypeData;
				SeedTypeData.SetNumUninitialized(NumSmoothingRegionVertices);
				FMemory::Memcpy(SeedTypeData.GetData(), DispatchData.SmoothingRegionIsAnchor.GetData(), NumSmoothingRegionVertices * sizeof(uint32));

				// When including Bulge vertices as Seeds (marked as 2)
				if (DispatchData.bIncludeBulgeVerticesAsSeeds && DispatchData.BulgeIndices.Num() > 0)
				{
					// Convert BulgeIndices to Set - O(M) space (M = Bulge vertex count)
					TSet<uint32> BulgeIndicesSet;
					BulgeIndicesSet.Reserve(DispatchData.BulgeIndices.Num());
					for (uint32 BulgeIdx : DispatchData.BulgeIndices)
					{
						BulgeIndicesSet.Add(BulgeIdx);
					}

					// Iterate SmoothingRegion area and mark Bulge vertices as 2
					for (uint32 ThreadIdx = 0; ThreadIdx < NumSmoothingRegionVertices; ++ThreadIdx)
					{
						if (SeedTypeData[ThreadIdx] == 0 &&
							BulgeIndicesSet.Contains(DispatchData.SmoothingRegionIndices[ThreadIdx]))
						{
							SeedTypeData[ThreadIdx] = 2;  // Bulge = 2
						}
					}
				}

				// Separate SeedTypeData: IsSeedFlags, IsBarrierFlags
				// Behavior changes based on bIncludeBulgeVerticesAsSeeds:
				//   false: Tightness is Seed (existing behavior)
				//   true:  Bulge is Seed, Tightness is Barrier (propagation blocked)
				TArray<uint32> IsSeedFlagsData;
				TArray<uint32> IsBarrierFlagsData;
				IsSeedFlagsData.SetNumUninitialized(NumSmoothingRegionVertices);
				IsBarrierFlagsData.SetNumUninitialized(NumSmoothingRegionVertices);

				for (uint32 i = 0; i < NumSmoothingRegionVertices; ++i)
				{
					if (DispatchData.bIncludeBulgeVerticesAsSeeds)
					{
						// Bulge only as Seed, Tightness as Barrier (propagation blocked)
						IsSeedFlagsData[i] = (SeedTypeData[i] == 2) ? 1 : 0;      // Bulge = Seed
						IsBarrierFlagsData[i] = (SeedTypeData[i] == 1) ? 1 : 0;   // Tightness = Barrier
					}
					else
					{
						// Tightness only as Seed, no Barrier (existing behavior)
						IsSeedFlagsData[i] = (SeedTypeData[i] == 1) ? 1 : 0;      // Tightness = Seed
						IsBarrierFlagsData[i] = 0;                                 // No Barrier
					}
				}

				// ========================================
				// 4.5. Calculate IsBoundarySeedFlags: Only Seeds with Non-Seed neighbors are boundary
				// ========================================
				// Purpose: Prevent strong deformation of internal Seeds from propagating beyond boundary
				// Only boundary Seeds set delta, internal Seeds have delta=0 (no propagation)
				constexpr uint32 MAX_NEIGHBORS_CONST = 12;
				TArray<uint32> IsBoundarySeedFlagsData;
				IsBoundarySeedFlagsData.SetNumZeroed(NumSmoothingRegionVertices);

				// Create VertexIndex → ThreadIndex reverse mapping
				TMap<uint32, uint32> VertexToThreadIndex;
				VertexToThreadIndex.Reserve(NumSmoothingRegionVertices);
				for (uint32 i = 0; i < NumSmoothingRegionVertices; ++i)
				{
					VertexToThreadIndex.Add(DispatchData.SmoothingRegionIndices[i], i);
				}

				// For each Seed, check if any neighbor is Non-Seed
				const TArray<uint32>& AdjacencyData = DispatchData.SmoothingRegionLaplacianAdjacency;
				for (uint32 i = 0; i < NumSmoothingRegionVertices; ++i)
				{
					if (IsSeedFlagsData[i] == 0)
					{
						continue;  // Non-Seed doesn't need boundary check
					}

					// Seed: boundary if any neighbor is Non-Seed
					uint32 AdjOffset = i * (1 + MAX_NEIGHBORS_CONST);
					if (AdjOffset >= (uint32)AdjacencyData.Num())
					{
						continue;
					}

					uint32 NeighborCount = AdjacencyData[AdjOffset];
					bool bHasNonSeedNeighbor = false;

					for (uint32 n = 0; n < NeighborCount && n < MAX_NEIGHBORS_CONST; ++n)
					{
						uint32 NeighborVertexIdx = AdjacencyData[AdjOffset + 1 + n];

						if (const uint32* NeighborThreadIdx = VertexToThreadIndex.Find(NeighborVertexIdx))
						{
							// Neighbor within SmoothingRegion area: check IsSeedFlags
							if (IsSeedFlagsData[*NeighborThreadIdx] == 0)
							{
								bHasNonSeedNeighbor = true;
								break;
							}
						}
						else
						{
							// Neighbor outside SmoothingRegion area → treated as Non-Seed
							bHasNonSeedNeighbor = true;
							break;
						}
					}

					IsBoundarySeedFlagsData[i] = bHasNonSeedNeighbor ? 1 : 0;
				}

				// Create IsSeedFlagsBuffer
				FRDGBufferRef IsSeedFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_IsSeed_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					IsSeedFlagsBuffer,
					IsSeedFlagsData.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Create IsBoundarySeedFlagsBuffer
				FRDGBufferRef IsBoundarySeedFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_IsBoundarySeed_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					IsBoundarySeedFlagsBuffer,
					IsBoundarySeedFlagsData.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Create IsBarrierFlagsBuffer
				FRDGBufferRef IsBarrierFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_IsBarrier_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					IsBarrierFlagsBuffer,
					IsBarrierFlagsData.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 5. Adjacency Data buffer (reuse Laplacian adjacency)
				// ========================================
				FRDGBufferRef AdjacencyDataBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.SmoothingRegionLaplacianAdjacency.Num()),
					*FString::Printf(TEXT("FleshRing_HeatProp_Adjacency_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					AdjacencyDataBuffer,
					DispatchData.SmoothingRegionLaplacianAdjacency.GetData(),
					DispatchData.SmoothingRegionLaplacianAdjacency.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 5.5. UV Seam Welding: RepresentativeIndices buffer (for HeatPropagation)
				// ========================================
				FRDGBufferRef HeatPropRepresentativeIndicesBuffer = nullptr;
				if (DispatchData.SmoothingRegionRepresentativeIndices.Num() == NumSmoothingRegionVertices)
				{
					HeatPropRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
						*FString::Printf(TEXT("FleshRing_HeatProp_RepIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						HeatPropRepresentativeIndicesBuffer,
						DispatchData.SmoothingRegionRepresentativeIndices.GetData(),
						NumSmoothingRegionVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// ========================================
				// 6. Heat Propagation Dispatch (Delta-based)
				// ========================================
				FHeatPropagationDispatchParams HeatPropParams;
				HeatPropParams.NumExtendedVertices = NumSmoothingRegionVertices;
				HeatPropParams.NumTotalVertices = ActualNumVertices;
				HeatPropParams.HeatLambda = DispatchData.HeatPropagationLambda;
				HeatPropParams.NumIterations = DispatchData.HeatPropagationIterations;

				DispatchFleshRingHeatPropagationCS(
					GraphBuilder,
					HeatPropParams,
					OriginalPositionsBuffer,       // Original bind pose
					TightenedBindPoseBuffer,       // Current deformed position (for Seed delta calculation)
					HeatPropOutputBuffer,          // Output position
					SmoothingRegionIndicesBuffer,         // SmoothingRegion area vertex indices
					IsSeedFlagsBuffer,             // Seed flags (1=Bulge, 0=others)
					IsBoundarySeedFlagsBuffer,     // Boundary Seed flags (1=has Non-Seed neighbor, 0=internal Seed or Non-Seed)
					IsBarrierFlagsBuffer,          // Barrier flags (1=Tightness/propagation blocked, 0=others)
					AdjacencyDataBuffer,           // Adjacency info (for diffusion)
					HeatPropRepresentativeIndicesBuffer   // Representative vertex indices for UV seam welding
				);

				// ========================================
				// 7. Copy result to TightenedBindPoseBuffer
				// ========================================
				AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, HeatPropOutputBuffer);
			}
		}

		// ===== PBD Edge Constraint (after BoneRatioCS, before LaplacianCS) =====
		// Tolerance-based PBD: Fix Affected Vertices (anchors) and only correct surrounding vertices
		// Preserve deformation within tolerance range, only correct extreme deformation outside range
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Skip if PBD Edge Constraint is disabled
				if (!DispatchData.bEnablePBDEdgeConstraint)
				{
					continue;
				}

				// Skip if no actual deformation (TightnessStrength=0 and no Bulge)
				const bool bHasDeformation =
					DispatchData.Params.TightnessStrength > KINDA_SMALL_NUMBER ||
					(DispatchData.bEnableBulge && DispatchData.BulgeStrength > KINDA_SMALL_NUMBER && DispatchData.BulgeIndices.Num() > 0);
				if (!bHasDeformation)
				{
					continue;
				}

				// ===== PBD region selection (use unified SmoothingRegion) =====
				const bool bUseSmoothingRegion =
					DispatchData.SmoothingRegionIndices.Num() > 0 &&
					DispatchData.SmoothingRegionIsAnchor.Num() == DispatchData.SmoothingRegionIndices.Num() &&
					DispatchData.SmoothingRegionPBDAdjacency.Num() > 0;

				// Skip if no SmoothingRegion data
				if (!bUseSmoothingRegion)
				{
					continue;
				}

				// Use unified data source
				const TArray<uint32>& IndicesSource = DispatchData.SmoothingRegionIndices;
				const TArray<uint32>& IsAnchorSource = DispatchData.SmoothingRegionIsAnchor;
				const TArray<uint32>& AdjacencySource = DispatchData.SmoothingRegionPBDAdjacency;
				const TArray<uint32>& RepresentativeSource = DispatchData.SmoothingRegionRepresentativeIndices;

				const uint32 NumAffected = IndicesSource.Num();
				if (NumAffected == 0) continue;

				// Skip if no adjacency data
				if (AdjacencySource.Num() == 0)
				{
					continue;
				}

				// FullVertexAnchorFlags validation
				if (DispatchData.FullVertexAnchorFlags.Num() == 0)
				{
					continue;
				}

				// Affected vertex index buffer
				FRDGBufferRef PBDIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
					*FString::Printf(TEXT("FleshRing_PBDIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					PBDIndicesBuffer,
					IndicesSource.GetData(),
					NumAffected * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// IsAnchorFlags buffer (per-thread anchor flags)
				// bPBDAnchorAffectedVertices=true: 1 = Affected (anchor, fixed), 0 = SmoothingRegion (free)
				// bPBDAnchorAffectedVertices=false: all vertices are 0 (free, PBD applied)
				FRDGBufferRef IsAnchorFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
					*FString::Printf(TEXT("FleshRing_PBDIsAnchor_Ring%d"), RingIdx)
				);

				// If bPBDAnchorAffectedVertices is false, release all anchors (all vertices free)
				if (DispatchData.bPBDAnchorAffectedVertices)
				{
					// Use existing IsAnchor data (Affected=1, SmoothingRegion=0)
					GraphBuilder.QueueBufferUpload(
						IsAnchorFlagsBuffer,
						IsAnchorSource.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}
				else
				{
					// Use cached Zero array (prevent per-tick allocation)
					GraphBuilder.QueueBufferUpload(
						IsAnchorFlagsBuffer,
						DispatchData.CachedZeroIsAnchorFlags.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// FullVertexAnchorFlags buffer (full mesh size, for neighbor anchor lookup)
				FRDGBufferRef FullVertexAnchorFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.FullVertexAnchorFlags.Num()),
					*FString::Printf(TEXT("FleshRing_FullVertexAnchorFlags_Ring%d"), RingIdx)
				);

				if (DispatchData.bPBDAnchorAffectedVertices)
				{
					// Use existing FullVertexAnchorFlags
					GraphBuilder.QueueBufferUpload(
						FullVertexAnchorFlagsBuffer,
						DispatchData.FullVertexAnchorFlags.GetData(),
						DispatchData.FullVertexAnchorFlags.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}
				else
				{
					// Use cached Zero array (prevent per-tick allocation)
					GraphBuilder.QueueBufferUpload(
						FullVertexAnchorFlagsBuffer,
						DispatchData.CachedZeroFullVertexAnchorFlags.GetData(),
						DispatchData.FullVertexAnchorFlags.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// PBD adjacency data buffer (includes rest length)
				FRDGBufferRef PBDAdjacencyBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AdjacencySource.Num()),
					*FString::Printf(TEXT("FleshRing_PBDAdjacency_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					PBDAdjacencyBuffer,
					AdjacencySource.GetData(),
					AdjacencySource.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ===== UV Seam Welding: Create RepresentativeIndices buffer (for PBD) =====
				// RepresentativeSource is already selected based on bUseSmoothingRegion (defined above)
				FRDGBufferRef PBDRepresentativeIndicesBuffer = nullptr;
				if (RepresentativeSource.Num() > 0 && RepresentativeSource.Num() == static_cast<int32>(NumAffected))
				{
					PBDRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_PBDRepIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						PBDRepresentativeIndicesBuffer,
						RepresentativeSource.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// PBD dispatch parameters (Tolerance-based)
				FPBDEdgeDispatchParams PBDParams;
				PBDParams.NumAffectedVertices = NumAffected;
				PBDParams.NumTotalVertices = ActualNumVertices;
				PBDParams.Stiffness = DispatchData.PBDStiffness;
				PBDParams.NumIterations = DispatchData.PBDIterations;
				PBDParams.Tolerance = DispatchData.PBDTolerance;

				// PBD Edge Constraint dispatch (Tolerance-based, in-place ping-pong)
				DispatchFleshRingPBDEdgeCS_MultiPass(
					GraphBuilder,
					PBDParams,
					TightenedBindPoseBuffer,
					PBDIndicesBuffer,
					PBDRepresentativeIndicesBuffer,  // Representative vertex indices for UV seam welding
					IsAnchorFlagsBuffer,             // per-thread anchor flags
					FullVertexAnchorFlagsBuffer,           // full mesh anchor map (for neighbor lookup)
					PBDAdjacencyBuffer
				);

				// [DEBUG] PBDEdgeCS log (uncomment if needed)
				// UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] PBDEdgeCS Ring[%d]: Tolerance=%.2f, %d vertices, Stiffness=%.2f, Iterations=%d"),
				// 	RingIdx, PBDParams.Tolerance, NumAffected, PBDParams.Stiffness, PBDParams.NumIterations);
			}
		}

		// ===== LaplacianCS Dispatch (after PBD Edge Constraint, before LayerPenetrationCS) =====
		// Apply overall mesh smoothing (smooth boundary regions)
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Skip if Laplacian smoothing is disabled
				if (!DispatchData.bEnableLaplacianSmoothing)
				{
					continue;
				}

				// Skip if no actual deformation (TightnessStrength=0 and no Bulge)
				// Smoothing is meaningless when mesh has no deformation
				const bool bHasDeformation =
					DispatchData.Params.TightnessStrength > KINDA_SMALL_NUMBER ||
					(DispatchData.bEnableBulge && DispatchData.BulgeStrength > KINDA_SMALL_NUMBER && DispatchData.BulgeIndices.Num() > 0);
				if (!bHasDeformation)
				{
					continue;
				}

				// ===== Smoothing region selection (use unified SmoothingRegion) =====
				// [Design] Use SmoothingRegion data if available, otherwise use original
				const bool bUseSmoothingRegion =
					DispatchData.SmoothingRegionIndices.Num() > 0 &&
					DispatchData.SmoothingRegionInfluences.Num() == DispatchData.SmoothingRegionIndices.Num() &&
					DispatchData.SmoothingRegionLaplacianAdjacency.Num() > 0;

				// Select data source to use (unified: SmoothingRegion > Original)
				const TArray<uint32>& IndicesSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionIndices : DispatchData.Indices;
				const TArray<float>& InfluenceSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionInfluences : DispatchData.Influences;
				const TArray<uint32>& AdjacencySource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionLaplacianAdjacency : DispatchData.LaplacianAdjacencyData;

				// Skip if no adjacency data
				if (AdjacencySource.Num() == 0)
				{
					continue;
				}

				const uint32 NumSmoothingVertices = IndicesSource.Num();
				if (NumSmoothingVertices == 0) continue;

				// Vertex index buffer
				FRDGBufferRef LaplacianIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingVertices),
					*FString::Printf(TEXT("FleshRing_LaplacianIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					LaplacianIndicesBuffer,
					IndicesSource.GetData(),
					NumSmoothingVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Influence buffer
				FRDGBufferRef LaplacianInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumSmoothingVertices),
					*FString::Printf(TEXT("FleshRing_LaplacianInfluences_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					LaplacianInfluencesBuffer,
					InfluenceSource.GetData(),
					NumSmoothingVertices * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// Laplacian adjacency data buffer
				FRDGBufferRef LaplacianAdjacencyBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AdjacencySource.Num()),
					*FString::Printf(TEXT("FleshRing_LaplacianAdjacency_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					LaplacianAdjacencyBuffer,
					AdjacencySource.GetData(),
					AdjacencySource.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Laplacian/Taubin dispatch parameters (use UI setting values)
				FLaplacianDispatchParams LaplacianParams;
				LaplacianParams.NumAffectedVertices = NumSmoothingVertices;
				LaplacianParams.NumTotalVertices = ActualNumVertices;
				LaplacianParams.SmoothingLambda = DispatchData.SmoothingLambda;
				LaplacianParams.NumIterations = DispatchData.SmoothingIterations;
				// Taubin smoothing (prevent shrinkage)
				LaplacianParams.bUseTaubinSmoothing = DispatchData.bUseTaubinSmoothing;
				LaplacianParams.TaubinMu = DispatchData.TaubinMu;
				// Enable stocking layer smoothing exclusion - prevent cracks in separated mesh
				LaplacianParams.bExcludeStockingFromSmoothing = true;
				// Anchor Mode: Fix original Affected Vertices (use IsAnchorFlags buffer)
				LaplacianParams.bAnchorDeformedVertices = DispatchData.bAnchorDeformedVertices;

				// ===== Create VertexLayerTypes buffer (for stocking smoothing exclusion) =====
				// [Optimization] Use FullMeshLayerTypes directly - remove shrink→expand conversion
				// Full mesh size array allows direct lookup by VertexIndex
				FRDGBufferRef LaplacianLayerTypesBuffer = nullptr;
				if (DispatchData.FullMeshLayerTypes.Num() > 0)
				{
					LaplacianLayerTypesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), DispatchData.FullMeshLayerTypes.Num()),
						*FString::Printf(TEXT("FleshRing_LaplacianLayerTypes_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LaplacianLayerTypesBuffer,
						DispatchData.FullMeshLayerTypes.GetData(),
						DispatchData.FullMeshLayerTypes.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// ===== UV Seam Welding: Create RepresentativeIndices buffer (for LaplacianCS) =====
				const TArray<uint32>& RepresentativeSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionRepresentativeIndices
					: DispatchData.RepresentativeIndices;

				FRDGBufferRef LaplacianRepresentativeIndicesBuffer = nullptr;
				if (RepresentativeSource.Num() > 0 && RepresentativeSource.Num() == static_cast<int32>(NumSmoothingVertices))
				{
					LaplacianRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingVertices),
						*FString::Printf(TEXT("FleshRing_LaplacianRepIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LaplacianRepresentativeIndicesBuffer,
						RepresentativeSource.GetData(),
						NumSmoothingVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// ===== Create IsAnchor buffer (for anchor mode) =====
				// Original Affected Vertices (Seeds) = anchor (skip smoothing)
				// Extended region = apply smoothing
				const TArray<uint32>& IsAnchorSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionIsAnchor : TArray<uint32>();

				FRDGBufferRef LaplacianIsAnchorBuffer = nullptr;
				if (LaplacianParams.bAnchorDeformedVertices && IsAnchorSource.Num() == static_cast<int32>(NumSmoothingVertices))
				{
					LaplacianIsAnchorBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingVertices),
						*FString::Printf(TEXT("FleshRing_LaplacianIsAnchor_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LaplacianIsAnchorBuffer,
						IsAnchorSource.GetData(),
						NumSmoothingVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// Laplacian MultiPass dispatch (in-place smoothing)
				DispatchFleshRingLaplacianCS_MultiPass(
					GraphBuilder,
					LaplacianParams,
					TightenedBindPoseBuffer,
					LaplacianIndicesBuffer,
					LaplacianInfluencesBuffer,
					LaplacianRepresentativeIndicesBuffer,  // Representative vertex indices for UV seam welding
					LaplacianAdjacencyBuffer,
					LaplacianLayerTypesBuffer,  // For stocking smoothing exclusion
					LaplacianIsAnchorBuffer     // For anchor mode (disabled if nullptr)
				);

			}
		}

		// ===== Layer Penetration Resolution =====
		// Ensure stocking layer always stays outside skin layer
		// Simple ON/OFF toggle: if OFF, skip entire dispatch
		{
			// Track state changes (detect ON↔OFF toggle)
			static bool bLastEnabled = true;  // Default true
			if (bLastEnabled != WorkItem.bEnableLayerPenetrationResolution)
			{
				UE_LOG(LogFleshRingWorker, Warning, TEXT("[LayerPenetration] %s"),
					WorkItem.bEnableLayerPenetrationResolution ? TEXT("ENABLED") : TEXT("DISABLED"));
				bLastEnabled = WorkItem.bEnableLayerPenetrationResolution;
			}
		}

		// ===== LayerPenetrationCS disabled =====
		// Testing replacement with per-layer Tightness differentiation (50%)
		// To enable, change condition below to true
		constexpr bool bForceDisableLayerPenetration = true;

		if (!WorkItem.bEnableLayerPenetrationResolution || bForceDisableLayerPenetration)
		{
			// OFF: Skip dispatch (do nothing)
		}
		else if (WorkItem.RingDispatchDataPtr.IsValid() && WorkItem.MeshIndicesPtr.IsValid())
		{
			const TArray<uint32>& MeshIndices = *WorkItem.MeshIndicesPtr;
			const uint32 NumTriangles = MeshIndices.Num() / 3;

			if (NumTriangles > 0)
			{
				// Create triangle index buffer (shared by all Rings)
				FRDGBufferRef LayerTriIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MeshIndices.Num()),
					TEXT("FleshRing_LayerTriIndices")
				);
				GraphBuilder.QueueBufferUpload(
					LayerTriIndicesBuffer,
					MeshIndices.GetData(),
					MeshIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

					// Skip if no layer type data
					if (DispatchData.LayerTypes.Num() == 0)
					{
						// [Debug] First frame only
						static TSet<int32> LoggedLayerSkipRings;
						if (!LoggedLayerSkipRings.Contains(RingIdx))
						{
							UE_LOG(LogFleshRingWorker, Warning, TEXT("[LayerPenetration] Ring[%d]: SKIPPED - LayerTypes is EMPTY!"), RingIdx);
							LoggedLayerSkipRings.Add(RingIdx);
						}
						continue;
					}

					// [Debug] Layer type distribution log (first frame only)
					static TSet<int32> LoggedLayerDistribution;
					if (!LoggedLayerDistribution.Contains(RingIdx))
					{
						int32 SkinCount = 0, StockingCount = 0, UnderwearCount = 0, OuterwearCount = 0, UnknownCount = 0;
						for (uint32 LayerType : DispatchData.LayerTypes)
						{
							switch (LayerType)
							{
								case 0: SkinCount++; break;
								case 1: StockingCount++; break;
								case 2: UnderwearCount++; break;
								case 3: OuterwearCount++; break;
								default: UnknownCount++; break;
							}
						}
						UE_LOG(LogFleshRingWorker, Warning,
							TEXT("[LayerPenetration] Ring[%d] LayerTypes: Skin=%d, Stocking=%d, Underwear=%d, Outerwear=%d, Unknown=%d"),
							RingIdx, SkinCount, StockingCount, UnderwearCount, OuterwearCount, UnknownCount);

						// Warn if layer separation not possible
						if (SkinCount == 0 || StockingCount == 0)
						{
							UE_LOG(LogFleshRingWorker, Error,
								TEXT("[LayerPenetration] Ring[%d] WARNING: No layer separation possible! Need both Skin AND Stocking."),
								RingIdx);
							UE_LOG(LogFleshRingWorker, Error,
								TEXT("  → Check material names contain keywords: 'skin'/'body' for Skin, 'stocking'/'sock'/'tights' for Stocking"));
							UE_LOG(LogFleshRingWorker, Error,
								TEXT("  → Or configure MaterialLayerMappings in FleshRingAsset"));
						}
						LoggedLayerDistribution.Add(RingIdx);
					}

					// ===== Region selection (use unified SmoothingRegion) =====
					// - ANY Smoothing ON:  Use SmoothingRegionIndices
					// - ALL Smoothing OFF: Indices (default SDF volume) - Only Tightness/Bulge work
					const bool bAnySmoothingEnabled =
						DispatchData.bEnableRadialSmoothing ||
						DispatchData.bEnableLaplacianSmoothing ||
						DispatchData.bEnablePBDEdgeConstraint;

					const bool bUseSmoothingRegion =
						bAnySmoothingEnabled &&
						DispatchData.SmoothingRegionIndices.Num() > 0 &&
						DispatchData.FullMeshLayerTypes.Num() > 0;

					const TArray<uint32>& PPIndices = bUseSmoothingRegion
						? DispatchData.SmoothingRegionIndices : DispatchData.Indices;

					const uint32 NumAffected = PPIndices.Num();
					if (NumAffected == 0) continue;

					// Affected vertex index buffer
					FRDGBufferRef LayerAffectedIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_LayerAffectedIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LayerAffectedIndicesBuffer,
						PPIndices.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// [Optimization] Use FullMeshLayerTypes directly - remove shrink→expand conversion
					// Full mesh size array allows direct lookup by VertexIndex
					FRDGBufferRef VertexLayerTypesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), DispatchData.FullMeshLayerTypes.Num()),
						*FString::Printf(TEXT("FleshRing_VertexLayerTypes_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						VertexLayerTypesBuffer,
						DispatchData.FullMeshLayerTypes.GetData(),
						DispatchData.FullMeshLayerTypes.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// NOTE: Normal buffer is no longer used (replaced with radial direction)
					// Shader calculates radial direction from RingCenter/RingAxis for alignment check
					// Create dummy buffer for function signature compatibility
					FRDGBufferRef LayerNormalsBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(float), 3),  // Minimum size (not used)
						*FString::Printf(TEXT("FleshRing_LayerNormals_Dummy_Ring%d"), RingIdx)
					);

					// LayerPenetration dispatch parameters
					// v4: Ensure adequate separation distance (too small won't resolve penetration)
					FLayerPenetrationDispatchParams LayerParams;
					LayerParams.NumAffectedVertices = NumAffected;
					LayerParams.NumTriangles = NumTriangles;
					LayerParams.MinSeparation = 0.02f;   // 0.2mm minimum separation
					LayerParams.MaxPushDistance = 1.0f;  // 1cm max push per iteration
					LayerParams.RingCenter = FVector3f(DispatchData.Params.RingCenter);
					LayerParams.RingAxis = FVector3f(DispatchData.Params.RingAxis);
					LayerParams.NumIterations = 8;       // 8 iterations (1cm×8=8cm max)
					// Dynamic separation and push parameters
					LayerParams.TightnessStrength = DispatchData.Params.TightnessStrength;
					LayerParams.OuterLayerPushRatio = 1.0f;  // Stocking 100% outward (skin stays in place)
					LayerParams.InnerLayerPushRatio = 0.0f;  // Don't push skin

					// LayerPenetration dispatch
					DispatchFleshRingLayerPenetrationCS(
						GraphBuilder,
						LayerParams,
						TightenedBindPoseBuffer,
						LayerNormalsBuffer,
						VertexLayerTypesBuffer,
						LayerAffectedIndicesBuffer,
						LayerTriIndicesBuffer
					);

				}
			}
		}

		// ===== SkinSDF Layer Separation (after LayerPenetrationCS) =====
		// Ensure complete layer separation using skin vertex-based implicit surface
		// Push stocking vertices outward if they are inside skin
		if (WorkItem.bEnableLayerPenetrationResolution && WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Process only if both skin/stocking vertices exist
				if (DispatchData.SkinVertexIndices.Num() == 0 || DispatchData.StockingVertexIndices.Num() == 0)
				{
					continue;
				}

				// Skin vertex index buffer
				FRDGBufferRef SkinIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.SkinVertexIndices.Num()),
					*FString::Printf(TEXT("FleshRing_SkinIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SkinIndicesBuffer,
					DispatchData.SkinVertexIndices.GetData(),
					DispatchData.SkinVertexIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Skin normal buffer (radial direction)
				FRDGBufferRef SkinNormalsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), DispatchData.SkinVertexNormals.Num()),
					*FString::Printf(TEXT("FleshRing_SkinNormals_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SkinNormalsBuffer,
					DispatchData.SkinVertexNormals.GetData(),
					DispatchData.SkinVertexNormals.Num() * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// Stocking vertex index buffer
				FRDGBufferRef StockingIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.StockingVertexIndices.Num()),
					*FString::Printf(TEXT("FleshRing_StockingIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					StockingIndicesBuffer,
					DispatchData.StockingVertexIndices.GetData(),
					DispatchData.StockingVertexIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ===== SkinSDF pass disabled =====
				// Testing replacement with per-layer Tightness differentiation (50%)
				// To enable, change bEnableSkinSDFSeparation = true
				constexpr bool bEnableSkinSDFSeparation = false;

				if (bEnableSkinSDFSeparation)
				{
					// SkinSDF dispatch parameters
					FSkinSDFDispatchParams SkinSDFParams;
					SkinSDFParams.NumStockingVertices = DispatchData.StockingVertexIndices.Num();
					SkinSDFParams.NumSkinVertices = DispatchData.SkinVertexIndices.Num();
					SkinSDFParams.NumTotalVertices = ActualNumVertices;
					SkinSDFParams.MinSeparation = 0.005f;
					SkinSDFParams.TargetSeparation = 0.02f;
					SkinSDFParams.MaxPushDistance = 0.5f;
					SkinSDFParams.MaxPullDistance = 0.0f;
					SkinSDFParams.MaxIterations = 50;
					SkinSDFParams.RingAxis = FVector3f(DispatchData.Params.RingAxis);
					SkinSDFParams.RingCenter = FVector3f(DispatchData.Params.RingCenter);

					DispatchFleshRingSkinSDFCS(
						GraphBuilder,
						SkinSDFParams,
						TightenedBindPoseBuffer,
						SkinIndicesBuffer,
						SkinNormalsBuffer,
						StockingIndicesBuffer
					);

				}
			}
		}

		// ===== NormalRecomputeCS Dispatch (Unified - after all deformations) =====
		// Recompute normals ONCE using unified data merged from all Rings
		// This prevents overlapping regions from being overwritten by the last Ring's results
		if (WorkItem.bEnableNormalRecompute && WorkItem.MeshIndicesPtr.IsValid() &&
			WorkItem.UnionAffectedIndicesPtr.IsValid() && WorkItem.UnionAffectedIndicesPtr->Num() > 0 &&
			WorkItem.UnionAdjacencyOffsetsPtr.IsValid() && WorkItem.UnionAdjacencyTrianglesPtr.IsValid())
		{
			const TArray<uint32>& MeshIndices = *WorkItem.MeshIndicesPtr;
			const TArray<uint32>& UnionIndices = *WorkItem.UnionAffectedIndicesPtr;
			const TArray<uint32>& UnionAdjacencyOffsets = *WorkItem.UnionAdjacencyOffsetsPtr;
			const TArray<uint32>& UnionAdjacencyTriangles = *WorkItem.UnionAdjacencyTrianglesPtr;

			const uint32 NumUnionAffected = UnionIndices.Num();

			if (NumUnionAffected > 0 && MeshIndices.Num() > 0 &&
				UnionAdjacencyOffsets.Num() > 0 && UnionAdjacencyTriangles.Num() > 0)
			{
				// Create mesh index buffer
				FRDGBufferRef MeshIndexBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MeshIndices.Num()),
					TEXT("FleshRing_MeshIndices")
				);
				GraphBuilder.QueueBufferUpload(
					MeshIndexBuffer,
					MeshIndices.GetData(),
					MeshIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Get SourceTangents SRV (includes original normals)
				FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();
				if (!SourceTangentsSRV)
				{
					UE_LOG(LogFleshRingWorker, Warning, TEXT("[NormalRecompute] SourceTangentsSRV is null, skipping"));
				}

				// Create original position buffer (bind pose)
				FRDGBufferRef OriginalPositionsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					TEXT("FleshRing_OriginalPositions")
				);
				GraphBuilder.QueueBufferUpload(
					OriginalPositionsBuffer,
					WorkItem.SourceDataPtr->GetData(),
					ActualBufferSize * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// Create output buffer (recomputed normals)
				RecomputedNormalsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					TEXT("FleshRing_RecomputedNormals")
				);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RecomputedNormalsBuffer, PF_R32_FLOAT), 0);

				// Create unified affected index buffer
				FRDGBufferRef UnionAffectedIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumUnionAffected),
					TEXT("FleshRing_UnionNormalAffectedIndices")
				);
				GraphBuilder.QueueBufferUpload(
					UnionAffectedIndicesBuffer,
					UnionIndices.GetData(),
					NumUnionAffected * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Create unified adjacency offset buffer
				FRDGBufferRef UnionAdjacencyOffsetsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), UnionAdjacencyOffsets.Num()),
					TEXT("FleshRing_UnionAdjacencyOffsets")
				);
				GraphBuilder.QueueBufferUpload(
					UnionAdjacencyOffsetsBuffer,
					UnionAdjacencyOffsets.GetData(),
					UnionAdjacencyOffsets.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Create unified adjacency triangle buffer
				FRDGBufferRef UnionAdjacencyTrianglesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), UnionAdjacencyTriangles.Num()),
					TEXT("FleshRing_UnionAdjacencyTriangles")
				);
				GraphBuilder.QueueBufferUpload(
					UnionAdjacencyTrianglesBuffer,
					UnionAdjacencyTriangles.GetData(),
					UnionAdjacencyTriangles.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// UV Sync: Position synchronization before Normal Recompute
				if (WorkItem.bUnionHasUVDuplicates && WorkItem.UnionRepresentativeIndicesPtr.IsValid() &&
					WorkItem.UnionRepresentativeIndicesPtr->Num() == static_cast<int32>(NumUnionAffected))
				{
					FRDGBufferRef UVSyncRepIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumUnionAffected),
						TEXT("FleshRing_UnionUVSyncRepIndices")
					);
					GraphBuilder.QueueBufferUpload(
						UVSyncRepIndicesBuffer,
						WorkItem.UnionRepresentativeIndicesPtr->GetData(),
						NumUnionAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					FUVSyncDispatchParams UVSyncParams(NumUnionAffected);
					DispatchFleshRingUVSyncCS(
						GraphBuilder,
						UVSyncParams,
						TightenedBindPoseBuffer,
						UnionAffectedIndicesBuffer,
						UVSyncRepIndicesBuffer
					);
				}

				// NormalRecomputeCS dispatch params
				FNormalRecomputeDispatchParams NormalParams(NumUnionAffected, ActualNumVertices, WorkItem.NormalRecomputeMode);
				NormalParams.FalloffType = WorkItem.NormalBlendFalloffType;

				// Hop-based blending (if available)
				FRDGBufferRef HopDistancesBuffer = nullptr;
				if (WorkItem.UnionHopDistancesPtr.IsValid() &&
					WorkItem.UnionHopDistancesPtr->Num() == static_cast<int32>(NumUnionAffected) &&
					WorkItem.UnionMaxHops > 0)
				{
					HopDistancesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), NumUnionAffected),
						TEXT("FleshRing_UnionHopDistances")
					);
					GraphBuilder.QueueBufferUpload(
						HopDistancesBuffer,
						WorkItem.UnionHopDistancesPtr->GetData(),
						NumUnionAffected * sizeof(int32),
						ERDGInitialDataFlags::None
					);

					NormalParams.bEnableHopBlending = WorkItem.bEnableNormalHopBlending;
					NormalParams.MaxHops = WorkItem.UnionMaxHops;
				}

				// Displacement-based blending
				NormalParams.bEnableDisplacementBlending = WorkItem.bEnableDisplacementBlending;
				NormalParams.MaxDisplacement = WorkItem.MaxDisplacementForBlend;

				// UV Seam Welding: RepresentativeIndices buffer
				FRDGBufferRef NormalRepresentativeIndicesBuffer = nullptr;
				if (WorkItem.bUnionHasUVDuplicates && WorkItem.UnionRepresentativeIndicesPtr.IsValid() &&
					WorkItem.UnionRepresentativeIndicesPtr->Num() == static_cast<int32>(NumUnionAffected))
				{
					NormalRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumUnionAffected),
						TEXT("FleshRing_UnionNormalRepIndices")
					);
					GraphBuilder.QueueBufferUpload(
						NormalRepresentativeIndicesBuffer,
						WorkItem.UnionRepresentativeIndicesPtr->GetData(),
						NumUnionAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
					NormalParams.bEnableUVSeamWelding = true;
				}

				DispatchFleshRingNormalRecomputeCS(
					GraphBuilder,
					NormalParams,
					TightenedBindPoseBuffer,
					OriginalPositionsBuffer,
					UnionAffectedIndicesBuffer,
					UnionAdjacencyOffsetsBuffer,
					UnionAdjacencyTrianglesBuffer,
					MeshIndexBuffer,
					SourceTangentsSRV,
					RecomputedNormalsBuffer,
					HopDistancesBuffer,
					NormalRepresentativeIndicesBuffer
				);

				UE_LOG(LogFleshRingWorker, Verbose,
					TEXT("[NormalRecompute] Unified dispatch: %d vertices"), NumUnionAffected);
			}
		}

		// ===== TangentRecomputeCS Dispatch (Unified - after NormalRecomputeCS) =====
		// Tangent recomputation: Gram-Schmidt orthonormalization (ONCE with unified data)
		if (WorkItem.bEnableTangentRecompute && RecomputedNormalsBuffer &&
			WorkItem.UnionAffectedIndicesPtr.IsValid() && WorkItem.UnionAffectedIndicesPtr->Num() > 0)
		{
			FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

			if (SourceTangentsSRV)
			{
				const TArray<uint32>& UnionIndices = *WorkItem.UnionAffectedIndicesPtr;
				const uint32 NumUnionAffected = UnionIndices.Num();

				// Create tangent output buffer (8 floats per vertex: TangentX.xyzw + TangentZ.xyzw)
				const uint32 TangentBufferSize = ActualNumVertices * 8;
				RecomputedTangentsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), TangentBufferSize),
					TEXT("FleshRing_RecomputedTangents")
				);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RecomputedTangentsBuffer, PF_R32_FLOAT), 0);

				// Create unified affected index buffer for tangent recompute
				FRDGBufferRef UnionTangentAffectedIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumUnionAffected),
					TEXT("FleshRing_UnionTangentAffectedIndices")
				);
				GraphBuilder.QueueBufferUpload(
					UnionTangentAffectedIndicesBuffer,
					UnionIndices.GetData(),
					NumUnionAffected * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// TangentRecomputeCS dispatch (Gram-Schmidt) - ONCE
				FTangentRecomputeDispatchParams TangentParams(NumUnionAffected, ActualNumVertices);

				DispatchFleshRingTangentRecomputeCS(
					GraphBuilder,
					TangentParams,
					RecomputedNormalsBuffer,
					SourceTangentsSRV,
					UnionTangentAffectedIndicesBuffer,
					RecomputedTangentsBuffer
				);

				UE_LOG(LogFleshRingWorker, Verbose,
					TEXT("[TangentRecompute] Unified dispatch: %d vertices"), NumUnionAffected);
			}
		}

		// ===== Debug Point Output Pass (based on final deformed positions after all CS complete) =====
		// Outputting from TightnessCS, BulgeCS would give intermediate positions,
		// So unified output here after all deformation passes (including smoothing) complete
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			// Tightness debug point output (final positions)
			// DebugInfluencesBuffer required: Uses Influence values computed on GPU
			if (WorkItem.bOutputDebugPoints && DebugPointBuffer && DebugInfluencesBuffer)
			{
				// DebugPointBuffer and DebugInfluencesBuffer have the same offset structure
				// (Both stored consecutively per Ring in NumAffectedVertices units)
				uint32 DebugCumulativeOffset = 0;

				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];
					if (DispatchData.Params.NumAffectedVertices == 0) continue;

					// Create index buffer
					FRDGBufferRef DebugIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.Indices.Num()),
						*FString::Printf(TEXT("FleshRing_DebugTightnessIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						DebugIndicesBuffer,
						DispatchData.Indices.GetData(),
						DispatchData.Indices.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// Debug point output pass dispatch
					// Use DebugInfluencesBuffer computed on GPU (instead of CPU Influences)
					FDebugPointOutputDispatchParams DebugParams;
					DebugParams.NumVertices = DispatchData.Params.NumAffectedVertices;
					DebugParams.NumTotalVertices = ActualNumVertices;
					DebugParams.RingIndex = DispatchData.OriginalRingIndex;
					DebugParams.BaseOffset = DebugCumulativeOffset;
					DebugParams.InfluenceBaseOffset = DebugCumulativeOffset;  // Use same offset
					DebugParams.LocalToWorld = WorkItem.LocalToWorldMatrix;

					DispatchFleshRingDebugPointOutputCS(
						GraphBuilder,
						DebugParams,
						TightenedBindPoseBuffer,  // Final deformed positions
						DebugIndicesBuffer,
						DebugInfluencesBuffer,    // Influence computed on GPU
						DebugPointBuffer
					);

					DebugCumulativeOffset += DebugParams.NumVertices;
				}
			}

			// Bulge debug point output (final positions)
			if (WorkItem.bOutputDebugBulgePoints && DebugBulgePointBuffer)
			{
				uint32 DebugBulgePointCumulativeOffset = 0;
				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];
					if (!DispatchData.bEnableBulge || DispatchData.BulgeIndices.Num() == 0) continue;

					const uint32 NumBulgeVertices = DispatchData.BulgeIndices.Num();

					// Create index buffer
					FRDGBufferRef DebugBulgeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumBulgeVertices),
						*FString::Printf(TEXT("FleshRing_DebugBulgeIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						DebugBulgeIndicesBuffer,
						DispatchData.BulgeIndices.GetData(),
						NumBulgeVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// Create Influence buffer
					FRDGBufferRef DebugBulgeInfluenceBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumBulgeVertices),
						*FString::Printf(TEXT("FleshRing_DebugBulgeInfluences_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						DebugBulgeInfluenceBuffer,
						DispatchData.BulgeInfluences.GetData(),
						NumBulgeVertices * sizeof(float),
						ERDGInitialDataFlags::None
					);

					// Debug point output pass dispatch
					// Bulge Influence is computed on CPU and passed as-is
					FDebugPointOutputDispatchParams DebugParams;
					DebugParams.NumVertices = NumBulgeVertices;
					DebugParams.NumTotalVertices = ActualNumVertices;
					DebugParams.RingIndex = DispatchData.OriginalRingIndex;
					DebugParams.BaseOffset = DebugBulgePointCumulativeOffset;
					DebugParams.InfluenceBaseOffset = 0;  // CPU upload buffer is separated per Ring
					DebugParams.LocalToWorld = WorkItem.LocalToWorldMatrix;

					DispatchFleshRingDebugPointOutputCS(
						GraphBuilder,
						DebugParams,
						TightenedBindPoseBuffer,  // Final deformed positions
						DebugBulgeIndicesBuffer,
						DebugBulgeInfluenceBuffer,
						DebugBulgePointBuffer
					);

					DebugBulgePointCumulativeOffset += NumBulgeVertices;
				}
			}
		}

		// Convert to persistent buffer and cache
		if (WorkItem.CachedBufferSharedPtr.IsValid())
		{
			*WorkItem.CachedBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(TightenedBindPoseBuffer);
		}

		// Cache recomputed normals buffer (used by SkinningCS)
		if (WorkItem.CachedNormalsBufferSharedPtr.IsValid())
		{
			if (RecomputedNormalsBuffer)
			{
				*WorkItem.CachedNormalsBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(RecomputedNormalsBuffer);
			}
			else if (WorkItem.CachedNormalsBufferSharedPtr->IsValid())
			{
				// Clear existing cache if bEnableNormalRecompute is false
				WorkItem.CachedNormalsBufferSharedPtr->SafeRelease();
			}
		}

		// Cache recomputed tangents buffer (Gram-Schmidt orthonormalization result)
		if (WorkItem.CachedTangentsBufferSharedPtr.IsValid())
		{
			if (RecomputedTangentsBuffer)
			{
				*WorkItem.CachedTangentsBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(RecomputedTangentsBuffer);
			}
			else if (WorkItem.CachedTangentsBufferSharedPtr->IsValid())
			{
				// Clear existing cache if bEnableTangentRecompute is false
				WorkItem.CachedTangentsBufferSharedPtr->SafeRelease();
			}
		}

		// Cache debug Influence buffer (for GPU value visualization in DrawDebugPoint)
		if (WorkItem.CachedDebugInfluencesBufferSharedPtr.IsValid() && DebugInfluencesBuffer)
		{
			TRefCountPtr<FRDGPooledBuffer> ExternalDebugBuffer = GraphBuilder.ConvertToExternalBuffer(DebugInfluencesBuffer);
			*WorkItem.CachedDebugInfluencesBufferSharedPtr = ExternalDebugBuffer;

			// ===== Schedule GPU Readback =====
			// Convert to external buffer then async Readback via FRHIGPUBufferReadback
			if (WorkItem.DebugInfluenceReadbackResultPtr.IsValid() &&
				WorkItem.bDebugInfluenceReadbackComplete.IsValid() &&
				WorkItem.DebugInfluenceCount > 0 &&
				ExternalDebugBuffer.IsValid())
			{
				// Initialize completion flag before starting Readback
				WorkItem.bDebugInfluenceReadbackComplete->store(false);

				// Capture data for Readback completion processing
				TSharedPtr<TArray<float>> ResultPtr = WorkItem.DebugInfluenceReadbackResultPtr;
				TSharedPtr<std::atomic<bool>> CompleteFlag = WorkItem.bDebugInfluenceReadbackComplete;
				uint32 Count = WorkItem.DebugInfluenceCount;
				TRefCountPtr<FRDGPooledBuffer> CapturedBuffer = ExternalDebugBuffer;

				// Perform Readback on render thread after RDG execution
				ENQUEUE_RENDER_COMMAND(FleshRingDebugInfluenceReadback)(
					[ResultPtr, CompleteFlag, Count, CapturedBuffer](FRHICommandListImmediate& RHICmdList)
					{
						if (!CapturedBuffer.IsValid() || !CapturedBuffer->GetRHI())
						{
							UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Readback buffer is not valid"));
							return;
						}

						FRHIBuffer* SrcBuffer = CapturedBuffer->GetRHI();
						const uint32 BufferSize = Count * sizeof(float);

						// Async Readback using FRHIGPUBufferReadback
						FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("FleshRing_DebugInfluenceReadback"));
						Readback->EnqueueCopy(RHICmdList, SrcBuffer, BufferSize);

						// Wait for GPU synchronization then read data
						RHICmdList.BlockUntilGPUIdle();

						if (Readback->IsReady())
						{
							const float* SrcData = static_cast<const float*>(Readback->Lock(BufferSize));
							if (SrcData && ResultPtr.IsValid())
							{
								ResultPtr->SetNum(Count);
								FMemory::Memcpy(ResultPtr->GetData(), SrcData, BufferSize);
							}
							Readback->Unlock();

							// Set completion flag
							if (CompleteFlag.IsValid())
							{
								CompleteFlag->store(true);
							}
						}

						delete Readback;
					});
			}
		}

		// Cache debug point buffer
		if (WorkItem.CachedDebugPointBufferSharedPtr.IsValid() && DebugPointBuffer)
		{
			*WorkItem.CachedDebugPointBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(DebugPointBuffer);
		}

		// Cache Bulge debug point buffer
		if (WorkItem.CachedDebugBulgePointBufferSharedPtr.IsValid() && DebugBulgePointBuffer)
		{
			*WorkItem.CachedDebugBulgePointBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(DebugBulgePointBuffer);
		}
	}
	else
	{
		// Use cached buffer
		if (WorkItem.CachedBufferSharedPtr.IsValid() && WorkItem.CachedBufferSharedPtr->IsValid())
		{
			TightenedBindPoseBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedBufferSharedPtr);
		}
		else
		{
			UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Cached buffer is not valid"));
			ExternalAccessQueue.Submit(GraphBuilder);
			WorkItem.FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Restore cached normal buffer (only when bEnableNormalRecompute is enabled)
		if (WorkItem.bEnableNormalRecompute &&
			WorkItem.CachedNormalsBufferSharedPtr.IsValid() && WorkItem.CachedNormalsBufferSharedPtr->IsValid())
		{
			RecomputedNormalsBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedNormalsBufferSharedPtr);
		}

		// Restore cached tangent buffer (only when bEnableTangentRecompute is enabled)
		if (WorkItem.bEnableTangentRecompute &&
			WorkItem.CachedTangentsBufferSharedPtr.IsValid() && WorkItem.CachedTangentsBufferSharedPtr->IsValid())
		{
			RecomputedTangentsBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedTangentsBufferSharedPtr);
		}

		// Restore DebugPointBuffer in caching mode
		if (WorkItem.CachedDebugPointBufferSharedPtr.IsValid() && WorkItem.CachedDebugPointBufferSharedPtr->IsValid())
		{
			DebugPointBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedDebugPointBufferSharedPtr);
		}

		// Restore DebugBulgePointBuffer in caching mode
		if (WorkItem.CachedDebugBulgePointBufferSharedPtr.IsValid() && WorkItem.CachedDebugBulgePointBufferSharedPtr->IsValid())
		{
			DebugBulgePointBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedDebugBulgePointBufferSharedPtr);
		}
	}

	// Apply skinning
	const FSkinWeightVertexBuffer* WeightBuffer = LODData.GetSkinWeightVertexBuffer();
	FRHIShaderResourceView* InputWeightStreamSRV = WeightBuffer ?
		WeightBuffer->GetDataVertexBuffer()->GetSRV() : nullptr;

	FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

	if (!InputWeightStreamSRV)
	{
		UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: No weight stream"));
		AddCopyBufferPass(GraphBuilder, OutputPositionBuffer, TightenedBindPoseBuffer);
	}
	else
	{
		// Allocate Tangent output buffer
		FRDGBuffer* OutputTangentBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryTangentBuffer(
			GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingTangentOutput"));

		const int32 NumSections = LODData.RenderSections.Num();

		for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
		{
			const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];

			FRHIShaderResourceView* BoneMatricesSRV = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(
				MeshObject, LODIndex, SectionIndex, false);
			if (!BoneMatricesSRV) continue;

			FSkinningDispatchParams SkinParams;
			SkinParams.BaseVertexIndex = Section.BaseVertexIndex;
			SkinParams.NumVertices = Section.NumVertices;
			SkinParams.InputWeightStride = WeightBuffer->GetConstantInfluencesVertexStride();
			SkinParams.InputWeightIndexSize = WeightBuffer->GetBoneIndexByteSize() |
				(WeightBuffer->GetBoneWeightByteSize() << 8);
			SkinParams.NumBoneInfluences = WeightBuffer->GetMaxBoneInfluences();
			SkinParams.bPassthroughSkinning = true; // Editor T-pose: skip bone skinning to avoid FP drift

			DispatchFleshRingSkinningCS(GraphBuilder, SkinParams, TightenedBindPoseBuffer,
				SourceTangentsSRV, OutputPositionBuffer, nullptr,
				OutputTangentBuffer, BoneMatricesSRV, nullptr, InputWeightStreamSRV,
				RecomputedNormalsBuffer, RecomputedTangentsBuffer);
		}
	}

	// Update VertexFactory buffer
	FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(
		GraphBuilder, MeshObject, LODIndex, WorkItem.bInvalidatePreviousPosition);

	ExternalAccessQueue.Submit(GraphBuilder);
}

// ============================================================================
// FFleshRingComputeSystem Implementation
// ============================================================================

FFleshRingComputeSystem& FFleshRingComputeSystem::Get()
{
	if (!Instance)
	{
		Instance = new FFleshRingComputeSystem();
	}
	return *Instance;
}

void FFleshRingComputeSystem::CreateWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& OutWorkers)
{
	FScopeLock Lock(&WorkersLock);

	FFleshRingComputeWorker* Worker = new FFleshRingComputeWorker(InScene);
	SceneWorkers.Add(InScene, Worker);
	OutWorkers.Add(Worker);
}

void FFleshRingComputeSystem::DestroyWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& InOutWorkers)
{
	FScopeLock Lock(&WorkersLock);

	FFleshRingComputeWorker** WorkerPtr = SceneWorkers.Find(InScene);
	if (WorkerPtr)
	{
		FFleshRingComputeWorker* Worker = *WorkerPtr;
		InOutWorkers.Remove(Worker);
		delete Worker;
		SceneWorkers.Remove(InScene);
	}
}

FFleshRingComputeWorker* FFleshRingComputeSystem::GetWorker(FSceneInterface const* InScene) const
{
	FScopeLock Lock(&WorkersLock);

	FFleshRingComputeWorker* const* WorkerPtr = SceneWorkers.Find(InScene);
	return WorkerPtr ? *WorkerPtr : nullptr;
}

void FFleshRingComputeSystem::Register()
{
	if (!bIsRegistered)
	{
		ComputeSystemInterface::RegisterSystem(&Get());
		bIsRegistered = true;
	}
}

void FFleshRingComputeSystem::Unregister()
{
	if (bIsRegistered)
	{
		ComputeSystemInterface::UnregisterSystem(&Get());
		bIsRegistered = false;

		if (Instance)
		{
			delete Instance;
			Instance = nullptr;
		}
	}
}
