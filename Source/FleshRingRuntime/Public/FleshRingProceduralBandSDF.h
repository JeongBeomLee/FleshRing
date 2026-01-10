// ============================================================================
// FleshRing VirtualBand Mathematical SDF Generator
// ============================================================================
// Purpose: Generate SDF using mathematical formulas for VirtualBand shapes
//          VirtualBand 형상에 대해 수학 공식으로 SDF 생성
//
// This replaces ray casting + flood fill for VirtualBand mode with:
//   1. Direct mathematical SDF computation
//   2. No mesh triangles needed (for SDF generation)
//   3. Exact results without numerical ray casting issues
//
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingTypes.h"

// ============================================================================
// FFleshRingProceduralBandSDFCS - Mathematical SDF Compute Shader
// ============================================================================

class FFleshRingProceduralBandSDFCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingProceduralBandSDFCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingProceduralBandSDFCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Output SDF texture
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, OutputSDF)

		// SDF volume parameters
		SHADER_PARAMETER(FVector3f, SDFBoundsMin)
		SHADER_PARAMETER(FVector3f, SDFBoundsMax)
		SHADER_PARAMETER(FIntVector, SDFResolution)

		// VirtualBand parameters (4 radii: Upper - MidUpper - MidLower - Lower)
		SHADER_PARAMETER(float, MidUpperRadius)
		SHADER_PARAMETER(float, MidLowerRadius)
		SHADER_PARAMETER(float, BandThickness)
		SHADER_PARAMETER(float, BandHeight)
		SHADER_PARAMETER(float, LowerRadius)
		SHADER_PARAMETER(float, LowerHeight)
		SHADER_PARAMETER(float, UpperRadius)
		SHADER_PARAMETER(float, UpperHeight)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

// ============================================================================
// FProceduralBandSDFDispatchParams - Dispatch Parameters
// ============================================================================

struct FProceduralBandSDFDispatchParams
{
	/** VirtualBand settings */
	FProceduralBandSettings BandSettings;

	/** SDF bounds in local space */
	FBox3f SDFBounds;

	/** SDF resolution (e.g., 64x64x64) */
	FIntVector Resolution;

	FProceduralBandSDFDispatchParams()
		: SDFBounds(FVector3f::ZeroVector, FVector3f::ZeroVector)
		, Resolution(64, 64, 64)
	{
	}
};

// ============================================================================
// Dispatch Function
// ============================================================================

/**
 * Dispatch mathematical SDF generation for VirtualBand
 *
 * @param GraphBuilder - RDG builder
 * @param Params - VirtualBand and SDF parameters
 * @param OutputSDFTexture - Output 3D texture for SDF (must be pre-created)
 */
void DispatchFleshRingProceduralBandSDF(
	FRDGBuilder& GraphBuilder,
	const FProceduralBandSDFDispatchParams& Params,
	FRDGTextureRef OutputSDFTexture);

/**
 * Create and dispatch mathematical SDF generation for VirtualBand
 * Creates the output texture and returns it
 *
 * @param GraphBuilder - RDG builder
 * @param Params - VirtualBand and SDF parameters
 * @return Created SDF texture
 */
FRDGTextureRef CreateAndDispatchProceduralBandSDF(
	FRDGBuilder& GraphBuilder,
	const FProceduralBandSDFDispatchParams& Params);
