// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingDebugPointShader.h"

// Register shaders with Unreal Engine shader system

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
