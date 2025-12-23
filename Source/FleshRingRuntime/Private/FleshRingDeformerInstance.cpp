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

		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGExternalAccessQueue ExternalAccessQueue;

		FRDGBuffer* PositionBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(
			GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingPosition"));

		if (!PositionBuffer)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Get source position data
		FSkeletalMeshLODRenderData const* LodRenderData = RenderData.GetPendingFirstLOD(LODIndex);
		if (!LodRenderData)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		const uint32 NumVertices = static_cast<uint32>(LodRenderData->GetNumVertices());
		FRHIShaderResourceView* SourcePositionSRV = LodRenderData->StaticVertexBuffers.PositionVertexBuffer.GetSRV();

		if (!SourcePositionSRV || NumVertices == 0)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Create UAV for output
		FRDGBufferUAVRef PositionUAV = GraphBuilder.CreateUAV(PositionBuffer, PF_R32_FLOAT);

		// Setup shader parameters
		FFleshRingWaveCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFleshRingWaveCS::FParameters>();
		PassParameters->SourcePositions = SourcePositionSRV;
		PassParameters->OutputPositions = PositionUAV;
		PassParameters->NumVertices = NumVertices;
		PassParameters->WaveAmplitude = WaveAmplitude;
		PassParameters->WaveFrequency = WaveFrequency;
		PassParameters->Time = Time * WaveSpeed;
		PassParameters->Velocity = VelocityForShader;
		PassParameters->InertiaStrength = InertiaStrength;

		// Get the shader
		TShaderMapRef<FFleshRingWaveCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		// Check if shader is valid
		if (!ComputeShader.IsValid())
		{
			UE_LOG(LogFleshRing, Warning, TEXT("FleshRingWaveCS shader is not valid!"));
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Add compute pass
		const uint32 ThreadGroupSize = 64;
		const uint32 NumGroups = FMath::DivideAndRoundUp(NumVertices, ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("FleshRingWaveDeform"),
			ComputeShader,
			PassParameters,
			FIntVector(static_cast<int32>(NumGroups), 1, 1)
		);

		// Update vertex factory to use the deformed buffer (must be before Execute, using GraphBuilder version)
		// Safe to call here because we verified bHasBeenUpdatedAtLeastOnce on game thread
		FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(GraphBuilder, MeshObject, LODIndex, bInvalidatePreviousPosition);

		// Submit external access queue
		ExternalAccessQueue.Submit(GraphBuilder);

		// Execute the graph
		GraphBuilder.Execute();
	});
}

EMeshDeformerOutputBuffer UFleshRingDeformerInstance::GetOutputBuffers() const
{
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition;
}
