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
    ClearDebugBulgePointBuffer();
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
    // TSharedPtr 자체와 내부 TRefCountPtr<FRDGPooledBuffer> 모두 유효해야 함
    bEnabled = (InBufferPtr.IsValid() && InBufferPtr->IsValid() && InPointCount > 0);
}

void FFleshRingDebugViewExtension::SetDebugBulgePointBufferShared(TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBufferPtr, uint32 InPointCount)
{
    FScopeLock Lock(&BufferLock);
    DebugBulgePointBufferSharedPtr = InBufferPtr;
    BulgePointCount = InPointCount;
    // TSharedPtr 자체와 내부 TRefCountPtr<FRDGPooledBuffer> 모두 유효해야 함
    bBulgeEnabled = (InBufferPtr.IsValid() && InBufferPtr->IsValid() && InPointCount > 0);
}

void FFleshRingDebugViewExtension::ClearDebugBulgePointBuffer()
{
    FScopeLock Lock(&BufferLock);
    DebugBulgePointBufferSharedPtr = nullptr;
    BulgePointCount = 0;
    bBulgeEnabled = false;
}

void FFleshRingDebugViewExtension::PostRenderViewFamily_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneViewFamily& InViewFamily)
{
    // ========================================
    // Local copies of buffer data (thread-safe)
    // 버퍼 데이터의 로컬 복사본 (스레드 안전)
    // ========================================
    TRefCountPtr<FRDGPooledBuffer> LocalTightnessBuffer;
    TRefCountPtr<FRDGPooledBuffer> LocalBulgeBuffer;
    uint32 LocalTightnessPointCount = 0;
    uint32 LocalBulgePointCount = 0;
    float LocalPointSizeBase = PointSizeBase;
    float LocalPointSizeInfluence = PointSizeInfluence;
    bool bRenderTightness = false;
    bool bRenderBulge = false;

    {
        FScopeLock Lock(&BufferLock);

        // Tightness buffer
        if (bEnabled && PointCount > 0)
        {
            if (DebugPointBufferSharedPtr.IsValid() && DebugPointBufferSharedPtr->IsValid())
            {
                LocalTightnessBuffer = *DebugPointBufferSharedPtr;
            }
            else if (DebugPointBuffer.IsValid())
            {
                LocalTightnessBuffer = DebugPointBuffer;
            }

            if (LocalTightnessBuffer.IsValid())
            {
                LocalTightnessPointCount = PointCount;
                bRenderTightness = true;
            }
        }

        // Bulge buffer
        if (bBulgeEnabled && BulgePointCount > 0)
        {
            if (DebugBulgePointBufferSharedPtr.IsValid() && DebugBulgePointBufferSharedPtr->IsValid())
            {
                LocalBulgeBuffer = *DebugBulgePointBufferSharedPtr;
                LocalBulgePointCount = BulgePointCount;
                bRenderBulge = true;
            }
        }
    }

    // Nothing to render
    if (!bRenderTightness && !bRenderBulge)
    {
        return;
    }

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
    FMatrix44f ViewProjectionMatrix = FMatrix44f(View->ViewMatrices.GetViewMatrix() * View->ViewMatrices.GetProjectionNoAAMatrix());
    FIntRect ViewRect = View->UnscaledViewRect;
    FVector2f InvViewportSize(1.0f / FMath::Max(1, ViewRect.Width()), 1.0f / FMath::Max(1, ViewRect.Height()));

    // Get render target from view family
    FRDGTextureRef RenderTarget = nullptr;
    if (InViewFamily.RenderTarget)
    {
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

    // 렌더 타겟의 MSAA 샘플 수 가져오기
    const uint32 NumSamples = RenderTarget->Desc.NumSamples;

    // 공유 뎁스 버퍼 생성 (Tightness + Bulge 모두 사용)
    FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
        FIntPoint(ViewRect.Width(), ViewRect.Height()),
        PF_DepthStencil,
        FClearValueBinding::DepthFar,
        TexCreate_DepthStencilTargetable,
        1,
        NumSamples);
    FRDGTextureRef DebugDepthBuffer = GraphBuilder.CreateTexture(DepthDesc, TEXT("FleshRingDebugDepth"));

    // ========================================
    // Pass 1: Tightness debug points (ColorMode = 0)
    // 패스 1: Tightness 디버그 포인트 (파랑→초록→빨강)
    // ========================================
    if (bRenderTightness)
    {
        FRDGBufferRef TightnessPointsRDG = GraphBuilder.RegisterExternalBuffer(LocalTightnessBuffer, TEXT("FleshRingDebugPoints_Tightness"));
        FRDGBufferSRVRef TightnessSRV = GraphBuilder.CreateSRV(TightnessPointsRDG);

        FFleshRingDebugPointPS::FParameters* PSParams =
            GraphBuilder.AllocParameters<FFleshRingDebugPointPS::FParameters>();
        PSParams->DebugPointsRDG = TightnessSRV;
        PSParams->RenderTargets[0] = FRenderTargetBinding(RenderTarget, ERenderTargetLoadAction::ELoad);
        PSParams->RenderTargets.DepthStencil = FDepthStencilBinding(DebugDepthBuffer, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilNop);

        GraphBuilder.AddPass(
            RDG_EVENT_NAME("FleshRingDebugPoints_Tightness"),
            PSParams,
            ERDGPassFlags::Raster,
            [VertexShader, PixelShader, LocalTightnessPointCount, ViewRect, TightnessSRV,
             ViewProjectionMatrix, InvViewportSize, LocalPointSizeBase, LocalPointSizeInfluence](FRHICommandList& RHICmdList)
            {
                RHICmdList.SetViewport(
                    ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
                    ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

                FGraphicsPipelineStateInitializer GraphicsPSOInit;
                RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

                GraphicsPSOInit.BlendState = TStaticBlendState<
                    CW_RGBA,
                    BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
                    BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
                GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI();
                GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
                GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
                GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

                SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

                FFleshRingDebugPointVS::FParameters VSParams;
                VSParams.DebugPoints = TightnessSRV ? TightnessSRV->GetRHI() : nullptr;
                VSParams.ViewProjectionMatrix = ViewProjectionMatrix;
                VSParams.InvViewportSize = InvViewportSize;
                VSParams.PointSizeBase = LocalPointSizeBase;
                VSParams.PointSizeInfluence = LocalPointSizeInfluence;
                VSParams.ColorMode = 0;  // Tightness: Blue → Green → Red

                SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParams);
                RHICmdList.DrawPrimitive(0, 2, LocalTightnessPointCount);
            }
        );
    }

    // ========================================
    // Pass 2: Bulge debug points (ColorMode = 1)
    // 패스 2: Bulge 디버그 포인트 (청록→마젠타)
    // ========================================
    if (bRenderBulge)
    {
        FRDGBufferRef BulgePointsRDG = GraphBuilder.RegisterExternalBuffer(LocalBulgeBuffer, TEXT("FleshRingDebugPoints_Bulge"));
        FRDGBufferSRVRef BulgeSRV = GraphBuilder.CreateSRV(BulgePointsRDG);

        FFleshRingDebugPointPS::FParameters* PSParams =
            GraphBuilder.AllocParameters<FFleshRingDebugPointPS::FParameters>();
        PSParams->DebugPointsRDG = BulgeSRV;
        PSParams->RenderTargets[0] = FRenderTargetBinding(RenderTarget, ERenderTargetLoadAction::ELoad);
        // 두 번째 패스: 뎁스 버퍼 로드 (기존 Tightness 포인트와 깊이 테스트)
        PSParams->RenderTargets.DepthStencil = FDepthStencilBinding(DebugDepthBuffer,
            bRenderTightness ? ERenderTargetLoadAction::ELoad : ERenderTargetLoadAction::EClear,
            FExclusiveDepthStencil::DepthWrite_StencilNop);

        GraphBuilder.AddPass(
            RDG_EVENT_NAME("FleshRingDebugPoints_Bulge"),
            PSParams,
            ERDGPassFlags::Raster,
            [VertexShader, PixelShader, LocalBulgePointCount, ViewRect, BulgeSRV,
             ViewProjectionMatrix, InvViewportSize, LocalPointSizeBase, LocalPointSizeInfluence](FRHICommandList& RHICmdList)
            {
                RHICmdList.SetViewport(
                    ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
                    ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

                FGraphicsPipelineStateInitializer GraphicsPSOInit;
                RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

                GraphicsPSOInit.BlendState = TStaticBlendState<
                    CW_RGBA,
                    BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
                    BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
                GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI();
                GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
                GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
                GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

                SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

                FFleshRingDebugPointVS::FParameters VSParams;
                VSParams.DebugPoints = BulgeSRV ? BulgeSRV->GetRHI() : nullptr;
                VSParams.ViewProjectionMatrix = ViewProjectionMatrix;
                VSParams.InvViewportSize = InvViewportSize;
                VSParams.PointSizeBase = LocalPointSizeBase;
                VSParams.PointSizeInfluence = LocalPointSizeInfluence;
                VSParams.ColorMode = 1;  // Bulge: Cyan → Magenta

                SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParams);
                RHICmdList.DrawPrimitive(0, 2, LocalBulgePointCount);
            }
        );
    }
}

bool FFleshRingDebugViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
    // Tightness 또는 Bulge 중 하나라도 활성화되어 있으면 활성화
    if (!bEnabled && !bBulgeEnabled)
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
