// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "FleshRingSubdivisionProcessor.h"

#if WITH_EDITOR
#include "Animation/Skeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "BoneWeights.h"
#include "RenderingThread.h"
#include "FleshRingComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/TransactionObjectEvent.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingAsset, Log, All);

UFleshRingAsset::UFleshRingAsset()
{
	// SdfSettings는 이제 각 Ring별로 FFleshRingSettings에 포함됨
}

int32 UFleshRingAsset::AddRing(const FFleshRingSettings& NewRing)
{
	return Rings.Add(NewRing);
}

bool UFleshRingAsset::RemoveRing(int32 Index)
{
	if (Rings.IsValidIndex(Index))
	{
		Rings.RemoveAt(Index);
		return true;
	}
	return false;
}

bool UFleshRingAsset::IsValid() const
{
	// 타겟 메시가 설정되어 있어야 함
	if (TargetSkeletalMesh.IsNull())
	{
		return false;
	}

	// Ring이 최소 1개 이상 있어야 함
	if (Rings.Num() == 0)
	{
		return false;
	}

	// 모든 Ring이 유효한 본 이름을 가져야 함
	for (const FFleshRingSettings& Ring : Rings)
	{
		if (Ring.BoneName == NAME_None)
		{
			return false;
		}
	}

	return true;
}

bool UFleshRingAsset::NeedsSubdivisionRegeneration() const
{
	if (!bEnableSubdivision)
	{
		return false;
	}

	if (!SubdividedMesh)
	{
		return true;
	}

	return CalculateSubdivisionParamsHash() != SubdivisionParamsHash;
}

uint32 UFleshRingAsset::CalculateSubdivisionParamsHash() const
{
	uint32 Hash = 0;

	// Target mesh path
	if (!TargetSkeletalMesh.IsNull())
	{
		Hash = HashCombine(Hash, GetTypeHash(TargetSkeletalMesh.ToSoftObjectPath().ToString()));
	}

	// Subdivision settings
	Hash = HashCombine(Hash, GetTypeHash(bEnableSubdivision));
	Hash = HashCombine(Hash, GetTypeHash(MaxSubdivisionLevel));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(MinEdgeLength * 100)));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(InfluenceRadiusMultiplier * 100)));

	// Ring settings (영향 영역 관련)
	for (const FFleshRingSettings& Ring : Rings)
	{
		Hash = HashCombine(Hash, GetTypeHash(Ring.BoneName.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingRadius * 10)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingWidth * 10)));
		Hash = HashCombine(Hash, GetTypeHash(Ring.RingOffset.ToString()));
	}

	return Hash;
}

#if WITH_EDITOR
void UFleshRingAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// EulerRotation 변경 시 FQuat 동기화
	for (FFleshRingSettings& Ring : Rings)
	{
		Ring.RingRotation = Ring.RingEulerRotation.Quaternion();
		Ring.MeshRotation = Ring.MeshEulerRotation.Quaternion();
	}

	// 에셋이 수정되었음을 표시
	MarkPackageDirty();

	// 전체 리프레시가 필요한 변경인지 확인
	bool bNeedsFullRefresh = false;

	// 배열 구조 변경 시 전체 갱신
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayMove)
	{
		bNeedsFullRefresh = true;
	}

	// 특정 프로퍼티 변경 시 전체 갱신
	if (PropertyChangedEvent.Property)
	{
		FName PropName = PropertyChangedEvent.Property->GetFName();

		if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh) ||
			PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, bEnableSubdivision) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingMesh) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode))
		{
			bNeedsFullRefresh = true;
		}
		// 트랜스폼 관련 프로퍼티 (Offset, Rotation, Scale, Radius, Strength, Falloff 등)는
		// 전체 갱신 불필요 - 경량 업데이트로 처리
	}

	// 구조적 변경 시에만 전체 리프레시 브로드캐스트
	// (트랜스폼 변경은 FFleshRingAssetEditor::OnObjectPropertyChanged에서 경량 업데이트 처리)
	if (bNeedsFullRefresh && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		OnAssetChanged.Broadcast(this);
	}
}

// Helper: 스켈레탈 메시 유효성 검사 (공통 유틸리티 래퍼)
static bool IsSkeletalMeshValidForUse(USkeletalMesh* Mesh)
{
	return FleshRingUtils::IsSkeletalMeshValid(Mesh, /*bLogWarnings=*/ false);
}

void UFleshRingAsset::PostTransacted(const FTransactionObjectEvent& TransactionEvent)
{
	Super::PostTransacted(TransactionEvent);

	// Undo/Redo 이벤트만 처리
	if (TransactionEvent.GetEventType() != ETransactionObjectEventType::UndoRedo)
	{
		return;
	}

	// SubdividedMesh가 손상된 경우 제거
	if (SubdividedMesh && !IsSkeletalMeshValidForUse(SubdividedMesh))
	{
		UE_LOG(LogFleshRingAsset, Warning,
			TEXT("PostTransacted: SubdividedMesh '%s' is invalid after Undo/Redo, clearing..."),
			*SubdividedMesh->GetName());

		// 손상된 메시 제거 (재생성은 사용자가 명시적으로 해야 함)
		// 자동 재생성하면 또 다른 Undo 트랜잭션 이슈 발생 가능
		SubdividedMesh = nullptr;
	}

	// PreviewSubdividedMesh도 검사 (Transient이지만 세션 중 손상 가능)
	if (PreviewSubdividedMesh && !IsSkeletalMeshValidForUse(PreviewSubdividedMesh))
	{
		UE_LOG(LogFleshRingAsset, Warning,
			TEXT("PostTransacted: PreviewSubdividedMesh is invalid after Undo/Redo, clearing..."));
		PreviewSubdividedMesh = nullptr;
	}

	// 프리뷰 씬에서 재생성할 수 있도록 브로드캐스트
	// (SetFleshRingAsset에서 HasValidPreviewMesh() 체크 후 재생성)
	OnAssetChanged.Broadcast(this);
}

void UFleshRingAsset::GenerateSubdividedMesh()
{
	// 이전 SubdividedMesh가 있으면 먼저 제거 (같은 이름 충돌 방지)
	if (SubdividedMesh)
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateSubdividedMesh: 기존 SubdividedMesh 제거 중..."));

		// ★ 중요: 메시를 파괴하기 전에 사용 중인 컴포넌트들이 원본 메시로 전환하도록 알림
		// 먼저 포인터를 임시 저장하고 nullptr로 설정 → 컴포넌트들이 원본 메시로 전환
		USkeletalMesh* MeshToDestroy = SubdividedMesh;
		SubdividedMesh = nullptr;

		// 델리게이트 브로드캐스트하여 프리뷰 씬 등이 원본 메시로 전환하게 함
		OnAssetChanged.Broadcast(this);

		// 월드의 FleshRingComponent들도 직접 업데이트
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UWorld* World = Context.World())
				{
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						if (UFleshRingComponent* Comp = It->FindComponentByClass<UFleshRingComponent>())
						{
							if (Comp->FleshRingAsset == this)
							{
								// ApplyAsset()이 SubdividedMesh == nullptr을 보고 원본 메시로 전환
								Comp->ApplyAsset();
							}
						}
					}
				}
			}
		}

		// 렌더 스레드가 메시 전환을 완료할 때까지 대기
		FlushRenderingCommands();

		// 이제 안전하게 이전 메시 파괴
		// GUID로 고유 이름 사용하므로 이름 충돌 없음 → CollectGarbage() 불필요
		// 다음 GC 사이클에서 자동 정리됨
		MeshToDestroy->ConditionalBeginDestroy();

		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateSubdividedMesh: 기존 SubdividedMesh 제거 완료"));
	}

	if (!bEnableSubdivision)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateSubdividedMesh: Subdivision이 비활성화됨"));
		return;
	}

	if (TargetSkeletalMesh.IsNull())
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: TargetSkeletalMesh가 설정되지 않음"));
		return;
	}

	USkeletalMesh* SourceMesh = TargetSkeletalMesh.LoadSynchronous();
	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: SourceMesh 로드 실패"));
		return;
	}

	if (Rings.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Ring이 설정되지 않음"));
		return;
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateSubdividedMesh 시작: SourceMesh=%s, Rings=%d"),
		*SourceMesh->GetName(), Rings.Num());

	// ============================================
	// 1. 소스 메시 렌더 데이터 획득
	// ============================================
	FSkeletalMeshRenderData* RenderData = SourceMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: RenderData 없음"));
		return;
	}

	const FSkeletalMeshLODRenderData& SourceLODData = RenderData->LODRenderData[0];
	const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	UE_LOG(LogFleshRingAsset, Log, TEXT("Source mesh: %d vertices"), SourceVertexCount);

	// ============================================
	// 2. 소스 버텍스 데이터 추출
	// ============================================
	TArray<FVector> SourcePositions;
	TArray<FVector> SourceNormals;
	TArray<FVector4> SourceTangents;
	TArray<FVector2D> SourceUVs;

	SourcePositions.SetNum(SourceVertexCount);
	SourceNormals.SetNum(SourceVertexCount);
	SourceTangents.SetNum(SourceVertexCount);
	SourceUVs.SetNum(SourceVertexCount);

	for (uint32 i = 0; i < SourceVertexCount; ++i)
	{
		SourcePositions[i] = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
		SourceNormals[i] = FVector(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));
		FVector4f TangentX = SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
		SourceTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);
		SourceUVs[i] = FVector2D(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
	}

	// 인덱스 추출
	TArray<uint32> SourceIndices;
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = SourceLODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (IndexBuffer)
	{
		const int32 NumIndices = IndexBuffer->Num();
		SourceIndices.SetNum(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			SourceIndices[i] = IndexBuffer->Get(i);
		}
	}

	// 섹션별 머티리얼 인덱스 추출 (삼각형별)
	TArray<int32> SourceTriangleMaterialIndices;
	{
		const int32 NumTriangles = SourceIndices.Num() / 3;
		SourceTriangleMaterialIndices.SetNum(NumTriangles);

		// 각 섹션의 삼각형 범위를 기반으로 머티리얼 인덱스 할당
		for (const FSkelMeshRenderSection& Section : SourceLODData.RenderSections)
		{
			const int32 StartTriangle = Section.BaseIndex / 3;
			const int32 EndTriangle = StartTriangle + Section.NumTriangles;

			for (int32 TriIdx = StartTriangle; TriIdx < EndTriangle && TriIdx < NumTriangles; ++TriIdx)
			{
				SourceTriangleMaterialIndices[TriIdx] = Section.MaterialIndex;
			}
		}

		UE_LOG(LogFleshRingAsset, Log, TEXT("Extracted %d sections from source mesh"),
			SourceLODData.RenderSections.Num());
	}

	// 본 웨이트 추출
	const int32 MaxBoneInfluences = SourceLODData.GetVertexBufferMaxBoneInfluences();
	TArray<TArray<uint16>> SourceBoneIndices;  // 실제 스켈레톤 본 인덱스로 변환됨
	TArray<TArray<uint8>> SourceBoneWeights;

	SourceBoneIndices.SetNum(SourceVertexCount);
	SourceBoneWeights.SetNum(SourceVertexCount);

	// ★ 버텍스별 섹션 인덱스 맵 생성 (BoneMap 변환용)
	TArray<int32> VertexToSectionIndex;
	VertexToSectionIndex.SetNum(SourceVertexCount);
	for (int32& SectionIdx : VertexToSectionIndex)
	{
		SectionIdx = INDEX_NONE;
	}

	// 인덱스 버퍼를 순회하여 각 버텍스가 속한 섹션 파악
	for (int32 SectionIdx = 0; SectionIdx < SourceLODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = SourceLODData.RenderSections[SectionIdx];
		const int32 StartIndex = Section.BaseIndex;
		const int32 EndIndex = StartIndex + Section.NumTriangles * 3;

		for (int32 IdxPos = StartIndex; IdxPos < EndIndex; ++IdxPos)
		{
			uint32 VertexIdx = SourceIndices[IdxPos];
			if (VertexIdx < SourceVertexCount && VertexToSectionIndex[VertexIdx] == INDEX_NONE)
			{
				VertexToSectionIndex[VertexIdx] = SectionIdx;
			}
		}
	}

	// ★ 디버그: 섹션별 BoneMap 정보 출력
	UE_LOG(LogFleshRingAsset, Log, TEXT("=== Section BoneMap Debug ==="));
	for (int32 SectionIdx = 0; SectionIdx < SourceLODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = SourceLODData.RenderSections[SectionIdx];
		UE_LOG(LogFleshRingAsset, Log, TEXT("Section %d: BoneMap size=%d, BaseVertex=%d, NumVertices=%d"),
			SectionIdx, Section.BoneMap.Num(), Section.BaseVertexIndex, Section.NumVertices);

		// 처음 5개 BoneMap 엔트리 출력
		FString BoneMapStr;
		for (int32 k = 0; k < FMath::Min(5, Section.BoneMap.Num()); ++k)
		{
			BoneMapStr += FString::Printf(TEXT("[%d->%d] "), k, Section.BoneMap[k]);
		}
		UE_LOG(LogFleshRingAsset, Log, TEXT("  BoneMap (first 5): %s"), *BoneMapStr);
	}

	const FSkinWeightVertexBuffer* SkinWeightBuffer = SourceLODData.GetSkinWeightVertexBuffer();
	if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
	{
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			SourceBoneIndices[i].SetNum(MaxBoneInfluences);
			SourceBoneWeights[i].SetNum(MaxBoneInfluences);

			// 버텍스가 속한 섹션 찾기
			int32 SectionIdx = VertexToSectionIndex[i];
			const TArray<FBoneIndexType>* BoneMap = nullptr;
			if (SectionIdx != INDEX_NONE && SectionIdx < SourceLODData.RenderSections.Num())
			{
				BoneMap = &SourceLODData.RenderSections[SectionIdx].BoneMap;
			}

			for (int32 j = 0; j < MaxBoneInfluences; ++j)
			{
				uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(i, j);
				uint8 Weight = SkinWeightBuffer->GetBoneWeight(i, j);

				// ★ BoneMap을 사용하여 실제 스켈레톤 본 인덱스로 변환
				uint16 GlobalBoneIdx = LocalBoneIdx;
				if (BoneMap && LocalBoneIdx < BoneMap->Num())
				{
					GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
				}

				SourceBoneIndices[i][j] = GlobalBoneIdx;
				SourceBoneWeights[i][j] = Weight;
			}
		}

		// ★ 디버그: 처음 5개 버텍스의 본 웨이트 출력
		UE_LOG(LogFleshRingAsset, Log, TEXT("=== Sample Vertex Bone Weights ==="));
		for (uint32 i = 0; i < FMath::Min(5u, SourceVertexCount); ++i)
		{
			FString WeightStr;
			for (int32 j = 0; j < MaxBoneInfluences; ++j)
			{
				if (SourceBoneWeights[i][j] > 0)
				{
					WeightStr += FString::Printf(TEXT("[Bone%d:%.2f] "),
						SourceBoneIndices[i][j], SourceBoneWeights[i][j] / 255.0f);
				}
			}
			UE_LOG(LogFleshRingAsset, Log, TEXT("Vertex %d (Section %d): %s"),
				i, VertexToSectionIndex[i], *WeightStr);
		}
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("Extracted: %d positions, %d indices, MaxBoneInfluences=%d"),
		SourcePositions.Num(), SourceIndices.Num(), MaxBoneInfluences);

	// ============================================
	// 2.5 엣지 길이 분석 (Subdivision 안전성 확인용)
	// ============================================
	{
		float MinEdge = FLT_MAX, MaxEdge = 0.0f, TotalEdge = 0.0f;
		int32 EdgeCount = 0;

		// 엣지 길이 분포 히스토그램 (0-1, 1-2, 2-5, 5-10, 10+ cm)
		int32 HistBins[5] = {0, 0, 0, 0, 0};

		for (int32 i = 0; i < SourceIndices.Num(); i += 3)
		{
			const FVector& P0 = SourcePositions[SourceIndices[i]];
			const FVector& P1 = SourcePositions[SourceIndices[i+1]];
			const FVector& P2 = SourcePositions[SourceIndices[i+2]];

			float Edges[3] = {
				FVector::Dist(P0, P1),
				FVector::Dist(P1, P2),
				FVector::Dist(P2, P0)
			};

			for (float E : Edges)
			{
				MinEdge = FMath::Min(MinEdge, E);
				MaxEdge = FMath::Max(MaxEdge, E);
				TotalEdge += E;
				EdgeCount++;

				// 히스토그램 분류
				if (E < 1.0f) HistBins[0]++;
				else if (E < 2.0f) HistBins[1]++;
				else if (E < 5.0f) HistBins[2]++;
				else if (E < 10.0f) HistBins[3]++;
				else HistBins[4]++;
			}
		}

		const float AvgEdge = EdgeCount > 0 ? TotalEdge / EdgeCount : 0.0f;

		UE_LOG(LogFleshRingAsset, Warning, TEXT(""));
		UE_LOG(LogFleshRingAsset, Warning, TEXT("╔══════════════════════════════════════════════════════════════╗"));
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║           EDGE LENGTH ANALYSIS (Subdivision Safety)         ║"));
		UE_LOG(LogFleshRingAsset, Warning, TEXT("╠══════════════════════════════════════════════════════════════╣"));
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║  Total Edges: %8d                                       ║"), EdgeCount);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║  Min Edge:    %8.3f cm                                   ║"), MinEdge);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║  Max Edge:    %8.3f cm                                   ║"), MaxEdge);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║  Avg Edge:    %8.3f cm                                   ║"), AvgEdge);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("╠══════════════════════════════════════════════════════════════╣"));
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║  Edge Distribution:                                         ║"));
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║    < 1 cm:  %6d (%5.1f%%)                                 ║"), HistBins[0], EdgeCount > 0 ? HistBins[0] * 100.0f / EdgeCount : 0.0f);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║    1-2 cm:  %6d (%5.1f%%)                                 ║"), HistBins[1], EdgeCount > 0 ? HistBins[1] * 100.0f / EdgeCount : 0.0f);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║    2-5 cm:  %6d (%5.1f%%)                                 ║"), HistBins[2], EdgeCount > 0 ? HistBins[2] * 100.0f / EdgeCount : 0.0f);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║    5-10 cm: %6d (%5.1f%%)                                 ║"), HistBins[3], EdgeCount > 0 ? HistBins[3] * 100.0f / EdgeCount : 0.0f);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║    > 10 cm: %6d (%5.1f%%)                                 ║"), HistBins[4], EdgeCount > 0 ? HistBins[4] * 100.0f / EdgeCount : 0.0f);
		UE_LOG(LogFleshRingAsset, Warning, TEXT("╠══════════════════════════════════════════════════════════════╣"));
		UE_LOG(LogFleshRingAsset, Warning, TEXT("║  Subdivision Level Prediction (MinEdgeLength=%.1f):          ║"), MinEdgeLength);

		int32 PredictedStopLevel = 0;
		for (int32 L = 1; L <= 6; ++L)
		{
			const float ExpectedAvg = AvgEdge / FMath::Pow(2.0f, (float)L);
			const bool bWillStop = ExpectedAvg < MinEdgeLength;
			if (bWillStop && PredictedStopLevel == 0)
			{
				PredictedStopLevel = L;
			}
			UE_LOG(LogFleshRingAsset, Warning, TEXT("║    Level %d: Avg → %6.3f cm  %s                        ║"),
				L, ExpectedAvg, bWillStop ? TEXT("[STOP]") : TEXT("       "));
		}

		UE_LOG(LogFleshRingAsset, Warning, TEXT("╠══════════════════════════════════════════════════════════════╣"));
		if (PredictedStopLevel > 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("║  ★ PREDICTION: Most edges will stop at Level %d             ║"), PredictedStopLevel);
			UE_LOG(LogFleshRingAsset, Warning, TEXT("║    Setting MaxLevel=%d is SAFE                              ║"), MaxSubdivisionLevel);
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("║  ⚠ WARNING: Edges may subdivide beyond Level 6!            ║"));
			UE_LOG(LogFleshRingAsset, Warning, TEXT("║    Consider increasing MinEdgeLength or reducing MaxLevel   ║"));
		}
		UE_LOG(LogFleshRingAsset, Warning, TEXT("╚══════════════════════════════════════════════════════════════╝"));
		UE_LOG(LogFleshRingAsset, Warning, TEXT(""));
	}

	// ============================================
	// 3. Subdivision 프로세서로 토폴로지 계산
	// ============================================
	FFleshRingSubdivisionProcessor Processor;

	if (!Processor.SetSourceMesh(SourcePositions, SourceIndices, SourceUVs, SourceTriangleMaterialIndices))
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: SetSourceMesh 실패"));
		return;
	}

	// 프로세서 설정
	FSubdivisionProcessorSettings Settings;
	Settings.MaxSubdivisionLevel = MaxSubdivisionLevel;
	Settings.MinEdgeLength = MinEdgeLength;
	Processor.SetSettings(Settings);

	// ★ 모든 Ring에 대해 파라미터 설정
	UE_LOG(LogFleshRingAsset, Log, TEXT("Setting up subdivision for %d Ring(s)..."), Rings.Num());

	const USkeleton* Skeleton = SourceMesh->GetSkeleton();
	const FReferenceSkeleton& RefSkeleton = SourceMesh->GetRefSkeleton();
	const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

	Processor.ClearRingParams();

	for (int32 RingIdx = 0; RingIdx < Rings.Num(); ++RingIdx)
	{
		const FFleshRingSettings& Ring = Rings[RingIdx];
		FSubdivisionRingParams RingParams;

		UE_LOG(LogFleshRingAsset, Log, TEXT("=== Setting up Ring %d/%d (Bone: %s) ==="),
			RingIdx + 1, Rings.Num(), *Ring.BoneName.ToString());

		int32 BoneIndex = RefSkeleton.FindBoneIndex(Ring.BoneName);

		if (BoneIndex != INDEX_NONE)
		{
			// 컴포넌트 스페이스 트랜스폼 계산 (부모 본 체인을 따라 누적)
			FTransform BoneTransform = RefBonePose[BoneIndex];
			int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
			while (ParentIndex != INDEX_NONE)
			{
				BoneTransform = BoneTransform * RefBonePose[ParentIndex];
				ParentIndex = RefSkeleton.GetParentIndex(ParentIndex);
			}

			// Auto 모드: RingMesh의 바운드 사용
			if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto && !Ring.RingMesh.IsNull())
			{
				UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
				if (RingMesh)
				{
					RingParams.bUseSDFBounds = true;

					// RingMesh의 로컬 바운드 획득
					FBox MeshBounds = RingMesh->GetBoundingBox();

					// FleshRingComponent::GenerateSDF와 동일한 방식으로 트랜스폼 계산
					FTransform MeshTransform = FTransform(Ring.MeshRotation, Ring.MeshOffset);
					MeshTransform.SetScale3D(Ring.MeshScale);
					FTransform LocalToComponent = MeshTransform * BoneTransform;

					RingParams.SDFBoundsMin = FVector(MeshBounds.Min);
					RingParams.SDFBoundsMax = FVector(MeshBounds.Max);
					RingParams.SDFLocalToComponent = LocalToComponent;
					RingParams.SDFInfluenceMultiplier = InfluenceRadiusMultiplier;

					UE_LOG(LogFleshRingAsset, Log, TEXT("  Auto mode: Bounds Min=%s, Max=%s"),
						*RingParams.SDFBoundsMin.ToString(), *RingParams.SDFBoundsMax.ToString());
				}
				else
				{
					UE_LOG(LogFleshRingAsset, Warning, TEXT("  RingMesh load failed, falling back to Manual mode"));
					RingParams.bUseSDFBounds = false;
				}
			}
			else
			{
				// Manual 모드: Torus 파라미터 사용
				RingParams.bUseSDFBounds = false;

				FVector LocalOffset = Ring.RingRotation.RotateVector(Ring.RingOffset);
				RingParams.Center = BoneTransform.GetLocation() + LocalOffset;
				RingParams.Axis = Ring.RingRotation.RotateVector(FVector::UpVector);
				RingParams.Radius = Ring.RingRadius;
				RingParams.Width = Ring.RingWidth;
				RingParams.InfluenceMultiplier = InfluenceRadiusMultiplier;

				UE_LOG(LogFleshRingAsset, Log, TEXT("  Manual mode: Center=%s, Radius=%.2f"),
					*RingParams.Center.ToString(), RingParams.Radius);
			}
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("  Bone '%s' not found, using default center"),
				*Ring.BoneName.ToString());
			RingParams.bUseSDFBounds = false;
			RingParams.Center = FVector::ZeroVector;
			RingParams.Axis = FVector::UpVector;
			RingParams.Radius = Ring.RingRadius;
			RingParams.Width = Ring.RingWidth;
		}

		Processor.AddRingParams(RingParams);
	}

	// Subdivision 실행
	FSubdivisionTopologyResult TopologyResult;
	if (!Processor.Process(TopologyResult))
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Subdivision 프로세스 실패"));
		return;
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("Subdivision complete: %d -> %d vertices, %d -> %d triangles"),
		TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount,
		TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount);

	// ============================================
	// 디버그: 처음 5개 서브디비전 버텍스의 부모 정보 출력
	// ============================================
	UE_LOG(LogFleshRingAsset, Log, TEXT("=== DEBUG: First 5 Subdivided Vertices Parent Info ==="));
	const int32 OrigVertCount = TopologyResult.OriginalVertexCount;
	for (int32 DbgIdx = OrigVertCount; DbgIdx < FMath::Min(OrigVertCount + 5, TopologyResult.VertexData.Num()); ++DbgIdx)
	{
		const FSubdivisionVertexData& DbgVD = TopologyResult.VertexData[DbgIdx];
		UE_LOG(LogFleshRingAsset, Log, TEXT("  V[%d]: Parents=(%u, %u, %u), Bary=(%.3f, %.3f, %.3f)"),
			DbgIdx, DbgVD.ParentV0, DbgVD.ParentV1, DbgVD.ParentV2,
			DbgVD.BarycentricCoords.X, DbgVD.BarycentricCoords.Y, DbgVD.BarycentricCoords.Z);

		// 부모 위치 출력 (유효 범위 체크)
		if (DbgVD.ParentV0 < (uint32)SourceVertexCount && DbgVD.ParentV1 < (uint32)SourceVertexCount)
		{
			const FVector& SP0 = SourcePositions[DbgVD.ParentV0];
			const FVector& SP1 = SourcePositions[DbgVD.ParentV1];
			const FVector& SP2 = (DbgVD.ParentV2 < (uint32)SourceVertexCount) ? SourcePositions[DbgVD.ParentV2] : FVector::ZeroVector;

			FVector ComputedPos = SP0 * DbgVD.BarycentricCoords.X + SP1 * DbgVD.BarycentricCoords.Y + SP2 * DbgVD.BarycentricCoords.Z;
			FVector ExpectedMidpoint = (SP0 + SP1) * 0.5f;  // Edge midpoint의 경우 예상 위치

			UE_LOG(LogFleshRingAsset, Log, TEXT("    P0 Pos: %s"), *SP0.ToString());
			UE_LOG(LogFleshRingAsset, Log, TEXT("    P1 Pos: %s"), *SP1.ToString());
			UE_LOG(LogFleshRingAsset, Log, TEXT("    Computed (Bary): %s"), *ComputedPos.ToString());
			UE_LOG(LogFleshRingAsset, Log, TEXT("    Expected (Midpoint): %s"), *ExpectedMidpoint.ToString());
			UE_LOG(LogFleshRingAsset, Log, TEXT("    Distance P0-P1: %.2f"), FVector::Dist(SP0, SP1));
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Error, TEXT("    INVALID PARENT INDEX! P0=%u, P1=%u, P2=%u (SourceVertexCount=%d)"),
				DbgVD.ParentV0, DbgVD.ParentV1, DbgVD.ParentV2, SourceVertexCount);
		}
	}
	UE_LOG(LogFleshRingAsset, Log, TEXT("=============================================="));

	// ============================================
	// 4. Barycentric 보간으로 새 버텍스 데이터 생성
	// ============================================
	const int32 NewVertexCount = TopologyResult.VertexData.Num();
	TArray<FVector> NewPositions;
	TArray<FVector> NewNormals;
	TArray<FVector4> NewTangents;
	TArray<FVector2D> NewUVs;
	TArray<TArray<uint16>> NewBoneIndices;
	TArray<TArray<uint8>> NewBoneWeights;

	NewPositions.SetNum(NewVertexCount);
	NewNormals.SetNum(NewVertexCount);
	NewTangents.SetNum(NewVertexCount);
	NewUVs.SetNum(NewVertexCount);
	NewBoneIndices.SetNum(NewVertexCount);
	NewBoneWeights.SetNum(NewVertexCount);

	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FSubdivisionVertexData& VD = TopologyResult.VertexData[i];
		const float U = VD.BarycentricCoords.X;
		const float V = VD.BarycentricCoords.Y;
		const float W = VD.BarycentricCoords.Z;

		const uint32 P0 = FMath::Min(VD.ParentV0, (uint32)(SourceVertexCount - 1));
		const uint32 P1 = FMath::Min(VD.ParentV1, (uint32)(SourceVertexCount - 1));
		const uint32 P2 = FMath::Min(VD.ParentV2, (uint32)(SourceVertexCount - 1));

		// Position 보간
		NewPositions[i] = SourcePositions[P0] * U + SourcePositions[P1] * V + SourcePositions[P2] * W;

		// Normal 보간 및 정규화
		FVector InterpolatedNormal = SourceNormals[P0] * U + SourceNormals[P1] * V + SourceNormals[P2] * W;
		NewNormals[i] = InterpolatedNormal.GetSafeNormal();

		// Tangent 보간
		FVector4 InterpTangent = SourceTangents[P0] * U + SourceTangents[P1] * V + SourceTangents[P2] * W;
		FVector TangentDir = FVector(InterpTangent.X, InterpTangent.Y, InterpTangent.Z).GetSafeNormal();
		NewTangents[i] = FVector4(TangentDir.X, TangentDir.Y, TangentDir.Z, SourceTangents[P0].W);

		// UV 보간
		NewUVs[i] = SourceUVs[P0] * U + SourceUVs[P1] * V + SourceUVs[P2] * W;

		// Bone Weight 보간 (바이트 정밀도로 barycentric 보간)
		NewBoneIndices[i].SetNum(MaxBoneInfluences);
		NewBoneWeights[i].SetNum(MaxBoneInfluences);

		// 모든 부모의 bone influence를 수집
		TMap<uint16, float> BoneWeightMap;

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			// Parent 0 기여
			if (SourceBoneWeights[P0][j] > 0)
			{
				uint16 BoneIdx = SourceBoneIndices[P0][j];
				float Weight = (SourceBoneWeights[P0][j] / 255.0f) * U;
				BoneWeightMap.FindOrAdd(BoneIdx) += Weight;
			}
			// Parent 1 기여
			if (SourceBoneWeights[P1][j] > 0)
			{
				uint16 BoneIdx = SourceBoneIndices[P1][j];
				float Weight = (SourceBoneWeights[P1][j] / 255.0f) * V;
				BoneWeightMap.FindOrAdd(BoneIdx) += Weight;
			}
			// Parent 2 기여
			if (SourceBoneWeights[P2][j] > 0)
			{
				uint16 BoneIdx = SourceBoneIndices[P2][j];
				float Weight = (SourceBoneWeights[P2][j] / 255.0f) * W;
				BoneWeightMap.FindOrAdd(BoneIdx) += Weight;
			}
		}

		// 가장 큰 영향력 순으로 정렬하고 MaxBoneInfluences개 선택
		TArray<TPair<uint16, float>> SortedWeights;
		for (const auto& Pair : BoneWeightMap)
		{
			SortedWeights.Add(TPair<uint16, float>(Pair.Key, Pair.Value));
		}
		SortedWeights.Sort([](const TPair<uint16, float>& A, const TPair<uint16, float>& B)
		{
			return A.Value > B.Value;
		});

		// 정규화 및 할당
		float TotalWeight = 0.0f;
		for (int32 j = 0; j < FMath::Min(SortedWeights.Num(), MaxBoneInfluences); ++j)
		{
			TotalWeight += SortedWeights[j].Value;
		}

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (j < SortedWeights.Num() && TotalWeight > 0.0f)
			{
				NewBoneIndices[i][j] = SortedWeights[j].Key;
				NewBoneWeights[i][j] = FMath::Clamp<uint8>(
					FMath::RoundToInt((SortedWeights[j].Value / TotalWeight) * 255.0f), 0, 255);
			}
			else
			{
				NewBoneIndices[i][j] = 0;
				NewBoneWeights[i][j] = 0;
			}
		}
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("Interpolated %d new vertices with bone weights"), NewVertexCount);

	// ============================================
	// 5. 새 USkeletalMesh 생성 (소스 메시 복제 방식)
	// ============================================
	// 기존 SubdividedMesh는 함수 시작 부분에서 이미 안전하게 제거됨

	// 소스 메시를 복제하여 모든 내부 구조 상속 (MorphTarget, LOD 데이터 등)
	// ★ 고유한 이름 사용 (기존 메시가 GC 대기 중일 수 있으므로 이름 충돌 방지)
	FString MeshName = FString::Printf(TEXT("%s_Subdivided_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	SubdividedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, this, FName(*MeshName));

	UE_LOG(LogFleshRingAsset, Log, TEXT("DuplicateObject: %s"),
		SubdividedMesh ? *SubdividedMesh->GetFullName() : TEXT("FAILED"));

	if (!SubdividedMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: 소스 메시 복제 실패"));
		return;
	}

	// 복제된 메시의 기존 MeshDescription 제거
	if (SubdividedMesh->HasMeshDescription(0))
	{
		SubdividedMesh->ClearMeshDescription(0);
	}

	// ============================================
	// 6. Import Data 설정 및 빌드
	// ============================================
	// FSkeletalMeshImportData를 사용하여 메시 데이터 설정
	FSkeletalMeshImportData ImportData;

	// Points (버텍스 위치)
	ImportData.Points.SetNum(NewVertexCount);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		ImportData.Points[i] = FVector3f(NewPositions[i]);
	}

	// Wedges (버텍스 속성)
	const int32 NumWedges = TopologyResult.Indices.Num();
	ImportData.Wedges.SetNum(NumWedges);
	for (int32 i = 0; i < NumWedges; ++i)
	{
		SkeletalMeshImportData::FVertex& Wedge = ImportData.Wedges[i];
		int32 VertexIndex = TopologyResult.Indices[i];
		Wedge.VertexIndex = VertexIndex;
		Wedge.UVs[0] = FVector2f(NewUVs[VertexIndex]);
		Wedge.MatIndex = 0;
	}

	// Faces
	const int32 NumFaces = TopologyResult.Indices.Num() / 3;
	ImportData.Faces.SetNum(NumFaces);
	for (int32 i = 0; i < NumFaces; ++i)
	{
		SkeletalMeshImportData::FTriangle& Face = ImportData.Faces[i];
		Face.WedgeIndex[0] = i * 3 + 0;
		Face.WedgeIndex[1] = i * 3 + 1;
		Face.WedgeIndex[2] = i * 3 + 2;

		// 각 wedge의 TangentZ (Normal) 설정
		for (int32 j = 0; j < 3; ++j)
		{
			int32 VertexIndex = TopologyResult.Indices[i * 3 + j];
			Face.TangentZ[j] = FVector3f(NewNormals[VertexIndex]);
			Face.TangentX[j] = FVector3f(NewTangents[VertexIndex].X, NewTangents[VertexIndex].Y, NewTangents[VertexIndex].Z);
			Face.TangentY[j] = FVector3f(FVector::CrossProduct(NewNormals[VertexIndex],
				FVector(NewTangents[VertexIndex].X, NewTangents[VertexIndex].Y, NewTangents[VertexIndex].Z)) * NewTangents[VertexIndex].W);
		}
		Face.SmoothingGroups = 1;
		Face.MatIndex = 0;
	}

	// Influences (본 웨이트)
	ImportData.Influences.Empty();
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (NewBoneWeights[i][j] > 0)
			{
				SkeletalMeshImportData::FRawBoneInfluence Influence;
				Influence.VertexIndex = i;
				Influence.BoneIndex = NewBoneIndices[i][j];
				Influence.Weight = NewBoneWeights[i][j] / 255.0f;
				ImportData.Influences.Add(Influence);
			}
		}
	}

	// RefBonesBinary (원본과 동일하게 사용)
	const FReferenceSkeleton& RefSkel = SourceMesh->GetRefSkeleton();
	ImportData.RefBonesBinary.SetNum(RefSkel.GetRawBoneNum());
	for (int32 i = 0; i < RefSkel.GetRawBoneNum(); ++i)
	{
		SkeletalMeshImportData::FBone& Bone = ImportData.RefBonesBinary[i];
		Bone.Name = RefSkel.GetBoneName(i).ToString();
		Bone.ParentIndex = RefSkel.GetParentIndex(i);
		Bone.NumChildren = 0;  // 빌드 시 계산됨
		Bone.Flags = 0;
		const FTransform& BonePose = RefSkel.GetRefBonePose()[i];
		Bone.BonePos.Transform.SetLocation(FVector3f(BonePose.GetLocation()));
		Bone.BonePos.Transform.SetRotation(FQuat4f(BonePose.GetRotation()));
		Bone.BonePos.Length = 0.0f;
		Bone.BonePos.XSize = 1.0f;
		Bone.BonePos.YSize = 1.0f;
		Bone.BonePos.ZSize = 1.0f;
	}

	// Materials
	ImportData.Materials.SetNum(1);
	ImportData.Materials[0].MaterialImportName = TEXT("DefaultMaterial");
	ImportData.Materials[0].Material = nullptr;

	// NumTexCoords
	ImportData.NumTexCoords = 1;
	ImportData.MaxMaterialIndex = 0;
	ImportData.bHasNormals = true;
	ImportData.bHasTangents = true;
	ImportData.bHasVertexColors = false;

	// ============================================
	// 7. SkeletalMesh 빌드
	// ============================================
	// 복제된 메시는 이미 LODInfo와 머티리얼이 있으므로 별도 설정 불필요

	// ============================================
	// MeshDescription 생성 및 커밋
	// ============================================
	FMeshDescription MeshDescription;
	FSkeletalMeshAttributes MeshAttributes(MeshDescription);
	MeshAttributes.Register();

	// 버텍스 등록
	MeshDescription.ReserveNewVertices(NewVertexCount);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FVertexID VertexID = MeshDescription.CreateVertex();
		MeshDescription.GetVertexPositions()[VertexID] = FVector3f(NewPositions[i]);
	}

	// 폴리곤 그룹 (머티리얼 섹션) 생성 - MaterialIndex별로 그룹 생성
	MeshDescription.PolygonGroupAttributes().RegisterAttribute<FName>(
		MeshAttribute::PolygonGroup::ImportedMaterialSlotName);

	// 사용 중인 MaterialIndex 수집 및 유효성 검사
	const int32 NumMaterials = SourceMesh ? SourceMesh->GetMaterials().Num() : 1;
	TSet<int32> UsedMaterialIndices;
	for (int32 TriIdx = 0; TriIdx < NumFaces; ++TriIdx)
	{
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(TriIdx)
			? TopologyResult.TriangleMaterialIndices[TriIdx] : 0;
		// 유효한 범위로 클램핑
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		UsedMaterialIndices.Add(MatIdx);
	}

	// MaterialIndex 순서대로 PolygonGroup 생성 (섹션 순서 보장)
	TMap<int32, FPolygonGroupID> MaterialIndexToPolygonGroup;
	TArray<int32> SortedMaterialIndices = UsedMaterialIndices.Array();
	SortedMaterialIndices.Sort();

	for (int32 MatIdx : SortedMaterialIndices)
	{
		FPolygonGroupID GroupID = MeshDescription.CreatePolygonGroup();
		MaterialIndexToPolygonGroup.Add(MatIdx, GroupID);

		// 원본 메시의 정확한 머티리얼 슬롯 이름 사용
		FName MaterialSlotName = NAME_None;
		if (SourceMesh && SourceMesh->GetMaterials().IsValidIndex(MatIdx))
		{
			MaterialSlotName = SourceMesh->GetMaterials()[MatIdx].ImportedMaterialSlotName;
		}
		if (MaterialSlotName.IsNone())
		{
			MaterialSlotName = *FString::Printf(TEXT("Material_%d"), MatIdx);
		}

		MeshDescription.PolygonGroupAttributes().SetAttribute(
			GroupID, MeshAttribute::PolygonGroup::ImportedMaterialSlotName, 0, MaterialSlotName);

		UE_LOG(LogFleshRingAsset, Log, TEXT("PolygonGroup %d: MaterialIndex=%d, SlotName=%s"),
			GroupID.GetValue(), MatIdx, *MaterialSlotName.ToString());
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("Created %d polygon groups for %d materials"), MaterialIndexToPolygonGroup.Num(), NumMaterials);

	// 삼각형 등록
	TArray<FVertexInstanceID> VertexInstanceIDs;
	VertexInstanceIDs.Reserve(TopologyResult.Indices.Num());

	for (int32 i = 0; i < TopologyResult.Indices.Num(); ++i)
	{
		const uint32 VertexIndex = TopologyResult.Indices[i];
		const FVertexID VertexID(VertexIndex);
		const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);
		VertexInstanceIDs.Add(VertexInstanceID);

		// UV
		MeshAttributes.GetVertexInstanceUVs().Set(VertexInstanceID, 0, FVector2f(NewUVs[VertexIndex]));

		// Normal
		MeshAttributes.GetVertexInstanceNormals().Set(VertexInstanceID, FVector3f(NewNormals[VertexIndex]));

		// Tangent
		MeshAttributes.GetVertexInstanceTangents().Set(VertexInstanceID,
			FVector3f(NewTangents[VertexIndex].X, NewTangents[VertexIndex].Y, NewTangents[VertexIndex].Z));
		MeshAttributes.GetVertexInstanceBinormalSigns().Set(VertexInstanceID, NewTangents[VertexIndex].W);
	}

	// 삼각형을 폴리곤으로 등록 (각 삼각형의 MaterialIndex에 맞는 PolygonGroup에 할당)
	for (int32 i = 0; i < NumFaces; ++i)
	{
		TArray<FVertexInstanceID> TriangleVertexInstances;
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 0]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 1]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 2]);

		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(i)
			? TopologyResult.TriangleMaterialIndices[i] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);  // 유효 범위로 클램핑
		FPolygonGroupID* GroupID = MaterialIndexToPolygonGroup.Find(MatIdx);
		if (GroupID)
		{
			MeshDescription.CreatePolygon(*GroupID, TriangleVertexInstances);
		}
	}

	// SkinWeight 설정
	FSkinWeightsVertexAttributesRef SkinWeights = MeshAttributes.GetVertexSkinWeights();
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		FVertexID VertexID(i);
		TArray<UE::AnimationCore::FBoneWeight> BoneWeightArray;

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (NewBoneWeights[i][j] > 0)
			{
				UE::AnimationCore::FBoneWeight BW;
				BW.SetBoneIndex(NewBoneIndices[i][j]);
				BW.SetWeight(NewBoneWeights[i][j] / 255.0f);
				BoneWeightArray.Add(BW);
			}
		}

		SkinWeights.Set(VertexID, BoneWeightArray);
	}

	// MeshDescription을 SkeletalMesh에 저장
	UE_LOG(LogFleshRingAsset, Log, TEXT("Step 1: CreateMeshDescription..."));
	SubdividedMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

	// 기존 렌더 리소스 해제 (DuplicateObject로 복제된 데이터 제거)
	UE_LOG(LogFleshRingAsset, Log, TEXT("Step 2: ReleaseResources..."));
	SubdividedMesh->ReleaseResources();
	SubdividedMesh->ReleaseResourcesFence.Wait();

	// MeshDescription을 실제 LOD 모델 데이터로 커밋
	// CreateMeshDescription은 저장만 하고, CommitMeshDescription이 실제 변환 수행
	UE_LOG(LogFleshRingAsset, Log, TEXT("Step 3: CommitMeshDescription..."));
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false; // 나중에 MarkPackageDirty() 호출함
	SubdividedMesh->CommitMeshDescription(0, CommitParams);

	// 메시 빌드 (LOD 모델 → 렌더 데이터)
	UE_LOG(LogFleshRingAsset, Log, TEXT("Step 4: Build..."));
	SubdividedMesh->Build();

	// Build 결과 검증
	FSkeletalMeshRenderData* NewRenderData = SubdividedMesh->GetResourceForRendering();
	if (!NewRenderData || NewRenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("Build failed - no RenderData!"));
		SubdividedMesh->ConditionalBeginDestroy();
		SubdividedMesh = nullptr;
		return;
	}
	UE_LOG(LogFleshRingAsset, Log, TEXT("Build succeeded: %d LODs, %d vertices in LOD0"),
		NewRenderData->LODRenderData.Num(),
		NewRenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());

	// 렌더 리소스 초기화
	UE_LOG(LogFleshRingAsset, Log, TEXT("Step 5: InitResources..."));
	SubdividedMesh->InitResources();

	// 렌더 스레드가 리소스 초기화를 완료할 때까지 대기
	// (컴포넌트가 메시를 사용하기 전에 완료되어야 함)
	UE_LOG(LogFleshRingAsset, Log, TEXT("Step 6: FlushRenderingCommands..."));
	FlushRenderingCommands();
	UE_LOG(LogFleshRingAsset, Log, TEXT("Step 7: All steps completed successfully"));

	// 바운딩 박스 재계산
	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		BoundingBox += NewPositions[i];
	}
	SubdividedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	SubdividedMesh->CalculateExtendedBounds();

	// 파라미터 해시 저장 (재생성 판단용)
	SubdivisionParamsHash = CalculateSubdivisionParamsHash();
	MarkPackageDirty();

	UE_LOG(LogFleshRingAsset, Log,
		TEXT("GenerateSubdividedMesh 완료: %d vertices, %d triangles"),
		NewVertexCount, TopologyResult.SubdividedTriangleCount);

	// 이 에셋을 사용하는 컴포넌트들에게 변경 알림
	OnAssetChanged.Broadcast(this);

	// 델리게이트 바인딩이 안 된 컴포넌트들도 업데이트하기 위해 직접 검색
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (UWorld* World = Context.World())
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (UFleshRingComponent* Comp = It->FindComponentByClass<UFleshRingComponent>())
					{
						if (Comp->FleshRingAsset == this)
						{
							UE_LOG(LogFleshRingAsset, Log, TEXT("Forcing ApplyAsset on component: %s"), *Comp->GetName());
							Comp->ApplyAsset();
						}
					}
				}
			}
		}
	}
}

void UFleshRingAsset::ClearSubdividedMesh()
{
	if (SubdividedMesh)
	{
		// 먼저 컴포넌트들에게 알림하여 원본 메시로 전환하도록 함
		// SubdividedMesh를 파괴하기 전에 호출해야 함
		USkeletalMesh* MeshToDestroy = SubdividedMesh;
		SubdividedMesh = nullptr;
		SubdivisionParamsHash = 0;

		// Subdivision 비활성화 (프리뷰에서 재생성 방지)
		bEnableSubdivision = false;

		// 에디터 프리뷰 메시도 함께 제거 (원본 메시로 완전 복원)
		ClearPreviewMesh();

		// 이 에셋을 사용하는 컴포넌트들에게 변경 알림 (원본 메시로 복원)
		OnAssetChanged.Broadcast(this);

		// 델리게이트 바인딩이 안 된 컴포넌트들도 업데이트하기 위해 직접 검색
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UWorld* World = Context.World())
				{
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						if (UFleshRingComponent* Comp = It->FindComponentByClass<UFleshRingComponent>())
						{
							if (Comp->FleshRingAsset == this)
							{
								UE_LOG(LogFleshRingAsset, Log, TEXT("Forcing ApplyAsset on component (Clear): %s"), *Comp->GetName());
								Comp->ApplyAsset();
							}
						}
					}
				}
			}
		}

		// 렌더링 스레드가 메시 전환을 완료할 때까지 대기
		FlushRenderingCommands();

		// 이제 안전하게 메시 파괴
		MeshToDestroy->ConditionalBeginDestroy();

		MarkPackageDirty();

		UE_LOG(LogFleshRingAsset, Log, TEXT("ClearSubdividedMesh: Subdivided 메시 제거됨"));
	}
}

void UFleshRingAsset::GeneratePreviewMesh()
{
	// 기존 PreviewMesh가 있으면 먼저 제거
	if (PreviewSubdividedMesh)
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh: 기존 PreviewMesh 제거 중..."));

		// ★ 중요: 메시를 파괴하기 전에 사용 중인 컴포넌트들이 원본 메시로 전환하도록 알림
		USkeletalMesh* MeshToDestroy = PreviewSubdividedMesh;
		PreviewSubdividedMesh = nullptr;

		// 델리게이트 브로드캐스트하여 프리뷰 씬이 원본 메시로 전환하게 함
		// (SetFleshRingAsset에서 HasValidPreviewMesh() = false를 보고 원본 사용)
		OnAssetChanged.Broadcast(this);

		// 렌더 스레드가 메시 전환을 완료할 때까지 대기
		FlushRenderingCommands();

		// 이제 안전하게 이전 메시 파괴
		// GUID로 고유 이름 사용하므로 이름 충돌 없음 → CollectGarbage() 불필요
		MeshToDestroy->ConditionalBeginDestroy();

		UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh: 기존 PreviewMesh 제거 완료"));
	}

	if (!bEnableSubdivision)
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh: Subdivision 비활성화됨"));
		return;
	}

	if (TargetSkeletalMesh.IsNull())
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: TargetSkeletalMesh가 설정되지 않음"));
		return;
	}

	USkeletalMesh* SourceMesh = TargetSkeletalMesh.LoadSynchronous();
	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: SourceMesh 로드 실패"));
		return;
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh 시작: SourceMesh=%s, PreviewLevel=%d"),
		*SourceMesh->GetName(), PreviewSubdivisionLevel);

	// 1. 소스 메시 렌더 데이터 획득
	FSkeletalMeshRenderData* RenderData = SourceMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: RenderData 없음"));
		return;
	}

	const FSkeletalMeshLODRenderData& SourceLODData = RenderData->LODRenderData[0];
	const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// 2. 소스 버텍스 데이터 추출
	TArray<FVector> SourcePositions;
	TArray<FVector> SourceNormals;
	TArray<FVector4> SourceTangents;
	TArray<FVector2D> SourceUVs;

	SourcePositions.SetNum(SourceVertexCount);
	SourceNormals.SetNum(SourceVertexCount);
	SourceTangents.SetNum(SourceVertexCount);
	SourceUVs.SetNum(SourceVertexCount);

	for (uint32 i = 0; i < SourceVertexCount; ++i)
	{
		SourcePositions[i] = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
		SourceNormals[i] = FVector(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));
		FVector4f TangentX = SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
		SourceTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);
		SourceUVs[i] = FVector2D(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
	}

	// 인덱스 추출
	TArray<uint32> SourceIndices;
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = SourceLODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (IndexBuffer)
	{
		const int32 NumIndices = IndexBuffer->Num();
		SourceIndices.SetNum(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			SourceIndices[i] = IndexBuffer->Get(i);
		}
	}

	// 섹션별 머티리얼 인덱스 추출
	TArray<int32> SourceTriangleMaterialIndices;
	{
		const int32 NumTriangles = SourceIndices.Num() / 3;
		SourceTriangleMaterialIndices.SetNum(NumTriangles);
		for (const FSkelMeshRenderSection& Section : SourceLODData.RenderSections)
		{
			const int32 StartTriangle = Section.BaseIndex / 3;
			const int32 EndTriangle = StartTriangle + Section.NumTriangles;
			for (int32 TriIdx = StartTriangle; TriIdx < EndTriangle && TriIdx < NumTriangles; ++TriIdx)
			{
				SourceTriangleMaterialIndices[TriIdx] = Section.MaterialIndex;
			}
		}
	}

	// 본 웨이트 추출
	const int32 MaxBoneInfluences = SourceLODData.GetVertexBufferMaxBoneInfluences();
	TArray<TArray<uint16>> SourceBoneIndices;
	TArray<TArray<uint8>> SourceBoneWeights;
	SourceBoneIndices.SetNum(SourceVertexCount);
	SourceBoneWeights.SetNum(SourceVertexCount);

	// 버텍스별 섹션 인덱스 맵 생성
	TArray<int32> VertexToSectionIndex;
	VertexToSectionIndex.SetNum(SourceVertexCount);
	for (int32& SectionIdx : VertexToSectionIndex) { SectionIdx = INDEX_NONE; }
	for (int32 SectionIdx = 0; SectionIdx < SourceLODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = SourceLODData.RenderSections[SectionIdx];
		const int32 StartIndex = Section.BaseIndex;
		const int32 EndIndex = StartIndex + Section.NumTriangles * 3;
		for (int32 IdxPos = StartIndex; IdxPos < EndIndex; ++IdxPos)
		{
			uint32 VertexIdx = SourceIndices[IdxPos];
			if (VertexIdx < SourceVertexCount && VertexToSectionIndex[VertexIdx] == INDEX_NONE)
			{
				VertexToSectionIndex[VertexIdx] = SectionIdx;
			}
		}
	}

	const FSkinWeightVertexBuffer* SkinWeightBuffer = SourceLODData.GetSkinWeightVertexBuffer();
	if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
	{
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			SourceBoneIndices[i].SetNum(MaxBoneInfluences);
			SourceBoneWeights[i].SetNum(MaxBoneInfluences);
			int32 SectionIdx = VertexToSectionIndex[i];
			const TArray<FBoneIndexType>* BoneMap = nullptr;
			if (SectionIdx != INDEX_NONE && SectionIdx < SourceLODData.RenderSections.Num())
			{
				BoneMap = &SourceLODData.RenderSections[SectionIdx].BoneMap;
			}
			for (int32 j = 0; j < MaxBoneInfluences; ++j)
			{
				uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(i, j);
				uint8 Weight = SkinWeightBuffer->GetBoneWeight(i, j);
				uint16 GlobalBoneIdx = LocalBoneIdx;
				if (BoneMap && LocalBoneIdx < BoneMap->Num())
				{
					GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
				}
				SourceBoneIndices[i][j] = GlobalBoneIdx;
				SourceBoneWeights[i][j] = Weight;
			}
		}
	}

	// 3. 균일 Subdivision 프로세서 실행
	FFleshRingSubdivisionProcessor Processor;
	if (!Processor.SetSourceMesh(SourcePositions, SourceIndices, SourceUVs, SourceTriangleMaterialIndices))
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: SetSourceMesh 실패"));
		return;
	}

	FSubdivisionProcessorSettings Settings;
	Settings.MinEdgeLength = MinEdgeLength;
	Processor.SetSettings(Settings);

	FSubdivisionTopologyResult TopologyResult;
	if (!Processor.ProcessUniform(TopologyResult, PreviewSubdivisionLevel))
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: ProcessUniform 실패"));
		return;
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh: Subdivision 완료 - %d -> %d vertices"),
		TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount);

	// 4. 새 버텍스 데이터 보간
	const int32 NewVertexCount = TopologyResult.VertexData.Num();
	TArray<FVector> NewPositions;
	TArray<FVector> NewNormals;
	TArray<FVector4> NewTangents;
	TArray<FVector2D> NewUVs;
	TArray<TArray<uint16>> NewBoneIndices;
	TArray<TArray<uint8>> NewBoneWeights;

	NewPositions.SetNum(NewVertexCount);
	NewNormals.SetNum(NewVertexCount);
	NewTangents.SetNum(NewVertexCount);
	NewUVs.SetNum(NewVertexCount);
	NewBoneIndices.SetNum(NewVertexCount);
	NewBoneWeights.SetNum(NewVertexCount);

	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FSubdivisionVertexData& VD = TopologyResult.VertexData[i];
		const float U = VD.BarycentricCoords.X;
		const float V = VD.BarycentricCoords.Y;
		const float W = VD.BarycentricCoords.Z;

		const uint32 P0 = FMath::Min(VD.ParentV0, (uint32)(SourceVertexCount - 1));
		const uint32 P1 = FMath::Min(VD.ParentV1, (uint32)(SourceVertexCount - 1));
		const uint32 P2 = FMath::Min(VD.ParentV2, (uint32)(SourceVertexCount - 1));

		NewPositions[i] = SourcePositions[P0] * U + SourcePositions[P1] * V + SourcePositions[P2] * W;
		FVector InterpolatedNormal = SourceNormals[P0] * U + SourceNormals[P1] * V + SourceNormals[P2] * W;
		NewNormals[i] = InterpolatedNormal.GetSafeNormal();
		FVector4 InterpTangent = SourceTangents[P0] * U + SourceTangents[P1] * V + SourceTangents[P2] * W;
		FVector TangentDir = FVector(InterpTangent.X, InterpTangent.Y, InterpTangent.Z).GetSafeNormal();
		NewTangents[i] = FVector4(TangentDir.X, TangentDir.Y, TangentDir.Z, SourceTangents[P0].W);
		NewUVs[i] = SourceUVs[P0] * U + SourceUVs[P1] * V + SourceUVs[P2] * W;

		// Bone Weight 보간
		NewBoneIndices[i].SetNum(MaxBoneInfluences);
		NewBoneWeights[i].SetNum(MaxBoneInfluences);
		TMap<uint16, float> BoneWeightMap;
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (SourceBoneWeights[P0][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P0][j]) += (SourceBoneWeights[P0][j] / 255.0f) * U;
			if (SourceBoneWeights[P1][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P1][j]) += (SourceBoneWeights[P1][j] / 255.0f) * V;
			if (SourceBoneWeights[P2][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P2][j]) += (SourceBoneWeights[P2][j] / 255.0f) * W;
		}
		TArray<TPair<uint16, float>> SortedWeights;
		for (const auto& Pair : BoneWeightMap) { SortedWeights.Add(TPair<uint16, float>(Pair.Key, Pair.Value)); }
		SortedWeights.Sort([](const TPair<uint16, float>& A, const TPair<uint16, float>& B) { return A.Value > B.Value; });
		float TotalWeight = 0.0f;
		for (int32 j = 0; j < FMath::Min(SortedWeights.Num(), MaxBoneInfluences); ++j) { TotalWeight += SortedWeights[j].Value; }
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (j < SortedWeights.Num() && TotalWeight > 0.0f)
			{
				NewBoneIndices[i][j] = SortedWeights[j].Key;
				NewBoneWeights[i][j] = FMath::Clamp<uint8>(FMath::RoundToInt((SortedWeights[j].Value / TotalWeight) * 255.0f), 0, 255);
			}
			else
			{
				NewBoneIndices[i][j] = 0;
				NewBoneWeights[i][j] = 0;
			}
		}
	}

	// 5. 프리뷰용 USkeletalMesh 생성 (DuplicateObject로 ImportedModel 구조 유지)
	// ★ 고유한 이름 사용 (기존 메시가 GC 대기 중일 수 있으므로 이름 충돌 방지)
	FString MeshName = FString::Printf(TEXT("%s_Preview_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	PreviewSubdividedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, GetTransientPackage(), FName(*MeshName));

	UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh: DuplicateObject: %s"),
		PreviewSubdividedMesh ? *PreviewSubdividedMesh->GetFullName() : TEXT("FAILED"));

	if (!PreviewSubdividedMesh)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: 메시 복제 실패"));
		return;
	}

	// 복제된 메시의 기존 렌더 리소스 완전 해제 (렌더 스레드 동기화)
	FlushRenderingCommands();
	PreviewSubdividedMesh->ReleaseResources();
	PreviewSubdividedMesh->ReleaseResourcesFence.Wait();

	// 기존 MeshDescription 제거
	if (PreviewSubdividedMesh->HasMeshDescription(0))
	{
		PreviewSubdividedMesh->ClearMeshDescription(0);
	}

	// 6. MeshDescription 생성
	FMeshDescription MeshDescription;
	FSkeletalMeshAttributes MeshAttributes(MeshDescription);
	MeshAttributes.Register();

	MeshDescription.ReserveNewVertices(NewVertexCount);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FVertexID VertexID = MeshDescription.CreateVertex();
		MeshDescription.GetVertexPositions()[VertexID] = FVector3f(NewPositions[i]);
	}

	MeshDescription.PolygonGroupAttributes().RegisterAttribute<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
	const int32 NumMaterials = SourceMesh ? SourceMesh->GetMaterials().Num() : 1;
	const int32 NumFaces = TopologyResult.Indices.Num() / 3;

	TSet<int32> UsedMaterialIndices;
	for (int32 TriIdx = 0; TriIdx < NumFaces; ++TriIdx)
	{
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(TriIdx) ? TopologyResult.TriangleMaterialIndices[TriIdx] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		UsedMaterialIndices.Add(MatIdx);
	}

	TMap<int32, FPolygonGroupID> MaterialIndexToPolygonGroup;
	TArray<int32> SortedMaterialIndices = UsedMaterialIndices.Array();
	SortedMaterialIndices.Sort();
	for (int32 MatIdx : SortedMaterialIndices)
	{
		FPolygonGroupID GroupID = MeshDescription.CreatePolygonGroup();
		MaterialIndexToPolygonGroup.Add(MatIdx, GroupID);
		FName MaterialSlotName = NAME_None;
		if (SourceMesh && SourceMesh->GetMaterials().IsValidIndex(MatIdx))
		{
			MaterialSlotName = SourceMesh->GetMaterials()[MatIdx].ImportedMaterialSlotName;
		}
		if (MaterialSlotName.IsNone()) { MaterialSlotName = *FString::Printf(TEXT("Material_%d"), MatIdx); }
		MeshDescription.PolygonGroupAttributes().SetAttribute(GroupID, MeshAttribute::PolygonGroup::ImportedMaterialSlotName, 0, MaterialSlotName);
	}

	TArray<FVertexInstanceID> VertexInstanceIDs;
	VertexInstanceIDs.Reserve(TopologyResult.Indices.Num());
	for (int32 i = 0; i < TopologyResult.Indices.Num(); ++i)
	{
		const uint32 VertexIndex = TopologyResult.Indices[i];
		const FVertexID VertexID(VertexIndex);
		const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);
		VertexInstanceIDs.Add(VertexInstanceID);
		MeshAttributes.GetVertexInstanceUVs().Set(VertexInstanceID, 0, FVector2f(NewUVs[VertexIndex]));
		MeshAttributes.GetVertexInstanceNormals().Set(VertexInstanceID, FVector3f(NewNormals[VertexIndex]));
		MeshAttributes.GetVertexInstanceTangents().Set(VertexInstanceID, FVector3f(NewTangents[VertexIndex].X, NewTangents[VertexIndex].Y, NewTangents[VertexIndex].Z));
		MeshAttributes.GetVertexInstanceBinormalSigns().Set(VertexInstanceID, NewTangents[VertexIndex].W);
	}

	for (int32 i = 0; i < NumFaces; ++i)
	{
		TArray<FVertexInstanceID> TriangleVertexInstances;
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 0]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 1]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 2]);
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(i) ? TopologyResult.TriangleMaterialIndices[i] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		FPolygonGroupID* GroupID = MaterialIndexToPolygonGroup.Find(MatIdx);
		if (GroupID) { MeshDescription.CreatePolygon(*GroupID, TriangleVertexInstances); }
	}

	FSkinWeightsVertexAttributesRef SkinWeights = MeshAttributes.GetVertexSkinWeights();
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		FVertexID VertexID(i);
		TArray<UE::AnimationCore::FBoneWeight> BoneWeightArray;
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (NewBoneWeights[i][j] > 0)
			{
				UE::AnimationCore::FBoneWeight BW;
				BW.SetBoneIndex(NewBoneIndices[i][j]);
				BW.SetWeight(NewBoneWeights[i][j] / 255.0f);
				BoneWeightArray.Add(BW);
			}
		}
		SkinWeights.Set(VertexID, BoneWeightArray);
	}

	PreviewSubdividedMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

	// ReleaseResources는 위에서 이미 호출됨
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	PreviewSubdividedMesh->CommitMeshDescription(0, CommitParams);
	PreviewSubdividedMesh->Build();
	PreviewSubdividedMesh->InitResources();

	// 렌더 스레드가 리소스 초기화를 완료할 때까지 대기
	FlushRenderingCommands();

	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < NewVertexCount; ++i) { BoundingBox += NewPositions[i]; }
	PreviewSubdividedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	PreviewSubdividedMesh->CalculateExtendedBounds();

	UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh 완료: %d vertices, %d triangles (Transient)"),
		NewVertexCount, TopologyResult.SubdividedTriangleCount);
}

void UFleshRingAsset::ClearPreviewMesh()
{
	if (PreviewSubdividedMesh)
	{
		FlushRenderingCommands();
		PreviewSubdividedMesh->ConditionalBeginDestroy();
		PreviewSubdividedMesh = nullptr;
		UE_LOG(LogFleshRingAsset, Log, TEXT("ClearPreviewMesh: Preview 메시 제거됨"));
	}
}

bool UFleshRingAsset::NeedsPreviewMeshRegeneration() const
{
	if (!bEnableSubdivision)
	{
		return false;
	}
	return PreviewSubdividedMesh == nullptr;
}
#endif
