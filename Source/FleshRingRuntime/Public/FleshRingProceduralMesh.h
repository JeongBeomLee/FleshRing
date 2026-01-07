// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.h"

/**
 * 프로시저럴 밴드 메시 생성 유틸리티
 *
 * 스타킹/타이즈용 비대칭 원기둥 형태의 메시를 생성합니다.
 * 생성된 메시는 SDF 생성용으로만 사용되며, 실제 렌더링되지 않습니다.
 *
 * 형태:
 *      ╱────╲      ← Upper Section (경사, 살 불룩 영역)
 *     │      │     ← Band Body (조임 영역)
 *     │      │
 *      ╲────╱      ← Lower Section (경사, 스타킹 영역)
 */
namespace FleshRingProceduralMesh
{
	/**
	 * 프로시저럴 밴드 메시 데이터 생성
	 *
	 * @param Settings 밴드 설정 (반경, 높이, 각도 등)
	 * @param OutVertices 출력 버텍스 배열 (로컬 스페이스)
	 * @param OutIndices 출력 인덱스 배열 (삼각형 리스트)
	 */
	void GenerateBandMesh(
		const FProceduralBandSettings& Settings,
		TArray<FVector3f>& OutVertices,
		TArray<uint32>& OutIndices);

	/**
	 * 밴드 메시의 바운딩 박스 계산
	 *
	 * @param Settings 밴드 설정
	 * @return 로컬 스페이스 바운딩 박스
	 */
	FBox3f CalculateBandBounds(const FProceduralBandSettings& Settings);

	/**
	 * 디버그 시각화용 와이어프레임 데이터 생성
	 *
	 * @param Settings 밴드 설정
	 * @param OutLines 출력 라인 배열 (시작점, 끝점 쌍)
	 * @param NumSegments 원형 세그먼트 수 (기본 24)
	 */
	void GenerateWireframeLines(
		const FProceduralBandSettings& Settings,
		TArray<TPair<FVector, FVector>>& OutLines,
		int32 NumSegments = 24);
}
