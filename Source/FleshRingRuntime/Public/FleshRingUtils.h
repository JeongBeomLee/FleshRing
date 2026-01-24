// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/**
 * FleshRing 플러그인 공통 유틸리티 함수
 */
namespace FleshRingUtils
{
	/**
	 * 스켈레탈 메시 유효성 검사
	 * Undo/Redo 시 손상된 메시나 미초기화된 렌더 리소스 감지
	 *
	 * 검사 항목:
	 * - 렌더 리소스 존재 여부
	 * - LOD 데이터 존재 여부
	 * - 버텍스 버퍼 유효성
	 * - 스켈레톤 본 유효성
	 * - 부모 인덱스 무결성 (EnsureParentsExist 크래시 방지)
	 *
	 * @param Mesh 검사할 스켈레탈 메시
	 * @param bLogWarnings true면 실패 시 경고 로그 출력
	 * @return 메시가 유효하면 true
	 */
	FLESHRINGRUNTIME_API bool IsSkeletalMeshValid(USkeletalMesh* Mesh, bool bLogWarnings = false);
}
