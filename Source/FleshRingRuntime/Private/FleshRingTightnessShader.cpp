// Purpose: Pull vertices toward Ring center axis (Tightness effect)

#include "FleshRingTightnessShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

// Shader Implementation Registration
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingTightnessCS,
    "/Plugin/FleshRingPlugin/FleshRingTightnessCS.usf",
    "MainCS",
    SF_Compute
);

// Dispatch Function Implementation
void DispatchFleshRingTightnessCS(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer)
{
    // Early out if no vertices to process
    if (Params.NumAffectedVertices == 0)
    {
        return;
    }

    // Allocate shader parameters
    FFleshRingTightnessCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingTightnessCS::FParameters>();

    // ==== Bind buffers ==== 
    // Create SRV (Read Only)
    PassParameters->SourcePositions = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);

    // Create UAV (Read and Write)
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

    // Set ring parameters
    PassParameters->RingCenter = Params.RingCenter;
    PassParameters->RingAxis = Params.RingAxis;
    PassParameters->TightnessStrength = Params.TightnessStrength;
    PassParameters->RingRadius = Params.RingRadius;
    PassParameters->RingWidth = Params.RingWidth;

    // Set counts
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;

    // Get shader reference
    TShaderMapRef<FFleshRingTightnessCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups
    const uint32 ThreadGroupSize = 64; // match .usf [numthreads(64,1,1)]
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize); // (NumAffectedVertices + 64 - 1) / 64

    // Add compute pass to RDG
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTightnessCS"),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1) // Dispatch(NumGroups, 1, 1)
    );
}

// Dispatch with Readback (for testing/validation)
void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback)
{
    // Dispatch the compute shader
    DispatchFleshRingTightnessCS(
        GraphBuilder,
        Params,
        SourcePositionsBuffer,
        AffectedIndicesBuffer,
        InfluencesBuffer,
        OutputPositionsBuffer
    );

    // Add readback pass
    AddEnqueueCopyPass(GraphBuilder, Readback, OutputPositionsBuffer, 0);
}

// Test Result Validation
void ValidateTightnessResults(
    const float* SourceData,
    const float* OutputData,
    const uint32* AffectedIndices,
    const float* Influences,
    uint32 NumAffected,
    const FVector3f& RingCenter,
    const FVector3f& RingAxis,
    float TightnessStrength)
{
    uint32 PassCount = 0;
    uint32 FailCount = 0;
    constexpr uint32 MaxErrorsToLog = 10;

    UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Validating %d affected vertices"), NumAffected);

    for (uint32 i = 0; i < NumAffected; ++i)
    {
        uint32 VertexIndex = AffectedIndices[i];
        float Influence = Influences[i];

        // Read source position
        uint32 BaseIndex = VertexIndex * 3;
        FVector3f SourcePos(
            SourceData[BaseIndex + 0],
            SourceData[BaseIndex + 1],
            SourceData[BaseIndex + 2]
        );

        // Read output position
        FVector3f OutputPos(
            OutputData[BaseIndex + 0],
            OutputData[BaseIndex + 1],
            OutputData[BaseIndex + 2]
        );

        // Calculate expected position (same logic as shader)
        FVector3f ToVertex = SourcePos - RingCenter;
        float AxisDist = FVector3f::DotProduct(ToVertex, RingAxis);
        FVector3f RadialVec = ToVertex - RingAxis * AxisDist;
        float RadialDist = RadialVec.Size();

        FVector3f ExpectedPos = SourcePos;
        if (RadialDist > 0.001f)
        {
            FVector3f InwardDir = -RadialVec / RadialDist;
            float Displacement = TightnessStrength * Influence;
            ExpectedPos = SourcePos + InwardDir * Displacement;
        }

        // Compare with tolerance
        bool bMatch = FVector3f::Distance(OutputPos, ExpectedPos) < 0.01f;

        if (bMatch)
        {
            PassCount++;
        }
        else
        {
            FailCount++;
            if (FailCount <= MaxErrorsToLog)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("FleshRing.TightnessTest: MISMATCH at AffectedIdx[%d] VertexIdx[%d]"),
                    i, VertexIndex);
                UE_LOG(LogTemp, Error,
                    TEXT("  Source: (%.3f, %.3f, %.3f)"),
                    SourcePos.X, SourcePos.Y, SourcePos.Z);
                UE_LOG(LogTemp, Error,
                    TEXT("  Expected: (%.3f, %.3f, %.3f)"),
                    ExpectedPos.X, ExpectedPos.Y, ExpectedPos.Z);
                UE_LOG(LogTemp, Error,
                    TEXT("  Actual: (%.3f, %.3f, %.3f)"),
                    OutputPos.X, OutputPos.Y, OutputPos.Z);
            }
        }
    }

    // Result summary
    if (FailCount == 0)
    {
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: ===== VALIDATION PASSED ====="));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: All %d vertices deformed correctly!"), PassCount);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("FleshRing.TightnessTest: ===== VALIDATION FAILED ====="));
        UE_LOG(LogTemp, Error, TEXT("FleshRing.TightnessTest: Passed: %d, Failed: %d"), PassCount, FailCount);
    }
}

// Console Command for Testing
static FAutoConsoleCommand GFleshRingTightnessTestCommand(
    TEXT("FleshRing.TightnessTest"), // console command name
    TEXT("Test Tightness Compute Shader with synthetic vertex data"),
    FConsoleCommandDelegate::CreateLambda([]() // execute lambda
    {
        // Test configuration
        const uint32 TotalVertexCount = 256;      // Total mesh vertices
        const uint32 AffectedVertexCount = 64;    // Vertices near ring

        // Ring parameters
        const FVector3f RingCenter(0.0f, 0.0f, 5.0f);
        const FVector3f RingAxis(0.0f, 0.0f, 1.0f);  // Z-up axis
        const float TightnessStrength = 2.0f;
        const float RingRadius = 3.0f;
        const float RingWidth = 2.0f;

        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Starting test"));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Total vertices: %d"), TotalVertexCount);
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Affected vertices: %d"), AffectedVertexCount);
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Ring center: (%.1f, %.1f, %.1f)"),
            RingCenter.X, RingCenter.Y, RingCenter.Z);
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Tightness strength: %.1f"), TightnessStrength);
        UE_LOG(LogTemp, Log, TEXT("========================================="));

        // Create test data on CPU
        TArray<float> SourcePositions;
        TArray<uint32> AffectedIndices;
        TArray<float> Influences;

        SourcePositions.SetNum(TotalVertexCount * 3);
        AffectedIndices.SetNum(AffectedVertexCount);
        Influences.SetNum(AffectedVertexCount);

        // Generate synthetic vertex positions (cylinder around Z axis)
        for (uint32 i = 0; i < TotalVertexCount; ++i)
        {
            float Angle = (float(i) / float(TotalVertexCount)) * 2.0f * PI;
            float Height = (float(i % 16) / 16.0f) * 10.0f;  // 0-10 height
            float Radius = 4.0f + FMath::Sin(Height) * 0.5f; // Varying radius

            SourcePositions[i * 3 + 0] = FMath::Cos(Angle) * Radius; // X
            SourcePositions[i * 3 + 1] = FMath::Sin(Angle) * Radius; // Y
            SourcePositions[i * 3 + 2] = Height;                      // Z
        }

        // Select affected vertices (vertices near ring height)
        uint32 AffectedIdx = 0;
        for (uint32 i = 0; i < TotalVertexCount && AffectedIdx < AffectedVertexCount; ++i)
        {
            float VertexZ = SourcePositions[i * 3 + 2]; // z height
            float DistFromRingPlane = FMath::Abs(VertexZ - RingCenter.Z); // |height - 5|

            // it's affected range
            if (DistFromRingPlane < RingWidth)
            {
                AffectedIndices[AffectedIdx] = i;
                // Linear falloff based on distance from ring plane
                Influences[AffectedIdx] = 1.0f - (DistFromRingPlane / RingWidth);
                AffectedIdx++;
            }
        }

        // Adjust actual affected count
        const uint32 ActualAffectedCount = AffectedIdx;
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Actually affected: %d"), ActualAffectedCount);

        // Create shared data for lambda captures
        TSharedPtr<TArray<float>> SourceDataPtr = MakeShared<TArray<float>>(SourcePositions);
        TSharedPtr<TArray<uint32>> IndicesPtr = MakeShared<TArray<uint32>>(AffectedIndices);
        TSharedPtr<TArray<float>> InfluencesPtr = MakeShared<TArray<float>>(Influences);
        TSharedPtr<FRHIGPUBufferReadback> Readback = MakeShared<FRHIGPUBufferReadback>(TEXT("TightnessTestReadback"));

        // Dispatch parameters
        FTightnessDispatchParams Params;
        Params.RingCenter = RingCenter;
        Params.RingAxis = RingAxis;
        Params.TightnessStrength = TightnessStrength;
        Params.RingRadius = RingRadius;
        Params.RingWidth = RingWidth;
        Params.NumAffectedVertices = ActualAffectedCount;
        Params.NumTotalVertices = TotalVertexCount;

        // Step 1: Create buffers and dispatch on render thread
        ENQUEUE_RENDER_COMMAND(FleshRingTightnessTest_Dispatch)(
            [SourceDataPtr, IndicesPtr, InfluencesPtr, Params, Readback, TotalVertexCount, ActualAffectedCount]
            (FRHICommandListImmediate& RHICmdList)
            {
                // this code is executed on render thread!
                FRDGBuilder GraphBuilder(RHICmdList);

                // Create source positions buffer (upload from CPU)
                FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                    TEXT("TightnessTest_SourcePositions")
                );

                // Upload source data
                GraphBuilder.QueueBufferUpload(
                    SourceBuffer,
                    SourceDataPtr->GetData(),
                    SourceDataPtr->Num() * sizeof(float),
                    ERDGInitialDataFlags::None
                );

                // Create affected indices buffer
                FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ActualAffectedCount),
                    TEXT("TightnessTest_AffectedIndices")
                );

                GraphBuilder.QueueBufferUpload(
                    IndicesBuffer,
                    IndicesPtr->GetData(),
                    IndicesPtr->Num() * sizeof(uint32),
                    ERDGInitialDataFlags::None
                );

                // Create influences buffer
                FRDGBufferRef InfluencesBuffer = GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateStructuredDesc(sizeof(float), ActualAffectedCount),
                    TEXT("TightnessTest_Influences")
                );

                GraphBuilder.QueueBufferUpload(
                    InfluencesBuffer,
                    InfluencesPtr->GetData(),
                    InfluencesPtr->Num() * sizeof(float),
                    ERDGInitialDataFlags::None
                );

                // Create output buffer
                FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                    TEXT("TightnessTest_OutputPositions")
                );

                // Copy source to output first (so non-affected vertices are preserved)
                AddCopyBufferPass(GraphBuilder, OutputBuffer, SourceBuffer);

                // Use the simplified dispatch function with RDG buffers
                DispatchFleshRingTightnessCS_WithReadback(
                    GraphBuilder,
                    Params,
                    SourceBuffer,
                    IndicesBuffer,
                    InfluencesBuffer,
                    OutputBuffer,
                    Readback.Get()
                );

                GraphBuilder.Execute();

                UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Dispatch completed"));
            });

        // Step 2: Validate results
        ENQUEUE_RENDER_COMMAND(FleshRingTightnessTest_Validate)(
            [SourceDataPtr, IndicesPtr, InfluencesPtr, Readback, Params, TotalVertexCount, ActualAffectedCount]
            (FRHICommandListImmediate& RHICmdList)
            {
                if (!Readback->IsReady())
                {
                    RHICmdList.BlockUntilGPUIdle();
                }

                if (Readback->IsReady())
                {
                    const float* OutputData = static_cast<const float*>(
                        Readback->Lock(TotalVertexCount * 3 * sizeof(float)));

                    if (OutputData)
                    {
                        ValidateTightnessResults(
                            SourceDataPtr->GetData(),
                            OutputData,
                            IndicesPtr->GetData(),
                            InfluencesPtr->GetData(),
                            ActualAffectedCount,
                            Params.RingCenter,
                            Params.RingAxis,
                            Params.TightnessStrength
                        );

                        Readback->Unlock();
                    }
                    else
                    {
                        UE_LOG(LogTemp, Error, TEXT("FleshRing.TightnessTest: Failed to lock readback buffer"));
                    }
                }
                else
                {
                    UE_LOG(LogTemp, Error, TEXT("FleshRing.TightnessTest: Readback not ready after GPU idle"));
                }
            });

        UE_LOG(LogTemp, Log, TEXT("FleshRing.TightnessTest: Test commands enqueued"));
    })
);
