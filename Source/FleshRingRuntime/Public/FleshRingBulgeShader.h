// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingDebugTypes.h"

/**
 * Bulge Compute Shader
 * Distributes accumulated volume from TightnessCS to surrounding vertices
 */
class FFleshRingBulgeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingBulgeCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingBulgeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, InputPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BulgeVertexIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, BulgeInfluences)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VolumeAccumBuffer)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

		// SDF
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SDFSampler)
		SHADER_PARAMETER(FVector3f, SDFBoundsMin)
		SHADER_PARAMETER(FVector3f, SDFBoundsMax)
		SHADER_PARAMETER(FMatrix44f, ComponentToSDFLocal)

		// Params
		SHADER_PARAMETER(uint32, NumBulgeVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)
		SHADER_PARAMETER(float, BulgeStrength)
		SHADER_PARAMETER(float, MaxBulgeDistance)
		SHADER_PARAMETER(float, FixedPointScale)
		SHADER_PARAMETER(int32, BulgeAxisDirection)
		SHADER_PARAMETER(uint32, RingIndex)           // Ring index (specifies VolumeAccumBuffer slot)
		SHADER_PARAMETER(float, BulgeRadialRatio)     // Radial vs Axial direction ratio (0.0~1.0, default 0.7)
		SHADER_PARAMETER(float, UpperBulgeStrength)   // Upper (positive axis) Bulge strength multiplier
		SHADER_PARAMETER(float, LowerBulgeStrength)   // Lower (negative axis) Bulge strength multiplier
		SHADER_PARAMETER(uint32, bUseSDFInfluence)    // 0 = VirtualRing (Component Space), 1 = SDF mode

		// Ring Center/Axis (SDF Local Space) - for SDF mode
		SHADER_PARAMETER(FVector3f, SDFLocalRingCenter)
		SHADER_PARAMETER(FVector3f, SDFLocalRingAxis)

		// Ring Center/Axis (Component Space) - for VirtualRing mode
		SHADER_PARAMETER(FVector3f, RingCenter)
		SHADER_PARAMETER(FVector3f, RingAxis)
		SHADER_PARAMETER(float, RingHeight)

		// Debug Point Output is handled in DebugPointOutputCS based on final positions
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
	}
};

struct FBulgeDispatchParams
{
	uint32 NumBulgeVertices = 0;	// Number of Bulge target vertices
	uint32 NumTotalVertices = 0;	// Total mesh vertex count (for range checking)
	float BulgeStrength = 1.0f;		// Bulge strength
	float MaxBulgeDistance = 10.0f;	// Maximum Bulge distance
	float FixedPointScale = 0.001f;	// Must match TightnessCS.
	int32 BulgeAxisDirection = 0;	// Bulge direction (-1: negative axis, +1: positive axis, 0: both directions)
	uint32 RingIndex = 0;			// Ring index (specifies VolumeAccumBuffer slot)
	float BulgeRadialRatio = 0.7f;	// Radial vs Axial direction ratio (0.0~1.0)
	float UpperBulgeStrength = 1.0f;	// Upper (positive axis) Bulge strength multiplier
	float LowerBulgeStrength = 1.0f;	// Lower (negative axis) Bulge strength multiplier
	uint32 bUseSDFInfluence = 1;	// 0 = VirtualRing (Component Space), 1 = SDF mode

	// Parameters for SDF mode
	FVector3f SDFBoundsMin = FVector3f::ZeroVector;			// in Ring Local Space
	FVector3f SDFBoundsMax = FVector3f::ZeroVector;			// in Ring Local Space
	FMatrix44f ComponentToSDFLocal = FMatrix44f::Identity;	// Component -> SDF Local
	FVector3f SDFLocalRingCenter = FVector3f::ZeroVector;
	FVector3f SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);

	// Parameters for VirtualRing mode (Component Space)
	FVector3f RingCenter = FVector3f::ZeroVector;
	FVector3f RingAxis = FVector3f(0.0f, 0.0f, 1.0f);
	float RingHeight = 2.0f;

	// Debug Point Output is handled in DebugPointOutputCS
};

void DispatchFleshRingBulgeCS(
	FRDGBuilder& GraphBuilder,
	const FBulgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef BulgeVertexIndicesBuffer,
	FRDGBufferRef BulgeInfluencesBuffer,
	FRDGBufferRef VolumeAccumBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGTextureRef SDFTexture);
