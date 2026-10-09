#include "Diff/DreamFXBuildSafetyGate.h"

#include "Diff/DreamFXAssetFacts.h"
#include "DreamFXModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Separates the two halves of a declaration key; no fact text can contain it. */
		const TCHAR* const KeySeparator = TEXT("\x1f");

		FString MakeKey(const FString& Scope, const FString& Subject)
		{
			return Scope + KeySeparator + Subject;
		}

		/**
		 * Removes one `Field=<bool>` from a struct export, comma included, so the parentheses stay
		 * balanced whether the field sat first, in the middle or last.
		 */
		void RemoveFlagField(FString& Text, const TCHAR* Field)
		{
			for (const TCHAR* Value : { TEXT("True"), TEXT("False") })
			{
				Text.ReplaceInline(*FString::Printf(TEXT(",%s=%s"), Field, Value), TEXT(""));
				Text.ReplaceInline(*FString::Printf(TEXT("(%s=%s,"), Field, Value), TEXT("("));
			}
		}

		/**
		 * The fields of a Niagara binding struct that are CACHES, not state, removed before two fact
		 * sets are compared.
		 *
		 * `AppendPropertyFacts` walks a renderer's bindings as a raw struct, so their exported text
		 * carries the exists-on-source and is-cached flags. Both are recomputed from the binding's
		 * root name and the compiled attribute set -- the same two the stage walk in
		 * DreamFXAssetFacts.cpp already skips -- and the measured case is `bBindingExistsOnSource`
		 * True -> False on one binding of one renderer (write-back-coverage.md 3.10), a difference no
		 * reader can act on.
		 *
		 * Narrow on purpose, and applied only here. `asset-diff` is a reporting tool, where a
		 * documented noisy line is tolerable and where its output is quoted as evidence, so its walk
		 * is left as it was. This gate is a REFUSAL: one that fires on a rebuild which lost nothing is
		 * a gate its users learn to bypass. Everything that carries a name or a serialized value stays
		 * in the comparison, `MaterialParamValidMask` included -- the tell for the accident this gate
		 * most needs to catch, a renderer whose material bindings were cleared.
		 */
		FString Canonicalize(const FString& Fact)
		{
			FString Out = Fact;
			RemoveFlagField(Out, TEXT("bBindingExistsOnSource"));
			RemoveFlagField(Out, TEXT("bIsCachedParticleValue"));
			return Out;
		}

		/**
		 * True for a `compiled ... writes:` fact whose list is empty.
		 *
		 * `AppendCompiledFacts` always emits that line, empty list included, so an asset whose cached VM
		 * PostLoad discarded reports `writes: ` and nothing else from that family -- a line that says
		 * only "no VM was loaded in this session". A script that genuinely writes nothing emits the
		 * same empty line on both sides, where the multiset cancels it on its own.
		 */
		bool IsEmptyWritesFact(const FString& Fact)
		{
			// " writes: " is nine characters; nothing after them means an empty list.
			const int32 At = Fact.Find(TEXT(" writes: "));
			return At != INDEX_NONE && At + 9 >= Fact.Len();
		}

		/** The facts that take part in a comparison: canonicalized, minus the placeholders above. */
		void CollectComparableFacts(const TArray<FString>& Raw, TArray<FString>& OutFacts)
		{
			OutFacts.Reserve(Raw.Num());
			for (const FString& Fact : Raw)
			{
				if (!IsEmptyWritesFact(Fact))
				{
					OutFacts.Add(Canonicalize(Fact));
				}
			}
		}

		/** Caps one fragment for the console; the lists written beside it hold the full text. */
		FString Truncate(const FString& Text, int32 Limit)
		{
			return Text.Len() <= Limit ? Text : Text.Left(Limit) + TEXT("...");
		}

		/** Splits `<Subject> = <Value>`; with no ` = ` the whole text is the subject. */
		void SplitSubjectValue(const FString& Text, FString& OutSubject, FString& OutValue)
		{
			const int32 At = Text.Find(TEXT(" = "));
			if (At == INDEX_NONE)
			{
				OutSubject = Text;
				OutValue.Reset();
			}
			else
			{
				OutSubject = Text.Left(At);
				OutValue = Text.Mid(At + 3);
			}
		}

		/**
		 * One fact, expressed as the structure it lives under and the property inside it.
		 *
		 * Scope and Subject are the key the source's declarations are filed under. Display is what the
		 * report prints, which keeps the parts the key drops (the script a value is stored in, the
		 * input's type) because a reader chasing one needs them.
		 */
		struct FAddressedFact
		{
			FString Scope;
			FString Subject;
			FString Value;
			FString Display;
		};

		/**
		 * Splits a fact into (structure, subject, value) so it can be compared one structure at a time.
		 *
		 * The families whose facts carry no address a declaration could be filed under -- a data
		 * interface subobject, the compiler's own view of a script, a `user` store entry, an event
		 * handler, a simulation stage, an emitter that has no data -- are SELF-SCOPED: the fact is its
		 * own structure. Rule 1 then treats a difference in them the way it treats an added or removed
		 * module: a structure only one side has was added or removed by the text, so it is intentional
		 * and not drift. That is a deliberate limit, not an oversight: a `di` or `compiled` fact is an
		 * instance with no name in the fact text to file a declaration under, and a stage's inner
		 * properties have no one-to-one spelling in the `Stage(...)` block.
		 */
		FAddressedFact AddressFact(const FString& Fact)
		{
			FAddressedFact Out;
			const int32 Assign = Fact.Find(TEXT(" = "));
			Out.Display = Assign == INDEX_NONE ? Fact : Fact.Left(Assign);

			if (Fact.StartsWith(TEXT("di ")) || Fact.StartsWith(TEXT("compiled "))
				|| Fact.StartsWith(TEXT("user "))
				|| Fact.Contains(TEXT(" event handler: "))
				|| Fact.Contains(TEXT(" simulation stage "))
				|| Fact.EndsWith(TEXT(" has no data")))
			{
				Out.Scope = Fact;
				return Out;
			}

			// `system <Prop> = <Value>`
			if (Fact.StartsWith(TEXT("system ")))
			{
				Out.Scope = TEXT("system");
				SplitSubjectValue(Fact.Mid(7), Out.Subject, Out.Value);
				return Out;
			}

			// `ri <emitter> <usage> Constants.<emitter>.<node>.<input> (<type>) = <value>`, and the
			// system-scope spelling `ri system-update Constants.<node>.<input> (<type>) = <value>`.
			if (Fact.StartsWith(TEXT("ri ")))
			{
				const int32 AddressAt = Fact.Find(TEXT("Constants."));
				if (AddressAt != INDEX_NONE)
				{
					FString Head = Out.Display.Mid(AddressAt);
					const int32 Paren = Head.Find(TEXT(" ("), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					if (Paren != INDEX_NONE)
					{
						Head = Head.Left(Paren);
					}

					// The scope drops the usage label as well as the input: a module declared in an
					// emitter's EmitterUpdate stack stores its constants in the SYSTEM update script,
					// so the script named in the fact says where the value lives, not where the source
					// declared it -- filing declarations under it would never match.
					const int32 Dot = Head.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					Out.Scope = TEXT("ri ") + (Dot == INDEX_NONE ? Head : Head.Left(Dot));
					Out.Subject = Dot == INDEX_NONE ? FString() : Head.Mid(Dot + 1);
					Out.Value = Assign == INDEX_NONE ? FString() : Fact.Mid(Assign + 3);
					return Out;
				}
			}

			if (Fact.StartsWith(TEXT("emitter ")))
			{
				const int32 RendererAt = Fact.Find(TEXT(" renderer "));
				if (RendererAt != INDEX_NONE)
				{
					// `emitter <E> renderer <i>:<Class> <Prop> = <Value>`
					const FString Rest = Fact.Mid(RendererAt + 10);
					const int32 Space = Rest.Find(TEXT(" "));
					Out.Scope = Fact.Left(RendererAt + 10) + (Space == INDEX_NONE ? Rest : Rest.Left(Space));
					SplitSubjectValue(Space == INDEX_NONE ? FString() : Rest.Mid(Space + 1),
						Out.Subject, Out.Value);
					return Out;
				}

				// `emitter <E> <Prop> = <Value>` -- the emitter's own data: its settings, the enabled
				// flag the adapter reports, its parent link.
				const int32 Space = Fact.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, 8);
				Out.Scope = Space == INDEX_NONE ? Fact : Fact.Left(Space);
				SplitSubjectValue(Space == INDEX_NONE ? FString() : Fact.Mid(Space + 1),
					Out.Subject, Out.Value);
				return Out;
			}

			// No address this gate knows: self-scoped, so it can never be accused of drifting inside a
			// structure that stayed.
			Out.Scope = Fact;
			return Out;
		}
	}

	void FDeclaredFacts::Add(const FString& Scope, const FString& Subject)
	{
		Pairs.Add(MakeKey(Scope, Subject));
		Scopes.Add(Scope);
	}

	bool FDeclaredFacts::Names(const FString& Scope, const FString& Subject) const
	{
		return Pairs.Contains(MakeKey(Scope, Subject));
	}

	bool FDeclaredFacts::HasScope(const FString& Scope) const
	{
		return Scopes.Contains(Scope);
	}

	FBuildSafetySnapshot FBuildSafetyGate::Capture(UNiagaraSystem* System)
	{
		FBuildSafetySnapshot Snapshot;
		if (System == nullptr)
		{
			return Snapshot;
		}

		DescribeSystemFacts(System, Snapshot.Facts);
		Snapshot.bCaptured = true;
		return Snapshot;
	}

	FBuildSafetyGate::FComparison FBuildSafetyGate::Compare(const TArray<FString>& BeforeFactsRaw,
		const TArray<FString>& AfterFactsRaw, const FDeclaredFacts& Declared)
	{
		FComparison Out;

		TArray<FString> BeforeFacts;
		CollectComparableFacts(BeforeFactsRaw, BeforeFacts);

		TArray<FString> AfterFacts;
		CollectComparableFacts(AfterFactsRaw, AfterFacts);

		Out.BeforeCount = BeforeFacts.Num();
		Out.AfterCount = AfterFacts.Num();

		// The multiset difference of the two sets: a fact both sides carry the same number of times is
		// agreement however the facts are ordered, so what is left over on the before side is every
		// fact the rebuild no longer holds. The same comparison asset-diff makes, for the same reason.
		TMap<FString, int32> Counts;
		for (const FString& Fact : BeforeFacts)
		{
			++Counts.FindOrAdd(Fact);
		}
		for (const FString& Fact : AfterFacts)
		{
			--Counts.FindOrAdd(Fact);
		}

		// The after side indexed by structure, which is what turns "this fact is gone" into one of the
		// four answers the gate gives: the structure went with it (the text's doing), or the fact
		// vanished inside a structure that stayed, or its value changed and the source does -- or does
		// not -- name it.
		//
		// Three indexes, because the rules ask three different questions: which structures stayed
		// (address without the script label), what the address holds now (for "the value changed"),
		// and which address+value pairs the rebuild still carries at all (for the copy collapse).
		TSet<FString> AfterScopes;
		TMap<FString, FString> AfterByKey;
		TSet<FString> AfterValuesByKey;
		for (const FString& Fact : AfterFacts)
		{
			const FAddressedFact Addressed = AddressFact(Fact);
			const FString Key = MakeKey(Addressed.Scope, Addressed.Subject);
			AfterScopes.Add(Addressed.Scope);
			AfterByKey.Add(Key, Addressed.Value);
			AfterValuesByKey.Add(MakeKey(Key, Addressed.Value));
		}

		for (const TPair<FString, int32>& Entry : Counts)
		{
			// Zero is a fact both sides carry the same number of times: agreement, and the bulk of
			// any real asset. Only the positive side is a candidate, and the negative side is what
			// the rebuild added.
			if (Entry.Value <= 0)
			{
				Out.Gained += -Entry.Value;
				continue;
			}

			const FAddressedFact Addressed = AddressFact(Entry.Key);

			// Rule 1. A structure only one side has was added or removed by the text. Deleting a
			// module, a renderer or a whole emitter is an intentional edit, and the facts that went
			// with it are the edit, not a loss -- while a structure BOTH sides have is one the build
			// was asked to reproduce, and anything missing inside it is drift.
			if (!AfterScopes.Contains(Addressed.Scope))
			{
				continue;
			}

			++Out.Candidates;
			const FString Key = MakeKey(Addressed.Scope, Addressed.Subject);
			const FString* NewValue = AfterByKey.Find(Key);

			for (int32 Copy = 0; Copy < Entry.Value; ++Copy)
			{
				FVerdict Verdict;
				Verdict.Fact = Entry.Key;
				Verdict.Display = Addressed.Display;
				Verdict.OldValue = Addressed.Value;

				if (AfterValuesByKey.Contains(MakeKey(Key, Addressed.Value)))
				{
					// Rules 1 and 2 meet here, and this is the one place a copy that went is not a
					// loss. The same value is still stored at this same `<emitter>.<node>.<input>`
					// address, in a script whose label the address deliberately drops -- because the
					// label says where a value is STORED, not where the source declared it: an emitter
					// update module's constant lives in the system update script, and the editor's own
					// `SetInput` (the slider a person drags) also writes a copy into the system spawn
					// script. A rebuild materialises one of them, deterministically, every time.
					//
					// So what went is a duplicate placement of a value that is still there, which is a
					// representation collapsing rather than state disappearing: nothing a save would
					// destroy, and nothing the source would need a spelling for. Reported as a collapse
					// (verbose, one line per copy) and never refused.
					//
					// Narrow on purpose: the address AND the value both have to match. A copy that
					// disagrees with the one that stayed, or a value nothing holds any more, falls
					// through to the rules below and is judged exactly as it was before this existed.
					Verdict.bCollapsedIntoAnotherStore = true;
					Out.Verdicts.Add(MoveTemp(Verdict));
					continue;
				}

				if (NewValue == nullptr)
				{
					// Rule 2. The fact is gone from a structure that stayed: the text has no spelling
					// for it, so a save would destroy it.
					Verdict.NewValue = TEXT("(missing)");
				}
				else if (*NewValue == Addressed.Value)
				{
					// The same fact, one copy fewer, and the value index above did not find it: this
					// cannot happen while both indexes are built from one walk of the after side, and
					// it is kept because "a fact went and nothing holds its value" is precisely what
					// rule 2 refuses -- so if the two ever diverge, this is the branch that must say so
					// rather than let the fact through.
					Verdict.NewValue = TEXT("one copy fewer");
				}
				else
				{
					// Rule 3. The value changed, and the source decides: a subject the text names is a
					// value the text meant, so the rebuild is doing what it was told.
					Verdict.NewValue = *NewValue;
					if (Declared.Names(Addressed.Scope, Addressed.Subject))
					{
						continue;
					}

					// Rule 4. Refuse when it cannot be decided, but say which of the two this is:
					// "the text does not name this" is a decision the reader can act on, "cannot tell"
					// is one they have to investigate first.
					Verdict.Kind = Declared.HasScope(Addressed.Scope)
						? FVerdict::EKind::Deterministic
						: FVerdict::EKind::Suspected;
				}

				Out.Verdicts.Add(MoveTemp(Verdict));
			}
		}

		return Out;
	}

	bool FBuildSafetyGate::CheckBeforeSave(UNiagaraSystem* System, const FBuildSafetySnapshot& Before,
		const FDeclaredFacts& Declared, bool bForceFromCommandLine, const FString& AssetPath,
		const FSourceLocation& Location, FDiagnosticSink& Diagnostics)
	{
		if (!Before.bCaptured || System == nullptr)
		{
			return true;
		}

		TArray<FString> RawAfter;
		DescribeSystemFacts(System, RawAfter);

		const FComparison Comparison = Compare(Before.Facts, RawAfter, Declared);

		TArray<FVerdict> Reported;
		TArray<FString> ReportedFacts;
		TArray<FString> Collapsed;

		for (const FVerdict& Verdict : Comparison.Verdicts)
		{
			if (Verdict.bCollapsedIntoAnotherStore)
			{
				Collapsed.Add(Verdict.Fact);
				continue;
			}

			ReportedFacts.Add(Verdict.Fact);
			Reported.Add(Verdict);
		}

		// A collapse is not a refusal, so it is stated at verbose -- one line per copy, because the
		// count alone leaves a reader unable to tell which constant it was about, and a line is what
		// separates "the gate looked at this and allowed it" from "the gate never saw it".
		const auto ReportCollapsed = [&Collapsed]()
		{
			for (const FString& Fact : Collapsed)
			{
				UE_LOG(LogDreamFX, Verbose,
					TEXT("            merged | %s -> the same value is still stored by another script, so the copy that went is not drift."),
					*Truncate(Fact, 400));
			}
		};

		if (Reported.Num() == 0)
		{
			if (Collapsed.Num() > 0)
			{
				UE_LOG(LogDreamFX, Verbose,
					TEXT("build safety: '%s' still holds every one of its %d fact(s) after the rebuild; %d duplicate cop(y/ies) of a constant another script's store also carries collapsed into one."),
					*AssetPath, Comparison.BeforeCount, Collapsed.Num());
				ReportCollapsed();
			}
			else
			{
				// Verbose, because a clean rebuild is the common case -- but it is a line, so "was this
				// build checked?" has an answer in the log rather than an absence of evidence.
				UE_LOG(LogDreamFX, Verbose,
					TEXT("build safety: '%s' still holds all %d fact(s) after the rebuild."),
					*AssetPath, Comparison.BeforeCount);
			}
			return true;
		}

		// Both lists, not just the losses: after a -Force save the asset no longer holds what it held
		// before, so this is the only record of it, and a truncated console line is not a record.
		const FString DumpDir = FPaths::ProjectSavedDir() / TEXT("DreamFX/BuildSafety");
		const FString BaseName = FPackageName::GetShortName(AssetPath);
		IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/true);
		TArray<FString> BeforeFacts;
		CollectComparableFacts(Before.Facts, BeforeFacts);
		TArray<FString> AfterFacts;
		CollectComparableFacts(RawAfter, AfterFacts);
		FFileHelper::SaveStringArrayToFile(BeforeFacts, *(DumpDir / BaseName + TEXT(".before.facts")));
		FFileHelper::SaveStringArrayToFile(AfterFacts, *(DumpDir / BaseName + TEXT(".after.facts")));
		FFileHelper::SaveStringArrayToFile(ReportedFacts, *(DumpDir / BaseName + TEXT(".lost.facts")));

		auto Report = [bForceFromCommandLine](const FString& Line)
		{
			// Warning once the save is allowed, error while it is not: the severity has to match what
			// the run is about to do, or a forced build reads as a failed one in the log.
			if (bForceFromCommandLine)
			{
				UE_LOG(LogDreamFX, Warning, TEXT("%s"), *Line);
			}
			else
			{
				UE_LOG(LogDreamFX, Error, TEXT("%s"), *Line);
			}
		};

		Report(FString::Printf(
			TEXT("build safety: '%s' held %d fact(s), the rebuild produces %d; of the %d fact(s) it no longer holds exactly, %d are drift inside a structure the rebuild kept, %d gained."),
			*AssetPath, Comparison.BeforeCount, Comparison.AfterCount, Comparison.Candidates, Reported.Num(), Comparison.Gained));

		// Beside the refusals rather than folded into their count: the two are different answers, and a
		// reader who has to reach for -Force should be able to see how much of what went was not drift
		// -- otherwise the next refusal looks larger than it is and the gate teaches bypassing.
		if (Collapsed.Num() > 0)
		{
			Report(FString::Printf(
				TEXT("build safety: '%s': a further %d cop(y/ies) of a constant another script's store still carries collapsed into one -- not drift, and not counted above."),
				*AssetPath, Collapsed.Num()));
			ReportCollapsed();
		}

		// Capped like every other report here, and the cap says so. A whole family can go at once (a
		// stripped module, a recreated renderer), and a report nobody reads to the end is a report
		// nobody acts on; the dump above is what a reader works from at that size.
		constexpr int32 MaxReported = 200;
		for (int32 Index = 0; Index < FMath::Min(Reported.Num(), MaxReported); ++Index)
		{
			const FVerdict& Loss = Reported[Index];
			FString Line = FString::Printf(TEXT("            lost | %s : %s -> %s"),
				*Truncate(Loss.Display, 300), *Truncate(Loss.OldValue, 240), *Truncate(Loss.NewValue, 240));

			if (Loss.Kind == FVerdict::EKind::Deterministic)
			{
				Line += TEXT("   [deterministic drift (the text does not name this input)]");
			}
			else if (Loss.Kind == FVerdict::EKind::Suspected)
			{
				Line += TEXT("   [suspected drift (cannot tell)]");
			}

			Report(Line);
		}

		if (Reported.Num() > MaxReported)
		{
			Report(FString::Printf(
				TEXT("            lost | ... and %d more; the complete list is beside the other two"),
				Reported.Num() - MaxReported));
		}

		const FString DumpPattern = DumpDir / BaseName + TEXT(".{before,after,lost}.facts");

		if (!bForceFromCommandLine)
		{
			Diagnostics.Error(TEXT("DFX8018"), Location, FString::Printf(
				TEXT("'%s' was not saved: rebuilding it would drop %d fact(s) this source cannot express, and a save would destroy them. They are listed above, and the asset still holds every one of them. -Force writes anyway; full lists in %s"),
				*AssetPath, Reported.Num(), *DumpPattern));
			return false;
		}

		Diagnostics.Warning(TEXT("DFX8018"), Location, FString::Printf(
			TEXT("'%s' was saved with -Force after dropping %d fact(s) this source cannot express -- the asset no longer holds them. They are listed above; full lists in %s"),
			*AssetPath, Reported.Num(), *DumpPattern));
		return true;
	}
}
