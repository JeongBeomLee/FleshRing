// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAsset.h"

UFleshRingAsset::UFleshRingAsset()
{
	// 기본 SDF 설정
	SdfSettings.Resolution = 64;
	SdfSettings.JfaIterations = 8;
	SdfSettings.UpdateMode = EFleshRingSdfUpdateMode::OnChange;
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

#if WITH_EDITOR
void UFleshRingAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 에셋이 수정되었음을 표시
	MarkPackageDirty();
}
#endif
