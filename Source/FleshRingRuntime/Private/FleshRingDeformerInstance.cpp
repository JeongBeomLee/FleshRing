// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformerInstance.h"
#include "FleshRingDeformer.h"
#include "FleshRingWaveShader.h"
#include "Components/SkinnedMeshComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRing, Log, All);

#include UE_INLINE_GENERATED_CPP_BY_NAME(FleshRingDeformerInstance)

UFleshRingDeformerInstance::UFleshRingDeformerInstance()
{
}

void UFleshRingDeformerInstance::SetupFromDeformer(UFleshRingDeformer* InDeformer, UMeshComponent* InMeshComponent)
{
	Deformer = InDeformer;
	MeshComponent = InMeshComponent;
	Scene = InMeshComponent ? InMeshComponent->GetScene() : nullptr;
	LastLodIndex = INDEX_NONE;
}

void UFleshRingDeformerInstance::AllocateResources()
{
	// Resources are allocated on-demand in EnqueueWork
}

void UFleshRingDeformerInstance::ReleaseResources()
{
	// Nothing to release for now
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
	// This ensures render resources (including passthrough vertex factory) are initialized
	// By the time bHasBeenUpdatedAtLeastOnce is true, the previous frame's render commands
	// (including InitResources/InitVertexFactories) have already executed
	if (!MeshObject->bHasBeenUpdatedAtLeastOnce)
	{
		// Not ready yet - passthrough vertex factory's InitRHI hasn't been called
		// Skip this frame and let fallback handle it
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// Capture parameters for render thread
	const float WaveAmplitude = DeformerPtr->WaveAmplitude;
	const float WaveFrequency = DeformerPtr->WaveFrequency;
	const float WaveSpeed = DeformerPtr->WaveSpeed;
	const float InertiaStrength = DeformerPtr->InertiaStrength;

	// Get actual world time instead of accumulating with hardcoded delta
	// This matches how Optimus handles time via Scene Data Interface
	float Time = 0.0f;
	float DeltaTime = 0.016f; // Fallback
	if (UWorld* World = SkinnedMeshComp->GetWorld())
	{
		Time = World->GetTimeSeconds();
		DeltaTime = World->GetDeltaSeconds();
	}

	// Calculate velocity from component movement
	const FVector CurrentWorldLocation = SkinnedMeshComp->GetComponentLocation();
	if (bHasPreviousLocation && DeltaTime > KINDA_SMALL_NUMBER)
	{
		// Smooth velocity calculation with exponential smoothing
		FVector InstantVelocity = (CurrentWorldLocation - PreviousWorldLocation) / DeltaTime;
		const float SmoothingFactor = FMath::Clamp(DeltaTime * 10.0f, 0.0f, 1.0f); // Responsive but smooth
		CurrentVelocity = FMath::Lerp(CurrentVelocity, InstantVelocity, SmoothingFactor);
	}
	PreviousWorldLocation = CurrentWorldLocation;
	bHasPreviousLocation = true;

	// Convert to local space velocity for shader (mesh deforms in local space)
	const FVector LocalVelocity = SkinnedMeshComp->GetComponentTransform().InverseTransformVector(CurrentVelocity);
	const FVector3f VelocityForShader = FVector3f(LocalVelocity);

	const int32 LODIndex = SkinnedMeshComp->GetPredictedLODLevel();

	// Track LOD changes for invalidating previous position (like Optimus does)
	bool bInvalidatePreviousPosition = false;
	if (LODIndex != LastLodIndex)
	{
		bInvalidatePreviousPosition = true;
		LastLodIndex = LODIndex;
	}

	ENQUEUE_RENDER_COMMAND(FleshRingDeformer)(
		[MeshObject, LODIndex, WaveAmplitude, WaveFrequency, WaveSpeed, Time, VelocityForShader, InertiaStrength, bInvalidatePreviousPosition, FallbackDelegate = InDesc.FallbackDelegate]
		(FRHICommandListImmediate& RHICmdList)
	{
		// Validate LOD index before proceeding
		if (!MeshObject || LODIndex < 0)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Check if render data is valid
		FSkeletalMeshRenderData const& RenderData = MeshObject->GetSkeletalMeshRenderData();
		if (LODIndex >= RenderData.LODRenderData.Num())
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Check if LOD has render sections (vertex factories might not be ready)
		const FSkeletalMeshLODRenderData& LODData = RenderData.LODRenderData[LODIndex];
		if (LODData.RenderSections.Num() == 0)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Check if this LOD is actually streamed in and ready
		if (!LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices())
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Check if vertex factory is initialized by verifying a valid section exists
		const int32 FirstAvailableSection = FSkeletalMeshDeformerHelpers::GetIndexOfFirstAvailableSection(MeshObject, LODIndex);
		if (FirstAvailableSection == INDEX_NONE)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// ============================================
		// Gather all data BEFORE creating RDG resources
		// (Following Optimus Skeleton Data Interface approach)
		// ============================================

		// Get LOD render data
		FSkeletalMeshLODRenderData const* LodRenderData = RenderData.GetPendingFirstLOD(LODIndex);
		if (!LodRenderData)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		const uint32 NumVertices = static_cast<uint32>(LodRenderData->GetNumVertices());
		if (NumVertices == 0)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Get bind pose position buffer (LOD-wide)
		FRHIShaderResourceView* SourcePositionSRV = LodRenderData->StaticVertexBuffers.PositionVertexBuffer.GetSRV();
		if (!SourcePositionSRV)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Get skin weight buffer (LOD-wide, same as Optimus)
		FSkinWeightVertexBuffer const* WeightBuffer = LodRenderData->GetSkinWeightVertexBuffer();
		if (!WeightBuffer)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		FRHIShaderResourceView* InputWeightStreamSRV = WeightBuffer->GetDataVertexBuffer()->GetSRV();
		if (!InputWeightStreamSRV)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Get skinning parameters (exactly as Optimus does)
		const uint32 NumBoneInfluences = WeightBuffer->GetMaxBoneInfluences();
		const uint32 InputWeightStride = WeightBuffer->GetConstantInfluencesVertexStride();
		const uint32 InputWeightIndexSize = WeightBuffer->GetBoneIndexByteSize() | (WeightBuffer->GetBoneWeightByteSize() << 8);

		// Get the shader
		TShaderMapRef<FFleshRingWaveCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		if (!ComputeShader.IsValid())
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// ============================================
		// All validation passed - now create RDG resources
		// No early returns after this point!
		// ============================================

		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGExternalAccessQueue ExternalAccessQueue;

		FRDGBuffer* PositionBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(
			GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingPosition"));

		if (!PositionBuffer)
		{
			// Must submit before returning after ExternalAccessQueue is created
			ExternalAccessQueue.Submit(GraphBuilder);
			GraphBuilder.Execute();
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Create UAV for output
		FRDGBufferUAVRef PositionUAV = GraphBuilder.CreateUAV(PositionBuffer, PF_R32_FLOAT);

		// ============================================
		// Dispatch per Section (each Section has its own BoneBuffer)
		// ============================================
		const FSkeletalMeshLODRenderData& LODRenderData = RenderData.LODRenderData[LODIndex];
		const uint32 ThreadGroupSize = 64;

		for (int32 SectionIdx = 0; SectionIdx < LODRenderData.RenderSections.Num(); SectionIdx++)
		{
			const FSkelMeshRenderSection& Section = LODRenderData.RenderSections[SectionIdx];

			// Skip disabled sections
			if (Section.bDisabled)
			{
				continue;
			}

			// Get this Section's BoneBuffer
			FRHIShaderResourceView* SectionBoneMatricesSRV = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(
				MeshObject, LODIndex, SectionIdx, false /* bPreviousFrame */);

			if (!SectionBoneMatricesSRV)
			{
				continue;
			}

			// Section's vertex range
			const uint32 BaseVertexIndex = Section.BaseVertexIndex;
			const uint32 NumSectionVertices = Section.NumVertices;

			if (NumSectionVertices == 0)
			{
				continue;
			}

			// Setup shader parameters for this Section
			FFleshRingWaveCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFleshRingWaveCS::FParameters>();

			// Vertex data
			PassParameters->SourcePositions = SourcePositionSRV;
			PassParameters->OutputPositions = PositionUAV;
			PassParameters->BaseVertexIndex = BaseVertexIndex;
			PassParameters->NumVertices = NumSectionVertices;

			// Skinning data (Section-specific BoneBuffer)
			PassParameters->BoneMatrices = SectionBoneMatricesSRV;
			PassParameters->InputWeightStream = InputWeightStreamSRV;
			PassParameters->NumBoneInfluences = NumBoneInfluences;
			PassParameters->InputWeightStride = InputWeightStride;
			PassParameters->InputWeightIndexSize = InputWeightIndexSize;

			// Jelly effect parameters
			PassParameters->WaveAmplitude = WaveAmplitude;
			PassParameters->WaveFrequency = WaveFrequency;
			PassParameters->Time = Time * WaveSpeed;
			PassParameters->Velocity = VelocityForShader;
			PassParameters->InertiaStrength = InertiaStrength;

			// Dispatch for this Section
			const uint32 NumGroups = FMath::DivideAndRoundUp(NumSectionVertices, ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FleshRingWaveDeform_Section%d", SectionIdx),
				ComputeShader,
				PassParameters,
				FIntVector(static_cast<int32>(NumGroups), 1, 1)
			);
		}

		// Update vertex factory to use the deformed buffer
		FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(GraphBuilder, MeshObject, LODIndex, bInvalidatePreviousPosition);

		// Submit external access queue and execute
		ExternalAccessQueue.Submit(GraphBuilder);
		GraphBuilder.Execute();
	});
}

EMeshDeformerOutputBuffer UFleshRingDeformerInstance::GetOutputBuffers() const
{
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition;
}