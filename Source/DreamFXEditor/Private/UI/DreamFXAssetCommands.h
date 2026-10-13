#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"
#include "WriteBack/DreamFXPull.h"

class UNiagaraEmitter;
class UNiagaraSystem;
class UObject;

namespace UE::DreamFX::Editor
{
	/** Completed export, shared by the UI and bridge without launching an editor or showing a toast. */
	struct FAssetExportResult
	{
		bool bSucceeded = false;
		FString OutputPath;
		/** A refused mirror export: the source that already builds the asset, when it is on disk. */
		FString ExistingSourcePath;
		FString Message;
		TArray<FString> UnsupportedFeatures;
		FDiagnosticSink Diagnostics;
	};

	/** What a write-back into an asset's source did, before any toast: shared by the menu, the save hook and tests. */
	struct FPullIntoSourceResult
	{
		/** The asset had a .dfs to write into and the pull ran. False means `Message` says why it did not. */
		bool bAttempted = false;
		bool bSucceeded = false;
		FString SourceFile;
		FString Message;
		FPullResult Pull;
		FDiagnosticSink Diagnostics;
	};

	struct FVerifyBatchResult
	{
		int32 Checked = 0;
		int32 Drifted = 0;
		int32 Failed = 0;
		FDiagnosticSink Diagnostics;

		bool IsSuccessful() const { return Drifted == 0 && Failed == 0; }
	};

	/**
	 * What every DreamFX menu entry actually does.
	 *
	 * One body per command, shared by the Tools menu, the Level Editor toolbar, the Content Browser
	 * context menu and the Niagara system editor toolbar (plan-v3 principle 2). The menus decide
	 * *when* an entry appears; nothing about *what* it does lives in a menu file, so a command cannot
	 * drift between the four places it is offered from.
	 */
	struct FDreamFXCommands
	{
		/** Queues every source through the watcher's build queue -- equivalent to saving them all. */
		static void RebuildAll();

		/** Verifies every source against its generated asset. Writes nothing. */
		static void VerifyAll();

		/** Shared synchronous verification for menus and the bridge; never queues a build. */
		static FVerifyBatchResult VerifyAllSources();
		static FVerifyBatchResult VerifySources(const TArray<FString>& SourceFiles);

		/** Rewrites `DFX/DreamFX.code-workspace` and opens it. */
		static void OpenWorkspace();

		/**
		 * Decompiles to `<DecompiledOutputDirectory>/<package path>/<asset>.dfs` and opens it.
		 *
		 * The exported source builds a separate asset in the Decompiled/ namespace. Gaps become a
		 * warning toast and are listed in the source header.
		 */
		static void ExportSystem(UNiagaraSystem* System);
		static FAssetExportResult ExportAsset(UObject* Asset);

		/** Checks file existence before parsing: an invalid source is still the author's file. */
		static bool ValidateAdoptDestination(const FString& PackagePath, const FString& SourcePath,
			FDiagnosticSink& Diagnostics);
		/** Publishes a new source without replacing a concurrently created destination. */
		static bool WriteNewAdoptSource(const FString& SourcePath, const FString& Source, FString& OutError);

		/**
		 * Export's strict form: decompile into the real source root, rebuild the asset from it, then
		 * re-decompile and compare byte for byte.
		 *
		 * Refuses outright when anything about the asset cannot be expressed, because adopting means
		 * the text becomes the only source of truth -- adopting with a known gap silently destroys the
		 * part that did not survive.
		 *
		 * @param bSkipConfirmation  suppresses the modal confirmation, for scripted use. The refusals
		 *                           are not suppressed: those are correctness gates, not prompts.
		 */
		static void AdoptSystem(UNiagaraSystem* System, bool bSkipConfirmation = false);

		/**
		 * Decompiles a standalone emitter asset to a `.dfe` and opens it.
		 *
		 * Same output directory and same gap reporting as Export .dfs. There is no Adopt counterpart:
		 * a .dfe generates no asset of its own -- it is copied into whichever system references it --
		 * so there is nothing for a provenance stamp to point at.
		 */
		static void ExportEmitter(UNiagaraEmitter* Emitter);

		/** Opens the source file recorded in the asset's provenance stamp. */
		static void OpenSource(UObject* Asset);

		/** Queues just this asset's source file. */
		static void RebuildFromSource(UObject* Asset);

		/** Verifies just this asset against its source. */
		static void VerifyAsset(UObject* Asset);

		/**
		 * Writes the values the asset holds back into the literals of its .dfs (`dfx pull -Apply`).
		 *
		 * Comments, indentation and line endings stay; only the byte range of each literal is replaced.
		 * `bStructure` is the setting of the same name -- whether modules the editor added or removed
		 * are added to or removed from the text too -- and is passed in rather than read here so a
		 * caller (a test, the bridge) states what it wants.
		 *
		 * No toast, no editor launch, no watcher interaction beyond telling it the file was written by
		 * us (FSourceWatcher::IgnoreOwnWrite). Only a system has a .dfs; everything else comes back
		 * with `bAttempted` false.
		 */
		static FPullIntoSourceResult PullAssetIntoSource(UObject* Asset, bool bStructure);

		/**
		 * The menu command: PullAssetIntoSource with the settings, then a toast and the log.
		 *
		 * @param bQuietWhenUnchanged  say nothing when the text already held every value and nothing went
		 *                             wrong. The save hook passes true, since a toast on every save of an
		 *                             asset nobody tuned would be noise; the menu passes false, since a
		 *                             command with no visible reaction reads as broken.
		 */
		static void PullIntoSource(UObject* Asset, bool bQuietWhenUnchanged = false);

		/** True when the asset carries a provenance stamp -- the discriminator for the two-state menu. */
		static bool HasProvenance(const UObject* Asset);
	};
}
