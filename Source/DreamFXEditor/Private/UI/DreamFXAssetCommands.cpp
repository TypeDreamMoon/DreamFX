#include "DreamFXAssetCommands.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXProvenance.h"
#include "SourceFiles/DreamFXPaths.h"
#include "Workspace/DreamFXSourceWatcher.h"
#include "Workspace/DreamFXWorkspaceService.h"

#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DreamFXAssetCommands"

namespace UE::DreamFX::Editor
{
	namespace
	{
		void Notify(const FText& Message, const bool bSuccess, const float ExpireDuration = 6.0f)
		{
			FNotificationInfo Info(Message);
			Info.ExpireDuration = ExpireDuration;
			Info.bFireAndForget = true;

			const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
			if (Notification.IsValid())
			{
				Notification->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			}
		}

		/** A failure toast that puts the file one click away, same as the build queue's (E5). */
		void NotifyWithFile(const FText& Message, const bool bSuccess, const FString& FilePath,
			const int32 Line = 1, const int32 Column = 1)
		{
			FNotificationInfo Info(Message);
			Info.ExpireDuration = 10.0f;
			Info.bFireAndForget = true;

			if (!FilePath.IsEmpty())
			{
				const FString Path = FilePath;
				Info.HyperlinkText = LOCTEXT("OpenInVSCode", "Open in VSCode");
				Info.Hyperlink = FSimpleDelegate::CreateLambda([Path, Line, Column]()
				{
					FDreamFXLaunchUtils::LaunchTextFileInPreferredEditor(Path, Line, Column);
				});
			}

			const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
			if (Notification.IsValid())
			{
				Notification->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			}
		}

		/** Where an asset's source would live, and how a `Root="..."` names it. */
		struct FAssetRoot
		{
			/** The token to stamp into the file, e.g. "Game" or "Plugin.MoonToon". */
			FString RootToken;
			/** The DFX directory that owns sources for this mount point. */
			FString SourceRootDirectory;
			/** "/Game", "/MoonToon". */
			FString MountPoint;
		};

		/**
		 * Derives the root from where the asset is mounted.
		 *
		 * Deliberately not "whichever root the user last used": an asset under `/MoonToon` adopted into
		 * the project's `DFX/` would build back into `/Game` and quietly leave the original behind.
		 */
		bool ResolveAssetRoot(const FString& PackagePath, FAssetRoot& OutRoot, FString& OutError)
		{
			if (!FDreamFXPaths::ResolveRootTokenForPackage(PackagePath, OutRoot.RootToken,
				OutRoot.MountPoint, OutError))
			{
				return false;
			}

			// Only Adopt needs the directory, and only because it writes a real source file there.
			if (OutRoot.RootToken == TEXT("Game"))
			{
				OutRoot.SourceRootDirectory = FPaths::ConvertRelativePathToFull(
					FPaths::Combine(FPaths::ProjectDir(), TEXT("DFX")));
				return true;
			}

			const FString PluginName = OutRoot.RootToken.RightChop(FCString::Strlen(TEXT("Plugin.")));
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			if (!Plugin.IsValid())
			{
				OutError = FString::Printf(TEXT("Plugin '%s' went missing between mount lookup and use."),
					*PluginName);
				return false;
			}

			OutRoot.SourceRootDirectory = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(Plugin->GetBaseDir(), TEXT("DFX")));
			return true;
		}

		/** "/Game/FX/NS_Spark" with mount point "/Game" -> "FX/NS_Spark". */
		FString PackagePathRelativeToMount(const FString& PackagePath, const FString& MountPoint)
		{
			FString Relative = PackagePath;
			Relative.RemoveFromStart(MountPoint, ESearchCase::IgnoreCase);
			Relative.RemoveFromStart(TEXT("/"));
			return Relative;
		}

		/** The first line that differs, so a failed comparison names a place instead of a size. */
		FString DiffFirstLine(const FString& Left, const FString& Right)
		{
			TArray<FString> LeftLines;
			TArray<FString> RightLines;
			Left.ParseIntoArrayLines(LeftLines, /*InCullEmpty=*/false);
			Right.ParseIntoArrayLines(RightLines, /*InCullEmpty=*/false);

			const int32 Count = FMath::Max(LeftLines.Num(), RightLines.Num());
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FString LeftLine = LeftLines.IsValidIndex(Index) ? LeftLines[Index] : FString(TEXT("<end of file>"));
				const FString RightLine = RightLines.IsValidIndex(Index) ? RightLines[Index] : FString(TEXT("<end of file>"));
				if (LeftLine != RightLine)
				{
					return FString::Printf(TEXT("line %d:\n  written : %s\n  re-read : %s"),
						Index + 1, *LeftLine, *RightLine);
				}
			}
			return TEXT("(the two exports differ only in line endings)");
		}

		/**
		 * Finds a .dfs that already claims this asset.
		 *
		 * Two sources generating one asset is not an error either of them can detect at build time --
		 * they just take turns overwriting each other, and whichever ran last wins. The only place to
		 * catch it is here, when a second source is about to be created.
		 */
		bool FindConflictingSource(const FString& TargetPackagePath, const FString& IgnoreFilePath,
			FString& OutConflictFile)
		{
			TArray<FString> SourceFiles;
			FDreamFXPaths::FindSourceFiles(SourceFiles);

			for (const FString& SourceFile : SourceFiles)
			{
				if (!FPaths::GetExtension(SourceFile).Equals(TEXT("dfs"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (FPaths::IsSamePath(SourceFile, IgnoreFilePath))
				{
					continue;
				}

				FDiagnosticSink Ignored;
				FDocument Document;
				if (!FParser::ParseFile(SourceFile, Document, Ignored))
				{
					continue; // a broken source claims nothing
				}

				FString PackagePath;
				FString Error;
				if (!FDreamFXPaths::ResolveAssetPath(Document.Name, Document.Root, PackagePath, Error))
				{
					continue;
				}

				if (PackagePath.Equals(TargetPackagePath, ESearchCase::IgnoreCase))
				{
					OutConflictFile = SourceFile;
					return true;
				}
			}

			return false;
		}

		/** Writes an export next to where the asset lives, opens it, and toasts the outcome. */
		void FinishExport(const FString& AssetName, const FString& PackagePath, const FString& Extension,
			const FDecompileResult& Result)
		{
			const FString OutputPath = FDreamFXPaths::DecompiledSourcePathFor(PackagePath, *Extension);

			if (!FFileHelper::SaveStringToFile(Result.Source, *OutputPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogDreamFX, Error, TEXT("Could not write '%s'."), *OutputPath);
				Notify(FText::Format(LOCTEXT("ExportWriteFailed", "DreamFX could not write '{0}'."),
					FText::FromString(OutputPath)), /*bSuccess=*/false);
				return;
			}

			UE_LOG(LogDreamFX, Display, TEXT("Exported %s to %s"), *PackagePath, *OutputPath);
			for (const FString& Feature : Result.UnsupportedFeatures)
			{
				UE_LOG(LogDreamFX, Warning, TEXT("Not represented in the export: %s"), *Feature);
			}

			FDreamFXLaunchUtils::LaunchTextFileInPreferredEditor(OutputPath);

			if (Result.UnsupportedFeatures.Num() > 0)
			{
				NotifyWithFile(FText::Format(
					LOCTEXT("ExportWithGaps", "Exported '{0}' with {1} feature(s) NOT represented -- they are listed at the top of the file."),
					FText::FromString(AssetName),
					FText::AsNumber(Result.UnsupportedFeatures.Num())), /*bSuccess=*/false, OutputPath);
				return;
			}

			NotifyWithFile(FText::Format(LOCTEXT("ExportOk", "Exported '{0}' to {1}"),
				FText::FromString(AssetName), FText::FromString(OutputPath)), /*bSuccess=*/true, OutputPath);
		}

		/**
		 * Refuses to export an asset that is itself a mirror, and says where its source is.
		 *
		 * Exporting one would write a second file claiming the same asset, under a disk path with
		 * `Decompiled` in it twice -- two sources for one asset, taking turns overwriting each other
		 * on alternate builds. The mirror already has a source; that is what the author wants open.
		 */
		bool RefuseMirrorExport(UObject* Asset, const FString& PackagePath)
		{
			if (!FDreamFXPaths::IsDecompiledNamespaceAsset(PackagePath))
			{
				return false;
			}

			// The stamp, not a path guess: the mirror's source lives at the *original* asset's export
			// path, which cannot be recovered from the mirror's own path.
			FProvenanceStamp Stamp;
			const bool bHasSource = FProvenance::Read(Asset, Stamp)
				&& !Stamp.SourceFullPath.IsEmpty()
				&& FPaths::FileExists(Stamp.SourceFullPath);

			NotifyWithFile(FText::Format(
				LOCTEXT("ExportRefusedMirror",
					"'{0}' is already a DreamFX mirror, built from a decompiled source. Edit that source instead -- exporting again would leave two sources claiming one asset."),
				FText::FromString(Asset != nullptr ? Asset->GetName() : PackagePath)), /*bSuccess=*/false,
				bHasSource ? Stamp.SourceFullPath : FString());
			return true;
		}

		/** The stamp, or nothing -- every per-asset command needs it and reports the same way when absent. */
		bool RequireProvenance(UObject* Asset, FProvenanceStamp& OutStamp)
		{
			if (Asset == nullptr)
			{
				return false;
			}

			if (!FProvenance::Read(Asset, OutStamp) || OutStamp.SourceFullPath.IsEmpty())
			{
				Notify(FText::Format(
					LOCTEXT("NoProvenance", "'{0}' was not generated by DreamFX -- it has no source file."),
					FText::FromString(Asset->GetName())), /*bSuccess=*/false);
				return false;
			}

			if (!FPaths::FileExists(OutStamp.SourceFullPath))
			{
				Notify(FText::Format(
					LOCTEXT("SourceMissing", "'{0}' names source '{1}', which is not on disk."),
					FText::FromString(Asset->GetName()),
					FText::FromString(OutStamp.SourceFullPath)), /*bSuccess=*/false);
				return false;
			}

			return true;
		}
	}

	// ------------------------------------------------------------------------------- project-wide

	void FDreamFXCommands::RebuildAll()
	{
		const int32 Queued = FSourceWatcher::QueueAllSources(/*bAnnounceSuccess=*/true);
		UE_LOG(LogDreamFX, Display, TEXT("Queued %d source file(s) for a full rebuild."), Queued);

		if (Queued == 0)
		{
			Notify(LOCTEXT("NothingToRebuild", "DreamFX found no .dfs/.dfe/.dfm sources to rebuild."),
				/*bSuccess=*/false);
		}
	}

	void FDreamFXCommands::VerifyAll()
	{
		FDreamFXPaths::InvalidateSourceRoots();

		TArray<FString> SourceFiles;
		FDreamFXPaths::FindSourceFiles(SourceFiles);

		FGenerateOptions Options;
		Options.bVerifyOnly = true;
		Options.bSave = false;
		Options.bForce = true;

		int32 Checked = 0;
		int32 Drifted = 0;
		int32 Failed = 0;

		for (const FString& SourceFile : SourceFiles)
		{
			// A .dfe generates no asset of its own, so there is nothing to verify it against.
			EDocumentKind Kind = EDocumentKind::System;
			FParser::DocumentKindFromExtension(FPaths::GetExtension(SourceFile), Kind);
			if (Kind == EDocumentKind::Emitter)
			{
				continue;
			}

			++Checked;

			FDiagnosticSink Diagnostics;
			const FGenerateResult Result = FGenerator::GenerateFromFile(SourceFile, Options, Diagnostics);
			LogDiagnostics(Diagnostics);

			if (Result.bDrifted)
			{
				++Drifted;
			}
			if (!Result.bSucceeded)
			{
				++Failed;
			}
		}

		UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX verify: %d checked, %d drifted, %d failed ==="),
			Checked, Drifted, Failed);

		if (Drifted == 0 && Failed == 0)
		{
			Notify(FText::Format(
				LOCTEXT("VerifyClean", "DreamFX: {0} source(s) verified, all assets in step."),
				FText::AsNumber(Checked)), /*bSuccess=*/true);
			return;
		}

		Notify(FText::Format(
			LOCTEXT("VerifyDrift", "DreamFX: {0} of {1} source(s) out of step ({2} failed). See the Output Log."),
			FText::AsNumber(Drifted), FText::AsNumber(Checked), FText::AsNumber(Failed)), /*bSuccess=*/false);
	}

	void FDreamFXCommands::OpenWorkspace()
	{
		FString WorkspacePath;
		FString Error;
		if (!FDreamFXWorkspaceService::WriteWorkspaceFile(WorkspacePath, Error))
		{
			UE_LOG(LogDreamFX, Error, TEXT("%s"), *Error);
			Notify(FText::Format(LOCTEXT("WorkspaceWriteFailed", "DreamFX failed to create workspace: {0}"),
				FText::FromString(Error)), /*bSuccess=*/false);
			return;
		}

		if (FDreamFXLaunchUtils::LaunchVSCodeWorkspace(WorkspacePath))
		{
			Notify(FText::Format(LOCTEXT("WorkspaceOpenedVSCode", "Opened DreamFX workspace in VSCode: {0}"),
				FText::FromString(WorkspacePath)), /*bSuccess=*/true);
			return;
		}

		if (FPlatformProcess::LaunchFileInDefaultExternalApplication(*WorkspacePath, nullptr, ELaunchVerb::Edit, false))
		{
			Notify(FText::Format(LOCTEXT("WorkspaceOpened", "Opened DreamFX workspace: {0}"),
				FText::FromString(WorkspacePath)), /*bSuccess=*/true);
			return;
		}

		if (FDreamFXLaunchUtils::LaunchTextFileWithNotepad(WorkspacePath))
		{
			Notify(FText::Format(LOCTEXT("WorkspaceOpenedNotepad", "Opened DreamFX workspace in Notepad: {0}"),
				FText::FromString(WorkspacePath)), /*bSuccess=*/true);
			return;
		}

		Notify(FText::Format(LOCTEXT("WorkspaceOpenFailed", "DreamFX could not open workspace: {0}"),
			FText::FromString(WorkspacePath)), /*bSuccess=*/false);
	}

	// ------------------------------------------------------------------------------- per-asset

	bool FDreamFXCommands::HasProvenance(const UObject* Asset)
	{
		if (Asset == nullptr)
		{
			return false;
		}

		FProvenanceStamp Stamp;
		return FProvenance::Read(Asset, Stamp) && !Stamp.SourceFullPath.IsEmpty();
	}

	void FDreamFXCommands::ExportSystem(UNiagaraSystem* System)
	{
		if (System == nullptr)
		{
			return;
		}

		const FString PackagePath = System->GetOutermost()->GetName();

		if (RefuseMirrorExport(System, PackagePath))
		{
			return;
		}

		FAssetRoot Root;
		FString RootError;
		if (!ResolveAssetRoot(PackagePath, Root, RootError))
		{
			UE_LOG(LogDreamFX, Error, TEXT("%s"), *RootError);
			Notify(FText::FromString(RootError), /*bSuccess=*/false);
			return;
		}

		// Export names the mirror, not the asset it read: whatever the author does to the file
		// afterwards, building it cannot reach this asset. Adopt is the command that opts into that.
		FDecompileOptions DecompileOptions;
		DecompileOptions.bDecompiledNamespace = true;

		FDiagnosticSink Diagnostics;
		const FDecompileResult Result = FDecompiler::Decompile(System, Root.RootToken, Diagnostics,
			DecompileOptions);
		LogDiagnostics(Diagnostics);

		if (!Result.bSucceeded)
		{
			Notify(FText::Format(LOCTEXT("ExportFailed", "DreamFX could not export '{0}'. See the Output Log."),
				FText::FromString(System->GetName())), /*bSuccess=*/false);
			return;
		}

		FinishExport(System->GetName(), PackagePath, TEXT(".dfs"), Result);
	}

	void FDreamFXCommands::ExportEmitter(UNiagaraEmitter* Emitter)
	{
		if (Emitter == nullptr)
		{
			return;
		}

		const FString PackagePath = Emitter->GetOutermost()->GetName();

		if (RefuseMirrorExport(Emitter, PackagePath))
		{
			return;
		}

		FAssetRoot Root;
		FString RootError;
		if (!ResolveAssetRoot(PackagePath, Root, RootError))
		{
			UE_LOG(LogDreamFX, Error, TEXT("%s"), *RootError);
			Notify(FText::FromString(RootError), /*bSuccess=*/false);
			return;
		}

		FDecompileOptions DecompileOptions;
		DecompileOptions.bDecompiledNamespace = true;

		FDiagnosticSink Diagnostics;
		const FDecompileResult Result = FDecompiler::DecompileEmitter(Emitter, Root.RootToken, Diagnostics,
			DecompileOptions);
		LogDiagnostics(Diagnostics);

		if (!Result.bSucceeded)
		{
			Notify(FText::Format(
				LOCTEXT("ExportEmitterFailed", "DreamFX could not export emitter '{0}'. See the Output Log."),
				FText::FromString(Emitter->GetName())), /*bSuccess=*/false);
			return;
		}

		FinishExport(Emitter->GetName(), PackagePath, TEXT(".dfe"), Result);
	}

	void FDreamFXCommands::AdoptSystem(UNiagaraSystem* System, const bool bSkipConfirmation)
	{
		if (System == nullptr)
		{
			return;
		}

		const FString PackagePath = System->GetOutermost()->GetName();

		FAssetRoot Root;
		FString RootError;
		if (!ResolveAssetRoot(PackagePath, Root, RootError))
		{
			UE_LOG(LogDreamFX, Error, TEXT("%s"), *RootError);
			Notify(FText::FromString(RootError), /*bSuccess=*/false);
			return;
		}

		// --- 1. decompile, and refuse anything lossy -------------------------------------------
		FDiagnosticSink ExportDiagnostics;
		const FDecompileResult Export = FDecompiler::Decompile(System, Root.RootToken, ExportDiagnostics);
		LogDiagnostics(ExportDiagnostics);

		if (!Export.bSucceeded)
		{
			Notify(FText::Format(LOCTEXT("AdoptDecompileFailed", "DreamFX could not decompile '{0}'. See the Output Log."),
				FText::FromString(System->GetName())), /*bSuccess=*/false);
			return;
		}

		if (Export.UnsupportedFeatures.Num() > 0)
		{
			TArray<FString> Sorted = Export.UnsupportedFeatures;
			Sorted.Sort();

			// The sink has to be spelled `Diagnostics`: gen-diagnostics.ps1 scans for that exact
			// receiver, and a code it cannot see is a code that never reaches Docs/diagnostics/.
			{
				FDiagnosticSink Diagnostics;
				for (const FString& Feature : Sorted)
				{
					Diagnostics.Error(TEXT("DFX8010"), FSourceLocation(), FString::Printf(
						TEXT("'%s' cannot be adopted: '%s' has no DreamFXLang form yet, so adopting would destroy it on the first rebuild. Export .dfs instead."),
						*PackagePath, *Feature));
				}
				LogDiagnostics(Diagnostics);
			}

			const FText Refusal = FText::Format(
				LOCTEXT("AdoptRefusedGaps",
					"DreamFX cannot adopt '{0}'.\n\n{1} feature(s) of this asset have no DreamFXLang form yet:\n\n{2}\n\n"
					"Adopting makes the text the only source of truth, so adopting now would destroy them on the first rebuild. "
					"Use Export .dfs instead -- it keeps the asset as it is and lists the gaps in the file."),
				FText::FromString(System->GetName()),
				FText::AsNumber(Sorted.Num()),
				FText::FromString(FString::Join(Sorted, TEXT("\n"))));

			// A modal is right for a human and wrong for a script -- it would block the calling thread
			// with no one to click it. The gate itself is unchanged either way.
			if (bSkipConfirmation)
			{
				Notify(Refusal, /*bSuccess=*/false, /*ExpireDuration=*/12.0f);
			}
			else
			{
				FMessageDialog::Open(EAppMsgType::Ok, Refusal);
			}
			return;
		}

		// --- 2. where the source has to live ---------------------------------------------------
		const FString RelativePath = PackagePathRelativeToMount(PackagePath, Root.MountPoint);
		const FString SourcePath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(Root.SourceRootDirectory, RelativePath + TEXT(".dfs")));

		// --- 3. refuse a second source for one asset -------------------------------------------
		FString ConflictFile;
		if (FindConflictingSource(PackagePath, SourcePath, ConflictFile))
		{
			{
				FDiagnosticSink Diagnostics;
				Diagnostics.SetFile(ConflictFile);
				Diagnostics.Error(TEXT("DFX8011"), FSourceLocation(), FString::Printf(
					TEXT("'%s' is already generated by an existing source, so a second one would silently overwrite it on alternate builds. Edit that file instead."),
					*PackagePath));
				LogDiagnostics(Diagnostics);
			}

			const FText Refusal = FText::Format(
				LOCTEXT("AdoptRefusedConflict",
					"DreamFX cannot adopt '{0}'.\n\nIt is already declared by an existing source:\n\n{1}\n\n"
					"Two sources generating one asset take turns overwriting each other. Edit that file instead."),
				FText::FromString(PackagePath), FText::FromString(ConflictFile));

			if (bSkipConfirmation)
			{
				NotifyWithFile(Refusal, /*bSuccess=*/false, ConflictFile);
			}
			else
			{
				FMessageDialog::Open(EAppMsgType::Ok, Refusal);
			}
			return;
		}

		// --- 4. confirm ------------------------------------------------------------------------
		const FText Confirmation = FText::Format(
			LOCTEXT("AdoptConfirm",
				"Adopt '{0}' as a DreamFX source?\n\n"
				"Write source file:\n  {1}\n\n"
				"Rebuild asset from it:\n  {2}\n\n"
				"After this the text is the source of truth: hand edits to the asset are overwritten by the next build."),
			FText::FromString(System->GetName()),
			FText::FromString(SourcePath),
			FText::FromString(PackagePath));

		if (!bSkipConfirmation && FMessageDialog::Open(EAppMsgType::YesNo, Confirmation) != EAppReturnType::Yes)
		{
			return;
		}

		// --- 5. write ---------------------------------------------------------------------------
		if (!FFileHelper::SaveStringToFile(Export.Source, *SourcePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogDreamFX, Error, TEXT("Could not write '%s'."), *SourcePath);
			Notify(FText::Format(LOCTEXT("AdoptWriteFailed", "DreamFX could not write '{0}'."),
				FText::FromString(SourcePath)), /*bSuccess=*/false);
			return;
		}

		// A brand new root directory means the watcher is not watching it yet.
		FDreamFXPaths::InvalidateSourceRoots();

		// --- 6. rebuild the asset from the text it just wrote -----------------------------------
		FGenerateOptions Options;
		Options.bSave = true;
		Options.bForce = true;

		FDiagnosticSink BuildDiagnostics;
		const FGenerateResult Rebuild = FGenerator::GenerateFromFile(SourcePath, Options, BuildDiagnostics);
		LogDiagnostics(BuildDiagnostics);

		if (!Rebuild.bSucceeded || Rebuild.System == nullptr)
		{
			NotifyWithFile(FText::Format(
				LOCTEXT("AdoptRebuildFailed", "Adopt failed: '{0}' does not rebuild from its own export. See the Output Log."),
				FText::FromString(System->GetName())), /*bSuccess=*/false, SourcePath);
			return;
		}

		// --- 7. the fixed-point check -----------------------------------------------------------
		FDiagnosticSink SecondDiagnostics;
		const FDecompileResult SecondExport = FDecompiler::Decompile(Rebuild.System, Root.RootToken, SecondDiagnostics);
		LogDiagnostics(SecondDiagnostics);

		if (!SecondExport.bSucceeded || SecondExport.Source != Export.Source)
		{
			const FString Diff = SecondExport.bSucceeded
				? DiffFirstLine(Export.Source, SecondExport.Source)
				: FString(TEXT("the rebuilt asset could not be decompiled"));

			{
				FDiagnosticSink Diagnostics;
				Diagnostics.SetFile(SourcePath);
				Diagnostics.Error(TEXT("DFX8012"), FSourceLocation(), FString::Printf(
					TEXT("'%s' was adopted, but re-exporting the rebuilt asset does not reproduce this file. First difference at %s"),
					*PackagePath, *Diff));
				LogDiagnostics(Diagnostics);
			}

			NotifyWithFile(FText::Format(
				LOCTEXT("AdoptNotIdempotent",
					"Adopted '{0}', but the rebuilt asset does not re-export to the same text. The source file is now authoritative; see the Output Log for the first difference."),
				FText::FromString(System->GetName())), /*bSuccess=*/false, SourcePath);
			return;
		}

		UE_LOG(LogDreamFX, Display, TEXT("Adopted %s as %s."), *PackagePath, *SourcePath);
		NotifyWithFile(FText::Format(
			LOCTEXT("AdoptOk", "Adopted '{0}'. Its source is now {1}"),
			FText::FromString(System->GetName()), FText::FromString(SourcePath)), /*bSuccess=*/true, SourcePath);

		FDreamFXLaunchUtils::LaunchTextFileInPreferredEditor(SourcePath);
	}

	void FDreamFXCommands::OpenSource(UObject* Asset)
	{
		FProvenanceStamp Stamp;
		if (!RequireProvenance(Asset, Stamp))
		{
			return;
		}

		if (!FDreamFXLaunchUtils::LaunchTextFileInPreferredEditor(Stamp.SourceFullPath))
		{
			Notify(FText::Format(LOCTEXT("OpenSourceFailed", "DreamFX could not open '{0}'."),
				FText::FromString(Stamp.SourceFullPath)), /*bSuccess=*/false);
		}
	}

	void FDreamFXCommands::RebuildFromSource(UObject* Asset)
	{
		FProvenanceStamp Stamp;
		if (!RequireProvenance(Asset, Stamp))
		{
			return;
		}

		FSourceWatcher::QueueFile(Stamp.SourceFullPath, /*bAnnounceSuccess=*/true);
		UE_LOG(LogDreamFX, Display, TEXT("Queued '%s' for rebuild."), *Stamp.SourceFullPath);
	}

	void FDreamFXCommands::VerifyAsset(UObject* Asset)
	{
		FProvenanceStamp Stamp;
		if (!RequireProvenance(Asset, Stamp))
		{
			return;
		}

		FGenerateOptions Options;
		Options.bVerifyOnly = true;
		Options.bSave = false;
		Options.bForce = true;

		FDiagnosticSink Diagnostics;
		const FGenerateResult Result = FGenerator::GenerateFromFile(Stamp.SourceFullPath, Options, Diagnostics);
		LogDiagnostics(Diagnostics);

		if (Result.bSucceeded && !Result.bDrifted)
		{
			Notify(FText::Format(LOCTEXT("VerifyAssetOk", "'{0}' is in step with {1}."),
				FText::FromString(Asset->GetName()),
				FText::FromString(FPaths::GetCleanFilename(Stamp.SourceFullPath))), /*bSuccess=*/true);
			return;
		}

		NotifyWithFile(FText::Format(
			LOCTEXT("VerifyAssetDrift", "'{0}' has drifted from {1}. See the Output Log."),
			FText::FromString(Asset->GetName()),
			FText::FromString(FPaths::GetCleanFilename(Stamp.SourceFullPath))),
			/*bSuccess=*/false, Stamp.SourceFullPath);
	}
}

#undef LOCTEXT_NAMESPACE
