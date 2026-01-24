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
		// nullptr 또는 pending kill/GC된 객체 체크
		if (!Mesh || !IsValid(Mesh))
		{
			return false;
		}

		// 파괴 진행 중인 객체 체크 (corrupt 상태 방지)
		if (Mesh->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return false;
		}

		// 렌더 리소스 체크 (GetSkeleton() 먼저 호출하여 기본 접근 검증)
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

		// LOD 데이터 존재 체크
		if (RenderData->LODRenderData.Num() == 0)
		{
			if (bLogWarnings)
			{
				UE_LOG(LogFleshRingUtils, Warning, TEXT("IsSkeletalMeshValid: Mesh '%s' has no LOD data"),
					*Mesh->GetName());
			}
			return false;
		}

		// LOD 0의 버텍스 버퍼 체크 (Null resource in uniform buffer 크래시 방지)
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

		// 스켈레톤 체크
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

		// 부모 인덱스 유효성 체크 (EnsureParentsExist 크래시 방지)
		for (int32 i = 0; i < NumBones; ++i)
		{
			const int32 ParentIndex = RefSkel.GetParentIndex(i);
			// 루트 본은 INDEX_NONE(-1), 나머지는 0 ~ i-1 범위여야 함
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
