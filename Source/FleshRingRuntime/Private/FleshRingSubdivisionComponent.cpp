// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSubdivisionComponent.cpp
// FleshRing Subdivision Component Implementation

#include "FleshRingSubdivisionComponent.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "FleshRingSubdivisionProcessor.h"
#include "FleshRingSubdivisionShader.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "DrawDebugHelpers.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingSubdivision, Log, All);

UFleshRingSubdivisionComponent::UFleshRingSubdivisionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	bTickInEditor = true;
}

void UFleshRingSubdivisionComponent::BeginPlay()
{
	Super::BeginPlay();
	Initialize();
}

void UFleshRingSubdivisionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Cleanup();
	Super::EndPlay(EndPlayReason);
}

void UFleshRingSubdivisionComponent::OnRegister()
{
	Super::OnRegister();

	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		Initialize();
	}
}

void UFleshRingSubdivisionComponent::OnUnregister()
{
	Cleanup();
	Super::OnUnregister();
}

#if WITH_EDITOR
void UFleshRingSubdivisionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Recalculation needed when settings change
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingSubdivisionComponent, MaxSubdivisionLevel) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingSubdivisionComponent, MinEdgeLength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingSubdivisionComponent, SubdivisionMode))
	{
		InvalidateCache();
	}
}
#endif

void UFleshRingSubdivisionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnableSubdivision || !bIsInitialized)
	{
		return;
	}

	// Update distance scale
	if (bEnableDistanceFalloff)
	{
		UpdateDistanceScale();
	}
	else
	{
		CurrentDistanceScale = 1.0f;
	}

	// Execute subdivision if needed
	if (CurrentDistanceScale > 0.0f && bNeedsRecompute)
	{
		ComputeSubdivision();
		bNeedsRecompute = false;
	}

#if WITH_EDITORONLY_DATA
	// Debug: Visualize subdivided vertices
	if (bShowSubdividedVertices && Processor.IsValid() && Processor->IsCacheValid())
	{
		DrawSubdividedVerticesDebug();
	}

	// Debug: Visualize subdivided wireframe
	if (bShowSubdividedWireframe && Processor.IsValid() && Processor->IsCacheValid())
	{
		DrawSubdividedWireframeDebug();
	}
#endif
}

void UFleshRingSubdivisionComponent::ForceRecompute()
{
	// Wait for GPU work completion and release resources (prevent memory leak)
	FlushRenderingCommands();

	if (Processor.IsValid())
	{
		Processor->InvalidateCache();
	}
	ResultCache.Reset();
	bNeedsRecompute = true;
}

void UFleshRingSubdivisionComponent::InvalidateCache()
{
	// Wait for GPU work completion and release resources (prevent memory leak)
	FlushRenderingCommands();

	if (Processor.IsValid())
	{
		Processor->InvalidateCache();
	}
	ResultCache.Reset();
	bNeedsRecompute = true;
}

int32 UFleshRingSubdivisionComponent::GetOriginalVertexCount() const
{
	if (Processor.IsValid() && Processor->IsCacheValid())
	{
		return Processor->GetCachedResult().OriginalVertexCount;
	}
	return 0;
}

int32 UFleshRingSubdivisionComponent::GetSubdividedVertexCount() const
{
	if (Processor.IsValid() && Processor->IsCacheValid())
	{
		return Processor->GetCachedResult().SubdividedVertexCount;
	}
	return 0;
}

int32 UFleshRingSubdivisionComponent::GetSubdividedTriangleCount() const
{
	if (Processor.IsValid() && Processor->IsCacheValid())
	{
		return Processor->GetCachedResult().SubdividedTriangleCount;
	}
	return 0;
}

void UFleshRingSubdivisionComponent::FindDependencies()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Find FleshRingComponent
	FleshRingComp = Owner->FindComponentByClass<UFleshRingComponent>();

	if (!FleshRingComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: FleshRingComponent not found on owner '%s'"),
			*Owner->GetName());
	}

	// Find SkeletalMeshComponent
	if (FleshRingComp.IsValid())
	{
		TargetMeshComp = FleshRingComp->GetResolvedTargetMesh();
	}

	if (!TargetMeshComp.IsValid())
	{
		TargetMeshComp = Owner->FindComponentByClass<USkeletalMeshComponent>();
	}

	if (!TargetMeshComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: SkeletalMeshComponent not found"));
	}
}

void UFleshRingSubdivisionComponent::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	FindDependencies();

	if (!TargetMeshComp.IsValid())
	{
		return;
	}

	// Create Processor
	Processor = MakeUnique<FFleshRingSubdivisionProcessor>();

	// Extract source data from SkeletalMesh
	USkeletalMesh* SkelMesh = TargetMeshComp->GetSkeletalMeshAsset();
	if (SkelMesh && Processor->SetSourceMeshFromSkeletalMesh(SkelMesh, 0))
	{
		bIsInitialized = true;
		bNeedsRecompute = true;

		UE_LOG(LogFleshRingSubdivision, Log,
			TEXT("FleshRingSubdivisionComponent initialized for '%s'"),
			*TargetMeshComp->GetName());
	}
	else
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: Failed to extract mesh data from '%s'"),
			SkelMesh ? *SkelMesh->GetName() : TEXT("null"));
	}
}

void UFleshRingSubdivisionComponent::Cleanup()
{
	// Wait for GPU work completion and release resources (prevent memory leak)
	FlushRenderingCommands();

	ResultCache.Reset();
	Processor.Reset();
	FleshRingComp.Reset();
	TargetMeshComp.Reset();
	bIsInitialized = false;
}

void UFleshRingSubdivisionComponent::UpdateDistanceScale()
{
	if (!TargetMeshComp.IsValid())
	{
		CurrentDistanceScale = 1.0f;
		return;
	}

	FVector CameraLocation = FVector::ZeroVector;

	UWorld* World = GetWorld();
	if (World)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		if (PC)
		{
			FVector CameraLoc;
			FRotator CameraRot;
			PC->GetPlayerViewPoint(CameraLoc, CameraRot);
			CameraLocation = CameraLoc;
		}
	}

	FVector MeshLocation = TargetMeshComp->GetComponentLocation();
	float Distance = FVector::Dist(MeshLocation, CameraLocation);

	if (Distance >= SubdivisionFadeDistance)
	{
		CurrentDistanceScale = 0.0f;
	}
	else if (Distance <= SubdivisionFullDistance)
	{
		CurrentDistanceScale = 1.0f;
	}
	else
	{
		float T = (Distance - SubdivisionFullDistance) / (SubdivisionFadeDistance - SubdivisionFullDistance);
		CurrentDistanceScale = 1.0f - FMath::Clamp(T, 0.0f, 1.0f);
	}
}

void UFleshRingSubdivisionComponent::ComputeSubdivision()
{
	if (!Processor.IsValid() || !FleshRingComp.IsValid())
	{
		return;
	}

	UFleshRingAsset* Asset = FleshRingComp->FleshRingAsset;
	if (!Asset || Asset->Rings.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: No rings in FleshRingAsset"));
		return;
	}

	// Use the first Ring (TODO: Support multiple Rings)
	const FFleshRingSettings& Ring = Asset->Rings[0];

	// Set Ring parameters
	FSubdivisionRingParams RingParams;

	// Determine SDF mode or VirtualRing mode based on InfluenceMode
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto)
	{
		// Auto mode: Use bounds information from SDF cache
		const FRingSDFCache* SDFCache = FleshRingComp->GetRingSDFCache(0);
		if (SDFCache && SDFCache->IsValid())
		{
			RingParams.bUseSDFBounds = true;
			RingParams.SDFBoundsMin = FVector(SDFCache->BoundsMin);
			RingParams.SDFBoundsMax = FVector(SDFCache->BoundsMax);
			RingParams.SDFLocalToComponent = SDFCache->LocalToComponent;

			UE_LOG(LogFleshRingSubdivision, Log,
				TEXT("Using SDF mode - Bounds: [%s] to [%s]"),
				*RingParams.SDFBoundsMin.ToString(),
				*RingParams.SDFBoundsMax.ToString());
		}
		else
		{
			// Fall back to VirtualRing mode if SDF cache is not available
			UE_LOG(LogFleshRingSubdivision, Warning,
				TEXT("SDF cache not available, falling back to VirtualRing mode"));
			RingParams.bUseSDFBounds = false;
			RingParams.Center = Ring.RingOffset;
			RingParams.Axis = FVector::UpVector;
			RingParams.Radius = Ring.RingRadius;
			RingParams.Width = Ring.RingHeight;
		}
	}
	else
	{
		// VirtualRing mode: existing geometric approach
		RingParams.bUseSDFBounds = false;
		RingParams.Center = Ring.RingOffset;
		RingParams.Axis = FVector::UpVector; // TODO: Calculate from bone direction
		RingParams.Radius = Ring.RingRadius;
		RingParams.Width = Ring.RingHeight;
	}

	Processor->SetRingParams(RingParams);

	// Configure Processor settings
	FSubdivisionProcessorSettings Settings;
	Settings.MaxSubdivisionLevel = MaxSubdivisionLevel;
	Settings.MinEdgeLength = MinEdgeLength;

	switch (SubdivisionMode)
	{
	case EFleshRingSubdivisionMode::BindPoseFixed:
		Settings.Mode = FSubdivisionProcessorSettings::EMode::BindPoseFixed;
		break;
	case EFleshRingSubdivisionMode::DynamicAsync:
		Settings.Mode = FSubdivisionProcessorSettings::EMode::DynamicAsync;
		break;
	case EFleshRingSubdivisionMode::PreSubdivideRegion:
		Settings.Mode = FSubdivisionProcessorSettings::EMode::PreSubdivideRegion;
		Settings.PreSubdivideMargin = PreSubdivideMargin;
		break;
	}

	Processor->SetSettings(Settings);

	// Execute CPU Subdivision
	FSubdivisionTopologyResult TopologyResult;
	if (Processor->Process(TopologyResult))
	{
		// Calculate statistics
		const int32 AddedVertices = TopologyResult.SubdividedVertexCount - TopologyResult.OriginalVertexCount;
		const int32 AddedTriangles = TopologyResult.SubdividedTriangleCount - TopologyResult.OriginalTriangleCount;
		const bool bWasSubdivided = (AddedVertices > 0 || AddedTriangles > 0);
		const FString ModeStr = RingParams.bUseSDFBounds ? TEXT("SDF") : TEXT("VirtualRing");

		// Always output log (to verify whether subdivision occurred)
		if (bWasSubdivided)
		{
			UE_LOG(LogFleshRingSubdivision, Log,
				TEXT("[%s] Subdivision SUCCESS - Mode: %s | Vertices: %d -> %d (+%d) | Triangles: %d -> %d (+%d)"),
				*GetOwner()->GetName(),
				*ModeStr,
				TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount, AddedVertices,
				TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount, AddedTriangles);
		}
		else
		{
			UE_LOG(LogFleshRingSubdivision, Warning,
				TEXT("[%s] Subdivision NO CHANGE - Mode: %s | Vertices: %d | Triangles: %d (no triangles in affected region?)"),
				*GetOwner()->GetName(),
				*ModeStr,
				TopologyResult.OriginalVertexCount,
				TopologyResult.OriginalTriangleCount);
		}

#if WITH_EDITORONLY_DATA
		// On-screen debug message (editor only)
		if (bLogSubdivisionStats && GEngine)
		{
			const FColor MsgColor = bWasSubdivided ? FColor::Green : FColor::Yellow;
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, MsgColor,
				FString::Printf(TEXT("Subdivision [%s]: V %d->%d (+%d), T %d->%d (+%d)"),
					*ModeStr,
					TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount, AddedVertices,
					TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount, AddedTriangles));
		}
#endif

		// Execute GPU interpolation
		ExecuteGPUInterpolation();
	}
	else
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("[%s] Subdivision FAILED - CPU subdivision failed"),
			*GetOwner()->GetName());
	}
}

void UFleshRingSubdivisionComponent::ExecuteGPUInterpolation()
{
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		return;
	}

	const FSubdivisionTopologyResult& TopologyResult = Processor->GetCachedResult();

	// Skip if no subdivision occurred
	if (TopologyResult.SubdividedVertexCount <= TopologyResult.OriginalVertexCount)
	{
		UE_LOG(LogFleshRingSubdivision, Log, TEXT("No subdivision occurred, skipping GPU interpolation"));
		return;
	}

	// Access SkeletalMesh LOD data
	if (!TargetMeshComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("ExecuteGPUInterpolation: TargetMeshComp is invalid"));
		return;
	}

	USkeletalMesh* SkelMesh = TargetMeshComp->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("ExecuteGPUInterpolation: No SkeletalMesh asset"));
		return;
	}

	FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("ExecuteGPUInterpolation: No render data available"));
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0]; // Use LOD 0
	const uint32 SourceVertexCount = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// Copy source mesh data (Processor cannot be accessed from render thread)
	TArray<FVector> SourcePositions = Processor->GetSourcePositions();
	TArray<FVector2D> SourceUVs = Processor->GetSourceUVs();

	// Verify that Position count matches SkeletalMesh vertex count
	if (SourcePositions.Num() != static_cast<int32>(SourceVertexCount))
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("ExecuteGPUInterpolation: Vertex count mismatch (Processor=%d, Mesh=%d) - using defaults"),
			SourcePositions.Num(), SourceVertexCount);
		// Fall back to using defaults if they don't match
		goto FallbackToDefaults;
	}

	{
		// ===== Extract Normals (TangentZ) =====
		TArray<FVector> SourceNormals;
		SourceNormals.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceNormals.Num(); ++i)
		{
			FVector4f TangentZ = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i);
			SourceNormals[i] = FVector(TangentZ.X, TangentZ.Y, TangentZ.Z);
		}

		// ===== Extract Tangents (TangentX, w = binormal sign) =====
		TArray<FVector4> SourceTangents;
		SourceTangents.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceTangents.Num(); ++i)
		{
			FVector4f TangentX = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
			SourceTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);
		}

		// ===== Extract Bone Weights/Indices =====
		const uint32 NumBoneInfluences = 4;
		TArray<float> SourceBoneWeights;
		TArray<uint32> SourceBoneIndices;
		SourceBoneWeights.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
		SourceBoneIndices.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);

		const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();
		if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
		{
			// Per-vertex section index mapping (for BoneMap conversion)
			TArray<int32> VertexToSectionIndex;
			VertexToSectionIndex.SetNumZeroed(SourcePositions.Num());
			for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
			{
				const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
				for (uint32 VertexIdx = Section.BaseVertexIndex;
					 VertexIdx < Section.BaseVertexIndex + Section.NumVertices && VertexIdx < static_cast<uint32>(SourcePositions.Num());
					 ++VertexIdx)
				{
					VertexToSectionIndex[VertexIdx] = SectionIdx;
				}
			}

			for (int32 i = 0; i < SourcePositions.Num(); ++i)
			{
				// Get the section BoneMap for this vertex
				const TArray<FBoneIndexType>* BoneMap = nullptr;
				int32 SectionIdx = VertexToSectionIndex[i];
				if (SectionIdx >= 0 && SectionIdx < LODData.RenderSections.Num())
				{
					BoneMap = &LODData.RenderSections[SectionIdx].BoneMap;
				}

				for (uint32 j = 0; j < NumBoneInfluences; ++j)
				{
					uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(i, j);
					uint8 Weight = SkinWeightBuffer->GetBoneWeight(i, j);

					// Convert local to global bone index using BoneMap
					uint16 GlobalBoneIdx = LocalBoneIdx;
					if (BoneMap && LocalBoneIdx < BoneMap->Num())
					{
						GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
					}

					// Convert weight from 0-255 to 0.0-1.0
					SourceBoneWeights[i * NumBoneInfluences + j] = static_cast<float>(Weight) / 255.0f;
					SourceBoneIndices[i * NumBoneInfluences + j] = static_cast<uint32>(GlobalBoneIdx);
				}
			}
		}
		else
		{
			UE_LOG(LogFleshRingSubdivision, Warning,
				TEXT("ExecuteGPUInterpolation: No SkinWeightBuffer - using default bone weights"));
			for (int32 i = 0; i < SourcePositions.Num(); ++i)
			{
				SourceBoneWeights[i * NumBoneInfluences] = 1.0f; // 100% weight to the first bone
			}
		}

		UE_LOG(LogFleshRingSubdivision, Log,
			TEXT("ExecuteGPUInterpolation: Extracted real mesh data - %d vertices (normals, tangents, bone weights)"),
			SourcePositions.Num());

		// Result cache pointer (for render thread access)
		FSubdivisionResultCache* ResultCachePtr = &ResultCache;
		const uint32 NumVertices = TopologyResult.SubdividedVertexCount;
		const uint32 NumIndices = TopologyResult.Indices.Num();

		// Execute GPU work and cache results on Render Thread
		ENQUEUE_RENDER_COMMAND(FleshRingSubdivisionGPU)(
			[TopologyResult, SourcePositions, SourceNormals, SourceTangents, SourceUVs, SourceBoneWeights, SourceBoneIndices,
			 NumBoneInfluences, ResultCachePtr, NumVertices, NumIndices]
			(FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				FSubdivisionInterpolationParams Params;
				Params.NumBoneInfluences = NumBoneInfluences;

				FSubdivisionGPUBuffers Buffers;

				// Upload source mesh data
				UploadSourceMeshToGPU(
					GraphBuilder,
					SourcePositions,
					SourceNormals,
					SourceTangents,
					SourceUVs,
					SourceBoneWeights,
					SourceBoneIndices,
					NumBoneInfluences,
					Buffers);

				// Create GPU buffers from topology result
				CreateSubdivisionGPUBuffersFromTopology(GraphBuilder, TopologyResult, Params, Buffers);

				// Dispatch GPU interpolation
				DispatchFleshRingBarycentricInterpolationCS(GraphBuilder, Params, Buffers);

				// Convert results to pooled buffers for caching
				// Extract RDG buffers to external buffers (persists after GraphBuilder.Execute())
				GraphBuilder.QueueBufferExtraction(Buffers.OutputPositions, &ResultCachePtr->PositionsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputNormals, &ResultCachePtr->NormalsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputTangents, &ResultCachePtr->TangentsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputUVs, &ResultCachePtr->UVsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputIndices, &ResultCachePtr->IndicesBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneWeights, &ResultCachePtr->BoneWeightsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneIndices, &ResultCachePtr->BoneIndicesBuffer);

				GraphBuilder.Execute();

				// Update cache metadata
				ResultCachePtr->NumVertices = NumVertices;
				ResultCachePtr->NumIndices = NumIndices;
				ResultCachePtr->bCached = true;

				UE_LOG(LogFleshRingSubdivision, Log,
					TEXT("GPU interpolation complete and cached: %d vertices, %d indices"),
					NumVertices, NumIndices);
			});

		return; // Successfully processed
	}

FallbackToDefaults:
	// Fallback: Use default values (existing behavior)
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("ExecuteGPUInterpolation: Using fallback default values for normals/tangents/bone weights"));

		TArray<FVector> SourceNormals;
		SourceNormals.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceNormals.Num(); ++i)
		{
			SourceNormals[i] = FVector::UpVector;
		}

		TArray<FVector4> SourceTangents;
		SourceTangents.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceTangents.Num(); ++i)
		{
			SourceTangents[i] = FVector4(1.0f, 0.0f, 0.0f, 1.0f);
		}

		const uint32 NumBoneInfluences = 4;
		TArray<float> SourceBoneWeights;
		TArray<uint32> SourceBoneIndices;
		SourceBoneWeights.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
		SourceBoneIndices.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
		for (int32 i = 0; i < SourcePositions.Num(); ++i)
		{
			SourceBoneWeights[i * NumBoneInfluences] = 1.0f;
		}

		// Result cache pointer (for render thread access)
		FSubdivisionResultCache* ResultCachePtr = &ResultCache;
		const uint32 NumVertices = TopologyResult.SubdividedVertexCount;
		const uint32 NumIndices = TopologyResult.Indices.Num();

		// Execute GPU work and cache results on Render Thread
		ENQUEUE_RENDER_COMMAND(FleshRingSubdivisionGPUFallback)(
			[TopologyResult, SourcePositions, SourceNormals, SourceTangents, SourceUVs, SourceBoneWeights, SourceBoneIndices,
			 NumBoneInfluences, ResultCachePtr, NumVertices, NumIndices]
			(FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				FSubdivisionInterpolationParams Params;
				Params.NumBoneInfluences = NumBoneInfluences;

				FSubdivisionGPUBuffers Buffers;

				// Upload source mesh data
				UploadSourceMeshToGPU(
					GraphBuilder,
					SourcePositions,
					SourceNormals,
					SourceTangents,
					SourceUVs,
					SourceBoneWeights,
					SourceBoneIndices,
					NumBoneInfluences,
					Buffers);

				// Create GPU buffers from topology result
				CreateSubdivisionGPUBuffersFromTopology(GraphBuilder, TopologyResult, Params, Buffers);

				// Dispatch GPU interpolation
				DispatchFleshRingBarycentricInterpolationCS(GraphBuilder, Params, Buffers);

				// Convert results to pooled buffers for caching
				GraphBuilder.QueueBufferExtraction(Buffers.OutputPositions, &ResultCachePtr->PositionsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputNormals, &ResultCachePtr->NormalsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputTangents, &ResultCachePtr->TangentsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputUVs, &ResultCachePtr->UVsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputIndices, &ResultCachePtr->IndicesBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneWeights, &ResultCachePtr->BoneWeightsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneIndices, &ResultCachePtr->BoneIndicesBuffer);

				GraphBuilder.Execute();

				// Update cache metadata
				ResultCachePtr->NumVertices = NumVertices;
				ResultCachePtr->NumIndices = NumIndices;
				ResultCachePtr->bCached = true;

				UE_LOG(LogFleshRingSubdivision, Log,
					TEXT("GPU interpolation (fallback) complete and cached: %d vertices, %d indices"),
					NumVertices, NumIndices);
			});
	}
}

#if WITH_EDITOR

void UFleshRingSubdivisionComponent::BakeSubdividedMesh()
{
	// 1. Check if subdivision has been computed
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		// Compute subdivision first
		ForceRecompute();

		// Manually call Tick to complete computation
		TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);

		if (!Processor.IsValid() || !Processor->IsCacheValid())
		{
			UE_LOG(LogFleshRingSubdivision, Error, TEXT("BakeSubdividedMesh: Subdivision calculation failed"));
			return;
		}
	}

	if (!TargetMeshComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Error, TEXT("BakeSubdividedMesh: TargetMeshComponent is invalid"));
		return;
	}

	USkeletalMesh* SourceMesh = TargetMeshComp->GetSkeletalMeshAsset();
	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingSubdivision, Error, TEXT("BakeSubdividedMesh: SourceMesh is null"));
		return;
	}

	const FSubdivisionTopologyResult& Result = Processor->GetCachedResult();

	// Skip if no subdivision occurred
	if (Result.SubdividedVertexCount <= Result.OriginalVertexCount)
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("BakeSubdividedMesh: No subdivision occurred (no new vertices)"));
		return;
	}

	UE_LOG(LogFleshRingSubdivision, Log,
		TEXT("BakeSubdividedMesh started: %d -> %d vertices, %d -> %d triangles"),
		Result.OriginalVertexCount, Result.SubdividedVertexCount,
		Result.OriginalTriangleCount, Result.SubdividedTriangleCount);

	// TODO: Create new SkeletalMesh asset
	// This feature requires using complex SkeletalMesh creation APIs,
	// so it should be implemented in the FleshRingEditor module.
	// Currently only outputs placeholder logs.

	UE_LOG(LogFleshRingSubdivision, Warning,
		TEXT("BakeSubdividedMesh: SkeletalMesh creation needs to be implemented in FleshRingEditor module"));
	UE_LOG(LogFleshRingSubdivision, Log,
		TEXT("  Save path: %s"),
		*BakedMeshSavePath);
	UE_LOG(LogFleshRingSubdivision, Log,
		TEXT("  Suffix: %s"),
		*BakedMeshSuffix);

	// Editor notification
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow,
			FString::Printf(TEXT("BakeSubdividedMesh: %d -> %d vertices (SkeletalMesh creation needs to be implemented in Editor module)"),
				Result.OriginalVertexCount, Result.SubdividedVertexCount));
	}
}

#endif // WITH_EDITOR

#if WITH_EDITORONLY_DATA

void UFleshRingSubdivisionComponent::DrawSubdividedVerticesDebug()
{
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		return;
	}

	if (!TargetMeshComp.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FSubdivisionTopologyResult& Result = Processor->GetCachedResult();
	const TArray<FVector>& SourcePositions = Processor->GetSourcePositions();

	if (SourcePositions.Num() == 0)
	{
		return;
	}

	// World transform (component space -> world space)
	const FTransform& MeshTransform = TargetMeshComp->GetComponentTransform();

	// Visualize only newly added vertices (exclude original vertices)
	int32 NewVertexCount = 0;
	for (int32 i = 0; i < Result.VertexData.Num(); ++i)
	{
		const FSubdivisionVertexData& VertexData = Result.VertexData[i];

		// Skip original vertices
		if (VertexData.IsOriginalVertex())
		{
			continue;
		}

		// Validate parent vertex indices
		const uint32 P0 = VertexData.ParentV0;
		const uint32 P1 = VertexData.ParentV1;
		const uint32 P2 = VertexData.ParentV2;

		if (P0 >= (uint32)SourcePositions.Num() ||
			P1 >= (uint32)SourcePositions.Num() ||
			P2 >= (uint32)SourcePositions.Num())
		{
			continue;
		}

		// Calculate position using barycentric interpolation (component space)
		const FVector3f& Bary = VertexData.BarycentricCoords;
		FVector LocalPosition =
			SourcePositions[P0] * Bary.X +
			SourcePositions[P1] * Bary.Y +
			SourcePositions[P2] * Bary.Z;

		// Transform to world space
		FVector WorldPosition = MeshTransform.TransformPosition(LocalPosition);

		// Draw as white point
		DrawDebugPoint(
			World,
			WorldPosition,
			DebugPointSize,
			FColor::White,
			false,  // bPersistent
			-1.0f   // LifeTime (every frame)
		);

		++NewVertexCount;
	}

	// Output log only on first frame
	static bool bFirstFrame = true;
	if (bFirstFrame && NewVertexCount > 0)
	{
		UE_LOG(LogFleshRingSubdivision, Log,
			TEXT("DrawSubdividedVerticesDebug: Drawing %d new vertices (Total: %d, Original: %d)"),
			NewVertexCount,
			Result.VertexData.Num(),
			Result.OriginalVertexCount);
		bFirstFrame = false;
	}
}

void UFleshRingSubdivisionComponent::DrawSubdividedWireframeDebug()
{
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		return;
	}

	if (!TargetMeshComp.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FSubdivisionTopologyResult& Result = Processor->GetCachedResult();
	const TArray<FVector>& SourcePositions = Processor->GetSourcePositions();

	if (SourcePositions.Num() == 0 || Result.Indices.Num() < 3)
	{
		return;
	}

	// World transform (component space -> world space)
	const FTransform& MeshTransform = TargetMeshComp->GetComponentTransform();

	// Pre-calculate all vertex positions (original + new vertices)
	TArray<FVector> AllPositions;
	AllPositions.SetNum(Result.VertexData.Num());

	for (int32 i = 0; i < Result.VertexData.Num(); ++i)
	{
		const FSubdivisionVertexData& VertexData = Result.VertexData[i];

		const uint32 P0 = VertexData.ParentV0;
		const uint32 P1 = VertexData.ParentV1;
		const uint32 P2 = VertexData.ParentV2;

		// Validation check
		if (P0 >= (uint32)SourcePositions.Num() ||
			P1 >= (uint32)SourcePositions.Num() ||
			P2 >= (uint32)SourcePositions.Num())
		{
			AllPositions[i] = FVector::ZeroVector;
			continue;
		}

		// Calculate position using barycentric interpolation
		const FVector3f& Bary = VertexData.BarycentricCoords;
		FVector LocalPosition =
			SourcePositions[P0] * Bary.X +
			SourcePositions[P1] * Bary.Y +
			SourcePositions[P2] * Bary.Z;

		// Transform to world space
		AllPositions[i] = MeshTransform.TransformPosition(LocalPosition);
	}

	// Draw edges of all triangles as red lines
	const int32 NumTriangles = Result.Indices.Num() / 3;

	// Draw only new triangles (after original triangle count)
	const int32 OriginalTriCount = Result.OriginalTriangleCount;

	for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
	{
		const uint32 I0 = Result.Indices[TriIdx * 3 + 0];
		const uint32 I1 = Result.Indices[TriIdx * 3 + 1];
		const uint32 I2 = Result.Indices[TriIdx * 3 + 2];

		if (I0 >= (uint32)AllPositions.Num() ||
			I1 >= (uint32)AllPositions.Num() ||
			I2 >= (uint32)AllPositions.Num())
		{
			continue;
		}

		const FVector& V0 = AllPositions[I0];
		const FVector& V1 = AllPositions[I1];
		const FVector& V2 = AllPositions[I2];

		// New triangles are red, original triangles are green
		// Since it's difficult to distinguish original and new triangles,
		// we determine by checking if the triangle contains new vertices
		const bool bHasNewVertex =
			!Result.VertexData[I0].IsOriginalVertex() ||
			!Result.VertexData[I1].IsOriginalVertex() ||
			!Result.VertexData[I2].IsOriginalVertex();

		FColor LineColor = bHasNewVertex ? FColor::Red : FColor::Green;

		// Draw the 3 edges of the triangle
		DrawDebugLine(World, V0, V1, LineColor, false, -1.0f, 0, 1.0f);
		DrawDebugLine(World, V1, V2, LineColor, false, -1.0f, 0, 1.0f);
		DrawDebugLine(World, V2, V0, LineColor, false, -1.0f, 0, 1.0f);
	}
}
#endif
