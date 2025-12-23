// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FFleshRingRuntimeModule"

void FFleshRingRuntimeModule::StartupModule()
{
    // Register shader directory mapping
    FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("FleshRingPlugin"))->GetBaseDir(), TEXT("Shaders"));
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/FleshRingPlugin"), PluginShaderDir);
}

void FFleshRingRuntimeModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingRuntimeModule, FleshRingRuntime)
