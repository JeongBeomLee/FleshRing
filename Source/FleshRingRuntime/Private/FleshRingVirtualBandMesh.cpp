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

	// 좌표계: Z=0이 Mid Band 중심
	const float MidOffset = Settings.GetMidOffset();

	// 높이 레이어 정의 (Height=0인 섹션은 스킵하고 Mid 값 사용)
	struct FLayerInfo { float Z; float Radius; };
	TArray<FLayerInfo> Layers;

	const float HeightEpsilon = 0.0001f;
	const bool bHasLowerSection = (Settings.Lower.Height > HeightEpsilon);
	const bool bHasUpperSection = (Settings.Upper.Height > HeightEpsilon);

	// 내부 좌표로 계산 후 MidOffset을 빼서 새 좌표로 변환
	float InternalZ = 0.0f;

	// Lower 섹션이 있는 경우에만 Lower.Radius 레이어 추가
	if (bHasLowerSection)
	{
		Layers.Add({ InternalZ - MidOffset, Settings.Lower.Radius });
		InternalZ += Settings.Lower.Height;
	}

	Layers.Add({ InternalZ - MidOffset, Settings.MidLowerRadius });

	InternalZ += Settings.BandHeight;
	Layers.Add({ InternalZ - MidOffset, Settings.MidUpperRadius });

	// Upper 섹션이 있는 경우에만 Upper.Radius 레이어 추가
	if (bHasUpperSection)
	{
		InternalZ += Settings.Upper.Height;
		Layers.Add({ InternalZ - MidOffset, Settings.Upper.Radius });
	}

	// 각 레이어의 원형 와이어프레임
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

	// 레이어 간 수직선 (4방향)
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
