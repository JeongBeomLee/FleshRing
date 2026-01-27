// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class IAssetTypeActions;
class FSlateStyleSet;

class FFleshRingEditorModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** FleshRing style set name */
	static FName GetStyleSetName() { return TEXT("FleshRingStyle"); }

private:
	/** Custom style set */
	TSharedPtr<FSlateStyleSet> StyleSet;

	TSharedPtr<IAssetTypeActions> FleshRingAssetTypeActions;
};
