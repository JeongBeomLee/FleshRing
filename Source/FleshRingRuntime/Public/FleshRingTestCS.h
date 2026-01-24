// Copyright 2026 LgThx. All Rights Reserved.

// FleshRing Test Compute Shader - 헤더 파일
// 목적: 외부에서 Dispatch 함수를 호출할 수 있도록 선언

#pragma once

#include "CoreMinimal.h"

// Forward Declaration (전방 선언)
// 실제 정의는 RenderGraphBuilder.h에 있지만, 헤더에서는 선언만 필요
class FRDGBuilder;

// ============================================================================
// Dispatch 함수 선언
// ============================================================================
// FLESHRINGRUNTIME_API: 이 함수를 모듈 외부에서 사용할 수 있게 export
//
// 파라미터:
// - GraphBuilder: RDG 빌더 (렌더링 스레드에서 전달받음)
// - Count: 처리할 데이터 개수
//
// 사용 예:
//   ENQUEUE_RENDER_COMMAND(FleshRingTest)(
//       [Count](FRHICommandListImmediate& RHICmdList)
//       {
//           FRDGBuilder GraphBuilder(RHICmdList);
//           DispatchFleshRingTestCS(GraphBuilder, Count);
//           GraphBuilder.Execute();
//       });
void DispatchFleshRingTestCS(FRDGBuilder& GraphBuilder, uint32 Count);
