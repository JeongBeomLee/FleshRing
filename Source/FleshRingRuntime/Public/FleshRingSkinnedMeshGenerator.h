// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingSubdivisionProcessor.h"

class UStaticMesh;
class USkeletalMesh;
class FVertexSpatialHash;

/**
 * Utility class for generating skinned ring meshes
 *
 * Converts StaticMesh ring to SkeletalMesh with bone weights sampled from nearby skin vertices.
 * This allows the ring mesh to deform along with the character's skin when twist bones rotate.
 */
class FLESHRINGRUNTIME_API FFleshRingSkinnedMeshGenerator
{
public:
	/**
	 * Generates a skinned ring SkeletalMesh from a StaticMesh
	 *
	 * @param RingStaticMesh - Original ring StaticMesh
	 * @param SourceSkeletalMesh - Character's SkeletalMesh to sample bone weights from
	 * @param RingTransform - Ring mesh transform in component space
	 * @param SamplingRadius - Radius (cm) to search for nearby skin vertices for weight sampling
	 * @param OuterObject - Outer object for the created SkeletalMesh (typically FleshRingAsset)
	 * @param MeshName - Name for the created mesh
	 * @return Generated SkeletalMesh with bone weights, or nullptr on failure
	 */
	static USkeletalMesh* GenerateSkinnedRingMesh(
		UStaticMesh* RingStaticMesh,
		USkeletalMesh* SourceSkeletalMesh,
		const FTransform& RingTransform,
		float SamplingRadius,
		UObject* OuterObject,
		const FString& MeshName
	);

private:
	/**
	 * Samples bone weights at a given position from nearby skin vertices
	 * Uses distance-weighted average of nearby vertices' bone weights
	 *
	 * @param RingVertexPosition - Position to sample weights at (component space)
	 * @param SkinVertices - Array of skin mesh vertex positions
	 * @param SkinBoneInfluences - Array of skin mesh bone influences
	 * @param SpatialHash - Spatial hash for fast nearest neighbor lookup
	 * @param SamplingRadius - Search radius in cm
	 * @param OutBoneIndices - Output bone indices (MAX_INFLUENCES elements)
	 * @param OutBoneWeights - Output bone weights (MAX_INFLUENCES elements, 0-255 normalized)
	 */
	static void SampleBoneWeightsAtPosition(
		const FVector& RingVertexPosition,
		const TArray<FVector3f>& SkinVertices,
		const TArray<FVertexBoneInfluence>& SkinBoneInfluences,
		const FVertexSpatialHash& SpatialHash,
		float SamplingRadius,
		TArray<uint16>& OutBoneIndices,
		TArray<uint8>& OutBoneWeights
	);

	/**
	 * Extracts vertex data from a StaticMesh
	 *
	 * @param StaticMesh - Source StaticMesh
	 * @param OutPositions - Output vertex positions
	 * @param OutNormals - Output vertex normals
	 * @param OutTangents - Output vertex tangents (XYZ + W for binormal sign)
	 * @param OutUVs - Output UV coordinates
	 * @param OutIndices - Output triangle indices
	 * @return true if extraction succeeded
	 */
	static bool ExtractStaticMeshData(
		UStaticMesh* StaticMesh,
		TArray<FVector>& OutPositions,
		TArray<FVector>& OutNormals,
		TArray<FVector4>& OutTangents,
		TArray<FVector2D>& OutUVs,
		TArray<uint32>& OutIndices
	);

	/**
	 * Extracts bone weight data from a SkeletalMesh
	 *
	 * @param SkeletalMesh - Source SkeletalMesh
	 * @param OutVertices - Output vertex positions
	 * @param OutBoneInfluences - Output bone influences per vertex
	 * @return true if extraction succeeded
	 */
	static bool ExtractSkeletalMeshBoneWeights(
		USkeletalMesh* SkeletalMesh,
		TArray<FVector3f>& OutVertices,
		TArray<FVertexBoneInfluence>& OutBoneInfluences
	);
};
