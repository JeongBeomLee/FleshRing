// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingFalloff.generated.h"

/**
 * Falloff 커브 타입 (Tightness + Bulge 공용)
 *
 * 모든 거리 기반 감쇠에 사용되는 통합 enum
 * Single Source of Truth - 계산과 시각화가 동일 함수 사용
 */
UENUM(BlueprintType)
enum class EFleshRingFalloffType : uint8
{
	/** 선형 감쇠: f(t) = t */
	Linear			UMETA(DisplayName = "Linear"),

	/** 2차 곡선 감쇠: f(t) = t^2 */
	Quadratic		UMETA(DisplayName = "Quadratic"),

	/** Hermite S-커브: f(t) = t^2 * (3 - 2t) - C1 연속 */
	Hermite			UMETA(DisplayName = "Hermite (S-Curve)"),

	/** Wendland C2 커널: f(t) = (1-q)^4 * (4q+1) - SPH/PBD 물리 표준, C2 연속 */
	WendlandC2		UMETA(DisplayName = "Wendland C2 (Physics)"),

	/** Perlin smootherstep: f(t) = t^3 * (t*(6t-15)+10) - C2 연속 */
	Smootherstep	UMETA(DisplayName = "Smootherstep (C2)")
};

/**
 * Falloff 함수 유틸리티
 *
 * 모든 Falloff 계산의 Single Source of Truth
 * Tightness, Bulge, 시각화 전부 이 함수 하나만 호출
 *
 * ARCHITECTURE NOTE:
 * All falloff calculations MUST use FFleshRingFalloff::Evaluate()
 * DO NOT hardcode falloff formulas elsewhere.
 * This ensures visualization matches computation.
 */
struct FLESHRINGRUNTIME_API FFleshRingFalloff
{
	/**
	 * 정규화된 거리(0~1)에 대한 Falloff 값 계산
	 *
	 * @param NormalizedDistance 0.0 = 중심(최대 영향), 1.0 = 경계(영향 없음)
	 * @param Type Falloff 커브 타입
	 * @return 0.0 ~ 1.0 사이의 영향도 (1 = 최대, 0 = 없음)
	 *
	 * 주의: 입력이 [0,1] 범위 밖이면 클램프됨
	 */
	static float Evaluate(float NormalizedDistance, EFleshRingFalloffType Type)
	{
		// NormalizedDistance: 0 = 가까움(최대 영향), 1 = 멂(영향 없음)
		// t: 영향도 (1 = 최대, 0 = 없음)
		const float t = FMath::Clamp(1.0f - NormalizedDistance, 0.0f, 1.0f);
		const float q = FMath::Clamp(NormalizedDistance, 0.0f, 1.0f);

		switch (Type)
		{
		case EFleshRingFalloffType::Linear:
			return t;

		case EFleshRingFalloffType::Quadratic:
			return t * t;

		case EFleshRingFalloffType::Hermite:
			// Hermite smoothstep: t^2 * (3 - 2t)
			return t * t * (3.0f - 2.0f * t);

		case EFleshRingFalloffType::WendlandC2:
			{
				// Wendland C2 Kernel: (1-q)^4 * (4q+1)
				// q = NormalizedDistance (0 = 중심, 1 = 경계)
				// 수학적 근거: SPH/PBD 물리 시뮬레이션 표준 커널
				const float OneMinusQ = 1.0f - q;
				return OneMinusQ * OneMinusQ * OneMinusQ * OneMinusQ * (4.0f * q + 1.0f);
			}

		case EFleshRingFalloffType::Smootherstep:
			// Perlin's smootherstep: t^3 * (t*(6t-15)+10)
			// C2 연속 (2차 미분까지 연속)
			return t * t * t * (t * (6.0f * t - 15.0f) + 10.0f);

		default:
			return t;
		}
	}

	/**
	 * Falloff 타입 이름 반환 (디버그/로깅용)
	 */
	static const TCHAR* GetTypeName(EFleshRingFalloffType Type)
	{
		switch (Type)
		{
		case EFleshRingFalloffType::Linear:       return TEXT("Linear");
		case EFleshRingFalloffType::Quadratic:    return TEXT("Quadratic");
		case EFleshRingFalloffType::Hermite:      return TEXT("Hermite");
		case EFleshRingFalloffType::WendlandC2:   return TEXT("WendlandC2");
		case EFleshRingFalloffType::Smootherstep: return TEXT("Smootherstep");
		default:                                   return TEXT("Unknown");
		}
	}
};
