// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDebugViewExtension.h"
#include "FleshRingDebugPointShader.h"
#include "FleshRingDebugTypes.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHIStaticStates.h"
#include "PipelineStateCache.h"
#include "CommonRenderResources.h"
#include "SceneView.h"

FFleshRingDebugViewExtension::FFleshRingDebugViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
    : FSceneViewExtensionBase(AutoRegister)
    , BoundWorld(InWorld)
{
}

FFleshRingDebugViewExtension::~FFleshRingDebugViewExtension()
{
    ClearDebugPointBuffer();
}

void FFleshRingDebugViewExtension::SetDebugPointBuffer(TRefCountPtr<FRDGPooledBuffer> InBuffer, uint32 InPointCount)
{
    FScopeLock Lock(&BufferLock);
    DebugPointBuffer = InBuffer;
    PointCount = InPointCount;
    bEnabled = (InBuffer.IsValid() && InPointCount > 0);
}

void FFleshRingDebugViewExtension::ClearDebugPointBuffer()
{
    FScopeLock Lock(&BufferLock);
    DebugPointBuffer = nullptr;
    DebugPointBufferSharedPtr = nullptr;
    PointCount = 0;
    bEnabled = false;
}

void FFleshRingDebugViewExtension::SetDebugPointBufferShared(TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBufferPtr, uint32 InPointCount)
{
    FScopeLock Lock(&BufferLock);
    DebugPointBufferSharedPtr = InBufferPtr;
    PointCount = InPointCount;
    bEnabled = (InBufferPtr.IsValid() && InPointCount > 0);
}

void FFleshRingDebugViewExtension::PostRenderViewFamily_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneViewFamily& InViewFamily)
{
    FRDGBufferRef DebugPointsRDG = nullptr;
    uint32 LocalPointCount = 0;
    float LocalPointSizeBase = PointSizeBase;
    float LocalPointSizeInfluence = PointSizeInfluence;

    // RegisterExternalBuffer를 사용하여 풀링된 버퍼를 RDG에 등록
    TRefCountPtr<FRDGPooledBuffer> LocalBuffer;
    {
        FScopeLock Lock(&BufferLock);
        if (!bEnabled || PointCount == 0)
        {
            return;
        }

        // SharedPtr 방식 우선 사용
        if (DebugPointBufferSharedPtr.IsValid() && DebugPointBufferSharedPtr->IsValid())
        {
            LocalBuffer = *DebugPointBufferSharedPtr;
        }
        else if (DebugPointBuffer.IsValid())
        {
            LocalBuffer = DebugPointBuffer;
        }
        else
        {
            return;
        }

        LocalPointCount = PointCount;
    }

    // Register external buffer
    DebugPointsRDG = GraphBuilder.RegisterExternalBuffer(LocalBuffer, TEXT("FleshRingDebugPoints"));

    // Get first view for rendering parameters
    if (InViewFamily.Views.Num() == 0 || !InViewFamily.Views[0])
    {
        return;
    }

    const FSceneView* View = InViewFamily.Views[0];

    // Get shader references
    TShaderMapRef<FFleshRingDebugPointVS> VertexShader(GetGlobalShaderMap(View->GetFeatureLevel()));
    TShaderMapRef<FFleshRingDebugPointPS> PixelShader(GetGlobalShaderMap(View->GetFeatureLevel()));

    if (!VertexShader.IsValid() || !PixelShader.IsValid())
    {
        return;
    }

    // Calculate view parameters (TAA 지터 없는 행렬 사용)
    // Use unjittered matrix to prevent TAA-related jittering
    FMatrix44f ViewProjectionMatrix = FMatrix44f(View->ViewMatrices.GetViewMatrix() * View->ViewMatrices.GetProjectionNoAAMatrix());
    FIntRect ViewRect = View->UnscaledViewRect;
    FVector2f InvViewportSize(1.0f / FMath::Max(1, ViewRect.Width()), 1.0f / FMath::Max(1, ViewRect.Height()));

    // Get render target from view family
    // 뷰 패밀리에서 렌더 타겟 가져오기
    FRDGTextureRef RenderTarget = nullptr;
    if (InViewFamily.RenderTarget)
    {
        // Try to get the render target texture
        FRHITexture* RHITexture = InViewFamily.RenderTarget->GetRenderTargetTexture();
        if (RHITexture)
        {
            RenderTarget = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(RHITexture, TEXT("FleshRingDebugRT")));
        }
    }

    if (!RenderTarget)
    {
        return;
    }

    // 렌더 타겟의 MSAA 샘플 수 가져오기 (와이어프레임 모드 등 호환성)
    const uint32 NumSamples = RenderTarget->Desc.NumSamples;

    // 디버그 포인트 전용 뎁스 버퍼 생성 (렌더 타겟과 동일한 MSAA 샘플 수)
    // Create2D 파라미터 순서: Extent, Format, ClearValue, Flags, NumMips, NumSamples
    FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
        FIntPoint(ViewRect.Width(), ViewRect.Height()),
        PF_DepthStencil,
        FClearValueBinding::DepthFar,
        TexCreate_DepthStencilTargetable,
        1,            // NumMips = 1 (depth stencil 필수)
        NumSamples);  // NumSamples = 렌더 타겟과 동일
    FRDGTextureRef DebugDepthBuffer = GraphBuilder.CreateTexture(DepthDesc, TEXT("FleshRingDebugDepth"));

    // RDG SRV 생성 (리소스 트래킹 및 RHI SRV 획득용)
    FRDGBufferSRVRef DebugPointsSRV = GraphBuilder.CreateSRV(DebugPointsRDG);

    // Allocate pixel shader parameters (RDG 리소스 트래킹용)
    // PSParameters에 RDG SRV를 포함시켜서 RDG가 버퍼 상태 전환을 올바르게 수행하도록 함
    FFleshRingDebugPointPS::FParameters* PSParameters =
        GraphBuilder.AllocParameters<FFleshRingDebugPointPS::FParameters>();
    PSParameters->DebugPointsRDG = DebugPointsSRV;
    PSParameters->RenderTargets[0] = FRenderTargetBinding(RenderTarget, ERenderTargetLoadAction::ELoad);
    PSParameters->RenderTargets.DepthStencil = FDepthStencilBinding(DebugDepthBuffer, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilNop);

    // Add render pass
    // 렌더 패스 추가
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("FleshRingDebugPoints"),
        PSParameters,
        ERDGPassFlags::Raster,
        [VertexShader, PixelShader, LocalPointCount, ViewRect, DebugPointsSRV,
         ViewProjectionMatrix, InvViewportSize, LocalPointSizeBase, LocalPointSizeInfluence](FRHICommandList& RHICmdList)
        {
            // Set viewport
            // 뷰포트 설정
            RHICmdList.SetViewport(
                ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
                ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

            // Setup graphics pipeline state
            // 그래픽스 파이프라인 상태 설정
            FGraphicsPipelineStateInitializer GraphicsPSOInit;
            RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

            // Blend state: Alpha blending for soft edges
            // 블렌드 상태: 부드러운 가장자리를 위한 알파 블렌딩
            GraphicsPSOInit.BlendState = TStaticBlendState<
                CW_RGBA,
                BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
                BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();

            // Rasterizer state: No culling (billboard quads)
            // 래스터라이저 상태: 컬링 없음 (빌보드 쿼드)
            GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

            // Depth state: Write enabled, test with GreaterEqual (reversed-Z)
            // 뎁스 상태: 쓰기 활성화, 포인트끼리 깊이 테스트 (카메라에 가까운 것이 앞)
            GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI();

            // Primitive type: Triangle strip (4 vertices per quad)
            // 프리미티브 타입: 트라이앵글 스트립 (쿼드당 4개 버텍스)
            GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

            // Bind shaders
            // 셰이더 바인딩
            GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
            GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
            GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

            // Set pipeline state
            // 파이프라인 상태 설정
            SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

            // VS 파라미터 설정 (람다 내부에서 RHI SRV 획득)
            // SHADER_PARAMETER_SRV는 FRHIShaderResourceView*를 기대하므로
            // RDG SRV에서 GetRHI()로 변환해야 함
            FFleshRingDebugPointVS::FParameters VSParams;
            VSParams.DebugPoints = DebugPointsSRV ? DebugPointsSRV->GetRHI() : nullptr;
            VSParams.ViewProjectionMatrix = ViewProjectionMatrix;
            VSParams.InvViewportSize = InvViewportSize;
            VSParams.PointSizeBase = LocalPointSizeBase;
            VSParams.PointSizeInfluence = LocalPointSizeInfluence;

            SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParams);

            // Draw instanced quads (4 vertices per quad, N instances)
            // 인스턴스드 쿼드 그리기 (쿼드당 4개 버텍스, N개 인스턴스)
            RHICmdList.DrawPrimitive(0, 2, LocalPointCount);  // 2 triangles per quad
        }
    );
}

bool FFleshRingDebugViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
    if (!bEnabled)
    {
        return false;
    }

    // World 필터링: BoundWorld와 일치하는 뷰포트에서만 활성화
    if (BoundWorld.IsValid() && Context.GetWorld() != BoundWorld.Get())
    {
        return false;
    }

    return true;
}
