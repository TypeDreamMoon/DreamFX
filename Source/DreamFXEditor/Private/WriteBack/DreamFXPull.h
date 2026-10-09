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

		/**
		 * Write the structural differences a stack shows, when they can be located without ambiguity
		 * (write-back ③; `Docs/tools/pull.md`).
		 *
		 * Off by default, and off means REPORT: a module the editor has and the text does not is named,
		 * with the line that would be inserted, and nothing is written. Structure is the one thing a
		 * text file cannot be wrong about quietly -- a misplaced line moves a module in the execution
		 * order -- so the switch is explicit and the default is the refusal.
		 */
		bool bStructure = false;

		/**
		 * Compare against the baseline the last `-Apply` recorded (write-back ③'s dirty set).
		 *
		 * On by default. A value the text disagrees with but the asset has not changed since that last
		 * apply is a value the TEXT moved, so the text is right and pull has nothing to say about it.
		 * With no baseline to read there is nothing to compare against and pull falls back to comparing
		 * every declared value, which it announces (DFX7114).
		 */
		bool bUseBaseline = true;
	};

	/**
	 * What the asset held the last time a pull applied, so this one can tell what changed since.
	 *
	 * A recorded baseline rather than `UPackage::IsDirty` or an editor event, and the reason is what
	 * each of them can answer. The package's dirty flag is one bit for the whole asset: it is set by
	 * loading in some paths, it cannot say WHICH value moved, and it says nothing at all to a
	 * commandlet that has just opened the asset in a fresh process -- so it cannot decide "write this
	 * line and not that one", which is the entire requirement. An editor event carries the same
	 * problem plus a lifetime that does not survive the process. A baseline is a file: it can be read,
	 * diffed and argued with, and "why was this line written?" has an answer that is still there
	 * tomorrow.
	 *
	 * `Records` is addressed exactly the way the diagnostics address a value -- `scope` + `key`, the
	 * same two halves the build safety gate files a fact under -- and holds the value's SOURCE
	 * SPELLING, not its bytes, so the record can be read by a human and compared against what pull
	 * would write.
	 */
	struct FPullBaseline
	{
		bool bLoaded = false;
		FString Path;
		TMap<FString, FString> Records;

		/** Look up one recorded value. False when the baseline says nothing about this address. */
		bool Find(const FString& Scope, const FString& Key, FString& OutValue) const;
		void Set(const FString& Scope, const FString& Key, const FString& Value);

		/**
		 * True when this address is not something the last apply saw, or has moved since.
		 *
		 * With no baseline loaded everything is dirty, which is the documented degradation: the first
		 * run after this feature arrives compares every declared value exactly as it did before.
		 */
		bool IsDirty(const FString& Scope, const FString& Key, const FString& Now) const;

		/** Reads a baseline file. A missing file is not an error -- it is the first run. */
		bool Load(const FString& InPath, FString& OutWhy);
		/** Writes the baseline, sorted, so two runs of the same asset produce the same file. */
		bool Save(FString& OutWhy) const;
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

		/** Values that differ but that the asset has not moved since the baseline: the text's, then. */
		int32 Withheld = 0;

		/** Statements the structural pass added to the text (`-Structure`). */
		int32 Added = 0;
		/** Statements it removed from the text. */
		int32 Removed = 0;
		/** Stacks whose structural difference was refused: no unique correspondence, or no switch. */
		int32 StructureRefused = 0;

		bool bWroteFile = false;
		FString BackupPath;
		FString ReportPath;

		/** True when a baseline was read and used to decide which values to write. */
		bool bBaselineUsed = false;
		FString BaselinePath;
		bool bBaselineWritten = false;

		/** How many edits of every kind the run produced; the splice count, not the report's. */
		int32 Edits = 0;

		/**
		 * Every address this run looked at and what the asset holds for it.
		 *
		 * Handed back so the caller can make it the next baseline, and kept in the result rather than
		 * written from here because "leave a record on disk" is the caller's decision -- a dry run must
		 * not leave one, or the next run would treat the very differences it just reported as dealt
		 * with.
		 */
		TMap<FString, FString> ObservedRecords;

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
	 *     does not declare is reported and skipped (`DFX7106`); structure is added or removed only
	 *     under `-Structure`, and only at a correspondence that is provably unique;
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
		 * `Baseline` is read and, on a successful apply, refilled with what the asset holds now; it is
		 * NOT written to disk here, because whether a run leaves a record is the caller's decision.
		 *
		 * @param OutNewText  the text with every edit applied. Equal to the input, byte for byte, when
		 *                    there was nothing to do -- which is the invariant the caller checks.
		 * @return false only when the run could not proceed at all; diagnostics say why.
		 */
		static bool PullText(const FString& SourceText, const FString& FilePath,
			UNiagaraSystem* SystemOverride, const FPullOptions& Options, FPullBaseline& Baseline,
			FDiagnosticSink& Diagnostics, FPullResult& OutResult, FString& OutNewText);
	};
}
