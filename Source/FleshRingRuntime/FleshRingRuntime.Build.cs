// Copyright 2026 LgThx. All Rights Reserved.

using UnrealBuildTool;

public class FleshRingRuntime : ModuleRules
{
	public FleshRingRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);


		PrivateIncludePaths.AddRange(
			new string[] {
				// Engine Internal path for SkeletalMeshUpdater.h access
				System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Engine/Internal"),
			}
			);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
                "RenderCore",
                "Renderer",
                "RHI",
                "Projects",
                "ProceduralMeshComponent",
                "MeshDescription",
                "StaticMeshDescription",
                "SkeletalMeshDescription",
                "AnimationCore"
				// ... add private dependencies that you statically link with here ...
			}
            );

		// Add UnrealEd dependency only in editor builds (for GEditor usage)
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
