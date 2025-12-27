// Copyright Epic Games, Inc. All Rights Reserved.

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

    // FleshRingComputeSystem 등록
    // 렌더러가 EndOfFrameUpdate에서 FleshRing 작업을 올바른 타이밍에 실행하도록 함
    FFleshRingComputeSystem::Register();
}

void FFleshRingRuntimeModule::ShutdownModule()
{
    // FleshRingComputeSystem 해제
    FFleshRingComputeSystem::Unregister();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingRuntimeModule, FleshRingRuntime)
