// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingRuntime.h"
#include "FleshRingComputeWorker.h"

#define LOCTEXT_NAMESPACE "FFleshRingRuntimeModule"

void FFleshRingRuntimeModule::StartupModule()
{
    // Shader directory mapping is handled by FleshRingShaders module (PostConfigInit)

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
