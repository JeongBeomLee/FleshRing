// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingWaveShader.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"

IMPLEMENT_GLOBAL_SHADER(FFleshRingWaveCS, "/Plugin/FleshRingPlugin/FleshRingWave.usf", "MainCS", SF_Compute);
