// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.h"

/**
 * Virtual band visualization utility
 *
 * Generates wireframe of asymmetric cylinder for stockings/tights.
 * Used for editor preview visualization.
 */
namespace FleshRingVirtualBandMesh
{
	/**
	 * Generate wireframe data for debug visualization
	 *
	 * @param Settings Band settings
	 * @param OutLines Output line array (start point, end point pairs)
	 * @param NumSegments Number of circular segments (default 24)
	 */
	void GenerateWireframeLines(
		const FVirtualBandSettings& Settings,
		TArray<TPair<FVector, FVector>>& OutLines,
		int32 NumSegments = 24);
}
