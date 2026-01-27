// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/**
 * FleshRing plugin common utility functions
 */
namespace FleshRingUtils
{
	/**
	 * Skeletal mesh validity check
	 * Detects corrupted mesh or uninitialized render resources during Undo/Redo
	 *
	 * Checks:
	 * - Render resource existence
	 * - LOD data existence
	 * - Vertex buffer validity
	 * - Skeleton bone validity
	 * - Parent index integrity (prevents EnsureParentsExist crash)
	 *
	 * @param Mesh Skeletal mesh to check
	 * @param bLogWarnings If true, output warning log on failure
	 * @return true if mesh is valid
	 */
	FLESHRINGRUNTIME_API bool IsSkeletalMeshValid(USkeletalMesh* Mesh, bool bLogWarnings = false);
}
