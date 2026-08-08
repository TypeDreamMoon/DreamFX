#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FDreamFXEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/**
	 * Whether menus, the watcher and the guard were created (plan-v3 E5).
	 *
	 * Remembered rather than re-evaluated at shutdown: unregistering something that was never
	 * registered is the kind of asymmetry that only shows up as a crash on exit.
	 */
	bool bInteractiveSurfaceEnabled = false;
};
