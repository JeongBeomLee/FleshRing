// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FleshRingTypes.h"
#include "FleshRingAsset.generated.h"

/**
 * FleshRing 설정을 저장하는 에셋
 * Content Browser에서 생성하여 여러 캐릭터에 재사용 가능
 */
UCLASS(BlueprintType)
class FLESHRINGRUNTIME_API UFleshRingAsset : public UObject
{
	GENERATED_BODY()

public:
	UFleshRingAsset();

	// =====================================
	// Target Mesh
	// =====================================

	/** 이 에셋이 타겟으로 하는 스켈레탈 메시 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	TSoftObjectPtr<USkeletalMesh> TargetSkeletalMesh;

	// =====================================
	// Ring Settings
	// =====================================

	/** Ring 설정 배열 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Settings")
	TArray<FFleshRingSettings> Rings;

	// =====================================
	// SDF Settings
	// =====================================

	/** SDF 설정 (각 Ring의 RingMesh에서 SDF 생성) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF Settings")
	FFleshRingSdfSettings SdfSettings;

	// =====================================
	// Utility Functions
	// =====================================

	/** Ring 추가 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	int32 AddRing(const FFleshRingSettings& NewRing);

	/** Ring 제거 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	bool RemoveRing(int32 Index);

	/** Ring 개수 반환 */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	int32 GetNumRings() const { return Rings.Num(); }

	/** 유효성 검사 */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	bool IsValid() const;

#if WITH_EDITOR
	/** 에디터에서 프로퍼티 변경 시 호출 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
