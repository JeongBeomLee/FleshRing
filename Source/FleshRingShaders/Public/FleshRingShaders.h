// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FFleshRingShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
