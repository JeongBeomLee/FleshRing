// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingDebugPointShader.h"

// Register shaders with Unreal Engine shader system
// 언리얼 엔진 셰이더 시스템에 셰이더 등록

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingDebugPointVS,
    "/Plugin/FleshRingPlugin/FleshRingDebugPointVS.usf",
    "MainVS",
    SF_Vertex
);

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingDebugPointPS,
    "/Plugin/FleshRingPlugin/FleshRingDebugPointPS.usf",
    "MainPS",
    SF_Pixel
);
