// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingShaders.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ShaderCore.h"

void FFleshRingShadersModule::StartupModule()
{
	FString PluginShaderDir;
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FleshRingPlugin"));

	if (Plugin.IsValid())
	{
		FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		if (FPaths::DirectoryExists(ShaderDir))
		{
			PluginShaderDir = ShaderDir;
		}
	}

	// Fallback: search common plugin directories
	if (PluginShaderDir.IsEmpty())
	{
		FString RelativeShaderPath = TEXT("FleshRingPlugin/Shaders");

		TArray<FString> SearchPaths;
		SearchPaths.Add(FPaths::ProjectPluginsDir());
		SearchPaths.Add(FPaths::EnginePluginsDir());

		for (const FString& SearchPath : SearchPaths)
		{
			FString Path = SearchPath / RelativeShaderPath;
			if (FPaths::DirectoryExists(Path))
			{
				PluginShaderDir = Path;
				break;
			}

			// Also check Marketplace subdirectory
			Path = SearchPath / TEXT("Marketplace") / RelativeShaderPath;
			if (FPaths::DirectoryExists(Path))
			{
				PluginShaderDir = Path;
				break;
			}
		}
	}

	FPaths::CollapseRelativeDirectories(PluginShaderDir);

	if (!PluginShaderDir.IsEmpty())
	{
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/FleshRingPlugin"), PluginShaderDir);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("FleshRingPlugin: Shaders directory not found. Compute shaders will not be available."));
	}
}

void FFleshRingShadersModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FFleshRingShadersModule, FleshRingShaders)
