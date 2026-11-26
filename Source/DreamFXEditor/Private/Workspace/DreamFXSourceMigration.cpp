#include "Workspace/DreamFXSourceMigration.h"

#include "DreamFXModule.h"
#include "Schema/DreamFXIndexExport.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealEdMisc.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DreamFXSourceMigration"

namespace UE::DreamFX::Editor
{
	namespace SourceMigrationLocal
	{
		FDelegateHandle GStartupHandle;
		TWeakPtr<SNotificationItem> GNotification;

		const TCHAR* const ConfigSection = TEXT("DreamSource");
		const TCHAR* const HideKey = TEXT("bHideFXMigrationNotice");
		const TCHAR* const WorkspaceFileName = TEXT("DreamFX.code-workspace");

		FSimpleMulticastDelegate& PostEngineInitDelegate()
		{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			return FCoreDelegates::OnPostEngineInit;
#else
			return FCoreDelegates::GetOnPostEngineInit();
#endif
		}

		bool IsHidden()
		{
			bool bHidden = false;
			GConfig->GetBool(ConfigSection, HideKey, bHidden, GEditorPerProjectIni);
			return bHidden;
		}

		void Hide()
		{
			GConfig->SetBool(ConfigSection, HideKey, true, GEditorPerProjectIni);
			GConfig->Flush(/*bRemoveFromCache=*/false, GEditorPerProjectIni);
		}

		void Dismiss()
		{
			if (const TSharedPtr<SNotificationItem> Item = GNotification.Pin())
			{
				Item->SetExpireDuration(0.0f);
				Item->SetFadeOutDuration(0.5f);
				Item->ExpireAndFadeout();
			}
			GNotification.Reset();
		}

		bool CanNotify()
		{
			return FSlateApplication::IsInitialized()
				&& !IsRunningCommandlet()
				&& !GIsAutomationTesting
				&& !FApp::IsUnattended();
		}

		void ShowResult(const FText& InText, bool bSuccess)
		{
			FNotificationInfo Info(InText);
			Info.bUseLargeFont = false;
			if (bSuccess)
			{
				// Nothing about the move takes effect until the editor starts again: offer it right here.
				Info.bFireAndForget = false;
				Info.ExpireDuration = 0.0f;
				Info.ButtonDetails.Add(FNotificationButtonInfo(
					LOCTEXT("RestartNow", "Restart Now"),
					FText::GetEmpty(),
					FSimpleDelegate::CreateLambda([] { Dismiss(); FUnrealEdMisc::Get().RestartEditor(/*bWarn=*/true); }),
					SNotificationItem::CS_Success));
				Info.ButtonDetails.Add(FNotificationButtonInfo(
					LOCTEXT("RestartLater", "Later"),
					FText::GetEmpty(),
					FSimpleDelegate::CreateLambda([] { Dismiss(); }),
					SNotificationItem::CS_Success));
			}
			else
			{
				Info.ExpireDuration = 12.0f;
			}
			if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
			{
				Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
				if (bSuccess)
				{
					GNotification = Item;
				}
			}
		}

		void ShowNotice()
		{
			const FProjectSourceDirectory Project = FDreamFXPaths::ResolveProjectSourceDirectory();
			if (!Project.bUsingLegacy && !Project.bLegacyIgnored)
			{
				return;
			}

			FNotificationInfo Info(Project.bUsingLegacy
				? FText::Format(LOCTEXT("UsingLegacy",
					"DreamFX: '{0}' is deprecated.\nMove it to '{1}' -- Dream sources are managed together under DreamSource/.\nIt keeps working until then."),
					FText::FromString(Project.LegacyDirectory), FText::FromString(Project.ConfiguredDirectory))
				: FText::Format(LOCTEXT("LegacyIgnored",
					"DreamFX: '{0}' is ignored because '{1}' exists.\nMerge what it still holds into '{1}', then delete it."),
					FText::FromString(Project.LegacyDirectory), FText::FromString(Project.ConfiguredDirectory)));
			Info.bFireAndForget = false;
			Info.ExpireDuration = 0.0f;
			Info.bUseLargeFont = false;
			if (Project.bUsingLegacy)
			{
				Info.ButtonDetails.Add(FNotificationButtonInfo(
					LOCTEXT("Migrate", "Migrate"),
					LOCTEXT("MigrateTip", "Move the folder now, then restart the editor. Close VS Code windows that have files in it open first."),
					FSimpleDelegate::CreateLambda([]
					{
						Dismiss();
						FString Error;
						if (FDreamFXSourceMigration::Migrate(Error))
						{
							ShowResult(FText::Format(LOCTEXT("Migrated",
								"DreamFX: sources moved to '{0}'.\nRestart the editor to use the new place."),
								FText::FromString(FDreamFXPaths::ResolveProjectSourceDirectory().ConfiguredDirectory)), true);
						}
						else
						{
							ShowResult(FText::Format(LOCTEXT("MigrateFailed", "DreamFX: could not migrate: {0}"),
								FText::FromString(Error)), false);
						}
					}),
					SNotificationItem::CS_None));
			}
			Info.ButtonDetails.Add(FNotificationButtonInfo(
				LOCTEXT("DontShowAgain", "Don't show again"),
				LOCTEXT("DontShowAgainTip", "Stop showing this notice on this machine. The Output Log still says it."),
				FSimpleDelegate::CreateLambda([] { Hide(); Dismiss(); }),
				SNotificationItem::CS_None));
			Info.ButtonDetails.Add(FNotificationButtonInfo(
				LOCTEXT("Later", "Later"),
				FText::GetEmpty(),
				FSimpleDelegate::CreateLambda([] { Dismiss(); }),
				SNotificationItem::CS_None));

			GNotification = FSlateNotificationManager::Get().AddNotification(Info);
		}

		void DeleteTree(const FString& InDirectory)
		{
			if (IFileManager::Get().DirectoryExists(*InDirectory)
				&& !IFileManager::Get().DeleteDirectory(*InDirectory, /*RequireExists=*/false, /*Tree=*/true))
			{
				UE_LOG(LogDreamFX, Warning,
					TEXT("DreamFX: could not delete '%s', which the DreamSource layout replaced; delete it once nothing holds it open."),
					*InDirectory);
			}
		}
	}

	void FDreamFXSourceMigration::Register(bool bInShowNotice)
	{
		using namespace SourceMigrationLocal;
		// After engine init: the settings CDOs, the plugin list and Slate all have to be up.
		GStartupHandle = SourceMigrationLocal::PostEngineInitDelegate().AddLambda([bInShowNotice]
		{
			FDreamFXPaths::LogDeprecatedSourceDirectoriesOnce();
			if (bInShowNotice && CanNotify() && !IsHidden())
			{
				ShowNotice();
			}
		});
	}

	void FDreamFXSourceMigration::Unregister()
	{
		using namespace SourceMigrationLocal;
		if (GStartupHandle.IsValid())
		{
			SourceMigrationLocal::PostEngineInitDelegate().Remove(GStartupHandle);
			GStartupHandle.Reset();
		}
		GNotification.Reset();
	}

	bool FDreamFXSourceMigration::RebaseWorkspaceFolders(
		const FString& InText, const FString& InOldDirectory, const FString& InNewDirectory, FString& OutText)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(InText), Root) || !Root.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Folders = nullptr;
		if (!Root->TryGetArrayField(TEXT("folders"), Folders))
		{
			return false;
		}

		OutText = InText;
		FString NewBase = InNewDirectory;
		FPaths::NormalizeDirectoryName(NewBase);
		NewBase += TEXT("/");
		for (const TSharedPtr<FJsonValue>& Folder : *Folders)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Path;
			if (!Folder.IsValid() || !Folder->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("path"), Path))
			{
				continue;
			}
			// "." is the source directory itself, wherever it is; an absolute path does not depend on where the file is.
			if (Path == TEXT(".") || !FPaths::IsRelative(Path))
			{
				continue;
			}
			FString Absolute = FPaths::ConvertRelativePathToFull(InOldDirectory, Path);
			FPaths::NormalizeDirectoryName(Absolute);
			FString Rebased = Absolute;
			if (!FPaths::MakePathRelativeTo(Rebased, *NewBase))
			{
				Rebased = Absolute;
			}
			// The text is edited, not re-serialized: the file is the author's too, and re-printing it would reflow it.
			OutText.ReplaceInline(
				*FString::Printf(TEXT("\"%s\""), *Path.ReplaceCharWithEscapedChar()),
				*FString::Printf(TEXT("\"%s\""), *Rebased.ReplaceCharWithEscapedChar()),
				ESearchCase::CaseSensitive);
		}
		return true;
	}

	bool FDreamFXSourceMigration::Migrate(FString& OutError)
	{
		using namespace SourceMigrationLocal;
		const FProjectSourceDirectory Project = FDreamFXPaths::ResolveProjectSourceDirectory();
		if (!Project.bUsingLegacy)
		{
			OutError = Project.bLegacyIgnored
				? FString::Printf(TEXT("'%s' already exists; merge '%s' into it by hand."), *Project.ConfiguredDirectory, *Project.LegacyDirectory)
				: FString::Printf(TEXT("'%s' is not in use, so there is nothing to move."), *Project.LegacyDirectory);
			return false;
		}
		if (IFileManager::Get().DirectoryExists(*Project.ConfiguredDirectory))
		{
			OutError = FString::Printf(TEXT("'%s' already exists; merge '%s' into it by hand."), *Project.ConfiguredDirectory, *Project.LegacyDirectory);
			return false;
		}

		IFileManager& FileManager = IFileManager::Get();
		FileManager.MakeDirectory(*FPaths::GetPath(Project.ConfiguredDirectory), /*Tree=*/true);

		// A rename first: same volume, one step, nothing half-done if it fails -- and a `.git` inside `DFX/` (a project
		// keeping its effects in their own repository) goes along untouched. The editor's own directory watcher can
		// refuse it on Windows; then copy and delete, and if the delete is what fails, both folders exist and the
		// configured one wins after the restart -- the notice then says to delete the old one, and nothing was lost.
		if (!FileManager.Move(*Project.ConfiguredDirectory, *Project.LegacyDirectory, /*Replace=*/false, /*EvenIfReadOnly=*/true))
		{
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			if (!PlatformFile.CopyDirectoryTree(*Project.ConfiguredDirectory, *Project.LegacyDirectory, /*bOverwriteAllExisting=*/false))
			{
				FileManager.DeleteDirectory(*Project.ConfiguredDirectory, /*RequireExists=*/false, /*Tree=*/true);
				OutError = FString::Printf(TEXT("could not move or copy '%s' to '%s'."), *Project.LegacyDirectory, *Project.ConfiguredDirectory);
				return false;
			}
			if (!FileManager.DeleteDirectory(*Project.LegacyDirectory, /*RequireExists=*/false, /*Tree=*/true))
			{
				UE_LOG(LogDreamFX, Warning,
					TEXT("DreamFX: copied '%s' to '%s' but could not delete the old folder; delete it once nothing holds it open."),
					*Project.LegacyDirectory, *Project.ConfiguredDirectory);
			}
		}

		// The old index files travelled with the folder; they become `.dreamfx/index.db` in the new place. A failure
		// here loses nothing -- the files stay, and the next `dfx index` imports them.
		FString ImportError;
		if (!FIndexExport::ImportLegacyIndex(Project.ConfiguredDirectory, ImportError))
		{
			UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: the old index files were not imported yet: %s"), *ImportError);
		}

		// The workspace file names the plugin roots relative to itself, and it is one level deeper now.
		const FString WorkspacePath = FPaths::Combine(Project.ConfiguredDirectory, WorkspaceFileName);
		FString WorkspaceText;
		if (FFileHelper::LoadFileToString(WorkspaceText, *WorkspacePath))
		{
			FString Rebased;
			if (RebaseWorkspaceFolders(WorkspaceText, Project.LegacyDirectory, Project.ConfiguredDirectory, Rebased)
				&& !Rebased.Equals(WorkspaceText, ESearchCase::CaseSensitive))
			{
				FFileHelper::SaveStringToFile(Rebased, *WorkspacePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
		}

		// What the DreamSource layout replaced: the old bridge folder, with its diagnostics.json. The next editor session
		// listens in Intermediate/DreamSource/FX/Bridge instead.
		DeleteTree(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamFX"), TEXT("Bridge"))));

		UE_LOG(LogDreamFX, Display, TEXT("DreamFX: moved '%s' to '%s'. Restart the editor to use it."),
			*Project.LegacyDirectory, *Project.ConfiguredDirectory);
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
