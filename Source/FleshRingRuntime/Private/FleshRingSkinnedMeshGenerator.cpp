// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingSkinnedMeshGenerator.h"
#include "FleshRingAffectedVertices.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "StaticMeshResources.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "MeshDescription.h"
#include "MeshAttributes.h"
#include "SkeletalMeshAttributes.h"
#include "BoneWeights.h"
#include "ReferenceSkeleton.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingSkinnedMesh, Log, All);

USkeletalMesh* FFleshRingSkinnedMeshGenerator::GenerateSkinnedRingMesh(
	UStaticMesh* RingStaticMesh,
	USkeletalMesh* SourceSkeletalMesh,
	const FTransform& RingTransform,
	float SamplingRadius,
	int32 AttachBoneIndex,
	UObject* OuterObject,
	const FString& MeshName)
{
	if (!RingStaticMesh || !SourceSkeletalMesh || !OuterObject)
	{
		UE_LOG(LogFleshRingSkinnedMesh, Warning, TEXT("GenerateSkinnedRingMesh: Invalid input parameters"));
		return nullptr;
	}

	// 1. Extract ring mesh data
	TArray<FVector> RingPositions;
	TArray<FVector> RingNormals;
	TArray<FVector4> RingTangents;
	TArray<FVector2D> RingUVs;
	TArray<uint32> RingIndices;

	if (!ExtractStaticMeshData(RingStaticMesh, RingPositions, RingNormals, RingTangents, RingUVs, RingIndices))
	{
		UE_LOG(LogFleshRingSkinnedMesh, Warning, TEXT("GenerateSkinnedRingMesh: Failed to extract StaticMesh data"));
		return nullptr;
	}

	const int32 RingVertexCount = RingPositions.Num();
	if (RingVertexCount == 0)
	{
		UE_LOG(LogFleshRingSkinnedMesh, Warning, TEXT("GenerateSkinnedRingMesh: Ring mesh has no vertices"));
		return nullptr;
	}

	// 2. Extract skin mesh bone weights
	TArray<FVector3f> SkinVertices;
	TArray<FVertexBoneInfluence> SkinBoneInfluences;

	if (!ExtractSkeletalMeshBoneWeights(SourceSkeletalMesh, SkinVertices, SkinBoneInfluences))
	{
		UE_LOG(LogFleshRingSkinnedMesh, Warning, TEXT("GenerateSkinnedRingMesh: Failed to extract SkeletalMesh bone weights"));
		return nullptr;
	}

	// 3. Build spatial hash for fast nearest neighbor lookup
	FVertexSpatialHash SpatialHash;
	SpatialHash.Build(SkinVertices, SamplingRadius);

	// 4. Build bone chain filter (attach bone + ancestors + descendants)
	// This prevents sampling weights from unrelated bones (e.g., wing when ring is on thigh)
	const FReferenceSkeleton& RefSkeleton = SourceSkeletalMesh->GetRefSkeleton();
	TSet<int32> AllowedBoneIndices = BuildBoneChainSet(RefSkeleton, AttachBoneIndex);

	// 5. Transform ring vertices to component space and sample bone weights
	TArray<TArray<uint16>> RingBoneIndices;
	TArray<TArray<uint8>> RingBoneWeights;
	RingBoneIndices.SetNum(RingVertexCount);
	RingBoneWeights.SetNum(RingVertexCount);

	for (int32 i = 0; i < RingVertexCount; ++i)
	{
		// Transform ring vertex from mesh local to component space
		FVector WorldPos = RingTransform.TransformPosition(RingPositions[i]);

		// Sample bone weights from nearby skin vertices (filtered by bone chain)
		SampleBoneWeightsAtPosition(
			WorldPos,
			SkinVertices,
			SkinBoneInfluences,
			SpatialHash,
			SamplingRadius,
			AllowedBoneIndices,
			RingBoneIndices[i],
			RingBoneWeights[i]
		);
	}

	// 6. Create SkeletalMesh by duplicating source (to copy skeleton and ImportedModel structure)
	USkeletalMesh* SkinnedRingMesh = DuplicateObject<USkeletalMesh>(
		SourceSkeletalMesh,
		OuterObject,
		FName(*MeshName)
	);

	if (!SkinnedRingMesh)
	{
		UE_LOG(LogFleshRingSkinnedMesh, Warning, TEXT("GenerateSkinnedRingMesh: Failed to duplicate SkeletalMesh"));
		return nullptr;
	}

	// Clear flags for permanent storage (not transient)
	SkinnedRingMesh->ClearFlags(RF_Transient);

	FlushRenderingCommands();
	SkinnedRingMesh->ReleaseResources();
	SkinnedRingMesh->ReleaseResourcesFence.Wait();

	// Get number of LODs
	const int32 NumLODs = SkinnedRingMesh->GetLODNum();

	// 7. Set materials BEFORE building mesh (so Build() can reference them)
	SkinnedRingMesh->GetMaterials().Empty();

	FName MaterialSlotName = TEXT("RingMaterial");
	if (RingStaticMesh->GetStaticMaterials().Num() > 0)
	{
		for (const FStaticMaterial& StaticMat : RingStaticMesh->GetStaticMaterials())
		{
			FSkeletalMaterial SkelMat;
			SkelMat.MaterialInterface = StaticMat.MaterialInterface;
			SkelMat.MaterialSlotName = StaticMat.MaterialSlotName;
#if WITH_EDITORONLY_DATA
			SkelMat.ImportedMaterialSlotName = StaticMat.ImportedMaterialSlotName;
#endif
			// Initialize UVChannelData to prevent crash in streaming system
			SkelMat.UVChannelData.bInitialized = true;
			SkinnedRingMesh->GetMaterials().Add(SkelMat);

			// Use first material's slot name for polygon group
			if (MaterialSlotName == TEXT("RingMaterial"))
			{
#if WITH_EDITORONLY_DATA
				MaterialSlotName = StaticMat.ImportedMaterialSlotName.IsNone()
					? StaticMat.MaterialSlotName
					: StaticMat.ImportedMaterialSlotName;
#else
				MaterialSlotName = StaticMat.MaterialSlotName;
#endif
				if (MaterialSlotName.IsNone())
				{
					MaterialSlotName = TEXT("RingMaterial");
				}
			}
		}
	}

	// 8. Build ring geometry for ALL LODs (prevents material index collision)
	// Ring mesh is small, so we use the same geometry for all LODs
	const int32 MaxBoneInfluences = FVertexBoneInfluence::MAX_INFLUENCES;
	const int32 NumIndices = RingIndices.Num();
	const int32 NumFaces = NumIndices / 3;

	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		FMeshDescription MeshDescription;
		FSkeletalMeshAttributes MeshAttributes(MeshDescription);
		MeshAttributes.Register();

		// Create vertices at bind pose position
		MeshDescription.ReserveNewVertices(RingVertexCount);
		for (int32 i = 0; i < RingVertexCount; ++i)
		{
			const FVertexID VertexID = MeshDescription.CreateVertex();
			FVector BindPosePos = RingTransform.TransformPosition(RingPositions[i]);
			MeshDescription.GetVertexPositions()[VertexID] = FVector3f(BindPosePos);
		}

		// Create polygon group (single material)
		MeshDescription.PolygonGroupAttributes().RegisterAttribute<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		FPolygonGroupID GroupID = MeshDescription.CreatePolygonGroup();
		MeshDescription.PolygonGroupAttributes().SetAttribute(GroupID, MeshAttribute::PolygonGroup::ImportedMaterialSlotName, 0, MaterialSlotName);

		// Create vertex instances
		TArray<FVertexInstanceID> VertexInstanceIDs;
		VertexInstanceIDs.Reserve(NumIndices);

		for (int32 i = 0; i < NumIndices; ++i)
		{
			const uint32 VertexIndex = RingIndices[i];
			const FVertexID VertexID(VertexIndex);
			const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);
			VertexInstanceIDs.Add(VertexInstanceID);

			// UV
			if (RingUVs.IsValidIndex(VertexIndex))
			{
				MeshAttributes.GetVertexInstanceUVs().Set(VertexInstanceID, 0, FVector2f(RingUVs[VertexIndex]));
			}

			// Normal
			if (RingNormals.IsValidIndex(VertexIndex))
			{
				FVector TransformedNormal = RingTransform.TransformVectorNoScale(RingNormals[VertexIndex]);
				MeshAttributes.GetVertexInstanceNormals().Set(VertexInstanceID, FVector3f(TransformedNormal.GetSafeNormal()));
			}

			// Tangent
			if (RingTangents.IsValidIndex(VertexIndex))
			{
				FVector TransformedTangent = RingTransform.TransformVectorNoScale(FVector(RingTangents[VertexIndex]));
				MeshAttributes.GetVertexInstanceTangents().Set(VertexInstanceID, FVector3f(TransformedTangent.GetSafeNormal()));
				MeshAttributes.GetVertexInstanceBinormalSigns().Set(VertexInstanceID, RingTangents[VertexIndex].W);
			}
		}

		// Create triangles
		for (int32 i = 0; i < NumFaces; ++i)
		{
			TArray<FVertexInstanceID> TriangleVertexInstances;
			TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 0]);
			TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 1]);
			TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 2]);
			MeshDescription.CreatePolygon(GroupID, TriangleVertexInstances);
		}

		// Set bone weights
		FSkinWeightsVertexAttributesRef SkinWeights = MeshAttributes.GetVertexSkinWeights();
		for (int32 i = 0; i < RingVertexCount; ++i)
		{
			FVertexID VertexID(i);
			TArray<UE::AnimationCore::FBoneWeight> BoneWeightArray;

			for (int32 j = 0; j < MaxBoneInfluences; ++j)
			{
				if (RingBoneWeights[i].IsValidIndex(j) && RingBoneWeights[i][j] > 0)
				{
					UE::AnimationCore::FBoneWeight BW;
					BW.SetBoneIndex(RingBoneIndices[i][j]);
					BW.SetWeight(RingBoneWeights[i][j] / 255.0f);
					BoneWeightArray.Add(BW);
				}
			}

			SkinWeights.Set(VertexID, BoneWeightArray);
		}

		// Commit this LOD (Editor-only API)
#if WITH_EDITORONLY_DATA
		SkinnedRingMesh->CreateMeshDescription(LODIndex, MoveTemp(MeshDescription));

		USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
		CommitParams.bMarkPackageDirty = false;
		SkinnedRingMesh->CommitMeshDescription(LODIndex, CommitParams);

		// Disable normal/tangent recomputation for this LOD
		if (FSkeletalMeshLODInfo* LODInfo = SkinnedRingMesh->GetLODInfo(LODIndex))
		{
			LODInfo->BuildSettings.bRecomputeNormals = false;
			LODInfo->BuildSettings.bRecomputeTangents = false;
		}
#endif
	}

#if WITH_EDITORONLY_DATA
	SkinnedRingMesh->Build();
#endif
	SkinnedRingMesh->InitResources();

	FlushRenderingCommands();

	// Update bounds (use transformed bind pose positions)
	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < RingVertexCount; ++i)
	{
		FVector BindPosePos = RingTransform.TransformPosition(RingPositions[i]);
		BoundingBox += BindPosePos;
	}
	SkinnedRingMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	SkinnedRingMesh->CalculateExtendedBounds();

	// Materials were already set before Build()

	return SkinnedRingMesh;
}

void FFleshRingSkinnedMeshGenerator::SampleBoneWeightsAtPosition(
	const FVector& RingVertexPosition,
	const TArray<FVector3f>& SkinVertices,
	const TArray<FVertexBoneInfluence>& SkinBoneInfluences,
	const FVertexSpatialHash& SpatialHash,
	float SamplingRadius,
	const TSet<int32>& AllowedBoneIndices,
	TArray<uint16>& OutBoneIndices,
	TArray<uint8>& OutBoneWeights)
{
	const int32 MaxInfluences = FVertexBoneInfluence::MAX_INFLUENCES;
	const bool bUseBoneFilter = AllowedBoneIndices.Num() > 0;

	OutBoneIndices.SetNum(MaxInfluences);
	OutBoneWeights.SetNum(MaxInfluences);

	// Initialize to zero
	for (int32 i = 0; i < MaxInfluences; ++i)
	{
		OutBoneIndices[i] = 0;
		OutBoneWeights[i] = 0;
	}

	// Query nearby vertices using spatial hash
	TArray<int32> NearbyVertices;
	FVector Min = RingVertexPosition - FVector(SamplingRadius);
	FVector Max = RingVertexPosition + FVector(SamplingRadius);
	SpatialHash.QueryAABB(Min, Max, NearbyVertices);

	if (NearbyVertices.Num() == 0)
	{
		// Fallback: find closest vertex using brute force
		float MinDistance = FLT_MAX;
		int32 ClosestVertex = INDEX_NONE;

		for (int32 i = 0; i < SkinVertices.Num(); ++i)
		{
			float Distance = FVector::Dist(RingVertexPosition, FVector(SkinVertices[i]));
			if (Distance < MinDistance)
			{
				MinDistance = Distance;
				ClosestVertex = i;
			}
		}

		if (ClosestVertex != INDEX_NONE)
		{
			// Copy weights from closest vertex (with bone filter)
			const FVertexBoneInfluence& Influence = SkinBoneInfluences[ClosestVertex];
			int32 OutIdx = 0;
			for (int32 i = 0; i < MaxInfluences && OutIdx < MaxInfluences; ++i)
			{
				if (Influence.BoneWeights[i] > 0)
				{
					// Apply bone filter if enabled
					if (bUseBoneFilter && !AllowedBoneIndices.Contains(Influence.BoneIndices[i]))
					{
						continue;
					}
					OutBoneIndices[OutIdx] = Influence.BoneIndices[i];
					OutBoneWeights[OutIdx] = Influence.BoneWeights[i];
					++OutIdx;
				}
			}
		}
		return;
	}

	// Accumulate weights with distance-based weighting
	TMap<uint16, float> AccumulatedWeights;
	float TotalDistanceWeight = 0.0f;

	for (int32 VertexIdx : NearbyVertices)
	{
		if (!SkinVertices.IsValidIndex(VertexIdx) || !SkinBoneInfluences.IsValidIndex(VertexIdx))
		{
			continue;
		}

		FVector SkinPos = FVector(SkinVertices[VertexIdx]);
		float Distance = FVector::Dist(RingVertexPosition, SkinPos);

		if (Distance > SamplingRadius)
		{
			continue;
		}

		// Distance-based weight (closer = higher weight)
		float NormalizedDistance = Distance / SamplingRadius;
		float DistanceWeight = FMath::Square(1.0f - NormalizedDistance);  // Quadratic falloff
		TotalDistanceWeight += DistanceWeight;

		// Accumulate bone weights (with bone filter)
		const FVertexBoneInfluence& Influence = SkinBoneInfluences[VertexIdx];
		for (int32 i = 0; i < MaxInfluences; ++i)
		{
			if (Influence.BoneWeights[i] > 0)
			{
				// Apply bone filter if enabled
				if (bUseBoneFilter && !AllowedBoneIndices.Contains(Influence.BoneIndices[i]))
				{
					continue;
				}
				float NormalizedBoneWeight = Influence.BoneWeights[i] / 255.0f;
				AccumulatedWeights.FindOrAdd(Influence.BoneIndices[i]) += NormalizedBoneWeight * DistanceWeight;
			}
		}
	}

	if (TotalDistanceWeight <= 0.0f || AccumulatedWeights.Num() == 0)
	{
		return;
	}

	// Sort by weight
	TArray<TPair<uint16, float>> SortedWeights;
	for (const auto& Pair : AccumulatedWeights)
	{
		SortedWeights.Add(TPair<uint16, float>(Pair.Key, Pair.Value / TotalDistanceWeight));
	}
	SortedWeights.Sort([](const TPair<uint16, float>& A, const TPair<uint16, float>& B)
	{
		return A.Value > B.Value;
	});

	// Normalize top weights
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < FMath::Min(SortedWeights.Num(), MaxInfluences); ++i)
	{
		TotalWeight += SortedWeights[i].Value;
	}

	// Output top influences
	for (int32 i = 0; i < MaxInfluences; ++i)
	{
		if (i < SortedWeights.Num() && TotalWeight > 0.0f)
		{
			OutBoneIndices[i] = SortedWeights[i].Key;
			OutBoneWeights[i] = FMath::Clamp<uint8>(
				FMath::RoundToInt((SortedWeights[i].Value / TotalWeight) * 255.0f),
				0, 255
			);
		}
		else
		{
			OutBoneIndices[i] = 0;
			OutBoneWeights[i] = 0;
		}
	}
}

bool FFleshRingSkinnedMeshGenerator::ExtractStaticMeshData(
	UStaticMesh* StaticMesh,
	TArray<FVector>& OutPositions,
	TArray<FVector>& OutNormals,
	TArray<FVector4>& OutTangents,
	TArray<FVector2D>& OutUVs,
	TArray<uint32>& OutIndices)
{
	if (!StaticMesh || !StaticMesh->GetRenderData())
	{
		return false;
	}

	const FStaticMeshLODResources& LODResources = StaticMesh->GetRenderData()->LODResources[0];

	// Extract vertex data
	const FPositionVertexBuffer& PositionBuffer = LODResources.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertexBuffer = LODResources.VertexBuffers.StaticMeshVertexBuffer;

	const int32 NumVertices = PositionBuffer.GetNumVertices();
	OutPositions.SetNum(NumVertices);
	OutNormals.SetNum(NumVertices);
	OutTangents.SetNum(NumVertices);
	OutUVs.SetNum(NumVertices);

	for (int32 i = 0; i < NumVertices; ++i)
	{
		OutPositions[i] = FVector(PositionBuffer.VertexPosition(i));
		OutNormals[i] = FVector(VertexBuffer.VertexTangentZ(i));

		FVector4f TangentX = VertexBuffer.VertexTangentX(i);
		OutTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);

		if (VertexBuffer.GetNumTexCoords() > 0)
		{
			OutUVs[i] = FVector2D(VertexBuffer.GetVertexUV(i, 0));
		}
		else
		{
			OutUVs[i] = FVector2D::ZeroVector;
		}
	}

	// Extract index data
	const FRawStaticIndexBuffer& IndexBuffer = LODResources.IndexBuffer;
	const int32 NumIndices = IndexBuffer.GetNumIndices();
	OutIndices.SetNum(NumIndices);

	// GetCopy handles both 16-bit and 32-bit indices, converting to uint32
	IndexBuffer.GetCopy(OutIndices);

	return true;
}

bool FFleshRingSkinnedMeshGenerator::ExtractSkeletalMeshBoneWeights(
	USkeletalMesh* SkeletalMesh,
	TArray<FVector3f>& OutVertices,
	TArray<FVertexBoneInfluence>& OutBoneInfluences)
{
	if (!SkeletalMesh)
	{
		return false;
	}

	FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return false;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
	const uint32 NumVertices = LODData.GetNumVertices();

	OutVertices.SetNum(NumVertices);
	OutBoneInfluences.SetNum(NumVertices);

	// Extract vertex positions
	const FPositionVertexBuffer& PositionBuffer = LODData.StaticVertexBuffers.PositionVertexBuffer;
	for (uint32 i = 0; i < NumVertices; ++i)
	{
		OutVertices[i] = PositionBuffer.VertexPosition(i);
	}

	// Extract bone weights
	const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();
	if (!SkinWeightBuffer || SkinWeightBuffer->GetNumVertices() == 0)
	{
		UE_LOG(LogFleshRingSkinnedMesh, Warning, TEXT("ExtractSkeletalMeshBoneWeights: No skin weight buffer"));
		return false;
	}

	const int32 MaxInfluences = FMath::Min(
		(int32)SkinWeightBuffer->GetMaxBoneInfluences(),
		FVertexBoneInfluence::MAX_INFLUENCES
	);

	// Build vertex to section mapping for bone index conversion
	TArray<int32> VertexToSectionIndex;
	VertexToSectionIndex.SetNum(NumVertices);
	for (int32& SectionIdx : VertexToSectionIndex)
	{
		SectionIdx = INDEX_NONE;
	}

	// Index buffer for section mapping
	TArray<uint32> Indices;
	LODData.MultiSizeIndexContainer.GetIndexBuffer(Indices);

	for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
		const int32 StartIndex = Section.BaseIndex;
		const int32 EndIndex = StartIndex + Section.NumTriangles * 3;

		for (int32 IdxPos = StartIndex; IdxPos < EndIndex; ++IdxPos)
		{
			if (Indices.IsValidIndex(IdxPos))
			{
				uint32 VertexIdx = Indices[IdxPos];
				if (VertexIdx < NumVertices && VertexToSectionIndex[VertexIdx] == INDEX_NONE)
				{
					VertexToSectionIndex[VertexIdx] = SectionIdx;
				}
			}
		}
	}

	// Extract bone weights per vertex
	for (uint32 VertIdx = 0; VertIdx < NumVertices; ++VertIdx)
	{
		FVertexBoneInfluence& Influence = OutBoneInfluences[VertIdx];
		FMemory::Memzero(Influence.BoneIndices, sizeof(Influence.BoneIndices));
		FMemory::Memzero(Influence.BoneWeights, sizeof(Influence.BoneWeights));

		int32 SectionIdx = VertexToSectionIndex[VertIdx];
		const TArray<FBoneIndexType>* BoneMap = nullptr;
		if (SectionIdx != INDEX_NONE && SectionIdx < LODData.RenderSections.Num())
		{
			BoneMap = &LODData.RenderSections[SectionIdx].BoneMap;
		}

		for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; ++InfluenceIdx)
		{
			uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(VertIdx, InfluenceIdx);
			uint8 Weight = SkinWeightBuffer->GetBoneWeight(VertIdx, InfluenceIdx);

			// Convert local to global bone index
			uint16 GlobalBoneIdx = LocalBoneIdx;
			if (BoneMap && LocalBoneIdx < BoneMap->Num())
			{
				GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
			}

			Influence.BoneIndices[InfluenceIdx] = GlobalBoneIdx;
			Influence.BoneWeights[InfluenceIdx] = Weight;
		}
	}

	return true;
}

TSet<int32> FFleshRingSkinnedMeshGenerator::BuildBoneChainSet(
	const FReferenceSkeleton& RefSkeleton,
	int32 BoneIndex)
{
	TSet<int32> BoneChain;

	if (BoneIndex == INDEX_NONE)
	{
		return BoneChain;
	}

	const int32 NumBones = RefSkeleton.GetNum();
	if (BoneIndex < 0 || BoneIndex >= NumBones)
	{
		return BoneChain;
	}

	// 1. Add the attach bone itself
	BoneChain.Add(BoneIndex);

	// 2. Add all ancestors (parents up to root)
	int32 CurrentBone = BoneIndex;
	while (CurrentBone != INDEX_NONE)
	{
		int32 ParentBone = RefSkeleton.GetParentIndex(CurrentBone);
		if (ParentBone != INDEX_NONE)
		{
			BoneChain.Add(ParentBone);
		}
		CurrentBone = ParentBone;
	}

	// 3. Add all descendants (children recursively)
	// Build parent-to-children map first for efficient traversal
	TMultiMap<int32, int32> ParentToChildren;
	for (int32 i = 0; i < NumBones; ++i)
	{
		int32 ParentIdx = RefSkeleton.GetParentIndex(i);
		if (ParentIdx != INDEX_NONE)
		{
			ParentToChildren.Add(ParentIdx, i);
		}
	}

	// BFS to collect all descendants
	TArray<int32> Queue;
	Queue.Add(BoneIndex);

	while (Queue.Num() > 0)
	{
		int32 Current = Queue.Pop(EAllowShrinking::No);

		TArray<int32> Children;
		ParentToChildren.MultiFind(Current, Children);

		for (int32 ChildBone : Children)
		{
			if (!BoneChain.Contains(ChildBone))
			{
				BoneChain.Add(ChildBone);
				Queue.Add(ChildBone);
			}
		}
	}

	return BoneChain;
}
