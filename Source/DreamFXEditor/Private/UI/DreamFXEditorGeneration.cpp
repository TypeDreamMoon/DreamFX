#include "UI/DreamFXEditorGeneration.h"

#include "DreamFXParser.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Editor.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"

namespace UE::DreamFX::Editor
{
	FGenerateResult FEditorGeneration::GenerateFromFile(const FString& FilePath, const FGenerateOptions& Options,
		FDiagnosticSink& Diagnostics)
	{
		UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		TWeakObjectPtr<UObject> ReopenAsset;

		// Verify never changes the script, so it must not close editors or prompt about their edits.
		if (!Options.bVerifyOnly && Editors != nullptr
			&& FPaths::GetExtension(FilePath).Equals(TEXT("dfm"), ESearchCase::IgnoreCase))
		{
			FDocument Document;
			FDiagnosticSink LookupDiagnostics;
			FString MountPoint, Error;
			// A failed lookup is left to the generator, which owns the actual validation diagnostics.
			if (FParser::ParseFile(FilePath, Document, LookupDiagnostics)
				&& FDreamFXPaths::ResolveRootMountPoint(Document.Root, MountPoint, Error))
			{
				const FString PackagePath = MountPoint / Document.Name;
				for (UObject* Asset : Editors->GetAllEditedAssets())
				{
					if (Asset == nullptr || !Asset->IsA<UNiagaraScript>()
						|| !Asset->GetOutermost()->GetName().Equals(PackagePath, ESearchCase::IgnoreCase))
					{
						continue;
					}

					// The toolkit owns the save/discard/cancel prompt. A cancellation must leave both
					// its editable copy and the generated asset untouched.
					Editors->CloseAllEditorsForAsset(Asset);
					if (Editors->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) != nullptr)
					{
						Diagnostics.SetFile(FilePath);
						Diagnostics.Error(TEXT("DFX5035"), Document.HeaderLocation, FString::Printf(
							TEXT("'%s' is still open in the Niagara script editor. The rebuild was cancelled to protect its editable copy. Close the editor, then rebuild again."),
							*Asset->GetName()));
						return FGenerateResult();
					}
					ReopenAsset = Asset;
					break;
				}
			}
		}

		const FGenerateResult Result = FGenerator::GenerateFromFile(FilePath, Options, Diagnostics);
		// Even a failed build should return the author to the editor they had open.
		if (ReopenAsset.IsValid())
		{
			Editors->OpenEditorForAsset(ReopenAsset.Get());
		}
		return Result;
	}
}
