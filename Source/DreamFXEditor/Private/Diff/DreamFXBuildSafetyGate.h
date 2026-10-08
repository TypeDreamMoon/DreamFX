#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"

class UNiagaraSystem;

namespace UE::DreamFX::Editor
{
	/**
	 * What one asset held when a build opened it.
	 *
	 * Held as TEXT rather than as a system pointer on purpose: the build mutates the very object these
	 * facts describe, so a snapshot that read through to the asset later would answer with the
	 * rebuild's own state and find nothing missing -- which is exactly how the loss this exists to
	 * catch stayed silent for so long.
	 */
	struct FBuildSafetySnapshot
	{
		TArray<FString> Facts;

		/**
		 * False when there was no asset to read. A first generation has nothing to lose: the gate is
		 * about a rebuild REPLACING content the source cannot express, and the first build of a path
		 * replaces nothing. Refusing it would only stop the one case that is safe by construction.
		 */
		bool bCaptured = false;
	};

	/**
	 * What the source declared, addressed exactly the way the facts are.
	 *
	 * Every entry is one `scope` + `subject` pair, the same two halves the gate splits a fact into: the
	 * structure a fact lives under (an emitter, a module node, a renderer) and the property or input
	 * inside it. Building it this way is what makes "did the text name this?" a lookup against the
	 * parse instead of a guess about names -- and it has to be filled where the names exist: a module's
	 * NODE name is assigned by the engine when the module is added (and `as <name>` may rename it
	 * afterwards), so nothing downstream of the build can recover it.
	 */
	struct FDeclaredFacts
	{
		void Add(const FString& Scope, const FString& Subject);
		bool Names(const FString& Scope, const FString& Subject) const;
		/** True when the source declared anything under this scope, whatever it was. */
		bool HasScope(const FString& Scope) const;
		int32 Num() const { return Pairs.Num(); }

	private:
		TSet<FString> Pairs;
		TSet<FString> Scopes;
	};

	/**
	 * The gate between a rebuild and the facts it would drop (write-back ①; write-back-coverage.md 6.1).
	 *
	 * `FGenerator` builds by replaying text onto an asset, and text is not a complete description of a
	 * Niagara system. A module input the decompiler suppressed (R8: only inputs that differ from a
	 * pristine module are printed), a renderer binding a commandlet patched in after the last build, a
	 * property no setting table carries -- none of them are in the source, and a rebuild silently
	 * replaces whatever they held. Three measured cases: 41 stored module-input constants on one asset,
	 * `MeshYaw` -90 -> 0 on another, 61 on a third.
	 *
	 * What it refuses is narrower than "anything that changed", because a gate that fires on ordinary
	 * authoring is a gate its users learn to bypass (measured: writing `UniformScale = 2.0` over the
	 * 1.0 the asset held blocked a build that was doing exactly what its text said). So it compares one
	 * structure at a time and asks the source before it accuses:
	 *
	 *   * a structure only one side has was added or removed BY the text -- intentional, ignored;
	 *   * inside a structure both sides have, a fact that DISAPPEARED is drift: the text has no way to
	 *     name it, so a save would destroy it (this is `MaterialParameters` losing its bindings);
	 *   * inside it, a fact whose VALUE changed is drift only when the source does not name that
	 *     subject -- the module call's argument list, the Settings block, the renderer's properties.
	 *     A value the text writes is a value the text meant, and it is allowed through;
	 *   * when the declaration cannot be consulted at all, it refuses too and says so (`suspected drift
	 *     (cannot tell)`), which is the conservative half of that rule.
	 *
	 * And one exception to the second rule, which is about where a value lives rather than whether it
	 * survived: a constant can sit in two scripts' rapid-iteration stores at once (an emitter update
	 * module's constant lives in the system update script, and the editor's own `SetInput` -- the
	 * slider -- also writes a copy into the system spawn script), while a rebuild materialises one.
	 * The copy that goes is then a copy and nothing else: the same value, at the same
	 * `<emitter>.<node>.<input>` address, is still stored by the script that stayed. That is reported
	 * (verbose, one line per copy), never refused -- and only when the value really is still there:
	 * two copies that DISAGREE, or a value that went with nothing left holding it, are judged by the
	 * rules above exactly as before.
	 */
	class FBuildSafetyGate
	{
	public:
		/** Reads the asset's facts. Call before the build's first write to it. */
		static FBuildSafetySnapshot Capture(UNiagaraSystem* System);

		/**
		 * One fact the rebuild no longer carries exactly, and what was decided about it.
		 *
		 * Kept separate from the report because the decision is the part worth asserting: the gate
		 * writes files, refuses saves and prints console lines, none of which a test wants to drive.
		 */
		struct FVerdict
		{
			/** The fact as the asset held it; one entry per copy that went. */
			FString Fact;
			/** What the report prints: the address, without the value. */
			FString Display;
			FString OldValue;
			FString NewValue;

			/**
			 * How the decision was reached, because the reader is the one who decides whether to reach
			 * for -Force: None -- the fact vanished, or a copy of it did, and no rule could have
			 * allowed it; Deterministic -- the source's declarations were consulted and do not name
			 * it; Suspected -- there is no declaration record for that structure at all.
			 */
			enum class EKind : uint8 { None, Deterministic, Suspected };
			EKind Kind = EKind::None;

			/**
			 * True when this copy went and the rebuild still stores its exact value at the same
			 * address in another script: a collapse of two identical copies into one, not a loss. Such
			 * a verdict never refuses a save, and the report names it (`merged |`) rather than
			 * dropping it silently.
			 */
			bool bCollapsedIntoAnotherStore = false;
		};

		/** What one comparison concluded, over the two fact sets the caller hands in. */
		struct FComparison
		{
			TArray<FVerdict> Verdicts;
			/** Comparable facts on each side after canonicalisation: what the report counts. */
			int32 BeforeCount = 0;
			int32 AfterCount = 0;
			/** Leftover facts inside a structure the rebuild kept. */
			int32 Candidates = 0;
			/** Facts the rebuild added. */
			int32 Gained = 0;

			/** The verdicts a save would be refused over. */
			int32 NumRefused() const
			{
				int32 Count = 0;
				for (const FVerdict& Verdict : Verdicts)
				{
					Count += Verdict.bCollapsedIntoAnotherStore ? 0 : 1;
				}
				return Count;
			}

			/** The verdicts that went but left their value in another script's store. */
			int32 NumCollapsed() const
			{
				int32 Count = 0;
				for (const FVerdict& Verdict : Verdicts)
				{
					Count += Verdict.bCollapsedIntoAnotherStore ? 1 : 0;
				}
				return Count;
			}
		};

		/**
		 * The four rules, applied to two fact sets -- no asset, no sink, no file.
		 *
		 * This is what `CheckBeforeSave` runs on a live asset, and what the corpus asserts on
		 * synthetic pairs (DreamFX.Corpus.WriteBack's `CollapsedCopy`): the rules are the part with a
		 * cost to getting wrong, so they are reachable without a Niagara system to hold them.
		 */
		static FComparison Compare(const TArray<FString>& BeforeFacts, const TArray<FString>& AfterFacts,
			const FDeclaredFacts& Declared);

		/**
		 * Compares the snapshot against the asset as it stands now -- after the build, after the
		 * compile, immediately before the save -- and reports every fact that is about to be lost.
		 *
		 * @param bForceFromCommandLine  `-Force`: report the losses and write anyway, instead of
		 *                               refusing. Overriding a refusal is allowed because the losses
		 *                               are still logged at warning severity with the same detail, so
		 *                               the run is accountable afterwards either way.
		 * @return true when the caller may save. False means nothing was written, so the asset still
		 *         holds every fact the diagnostics name.
		 */
		static bool CheckBeforeSave(UNiagaraSystem* System, const FBuildSafetySnapshot& Before,
			const FDeclaredFacts& Declared, bool bForceFromCommandLine, const FString& AssetPath,
			const FSourceLocation& Location, FDiagnosticSink& Diagnostics);
	};
}
