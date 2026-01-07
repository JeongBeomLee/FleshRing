// ============================================================================
// FleshRing ProceduralBand Mathematical SDF Generator - Implementation
// ============================================================================

#include "FleshRingProceduralBandSDF.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
	FFleshRingProceduralBandSDFCS,
	"/Plugin/FleshRingPlugin/FleshRingProceduralBandSDF.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingProceduralBandSDF(
	FRDGBuilder& GraphBuilder,
	const FProceduralBandSDFDispatchParams& Params,
	FRDGTextureRef OutputSDFTexture)
{
	// Validate parameters
	if (!OutputSDFTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("DispatchFleshRingProceduralBandSDF: OutputSDFTexture is null"));
		return;
	}

	if (Params.Resolution.X <= 0 || Params.Resolution.Y <= 0 || Params.Resolution.Z <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("DispatchFleshRingProceduralBandSDF: Invalid resolution"));
		return;
	}

	// Allocate shader parameters
	FFleshRingProceduralBandSDFCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingProceduralBandSDFCS::FParameters>();

	// Bind output texture
	PassParameters->OutputSDF = GraphBuilder.CreateUAV(OutputSDFTexture);

	// Set SDF volume parameters
	PassParameters->SDFBoundsMin = Params.SDFBounds.Min;
	PassParameters->SDFBoundsMax = Params.SDFBounds.Max;
	PassParameters->SDFResolution = Params.Resolution;

	// Set ProceduralBand parameters
	const auto& Settings = Params.BandSettings;
	PassParameters->BandRadius = Settings.BandRadius;
	PassParameters->BandThickness = Settings.BandThickness;
	PassParameters->BandHeight = Settings.BandHeight;
	PassParameters->LowerRadius = Settings.Lower.Radius;
	PassParameters->LowerHeight = Settings.Lower.Height;
	PassParameters->UpperRadius = Settings.Upper.Radius;
	PassParameters->UpperHeight = Settings.Upper.Height;

	// Get shader
	TShaderMapRef<FFleshRingProceduralBandSDFCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups (8x8x8 threads per group)
	const FIntVector NumGroups(
		FMath::DivideAndRoundUp(Params.Resolution.X, 8),
		FMath::DivideAndRoundUp(Params.Resolution.Y, 8),
		FMath::DivideAndRoundUp(Params.Resolution.Z, 8)
	);

	// Add compute pass
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingProceduralBandSDF"),
		ComputeShader,
		PassParameters,
		NumGroups
	);

	UE_LOG(LogTemp, Verbose, TEXT("DispatchFleshRingProceduralBandSDF: Dispatched %dx%dx%d grid (Resolution: %dx%dx%d)"),
		NumGroups.X, NumGroups.Y, NumGroups.Z,
		Params.Resolution.X, Params.Resolution.Y, Params.Resolution.Z);
}

// ============================================================================
// Create and Dispatch Function
// ============================================================================

FRDGTextureRef CreateAndDispatchProceduralBandSDF(
	FRDGBuilder& GraphBuilder,
	const FProceduralBandSDFDispatchParams& Params)
{
	// Create 3D texture for SDF output
	FRDGTextureDesc SDFTextureDesc = FRDGTextureDesc::Create3D(
		FIntVector(Params.Resolution.X, Params.Resolution.Y, Params.Resolution.Z),
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV
	);

	FRDGTextureRef SDFTexture = GraphBuilder.CreateTexture(
		SDFTextureDesc,
		TEXT("FleshRing_ProceduralBandSDF")
	);

	// Dispatch the compute shader
	DispatchFleshRingProceduralBandSDF(GraphBuilder, Params, SDFTexture);

	return SDFTexture;
}
