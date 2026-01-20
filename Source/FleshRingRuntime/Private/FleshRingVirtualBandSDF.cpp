// ============================================================================
// FleshRing VirtualBand Mathematical SDF Generator - Implementation
// ============================================================================

#include "FleshRingVirtualBandSDF.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
	FFleshRingVirtualBandSDFCS,
	"/Plugin/FleshRingPlugin/FleshRingVirtualBandSDF.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingVirtualBandSDF(
	FRDGBuilder& GraphBuilder,
	const FVirtualBandSDFDispatchParams& Params,
	FRDGTextureRef OutputSDFTexture)
{
	// Validate parameters
	if (!OutputSDFTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("DispatchFleshRingVirtualBandSDF: OutputSDFTexture is null"));
		return;
	}

	if (Params.Resolution.X <= 0 || Params.Resolution.Y <= 0 || Params.Resolution.Z <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("DispatchFleshRingVirtualBandSDF: Invalid resolution"));
		return;
	}

	// Allocate shader parameters
	FFleshRingVirtualBandSDFCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingVirtualBandSDFCS::FParameters>();

	// Bind output texture
	PassParameters->OutputSDF = GraphBuilder.CreateUAV(OutputSDFTexture);

	// Set SDF volume parameters
	PassParameters->SDFBoundsMin = Params.SDFBounds.Min;
	PassParameters->SDFBoundsMax = Params.SDFBounds.Max;
	PassParameters->SDFResolution = Params.Resolution;

	// Set VirtualBand parameters (4 radii: Upper - MidUpper - MidLower - Lower)
	const auto& Settings = Params.BandSettings;
	PassParameters->MidUpperRadius = Settings.MidUpperRadius;
	PassParameters->MidLowerRadius = Settings.MidLowerRadius;
	PassParameters->BandThickness = Settings.BandThickness;
	PassParameters->BandHeight = Settings.BandHeight;
	PassParameters->LowerRadius = Settings.Lower.Radius;
	PassParameters->LowerHeight = Settings.Lower.Height;
	PassParameters->UpperRadius = Settings.Upper.Radius;
	PassParameters->UpperHeight = Settings.Upper.Height;

	// Get shader
	TShaderMapRef<FFleshRingVirtualBandSDFCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups (8x8x8 threads per group)
	const FIntVector NumGroups(
		FMath::DivideAndRoundUp(Params.Resolution.X, 8),
		FMath::DivideAndRoundUp(Params.Resolution.Y, 8),
		FMath::DivideAndRoundUp(Params.Resolution.Z, 8)
	);

	// Add compute pass
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingVirtualBandSDF"),
		ComputeShader,
		PassParameters,
		NumGroups
	);

	UE_LOG(LogTemp, Verbose, TEXT("DispatchFleshRingVirtualBandSDF: Dispatched %dx%dx%d grid (Resolution: %dx%dx%d)"),
		NumGroups.X, NumGroups.Y, NumGroups.Z,
		Params.Resolution.X, Params.Resolution.Y, Params.Resolution.Z);
}

// ============================================================================
// Create and Dispatch Function
// ============================================================================

FRDGTextureRef CreateAndDispatchVirtualBandSDF(
	FRDGBuilder& GraphBuilder,
	const FVirtualBandSDFDispatchParams& Params)
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
		TEXT("FleshRing_VirtualBandSDF")
	);

	// Dispatch the compute shader
	DispatchFleshRingVirtualBandSDF(GraphBuilder, Params, SDFTexture);

	return SDFTexture;
}
