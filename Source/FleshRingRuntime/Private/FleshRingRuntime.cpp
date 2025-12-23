// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FFleshRingRuntimeModule"

void FFleshRingRuntimeModule::StartupModule()
{
	// Shader directory is automatically registered by UE
	// (Plugins/FleshRingPlugin/Shaders/ -> /Plugin/FleshRingPlugin/)
}

void FFleshRingRuntimeModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingRuntimeModule, FleshRingRuntime)
