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
	// 기본 설정
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

	// Post-Opaque 렌더 델리게이트 등록
	// Opaque 렌더링 이후, Translucency 이전에 호출됨
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
	// 델리게이트 등록 해제
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
	// 실제 렌더링은 PostOpaqueRenderDelegate에서 수행
	// GetDynamicMeshElements는 빈 구현
}

void FFleshRingDebugPointSceneProxy::RenderPostOpaque_RenderThread(FPostOpaqueRenderParameters& Parameters)
{
	// 버퍼 유효성 체크 및 로컬 복사
	TRefCountPtr<FRDGPooledBuffer> LocalTightnessBuffer;
	TRefCountPtr<FRDGPooledBuffer> LocalBulgeBuffer;
	uint32 LocalTightnessPointCount = 0;
	uint32 LocalBulgePointCount = 0;
	TArray<uint32> LocalVisibilityMaskArray;
	bool bRenderTightness = false;
	bool bRenderBulge = false;

	{
		FScopeLock Lock(&BufferLock);

		// Tightness buffer 검증
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

		// Bulge buffer 검증
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

	// 렌더링할 것이 없으면 리턴
	if (!bRenderTightness && !bRenderBulge)
	{
		return;
	}

	// View 유효성 체크
	if (!Parameters.View)
	{
		return;
	}

	// FViewInfo는 FSceneView를 상속, FSceneView 인터페이스 사용 가능
	const FSceneView* View = reinterpret_cast<const FSceneView*>(Parameters.View);

	// Scene 필터링: 이 프록시가 등록된 Scene과 동일한 뷰포트에서만 렌더링
	// PostOpaqueRenderDelegate는 전역 델리게이트이므로 모든 뷰포트에서 호출됨
	if (View->Family && View->Family->Scene != &GetScene())
	{
		return;
	}

	// 셰이더 가져오기
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View->GetFeatureLevel());
	TShaderMapRef<FFleshRingDebugPointVS> VertexShader(ShaderMap);
	TShaderMapRef<FFleshRingDebugPointPS> PixelShader(ShaderMap);

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	// RDG 그래프 빌더 사용
	FRDGBuilder& GraphBuilder = *Parameters.GraphBuilder;

	// 렌더 타겟 가져오기
	FRDGTextureRef ColorTarget = Parameters.ColorTexture;
	if (!ColorTarget)
	{
		return;
	}

	// 뷰 파라미터 계산
	FMatrix44f ViewProjectionMatrix = FMatrix44f(View->ViewMatrices.GetViewMatrix() * View->ViewMatrices.GetProjectionNoAAMatrix());
	FIntRect ViewRect = View->UnscaledViewRect;
	FVector2f InvViewportSize(1.0f / FMath::Max(1, ViewRect.Width()), 1.0f / FMath::Max(1, ViewRect.Height()));

	// 가시성 마스크 배열이 비어있으면 모든 링 가시로 설정 (기본값)
	if (LocalVisibilityMaskArray.Num() == 0)
	{
		LocalVisibilityMaskArray.Add(0xFFFFFFFFu);
	}

	// 가시성 마스크용 StructuredBuffer 생성 (무제한 링 지원)
	const uint32 NumMaskElements = static_cast<uint32>(LocalVisibilityMaskArray.Num());
	FRDGBufferRef VisibilityMaskBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumMaskElements),
		TEXT("FleshRingVisibilityMask"));

	// 마스크 데이터 업로드
	GraphBuilder.QueueBufferUpload(
		VisibilityMaskBuffer,
		LocalVisibilityMaskArray.GetData(),
		LocalVisibilityMaskArray.Num() * sizeof(uint32));

	FRDGBufferSRVRef VisibilityMaskSRV = GraphBuilder.CreateSRV(VisibilityMaskBuffer);

	// 공유 뎁스 버퍼 생성 (Tightness + Bulge 모두 사용)
	FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
		ColorTarget->Desc.Extent,
		PF_DepthStencil,
		FClearValueBinding::DepthFar,
		TexCreate_DepthStencilTargetable,
		1,
		ColorTarget->Desc.NumSamples);
	FRDGTextureRef DebugDepthBuffer = GraphBuilder.CreateTexture(DepthDesc, TEXT("FleshRingDebugDepth"));

	// 렌더링 파라미터 캡처
	float LocalPointSizeBase = PointSizeBase;
	float LocalPointSizeInfluence = PointSizeInfluence;

	// ========================================
	// Pass 1: Tightness 디버그 포인트 (ColorMode = 0)
	// ========================================
	if (bRenderTightness)
	{
		FRDGBufferRef TightnessPointsRDG = GraphBuilder.RegisterExternalBuffer(LocalTightnessBuffer, TEXT("FleshRingDebugPoints_Tightness"));
		FRDGBufferSRVRef TightnessSRV = GraphBuilder.CreateSRV(TightnessPointsRDG);

		FFleshRingDebugPointPS::FParameters* PSParams =
			GraphBuilder.AllocParameters<FFleshRingDebugPointPS::FParameters>();
		PSParams->DebugPointsRDG = TightnessSRV;
		// RingVisibilityMask는 RHI SRV로 lambda 내에서 바인딩
		PSParams->RenderTargets[0] = FRenderTargetBinding(ColorTarget, ERenderTargetLoadAction::ELoad);
		// 첫 번째 패스: depth buffer Clear
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
			 VisibilityMaskSRV, NumMaskElements](FRHICommandList& RHICmdList)
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

				FFleshRingDebugPointPS::FParameters PSParamsLocal;
				PSParamsLocal.RingVisibilityMask = VisibilityMaskSRV ? VisibilityMaskSRV->GetRHI() : nullptr;
				PSParamsLocal.NumVisibilityMaskElements = NumMaskElements;

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParams);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParamsLocal);

				RHICmdList.DrawPrimitive(0, 2, LocalTightnessPointCount);
			}
		);
	}

	// ========================================
	// Pass 2: Bulge 디버그 포인트 (ColorMode = 1)
	// ========================================
	if (bRenderBulge)
	{
		FRDGBufferRef BulgePointsRDG = GraphBuilder.RegisterExternalBuffer(LocalBulgeBuffer, TEXT("FleshRingDebugPoints_Bulge"));
		FRDGBufferSRVRef BulgeSRV = GraphBuilder.CreateSRV(BulgePointsRDG);

		FFleshRingDebugPointPS::FParameters* PSParams =
			GraphBuilder.AllocParameters<FFleshRingDebugPointPS::FParameters>();
		PSParams->DebugPointsRDG = BulgeSRV;
		// RingVisibilityMask는 RHI SRV로 lambda 내에서 바인딩
		PSParams->RenderTargets[0] = FRenderTargetBinding(ColorTarget, ERenderTargetLoadAction::ELoad);
		// 두 번째 패스: Tightness가 렌더링된 경우 Load, 아니면 Clear
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
			 VisibilityMaskSRV, NumMaskElements](FRHICommandList& RHICmdList)
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

				FFleshRingDebugPointPS::FParameters PSParamsLocal;
				PSParamsLocal.RingVisibilityMask = VisibilityMaskSRV ? VisibilityMaskSRV->GetRHI() : nullptr;
				PSParamsLocal.NumVisibilityMaskElements = NumMaskElements;

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParams);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParamsLocal);

				RHICmdList.DrawPrimitive(0, 2, LocalBulgePointCount);
			}
		);
	}
}
