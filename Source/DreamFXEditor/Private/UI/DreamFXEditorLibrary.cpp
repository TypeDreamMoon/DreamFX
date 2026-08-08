#include "DreamFXEditorLibrary.h"

#include "DreamFXModule.h"
#include "UI/DreamFXAssetCommands.h"
#include "Workspace/DreamFXWorkspaceService.h"

void UDreamFXEditorLibrary::RebuildAllSources()
{
	UE::DreamFX::Editor::FDreamFXCommands::RebuildAll();
}

void UDreamFXEditorLibrary::VerifyAllSources()
{
	UE::DreamFX::Editor::FDreamFXCommands::VerifyAll();
}

void UDreamFXEditorLibrary::OpenWorkspace()
{
	UE::DreamFX::Editor::FDreamFXCommands::OpenWorkspace();
}

FString UDreamFXEditorLibrary::WriteWorkspaceFile()
{
	FString WorkspacePath;
	FString Error;
	if (!UE::DreamFX::Editor::FDreamFXWorkspaceService::WriteWorkspaceFile(WorkspacePath, Error))
	{
		UE_LOG(LogDreamFX, Error, TEXT("%s"), *Error);
		return FString();
	}
	return WorkspacePath;
}

void UDreamFXEditorLibrary::ExportSystem(UNiagaraSystem* System)
{
	UE::DreamFX::Editor::FDreamFXCommands::ExportSystem(System);
}

void UDreamFXEditorLibrary::ExportEmitter(UNiagaraEmitter* Emitter)
{
	UE::DreamFX::Editor::FDreamFXCommands::ExportEmitter(Emitter);
}

void UDreamFXEditorLibrary::AdoptSystem(UNiagaraSystem* System, const bool bSkipConfirmation)
{
	UE::DreamFX::Editor::FDreamFXCommands::AdoptSystem(System, bSkipConfirmation);
}

void UDreamFXEditorLibrary::OpenSource(UObject* Asset)
{
	UE::DreamFX::Editor::FDreamFXCommands::OpenSource(Asset);
}

void UDreamFXEditorLibrary::RebuildFromSource(UObject* Asset)
{
	UE::DreamFX::Editor::FDreamFXCommands::RebuildFromSource(Asset);
}

void UDreamFXEditorLibrary::VerifyAsset(UObject* Asset)
{
	UE::DreamFX::Editor::FDreamFXCommands::VerifyAsset(Asset);
}

bool UDreamFXEditorLibrary::HasProvenance(UObject* Asset)
{
	return UE::DreamFX::Editor::FDreamFXCommands::HasProvenance(Asset);
}
