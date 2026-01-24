// Copyright 2026 LgThx. All Rights Reserved.

// FleshRing Test Compute Shader - C++ Implementation
// Purpose: Verify GPU Compute Shader pipeline operation

// Unreal Coding Standard: Include own header first
#include "FleshRingTestCS.h"

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "RenderingThread.h"  // For ENQUEUE_RENDER_COMMAND
#include "RHIGPUReadback.h"   // For GPU -> CPU readback

// ============================================================================
// Compute Shader Class Definition
// ============================================================================
// FGlobalShader: Shader that can be used independently without materials
// Compute Shaders typically inherit from FGlobalShader
class FFleshRingTestCS : public FGlobalShader
{
public:
    // Register this class as a Global Shader to Unreal Engine
    DECLARE_GLOBAL_SHADER(FFleshRingTestCS);

    // Use parameter struct defined with BEGIN_SHADER_PARAMETER_STRUCT
    SHADER_USE_PARAMETER_STRUCT(FFleshRingTestCS, FGlobalShader);

    // ========================================================================
    // Shader Parameter Struct
    // ========================================================================
    // Must match 1:1 with variables in .usf file
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Matches: RWStructuredBuffer<float> TestBuffer;
        // UAV = Unordered Access View (Read+Write access)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, TestBuffer)

        // Matches: uint BufferSize;
        SHADER_PARAMETER(uint32, BufferSize)
    END_SHADER_PARAMETER_STRUCT()

    // ========================================================================
    // Shader Compile Conditions (Optional)
    // ========================================================================
    // Determines which platforms compile this shader
    // Modern platforms (Windows/PS5/XSX) all support Compute Shaders
    // Add IsFeatureLevelSupported() condition if mobile target is needed
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return true;
    }
};

// ============================================================================
// Shader Implementation Registration
// ============================================================================
// IMPLEMENT_GLOBAL_SHADER(ClassName, .usfPath, EntryFunction, ShaderType)
//
// Path Explanation:
// - "/FleshRingPlugin/..." is a virtual path
// - Mapped to actual path in FleshRingRuntime.cpp's StartupModule()
// - Actual file: Plugins/FleshRingPlugin/Shaders/FleshRingTestCS.usf
//
// SF_Compute: Specifies this shader is a Compute Shader
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingTestCS,                           // C++ class name
    "/Plugin/FleshRingPlugin/FleshRingTestCS.usf",     // .usf file virtual path
    "MainCS",                                    // Entry function name in .usf
    SF_Compute                                   // Shader type
);

// ============================================================================
// Dispatch Function (For external calls)
// ============================================================================
// FRDGBuilder: Render Dependency Graph builder
// - Handles GPU resource creation, Pass addition, dependency management
//
// Count: Number of data elements to process
void DispatchFleshRingTestCS(FRDGBuilder& GraphBuilder, uint32 Count)
{
    // 1. Allocate parameter struct
    // GraphBuilder manages memory (no manual release needed)
    FFleshRingTestCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FFleshRingTestCS::FParameters>();

    // 2. Create GPU buffer
    // CreateStructuredDesc: Creates structured buffer descriptor
    // - sizeof(float): Size of each element (4 bytes)
    // - Count: Number of elements
    FRDGBufferRef TestBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Count),
        TEXT("FleshRingTestBuffer")  // Debug name (visible in RenderDoc)
    );

    // 3. Create UAV (writable view) and bind to parameters
    Parameters->TestBuffer = GraphBuilder.CreateUAV(TestBuffer);
    Parameters->BufferSize = Count;

    // 4. Get shader reference
    // TShaderMapRef: Reference to compiled shader
    // GetGlobalShaderMap: Gets from global shader map
    // GMaxRHIFeatureLevel: Current RHI's max feature level
    TShaderMapRef<FFleshRingTestCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 5. Add Compute Shader Pass
    // FComputeShaderUtils::AddPass: Adds CS execution Pass to RDG
    //
    // Parameters:
    // - GraphBuilder: RDG builder
    // - RDG_EVENT_NAME: Debug event name (visible in RenderDoc)
    // - ComputeShader: Shader to execute
    // - Parameters: Parameters to pass to shader
    // - FIntVector: Dispatch group count (X, Y, Z)
    //
    // Group count calculation:
    // - Total data: Count elements
    // - Threads per group: 64 ([numthreads(64,1,1)])
    // - Groups needed: ceil(Count / 64)
    // - DivideAndRoundUp: Ceiling division
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTestCS"),
        ComputeShader,
        Parameters,
        FIntVector(
            FMath::DivideAndRoundUp(Count, 64u),  // X direction groups
            1,                                     // Y direction groups
            1                                      // Z direction groups
        )
    );

    // Note: Actual GPU execution happens when GraphBuilder.Execute() is called
    // This function only "schedules" the Pass
}

// ============================================================================
// Dispatch Function with Validation (Includes Readback)
// ============================================================================
// Reads GPU computed results back to CPU for validation
// Readback object ownership belongs to caller
void DispatchFleshRingTestCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    uint32 Count,
    FRHIGPUBufferReadback* Readback)
{
    // 1. Allocate parameter struct
    FFleshRingTestCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FFleshRingTestCS::FParameters>();

    // 2. Create GPU buffer
    FRDGBufferRef TestBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Count),
        TEXT("FleshRingTestBuffer")
    );

    // 3. Create UAV and bind to parameters
    Parameters->TestBuffer = GraphBuilder.CreateUAV(TestBuffer);
    Parameters->BufferSize = Count;

    // 4. Get shader reference
    TShaderMapRef<FFleshRingTestCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 5. Add Compute Shader Pass
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTestCS"),
        ComputeShader,
        Parameters,
        FIntVector(FMath::DivideAndRoundUp(Count, 64u), 1, 1)
    );

    // 6. Add Readback Pass
    // Schedules GPU buffer -> CPU memory copy
    // AddEnqueueCopyPass: Copies RDG buffer to Readback object
    AddEnqueueCopyPass(GraphBuilder, Readback, TestBuffer, 0);
}

// ============================================================================
// Result Validation Function
// ============================================================================
// Verifies GPU computed values match expected values (ThreadId * 2.0f)
void ValidateTestCSResults(const float* Data, uint32 Count)
{
    uint32 PassCount = 0;
    uint32 FailCount = 0;
    constexpr uint32 MaxErrorsToLog = 10;  // Prevent too many error logs

    for (uint32 i = 0; i < Count; ++i)
    {
        // Expected value: ThreadId * 2.0f (see FleshRingTestCS.usf)
        const float Expected = static_cast<float>(i) * 2.0f;
        const float Actual = Data[i];

        // Floating point comparison (with tolerance)
        if (FMath::IsNearlyEqual(Actual, Expected, 0.001f))
        {
            PassCount++;
        }
        else
        {
            FailCount++;
            if (FailCount <= MaxErrorsToLog)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("FleshRing.TestCS: MISMATCH at [%d] - Expected: %.2f, Actual: %.2f"),
                    i, Expected, Actual);
            }
        }
    }

    // Result summary log
    if (FailCount == 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("FleshRing.TestCS: ===== VALIDATION PASSED ====="));
        UE_LOG(LogTemp, Log,
            TEXT("FleshRing.TestCS: All %d elements computed correctly!"), PassCount);
        UE_LOG(LogTemp, Log,
            TEXT("FleshRing.TestCS: Sample values - [0]=%.1f, [1]=%.1f, [2]=%.1f, [100]=%.1f"),
            Data[0], Data[1], Data[2], Data[100]);
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("FleshRing.TestCS: ===== VALIDATION FAILED ====="));
        UE_LOG(LogTemp, Error,
            TEXT("FleshRing.TestCS: Passed: %d, Failed: %d (Total: %d)"),
            PassCount, FailCount, Count);
    }
}

// ============================================================================
// Console Command Registration
// ============================================================================
// FAutoConsoleCommand: Creating as static object auto-registers on module load
//
// Usage: Enter "FleshRing.TestCS" in editor console (~)
static FAutoConsoleCommand GFleshRingTestCSCommand(
    TEXT("FleshRing.TestCS"),
    TEXT("Execute FleshRing Compute Shader test and validate results"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        const uint32 TestCount = 1024;

        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Starting compute shader test"));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Element count: %d"), TestCount);
        UE_LOG(LogTemp, Log, TEXT("========================================="));

        // Create Readback object (shared_ptr for sharing between lambdas)
        // Object for GPU -> CPU data transfer
        TSharedPtr<FRHIGPUBufferReadback> Readback =
            MakeShared<FRHIGPUBufferReadback>(TEXT("FleshRingTestReadback"));

        // Step 1: Execute CS and schedule Readback on render thread
        ENQUEUE_RENDER_COMMAND(FleshRingTestCS_Dispatch)(
            [TestCount, Readback](FRHICommandListImmediate& RHICmdList)
            {
                FRDGBuilder GraphBuilder(RHICmdList);
                DispatchFleshRingTestCS_WithReadback(GraphBuilder, TestCount, Readback.Get());
                GraphBuilder.Execute();

                UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Dispatch and readback enqueued"));
            });

        // Step 2: Validate results (after Readback completion)
        // Separate render command to ensure previous command completion
        ENQUEUE_RENDER_COMMAND(FleshRingTestCS_Validate)(
            [TestCount, Readback](FRHICommandListImmediate& RHICmdList)
            {
                // Wait for Readback completion
                // Wait until IsReady() returns true
                // Usually ready immediately after previous command execution
                if (!Readback->IsReady())
                {
                    // If not ready yet, force sync with RHI flush
                    RHICmdList.BlockUntilGPUIdle();
                }

                if (Readback->IsReady())
                {
                    // Map GPU memory -> CPU memory
                    const float* ResultData = static_cast<const float*>(
                        Readback->Lock(TestCount * sizeof(float)));

                    if (ResultData)
                    {
                        // Validate results
                        ValidateTestCSResults(ResultData, TestCount);

                        // Unmap memory
                        Readback->Unlock();
                    }
                    else
                    {
                        UE_LOG(LogTemp, Error,
                            TEXT("FleshRing.TestCS: Failed to lock readback buffer"));
                    }
                }
                else
                {
                    UE_LOG(LogTemp, Error,
                        TEXT("FleshRing.TestCS: Readback not ready after GPU idle"));
                }
            });

        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Test commands enqueued"));
    })
);
