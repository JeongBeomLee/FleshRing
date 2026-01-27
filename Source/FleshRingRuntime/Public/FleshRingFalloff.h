// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingFalloff.generated.h"

/**
 * Falloff curve type (shared by Tightness + Bulge)
 *
 * Unified enum used for all distance-based attenuation
 * Single Source of Truth - computation and visualization use same function
 */
UENUM(BlueprintType)
enum class EFleshRingFalloffType : uint8
{
	/** Linear attenuation: f(t) = t */
	Linear			UMETA(DisplayName = "Linear"),

	/** Quadratic curve attenuation: f(t) = t^2 */
	Quadratic		UMETA(DisplayName = "Quadratic"),

	/** Hermite S-curve: f(t) = t^2 * (3 - 2t) - C1 continuous */
	Hermite			UMETA(DisplayName = "Hermite (S-Curve)"),

	/** Wendland C2 kernel: f(t) = (1-q)^4 * (4q+1) - SPH/PBD physics standard, C2 continuous */
	WendlandC2		UMETA(DisplayName = "Wendland C2 (Physics)"),

	/** Perlin smootherstep: f(t) = t^3 * (t*(6t-15)+10) - C2 continuous */
	Smootherstep	UMETA(DisplayName = "Smootherstep (C2)")
};

/**
 * Falloff function utility
 *
 * Single Source of Truth for all Falloff calculations
 * Tightness, Bulge, and visualization all call this single function
 *
 * ARCHITECTURE NOTE:
 * All falloff calculations MUST use FFleshRingFalloff::Evaluate()
 * DO NOT hardcode falloff formulas elsewhere.
 * This ensures visualization matches computation.
 */
struct FLESHRINGRUNTIME_API FFleshRingFalloff
{
	/**
	 * Calculate Falloff value for normalized distance (0~1)
	 *
	 * @param NormalizedDistance 0.0 = center (max influence), 1.0 = boundary (no influence)
	 * @param Type Falloff curve type
	 * @return Influence between 0.0 ~ 1.0 (1 = max, 0 = none)
	 *
	 * Note: Input outside [0,1] range is clamped
	 */
	static float Evaluate(float NormalizedDistance, EFleshRingFalloffType Type)
	{
		// NormalizedDistance: 0 = close (max influence), 1 = far (no influence)
		// t: influence (1 = max, 0 = none)
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
				// q = NormalizedDistance (0 = center, 1 = boundary)
				// Mathematical basis: SPH/PBD physics simulation standard kernel
				const float OneMinusQ = 1.0f - q;
				return OneMinusQ * OneMinusQ * OneMinusQ * OneMinusQ * (4.0f * q + 1.0f);
			}

		case EFleshRingFalloffType::Smootherstep:
			// Perlin's smootherstep: t^3 * (t*(6t-15)+10)
			// C2 continuous (continuous up to second derivative)
			return t * t * t * (t * (6.0f * t - 15.0f) + 10.0f);

		default:
			return t;
		}
	}

	/**
	 * Return Falloff type name (for debug/logging)
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
