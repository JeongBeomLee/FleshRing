// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "FleshRingSubdivisionProcessor.h"

#if WITH_EDITOR
#include "UObject/ObjectSaveContext.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "Animation/Skeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "BoneWeights.h"
#include "RenderingThread.h"
#include "FleshRingComponent.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingAffectedVertices.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/TransactionObjectEvent.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingAsset, Log, All);

UFleshRingAsset::UFleshRingAsset()
{
}

void UFleshRingAsset::PostLoad()
{
	Super::PostLoad();

	// RingWidth → RingHeight 마이그레이션 (구 에셋 호환성)
	for (FFleshRingSettings& Ring : Rings)
	{
		if (Ring.RingWidth_DEPRECATED > 0.0f)
		{
			Ring.RingHeight = Ring.RingWidth_DEPRECATED;
			Ring.RingWidth_DEPRECATED = 0.0f;
			MarkPackageDirty();
		}
	}

	// 에셋 로드 시 에디터 선택 상태 초기화
	// (UPROPERTY()로 직렬화되지만, 로드 후에는 항상 초기화)
	EditorSelectedRingIndex = -1;
	EditorSelectionType = EFleshRingSelectionType::None;
}

#if WITH_EDITOR
void UFleshRingAsset::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	// 자동 베이크: Ring 설정 변경 또는 BakedMesh가 없을 때
	// 에디터 프리뷰 컴포넌트에서 GPU 변형 데이터를 Readback하여 BakedMesh 생성
	if (NeedsBakeRegeneration() && Rings.Num() > 0)
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("PreSave: Auto-bake triggered (NeedsBakeRegeneration=true)"));

		// 에디터 프리뷰 컴포넌트 찾기
		UFleshRingComponent* PreviewComponent = nullptr;
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UWorld* World = Context.World())
				{
					// 에디터 프리뷰 월드 검색
					if (World->WorldType == EWorldType::EditorPreview || World->WorldType == EWorldType::Editor)
					{
						for (TActorIterator<AActor> It(World); It; ++It)
						{
							if (UFleshRingComponent* Comp = It->FindComponentByClass<UFleshRingComponent>())
							{
								if (Comp->FleshRingAsset == this)
								{
									PreviewComponent = Comp;
									break;
								}
							}
						}
						if (PreviewComponent)
						{
							break;
						}
					}
				}
			}
		}

		if (PreviewComponent)
		{
			// GPU 작업 완료 대기
			FlushRenderingCommands();

			// 베이크 시도 (최대 30프레임 대기)
			// Subdivision OFF: 보통 몇 프레임 후 성공
			// Subdivision ON: 캐시가 이미 유효하면 성공, 아니면 스킵 (Bake 버튼 먼저 사용)
			bool bBakeSuccess = false;
			for (int32 FrameCount = 0; FrameCount < 30; ++FrameCount)
			{
				if (GenerateBakedMesh(PreviewComponent))
				{
					UE_LOG(LogFleshRingAsset, Log, TEXT("PreSave: Auto-bake completed successfully (frame %d)"), FrameCount);
					bBakeSuccess = true;
					break;
				}

				FlushRenderingCommands();
				FPlatformProcess::Sleep(0.016f);
			}

			if (!bBakeSuccess)
			{
				UE_LOG(LogFleshRingAsset, Log, TEXT("PreSave: Auto-bake skipped (cache not ready, use Bake button first)"));
			}
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("PreSave: Auto-bake skipped - no preview component found"));
		}
	}
}
#endif

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

bool UFleshRingAsset::IsRingNameUnique(FName Name, int32 ExcludeIndex) const
{
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		if (i != ExcludeIndex && Rings[i].RingName == Name)
		{
			return false;
		}
	}
	return true;
}

FName UFleshRingAsset::MakeUniqueRingName(FName BaseName, int32 ExcludeIndex) const
{
	// 이미 고유하면 그대로 반환
	if (IsRingNameUnique(BaseName, ExcludeIndex))
	{
		return BaseName;
	}

	// FName의 내장 넘버링 시스템 사용 (언리얼 소켓과 동일한 방식)
	int32 NewNumber = BaseName.GetNumber();
	while (!IsRingNameUnique(FName(BaseName, NewNumber), ExcludeIndex))
	{
		++NewNumber;
	}

	return FName(BaseName, NewNumber);
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

// =====================================
// Material Layer Utilities
// =====================================

void UFleshRingAsset::AutoPopulateMaterialLayers()
{
	// 타겟 메시가 필요함
	if (TargetSkeletalMesh.IsNull())
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("AutoPopulateMaterialLayers: TargetSkeletalMesh is not set"));
		return;
	}

	USkeletalMesh* Mesh = TargetSkeletalMesh.LoadSynchronous();
	if (!Mesh)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("AutoPopulateMaterialLayers: Failed to load TargetSkeletalMesh"));
		return;
	}

	// 기존 슬롯 인덱스 수집 (중복 방지)
	TSet<int32> ExistingSlotIndices;
	for (const FMaterialLayerMapping& Mapping : MaterialLayerMappings)
	{
		ExistingSlotIndices.Add(Mapping.MaterialSlotIndex);
	}

	// 머티리얼 슬롯 순회
	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	for (int32 SlotIndex = 0; SlotIndex < Materials.Num(); ++SlotIndex)
	{
		// 이미 매핑된 슬롯은 스킵
		if (ExistingSlotIndices.Contains(SlotIndex))
		{
			continue;
		}

		const FSkeletalMaterial& SkeletalMaterial = Materials[SlotIndex];
		FName SlotName = SkeletalMaterial.MaterialSlotName;
		FString MaterialName = SlotName.ToString();

		// 머티리얼 인스턴스 이름도 고려
		if (SkeletalMaterial.MaterialInterface)
		{
			MaterialName = SkeletalMaterial.MaterialInterface->GetName();
		}

		// 키워드 기반 레이어 타입 추측
		EFleshRingLayerType DetectedType = EFleshRingLayerType::Unknown;
		FString LowerName = MaterialName.ToLower();

		// Stocking 키워드 (우선)
		static const TArray<FString> StockingKeywords = {
			TEXT("stocking"), TEXT("tight"), TEXT("pantyhose"),
			TEXT("hosiery"), TEXT("nylon"), TEXT("sock"), TEXT("legging")
		};
		for (const FString& Keyword : StockingKeywords)
		{
			if (LowerName.Contains(Keyword))
			{
				DetectedType = EFleshRingLayerType::Stocking;
				break;
			}
		}

		// Underwear 키워드
		if (DetectedType == EFleshRingLayerType::Unknown)
		{
			static const TArray<FString> UnderwearKeywords = {
				TEXT("underwear"), TEXT("bra"), TEXT("panty"), TEXT("panties"),
				TEXT("lingerie"), TEXT("bikini"), TEXT("brief"), TEXT("thong")
			};
			for (const FString& Keyword : UnderwearKeywords)
			{
				if (LowerName.Contains(Keyword))
				{
					DetectedType = EFleshRingLayerType::Underwear;
					break;
				}
			}
		}

		// Outerwear 키워드
		if (DetectedType == EFleshRingLayerType::Unknown)
		{
			static const TArray<FString> OuterwearKeywords = {
				TEXT("cloth"), TEXT("dress"), TEXT("shirt"), TEXT("skirt"),
				TEXT("jacket"), TEXT("coat"), TEXT("pants"), TEXT("jeans")
			};
			for (const FString& Keyword : OuterwearKeywords)
			{
				if (LowerName.Contains(Keyword))
				{
					DetectedType = EFleshRingLayerType::Outerwear;
					break;
				}
			}
		}

		// Skin 키워드 (기본값으로 사용하기 좋음)
		if (DetectedType == EFleshRingLayerType::Unknown)
		{
			static const TArray<FString> SkinKeywords = {
				TEXT("skin"), TEXT("body"), TEXT("flesh"), TEXT("face"),
				TEXT("hand"), TEXT("leg"), TEXT("arm"), TEXT("foot"), TEXT("head")
			};
			for (const FString& Keyword : SkinKeywords)
			{
				if (LowerName.Contains(Keyword))
				{
					DetectedType = EFleshRingLayerType::Skin;
					break;
				}
			}
		}

		// 매핑 추가
		MaterialLayerMappings.Add(FMaterialLayerMapping(SlotIndex, SlotName, DetectedType));
	}

#if WITH_EDITOR
	// 에디터에서 변경 알림
	Modify();
#endif
}

EFleshRingLayerType UFleshRingAsset::GetLayerTypeForMaterialSlot(int32 MaterialSlotIndex) const
{
	for (const FMaterialLayerMapping& Mapping : MaterialLayerMappings)
	{
		if (Mapping.MaterialSlotIndex == MaterialSlotIndex)
		{
			return Mapping.LayerType;
		}
	}
	return EFleshRingLayerType::Unknown;
}

void UFleshRingAsset::ClearMaterialLayerMappings()
{
	MaterialLayerMappings.Empty();

#if WITH_EDITOR
	Modify();
#endif
}

bool UFleshRingAsset::NeedsSubdivisionRegeneration() const
{
	if (!SubdivisionSettings.bEnableSubdivision)
	{
		return false;
	}

	if (!SubdivisionSettings.SubdividedMesh)
	{
		return true;
	}

	return CalculateSubdivisionParamsHash() != SubdivisionSettings.SubdivisionParamsHash;
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
	Hash = HashCombine(Hash, GetTypeHash(SubdivisionSettings.bEnableSubdivision));
	Hash = HashCombine(Hash, GetTypeHash(SubdivisionSettings.MaxSubdivisionLevel));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(SubdivisionSettings.MinEdgeLength * 100)));

	// Ring settings (영향 영역 관련 - 서브디비전 대상 삼각형 선택에 영향)
	for (const FFleshRingSettings& Ring : Rings)
	{
		// 기본 Ring 식별
		Hash = HashCombine(Hash, GetTypeHash(Ring.BoneName.ToString()));

		// InfluenceMode (Auto vs Manual)
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Ring.InfluenceMode)));

		// Auto 모드: RingMesh 바운드 + 트랜스폼이 영역에 영향
		if (!Ring.RingMesh.IsNull())
		{
			Hash = HashCombine(Hash, GetTypeHash(Ring.RingMesh.ToSoftObjectPath().ToString()));
		}
		Hash = HashCombine(Hash, GetTypeHash(Ring.MeshOffset.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Ring.MeshRotation.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Ring.MeshScale.ToString()));

		// Manual 모드: Torus 파라미터가 영역에 영향
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingRadius * 10)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingHeight * 10)));
		Hash = HashCombine(Hash, GetTypeHash(Ring.RingOffset.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Ring.RingRotation.ToString()));

		// 영역 확장 파라미터 (PostProcess, Smoothing Volume)
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnablePostProcess));
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Ring.SmoothingVolumeMode)));
		Hash = HashCombine(Hash, GetTypeHash(Ring.MaxSmoothingHops));
	}

	return Hash;
}

#if WITH_EDITOR

// ============================================
// Subdivision 영역 계산용 헬퍼 함수들
// ============================================

namespace SubdivisionHelpers
{
	/** 위치를 정수 셀로 양자화 (UV Seam 용접용) */
	FORCEINLINE FIntVector QuantizePosition(const FVector& Position, float CellSize = 0.01f)
	{
		return FIntVector(
			FMath::FloorToInt(Position.X / CellSize),
			FMath::FloorToInt(Position.Y / CellSize),
			FMath::FloorToInt(Position.Z / CellSize)
		);
	}

	/**
	 * 위치 기반 버텍스 그룹화 (UV Seam 용접)
	 * 같은 3D 위치의 버텍스들을 그룹화하여 함께 처리
	 * @param Positions - 버텍스 위치 배열
	 * @param CellSize - 양자화 셀 크기 (cm), 이 범위 내의 버텍스는 같은 위치로 간주
	 * @return 양자화된 위치 → 버텍스 인덱스 배열 맵
	 */
	TMap<FIntVector, TArray<uint32>> BuildPositionGroups(const TArray<FVector>& Positions, float CellSize = 0.01f)
	{
		TMap<FIntVector, TArray<uint32>> PositionGroups;
		PositionGroups.Reserve(Positions.Num());

		for (int32 i = 0; i < Positions.Num(); ++i)
		{
			FIntVector Cell = QuantizePosition(Positions[i], CellSize);
			PositionGroups.FindOrAdd(Cell).Add(static_cast<uint32>(i));
		}

		return PositionGroups;
	}

	/**
	 * 버텍스 인접성 맵 빌드 (HopBased 확장용)
	 * 삼각형 인덱스에서 각 버텍스의 이웃 버텍스 목록 생성
	 * @param Indices - 삼각형 인덱스 배열 (3개씩 한 삼각형)
	 * @return 버텍스 인덱스 → 이웃 버텍스 인덱스 집합 맵
	 */
	TMap<uint32, TSet<uint32>> BuildAdjacencyMap(const TArray<uint32>& Indices)
	{
		TMap<uint32, TSet<uint32>> AdjacencyMap;

		const int32 NumTriangles = Indices.Num() / 3;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			const uint32 V0 = Indices[TriIdx * 3 + 0];
			const uint32 V1 = Indices[TriIdx * 3 + 1];
			const uint32 V2 = Indices[TriIdx * 3 + 2];

			// 양방향 연결
			AdjacencyMap.FindOrAdd(V0).Add(V1);
			AdjacencyMap.FindOrAdd(V0).Add(V2);
			AdjacencyMap.FindOrAdd(V1).Add(V0);
			AdjacencyMap.FindOrAdd(V1).Add(V2);
			AdjacencyMap.FindOrAdd(V2).Add(V0);
			AdjacencyMap.FindOrAdd(V2).Add(V1);
		}

		return AdjacencyMap;
	}

	/**
	 * 위치 기반 인접성 맵 확장 (UV Seam 처리)
	 * 같은 위치의 버텍스들이 서로의 이웃을 공유하도록 확장
	 * @param AdjacencyMap - 원본 인접성 맵 (수정됨)
	 * @param PositionGroups - 위치 기반 버텍스 그룹
	 */
	void ExpandAdjacencyForUVSeams(
		TMap<uint32, TSet<uint32>>& AdjacencyMap,
		const TMap<FIntVector, TArray<uint32>>& PositionGroups)
	{
		for (const auto& Group : PositionGroups)
		{
			const TArray<uint32>& Vertices = Group.Value;
			if (Vertices.Num() <= 1)
			{
				continue;
			}

			// 그룹 내 모든 버텍스의 이웃을 합집합으로 수집
			TSet<uint32> CombinedNeighbors;
			for (uint32 V : Vertices)
			{
				if (TSet<uint32>* Neighbors = AdjacencyMap.Find(V))
				{
					CombinedNeighbors.Append(*Neighbors);
				}
			}

			// 그룹 내 버텍스들은 이웃에서 제외
			for (uint32 V : Vertices)
			{
				CombinedNeighbors.Remove(V);
			}

			// 그룹 내 모든 버텍스에 합집합 이웃 적용
			for (uint32 V : Vertices)
			{
				AdjacencyMap.FindOrAdd(V) = CombinedNeighbors;
			}
		}
	}

	/**
	 * 선택된 버텍스에 UV Seam 중복 버텍스 추가
	 * @param SelectedVertices - 선택된 버텍스 집합 (수정됨)
	 * @param Positions - 버텍스 위치 배열
	 * @param PositionGroups - 위치 기반 버텍스 그룹
	 */
	void AddPositionDuplicates(
		TSet<uint32>& SelectedVertices,
		const TArray<FVector>& Positions,
		const TMap<FIntVector, TArray<uint32>>& PositionGroups,
		float CellSize = 0.01f)
	{
		TSet<uint32> Duplicates;

		for (uint32 V : SelectedVertices)
		{
			FIntVector Cell = QuantizePosition(Positions[V], CellSize);
			if (const TArray<uint32>* Group = PositionGroups.Find(Cell))
			{
				for (uint32 DupV : *Group)
				{
					if (!SelectedVertices.Contains(DupV))
					{
						Duplicates.Add(DupV);
					}
				}
			}
		}

		SelectedVertices.Append(Duplicates);
	}

	/**
	 * 본 체인을 따라 컴포넌트 스페이스 트랜스폼 계산
	 * @param BoneIndex - 본 인덱스
	 * @param RefSkeleton - 레퍼런스 스켈레톤
	 * @param RefBonePose - 레퍼런스 본 포즈
	 * @return 컴포넌트 스페이스 본 트랜스폼
	 */
	FTransform CalculateBoneTransform(
		int32 BoneIndex,
		const FReferenceSkeleton& RefSkeleton,
		const TArray<FTransform>& RefBonePose)
	{
		if (BoneIndex == INDEX_NONE || !RefBonePose.IsValidIndex(BoneIndex))
		{
			return FTransform::Identity;
		}

		FTransform BoneTransform = RefBonePose[BoneIndex];
		int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);

		while (ParentIndex != INDEX_NONE)
		{
			BoneTransform = BoneTransform * RefBonePose[ParentIndex];
			ParentIndex = RefSkeleton.GetParentIndex(ParentIndex);
		}

		return BoneTransform;
	}

	/**
	 * 기본 Affected 버텍스 선택 (Auto/Manual 모드)
	 * @param Ring - Ring 설정
	 * @param Positions - 버텍스 위치 배열
	 * @param BoneTransform - 본의 컴포넌트 스페이스 트랜스폼
	 * @param OutAffectedVertices - 출력: 영향받는 버텍스 인덱스 집합
	 * @param OutRingBounds - 출력: Ring 영역의 로컬 바운드 (Auto 모드에서만 유효)
	 * @param OutRingTransform - 출력: Ring 로컬 → 컴포넌트 트랜스폼
	 * @return 성공 여부
	 */
	bool SelectAffectedVertices(
		const FFleshRingSettings& Ring,
		const TArray<FVector>& Positions,
		const FTransform& BoneTransform,
		TSet<uint32>& OutAffectedVertices,
		FBox& OutRingBounds,
		FTransform& OutRingTransform)
	{
		OutAffectedVertices.Empty();
		OutRingBounds = FBox(EForceInit::ForceInit);
		OutRingTransform = FTransform::Identity;

		// 기본 마진: PostProcess OFF일 때도 최소한의 여유 확보
		// 변형 경계 영역의 polygon이 너무 거칠어지는 것 방지
		constexpr float DefaultZMargin = 3.0f;  // cm
		constexpr float DefaultRadialMargin = 1.5f;  // cm (Manual 모드용)

		if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto && !Ring.RingMesh.IsNull())
		{
			// =====================================
			// Auto 모드: SDF 바운드 기반
			// =====================================
			UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
			if (!RingMesh)
			{
				return false;
			}

			// RingMesh의 로컬 바운드
			FBox MeshBounds = RingMesh->GetBoundingBox();

			// Ring 로컬 → 컴포넌트 스페이스 트랜스폼
			FTransform MeshTransform(Ring.MeshRotation, Ring.MeshOffset);
			MeshTransform.SetScale3D(Ring.MeshScale);
			OutRingTransform = MeshTransform * BoneTransform;

			// SDFBoundsExpandX/Y + 기본 Z 마진 적용
			// Z 방향에도 기본 마진을 추가하여 상하 경계 영역 포함
			FVector Expand(Ring.SDFBoundsExpandX, Ring.SDFBoundsExpandY, DefaultZMargin);
			MeshBounds.Min -= Expand;
			MeshBounds.Max += Expand;

			OutRingBounds = MeshBounds;

			// 컴포넌트 → Ring 로컬 역변환
			FTransform ComponentToLocal = OutRingTransform.Inverse();

			// 바운드 내부 버텍스 선택
			for (int32 i = 0; i < Positions.Num(); ++i)
			{
				FVector LocalPos = ComponentToLocal.TransformPosition(Positions[i]);
				if (MeshBounds.IsInside(LocalPos))
				{
					OutAffectedVertices.Add(static_cast<uint32>(i));
				}
			}
		}
		else
		{
			// =====================================
			// Manual 모드: Torus 영역 기반
			// =====================================
			FVector LocalOffset = Ring.RingRotation.RotateVector(Ring.RingOffset);
			FVector Center = BoneTransform.GetLocation() + LocalOffset;
			FVector Axis = BoneTransform.GetRotation().RotateVector(
				Ring.RingRotation.RotateVector(FVector::UpVector));
			Axis.Normalize();

			// Ring 트랜스폼 설정 (BoundsExpand에서 사용)
			OutRingTransform = FTransform(Ring.RingRotation, LocalOffset) * BoneTransform;

			// Torus 파라미터 + 기본 마진
			// 마진을 추가하여 경계 영역의 버텍스도 포함
			const float InnerRadius = FMath::Max(0.0f, Ring.RingRadius - DefaultRadialMargin);
			const float OuterRadius = Ring.RingRadius + Ring.RingThickness + DefaultRadialMargin;
			const float HalfHeight = Ring.RingHeight * 0.5f + DefaultZMargin;

			// Torus 바운드 (마진 포함)
			OutRingBounds = FBox(
				FVector(-OuterRadius, -OuterRadius, -HalfHeight),
				FVector(OuterRadius, OuterRadius, HalfHeight)
			);

			// Torus 영역 내부 버텍스 선택
			for (int32 i = 0; i < Positions.Num(); ++i)
			{
				FVector ToVertex = Positions[i] - Center;

				// 축 방향 거리 (높이)
				float AxisDist = FVector::DotProduct(ToVertex, Axis);

				// 반경 방향 거리
				FVector RadialVec = ToVertex - Axis * AxisDist;
				float RadialDist = RadialVec.Size();

				// Torus 영역 내부인지 확인 (마진 포함)
				if (FMath::Abs(AxisDist) <= HalfHeight &&
					RadialDist >= InnerRadius &&
					RadialDist <= OuterRadius)
				{
					OutAffectedVertices.Add(static_cast<uint32>(i));
				}
			}
		}

		return OutAffectedVertices.Num() > 0;
	}

	/**
	 * BoundsExpand 모드: Z축 바운드 확장으로 추가 버텍스 선택
	 * @param Ring - Ring 설정
	 * @param Positions - 버텍스 위치 배열
	 * @param RingTransform - Ring 로컬 → 컴포넌트 트랜스폼
	 * @param OriginalBounds - 기존 Ring 바운드 (로컬 스페이스)
	 * @param SeedVertices - 기본 Affected 버텍스
	 * @param OutExpandedVertices - 출력: 확장된 버텍스 집합
	 */
	void ExpandByBounds(
		const FFleshRingSettings& Ring,
		const TArray<FVector>& Positions,
		const FTransform& RingTransform,
		const FBox& OriginalBounds,
		const TSet<uint32>& SeedVertices,
		TSet<uint32>& OutExpandedVertices)
	{
		OutExpandedVertices = SeedVertices;

		// Z축으로 바운드 확장
		FBox ExpandedBounds = OriginalBounds;
		ExpandedBounds.Min.Z -= Ring.SmoothingBoundsZBottom;
		ExpandedBounds.Max.Z += Ring.SmoothingBoundsZTop;

		// 컴포넌트 → Ring 로컬 역변환
		FTransform ComponentToLocal = RingTransform.Inverse();

		// 확장된 바운드 내부 버텍스 추가 선택
		for (int32 i = 0; i < Positions.Num(); ++i)
		{
			uint32 VertexIdx = static_cast<uint32>(i);
			if (SeedVertices.Contains(VertexIdx))
			{
				continue; // 이미 선택됨
			}

			FVector LocalPos = ComponentToLocal.TransformPosition(Positions[i]);
			if (ExpandedBounds.IsInside(LocalPos))
			{
				OutExpandedVertices.Add(VertexIdx);
			}
		}
	}

	/**
	 * HopBased 모드: BFS N-hop으로 버텍스 확장
	 * @param SeedVertices - 시드 버텍스 (기본 Affected)
	 * @param AdjacencyMap - 버텍스 인접성 맵
	 * @param MaxHops - 최대 홉 수
	 * @param OutExpandedVertices - 출력: 확장된 버텍스 집합
	 */
	void ExpandByHops(
		const TSet<uint32>& SeedVertices,
		const TMap<uint32, TSet<uint32>>& AdjacencyMap,
		int32 MaxHops,
		TSet<uint32>& OutExpandedVertices)
	{
		OutExpandedVertices = SeedVertices;

		TSet<uint32> CurrentFrontier = SeedVertices;

		for (int32 Hop = 0; Hop < MaxHops; ++Hop)
		{
			TSet<uint32> NextFrontier;

			for (uint32 V : CurrentFrontier)
			{
				if (const TSet<uint32>* Neighbors = AdjacencyMap.Find(V))
				{
					for (uint32 N : *Neighbors)
					{
						if (!OutExpandedVertices.Contains(N))
						{
							OutExpandedVertices.Add(N);
							NextFrontier.Add(N);
						}
					}
				}
			}

			CurrentFrontier = MoveTemp(NextFrontier);

			if (CurrentFrontier.Num() == 0)
			{
				break; // 더 이상 확장할 버텍스 없음
			}
		}
	}

	/**
	 * DI의 AffectedVertices를 위치 기반 매칭으로 원본 메시 인덱스로 변환
	 *
	 * PreviewComponent의 DeformerInstance는 SubdivisionSettings.PreviewSubdividedMesh(토폴로지가 다름)를 사용하므로
	 * 버텍스 인덱스가 원본 메시와 다름. 하지만 위치는 거의 동일하므로 위치 기반 매칭으로 변환.
	 *
	 * @param SourceComponent - DeformerInstance를 가진 FleshRingComponent (에디터 프리뷰)
	 * @param SourceMesh - 원본 SkeletalMesh (subdivision 전)
	 * @param OutVertexIndices - 출력: 원본 메시의 버텍스 인덱스 집합
	 * @return true if matching succeeded, false if fallback needed
	 */
	bool ExtractAffectedVerticesFromDI(
		const UFleshRingComponent* SourceComponent,
		const USkeletalMesh* SourceMesh,
		TSet<uint32>& OutVertexIndices)
	{
		OutVertexIndices.Empty();

		if (!SourceComponent || !SourceMesh)
		{
			return false;
		}

		// SkeletalMeshComponent 가져오기 (DI가 바인딩된 메시)
		USkeletalMeshComponent* SMC = const_cast<UFleshRingComponent*>(SourceComponent)->GetResolvedTargetMesh();
		if (!SMC)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No SkeletalMeshComponent"));
			return false;
		}

		// DeformerInstance 가져오기 (SkeletalMeshComponent에 바인딩됨)
		UMeshDeformerInstance* BaseDI = SMC->GetMeshDeformerInstance();
		if (!BaseDI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No MeshDeformerInstance"));
			return false;
		}

		// FleshRingDeformerInstance로 캐스트
		const UFleshRingDeformerInstance* DI = Cast<UFleshRingDeformerInstance>(BaseDI);
		if (!DI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: DeformerInstance is not FleshRingDeformerInstance"));
			return false;
		}

		// LOD 0의 AffectedVertices 데이터 가져오기
		const TArray<FRingAffectedData>* AllRingData = DI->GetAffectedRingDataForDebug(0);
		if (!AllRingData || AllRingData->Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No ring data in DI"));
			return false;
		}

		// DI가 사용 중인 메시 (PreviewSubdividedMesh일 수 있음)
		USkeletalMesh* DIMesh = SMC->GetSkeletalMeshAsset();
		if (!DIMesh)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No mesh in SMC"));
			return false;
		}

		// DI 메시가 원본 메시와 같으면 인덱스를 그대로 사용
		bool bSameMesh = (DIMesh == SourceMesh);

		// DI 메시의 버텍스 위치 추출 (위치 매칭용)
		FSkeletalMeshRenderData* DIRenderData = DIMesh->GetResourceForRendering();
		if (!DIRenderData || DIRenderData->LODRenderData.Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No DI mesh render data"));
			return false;
		}

		const FSkeletalMeshLODRenderData& DILODData = DIRenderData->LODRenderData[0];
		const uint32 DIVertexCount = DILODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		// 원본 메시의 버텍스 위치 추출
		FSkeletalMeshRenderData* SourceRenderData = SourceMesh->GetResourceForRendering();
		if (!SourceRenderData || SourceRenderData->LODRenderData.Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedVerticesFromDI: No source mesh render data"));
			return false;
		}

		const FSkeletalMeshLODRenderData& SourceLODData = SourceRenderData->LODRenderData[0];
		const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		// DI의 모든 Ring에서 영향받는 버텍스 인덱스 수집
		TSet<uint32> DIAffectedIndices;
		for (const FRingAffectedData& RingData : *AllRingData)
		{
			// Tightness 영역 (PackedIndices)
			for (uint32 Idx : RingData.PackedIndices)
			{
				if (Idx < DIVertexCount)
				{
					DIAffectedIndices.Add(Idx);
				}
			}

			// PostProcessing 영역 (Z 확장)
			for (uint32 Idx : RingData.PostProcessingIndices)
			{
				if (Idx < DIVertexCount)
				{
					DIAffectedIndices.Add(Idx);
				}
			}
		}

		if (DIAffectedIndices.Num() == 0)
		{
			return false;
		}

		// 메시가 같으면 인덱스 그대로 사용
		if (bSameMesh)
		{
			OutVertexIndices = MoveTemp(DIAffectedIndices);
			return true;
		}

		// 메시가 다르면 위치 기반 매칭 수행

		// 1. DI 영향 버텍스들의 위치 추출
		TArray<FVector> DIAffectedPositions;
		DIAffectedPositions.Reserve(DIAffectedIndices.Num());
		for (uint32 DIIdx : DIAffectedIndices)
		{
			FVector Pos = FVector(DILODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(DIIdx));
			DIAffectedPositions.Add(Pos);
		}

		// 2. 원본 메시 버텍스들의 공간 해시 빌드 (위치 → 인덱스 매핑)
		// Grid 크기: 0.1cm (매우 정밀)
		constexpr float GridSize = 0.1f;
		constexpr float MatchTolerance = 0.5f;  // 0.5cm 이내면 매칭

		TMap<FIntVector, TArray<uint32>> SourcePositionHash;
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			FVector Pos = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
			FIntVector GridKey(
				FMath::FloorToInt(Pos.X / GridSize),
				FMath::FloorToInt(Pos.Y / GridSize),
				FMath::FloorToInt(Pos.Z / GridSize)
			);
			SourcePositionHash.FindOrAdd(GridKey).Add(i);
		}

		// 3. 각 DI 영향 위치에 대해 원본 메시에서 가장 가까운 버텍스 찾기
		int32 MatchedCount = 0;
		for (const FVector& DIPos : DIAffectedPositions)
		{
			FIntVector CenterKey(
				FMath::FloorToInt(DIPos.X / GridSize),
				FMath::FloorToInt(DIPos.Y / GridSize),
				FMath::FloorToInt(DIPos.Z / GridSize)
			);

			float BestDistSq = MatchTolerance * MatchTolerance;
			int32 BestSourceIdx = INDEX_NONE;

			// 27-cell 이웃 탐색 (3x3x3)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					for (int32 dz = -1; dz <= 1; ++dz)
					{
						FIntVector NeighborKey = CenterKey + FIntVector(dx, dy, dz);
						if (const TArray<uint32>* Indices = SourcePositionHash.Find(NeighborKey))
						{
							for (uint32 SourceIdx : *Indices)
							{
								FVector SourcePos = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(SourceIdx));
								float DistSq = FVector::DistSquared(DIPos, SourcePos);
								if (DistSq < BestDistSq)
								{
									BestDistSq = DistSq;
									BestSourceIdx = SourceIdx;
								}
							}
						}
					}
				}
			}

			if (BestSourceIdx != INDEX_NONE)
			{
				OutVertexIndices.Add(static_cast<uint32>(BestSourceIdx));
				MatchedCount++;
			}
		}

		return OutVertexIndices.Num() > 0;
	}

	/**
	 * 점에서 삼각형까지의 최단 거리 제곱 계산
	 * @param Point - 검사할 점
	 * @param V0, V1, V2 - 삼각형 버텍스들
	 * @return 최단 거리 제곱
	 */
	float PointToTriangleDistSq(const FVector& Point, const FVector& V0, const FVector& V1, const FVector& V2)
	{
		// 삼각형 평면에 점 투영
		FVector Edge0 = V1 - V0;
		FVector Edge1 = V2 - V0;
		FVector Normal = FVector::CrossProduct(Edge0, Edge1);
		float NormalLenSq = Normal.SizeSquared();

		if (NormalLenSq < SMALL_NUMBER)
		{
			// Degenerate 삼각형
			return FLT_MAX;
		}

		Normal /= FMath::Sqrt(NormalLenSq);

		// 평면까지 거리
		FVector ToPoint = Point - V0;
		float PlaneDist = FVector::DotProduct(ToPoint, Normal);
		FVector Projected = Point - Normal * PlaneDist;

		// Barycentric 좌표 계산
		FVector V0ToP = Projected - V0;
		float D00 = FVector::DotProduct(Edge0, Edge0);
		float D01 = FVector::DotProduct(Edge0, Edge1);
		float D11 = FVector::DotProduct(Edge1, Edge1);
		float D20 = FVector::DotProduct(V0ToP, Edge0);
		float D21 = FVector::DotProduct(V0ToP, Edge1);

		float Denom = D00 * D11 - D01 * D01;
		if (FMath::Abs(Denom) < SMALL_NUMBER)
		{
			return FLT_MAX;
		}

		float V = (D11 * D20 - D01 * D21) / Denom;
		float W = (D00 * D21 - D01 * D20) / Denom;
		float U = 1.0f - V - W;

		// 삼각형 내부인지 확인
		if (U >= 0.0f && V >= 0.0f && W >= 0.0f)
		{
			// 내부: 평면까지 거리만 반환
			return PlaneDist * PlaneDist;
		}

		// 외부: 가장 가까운 엣지/버텍스까지 거리
		auto PointToSegmentDistSq = [](const FVector& P, const FVector& A, const FVector& B) -> float
		{
			FVector AB = B - A;
			FVector AP = P - A;
			float T = FVector::DotProduct(AP, AB) / FMath::Max(FVector::DotProduct(AB, AB), SMALL_NUMBER);
			T = FMath::Clamp(T, 0.0f, 1.0f);
			FVector Closest = A + AB * T;
			return FVector::DistSquared(P, Closest);
		};

		float D0 = PointToSegmentDistSq(Point, V0, V1);
		float D1 = PointToSegmentDistSq(Point, V1, V2);
		float D2 = PointToSegmentDistSq(Point, V2, V0);

		return FMath::Min3(D0, D1, D2);
	}

	/**
	 * DI의 AffectedVertices 위치들이 속한 원본 메시 삼각형 찾기
	 *
	 * PreviewSubdividedMesh의 AffectedVertices 위치들이
	 * 원본 메시의 어느 삼각형 내부/근처에 있는지 찾아서 반환
	 *
	 * @param SourceComponent - DeformerInstance를 가진 FleshRingComponent
	 * @param SourceMesh - 원본 SkeletalMesh (subdivision 전)
	 * @param SourcePositions - 원본 메시 버텍스 위치 배열
	 * @param SourceIndices - 원본 메시 인덱스 배열
	 * @param OutTriangleIndices - 출력: 영향받는 삼각형 인덱스 집합
	 * @return true if succeeded
	 */
	bool ExtractAffectedTrianglesFromDI(
		const UFleshRingComponent* SourceComponent,
		const USkeletalMesh* SourceMesh,
		const TArray<FVector>& SourcePositions,
		const TArray<uint32>& SourceIndices,
		TSet<int32>& OutTriangleIndices)
	{
		OutTriangleIndices.Empty();

		if (!SourceComponent || !SourceMesh)
		{
			return false;
		}

		// SkeletalMeshComponent 가져오기
		USkeletalMeshComponent* SMC = const_cast<UFleshRingComponent*>(SourceComponent)->GetResolvedTargetMesh();
		if (!SMC)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No SkeletalMeshComponent"));
			return false;
		}

		// DeformerInstance 가져오기
		UMeshDeformerInstance* BaseDI = SMC->GetMeshDeformerInstance();
		if (!BaseDI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No MeshDeformerInstance"));
			return false;
		}

		const UFleshRingDeformerInstance* DI = Cast<UFleshRingDeformerInstance>(BaseDI);
		if (!DI)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: Not FleshRingDeformerInstance"));
			return false;
		}

		// LOD 0의 AffectedVertices 데이터
		const TArray<FRingAffectedData>* AllRingData = DI->GetAffectedRingDataForDebug(0);
		if (!AllRingData || AllRingData->Num() == 0)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No ring data"));
			return false;
		}

		// DI가 사용 중인 메시 (SubdivisionSettings.PreviewSubdividedMesh)
		USkeletalMesh* DIMesh = SMC->GetSkeletalMeshAsset();
		if (!DIMesh)
		{
			return false;
		}

		FSkeletalMeshRenderData* DIRenderData = DIMesh->GetResourceForRendering();
		if (!DIRenderData || DIRenderData->LODRenderData.Num() == 0)
		{
			return false;
		}

		const FSkeletalMeshLODRenderData& DILODData = DIRenderData->LODRenderData[0];
		const uint32 DIVertexCount = DILODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		// FleshRingAsset에서 Ring 설정 가져오기
		const UFleshRingAsset* Asset = SourceComponent->FleshRingAsset;
		if (!Asset)
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("ExtractAffectedTrianglesFromDI: No FleshRingAsset"));
			return false;
		}

		// DI의 AffectedVertices 인덱스 수집
		// Ring 설정에 따라 조건부 수집:
		// - bEnablePostProcess == false → PackedIndices만
		// - bEnablePostProcess == true && BoundsExpand → PackedIndices + PostProcessingIndices
		// - bEnablePostProcess == true && HopBased → PackedIndices + ExtendedSmoothingIndices
		TSet<uint32> DIAffectedIndices;
		const int32 NumRings = FMath::Min(AllRingData->Num(), Asset->Rings.Num());

		for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
		{
			const FRingAffectedData& RingData = (*AllRingData)[RingIdx];
			const FFleshRingSettings& RingSettings = Asset->Rings[RingIdx];

			// 1. 기본 영역 (Tightness 대상) - 항상 수집
			for (uint32 Idx : RingData.PackedIndices)
			{
				if (Idx < DIVertexCount) DIAffectedIndices.Add(Idx);
			}

			// 2. PostProcess가 켜져있을 때만 확장 영역 수집
			if (RingSettings.bEnablePostProcess)
			{
				if (RingSettings.SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand)
				{
					// BoundsExpand 모드: PostProcessingIndices (Z 확장)
					for (uint32 Idx : RingData.PostProcessingIndices)
					{
						if (Idx < DIVertexCount) DIAffectedIndices.Add(Idx);
					}
				}
				else // HopBased
				{
					// HopBased 모드: ExtendedSmoothingIndices (N-hop 확장)
					for (uint32 Idx : RingData.ExtendedSmoothingIndices)
					{
						if (Idx < DIVertexCount) DIAffectedIndices.Add(Idx);
					}
				}
			}
		}

		if (DIAffectedIndices.Num() == 0)
		{
			return false;
		}

		// AffectedVertices의 위치들 추출
		TArray<FVector> AffectedPositions;
		AffectedPositions.Reserve(DIAffectedIndices.Num());
		for (uint32 DIIdx : DIAffectedIndices)
		{
			FVector Pos = FVector(DILODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(DIIdx));
			AffectedPositions.Add(Pos);
		}

		// ============================================
		// 원본 메시 삼각형 공간 해시 빌드
		// ============================================
		const int32 NumTriangles = SourceIndices.Num() / 3;
		constexpr float GridSize = 5.0f;  // 5cm 그리드

		// 삼각형 AABB → 그리드 셀 매핑
		TMap<FIntVector, TArray<int32>> TriangleSpatialHash;

		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			const FVector& V0 = SourcePositions[SourceIndices[TriIdx * 3 + 0]];
			const FVector& V1 = SourcePositions[SourceIndices[TriIdx * 3 + 1]];
			const FVector& V2 = SourcePositions[SourceIndices[TriIdx * 3 + 2]];

			// 삼각형 AABB
			FVector MinBound = V0.ComponentMin(V1.ComponentMin(V2));
			FVector MaxBound = V0.ComponentMax(V1.ComponentMax(V2));

			// AABB가 걸치는 모든 그리드 셀에 등록
			FIntVector MinCell(
				FMath::FloorToInt(MinBound.X / GridSize),
				FMath::FloorToInt(MinBound.Y / GridSize),
				FMath::FloorToInt(MinBound.Z / GridSize)
			);
			FIntVector MaxCell(
				FMath::FloorToInt(MaxBound.X / GridSize),
				FMath::FloorToInt(MaxBound.Y / GridSize),
				FMath::FloorToInt(MaxBound.Z / GridSize)
			);

			for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
			{
				for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
				{
					for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
					{
						TriangleSpatialHash.FindOrAdd(FIntVector(X, Y, Z)).Add(TriIdx);
					}
				}
			}
		}

		// ============================================
		// 각 AffectedPosition이 속한 삼각형 찾기
		// ============================================
		constexpr float MaxDistSq = 2.0f * 2.0f;  // 2cm 이내

		for (const FVector& Pos : AffectedPositions)
		{
			FIntVector CellKey(
				FMath::FloorToInt(Pos.X / GridSize),
				FMath::FloorToInt(Pos.Y / GridSize),
				FMath::FloorToInt(Pos.Z / GridSize)
			);

			float BestDistSq = MaxDistSq;
			int32 BestTriIdx = INDEX_NONE;

			// 현재 셀 + 이웃 셀 탐색 (3x3x3)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					for (int32 dz = -1; dz <= 1; ++dz)
					{
						FIntVector NeighborKey = CellKey + FIntVector(dx, dy, dz);
						if (const TArray<int32>* TriIndices = TriangleSpatialHash.Find(NeighborKey))
						{
							for (int32 TriIdx : *TriIndices)
							{
								const FVector& V0 = SourcePositions[SourceIndices[TriIdx * 3 + 0]];
								const FVector& V1 = SourcePositions[SourceIndices[TriIdx * 3 + 1]];
								const FVector& V2 = SourcePositions[SourceIndices[TriIdx * 3 + 2]];

								float DistSq = PointToTriangleDistSq(Pos, V0, V1, V2);
								if (DistSq < BestDistSq)
								{
									BestDistSq = DistSq;
									BestTriIdx = TriIdx;
								}
							}
						}
					}
				}
			}

			if (BestTriIdx != INDEX_NONE)
			{
				OutTriangleIndices.Add(BestTriIdx);
			}
		}

		return OutTriangleIndices.Num() > 0;
	}

} // namespace SubdivisionHelpers

// ============================================
// UFleshRingAsset 에디터 전용 함수들
// ============================================

void UFleshRingAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// EulerRotation 변경 시 FQuat 동기화
	for (FFleshRingSettings& Ring : Rings)
	{
		Ring.RingRotation = Ring.RingEulerRotation.Quaternion();
		Ring.MeshRotation = Ring.MeshEulerRotation.Quaternion();
	}

	// RingName 고유성 보장 (빈 이름 및 중복 이름 처리)
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		FName& CurrentName = Rings[i].RingName;

		// 1. 빈 이름이면 기본 이름 설정
		if (CurrentName.IsNone())
		{
			CurrentName = FName(*FString::Printf(TEXT("FleshRing_%d"), i));
		}

		// 2. 중복 이름 확인 (이전 인덱스들과 비교)
		bool bIsDuplicate = false;
		for (int32 j = 0; j < i; ++j)
		{
			if (Rings[j].RingName == CurrentName)
			{
				bIsDuplicate = true;
				break;
			}
		}

		// 3. 중복이면 MakeUniqueRingName 사용하여 고유한 이름 생성
		if (bIsDuplicate)
		{
			CurrentName = MakeUniqueRingName(CurrentName, i);
		}
	}

	// 에셋이 수정되었음을 표시
	MarkPackageDirty();

	// 전체 리프레시가 필요한 변경인지 확인
	bool bNeedsFullRefresh = false;

	// 배열 구조 변경 시 전체 갱신 + 캐시 무효화
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate ||
		PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayMove)
	{
		// ★ 링 추가/삭제 시 프리뷰 메시 캐시 무효화
		InvalidatePreviewMeshCache();
		bNeedsFullRefresh = true;
	}

	// 특정 프로퍼티 변경 시 전체 갱신
	if (PropertyChangedEvent.Property)
	{
		FName PropName = PropertyChangedEvent.Property->GetFName();

		if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, bEnableSubdivision) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingMesh) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode))
		{
			bNeedsFullRefresh = true;

			// ★ BoneName 변경 시 프리뷰 메시 캐시 무효화 (본 영역이 달라지므로)
			if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName))
			{
				InvalidatePreviewMeshCache();
			}
		}

		// ★ Manual 모드 Ring 파라미터 변경 시 디버그 시각화 갱신
		if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness) ||
			PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingHeight))
		{
			bNeedsFullRefresh = true;
		}

		// ★ Preview subdivision 파라미터 변경 시 캐시 무효화
		if (PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewSubdivisionLevel) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneHopCount) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneWeightThreshold) ||
			PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MinEdgeLength))
		{
			InvalidatePreviewMeshCache();
		}

		// ProceduralBand 관련 프로퍼티 변경 감지
		// MemberProperty 체인을 확인하여 ProceduralBand 하위 프로퍼티인지 검사
		bool bIsProceduralBandProperty = false;

		// 직접 프로퍼티 이름 체크 (VirtualBand 관련)
		if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, ProceduralBand) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSettings, MidUpperRadius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSettings, MidLowerRadius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSettings, BandHeight) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSettings, BandThickness) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSettings, Upper) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSettings, Lower) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSettings, RadialSegments) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSection, Radius) ||
			PropName == GET_MEMBER_NAME_CHECKED(FProceduralBandSection, Height))
		{
			bIsProceduralBandProperty = true;
		}

		// MemberProperty 체인에서 ProceduralBand 찾기
		if (!bIsProceduralBandProperty && PropertyChangedEvent.MemberProperty)
		{
			FName MemberName = PropertyChangedEvent.MemberProperty->GetFName();
			if (MemberName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, ProceduralBand))
			{
				bIsProceduralBandProperty = true;
			}
		}

		if (bIsProceduralBandProperty)
		{
			bNeedsFullRefresh = true;
		}

		// TargetSkeletalMesh 변경 시 Subdivision 메시들 클리어 (새 메시로 재생성 필요)
		if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh))
		{
			if (SubdivisionSettings.PreviewSubdividedMesh)
			{
				ClearPreviewMesh();
			}
			// SubdividedMesh도 클리어 (소스 메시가 변경되었으므로 재생성 필요)
			if (SubdivisionSettings.SubdividedMesh)
			{
				ClearSubdividedMesh();
				// ClearSubdividedMesh()가 OnAssetChanged.Broadcast()를 호출하므로 중복 방지
				bNeedsFullRefresh = false;
			}
		}

		// bEnableSubdivision이 false로 변경되면 SubdividedMesh도 정리
		// (상태 불일치로 인한 크래시 방지)
		if (PropName == GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, bEnableSubdivision))
		{
			if (!SubdivisionSettings.bEnableSubdivision && SubdivisionSettings.SubdividedMesh)
			{
				// ClearSubdividedMesh() 내부에서 OnAssetChanged.Broadcast() 호출함
				ClearSubdividedMesh();
				// 중복 브로드캐스트 방지
				bNeedsFullRefresh = false;
			}
		}

		// 트랜스폼 관련 프로퍼티 (Offset, Rotation, Scale, Radius, Strength, Falloff 등)는
		// 전체 갱신 불필요 - 경량 업데이트로 처리
	}

	// 구조적 변경 시에만 전체 리프레시 브로드캐스트
	// ProceduralBand 프로퍼티는 드래그 끝날 때(ValueSet)만 갱신 (Interactive 제외)
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

	// SubdividedMesh는 Undo/Redo 후 무조건 클리어
	// (Undo로 dangling pointer가 복원되거나 TargetMesh와 불일치할 수 있음)
	// 재생성은 사용자가 GenerateSubdividedMesh 버튼으로 명시적으로 해야 함
	if (SubdivisionSettings.SubdividedMesh)
	{
		SubdivisionSettings.SubdividedMesh = nullptr;
	}

	// ★ PreviewSubdividedMesh는 Transient - Undo/Redo로 영향 없음
	// 구조가 바뀌었는지 Hash로 판단하여 필요할 때만 재생성
	const uint32 CurrentHash = CalculatePreviewBoneConfigHash();
	const bool bStructureChanged = (SubdivisionSettings.CachedPreviewBoneConfigHash != CurrentHash);

	if (bStructureChanged)
	{
		// 구조 변경됨 (Ring 추가/삭제, BoneName 변경 등) - 재생성 필요
		if (SubdivisionSettings.PreviewSubdividedMesh)
		{
			SubdivisionSettings.PreviewSubdividedMesh = nullptr;
		}
		InvalidatePreviewMeshCache();
	}

	// 에셋 변경 알림 (Deformer 파라미터 갱신 등)
	OnAssetChanged.Broadcast(this);
}

void UFleshRingAsset::GenerateSubdividedMesh(UFleshRingComponent* SourceComponent)
{
	// SourceComponent가 있으면 DeformerInstance의 AffectedVertices 데이터를 활용
	// DI가 이미 정확하게 계산한 영역을 위치 기반 매칭으로 원본 메시 인덱스로 변환
	// SourceComponent가 없으면 폴백으로 원본 메시 기반 직접 계산

	// 이전 SubdividedMesh가 있으면 먼저 제거 (같은 이름 충돌 방지)
	if (SubdivisionSettings.SubdividedMesh)
	{
		// ★ 즉시 파괴하지 않고 포인터만 null로 설정 (렌더 스레드 안전)
		// GC가 안전하게 정리하게 함
		SubdivisionSettings.SubdividedMesh = nullptr;

		// Note: OnAssetChanged.Broadcast() 호출 안 함
		// SubdividedMesh는 런타임용이고, 프리뷰는 PreviewSubdividedMesh를 사용
		// 브로드캐스트 시 프리뷰 DeformerInstance가 재초기화되어 변형 데이터 손실됨

		// 월드의 FleshRingComponent들만 직접 업데이트 (프리뷰 제외)
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
								// ApplyAsset()이 SubdivisionSettings.SubdividedMesh == nullptr을 보고 원본 메시로 전환
								Comp->ApplyAsset();
							}
						}
					}
				}
			}
		}
	}

	if (!SubdivisionSettings.bEnableSubdivision)
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
	Settings.MaxSubdivisionLevel = SubdivisionSettings.MaxSubdivisionLevel;
	Settings.MinEdgeLength = SubdivisionSettings.MinEdgeLength;
	Processor.SetSettings(Settings);

	// ★ 모든 Ring에 대해 파라미터 설정
	const USkeleton* Skeleton = SourceMesh->GetSkeleton();
	const FReferenceSkeleton& RefSkeleton = SourceMesh->GetRefSkeleton();
	const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

	Processor.ClearRingParams();

	for (int32 RingIdx = 0; RingIdx < Rings.Num(); ++RingIdx)
	{
		const FFleshRingSettings& Ring = Rings[RingIdx];
		FSubdivisionRingParams RingParams;

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
				}
				else
				{
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
				RingParams.Width = Ring.RingHeight;
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
			RingParams.Width = Ring.RingHeight;
		}

		Processor.AddRingParams(RingParams);
	}

	// ============================================
	// 3-1. Affected 영역 계산 (삼각형 기반)
	// ============================================
	// 우선순위:
	// 1. SourceComponent의 DI에서 AffectedVertices 위치 추출 → 해당 위치를 포함하는 삼각형 찾기
	//    - PreviewMesh의 subdivision된 버텍스 위치를 사용하여 원본 메시의 삼각형을 정확히 선택
	//    - 새 버텍스(subdivision으로 생성된)도 포함하여 누락 영역 없음
	// 2. 폴백: 원본 메시 기반 버텍스 계산 → 삼각형으로 변환
	{
		using namespace SubdivisionHelpers;

		TSet<int32> CombinedTriangleIndices;
		bool bUsedDIData = false;

		// ★ 방법 1: SourceComponent의 DI에서 삼각형 추출 시도 (Point-in-Triangle)
		if (SourceComponent)
		{
			if (ExtractAffectedTrianglesFromDI(SourceComponent, SourceMesh, SourcePositions, SourceIndices, CombinedTriangleIndices))
			{
				bUsedDIData = true;
			}
		}

		// ★ 방법 2: 폴백 - 원본 메시 기반 버텍스 계산 후 삼각형으로 변환
		if (!bUsedDIData)
		{

			TSet<uint32> CombinedVertexIndices;

			// UV Seam 용접을 위한 위치 그룹화
			TMap<FIntVector, TArray<uint32>> PositionGroups = BuildPositionGroups(SourcePositions);

			// 인접성 맵 빌드 (HopBased용)
			TMap<uint32, TSet<uint32>> AdjacencyMap = BuildAdjacencyMap(SourceIndices);

			// UV Seam 처리: 같은 위치 버텍스들이 이웃을 공유하도록 확장
			ExpandAdjacencyForUVSeams(AdjacencyMap, PositionGroups);

			for (int32 RingIdx = 0; RingIdx < Rings.Num(); ++RingIdx)
			{
				const FFleshRingSettings& Ring = Rings[RingIdx];

				// 본 트랜스폼 계산
				int32 BoneIndex = RefSkeleton.FindBoneIndex(Ring.BoneName);
				FTransform BoneTransform = CalculateBoneTransform(BoneIndex, RefSkeleton, RefBonePose);

				// 1. 기본 Affected 버텍스 선택
				TSet<uint32> AffectedVertices;
				FBox RingBounds;
				FTransform RingTransform;

				if (!SelectAffectedVertices(Ring, SourcePositions, BoneTransform,
					AffectedVertices, RingBounds, RingTransform))
				{
					continue;
				}

				// 2. SmoothingVolumeMode에 따른 확장
				TSet<uint32> ExtendedVertices;

				if (!Ring.bEnablePostProcess)
				{
					ExtendedVertices = AffectedVertices;
				}
				else if (Ring.SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand)
				{
					ExpandByBounds(Ring, SourcePositions, RingTransform, RingBounds,
						AffectedVertices, ExtendedVertices);
				}
				else // HopBased
				{
					ExpandByHops(AffectedVertices, AdjacencyMap, Ring.MaxSmoothingHops, ExtendedVertices);
				}

				// 3. UV Seam 처리: 선택된 버텍스들의 같은 위치 버텍스도 추가
				AddPositionDuplicates(ExtendedVertices, SourcePositions, PositionGroups);

				// 합집합에 추가
				CombinedVertexIndices.Append(ExtendedVertices);
			}

			// 버텍스 → 삼각형 변환 (폴백의 경우)
			const int32 NumTriangles = SourceIndices.Num() / 3;
			for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
			{
				uint32 V0 = SourceIndices[TriIdx * 3 + 0];
				uint32 V1 = SourceIndices[TriIdx * 3 + 1];
				uint32 V2 = SourceIndices[TriIdx * 3 + 2];

				if (CombinedVertexIndices.Contains(V0) ||
					CombinedVertexIndices.Contains(V1) ||
					CombinedVertexIndices.Contains(V2))
				{
					CombinedTriangleIndices.Add(TriIdx);
				}
			}
		}

		// 삼각형 기반 모드 설정
		if (CombinedTriangleIndices.Num() > 0)
		{
			Processor.SetTargetTriangleIndices(CombinedTriangleIndices);
		}
		else
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateSubdividedMesh: No triangles selected, falling back to Ring params"));
		}
	}

	// Subdivision 실행
	FSubdivisionTopologyResult TopologyResult;
	if (!Processor.Process(TopologyResult))
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Subdivision 프로세스 실패"));
		return;
	}

	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateSubdividedMesh: %d -> %d vertices, %d -> %d triangles"),
		TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount,
		TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount);

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

	// ★ 루프 밖에서 선언하여 메모리 재사용 (힙 할당 최소화)
	TMap<uint16, float> BoneWeightMap;
	TArray<TPair<uint16, float>> SortedWeights;

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

		// ★ Reset()으로 내용만 비움 (메모리 유지)
		BoneWeightMap.Reset();
		SortedWeights.Reset();

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (SourceBoneWeights[P0][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P0][j]) += (SourceBoneWeights[P0][j] / 255.0f) * U;
			if (SourceBoneWeights[P1][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P1][j]) += (SourceBoneWeights[P1][j] / 255.0f) * V;
			if (SourceBoneWeights[P2][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P2][j]) += (SourceBoneWeights[P2][j] / 255.0f) * W;
		}

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

	// ============================================
	// 5. 새 USkeletalMesh 생성 (소스 메시 복제 방식)
	// ============================================
	// 기존 SubdividedMesh는 함수 시작 부분에서 이미 안전하게 제거됨

	// 소스 메시를 복제하여 모든 내부 구조 상속 (MorphTarget, LOD 데이터 등)
	// ★ 고유한 이름 사용 (기존 메시가 GC 대기 중일 수 있으므로 이름 충돌 방지)
	FString MeshName = FString::Printf(TEXT("%s_Subdivided_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	SubdivisionSettings.SubdividedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, this, FName(*MeshName));

	if (!SubdivisionSettings.SubdividedMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: 소스 메시 복제 실패"));
		return;
	}

	// 복제된 메시의 기존 MeshDescription 제거
	if (SubdivisionSettings.SubdividedMesh->HasMeshDescription(0))
	{
		SubdivisionSettings.SubdividedMesh->ClearMeshDescription(0);
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
	}

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
	SubdivisionSettings.SubdividedMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

	// 기존 렌더 리소스 해제 (DuplicateObject로 복제된 데이터 제거)
	SubdivisionSettings.SubdividedMesh->ReleaseResources();
	SubdivisionSettings.SubdividedMesh->ReleaseResourcesFence.Wait();

	// MeshDescription을 실제 LOD 모델 데이터로 커밋
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	SubdivisionSettings.SubdividedMesh->CommitMeshDescription(0, CommitParams);

	// 메시 빌드 (LOD 모델 → 렌더 데이터)
	SubdivisionSettings.SubdividedMesh->Build();

	// Build 결과 검증
	FSkeletalMeshRenderData* NewRenderData = SubdivisionSettings.SubdividedMesh->GetResourceForRendering();
	if (!NewRenderData || NewRenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateSubdividedMesh: Build failed - no RenderData"));
		SubdivisionSettings.SubdividedMesh->ConditionalBeginDestroy();
		SubdivisionSettings.SubdividedMesh = nullptr;
		return;
	}

	// 렌더 리소스 초기화
	SubdivisionSettings.SubdividedMesh->InitResources();
	FlushRenderingCommands();

	// 바운딩 박스 재계산
	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		BoundingBox += NewPositions[i];
	}
	SubdivisionSettings.SubdividedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	SubdivisionSettings.SubdividedMesh->CalculateExtendedBounds();

	// 파라미터 해시 저장 (재생성 판단용)
	SubdivisionSettings.SubdivisionParamsHash = CalculateSubdivisionParamsHash();
	MarkPackageDirty();

	// Note: OnAssetChanged.Broadcast() 호출 안 함
	// SubdividedMesh는 런타임용이고, 프리뷰는 PreviewSubdividedMesh를 사용
	// 브로드캐스트 시 프리뷰 DeformerInstance가 재초기화되어 변형 데이터 손실됨

	// 월드의 FleshRingComponent들만 직접 업데이트 (프리뷰 제외)
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
	if (SubdivisionSettings.SubdividedMesh)
	{
		// 이전 메시를 Transient 패키지로 이동시켜 GC가 정리하도록 함
		// 이렇게 하지 않으면 에셋 내에 Subdivided_1, Subdivided_2... 가 계속 누적됨
		USkeletalMesh* OldMesh = SubdivisionSettings.SubdividedMesh;
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		OldMesh->ClearFlags(RF_Public | RF_Standalone);
		OldMesh->SetFlags(RF_Transient);

		SubdivisionSettings.SubdividedMesh = nullptr;
		SubdivisionSettings.SubdivisionParamsHash = 0;

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
								Comp->ApplyAsset();
							}
						}
					}
				}
			}
		}

		MarkPackageDirty();
	}
}

void UFleshRingAsset::GeneratePreviewMesh()
{
	// ★ 캐시 체크 - 이미 유효하면 재생성 불필요
	if (IsPreviewMeshCacheValid())
	{
		return;
	}

	// 기존 PreviewMesh가 있으면 먼저 제거
	if (SubdivisionSettings.PreviewSubdividedMesh)
	{
		// ★ 즉시 파괴하지 않고 포인터만 null로 설정 (렌더 스레드 안전)
		SubdivisionSettings.PreviewSubdividedMesh = nullptr;
		OnAssetChanged.Broadcast(this);
	}

	if (!SubdivisionSettings.bEnableSubdivision)
	{
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

	// ★ 성능 측정 시작
	const double StartTime = FPlatformTime::Seconds();

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

	// ★ Processor 전달용 FVertexBoneInfluence 배열 (중복 추출 방지)
	TArray<FVertexBoneInfluence> VertexBoneInfluences;
	VertexBoneInfluences.SetNum(SourceVertexCount);

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
		const int32 ClampedInfluences = FMath::Min(MaxBoneInfluences, FVertexBoneInfluence::MAX_INFLUENCES);
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			SourceBoneIndices[i].SetNum(MaxBoneInfluences);
			SourceBoneWeights[i].SetNum(MaxBoneInfluences);

			// FVertexBoneInfluence 초기화
			FVertexBoneInfluence& Influence = VertexBoneInfluences[i];
			FMemory::Memzero(Influence.BoneIndices, sizeof(Influence.BoneIndices));
			FMemory::Memzero(Influence.BoneWeights, sizeof(Influence.BoneWeights));

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

				// ★ FVertexBoneInfluence에도 저장 (Processor 전달용)
				if (j < ClampedInfluences)
				{
					Influence.BoneIndices[j] = GlobalBoneIdx;
					Influence.BoneWeights[j] = Weight;
				}
			}
		}
	}

	// 3. ★ 본 기반 Subdivision 프로세서 실행 (중복 추출 제거)
	FFleshRingSubdivisionProcessor Processor;

	// ★ 이미 추출한 데이터를 직접 전달 (SetSourceMeshWithBoneInfo 대신)
	if (!Processor.SetSourceMesh(SourcePositions, SourceIndices, SourceUVs, SourceTriangleMaterialIndices))
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: SetSourceMesh 실패"));
		return;
	}
	Processor.SetVertexBoneInfluences(VertexBoneInfluences);

	FSubdivisionProcessorSettings Settings;
	Settings.MinEdgeLength = SubdivisionSettings.MinEdgeLength;
	Processor.SetSettings(Settings);

	FSubdivisionTopologyResult TopologyResult;

	// ★ 본 기반 영역 subdivision 사용 (링이 있는 경우)
	if (Rings.Num() > 0 && Processor.HasBoneInfo())
	{
		// 링 부착 본 인덱스 수집
		const FReferenceSkeleton& RefSkeleton = SourceMesh->GetRefSkeleton();
		TArray<int32> RingBoneIndices;
		for (const FFleshRingSettings& Ring : Rings)
		{
			int32 BoneIdx = RefSkeleton.FindBoneIndex(Ring.BoneName);
			if (BoneIdx != INDEX_NONE)
			{
				RingBoneIndices.Add(BoneIdx);
			}
		}

		// 이웃 본 수집
		TSet<int32> TargetBones = FFleshRingSubdivisionProcessor::GatherNeighborBones(
			RefSkeleton, RingBoneIndices, SubdivisionSettings.PreviewBoneHopCount);

		// 본 영역 파라미터 설정
		FBoneRegionSubdivisionParams BoneParams;
		BoneParams.TargetBoneIndices = TargetBones;
		BoneParams.BoneWeightThreshold = static_cast<uint8>(SubdivisionSettings.PreviewBoneWeightThreshold * 255);
		BoneParams.NeighborHopCount = SubdivisionSettings.PreviewBoneHopCount;
		BoneParams.MaxSubdivisionLevel = SubdivisionSettings.PreviewSubdivisionLevel;

		if (!Processor.ProcessBoneRegion(TopologyResult, BoneParams))
		{
			// ProcessBoneRegion 실패 시 ProcessUniform으로 fallback
			if (!Processor.ProcessUniform(TopologyResult, SubdivisionSettings.PreviewSubdivisionLevel))
			{
				UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: Subdivision 실패"));
				return;
			}
		}
	}
	else
	{
		// 링이 없거나 본 정보가 없으면 균일 subdivision (fallback)
		if (!Processor.ProcessUniform(TopologyResult, SubdivisionSettings.PreviewSubdivisionLevel))
		{
			UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: Subdivision 실패"));
			return;
		}
	}

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

	// ★ 루프 밖에서 선언하여 메모리 재사용 (힙 할당 최소화)
	TMap<uint16, float> BoneWeightMap;
	TArray<TPair<uint16, float>> SortedWeights;

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

		// ★ Reset()으로 내용만 비움 (메모리 유지)
		BoneWeightMap.Reset();
		SortedWeights.Reset();

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (SourceBoneWeights[P0][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P0][j]) += (SourceBoneWeights[P0][j] / 255.0f) * U;
			if (SourceBoneWeights[P1][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P1][j]) += (SourceBoneWeights[P1][j] / 255.0f) * V;
			if (SourceBoneWeights[P2][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P2][j]) += (SourceBoneWeights[P2][j] / 255.0f) * W;
		}
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
	SubdivisionSettings.PreviewSubdividedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, GetTransientPackage(), FName(*MeshName));

	if (!SubdivisionSettings.PreviewSubdividedMesh)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GeneratePreviewMesh: 메시 복제 실패"));
		return;
	}

	// 복제된 메시의 기존 렌더 리소스 완전 해제 (렌더 스레드 동기화)
	FlushRenderingCommands();
	SubdivisionSettings.PreviewSubdividedMesh->ReleaseResources();
	SubdivisionSettings.PreviewSubdividedMesh->ReleaseResourcesFence.Wait();

	// 기존 MeshDescription 제거
	if (SubdivisionSettings.PreviewSubdividedMesh->HasMeshDescription(0))
	{
		SubdivisionSettings.PreviewSubdividedMesh->ClearMeshDescription(0);
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

	SubdivisionSettings.PreviewSubdividedMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

	// ReleaseResources는 위에서 이미 호출됨
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	SubdivisionSettings.PreviewSubdividedMesh->CommitMeshDescription(0, CommitParams);
	SubdivisionSettings.PreviewSubdividedMesh->Build();
	SubdivisionSettings.PreviewSubdividedMesh->InitResources();

	// 렌더 스레드가 리소스 초기화를 완료할 때까지 대기
	FlushRenderingCommands();

	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < NewVertexCount; ++i) { BoundingBox += NewPositions[i]; }
	SubdivisionSettings.PreviewSubdividedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	SubdivisionSettings.PreviewSubdividedMesh->CalculateExtendedBounds();

	// ★ 캐시 해시 업데이트
	SubdivisionSettings.CachedPreviewBoneConfigHash = CalculatePreviewBoneConfigHash();

	// ★ 성능 측정 종료
	const double EndTime = FPlatformTime::Seconds();
	const double ElapsedMs = (EndTime - StartTime) * 1000.0;

	UE_LOG(LogFleshRingAsset, Log, TEXT("GeneratePreviewMesh 완료: %d vertices, %d triangles (%.2fms, CacheHash=%u)"),
		NewVertexCount, TopologyResult.SubdividedTriangleCount, ElapsedMs, SubdivisionSettings.CachedPreviewBoneConfigHash);
}

void UFleshRingAsset::ClearPreviewMesh()
{
	if (SubdivisionSettings.PreviewSubdividedMesh)
	{
		// ★ 즉시 파괴하지 않고 포인터만 null로 설정
		// 렌더 스레드가 아직 메시를 사용 중일 수 있으므로 GC가 안전하게 정리하게 함
		// (ConditionalBeginDestroy()를 호출하면 렌더 스레드 크래시 발생 가능)
		SubdivisionSettings.PreviewSubdividedMesh = nullptr;
	}
}

bool UFleshRingAsset::NeedsPreviewMeshRegeneration() const
{
	if (!SubdivisionSettings.bEnableSubdivision)
	{
		return false;
	}

	// 메시가 없으면 재생성 필요
	if (SubdivisionSettings.PreviewSubdividedMesh == nullptr)
	{
		return true;
	}

	// ★ 캐시가 무효화되었으면 재생성 필요 (본 변경, 링 추가/삭제 등)
	if (!IsPreviewMeshCacheValid())
	{
		return true;
	}

	return false;
}

void UFleshRingAsset::SetEditorSelectedRingIndex(int32 RingIndex, EFleshRingSelectionType SelectionType)
{
	EditorSelectedRingIndex = RingIndex;
	EditorSelectionType = SelectionType;

	// 델리게이트 브로드캐스트 (디테일 패널 → 뷰포트/트리 동기화)
	OnRingSelectionChanged.Broadcast(RingIndex);
}

void UFleshRingAsset::InvalidatePreviewMeshCache()
{
	// ★ MAX_uint32로 설정하여 계산된 해시와 절대 일치하지 않도록 함
	SubdivisionSettings.CachedPreviewBoneConfigHash = MAX_uint32;
}

uint32 UFleshRingAsset::CalculatePreviewBoneConfigHash() const
{
	uint32 Hash = 0;

	// 링 부착 본 목록 해시
	for (const FFleshRingSettings& Ring : Rings)
	{
		Hash = HashCombine(Hash, GetTypeHash(Ring.BoneName));
	}

	// subdivision 파라미터 해시
	Hash = HashCombine(Hash, GetTypeHash(SubdivisionSettings.PreviewSubdivisionLevel));
	Hash = HashCombine(Hash, GetTypeHash(SubdivisionSettings.PreviewBoneHopCount));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(SubdivisionSettings.PreviewBoneWeightThreshold * 255)));
	Hash = HashCombine(Hash, GetTypeHash(SubdivisionSettings.MinEdgeLength));

	return Hash;
}

bool UFleshRingAsset::IsPreviewMeshCacheValid() const
{
	if (!HasValidPreviewMesh())
	{
		return false;
	}

	// 해시 비교
	return SubdivisionSettings.CachedPreviewBoneConfigHash == CalculatePreviewBoneConfigHash();
}

// =====================================
// Baked Mesh 관련 함수
// =====================================

bool UFleshRingAsset::GenerateBakedMesh(UFleshRingComponent* SourceComponent)
{
	if (!SourceComponent)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: SourceComponent is null"));
		return false;
	}

	USkeletalMeshComponent* SkelMeshComp = SourceComponent->GetResolvedTargetMesh();
	if (!SkelMeshComp)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: SourceComponent has no resolved target mesh"));
		return false;
	}

	// =====================================
	// 소스 메시 결정: Subdivision ON → SubdividedMesh, OFF → 원본 메시
	// =====================================
	USkeletalMesh* SourceMesh = nullptr;

	if (SubdivisionSettings.bEnableSubdivision)
	{
		// 서브디비전 ON: SubdividedMesh 생성/사용
		if (!SubdivisionSettings.SubdividedMesh || NeedsSubdivisionRegeneration())
		{
			GenerateSubdividedMesh(SourceComponent);
		}

		if (SubdivisionSettings.SubdividedMesh)
		{
			SourceMesh = SubdivisionSettings.SubdividedMesh;
			UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Using SubdividedMesh"));
		}
		else
		{
			// 서브디비전 생성 실패 → 원본 메시로 폴백
			SourceMesh = TargetSkeletalMesh.LoadSynchronous();
			UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: SubdividedMesh generation failed, falling back to original mesh"));
		}
	}
	else
	{
		// 서브디비전 OFF: 원본 메시에 변형만 적용하여 베이크
		SourceMesh = TargetSkeletalMesh.LoadSynchronous();
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Subdivision disabled, using original mesh"));
	}

	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: No source mesh available"));
		return false;
	}

	UFleshRingDeformer* Deformer = SourceComponent->GetDeformer();
	if (!Deformer)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: Deformer is null"));
		return false;
	}

	UFleshRingDeformerInstance* DeformerInstance = Deformer->GetActiveInstance();
	if (!DeformerInstance)
	{
		UE_LOG(LogFleshRingAsset, Warning, TEXT("GenerateBakedMesh: DeformerInstance is null"));
		return false;
	}

	// =====================================
	// GPU 베이킹: SourceMesh를 렌더링하여 Readback
	// (Subdivision ON: SubdividedMesh / OFF: 원본 메시)
	// =====================================
	//
	// 비동기 베이크 지원 방식:
	// 1. 현재 메시가 SourceMesh가 아니면 → 스왑만 하고 false 반환 (비동기 시스템이 대기)
	// 2. SourceMesh이고 캐시가 유효하면 → Readback 진행
	// 3. SourceMesh이지만 캐시가 아직 안 유효하면 → false 반환 (비동기 시스템이 대기)

	USkeletalMesh* CurrentMesh = SkelMeshComp->GetSkeletalMeshAsset();
	const bool bAlreadyUsingSourceMesh = (CurrentMesh == SourceMesh);

	if (!bAlreadyUsingSourceMesh)
	{
		// Step 1: SourceMesh로 스왑 (첫 호출)
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Swapping to SourceMesh..."));
		SkelMeshComp->SetSkeletalMeshAsset(SourceMesh);

		// Step 2: ★ MeshObject 완전 재생성 (동기적)
		// RecreateRenderState_Concurrent()는 비동기적이라 MeshObject가 바로 업데이트되지 않음
		// UnregisterComponent/RegisterComponent를 사용하여 동기적으로 재생성
		SkelMeshComp->UnregisterComponent();
		SkelMeshComp->RegisterComponent();
		FlushRenderingCommands();

		// Step 3: Deformer 완전 재초기화 (새 메시 기준으로 LODData/AffectedVertices 재등록)
		// ★ 반드시 MeshObject 재생성 후 호출해야 새 메시의 RenderData를 읽음
		DeformerInstance->InvalidateForMeshChange();

		// 메시 스왑만 하고 반환 - 비동기 시스템이 캐시 유효해질 때까지 대기 후 재호출
		return false;
	}

	// 이미 SourceMesh가 설정됨 - 캐시 확인
	if (!DeformerInstance->HasCachedDeformedGeometry(0))
	{
		// 캐시가 아직 유효하지 않음 - 비동기 시스템이 재시도
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Waiting for cache to become valid..."));
		return false;
	}

	// 캐시 유효 - Readback 진행
	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Cache valid, proceeding with readback..."));

	// GPU Readback (SourceMesh 기준 - 직접 대응)
	TArray<FVector3f> DeformedPositions;
	TArray<FVector3f> DeformedNormals;
	TArray<FVector4f> DeformedTangents;

	if (!DeformerInstance->ReadbackDeformedGeometry(DeformedPositions, DeformedNormals, DeformedTangents, 0))
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: GPU Readback failed"));
		// 메시 복원은 비동기 시스템(CleanupAsyncBake)이 처리
		return false;
	}

	// Readback 검증
	const FSkeletalMeshRenderData* SourceRenderData = SourceMesh->GetResourceForRendering();
	if (!SourceRenderData || SourceRenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Source mesh has no render data"));
		return false;
	}

	const FSkeletalMeshLODRenderData& SourceLODData = SourceRenderData->LODRenderData[0];
	const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// 버퍼 크기 검증 (SourceMesh 직접 렌더링이므로 정확히 일치해야 함)
	if (DeformedPositions.Num() != (int32)SourceVertexCount)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Vertex count mismatch - Readback=%d, Expected=%d"),
			DeformedPositions.Num(), SourceVertexCount);
		return false;
	}

	// Normal/Tangent 기본값 채우기 (없는 경우)
	const bool bHasNormals = DeformedNormals.Num() == (int32)SourceVertexCount;
	const bool bHasTangents = DeformedTangents.Num() == (int32)SourceVertexCount;

	if (!bHasNormals)
	{
		DeformedNormals.SetNum(SourceVertexCount);
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			DeformedNormals[i] = FVector3f(0, 0, 1);
		}
	}

	if (!bHasTangents)
	{
		DeformedTangents.SetNum(SourceVertexCount);
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			DeformedTangents[i] = FVector4f(1, 0, 0, 1);
		}
	}

	// ★ 수정: 기존 BakedMesh 정리를 나중에 수행 (새 메시 생성 성공 후)
	// 먼저 정리하면, 새 메시 생성 실패 시 이전 메시도 없어짐

	// =====================================
	// MeshDescription 기반 방식 (SubdividedMesh와 동일)
	// DuplicateObject로 복제하면 MeshDescription(스킨 웨이트 포함)이 복사됨
	// MeshDescription에서 버텍스 위치만 수정 후 Build() 호출
	// 이 방식은 제대로 직렬화되고, 스킨 웨이트 매핑도 유지됨
	// =====================================

	// 고유한 이름으로 새 SkeletalMesh 생성 (기존 메시가 GC 대기 중일 수 있으므로)
	FString MeshName = FString::Printf(TEXT("%s_Baked_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	USkeletalMesh* NewBakedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, this, FName(*MeshName));
	if (!NewBakedMesh)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Failed to duplicate source mesh"));
		return false;
	}

	// ★ 애니메이션 관련 속성을 원본과 동일하게 유지 (AnimInstance 재초기화 방지)
	NewBakedMesh->SetSkeleton(SourceMesh->GetSkeleton());
	NewBakedMesh->SetPhysicsAsset(SourceMesh->GetPhysicsAsset());
	NewBakedMesh->SetShadowPhysicsAsset(SourceMesh->GetShadowPhysicsAsset());

	// MeshDescription 가져오기 (DuplicateObject로 복사됨, 스킨 웨이트 포함)
	FMeshDescription* MeshDesc = NewBakedMesh->GetMeshDescription(0);
	if (!MeshDesc)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Duplicated mesh has no MeshDescription"));
		NewBakedMesh->ConditionalBeginDestroy();
		return false;
	}

	// =====================================
	// MeshDescription에서 버텍스 위치 수정
	// 스킨 웨이트는 MeshDescription 안에 이미 있으므로 그대로 유지됨
	// =====================================
	TVertexAttributesRef<FVector3f> VertexPositions = MeshDesc->GetVertexPositions();
	const int32 MeshDescVertexCount = MeshDesc->Vertices().Num();

	// 버텍스 수 검증: RenderData 버텍스 수와 MeshDescription 버텍스 수는 다를 수 있음
	// MeshDescription은 고유 버텍스, RenderData는 VertexInstance(중복 포함)
	// GPU Readback 데이터는 RenderData 기준이므로 매핑이 필요
	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: MeshDesc vertices=%d, RenderData vertices=%d"),
		MeshDescVertexCount, SourceVertexCount);

	// =====================================
	// 버텍스 매핑 및 위치 업데이트 (최적화된 해시맵 방식)
	// RenderData 버텍스 → MeshDescription 버텍스 매핑
	// =====================================

	// 원본 RenderData에서 위치 기반 매핑 구축
	// (SourceRenderData는 이미 위에서 선언되어 있음)
	if (SourceRenderData && SourceRenderData->LODRenderData.Num() > 0)
	{
		const FPositionVertexBuffer& SrcPosBuffer = SourceRenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer;

		// 위치를 정수 그리드로 양자화하여 해시 키로 사용 (O(1) 룩업)
		// 스케일: 0.001 단위로 양자화 (1mm 정밀도)
		auto QuantizePosition = [](const FVector3f& Pos) -> FIntVector
		{
			const float Scale = 1000.0f;  // 0.001 단위
			return FIntVector(
				FMath::RoundToInt(Pos.X * Scale),
				FMath::RoundToInt(Pos.Y * Scale),
				FMath::RoundToInt(Pos.Z * Scale)
			);
		};

		// ★ 수정: UV seam에서 같은 위치에 여러 버텍스가 있을 수 있으므로 TArray 사용
		// MeshDescription 버텍스를 양자화된 위치로 인덱싱 (위치당 여러 버텍스 허용)
		TMap<FIntVector, TArray<FVertexID>> QuantizedPosToVertices;
		QuantizedPosToVertices.Reserve(MeshDescVertexCount);

		for (const FVertexID VertexID : MeshDesc->Vertices().GetElementIDs())
		{
			FIntVector QuantizedPos = QuantizePosition(VertexPositions[VertexID]);
			QuantizedPosToVertices.FindOrAdd(QuantizedPos).Add(VertexID);
		}

		// RenderData 버텍스 → MeshDescription 버텍스 매핑 (O(n) 복잡도)
		// ★ 같은 위치의 모든 MeshDescription 버텍스에 동일한 RenderIdx 매핑
		TMap<FVertexID, uint32> VertexToFirstRenderIdx;
		VertexToFirstRenderIdx.Reserve(MeshDescVertexCount);

		for (uint32 RenderIdx = 0; RenderIdx < SourceVertexCount; ++RenderIdx)
		{
			FVector3f RenderPos = SrcPosBuffer.VertexPosition(RenderIdx);
			FIntVector QuantizedPos = QuantizePosition(RenderPos);

			// 해시맵에서 O(1) 룩업 - 같은 위치의 모든 버텍스에 매핑
			TArray<FVertexID>* FoundVertexIDs = QuantizedPosToVertices.Find(QuantizedPos);
			if (FoundVertexIDs)
			{
				for (const FVertexID& VertexID : *FoundVertexIDs)
				{
					// 첫 번째 매핑만 저장 (같은 위치의 여러 RenderData 버텍스 중 하나만 필요)
					if (!VertexToFirstRenderIdx.Contains(VertexID))
					{
						VertexToFirstRenderIdx.Add(VertexID, RenderIdx);
					}
				}
			}
		}

		// MeshDescription 버텍스 위치 업데이트
		for (const FVertexID VertexID : MeshDesc->Vertices().GetElementIDs())
		{
			uint32* RenderIdxPtr = VertexToFirstRenderIdx.Find(VertexID);
			if (RenderIdxPtr && *RenderIdxPtr < SourceVertexCount)
			{
				VertexPositions[VertexID] = DeformedPositions[*RenderIdxPtr];
			}
		}

		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Mapped %d/%d vertices"),
			VertexToFirstRenderIdx.Num(), MeshDescVertexCount);

		// =====================================
		// 노멀/탄젠트 업데이트 (VertexInstance 기반)
		// MeshDescription에서 노멀/탄젠트는 VertexInstance에 저장됨
		// ★ 수정: 순차 인덱싱 대신 VertexID 기반 매핑 사용
		// =====================================
		if (bHasNormals && bHasTangents)
		{
			FSkeletalMeshAttributes MeshAttributes(*MeshDesc);
			TVertexInstanceAttributesRef<FVector3f> InstanceNormals = MeshAttributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> InstanceTangents = MeshAttributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<float> InstanceBinormalSigns = MeshAttributes.GetVertexInstanceBinormalSigns();

			// VertexInstance를 순회하며 노멀/탄젠트 업데이트
			// ★ VertexInstance의 부모 VertexID를 통해 RenderData 인덱스 찾기
			for (const FVertexInstanceID InstanceID : MeshDesc->VertexInstances().GetElementIDs())
			{
				FVertexID VertexID = MeshDesc->GetVertexInstanceVertex(InstanceID);
				uint32* RenderIdxPtr = VertexToFirstRenderIdx.Find(VertexID);

				if (RenderIdxPtr && *RenderIdxPtr < SourceVertexCount)
				{
					uint32 RenderIdx = *RenderIdxPtr;
					const FVector3f& Normal = DeformedNormals[RenderIdx];
					// GPU에서 재계산된 노멀이 유효한 경우만 적용
					if (!Normal.IsNearlyZero())
					{
						FVector3f Tangent(DeformedTangents[RenderIdx].X, DeformedTangents[RenderIdx].Y, DeformedTangents[RenderIdx].Z);
						float BinormalSign = DeformedTangents[RenderIdx].W;

						InstanceNormals[InstanceID] = Normal;
						InstanceTangents[InstanceID] = Tangent;
						InstanceBinormalSigns[InstanceID] = BinormalSign;
					}
				}
				// 매핑 안 된 VertexInstance는 원본 노멀 유지 (얼굴 등 영향받지 않는 영역)
			}
		}
	}

	// =====================================
	// MeshDescription 커밋 및 빌드 (SubdividedMesh와 동일)
	// =====================================
	// 기존 렌더 리소스 해제
	NewBakedMesh->ReleaseResources();
	NewBakedMesh->ReleaseResourcesFence.Wait();
	FlushRenderingCommands();

	// MeshDescription을 LOD 모델로 커밋
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	NewBakedMesh->CommitMeshDescription(0, CommitParams);

	// 메시 빌드 (RenderData 생성)
	NewBakedMesh->Build();

	// RenderData 검증
	FSkeletalMeshRenderData* NewRenderData = NewBakedMesh->GetResourceForRendering();
	if (!NewRenderData || NewRenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingAsset, Error, TEXT("GenerateBakedMesh: Build failed - no RenderData"));
		NewBakedMesh->ConditionalBeginDestroy();
		return false;
	}

	// 렌더 리소스 초기화
	NewBakedMesh->InitResources();
	FlushRenderingCommands();

	// 바운딩 박스 재계산
	FBox BoundingBox(ForceInit);
	for (uint32 i = 0; i < SourceVertexCount; ++i)
	{
		BoundingBox += FVector(DeformedPositions[i]);
	}
	NewBakedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	NewBakedMesh->CalculateExtendedBounds();

	// Ring 트랜스폼 저장 (본 상대 좌표로 저장)
	SubdivisionSettings.BakedRingTransforms.Empty();
	for (const FFleshRingSettings& Ring : Rings)
	{
		FTransform RingRelativeTransform;
		RingRelativeTransform.SetLocation(Ring.MeshOffset);
		RingRelativeTransform.SetRotation(FQuat(Ring.MeshRotation));
		RingRelativeTransform.SetScale3D(Ring.MeshScale);
		SubdivisionSettings.BakedRingTransforms.Add(RingRelativeTransform);
	}

	// ★ 새 메시가 완전히 준비되었으므로 이제 이전 BakedMesh 정리
	// (생성 실패 시에도 이전 메시가 유지됨)
	if (SubdivisionSettings.BakedMesh)
	{
		USkeletalMesh* OldMesh = SubdivisionSettings.BakedMesh;
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		OldMesh->ClearFlags(RF_Public | RF_Standalone);
		OldMesh->SetFlags(RF_Transient);
		UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Cleaned up previous BakedMesh"));
	}
	SubdivisionSettings.BakedRingTransforms.Empty();

	// 결과 저장
	SubdivisionSettings.BakedMesh = NewBakedMesh;
	SubdivisionSettings.BakeParamsHash = CalculateBakeParamsHash();

	UE_LOG(LogFleshRingAsset, Log, TEXT("GenerateBakedMesh: Success - %d vertices, %d rings, Hash=%u"),
		SourceVertexCount, Rings.Num(), SubdivisionSettings.BakeParamsHash);

	// ★ 베이크 성공 후 SubdividedMesh 정리 (더 이상 필요 없음)
	// BakedMesh가 SubdividedMesh의 변형 결과를 포함하므로 중복 저장 불필요
	if (SubdivisionSettings.SubdividedMesh)
	{
		USkeletalMesh* OldSubdivMesh = SubdivisionSettings.SubdividedMesh;
		OldSubdivMesh->Rename(nullptr, GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional);
		OldSubdivMesh->ClearFlags(RF_Public | RF_Standalone);
		OldSubdivMesh->SetFlags(RF_Transient);
		SubdivisionSettings.SubdividedMesh = nullptr;
		SubdivisionSettings.SubdivisionParamsHash = 0;

		UE_LOG(LogFleshRingAsset, Log,
			TEXT("GenerateBakedMesh: Cleared SubdividedMesh (no longer needed after bake)"));
	}

	// 에셋 변경 통지
	MarkPackageDirty();
	OnAssetChanged.Broadcast(this);

	return true;
}

void UFleshRingAsset::ClearBakedMesh()
{
	if (SubdivisionSettings.BakedMesh)
	{
		// 이전 메시를 Transient 패키지로 이동시켜 GC가 정리하도록 함
		// 이렇게 하지 않으면 에셋 내에 BakedMesh_1, BakedMesh_2... 가 계속 누적됨
		USkeletalMesh* OldMesh = SubdivisionSettings.BakedMesh;
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		OldMesh->ClearFlags(RF_Public | RF_Standalone);
		OldMesh->SetFlags(RF_Transient);

		SubdivisionSettings.BakedMesh = nullptr;
	}
	SubdivisionSettings.BakedRingTransforms.Empty();
	SubdivisionSettings.BakeParamsHash = 0;

	MarkPackageDirty();
}

bool UFleshRingAsset::NeedsBakeRegeneration() const
{
	// BakedMesh가 없으면 재생성 필요
	if (!SubdivisionSettings.BakedMesh)
	{
		return true;
	}

	// 해시 비교로 파라미터 변경 여부 확인
	return SubdivisionSettings.BakeParamsHash != CalculateBakeParamsHash();
}

uint32 UFleshRingAsset::CalculateBakeParamsHash() const
{
	// Subdivision 파라미터 해시를 기본으로
	uint32 Hash = CalculateSubdivisionParamsHash();

	// Ring별 변형 파라미터 추가
	for (const FFleshRingSettings& Ring : Rings)
	{
		// 위치/회전 (정밀도 제한)
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingOffset.X * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingOffset.Y * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingOffset.Z * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingEulerRotation.Pitch * 10)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingEulerRotation.Yaw * 10)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.RingEulerRotation.Roll * 10)));

		// 변형 강도
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.TightnessStrength * 1000)));
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnableBulge));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.BulgeIntensity * 1000)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.BulgeAxialRange * 100)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.BulgeRadialRange * 100)));

		// 스무딩 설정
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnablePostProcess));
		Hash = HashCombine(Hash, GetTypeHash(Ring.bEnableSmoothing));
		Hash = HashCombine(Hash, GetTypeHash(Ring.SmoothingIterations));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Ring.SmoothingLambda * 1000)));
	}

	return Hash;
}

int32 UFleshRingAsset::CleanupOrphanedMeshes()
{
	int32 RemovedCount = 0;

	// 현재 사용 중인 메시 포인터 수집
	TSet<USkeletalMesh*> ActiveMeshes;
	if (SubdivisionSettings.SubdividedMesh)
	{
		ActiveMeshes.Add(SubdivisionSettings.SubdividedMesh);
	}
	if (SubdivisionSettings.BakedMesh)
	{
		ActiveMeshes.Add(SubdivisionSettings.BakedMesh);
	}
	if (SubdivisionSettings.PreviewSubdividedMesh)
	{
		ActiveMeshes.Add(SubdivisionSettings.PreviewSubdividedMesh);
	}

	// 이 에셋의 모든 SkeletalMesh 서브오브젝트 수집
	TArray<UObject*> SubObjects;
	GetObjectsWithOuter(this, SubObjects, false);

	for (UObject* SubObj : SubObjects)
	{
		USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(SubObj);
		if (SkelMesh && !ActiveMeshes.Contains(SkelMesh))
		{
			// 고아 메시 발견 - Transient 패키지로 이동
			UE_LOG(LogFleshRingAsset, Log, TEXT("CleanupOrphanedMeshes: Removing orphaned mesh '%s'"), *SkelMesh->GetName());

			SkelMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			SkelMesh->ClearFlags(RF_Public | RF_Standalone);
			SkelMesh->SetFlags(RF_Transient);
			RemovedCount++;
		}
	}

	if (RemovedCount > 0)
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("CleanupOrphanedMeshes: Removed %d orphaned mesh(es)"), RemovedCount);
		MarkPackageDirty();
	}
	else
	{
		UE_LOG(LogFleshRingAsset, Log, TEXT("CleanupOrphanedMeshes: No orphaned meshes found"));
	}

	return RemovedCount;
}
#endif
