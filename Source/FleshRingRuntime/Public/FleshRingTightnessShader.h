// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Tightness Shader
// ============================================================================
// Purpose: Creating tight flesh appearance with optional GPU skinning

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingAffectedVertices.h"
#include "FleshRingDebugTypes.h"

// ============================================================================
// FFleshRingTightnessCS - Tightness Compute Shader
// ============================================================================
// Processes only AffectedVertices (not all mesh vertices) for performance
// Pulls vertices inward toward Ring center axis
// Supports optional GPU skinning for animated meshes
class FFleshRingTightnessCS : public FGlobalShader
{
public:
    // Register this class to UE shader system
    DECLARE_GLOBAL_SHADER(FFleshRingTightnessCS);

    // Declare using parameter struct
    SHADER_USE_PARAMETER_STRUCT(FFleshRingTightnessCS, FGlobalShader);

    // Shader Parameters - Must match FleshRingTightnessCS.usf
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // ===== Input Buffers (SRV - Read Only) =====

        // Input: Original vertex positions (bind pose component space)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)

        // Input: Indices of affected vertices to process
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

        // Influence is calculated directly on GPU (VirtualRing: CalculateVirtualRingInfluence, VirtualBand: CalculateVirtualBandInfluence)

        // Input: Representative vertex indices for UV seam welding
        // RepresentativeIndices[ThreadIndex] = representative vertex index for that position group
        // In shader: read representative position -> compute deformation -> write to own index
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

        // ===== Output Buffer (UAV - Read/Write) =====

        // Output: Deformed vertex positions
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Output: Volume accumulation buffer for Bulge pass (Atomic operation)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, VolumeAccumBuffer)

        // Output: Debug Influence values for visualization (1 float per ThreadIndex)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, DebugInfluences)

        // Flag to enable debug influence output (0 = disabled, 1 = enabled)
        SHADER_PARAMETER(uint32, bOutputDebugInfluences)

        // Base offset for debug buffer (multi-ring support) - used for DebugInfluences
        SHADER_PARAMETER(uint32, DebugPointBaseOffset)

        // DebugPointBuffer is processed in DebugPointOutputCS based on final positions

        // ===== Skinning Buffers (SRV - Read Only) =====

        // Bone matrices (3 float4 per bone = 3x4 matrix)
        // RefToLocal matrix: [Bind Pose Component Space] -> [Animated Component Space]
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, BoneMatrices)

        // Packed bone indices + weights
        // Per vertex: [BoneIndex0, BoneIndex1, ...] [Weight0, Weight1, ...]
        // For 4-bone skinning at vertex 100: [5,6,7,0] [0.5,0.3,0.2,0.0]
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InputWeightStream)

        // ===== Skinning Parameters =====

        SHADER_PARAMETER(uint32, InputWeightStride)    // Weight stream stride (bytes)
        SHADER_PARAMETER(uint32, InputWeightIndexSize) // Index/weight byte size (packed)
        SHADER_PARAMETER(uint32, NumBoneInfluences)    // Bone influences per vertex (4 or 8)
        SHADER_PARAMETER(uint32, bEnableSkinning)      // Skinning enable flag (0 or 1)

        // ===== Ring Parameters (Constant Buffer) =====

        SHADER_PARAMETER(FVector3f, RingCenter)       // Ring center position (component space)
        SHADER_PARAMETER(FVector3f, RingAxis)         // Ring axis direction (normalized)
        SHADER_PARAMETER(float, TightnessStrength)    // Tightness strength
        SHADER_PARAMETER(float, RingRadius)           // Ring inner radius
        SHADER_PARAMETER(float, RingHeight)           // Ring height (axis direction)
        SHADER_PARAMETER(float, RingThickness)        // Ring thickness (radial falloff range) - for VirtualRing mode GPU Influence calculation
        SHADER_PARAMETER(uint32, FalloffType)         // Falloff type (0=Linear, 1=Quadratic, 2=Hermite) - for VirtualRing mode GPU Influence calculation
        SHADER_PARAMETER(uint32, InfluenceMode)       // Influence mode (0=Auto/SDF, 1=VirtualRing, 2=VirtualBand)

        // ===== VirtualBand (Virtual Band) Parameters =====
        // For variable radius GPU Influence calculation (Catmull-Rom spline)
        SHADER_PARAMETER(float, LowerRadius)          // Lower end radius
        SHADER_PARAMETER(float, MidLowerRadius)       // Band lower radius
        SHADER_PARAMETER(float, MidUpperRadius)       // Band upper radius
        SHADER_PARAMETER(float, UpperRadius)          // Upper end radius
        SHADER_PARAMETER(float, LowerHeight)          // Lower section height
        SHADER_PARAMETER(float, BandSectionHeight)    // Band section height
        SHADER_PARAMETER(float, UpperHeight)          // Upper section height

        // ===== Counts =====

        SHADER_PARAMETER(uint32, NumAffectedVertices) // Number of affected vertices
        SHADER_PARAMETER(uint32, NumTotalVertices)    // Total vertex count (for bounds checking)

        // ===== Volume Accumulation Parameters (for Bulge pass) =====

        SHADER_PARAMETER(uint32, bAccumulateVolume)   // Enable volume accumulation (0 = disabled, 1 = enabled)
        SHADER_PARAMETER(float, FixedPointScale)      // Fixed-point scale (e.g., 1000.0)
        SHADER_PARAMETER(uint32, RingIndex)           // Ring index (VolumeAccumBuffer slot designation)

        // ===== SDF Parameters (OBB Design) =====
        //
        // OBB approach: SDF is generated in Ring local space
        // In shader: transform vertex (component space) to local, then sample SDF
        // ComponentToSDFLocal = LocalToComponent.Inverse()

        // SDF 3D texture (Ring local space)
        // SDF value: negative = inside, positive = outside
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SDFSampler)

        // SDF volume bounds (Ring local space)
        // UV transform: (LocalPos - BoundsMin) / (BoundsMax - BoundsMin)
        SHADER_PARAMETER(FVector3f, SDFBoundsMin)
        SHADER_PARAMETER(FVector3f, SDFBoundsMax)

        // SDF influence mode (0 = VirtualRing, 1 = Auto/SDF-based)
        SHADER_PARAMETER(uint32, bUseSDFInfluence)

        // Component space -> SDF local space transform matrix (OBB support)
        // Used to inverse-transform BindPos to local, then sample SDF
        SHADER_PARAMETER(FMatrix44f, ComponentToSDFLocal)

        // SDF local space -> Component space transform matrix
        // Used to transform local displacement to component (accurate inverse with scale)
        SHADER_PARAMETER(FMatrix44f, SDFLocalToComponent)

        // SDF mode falloff distance (Influence becomes 0 at this distance)
        SHADER_PARAMETER(float, SDFInfluenceFalloffDistance)

        // Ring Center/Axis (SDF Local Space)
        // Calculated based on original Ring bounds (before extension)
        // Accurately conveys Ring's actual position/axis even if SDF bounds are extended
        SHADER_PARAMETER(FVector3f, SDFLocalRingCenter)
        SHADER_PARAMETER(FVector3f, SDFLocalRingAxis)

        // Z-axis top extension distance (absolute value, in cm)
        // How far to extend above the ring (0 = no extension)
        SHADER_PARAMETER(float, BoundsZTop)

        // Z-axis bottom extension distance (absolute value, in cm)
        // How far to extend below the ring (0 = no extension)
        SHADER_PARAMETER(float, BoundsZBottom)
    END_SHADER_PARAMETER_STRUCT()

    // Shader Compilation Settings
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        // SM5 = Shader Model 5 (~=DX11)
        // Require SM5 for compute shader support
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

        // Thread group size = 64 (must match .usf)
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);

        // Note: Influence values are pre-calculated on CPU with FalloffType and passed to GPU
    }
};

// ============================================================================
// FTightnessDispatchParams - Dispatch Parameters
// ============================================================================

/**
 * Structure that encapsulates the parameters to be passed to the Dispatch function
 * Makes it easier to modify parameters without changing function signatures
 */
struct FTightnessDispatchParams
{
    // =========== Ring Transform ===========

    /**
     * Ring center position (component space)
     * - bEnableSkinning=false: bind pose component space
     * - bEnableSkinning=true: animated component space (current frame)
     */
    FVector3f RingCenter;

    /**
     * Ring orientation axis (normalized)
     */
    FVector3f RingAxis;

    // =========== Ring Geometry ===========

    /**
     * Inner radius from bone axis to ring inner surface
     */
    float RingRadius;

    /**
     * Ring height along axis direction (for GPU reference only, not used in actual deformation)
     */
    float RingHeight;

    /**
     * Ring thickness (radial falloff range) - for VirtualRing mode GPU Influence calculation
     */
    float RingThickness;

    /**
     * Falloff type (0=Linear, 1=Quadratic, 2=Hermite) - for VirtualRing mode GPU Influence calculation
     */
    uint32 FalloffType;

    /**
     * Influence mode (0=Auto/SDF, 1=VirtualRing, 2=VirtualBand)
     */
    uint32 InfluenceMode;

    // =========== VirtualBand (Virtual Band) Parameters ===========
    // For variable radius-based GPU Influence calculation (Catmull-Rom spline)

    /** Lower end radius (below Lower Section) */
    float LowerRadius;

    /** Band lower radius (MidLower - below tightening point) */
    float MidLowerRadius;

    /** Band upper radius (MidUpper - above tightening point) */
    float MidUpperRadius;

    /** Upper end radius (above Upper Section, flesh bulge) */
    float UpperRadius;

    /** Lower section height (lower slope region) */
    float LowerHeight;

    /** Band section height (tightening region) */
    float BandSectionHeight;

    /** Upper section height (upper slope region, bulge area) */
    float UpperHeight;

    // =========== Deformation Parameters ===========

    /**
     * Tightness deformation strength
     */
    float TightnessStrength;

    // =========== Vertex Counts ===========

    /**
     * Number of affected vertices to process
     */
    uint32 NumAffectedVertices;

    /**
     * Total mesh vertex count (for bounds checking)
     */
    uint32 NumTotalVertices;

    // =========== Skinning Parameters ===========

    /**
     * Enable GPU skinning (0=bind pose, 1=skinned)
     */
    uint32 bEnableSkinning;

    /**
     * Weight stream stride in bytes
     * Bytes to skip to reach next vertex data
     * 4-bone skinning: 12, 8-bone skinning: 24
     */
    uint32 InputWeightStride;

    /**
     * Packed: BoneIndexByteSize | (BoneWeightByteSize << 8)
     * Lower 8 bits: bone index size (bytes), Upper 8 bits: bone weight size (bytes)
     * Typical values:
     * Bone index: 1 byte (0~255 range, up to 256 bones)
     * Bone weight: 2 bytes (0~65535 range, higher precision)
     */
    uint32 InputWeightIndexSize;

    /**
     * Number of bone influences per vertex (4 or 8)
     */
    uint32 NumBoneInfluences;

    // =========== SDF Parameters (OBB Design) ===========

    /**
     * SDF volume minimum bounds (Ring local space)
     */
    FVector3f SDFBoundsMin;

    /**
     * SDF volume maximum bounds (Ring local space)
     */
    FVector3f SDFBoundsMax;

    /**
     * Use SDF-based influence calculation (0=VirtualRing/manual, 1=SDF Auto)
     */
    uint32 bUseSDFInfluence;

    /**
     * Component space -> SDF local space transform matrix (OBB support)
     * = LocalToComponent.Inverse()
     * Used to inverse-transform BindPos to local, then sample SDF
     */
    FMatrix44f ComponentToSDFLocal;

    /**
     * SDF local space -> Component space transform matrix
     * Used to transform local displacement to component (accurate inverse with scale)
     */
    FMatrix44f SDFLocalToComponent;

    /**
     * SDF falloff distance - Influence goes from 1.0 to 0.0 over this distance
     * Default: 5.0 (smooth transition near Ring)
     */
    float SDFInfluenceFalloffDistance;

    /**
     * Z-axis top extension distance (absolute value, in cm)
     * How far to extend above the ring (0 = no extension)
     */
    float BoundsZTop;

    /**
     * Z-axis bottom extension distance (absolute value, in cm)
     * How far to extend below the ring (0 = no extension)
     */
    float BoundsZBottom;

    // =========== SDF Local Ring Geometry ===========

    /**
     * Ring Center (SDF Local Space)
     * Calculated based on original Ring bounds (before extension)
     * Accurately conveys Ring's actual position even if SDF bounds are extended
     */
    FVector3f SDFLocalRingCenter;

    /**
     * Ring Axis (SDF Local Space)
     * Calculated based on original Ring bounds (before extension)
     * Accurately conveys Ring's actual axis even if SDF bounds are extended
     */
    FVector3f SDFLocalRingAxis;

    // =========== Volume Accumulation Parameters (for Bulge pass) ===========

    /**
     * Enable volume accumulation for Bulge pass (0 = disabled, 1 = enabled)
     */
    uint32 bAccumulateVolume;

    /**
     * Fixed-point scale for Atomic operations (e.g., 1000.0)
     * float x Scale -> convert to uint for Atomic operations
     */
    float FixedPointScale;

    /**
     * Ring index for per-ring VolumeAccumBuffer slot designation
     */
    uint32 RingIndex;

    // =========== Debug Parameters ===========

    /**
     * Enable debug influence output for visualization (0 = disabled, 1 = enabled)
     */
    uint32 bOutputDebugInfluences;

    // DebugPoint output is handled in DebugPointOutputCS

    /**
     * Base offset for debug buffer (multi-ring support) - used for DebugInfluences
     * Ring 0: offset 0, Ring 1: offset = Ring0.NumAffectedVertices, etc.
     */
    uint32 DebugPointBaseOffset;

    FTightnessDispatchParams()
        : RingCenter(FVector3f::ZeroVector)
        , RingAxis(FVector3f::UpVector)
        , RingRadius(5.0f)
        , RingHeight(2.0f)
        , RingThickness(2.0f)
        , FalloffType(0)
        , InfluenceMode(1)  // Default: VirtualRing (when SDF is not available)
        , LowerRadius(9.0f)
        , MidLowerRadius(8.0f)
        , MidUpperRadius(8.0f)
        , UpperRadius(11.0f)
        , LowerHeight(1.0f)
        , BandSectionHeight(2.0f)
        , UpperHeight(2.0f)
        , TightnessStrength(1.0f)
        , NumAffectedVertices(0)
        , NumTotalVertices(0)
        , bEnableSkinning(0)
        , InputWeightStride(0)
        , InputWeightIndexSize(0)
        , NumBoneInfluences(0)
        , SDFBoundsMin(FVector3f::ZeroVector)
        , SDFBoundsMax(FVector3f::ZeroVector)
        , bUseSDFInfluence(0)
        , ComponentToSDFLocal(FMatrix44f::Identity)
        , SDFLocalToComponent(FMatrix44f::Identity)
        , SDFInfluenceFalloffDistance(5.0f)
        , BoundsZTop(5.0f)
        , BoundsZBottom(0.0f)
        , SDFLocalRingCenter(FVector3f::ZeroVector)
        , SDFLocalRingAxis(FVector3f(0.0f, 0.0f, 1.0f))
        , bAccumulateVolume(0)
        , FixedPointScale(1000.0f)
        , RingIndex(0)
        , bOutputDebugInfluences(0)
        , DebugPointBaseOffset(0)
    {
    }
};

// ============================================================================
// Helper Functions - Parameter Creation
// ============================================================================

/**
 * Create FTightnessDispatchParams from FRingAffectedData (bind pose mode)
 *
 * @param AffectedData - Affected vertex data (including Ring parameters)
 * @param TotalVertexCount - Total mesh vertex count
 * @return Parameter struct for GPU Dispatch (skinning disabled)
 */
inline FTightnessDispatchParams CreateTightnessParams(
    const FRingAffectedData& AffectedData,
    uint32 TotalVertexCount)
{
    FTightnessDispatchParams Params;

    // Ring transform info (bind pose)
    Params.RingCenter = FVector3f(AffectedData.RingCenter);
    Params.RingAxis = FVector3f(AffectedData.RingAxis);

    // Ring geometry info
    Params.RingRadius = AffectedData.RingRadius;
    Params.RingHeight = AffectedData.RingHeight;
    Params.RingThickness = AffectedData.RingThickness;
    Params.FalloffType = static_cast<uint32>(AffectedData.FalloffType);

    // Deformation strength (copied from FFleshRingSettings)
    Params.TightnessStrength = AffectedData.TightnessStrength;

    // Vertex counts
    Params.NumAffectedVertices = static_cast<uint32>(AffectedData.Vertices.Num());
    Params.NumTotalVertices = TotalVertexCount;

    // Disable skinning
    Params.bEnableSkinning = 0;
    Params.InputWeightStride = 0;
    Params.InputWeightIndexSize = 0;
    Params.NumBoneInfluences = 0;

    return Params;
}

// ============================================================================
// Dispatch Function Declarations
// ============================================================================

/**
 * Dispatch TightnessCS to process affected vertices (bind pose mode)
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters (Ring settings, counts, volume accumulation, etc.)
 * @param SourcePositionsBuffer - RDG buffer containing source vertex positions
 * @param AffectedIndicesBuffer - Buffer containing vertex indices to process
 * @param InfluencesBuffer - Buffer containing per-vertex influence weights
 * @param RepresentativeIndicesBuffer - Buffer containing representative vertex indices for UV seam welding
 *                                      (uses AffectedIndices if nullptr)
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 * @param SDFTexture - (Optional) SDF 3D texture for Auto influence mode
 *                     (VirtualRing mode using Influences buffer if nullptr)
 * @param VolumeAccumBuffer - (Optional) Volume accumulation buffer for Bulge pass
 *                            (volume accumulation disabled if nullptr)
 * @param DebugInfluencesBuffer - (Optional) Debug influence output buffer
 *                                Used when Params.bOutputDebugInfluences=1
 *                                DebugPointBuffer is processed in DebugPointOutputCS based on final positions
 */
void DispatchFleshRingTightnessCS(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    // Influence is calculated directly on GPU
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGTextureRef SDFTexture = nullptr,
    FRDGBufferRef VolumeAccumBuffer = nullptr,
    FRDGBufferRef DebugInfluencesBuffer = nullptr);

/**
 * Dispatch TightnessCS with readback for validation/testing (bind pose mode)
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters
 * @param SourcePositionsBuffer - RDG buffer containing source vertex positions
 * @param AffectedIndicesBuffer - Buffer containing vertex indices to process
 * @param InfluencesBuffer - Buffer containing per-vertex influence weights
 * @param RepresentativeIndicesBuffer - Buffer containing representative vertex indices for UV seam welding
 *                                      (uses AffectedIndices if nullptr)
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 * @param Readback - Readback object for GPU->CPU transfer
 * @param SDFTexture - (Optional) SDF 3D texture for Auto influence mode
 * @param VolumeAccumBuffer - (Optional) Volume accumulation buffer for Bulge pass
 * @param DebugInfluencesBuffer - (Optional) Debug influence output buffer
 *                                DebugPointBuffer is processed in DebugPointOutputCS
 */
void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    // Influence is calculated directly on GPU
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback,
    FRDGTextureRef SDFTexture = nullptr,
    FRDGBufferRef VolumeAccumBuffer = nullptr,
    FRDGBufferRef DebugInfluencesBuffer = nullptr);
