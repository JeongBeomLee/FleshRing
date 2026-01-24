// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingWaveShader.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"

IMPLEMENT_GLOBAL_SHADER(FFleshRingWaveCS, "/Plugin/FleshRingPlugin/FleshRingWave.usf", "MainCS", SF_Compute);
