#pragma once

#include "CoreMinimal.h"

class UNiagaraEmitter;
class UNiagaraSystem;
class UObject;

namespace UE::DreamFX::Editor
{
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

		/** Rewrites `DFX/DreamFX.code-workspace` and opens it. */
		static void OpenWorkspace();

		/**
		 * Decompiles to `<DecompiledOutputDirectory>/<package path>/<asset>.dfs` and opens it.
		 *
		 * A copy for reading and editing, not a source root: nothing watches it and nothing claims the
		 * asset. Gaps become a warning toast; they are in the file either way (E4-0).
		 */
		static void ExportSystem(UNiagaraSystem* System);

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

		/** True when the asset carries a provenance stamp -- the discriminator for the two-state menu. */
		static bool HasProvenance(const UObject* Asset);
	};
}
