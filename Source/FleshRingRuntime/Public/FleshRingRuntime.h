// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FFleshRingRuntimeModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
