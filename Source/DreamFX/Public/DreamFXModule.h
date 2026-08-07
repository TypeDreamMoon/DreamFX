#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DREAMFX_API DECLARE_LOG_CATEGORY_EXTERN(LogDreamFX, Log, All);

class DREAMFX_API FDreamFXModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
