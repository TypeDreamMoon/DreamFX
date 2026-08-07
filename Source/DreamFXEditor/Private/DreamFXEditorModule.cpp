#include "DreamFXEditorModule.h"

#include "UI/DreamFXGeneratedAssetGuard.h"
#include "Workspace/DreamFXSourceWatcher.h"

#define LOCTEXT_NAMESPACE "FDreamFXEditorModule"

void FDreamFXEditorModule::StartupModule()
{
	// Both features are interactive: the guard notifies through Slate, and the watcher exists to
	// shorten an edit-save-look loop that a commandlet does not have.
	if (!IsRunningCommandlet())
	{
		UE::DreamFX::Editor::FGeneratedAssetGuard::Register();
		UE::DreamFX::Editor::FSourceWatcher::Register();
	}
}

void FDreamFXEditorModule::ShutdownModule()
{
	if (!IsRunningCommandlet())
	{
		UE::DreamFX::Editor::FSourceWatcher::Unregister();
		UE::DreamFX::Editor::FGeneratedAssetGuard::Unregister();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDreamFXEditorModule, DreamFXEditor)
