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

		/** One line of the report: what the fact was about, what it held, and what it becomes. */
		struct FReportedLoss
		{
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
		};
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

		TArray<FString> BeforeFacts;
		CollectComparableFacts(Before.Facts, BeforeFacts);

		TArray<FString> AfterFacts;
		CollectComparableFacts(RawAfter, AfterFacts);

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
		TSet<FString> AfterScopes;
		TMap<FString, FString> AfterByKey;
		for (const FString& Fact : AfterFacts)
		{
			const FAddressedFact Addressed = AddressFact(Fact);
			AfterScopes.Add(Addressed.Scope);
			AfterByKey.Add(MakeKey(Addressed.Scope, Addressed.Subject), Addressed.Value);
		}

		TArray<FReportedLoss> Reported;
		TArray<FString> ReportedFacts;
		int32 Candidates = 0;
		int32 Gained = 0;

		for (const TPair<FString, int32>& Entry : Counts)
		{
			// Zero is a fact both sides carry the same number of times: agreement, and the bulk of
			// any real asset. Only the positive side is a candidate, and the negative side is what
			// the rebuild added.
			if (Entry.Value <= 0)
			{
				Gained += -Entry.Value;
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

			++Candidates;
			const FString* NewValue = AfterByKey.Find(MakeKey(Addressed.Scope, Addressed.Subject));

			for (int32 Copy = 0; Copy < Entry.Value; ++Copy)
			{
				FReportedLoss Loss;
				Loss.Display = Addressed.Display;
				Loss.OldValue = Addressed.Value;

				if (NewValue == nullptr)
				{
					// Rule 2. The fact is gone from a structure that stayed: the text has no spelling
					// for it, so a save would destroy it.
					Loss.NewValue = TEXT("(missing)");
				}
				else if (*NewValue == Addressed.Value)
				{
					// The same fact, one copy fewer: a repeated fact the rebuild now carries once.
					Loss.NewValue = TEXT("one copy fewer");
				}
				else
				{
					// Rule 3. The value changed, and the source decides: a subject the text names is a
					// value the text meant, so the rebuild is doing what it was told.
					Loss.NewValue = *NewValue;
					if (Declared.Names(Addressed.Scope, Addressed.Subject))
					{
						continue;
					}

					// Rule 4. Refuse when it cannot be decided, but say which of the two this is:
					// "the text does not name this" is a decision the reader can act on, "cannot tell"
					// is one they have to investigate first.
					Loss.Kind = Declared.HasScope(Addressed.Scope)
						? FReportedLoss::EKind::Deterministic
						: FReportedLoss::EKind::Suspected;
				}

				Reported.Add(MoveTemp(Loss));
				ReportedFacts.Add(Entry.Key);
			}
		}

		if (Reported.Num() == 0)
		{
			// Verbose, because a clean rebuild is the common case -- but it is a line, so "was this
			// build checked?" has an answer in the log rather than an absence of evidence.
			UE_LOG(LogDreamFX, Verbose,
				TEXT("build safety: '%s' still holds all %d fact(s) after the rebuild."),
				*AssetPath, BeforeFacts.Num());
			return true;
		}

		// Both lists, not just the losses: after a -Force save the asset no longer holds what it held
		// before, so this is the only record of it, and a truncated console line is not a record.
		const FString DumpDir = FPaths::ProjectSavedDir() / TEXT("DreamFX/BuildSafety");
		const FString BaseName = FPackageName::GetShortName(AssetPath);
		IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/true);
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
			*AssetPath, BeforeFacts.Num(), AfterFacts.Num(), Candidates, Reported.Num(), Gained));

		// Capped like every other report here, and the cap says so. A whole family can go at once (a
		// stripped module, a recreated renderer), and a report nobody reads to the end is a report
		// nobody acts on; the dump above is what a reader works from at that size.
		constexpr int32 MaxReported = 200;
		for (int32 Index = 0; Index < FMath::Min(Reported.Num(), MaxReported); ++Index)
		{
			const FReportedLoss& Loss = Reported[Index];
			FString Line = FString::Printf(TEXT("            lost | %s : %s -> %s"),
				*Truncate(Loss.Display, 300), *Truncate(Loss.OldValue, 240), *Truncate(Loss.NewValue, 240));

			if (Loss.Kind == FReportedLoss::EKind::Deterministic)
			{
				Line += TEXT("   [deterministic drift (the text does not name this input)]");
			}
			else if (Loss.Kind == FReportedLoss::EKind::Suspected)
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
			Diagnostics.Error(TEXT("DFX8017"), Location, FString::Printf(
				TEXT("'%s' was not saved: rebuilding it would drop %d fact(s) this source cannot express, and a save would destroy them. They are listed above, and the asset still holds every one of them. -Force writes anyway; full lists in %s"),
				*AssetPath, Reported.Num(), *DumpPattern));
			return false;
		}

		Diagnostics.Warning(TEXT("DFX8017"), Location, FString::Printf(
			TEXT("'%s' was saved with -Force after dropping %d fact(s) this source cannot express -- the asset no longer holds them. They are listed above; full lists in %s"),
			*AssetPath, Reported.Num(), *DumpPattern));
		return true;
	}
}
