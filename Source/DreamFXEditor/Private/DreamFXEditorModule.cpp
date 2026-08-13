#include "DreamFXEditorModule.h"

#include "Bridge/DreamFXBridgeService.h"
#include "DreamFXModule.h"
#include "UI/DreamFXGeneratedAssetGuard.h"
#include "UI/DreamFXMenus.h"
#include "Workspace/DreamFXSourceWatcher.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#define LOCTEXT_NAMESPACE "FDreamFXEditorModule"

namespace
{
	/**
	 * plan-v3 E5, mirroring DreamShader's `-NoDreamShaderEditorBridge`.
	 *
	 * A commandlet already reaches this state by another route; this switch is for the case a
	 * commandlet cannot cover -- an editor session where DreamFX itself is the suspect, and the
	 * question is whether the watcher, the guard or a menu is causing what you are seeing.
	 */
	bool IsInteractiveSurfaceEnabled()
	{
		if (IsRunningCommandlet())
		{
			return false;
		}

		if (FParse::Param(FCommandLine::Get(), TEXT("NoDreamFXEditor")))
		{
			UE_LOG(LogDreamFX, Display,
				TEXT("-NoDreamFXEditor: menus, toolbars, the asset context menu, the source watcher and the generated-asset guard are all off."));
			return false;
		}

		return true;
	}
}

void FDreamFXEditorModule::StartupModule()
{
	// Every one of these is interactive: the guard notifies through Slate, the watcher exists to
	// shorten an edit-save-look loop that a commandlet does not have, and the menus are Slate by
	// definition. None of them is on the path a commandlet takes.
	bInteractiveSurfaceEnabled = IsInteractiveSurfaceEnabled();
	if (bInteractiveSurfaceEnabled)
	{
		UE::DreamFX::Editor::FGeneratedAssetGuard::Register();
		UE::DreamFX::Editor::FSourceWatcher::Register();
		UE::DreamFX::Editor::FDreamFXMenus::Register();
		UE::DreamFX::Editor::FBridgeService::Register();
	}
}

void FDreamFXEditorModule::ShutdownModule()
{
	if (bInteractiveSurfaceEnabled)
	{
		UE::DreamFX::Editor::FBridgeService::Unregister();
		UE::DreamFX::Editor::FDreamFXMenus::Unregister();
		UE::DreamFX::Editor::FSourceWatcher::Unregister();
		UE::DreamFX::Editor::FGeneratedAssetGuard::Unregister();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDreamFXEditorModule, DreamFXEditor)
