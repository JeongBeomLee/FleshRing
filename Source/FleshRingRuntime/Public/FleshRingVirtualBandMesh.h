// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.h"

/**
 * 가상 밴드 시각화 유틸리티
 *
 * 스타킹/타이즈용 비대칭 원기둥의 와이어프레임을 생성합니다.
 * 에디터 프리뷰 시각화용으로 사용됩니다.
 */
namespace FleshRingVirtualBandMesh
{
	/**
	 * 디버그 시각화용 와이어프레임 데이터 생성
	 *
	 * @param Settings 밴드 설정
	 * @param OutLines 출력 라인 배열 (시작점, 끝점 쌍)
	 * @param NumSegments 원형 세그먼트 수 (기본 24)
	 */
	void GenerateWireframeLines(
		const FVirtualBandSettings& Settings,
		TArray<TPair<FVector, FVector>>& OutLines,
		int32 NumSegments = 24);
}
