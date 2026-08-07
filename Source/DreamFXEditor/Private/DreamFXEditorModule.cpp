#include "DreamFXEditorModule.h"

#include "UI/DreamFXGeneratedAssetGuard.h"

#define LOCTEXT_NAMESPACE "FDreamFXEditorModule"

void FDreamFXEditorModule::StartupModule()
{
	// Commandlets have no Slate to notify through, and the guard is a purely interactive affordance.
	if (!IsRunningCommandlet())
	{
		UE::DreamFX::Editor::FGeneratedAssetGuard::Register();
	}
}

void FDreamFXEditorModule::ShutdownModule()
{
	if (!IsRunningCommandlet())
	{
		UE::DreamFX::Editor::FGeneratedAssetGuard::Unregister();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDreamFXEditorModule, DreamFXEditor)
