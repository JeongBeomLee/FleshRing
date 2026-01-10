// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingProceduralMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingProceduralMesh, Log, All);

namespace FleshRingProceduralMesh
{

void GenerateBandMesh(
	const FProceduralBandSettings& Settings,
	TArray<FVector3f>& OutVertices,
	TArray<uint32>& OutIndices)
{
	OutVertices.Reset();
	OutIndices.Reset();

	const int32 RadialSegs = Settings.RadialSegments;
	const float MidUpperRadius = Settings.MidUpperRadius;
	const float MidLowerRadius = Settings.MidLowerRadius;
	const float BandHeight = Settings.BandHeight;
	const float Thickness = Settings.BandThickness;

	// ========================================
	// 높이 레이어 정의 (Z=0부터 위로) - 4개의 반경 사용
	// ========================================
	//
	// Layer 3: Upper 끝        ╱──╲      ← Upper.Radius
	// Layer 2: Band-Upper 경계 │  │      ← MidUpperRadius
	// Layer 1: Lower-Band 경계 │  │      ← MidLowerRadius
	// Layer 0: Lower 끝        ╲──╱      ← Lower.Radius
	//

	struct FLayerInfo
	{
		float Z;           // 높이 (로컬 스페이스)
		float OuterRadius; // 외부 반경
		float InnerRadius; // 내부 반경 (구멍)
	};

	TArray<FLayerInfo> Layers;

	float CurrentZ = 0.0f;
	const float HeightEpsilon = 0.0001f;
	const bool bHasLowerSection = (Settings.Lower.Height > HeightEpsilon);
	const bool bHasUpperSection = (Settings.Upper.Height > HeightEpsilon);

	// Layer 0: 하단 끝
	// Lower.Height=0이면 Lower 섹션 없음 → MidLowerRadius 사용
	if (bHasLowerSection)
	{
		Layers.Add({ CurrentZ, Settings.Lower.Radius, Settings.Lower.Radius - Thickness });
		CurrentZ += Settings.Lower.Height;
	}

	// Layer 1: 하단-밴드 경계 (MidLowerRadius)
	Layers.Add({ CurrentZ, MidLowerRadius, MidLowerRadius - Thickness });

	// Layer 2: 밴드-상단 경계 (MidUpperRadius)
	CurrentZ += BandHeight;
	Layers.Add({ CurrentZ, MidUpperRadius, MidUpperRadius - Thickness });

	// Layer 3: 상단 끝
	// Upper.Height=0이면 Upper 섹션 없음 → MidUpperRadius 사용
	if (bHasUpperSection)
	{
		CurrentZ += Settings.Upper.Height;
		Layers.Add({ CurrentZ, Settings.Upper.Radius, Settings.Upper.Radius - Thickness });
	}

	// ========================================
	// 버텍스 생성
	// ========================================
	// 각 레이어에 대해 외부 링과 내부 링 생성
	// 추가로, 반경 변화가 있는 레이어 경계에서는 환형 수평면을 위한 추가 버텍스 생성
	//
	// 버텍스 구조 (각 레이어당):
	// - 기본: Outer, Inner (각 RadialSegs개)
	// - 추가 (다음 레이어와 반경이 다른 경우): PrevOuter, PrevInner (환형면용)
	//
	// 예시 (Lower.Radius > MidLowerRadius인 경우):
	// Layer 1 (MidLowerRadius)에서:
	//   - Outer: MidLowerRadius (기본)
	//   - Inner: MidLowerRadius - Thickness (기본)
	//   - PrevOuter: Lower.Radius (Layer 0의 외부 반경, 환형면용)
	//   - PrevInner: Lower.Radius - Thickness (Layer 0의 내부 반경, 환형면용)

	// 먼저 환형면 필요 여부 결정
	TArray<bool> bNeedsOuterAnnular;
	TArray<bool> bNeedsInnerAnnular;
	bNeedsOuterAnnular.SetNum(Layers.Num());
	bNeedsInnerAnnular.SetNum(Layers.Num());

	for (int32 LayerIdx = 0; LayerIdx < Layers.Num(); LayerIdx++)
	{
		bNeedsOuterAnnular[LayerIdx] = false;
		bNeedsInnerAnnular[LayerIdx] = false;

		if (LayerIdx > 0)
		{
			const FLayerInfo& PrevLayer = Layers[LayerIdx - 1];
			const FLayerInfo& CurrLayer = Layers[LayerIdx];

			// 이전 레이어와 외부 반경이 다르면 환형면 필요
			if (!FMath::IsNearlyEqual(PrevLayer.OuterRadius, CurrLayer.OuterRadius, 0.01f))
			{
				bNeedsOuterAnnular[LayerIdx] = true;
			}
			// 이전 레이어와 내부 반경이 다르면 환형면 필요
			if (!FMath::IsNearlyEqual(PrevLayer.InnerRadius, CurrLayer.InnerRadius, 0.01f))
			{
				bNeedsInnerAnnular[LayerIdx] = true;
			}
		}
	}

	// 각 레이어의 버텍스 시작 인덱스 저장
	TArray<int32> LayerBaseIndices;
	LayerBaseIndices.SetNum(Layers.Num());

	// ========================================
	// 버텍스 생성 (Non-Interleaved 방식)
	// ========================================
	// 레이어별 버텍스 배치:
	//   [Outer0, Outer1, ..., OuterN-1]      <- RadialSegs개
	//   [Inner0, Inner1, ..., InnerN-1]      <- RadialSegs개
	//   [PrevOuter0, ..., PrevOuterN-1]      <- RadialSegs개 (있는 경우만)
	//   [PrevInner0, ..., PrevInnerN-1]      <- RadialSegs개 (있는 경우만)
	//
	// 이 방식은 인덱스 계산 함수와 일치함

	for (int32 LayerIdx = 0; LayerIdx < Layers.Num(); LayerIdx++)
	{
		LayerBaseIndices[LayerIdx] = OutVertices.Num();
		const FLayerInfo& Layer = Layers[LayerIdx];

		// 1. 모든 Outer 버텍스
		for (int32 RadIdx = 0; RadIdx < RadialSegs; RadIdx++)
		{
			const float Angle = 2.0f * PI * RadIdx / RadialSegs;
			OutVertices.Add(FVector3f(
				Layer.OuterRadius * FMath::Cos(Angle),
				Layer.OuterRadius * FMath::Sin(Angle),
				Layer.Z
			));
		}

		// 2. 모든 Inner 버텍스
		for (int32 RadIdx = 0; RadIdx < RadialSegs; RadIdx++)
		{
			const float Angle = 2.0f * PI * RadIdx / RadialSegs;
			OutVertices.Add(FVector3f(
				Layer.InnerRadius * FMath::Cos(Angle),
				Layer.InnerRadius * FMath::Sin(Angle),
				Layer.Z
			));
		}

		// 3. 모든 PrevOuter 버텍스 (환형면 필요 시)
		if (bNeedsOuterAnnular[LayerIdx])
		{
			const FLayerInfo& PrevLayer = Layers[LayerIdx - 1];
			for (int32 RadIdx = 0; RadIdx < RadialSegs; RadIdx++)
			{
				const float Angle = 2.0f * PI * RadIdx / RadialSegs;
				OutVertices.Add(FVector3f(
					PrevLayer.OuterRadius * FMath::Cos(Angle),
					PrevLayer.OuterRadius * FMath::Sin(Angle),
					Layer.Z  // 현재 레이어의 Z!
				));
			}
		}

		// 4. 모든 PrevInner 버텍스 (환형면 필요 시)
		if (bNeedsInnerAnnular[LayerIdx])
		{
			const FLayerInfo& PrevLayer = Layers[LayerIdx - 1];
			for (int32 RadIdx = 0; RadIdx < RadialSegs; RadIdx++)
			{
				const float Angle = 2.0f * PI * RadIdx / RadialSegs;
				OutVertices.Add(FVector3f(
					PrevLayer.InnerRadius * FMath::Cos(Angle),
					PrevLayer.InnerRadius * FMath::Sin(Angle),
					Layer.Z  // 현재 레이어의 Z!
				));
			}
		}
	}

	// ========================================
	// 버텍스 인덱스 계산 헬퍼
	// ========================================
	// Non-Interleaved 배치에 맞춤:
	//   Outer:     Base + RadIdx
	//   Inner:     Base + RadialSegs + RadIdx
	//   PrevOuter: Base + RadialSegs*2 + RadIdx
	//   PrevInner: Base + RadialSegs*2 + (PrevOuter있으면 RadialSegs) + RadIdx

	auto GetOuterIdx = [&](int32 LayerIdx, int32 RadIdx) -> int32 {
		return LayerBaseIndices[LayerIdx] + RadIdx;
	};
	auto GetInnerIdx = [&](int32 LayerIdx, int32 RadIdx) -> int32 {
		return LayerBaseIndices[LayerIdx] + RadialSegs + RadIdx;
	};
	auto GetPrevOuterIdx = [&](int32 LayerIdx, int32 RadIdx) -> int32 {
		return LayerBaseIndices[LayerIdx] + RadialSegs * 2 + RadIdx;
	};
	auto GetPrevInnerIdx = [&](int32 LayerIdx, int32 RadIdx) -> int32 {
		int32 Base = LayerBaseIndices[LayerIdx] + RadialSegs * 2;
		if (bNeedsOuterAnnular[LayerIdx]) Base += RadialSegs;
		return Base + RadIdx;
	};

	// ========================================
	// 인덱스 생성 (삼각형)
	// ========================================

	for (int32 LayerIdx = 0; LayerIdx < Layers.Num() - 1; LayerIdx++)
	{
		const int32 NextLayerIdx = LayerIdx + 1;

		for (int32 RadIdx = 0; RadIdx < RadialSegs; RadIdx++)
		{
			const int32 NextRadIdx = (RadIdx + 1) % RadialSegs;

			// 헬퍼로 버텍스 인덱스 가져오기
			const int32 OuterCurr = GetOuterIdx(LayerIdx, RadIdx);
			const int32 OuterNext = GetOuterIdx(LayerIdx, NextRadIdx);
			const int32 OuterCurrUp = GetOuterIdx(NextLayerIdx, RadIdx);
			const int32 OuterNextUp = GetOuterIdx(NextLayerIdx, NextRadIdx);

			const int32 InnerCurr = GetInnerIdx(LayerIdx, RadIdx);
			const int32 InnerNext = GetInnerIdx(LayerIdx, NextRadIdx);
			const int32 InnerCurrUp = GetInnerIdx(NextLayerIdx, RadIdx);
			const int32 InnerNextUp = GetInnerIdx(NextLayerIdx, NextRadIdx);

			// ===== 외부 면 =====
			// 환형면이 필요한 경우: PrevOuter를 경유
			// 환형면이 필요 없는 경우: 직접 연결
			if (bNeedsOuterAnnular[NextLayerIdx])
			{
				// 다음 레이어에 PrevOuter 버텍스가 있음 (현재 레이어 반경, 다음 레이어 Z)
				const int32 PrevOuterCurrUp = GetPrevOuterIdx(NextLayerIdx, RadIdx);
				const int32 PrevOuterNextUp = GetPrevOuterIdx(NextLayerIdx, NextRadIdx);

				// 외부 경사면: OuterCurr → PrevOuterUp (같은 반경, 다른 Z = 수직면)
				// 삼각형 1
				OutIndices.Add(OuterCurr);
				OutIndices.Add(PrevOuterCurrUp);
				OutIndices.Add(OuterNext);
				// 삼각형 2
				OutIndices.Add(OuterNext);
				OutIndices.Add(PrevOuterCurrUp);
				OutIndices.Add(PrevOuterNextUp);

				// 환형 수평면: PrevOuterUp → OuterUp (다른 반경, 같은 Z = 수평면)
				const FLayerInfo& CurrLayer = Layers[LayerIdx];
				const FLayerInfo& NextLayer = Layers[NextLayerIdx];

				if (CurrLayer.OuterRadius > NextLayer.OuterRadius)
				{
					// 위로 갈수록 좁아짐: 아래 방향 노멀 (링 외부에서 봤을 때)
					// 삼각형 1: PrevOuterCurrUp → OuterNextUp → OuterCurrUp
					OutIndices.Add(PrevOuterCurrUp);
					OutIndices.Add(OuterNextUp);
					OutIndices.Add(OuterCurrUp);
					// 삼각형 2: PrevOuterCurrUp → PrevOuterNextUp → OuterNextUp
					OutIndices.Add(PrevOuterCurrUp);
					OutIndices.Add(PrevOuterNextUp);
					OutIndices.Add(OuterNextUp);
				}
				else
				{
					// 위로 갈수록 넓어짐: 위 방향 노멀
					// 삼각형 1: OuterCurrUp → OuterNextUp → PrevOuterCurrUp
					OutIndices.Add(OuterCurrUp);
					OutIndices.Add(OuterNextUp);
					OutIndices.Add(PrevOuterCurrUp);
					// 삼각형 2: OuterNextUp → PrevOuterNextUp → PrevOuterCurrUp
					OutIndices.Add(OuterNextUp);
					OutIndices.Add(PrevOuterNextUp);
					OutIndices.Add(PrevOuterCurrUp);
				}
			}
			else
			{
				// 환형면 불필요: 직접 연결 (기존 로직)
				// 삼각형 1
				OutIndices.Add(OuterCurr);
				OutIndices.Add(OuterCurrUp);
				OutIndices.Add(OuterNext);
				// 삼각형 2
				OutIndices.Add(OuterNext);
				OutIndices.Add(OuterCurrUp);
				OutIndices.Add(OuterNextUp);
			}

			// ===== 내부 면 =====
			if (bNeedsInnerAnnular[NextLayerIdx])
			{
				// 다음 레이어에 PrevInner 버텍스가 있음
				const int32 PrevInnerCurrUp = GetPrevInnerIdx(NextLayerIdx, RadIdx);
				const int32 PrevInnerNextUp = GetPrevInnerIdx(NextLayerIdx, NextRadIdx);

				// 내부 수직면: InnerCurr → PrevInnerUp
				// 삼각형 1 (CW 와인딩, 안쪽 방향 노멀)
				OutIndices.Add(InnerCurr);
				OutIndices.Add(InnerNext);
				OutIndices.Add(PrevInnerCurrUp);
				// 삼각형 2
				OutIndices.Add(InnerNext);
				OutIndices.Add(PrevInnerNextUp);
				OutIndices.Add(PrevInnerCurrUp);

				// 환형 수평면: PrevInnerUp → InnerUp
				const FLayerInfo& CurrLayer = Layers[LayerIdx];
				const FLayerInfo& NextLayer = Layers[NextLayerIdx];

				if (CurrLayer.InnerRadius > NextLayer.InnerRadius)
				{
					// 위로 갈수록 구멍이 작아짐: 위 방향 노멀 (구멍 안에서 봤을 때)
					// 삼각형 1: InnerCurrUp → PrevInnerCurrUp → InnerNextUp
					OutIndices.Add(InnerCurrUp);
					OutIndices.Add(PrevInnerCurrUp);
					OutIndices.Add(InnerNextUp);
					// 삼각형 2: InnerNextUp → PrevInnerCurrUp → PrevInnerNextUp
					OutIndices.Add(InnerNextUp);
					OutIndices.Add(PrevInnerCurrUp);
					OutIndices.Add(PrevInnerNextUp);
				}
				else
				{
					// 위로 갈수록 구멍이 커짐: 아래 방향 노멀
					// 삼각형 1: PrevInnerCurrUp → InnerCurrUp → PrevInnerNextUp
					OutIndices.Add(PrevInnerCurrUp);
					OutIndices.Add(InnerCurrUp);
					OutIndices.Add(PrevInnerNextUp);
					// 삼각형 2: PrevInnerNextUp → InnerCurrUp → InnerNextUp
					OutIndices.Add(PrevInnerNextUp);
					OutIndices.Add(InnerCurrUp);
					OutIndices.Add(InnerNextUp);
				}
			}
			else
			{
				// 환형면 불필요: 직접 연결 (기존 로직)
				// 삼각형 1
				OutIndices.Add(InnerCurr);
				OutIndices.Add(InnerNext);
				OutIndices.Add(InnerCurrUp);
				// 삼각형 2
				OutIndices.Add(InnerNext);
				OutIndices.Add(InnerNextUp);
				OutIndices.Add(InnerCurrUp);
			}
		}
	}

	// ========================================
	// 상단/하단 캡은 생성하지 않음
	// ========================================
	// SDF 생성 시 완전히 닫힌 캡이 있으면 구멍 내부가 "내부"로 잘못 판정됨
	// 튜브의 위/아래는 열린 형태로 유지
	// 단, 레이어 간 반경 변화가 있는 경계에서는 환형 수평면이 필요함 (위에서 추가됨)

	// ========================================
	// 디버그 출력
	// ========================================
	UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("=== VirtualBand Mesh Generated ==="));
	UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("Settings: MidUpper=%.2f, MidLower=%.2f, Thickness=%.2f, Lower.Radius=%.2f, Upper.Radius=%.2f"),
		Settings.MidUpperRadius, Settings.MidLowerRadius, Settings.BandThickness, Settings.Lower.Radius, Settings.Upper.Radius);
	UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("Vertices: %d, Triangles: %d"), OutVertices.Num(), OutIndices.Num() / 3);

	// 각 레이어의 버텍스 정보
	for (int32 LayerIdx = 0; LayerIdx < Layers.Num(); LayerIdx++)
	{
		const FLayerInfo& Layer = Layers[LayerIdx];
		UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("Layer[%d]: Z=%.2f, OuterR=%.2f, InnerR=%.2f, NeedsOuterAnnular=%d, NeedsInnerAnnular=%d"),
			LayerIdx, Layer.Z, Layer.OuterRadius, Layer.InnerRadius,
			bNeedsOuterAnnular[LayerIdx] ? 1 : 0, bNeedsInnerAnnular[LayerIdx] ? 1 : 0);

		// 샘플 버텍스 위치 출력
		if (LayerIdx < Layers.Num())
		{
			int32 OuterIdx = GetOuterIdx(LayerIdx, 0);
			int32 InnerIdx = GetInnerIdx(LayerIdx, 0);
			if (OuterIdx < OutVertices.Num() && InnerIdx < OutVertices.Num())
			{
				FVector3f OuterV = OutVertices[OuterIdx];
				FVector3f InnerV = OutVertices[InnerIdx];
				float OuterR = FMath::Sqrt(OuterV.X * OuterV.X + OuterV.Y * OuterV.Y);
				float InnerR = FMath::Sqrt(InnerV.X * InnerV.X + InnerV.Y * InnerV.Y);
				UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("  -> Outer[0] idx=%d pos=(%.2f,%.2f,%.2f) R=%.2f"),
					OuterIdx, OuterV.X, OuterV.Y, OuterV.Z, OuterR);
				UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("  -> Inner[0] idx=%d pos=(%.2f,%.2f,%.2f) R=%.2f"),
					InnerIdx, InnerV.X, InnerV.Y, InnerV.Z, InnerR);
			}

			// PrevOuter/PrevInner 출력 (있는 경우)
			if (bNeedsOuterAnnular[LayerIdx])
			{
				int32 PrevOuterIdx = GetPrevOuterIdx(LayerIdx, 0);
				if (PrevOuterIdx < OutVertices.Num())
				{
					FVector3f PrevOuterV = OutVertices[PrevOuterIdx];
					float PrevOuterR = FMath::Sqrt(PrevOuterV.X * PrevOuterV.X + PrevOuterV.Y * PrevOuterV.Y);
					UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("  -> PrevOuter[0] idx=%d pos=(%.2f,%.2f,%.2f) R=%.2f"),
						PrevOuterIdx, PrevOuterV.X, PrevOuterV.Y, PrevOuterV.Z, PrevOuterR);
				}
			}
			if (bNeedsInnerAnnular[LayerIdx])
			{
				int32 PrevInnerIdx = GetPrevInnerIdx(LayerIdx, 0);
				if (PrevInnerIdx < OutVertices.Num())
				{
					FVector3f PrevInnerV = OutVertices[PrevInnerIdx];
					float PrevInnerR = FMath::Sqrt(PrevInnerV.X * PrevInnerV.X + PrevInnerV.Y * PrevInnerV.Y);
					UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("  -> PrevInner[0] idx=%d pos=(%.2f,%.2f,%.2f) R=%.2f"),
						PrevInnerIdx, PrevInnerV.X, PrevInnerV.Y, PrevInnerV.Z, PrevInnerR);
				}
			}
		}
	}
	UE_LOG(LogFleshRingProceduralMesh, Log, TEXT("====================================="));
}

FBox3f CalculateBandBounds(const FProceduralBandSettings& Settings)
{
	const float MaxRadius = Settings.GetMaxRadius();
	const float TotalHeight = Settings.GetTotalHeight();

	// Auto 모드와 동일하게 패딩 없이 메시의 실제 bounds 사용
	// (Auto 모드도 BoundsPadding = 0.0f)
	return FBox3f(
		FVector3f(-MaxRadius, -MaxRadius, 0.0f),
		FVector3f(MaxRadius, MaxRadius, TotalHeight)
	);
}

void GenerateWireframeLines(
	const FProceduralBandSettings& Settings,
	TArray<TPair<FVector, FVector>>& OutLines,
	int32 NumSegments)
{
	OutLines.Reset();

	// 높이 레이어 정의 (Height=0인 섹션은 스킵하고 Mid 값 사용)
	struct FLayerInfo { float Z; float Radius; };
	TArray<FLayerInfo> Layers;

	const float HeightEpsilon = 0.0001f;
	const bool bHasLowerSection = (Settings.Lower.Height > HeightEpsilon);
	const bool bHasUpperSection = (Settings.Upper.Height > HeightEpsilon);

	float CurrentZ = 0.0f;

	// Lower 섹션이 있는 경우에만 Lower.Radius 레이어 추가
	if (bHasLowerSection)
	{
		Layers.Add({ CurrentZ, Settings.Lower.Radius });
		CurrentZ += Settings.Lower.Height;
	}

	Layers.Add({ CurrentZ, Settings.MidLowerRadius });

	CurrentZ += Settings.BandHeight;
	Layers.Add({ CurrentZ, Settings.MidUpperRadius });

	// Upper 섹션이 있는 경우에만 Upper.Radius 레이어 추가
	if (bHasUpperSection)
	{
		CurrentZ += Settings.Upper.Height;
		Layers.Add({ CurrentZ, Settings.Upper.Radius });
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

} // namespace FleshRingProceduralMesh
