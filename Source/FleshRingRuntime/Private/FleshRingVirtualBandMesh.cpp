// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingVirtualBandMesh.h"

namespace FleshRingVirtualBandMesh
{

void GenerateWireframeLines(
	const FVirtualBandSettings& Settings,
	TArray<TPair<FVector, FVector>>& OutLines,
	int32 NumSegments)
{
	OutLines.Reset();

	// Coordinate system: Z=0 is the center of the Mid Band
	const float MidOffset = Settings.GetMidOffset();

	// Height layer definition (sections with Height=0 are skipped, using Mid values instead)
	struct FLayerInfo { float Z; float Radius; };
	TArray<FLayerInfo> Layers;

	const float HeightEpsilon = 0.0001f;
	const bool bHasLowerSection = (Settings.Lower.Height > HeightEpsilon);
	const bool bHasUpperSection = (Settings.Upper.Height > HeightEpsilon);

	// Calculate using internal coordinates, then subtract MidOffset to convert to new coordinates
	float InternalZ = 0.0f;

	// Add Lower.Radius layer only if the Lower section exists
	if (bHasLowerSection)
	{
		Layers.Add({ InternalZ - MidOffset, Settings.Lower.Radius });
		InternalZ += Settings.Lower.Height;
	}

	Layers.Add({ InternalZ - MidOffset, Settings.MidLowerRadius });

	InternalZ += Settings.BandHeight;
	Layers.Add({ InternalZ - MidOffset, Settings.MidUpperRadius });

	// Add Upper.Radius layer only if the Upper section exists
	if (bHasUpperSection)
	{
		InternalZ += Settings.Upper.Height;
		Layers.Add({ InternalZ - MidOffset, Settings.Upper.Radius });
	}

	// Circular wireframe for each layer
	for (const FLayerInfo& Layer : Layers)
	{
		for (int32 i = 0; i < NumSegments; i++)
		{
			const float Angle1 = 2.0f * PI * i / NumSegments;
			const float Angle2 = 2.0f * PI * (i + 1) / NumSegments;

			FVector P1(
				Layer.Radius * FMath::Cos(Angle1),
				Layer.Radius * FMath::Sin(Angle1),
				Layer.Z
			);
			FVector P2(
				Layer.Radius * FMath::Cos(Angle2),
				Layer.Radius * FMath::Sin(Angle2),
				Layer.Z
			);

			OutLines.Add(TPair<FVector, FVector>(P1, P2));
		}
	}

	// Vertical lines between layers (4 directions)
	for (int32 i = 0; i < 4; i++)
	{
		const float Angle = PI * 0.5f * i;

		for (int32 j = 0; j < Layers.Num() - 1; j++)
		{
			FVector P1(
				Layers[j].Radius * FMath::Cos(Angle),
				Layers[j].Radius * FMath::Sin(Angle),
				Layers[j].Z
			);
			FVector P2(
				Layers[j + 1].Radius * FMath::Cos(Angle),
				Layers[j + 1].Radius * FMath::Sin(Angle),
				Layers[j + 1].Z
			);

			OutLines.Add(TPair<FVector, FVector>(P1, P2));
		}
	}
}

} // namespace FleshRingVirtualBandMesh
