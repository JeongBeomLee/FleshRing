// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingDebugPointSceneProxy.h"
#include "FleshRingDebugPointComponent.h"
#include "FleshRingDebugPointShader.h"

#include "SceneManagement.h"
#include "SceneView.h"
#include "RHIStaticStates.h"
#include "PipelineStateCache.h"
#include "CommonRenderResources.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "Modules/ModuleManager.h"

FFleshRingDebugPointSceneProxy::FFleshRingDebugPointSceneProxy(const UFleshRingDebugPointComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
	// Default settings
	bAlwaysHasVelocity = false;
	bCastDynamicShadow = false;
}

FFleshRingDebugPointSceneProxy::~FFleshRingDebugPointSceneProxy()
{
}

SIZE_T FFleshRingDebugPointSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FFleshRingDebugPointSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);

	// Register Post-Opaque render delegate
	// Called after Opaque rendering, before Translucency
	if (IsInRenderingThread())
	{
		IRendererModule& RendererModule = FModuleManager::GetModuleChecked<IRendererModule>("Renderer");
		PostOpaqueRenderDelegateHandle = RendererModule.RegisterPostOpaqueRenderDelegate(
			FPostOpaqueRenderDelegate::CreateRaw(this, &FFleshRingDebugPointSceneProxy::RenderPostOpaque_RenderThread)
		);
	}
}

void FFleshRingDebugPointSceneProxy::DestroyRenderThreadResources()
{
	// Unregister delegate
	if (PostOpaqueRenderDelegateHandle.IsValid())
	{
		IRendererModule& RendererModule = FModuleManager::GetModuleChecked<IRendererModule>("Renderer");
		RendererModule.RemovePostOpaqueRenderDelegate(PostOpaqueRenderDelegateHandle);
		PostOpaqueRenderDelegateHandle.Reset();
	}

	ClearBuffer_RenderThread();

	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FFleshRingDebugPointSceneProxy::UpdateTightnessBuffer_RenderThread(
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
	const TArray<uint32>& InVisibilityMaskArray)
{
	check(IsInRenderingThread());
	FScopeLock Lock(&BufferLock);
	TightnessBufferShared = InBuffer;
	VisibilityMaskArray = InVisibilityMaskArray;
}

void FFleshRingDebugPointSceneProxy::UpdateBulgeBuffer_RenderThread(
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
	const TArray<uint32>& InVisibilityMaskArray)
{
	check(IsInRenderingThread());
	FScopeLock Lock(&BufferLock);
	BulgeBufferShared = InBuffer;
	VisibilityMaskArray = InVisibilityMaskArray;
}

void FFleshRingDebugPointSceneProxy::ClearTightnessBuffer_RenderThread()
{
	FScopeLock Lock(&BufferLock);
	TightnessBufferShared = nullptr;
}

void FFleshRingDebugPointSceneProxy::ClearBulgeBuffer_RenderThread()
{
	FScopeLock Lock(&BufferLock);
	BulgeBufferShared = nullptr;
}

void FFleshRingDebugPointSceneProxy::ClearBuffer_RenderThread()
{
	FScopeLock Lock(&BufferLock);
	TightnessBufferShared = nullptr;
	BulgeBufferShared = nullptr;
}

FPrimitiveViewRelevance FFleshRingDebugPointSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Relevance;
	Relevance.bDrawRelevance = true;
	Relevance.bDynamicRelevance = true;
	Relevance.bRenderInMainPass = true;
	Relevance.bRenderInDepthPass = false;
	Relevance.bShadowRelevance = false;
	Relevance.bEditorPrimitiveRelevance = true;
	return Relevance;
}

void FFleshRingDebugPointSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	// Actual rendering is performed in PostOpaqueRenderDelegate
	// GetDynamicMeshElements is left empty
}

void FFleshRingDebugPointSceneProxy::RenderPostOpaque_RenderThread(FPostOpaqueRenderParameters& Parameters)
{
	// Buffer validity check and local copy
	TRefCountPtr<FRDGPooledBuffer> LocalTightnessBuffer;
	TRefCountPtr<FRDGPooledBuffer> LocalBulgeBuffer;
	uint32 LocalTightnessPointCount = 0;
	uint32 LocalBulgePointCount = 0;
	TArray<uint32> LocalVisibilityMaskArray;
	bool bRenderTightness = false;
	bool bRenderBulge = false;

	{
		FScopeLock Lock(&BufferLock);

		// Tightness buffer validation
		if (TightnessBufferShared.IsValid() && TightnessBufferShared->IsValid())
		{
			LocalTightnessBuffer = *TightnessBufferShared;
			if (LocalTightnessBuffer.IsValid() &&
				LocalTightnessBuffer->GetRHI() != nullptr &&
				LocalTightnessBuffer->Desc.NumElements > 0)
			{
				LocalTightnessPointCount = LocalTightnessBuffer->Desc.NumElements;
				bRenderTightness = true;
			}
		}

		// Bulge buffer validation
		if (BulgeBufferShared.IsValid() && BulgeBufferShared->IsValid())
		{
			LocalBulgeBuffer = *BulgeBufferShared;
			if (LocalBulgeBuffer.IsValid() &&
				LocalBulgeBuffer->GetRHI() != nullptr &&
				LocalBulgeBuffer->Desc.NumElements > 0)
			{
				LocalBulgePointCount = LocalBulgeBuffer->Desc.NumElements;
				bRenderBulge = true;
			}
		}

		LocalVisibilityMaskArray = VisibilityMaskArray;
	}

	// Return if there's nothing to render
	if (!bRenderTightness && !bRenderBulge)
	{
		return;
	}

	// View validity check
	if (!Parameters.View)
	{
		return;
	}

	// FViewInfo inherits from FSceneView, can use FSceneView interface
	const FSceneView* View = reinterpret_cast<const FSceneView*>(Parameters.View);

	// Scene filtering: Only render in viewports matching the Scene this proxy is registered to
	// PostOpaqueRenderDelegate is a global delegate, so it's called for all viewports
	if (View->Family && View->Family->Scene != &GetScene())
	{
		return;
	}

	// Get shaders
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View->GetFeatureLevel());
	TShaderMapRef<FFleshRingDebugPointVS> VertexShader(ShaderMap);
	TShaderMapRef<FFleshRingDebugPointPS> PixelShader(ShaderMap);

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	// Use RDG graph builder
	FRDGBuilder& GraphBuilder = *Parameters.GraphBuilder;

	// Get render target
	FRDGTextureRef ColorTarget = Parameters.ColorTexture;
	if (!ColorTarget)
	{
		return;
	}

	// Calculate view parameters
	FMatrix44f ViewProjectionMatrix = FMatrix44f(View->ViewMatrices.GetViewMatrix() * View->ViewMatrices.GetProjectionNoAAMatrix());
	FIntRect ViewRect = Parameters.ViewportRect;
	FVector2f InvViewportSize(1.0f / FMath::Max(1, ViewRect.Width()), 1.0f / FMath::Max(1, ViewRect.Height()));

	// If visibility mask array is empty, set all rings visible (default)
	if (LocalVisibilityMaskArray.Num() == 0)
	{
		LocalVisibilityMaskArray.Add(0xFFFFFFFFu);
	}

	// Create StructuredBuffer for visibility mask (supports unlimited rings)
	const uint32 NumMaskElements = static_cast<uint32>(LocalVisibilityMaskArray.Num());
	FRDGBufferRef VisibilityMaskBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumMaskElements),
		TEXT("FleshRingVisibilityMask"));

	// Upload mask data
	GraphBuilder.QueueBufferUpload(
		VisibilityMaskBuffer,
		LocalVisibilityMaskArray.GetData(),
		LocalVisibilityMaskArray.Num() * sizeof(uint32));

	FRDGBufferSRVRef VisibilityMaskSRV = GraphBuilder.CreateSRV(VisibilityMaskBuffer);

	// Create shared depth buffer (used by both Tightness and Bulge)
	FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
		ColorTarget->Desc.Extent,
		PF_DepthStencil,
		FClearValueBinding::DepthFar,
		TexCreate_DepthStencilTargetable,
		1,
		ColorTarget->Desc.NumSamples);
	FRDGTextureRef DebugDepthBuffer = GraphBuilder.CreateTexture(DepthDesc, TEXT("FleshRingDebugDepth"));

	// Capture rendering parameters
	float LocalPointSizeBase = PointSizeBase;
	float LocalPointSizeInfluence = PointSizeInfluence;

	// ========================================
	// Pass 1: Tightness debug points (ColorMode = 0)
	// ========================================
	if (bRenderTightness)
	{
		FRDGBufferRef TightnessPointsRDG = GraphBuilder.RegisterExternalBuffer(LocalTightnessBuffer, TEXT("FleshRingDebugPoints_Tightness"));
		FRDGBufferSRVRef TightnessSRV = GraphBuilder.CreateSRV(TightnessPointsRDG);

		FFleshRingDebugPointPS::FParameters* PSParams =
			GraphBuilder.AllocParameters<FFleshRingDebugPointPS::FParameters>();
		PSParams->DebugPointsRDG = TightnessSRV;
		PSParams->RingVisibilityMask = VisibilityMaskSRV;
		PSParams->NumVisibilityMaskElements = NumMaskElements;
		PSParams->RenderTargets[0] = FRenderTargetBinding(ColorTarget, ERenderTargetLoadAction::ELoad);
		// First pass: Clear depth buffer
		PSParams->RenderTargets.DepthStencil = FDepthStencilBinding(
			DebugDepthBuffer,
			ERenderTargetLoadAction::EClear,
			FExclusiveDepthStencil::DepthWrite_StencilNop);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("FleshRingDebugPoints_Tightness"),
			PSParams,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, LocalTightnessPointCount, ViewRect, TightnessSRV,
			 ViewProjectionMatrix, InvViewportSize, LocalPointSizeBase, LocalPointSizeInfluence,
			 PSParams](FRHICommandList& RHICmdList)
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
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PSParams);

				RHICmdList.DrawPrimitive(0, 2, LocalTightnessPointCount);
			}
		);
	}

	// ========================================
	// Pass 2: Bulge debug points (ColorMode = 1)
	// ========================================
	if (bRenderBulge)
	{
		FRDGBufferRef BulgePointsRDG = GraphBuilder.RegisterExternalBuffer(LocalBulgeBuffer, TEXT("FleshRingDebugPoints_Bulge"));
		FRDGBufferSRVRef BulgeSRV = GraphBuilder.CreateSRV(BulgePointsRDG);

		FFleshRingDebugPointPS::FParameters* PSParams =
			GraphBuilder.AllocParameters<FFleshRingDebugPointPS::FParameters>();
		PSParams->DebugPointsRDG = BulgeSRV;
		PSParams->RingVisibilityMask = VisibilityMaskSRV;
		PSParams->NumVisibilityMaskElements = NumMaskElements;
		PSParams->RenderTargets[0] = FRenderTargetBinding(ColorTarget, ERenderTargetLoadAction::ELoad);
		// Second pass: Load if Tightness was rendered, otherwise Clear
		PSParams->RenderTargets.DepthStencil = FDepthStencilBinding(
			DebugDepthBuffer,
			bRenderTightness ? ERenderTargetLoadAction::ELoad : ERenderTargetLoadAction::EClear,
			FExclusiveDepthStencil::DepthWrite_StencilNop);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("FleshRingDebugPoints_Bulge"),
			PSParams,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, LocalBulgePointCount, ViewRect, BulgeSRV,
			 ViewProjectionMatrix, InvViewportSize, LocalPointSizeBase, LocalPointSizeInfluence,
			 PSParams](FRHICommandList& RHICmdList)
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
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PSParams);

				RHICmdList.DrawPrimitive(0, 2, LocalBulgePointCount);
			}
		);
	}
}
