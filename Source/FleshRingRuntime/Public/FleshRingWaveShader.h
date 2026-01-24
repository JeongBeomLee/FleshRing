// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

class FFleshRingWaveCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFleshRingWaveCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingWaveCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Vertex Data
		SHADER_PARAMETER_SRV(Buffer<float>, SourcePositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)
		SHADER_PARAMETER(uint32, BaseVertexIndex)  // Section's base vertex index
		SHADER_PARAMETER(uint32, NumVertices)      // Section's vertex count

		// Skinning Data (same as Optimus Skeleton Data Interface)
		SHADER_PARAMETER_SRV(Buffer<float4>, BoneMatrices)  // Section-specific bone buffer
		SHADER_PARAMETER_SRV(Buffer<uint>, InputWeightStream)
		SHADER_PARAMETER(uint32, NumBoneInfluences)
		SHADER_PARAMETER(uint32, InputWeightStride)
		SHADER_PARAMETER(uint32, InputWeightIndexSize)

		// Jelly Effect Parameters
		SHADER_PARAMETER(float, WaveAmplitude)
		SHADER_PARAMETER(float, WaveFrequency)
		SHADER_PARAMETER(float, Time)
		SHADER_PARAMETER(FVector3f, Velocity)
		SHADER_PARAMETER(float, InertiaStrength)
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
