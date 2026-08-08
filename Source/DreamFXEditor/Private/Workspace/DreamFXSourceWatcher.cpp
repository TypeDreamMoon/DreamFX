#include "DreamFXSourceWatcher.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "SourceFiles/DreamFXPaths.h"
#include "Workspace/DreamFXWorkspaceService.h"

#include "Algo/StableSort.h"
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

		/** Set by an explicit command; a plain save leaves it false and stays quiet when it worked. */
		bool GAnnounceSuccess = false;

		/** What one drained queue did, and where to send the author when it did not work. */
		struct FBatchResult
		{
			int32 Built = 0;
			int32 Skipped = 0;
			int32 Failed = 0;

			FString FirstErrorFile;
			int32 FirstErrorLine = 1;
			int32 FirstErrorColumn = 1;
			FString FirstErrorText;

			bool HasFirstError() const { return !FirstErrorFile.IsEmpty(); }
		};

		/**
		 * Modules before emitters before systems.
		 *
		 * The same ordering the commandlet uses: a .dfs resolves the modules it calls out of the asset
		 * library at build time, so a .dfm and its caller queued together only work if the module asset
		 * lands first. Irrelevant for a one-file save, load-bearing for *Rebuild DFX*.
		 */
		int32 BuildOrder(const FString& File)
		{
			EDocumentKind Kind = EDocumentKind::System;
			FParser::DocumentKindFromExtension(FPaths::GetExtension(File), Kind);
			switch (Kind)
			{
			case EDocumentKind::Module:
			case EDocumentKind::DynamicInput: return 0;
			case EDocumentKind::Emitter:      return 1;
			default:                          return 2;
			}
		}

		/** Remembers the first error's position so the toast can open the file on the offending token. */
		void RecordFirstError(FBatchResult& Batch, const FString& SourceFile, const FDiagnosticSink& Diagnostics)
		{
			if (Batch.HasFirstError())
			{
				return;
			}

			const FDiagnostic* FirstError = Diagnostics.GetDiagnostics().FindByPredicate(
				[](const FDiagnostic& Candidate) { return Candidate.Severity == EDiagnosticSeverity::Error; });

			// A diagnostic carries its own file: a .dfs that pulls in a broken .dfe fails at the .dfe's
			// position, and sending the author to the .dfs would be sending them to the wrong file.
			Batch.FirstErrorFile = (FirstError && !FirstError->File.IsEmpty()) ? FirstError->File : SourceFile;
			Batch.FirstErrorLine = FirstError ? FMath::Max(1, FirstError->Location.Line) : 1;
			Batch.FirstErrorColumn = FirstError ? FMath::Max(1, FirstError->Location.Column) : 1;
			Batch.FirstErrorText = FirstError ? FirstError->Format() : SourceFile;
		}

		/**
		 * One notification for the whole batch.
		 *
		 * plan-v3 E5: a failure toast carries the jump. The diagnostic has had a line and column all
		 * along -- it only ever reached a log line, so the author read the position and then navigated
		 * by hand.
		 */
		void ReportBatch(const FBatchResult& Batch, bool bAnnounceSuccess)
		{
			if (Batch.Failed == 0 && !bAnnounceSuccess)
			{
				return;
			}

			FNotificationInfo Info(FText::GetEmpty());
			Info.ExpireDuration = Batch.Failed > 0 ? 10.0f : 5.0f;
			Info.bFireAndForget = true;

			if (Batch.Failed > 0)
			{
				Info.Text = FText::Format(
					LOCTEXT("DreamFXBuildFailed", "DreamFX build failed: {0}"),
					FText::FromString(Batch.FirstErrorText));

				if (Batch.HasFirstError())
				{
					const FString File = Batch.FirstErrorFile;
					const int32 Line = Batch.FirstErrorLine;
					const int32 Column = Batch.FirstErrorColumn;
					Info.Hyperlink = FSimpleDelegate::CreateLambda([File, Line, Column]()
					{
						FDreamFXLaunchUtils::LaunchTextFileInPreferredEditor(File, Line, Column);
					});
					Info.HyperlinkText = LOCTEXT("DreamFXOpenFirstError", "Open in VSCode");
				}
			}
			else
			{
				Info.Text = FText::Format(
					LOCTEXT("DreamFXBuildSucceeded", "DreamFX: {0} built, {1} up to date."),
					FText::AsNumber(Batch.Built), FText::AsNumber(Batch.Skipped));
			}

			const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
			if (Notification.IsValid())
			{
				Notification->SetCompletionState(
					Batch.Failed > 0 ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
			}
		}

		void DrainQueue()
		{
			TArray<FString> Files = GPendingFiles.Array();
			GPendingFiles.Reset();

			const bool bAnnounceSuccess = GAnnounceSuccess;
			GAnnounceSuccess = false;

			Files.Sort();
			Algo::StableSortBy(Files, [](const FString& File) { return BuildOrder(File); });

			FGenerateOptions Options;
			Options.bSave = true;
			// The hash is what the watcher exists to react to, but the file may also have been
			// rebuilt already by a manual run; forcing avoids a confusing "nothing happened" on save.
			Options.bForce = true;

			FBatchResult Batch;

			for (const FString& File : Files)
			{
				if (!FPaths::FileExists(File))
				{
					continue; // deleted or renamed between the event and now
				}

				// A .dfe generates nothing of its own -- it is merged into its host by copy (R3) -- so
				// building it would report a failure for a file that is not broken. Parsing it still
				// catches a syntax error at save time rather than in whatever .dfs pulls it in.
				EDocumentKind Kind = EDocumentKind::System;
				FParser::DocumentKindFromExtension(FPaths::GetExtension(File), Kind);
				const bool bGenerates = Kind != EDocumentKind::Emitter;

				UE_LOG(LogDreamFX, Display, TEXT("Rebuilding '%s' after save."), *File);

				FDiagnosticSink Diagnostics;
				bool bSucceeded;
				if (bGenerates)
				{
					const FGenerateResult Result = FGenerator::GenerateFromFile(File, Options, Diagnostics);
					bSucceeded = Result.bSucceeded;
					if (Result.bSkipped)
					{
						++Batch.Skipped;
					}
					else if (Result.bSucceeded)
					{
						++Batch.Built;
					}
				}
				else
				{
					FDocument Document;
					bSucceeded = FParser::ParseFile(File, Document, Diagnostics);
					if (bSucceeded)
					{
						++Batch.Skipped;
					}
				}

				LogDiagnostics(Diagnostics);

				if (!bSucceeded)
				{
					++Batch.Failed;
					RecordFirstError(Batch, File, Diagnostics);
				}
			}

			ReportBatch(Batch, bAnnounceSuccess);
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

			DrainQueue();
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
				// Decompiled exports included (plan-v4 V1-3): they rebuild a mirror under
				// `Decompiled/`, never the asset they were read from, so save-to-rebuild is as safe
				// here as anywhere else. Skipping them was the reason editing an export and saving
				// produced no build and no message at all.
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

		// The ticker is what drains the queue, so it has to exist even with nothing watched -- the
		// menu commands queue through the same path and a project with no DFX roots yet still has a
		// *Rebuild DFX* entry.
		GTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&Tick), /*InDelay=*/0.25f);
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
		GAnnounceSuccess = false;
	}

	void FSourceWatcher::FlushPending()
	{
		GLastChangeTime = 0.0;
		Tick(0.0f);
	}

	void FSourceWatcher::QueueFile(const FString& FilePath, const bool bAnnounceSuccess)
	{
		GPendingFiles.Add(FPaths::ConvertRelativePathToFull(FilePath));
		GLastChangeTime = FPlatformTime::Seconds();
		GAnnounceSuccess |= bAnnounceSuccess;
	}

	int32 FSourceWatcher::QueueAllSources(const bool bAnnounceSuccess)
	{
		// A plugin may have been enabled since the last scan, and *Rebuild DFX* is exactly when
		// someone would expect a newly added root to be picked up.
		FDreamFXPaths::InvalidateSourceRoots();

		TArray<FString> Files;
		FDreamFXPaths::FindSourceFiles(Files);
		for (const FString& File : Files)
		{
			GPendingFiles.Add(File);
		}

		GLastChangeTime = FPlatformTime::Seconds();
		GAnnounceSuccess |= bAnnounceSuccess;
		return Files.Num();
	}
}

#undef LOCTEXT_NAMESPACE
