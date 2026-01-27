// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingUtils, Log, All);

namespace FleshRingUtils
{
	bool IsSkeletalMeshValid(USkeletalMesh* Mesh, bool bLogWarnings)
	{
		// Check for nullptr or pending kill/GC'd object
		if (!Mesh || !IsValid(Mesh))
		{
			return false;
		}

		// Check for object being destroyed (prevent corrupt state)
		if (Mesh->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return false;
		}

		// Check render resource (call GetSkeleton() first to verify basic access)
		if (!Mesh->GetSkeleton())
		{
			if (bLogWarnings)
			{
				UE_LOG(LogFleshRingUtils, Warning, TEXT("IsSkeletalMeshValid: Mesh '%s' has no skeleton"),
					*Mesh->GetName());
			}
			return false;
		}

		FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		if (!RenderData)
		{
			if (bLogWarnings)
			{
				UE_LOG(LogFleshRingUtils, Warning, TEXT("IsSkeletalMeshValid: Mesh '%s' has no render resource"),
					*Mesh->GetName());
			}
			return false;
		}

		// Check LOD data exists
		if (RenderData->LODRenderData.Num() == 0)
		{
			if (bLogWarnings)
			{
				UE_LOG(LogFleshRingUtils, Warning, TEXT("IsSkeletalMeshValid: Mesh '%s' has no LOD data"),
					*Mesh->GetName());
			}
			return false;
		}

		// Check vertex buffer of LOD 0 (prevent "Null resource in uniform buffer" crash)
		const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
		if (LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() == 0)
		{
			if (bLogWarnings)
			{
				UE_LOG(LogFleshRingUtils, Warning, TEXT("IsSkeletalMeshValid: Mesh '%s' has empty position buffer"),
					*Mesh->GetName());
			}
			return false;
		}

		// Check skeleton
		const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
		const int32 NumBones = RefSkel.GetNum();

		if (NumBones == 0)
		{
			if (bLogWarnings)
			{
				UE_LOG(LogFleshRingUtils, Warning, TEXT("IsSkeletalMeshValid: Mesh '%s' has no bones"),
					*Mesh->GetName());
			}
			return false;
		}

		// Check parent index validity (prevent EnsureParentsExist crash)
		for (int32 i = 0; i < NumBones; ++i)
		{
			const int32 ParentIndex = RefSkel.GetParentIndex(i);
			// Root bone is INDEX_NONE(-1), others must be in range 0 ~ i-1
			if (ParentIndex != INDEX_NONE && (ParentIndex < 0 || ParentIndex >= i))
			{
				if (bLogWarnings)
				{
					UE_LOG(LogFleshRingUtils, Warning,
						TEXT("IsSkeletalMeshValid: Mesh '%s' bone %d has invalid parent index %d (NumBones=%d)"),
						*Mesh->GetName(), i, ParentIndex, NumBones);
				}
				return false;
			}
		}

		return true;
	}
}
