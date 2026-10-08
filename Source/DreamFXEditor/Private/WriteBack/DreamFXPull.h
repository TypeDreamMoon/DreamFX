#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"

class UNiagaraSystem;

namespace UE::DreamFX::Editor
{
	/** How one pull run is allowed to behave. */
	struct FPullOptions
	{
		/**
		 * Write the values into the text. Off by default, and the default is the point: a pull run
		 * prints the lines it would touch and a reader decides. `-Apply` is the only way to a byte.
		 */
		bool bApply = false;

		/**
		 * Read the values from this asset instead of the one the text names.
		 *
		 * Needed because an export cannot name the asset it came from: plan-v4 V1 writes every export
		 * into the `Decompiled/` namespace precisely so that rebuilding one cannot reach the original,
		 * which also means "pull the original's values into this export" has no other spelling. The
		 * text's own `Name=` still decides what a build writes; this decides only what pull reads.
		 */
		FString AssetOverride;

	};
	/** What one pull run did, for the summary the caller prints. */
	struct FPullResult
	{
		bool bSucceeded = false;
		FString FilePath;
		FString AssetPath;

		/** Inputs the text declares that the asset also stores a value for: the comparable set. */
		int32 Compared = 0;
		/** Of those, the ones whose literal differs and is about to be -- or was -- rewritten. */
		int32 Changed = 0;
		/** Values the asset stores for inputs this text does not declare. Never written. */
		int32 Undeclared = 0;
		/** Declared inputs pull could not write back, for a reason it reported one line each about. */
		int32 Unwritable = 0;
		/** Stacks the text and the asset describe differently, so nothing in them was addressed. */
		int32 Unaddressable = 0;

		bool bWroteFile = false;
		FString BackupPath;
		FString ReportPath;

		/** How many edits of every kind the run produced; the splice count, not the report's. */
		int32 Edits = 0;

		/** The messages the run produced, in order; the caller writes them beside the diagnostics. */
		TArray<FString> Report;
	};

	/**
	 * Asset values back into the text (write-back ②; Docs/tools/pull.md).
	 *
	 * The other direction from `build`, and the missing half of "author the structure in text, tune
	 * the numbers in the editor": a value that was tuned in the editor lives in the asset's
	 * rapid-iteration store and nowhere else, so the next rebuild -- which writes the text's literals
	 * over it -- loses it. Pulling it back makes the text carry it, which is the only thing that makes
	 * the text the whole truth.
	 *
	 * What it may touch is deliberately narrow, and every rule is a refusal rather than a guess:
	 *
	 *   * the three things a text can already say about an asset -- a module call's arguments, a
	 *     settings block, a renderer's properties and bindings -- and nothing else. A value the text
	 *     does not declare is reported and skipped (`DFX7106`), and structure is never added or
	 *     removed: a module the editor has and the text does not is named, and nothing is written;
	 *   * only where the address is exact: the asset's own node name, matched to the text's statement
	 *     at the same position in the same stack, with the module asset and the node name verified
	 *     first. Any disagreement and the whole stack is refused, because rewriting a value through a
	 *     correspondence that might be off by one is worse than not rewriting it;
	 *   * only a literal. A link, a dynamic input, an hlsl block or a curve is a different value MODE,
	 *     and changing a mode is an edit, not a write-back;
	 *   * only the value's own characters. The replacement is spliced over the byte range the parser
	 *     recorded for that literal, so indentation, trailing comments and line endings survive.
	 *
	 * The value read is the asset's rapid-iteration store -- the same bytes `asset-diff` and the build
	 * safety gate compare -- and not the resolved pin. That distinction is the whole reason this exists:
	 * `NS_Effects1_Mesh` stores `Sprite_Atlas_Size.MeshYaw` = -90 while every read of the pin returns
	 * 0.0 (write-back-coverage.md 3.5), so a pull that asked the pin would find nothing to write.
	 *
	 * A `Settings` key and a renderer property are read through the SAME JSON the plan side writes
	 * (`GetSystemProperties` / `GetEmitterProperties` / `GetRendererProperties`, the inverse of
	 * `SetSystemProperties` and friends), and spelled by the one value renderer the decompiler uses as
	 * well (`WriteBack/DreamFXSourceValue.h`). Reading the property is what keeps the two directions
	 * from disagreeing about what a setting is; there is no second table.
	 */
	class FPuller
	{
	public:
		/**
		 * Reads the values for one source file and, with `-Apply`, writes them into it.
		 *
		 * Never writes an asset: the system is loaded, read and left as it was.
		 */
		static FPullResult PullFile(const FString& FilePath, const FPullOptions& Options,
			FDiagnosticSink& Diagnostics);

		/**
		 * The same write-back over text that is already in memory, which is what makes it testable
		 * without a file on disk and what lets a caller drive it against a system it built itself.
		 *
		 * `SystemOverride` null means "the one the text names, resolved the way a build resolves it".
		 *
		 * @param OutNewText  the text with every edit applied. Equal to the input, byte for byte, when
		 *                    there was nothing to do -- which is the invariant the caller checks.
		 * @return false only when the run could not proceed at all; diagnostics say why.
		 */
		static bool PullText(const FString& SourceText, const FString& FilePath,
			UNiagaraSystem* SystemOverride, const FPullOptions& Options,
			FDiagnosticSink& Diagnostics, FPullResult& OutResult, FString& OutNewText);
	};
}
