// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "FleshRingDebugTypes.h"

/**
 * FFleshRingDebugPointVS - Debug Point Vertex Shader
 * GPU 원형 디버그 포인트 렌더링용 버텍스 셰이더
 *
 * TightnessCS에서 생성된 디버그 포인트를 스크린 스페이스 빌보드로 변환.
 * 인스턴스드 렌더링: 쿼드당 4개 버텍스, N개 인스턴스 (포인트).
 */
class FFleshRingDebugPointVS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingDebugPointVS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingDebugPointVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Input: Debug points from TightnessCS
        // 입력: TightnessCS에서 생성된 디버그 포인트
        // RHI SRV 직접 사용 (RDG SRV 바인딩 문제 우회)
        SHADER_PARAMETER_SRV(StructuredBuffer<FFleshRingDebugPoint>, DebugPoints)

        // View-Projection matrix for world → clip space transformation
        // 월드 → 클립 스페이스 변환용 뷰프로젝션 행렬
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)

        // 1/ViewportSize for pixel → NDC conversion
        // 픽셀 → NDC 변환용 1/ViewportSize
        SHADER_PARAMETER(FVector2f, InvViewportSize)

        // Base point size in pixels
        // 기본 포인트 크기 (픽셀)
        SHADER_PARAMETER(float, PointSizeBase)

        // Additional size based on Influence value
        // Influence 값에 따른 추가 크기
        SHADER_PARAMETER(float, PointSizeInfluence)

        // Color mode: 0 = Tightness (Blue→Green→Red), 1 = Bulge (Cyan→Magenta)
        // 색상 모드: 0 = Tightness (파랑→초록→빨강), 1 = Bulge (청록→마젠타)
        SHADER_PARAMETER(uint32, ColorMode)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        // Require SM5 for structured buffer support
        // 구조화 버퍼 지원을 위해 SM5 필요
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

/**
 * FFleshRingDebugPointPS - Debug Point Pixel Shader
 * GPU 원형 디버그 포인트 렌더링용 픽셀 셰이더
 *
 * Renders circular points with influence-based color gradient:
 * Blue (0) → Green (0.5) → Red (1)
 * Influence 기반 색상 그라데이션으로 원형 포인트를 렌더링합니다:
 * 파랑 (0) → 초록 (0.5) → 빨강 (1)
 */
class FFleshRingDebugPointPS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingDebugPointPS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingDebugPointPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // RDG 리소스 트래킹용 버퍼 (RDG가 리소스 상태 전환을 올바르게 수행하도록)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FFleshRingDebugPoint>, DebugPointsRDG)

        // Ring 가시성 필터링용 비트마스크 배열 (무제한 Ring 지원)
        // 각 uint32 요소는 32개 Ring의 가시성 비트마스크
        // 요소[0] = Ring 0-31, 요소[1] = Ring 32-63, ...
        // RHI SRV 직접 사용 (lambda 내 바인딩용)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, RingVisibilityMask)
        SHADER_PARAMETER(uint32, NumVisibilityMaskElements)

        // Render target binding for output
        // 출력용 렌더 타겟 바인딩
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        // Require SM5 for consistent rendering
        // 일관된 렌더링을 위해 SM5 필요
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};
