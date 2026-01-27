// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingRuntime.h"
#include "FleshRingComputeWorker.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FFleshRingRuntimeModule"

void FFleshRingRuntimeModule::StartupModule()
{
    // Register shader directory mapping
    FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("FleshRingPlugin"))->GetBaseDir(), TEXT("Shaders"));
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/FleshRingPlugin"), PluginShaderDir);

    // Register FleshRingComputeSystem
    // Ensures the renderer executes FleshRing tasks at the correct timing in EndOfFrameUpdate
    FFleshRingComputeSystem::Register();
}

void FFleshRingRuntimeModule::ShutdownModule()
{
    // Unregister FleshRingComputeSystem
    FFleshRingComputeSystem::Unregister();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingRuntimeModule, FleshRingRuntime)
