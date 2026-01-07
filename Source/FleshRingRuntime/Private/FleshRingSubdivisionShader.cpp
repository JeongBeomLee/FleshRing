// FleshRingSubdivisionShader.cpp
// GPU Barycentric Interpolation Shader Implementation

#include "FleshRingSubdivisionShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingSubdivisionShader, Log, All);

// ============================================================================
// Shader Implementation
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
	FFleshRingBarycentricInterpolationCS,
	"/Plugin/FleshRingPlugin/FleshRingSubdivisionCS.usf",
	"BarycentricInterpolationCS",
	SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingBarycentricInterpolationCS(
	FRDGBuilder& GraphBuilder,
	const FSubdivisionInterpolationParams& Params,
	const FSubdivisionGPUBuffers& Buffers)
{
	if (Params.NumOutputVertices == 0)
	{
		UE_LOG(LogFleshRingSubdivisionShader, Warning, TEXT("NumOutputVertices is 0, skipping dispatch"));
		return;
	}

	TShaderMapRef<FFleshRingBarycentricInterpolationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	FFleshRingBarycentricInterpolationCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingBarycentricInterpolationCS::FParameters>();

	// Source mesh data
	PassParameters->SourcePositions = GraphBuilder.CreateSRV(Buffers.SourcePositions, PF_R32_FLOAT);
	PassParameters->SourceNormals = GraphBuilder.CreateSRV(Buffers.SourceNormals, PF_R32_FLOAT);
	PassParameters->SourceTangents = GraphBuilder.CreateSRV(Buffers.SourceTangents, PF_R32_FLOAT);
	PassParameters->SourceUVs = GraphBuilder.CreateSRV(Buffers.SourceUVs, PF_R32_FLOAT);
	PassParameters->SourceBoneWeights = GraphBuilder.CreateSRV(Buffers.SourceBoneWeights, PF_R32_FLOAT);
	PassParameters->SourceBoneIndices = GraphBuilder.CreateSRV(Buffers.SourceBoneIndices, PF_R32_UINT);

	// Topology data from CPU
	PassParameters->VertexParentIndices = GraphBuilder.CreateSRV(Buffers.VertexParentIndices, PF_R32_UINT);
	PassParameters->VertexBarycentrics = GraphBuilder.CreateSRV(Buffers.VertexBarycentrics, PF_R32_FLOAT);

	// Output buffers
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(Buffers.OutputPositions, PF_R32_FLOAT);
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(Buffers.OutputNormals, PF_R32_FLOAT);
	PassParameters->OutputTangents = GraphBuilder.CreateUAV(Buffers.OutputTangents, PF_R32_FLOAT);
	PassParameters->OutputUVs = GraphBuilder.CreateUAV(Buffers.OutputUVs, PF_R32_FLOAT);
	PassParameters->OutputBoneWeights = GraphBuilder.CreateUAV(Buffers.OutputBoneWeights, PF_R32_FLOAT);
	PassParameters->OutputBoneIndices = GraphBuilder.CreateUAV(Buffers.OutputBoneIndices, PF_R32_UINT);

	// Parameters
	PassParameters->NumOutputVertices = Params.NumOutputVertices;
	PassParameters->NumBoneInfluences = Params.NumBoneInfluences;

	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumOutputVertices, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRing_BarycentricInterpolation (%d vertices)", Params.NumOutputVertices),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

// ============================================================================
// Buffer Creation Functions
// ============================================================================

void CreateSubdivisionGPUBuffersFromTopology(
	FRDGBuilder& GraphBuilder,
	const FSubdivisionTopologyResult& TopologyResult,
	FSubdivisionInterpolationParams& OutParams,
	FSubdivisionGPUBuffers& OutBuffers)
{
	const int32 NumVertices = TopologyResult.VertexData.Num();
	const int32 NumIndices = TopologyResult.Indices.Num();

	if (NumVertices == 0 || NumIndices == 0)
	{
		UE_LOG(LogFleshRingSubdivisionShader, Warning, TEXT("Empty topology result"));
		return;
	}

	OutParams.NumOutputVertices = NumVertices;

	// Vertex parent indices (3 uints per vertex)
	{
		TArray<uint32> ParentIndices;
		ParentIndices.SetNum(NumVertices * 3);

		for (int32 i = 0; i < NumVertices; ++i)
		{
			const FSubdivisionVertexData& Data = TopologyResult.VertexData[i];
			ParentIndices[i * 3 + 0] = Data.ParentV0;
			ParentIndices[i * 3 + 1] = Data.ParentV1;
			ParentIndices[i * 3 + 2] = Data.ParentV2;
		}

		OutBuffers.VertexParentIndices = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumVertices * 3),
			TEXT("FleshRing_VertexParentIndices"));

		GraphBuilder.QueueBufferUpload(
			OutBuffers.VertexParentIndices,
			ParentIndices.GetData(),
			ParentIndices.Num() * sizeof(uint32),
			ERDGInitialDataFlags::None);
	}

	// Barycentric coordinates (3 floats per vertex)
	{
		TArray<float> Barycentrics;
		Barycentrics.SetNum(NumVertices * 3);

		for (int32 i = 0; i < NumVertices; ++i)
		{
			const FSubdivisionVertexData& Data = TopologyResult.VertexData[i];
			Barycentrics[i * 3 + 0] = Data.BarycentricCoords.X;
			Barycentrics[i * 3 + 1] = Data.BarycentricCoords.Y;
			Barycentrics[i * 3 + 2] = Data.BarycentricCoords.Z;
		}

		OutBuffers.VertexBarycentrics = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 3),
			TEXT("FleshRing_VertexBarycentrics"));

		GraphBuilder.QueueBufferUpload(
			OutBuffers.VertexBarycentrics,
			Barycentrics.GetData(),
			Barycentrics.Num() * sizeof(float),
			ERDGInitialDataFlags::None);
	}

	// Output indices (direct copy from topology)
	{
		OutBuffers.OutputIndices = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumIndices),
			TEXT("FleshRing_SubdividedIndices"));

		GraphBuilder.QueueBufferUpload(
			OutBuffers.OutputIndices,
			TopologyResult.Indices.GetData(),
			TopologyResult.Indices.Num() * sizeof(uint32),
			ERDGInitialDataFlags::None);
	}

	// Output vertex buffers
	OutBuffers.OutputPositions = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 3),
		TEXT("FleshRing_SubdividedPositions"));

	OutBuffers.OutputNormals = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 3),
		TEXT("FleshRing_SubdividedNormals"));

	OutBuffers.OutputTangents = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 4),
		TEXT("FleshRing_SubdividedTangents"));

	OutBuffers.OutputUVs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 2),
		TEXT("FleshRing_SubdividedUVs"));

	OutBuffers.OutputBoneWeights = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * OutParams.NumBoneInfluences),
		TEXT("FleshRing_SubdividedBoneWeights"));

	OutBuffers.OutputBoneIndices = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumVertices * OutParams.NumBoneInfluences),
		TEXT("FleshRing_SubdividedBoneIndices"));

	UE_LOG(LogFleshRingSubdivisionShader, Log,
		TEXT("Created GPU buffers: %d vertices, %d indices"),
		NumVertices, NumIndices);
}

void UploadSourceMeshToGPU(
	FRDGBuilder& GraphBuilder,
	const TArray<FVector>& SourcePositions,
	const TArray<FVector>& SourceNormals,
	const TArray<FVector4>& SourceTangents,
	const TArray<FVector2D>& SourceUVs,
	const TArray<float>& SourceBoneWeights,
	const TArray<uint32>& SourceBoneIndices,
	uint32 NumBoneInfluences,
	FSubdivisionGPUBuffers& OutBuffers)
{
	const int32 NumVertices = SourcePositions.Num();

	if (NumVertices == 0)
	{
		UE_LOG(LogFleshRingSubdivisionShader, Warning, TEXT("Empty source mesh"));
		return;
	}

	// Positions (convert FVector to float array)
	{
		TArray<float> PositionData;
		PositionData.SetNum(NumVertices * 3);
		for (int32 i = 0; i < NumVertices; ++i)
		{
			PositionData[i * 3 + 0] = static_cast<float>(SourcePositions[i].X);
			PositionData[i * 3 + 1] = static_cast<float>(SourcePositions[i].Y);
			PositionData[i * 3 + 2] = static_cast<float>(SourcePositions[i].Z);
		}

		OutBuffers.SourcePositions = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 3),
			TEXT("FleshRing_SourcePositions"));

		GraphBuilder.QueueBufferUpload(
			OutBuffers.SourcePositions,
			PositionData.GetData(),
			PositionData.Num() * sizeof(float),
			ERDGInitialDataFlags::None);
	}

	// Normals
	{
		TArray<float> NormalData;
		NormalData.SetNum(NumVertices * 3);

		if (SourceNormals.Num() == NumVertices)
		{
			for (int32 i = 0; i < NumVertices; ++i)
			{
				NormalData[i * 3 + 0] = static_cast<float>(SourceNormals[i].X);
				NormalData[i * 3 + 1] = static_cast<float>(SourceNormals[i].Y);
				NormalData[i * 3 + 2] = static_cast<float>(SourceNormals[i].Z);
			}
		}
		else
		{
			// Default to up vector if no normals provided
			for (int32 i = 0; i < NumVertices; ++i)
			{
				NormalData[i * 3 + 0] = 0.0f;
				NormalData[i * 3 + 1] = 0.0f;
				NormalData[i * 3 + 2] = 1.0f;
			}
		}

		OutBuffers.SourceNormals = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 3),
			TEXT("FleshRing_SourceNormals"));

		GraphBuilder.QueueBufferUpload(
			OutBuffers.SourceNormals,
			NormalData.GetData(),
			NormalData.Num() * sizeof(float),
			ERDGInitialDataFlags::None);
	}

	// Tangents
	{
		TArray<float> TangentData;
		TangentData.SetNum(NumVertices * 4);

		if (SourceTangents.Num() == NumVertices)
		{
			for (int32 i = 0; i < NumVertices; ++i)
			{
				TangentData[i * 4 + 0] = static_cast<float>(SourceTangents[i].X);
				TangentData[i * 4 + 1] = static_cast<float>(SourceTangents[i].Y);
				TangentData[i * 4 + 2] = static_cast<float>(SourceTangents[i].Z);
				TangentData[i * 4 + 3] = static_cast<float>(SourceTangents[i].W);
			}
		}
		else
		{
			// Default to X-axis tangent with positive binormal sign
			for (int32 i = 0; i < NumVertices; ++i)
			{
				TangentData[i * 4 + 0] = 1.0f;
				TangentData[i * 4 + 1] = 0.0f;
				TangentData[i * 4 + 2] = 0.0f;
				TangentData[i * 4 + 3] = 1.0f;
			}
		}

		OutBuffers.SourceTangents = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 4),
			TEXT("FleshRing_SourceTangents"));

		GraphBuilder.QueueBufferUpload(
			OutBuffers.SourceTangents,
			TangentData.GetData(),
			TangentData.Num() * sizeof(float),
			ERDGInitialDataFlags::None);
	}

	// UVs
	{
		TArray<float> UVData;
		UVData.SetNum(NumVertices * 2);

		if (SourceUVs.Num() == NumVertices)
		{
			for (int32 i = 0; i < NumVertices; ++i)
			{
				UVData[i * 2 + 0] = static_cast<float>(SourceUVs[i].X);
				UVData[i * 2 + 1] = static_cast<float>(SourceUVs[i].Y);
			}
		}
		else
		{
			FMemory::Memzero(UVData.GetData(), UVData.Num() * sizeof(float));
		}

		OutBuffers.SourceUVs = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * 2),
			TEXT("FleshRing_SourceUVs"));

		GraphBuilder.QueueBufferUpload(
			OutBuffers.SourceUVs,
			UVData.GetData(),
			UVData.Num() * sizeof(float),
			ERDGInitialDataFlags::None);
	}

	// Bone Weights
	{
		OutBuffers.SourceBoneWeights = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumVertices * NumBoneInfluences),
			TEXT("FleshRing_SourceBoneWeights"));

		if (SourceBoneWeights.Num() == NumVertices * static_cast<int32>(NumBoneInfluences))
		{
			GraphBuilder.QueueBufferUpload(
				OutBuffers.SourceBoneWeights,
				SourceBoneWeights.GetData(),
				SourceBoneWeights.Num() * sizeof(float),
				ERDGInitialDataFlags::None);
		}
		else
		{
			// Default weights (first bone = 1.0, rest = 0.0)
			TArray<float> DefaultWeights;
			DefaultWeights.SetNumZeroed(NumVertices * NumBoneInfluences);
			for (int32 i = 0; i < NumVertices; ++i)
			{
				DefaultWeights[i * NumBoneInfluences] = 1.0f;
			}
			GraphBuilder.QueueBufferUpload(
				OutBuffers.SourceBoneWeights,
				DefaultWeights.GetData(),
				DefaultWeights.Num() * sizeof(float),
				ERDGInitialDataFlags::None);
		}
	}

	// Bone Indices
	{
		OutBuffers.SourceBoneIndices = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumVertices * NumBoneInfluences),
			TEXT("FleshRing_SourceBoneIndices"));

		if (SourceBoneIndices.Num() == NumVertices * static_cast<int32>(NumBoneInfluences))
		{
			GraphBuilder.QueueBufferUpload(
				OutBuffers.SourceBoneIndices,
				SourceBoneIndices.GetData(),
				SourceBoneIndices.Num() * sizeof(uint32),
				ERDGInitialDataFlags::None);
		}
		else
		{
			// Default to bone 0
			TArray<uint32> DefaultIndices;
			DefaultIndices.SetNumZeroed(NumVertices * NumBoneInfluences);
			GraphBuilder.QueueBufferUpload(
				OutBuffers.SourceBoneIndices,
				DefaultIndices.GetData(),
				DefaultIndices.Num() * sizeof(uint32),
				ERDGInitialDataFlags::None);
		}
	}

	UE_LOG(LogFleshRingSubdivisionShader, Log,
		TEXT("Uploaded source mesh: %d vertices, %d bone influences"),
		NumVertices, NumBoneInfluences);
}

bool ExecuteSubdivisionInterpolation(
	FRDGBuilder& GraphBuilder,
	const FFleshRingSubdivisionProcessor& Processor,
	const TArray<FVector>& SourceNormals,
	const TArray<FVector4>& SourceTangents,
	const TArray<float>& SourceBoneWeights,
	const TArray<uint32>& SourceBoneIndices,
	uint32 NumBoneInfluences,
	FSubdivisionGPUBuffers& OutBuffers)
{
	if (!Processor.IsCacheValid())
	{
		UE_LOG(LogFleshRingSubdivisionShader, Warning, TEXT("Processor cache is not valid"));
		return false;
	}

	const FSubdivisionTopologyResult& TopologyResult = Processor.GetCachedResult();

	if (!TopologyResult.IsValid())
	{
		UE_LOG(LogFleshRingSubdivisionShader, Warning, TEXT("Topology result is not valid"));
		return false;
	}

	// Get source positions from processor (stored internally)
	// Note: We need to access these from processor - for now using empty array
	// In real implementation, processor should expose source data
	TArray<FVector> SourcePositions;
	TArray<FVector2D> SourceUVs;

	// TODO: Processor should expose source mesh data
	// For now, this function requires caller to provide all source data

	FSubdivisionInterpolationParams Params;
	Params.NumBoneInfluences = NumBoneInfluences;

	// Create topology buffers
	CreateSubdivisionGPUBuffersFromTopology(GraphBuilder, TopologyResult, Params, OutBuffers);

	// Upload source mesh (caller should provide these)
	// UploadSourceMeshToGPU(...);

	// Dispatch interpolation shader
	DispatchFleshRingBarycentricInterpolationCS(GraphBuilder, Params, OutBuffers);

	return true;
}
