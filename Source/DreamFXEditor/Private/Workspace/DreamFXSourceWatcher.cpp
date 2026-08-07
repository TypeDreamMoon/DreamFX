#include "DreamFXSourceWatcher.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "Generation/DreamFXGenerator.h"
#include "SourceFiles/DreamFXPaths.h"

#include "DirectoryWatcherModule.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IDirectoryWatcher.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DreamFXSourceWatcher"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/**
		 * Editors write a file more than once per save -- a temp file, a rename, a metadata touch --
		 * and a rebuild per event would mean three compiles for one Ctrl-S. Collecting changes and
		 * building after a quiet interval is what makes save-to-rebuild usable.
		 */
		constexpr float DebounceSeconds = 0.75f;

		TMap<FString, FDelegateHandle> GWatchHandles;
		FTSTicker::FDelegateHandle GTickerHandle;
		TSet<FString> GPendingFiles;
		double GLastChangeTime = 0.0;

		void ReportResult(const FString& SourceFile, const FDiagnosticSink& Diagnostics, bool bSucceeded)
		{
			for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
			{
				switch (Diagnostic.Severity)
				{
				case EDiagnosticSeverity::Error:
					UE_LOG(LogDreamFX, Error, TEXT("%s"), *Diagnostic.Format());
					break;
				case EDiagnosticSeverity::Warning:
					UE_LOG(LogDreamFX, Warning, TEXT("%s"), *Diagnostic.Format());
					break;
				default:
					UE_LOG(LogDreamFX, Display, TEXT("%s"), *Diagnostic.Format());
					break;
				}
			}

			// A toast only for failures. A successful rebuild already shows itself in the open asset
			// editor, and a notification per save would be noise on the happy path.
			if (bSucceeded)
			{
				return;
			}

			const FDiagnostic* FirstError = Diagnostics.GetDiagnostics().FindByPredicate(
				[](const FDiagnostic& Candidate) { return Candidate.Severity == EDiagnosticSeverity::Error; });

			FNotificationInfo Info(FText::Format(
				LOCTEXT("DreamFXWatchFailed", "DreamFX build failed: {0}"),
				FText::FromString(FirstError ? FirstError->Format() : SourceFile)));
			Info.ExpireDuration = 10.0f;
			Info.bFireAndForget = true;
			FSlateNotificationManager::Get().AddNotification(Info);
		}

		bool Tick(float /*DeltaTime*/)
		{
			if (GPendingFiles.Num() == 0)
			{
				return true;
			}

			if (FPlatformTime::Seconds() - GLastChangeTime < DebounceSeconds)
			{
				return true;
			}

			TArray<FString> Files = GPendingFiles.Array();
			GPendingFiles.Reset();
			Files.Sort();

			FGenerateOptions Options;
			Options.bSave = true;
			// The hash is what the watcher exists to react to, but the file may also have been
			// rebuilt already by a manual run; forcing avoids a confusing "nothing happened" on save.
			Options.bForce = true;

			for (const FString& File : Files)
			{
				if (!FPaths::FileExists(File))
				{
					continue; // deleted or renamed between the event and now
				}

				UE_LOG(LogDreamFX, Display, TEXT("Rebuilding '%s' after save."), *File);

				FDiagnosticSink Diagnostics;
				const FGenerateResult Result = FGenerator::GenerateFromFile(File, Options, Diagnostics);
				ReportResult(File, Diagnostics, Result.bSucceeded);
			}

			return true;
		}

		void OnDirectoryChanged(const TArray<FFileChangeData>& Changes)
		{
			for (const FFileChangeData& Change : Changes)
			{
				if (Change.Action == FFileChangeData::FCA_Removed)
				{
					continue;
				}
				if (!FDreamFXPaths::IsSourceFile(Change.Filename))
				{
					continue;
				}
				GPendingFiles.Add(FPaths::ConvertRelativePathToFull(Change.Filename));
			}

			if (GPendingFiles.Num() > 0)
			{
				GLastChangeTime = FPlatformTime::Seconds();
			}
		}
	}

	void FSourceWatcher::Register()
	{
		FDirectoryWatcherModule& Module = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(
			TEXT("DirectoryWatcher"));
		IDirectoryWatcher* Watcher = Module.Get();
		if (Watcher == nullptr)
		{
			return;
		}

		for (const FSourceRoot& Root : FDreamFXPaths::GetSourceRoots())
		{
			FDelegateHandle Handle;
			if (Watcher->RegisterDirectoryChangedCallback_Handle(
				Root.Directory,
				IDirectoryWatcher::FDirectoryChanged::CreateStatic(&OnDirectoryChanged),
				Handle,
				IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges))
			{
				GWatchHandles.Add(Root.Directory, Handle);
				UE_LOG(LogDreamFX, Display, TEXT("Watching '%s' for source changes."), *Root.Directory);
			}
		}

		if (GWatchHandles.Num() > 0)
		{
			GTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateStatic(&Tick), /*InDelay=*/0.25f);
		}
	}

	void FSourceWatcher::Unregister()
	{
		if (GTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GTickerHandle);
			GTickerHandle.Reset();
		}

		if (FDirectoryWatcherModule* Module = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(
			TEXT("DirectoryWatcher")))
		{
			if (IDirectoryWatcher* Watcher = Module->Get())
			{
				for (const TPair<FString, FDelegateHandle>& Entry : GWatchHandles)
				{
					Watcher->UnregisterDirectoryChangedCallback_Handle(Entry.Key, Entry.Value);
				}
			}
		}

		GWatchHandles.Reset();
		GPendingFiles.Reset();
	}

	void FSourceWatcher::FlushPending()
	{
		GLastChangeTime = 0.0;
		Tick(0.0f);
	}
}

#undef LOCTEXT_NAMESPACE
