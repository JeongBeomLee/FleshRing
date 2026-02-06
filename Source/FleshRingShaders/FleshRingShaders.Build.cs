// Copyright 2026 LgThx. All Rights Reserved.

using UnrealBuildTool;

public class FleshRingShaders : ModuleRules
{
    public FleshRingShaders(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "RenderCore",
            "RHI",
            "Projects"
        });
    }
}
