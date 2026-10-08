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
		 * PostLoad discarded reports `writes: ` and nothing else from that family -- no stages, no data
		 * interfaces, and a writes line that says only "no VM was loaded in this session". The rebuild's
		 * own compile never produces an empty list, so on the before side that line is a fact the after
		 * side cannot have. Measured on `破空灰尘`: five of them were the entire "loss" of a rebuild that
		 * had lost nothing the compiler ever had a view of.
		 *
		 * Dropping it costs nothing. A script that genuinely writes nothing emits the same empty line on
		 * both sides, and two equal facts cancel in the comparison on their own. This does not excuse the
		 * gate from the rest of that family: when the before side DOES carry a VM, its stages, data
		 * interfaces and written attributes are compared like anything else.
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

		/**
		 * One fact, split into what it is about and the value it holds, so a loss can be reported as
		 * "was X, the rebuild writes Y" instead of as two unrelated lines.
		 *
		 * Only the `path = value` families split. A stage's fact is
		 * `emitter E simulation stage 0:C { ... }` and an event handler's is a sentence; both are their
		 * own address already, and splitting a stage on the first inner ` = ` would name half a stage
		 * as the path.
		 */
		void SplitFact(const FString& Fact, FString& OutPath, FString& OutValue)
		{
			if (Fact.Contains(TEXT("{")))
			{
				OutPath = Fact;
				OutValue.Reset();
				return;
			}

			int32 Separator = Fact.Find(TEXT(" = "));
			if (Separator != INDEX_NONE)
			{
				OutPath = Fact.Left(Separator);
				OutValue = Fact.Mid(Separator + 3);
			}
			else
			{
				OutPath = Fact;
				OutValue.Reset();
			}
		}

		/** Caps one fragment for the console; the lists written beside it hold the full text. */
		FString Truncate(const FString& Text, int32 Limit)
		{
			return Text.Len() <= Limit ? Text : Text.Left(Limit) + TEXT("...");
		}
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
		bool bForceFromCommandLine, const FString& AssetPath, const FSourceLocation& Location,
		FDiagnosticSink& Diagnostics)
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
		// agreement however the facts are ordered, so what is left over on the before side is exactly
		// what the rebuild drops. The same comparison asset-diff makes, for the same reason -- a
		// positional walk would report the first moved fact as every later fact having changed.
		TMap<FString, int32> Counts;
		for (const FString& Fact : BeforeFacts)
		{
			++Counts.FindOrAdd(Fact);
		}
		for (const FString& Fact : AfterFacts)
		{
			--Counts.FindOrAdd(Fact);
		}

		// The after side indexed by path, so a lost fact can name what replaced it. This is what turns
		// "MeshYaw is gone" into the fact a reader can decide on: it did not vanish, it became 0.
		TMap<FString, FString> AfterByPath;
		for (const FString& Fact : AfterFacts)
		{
			FString Path;
			FString Value;
			SplitFact(Fact, Path, Value);
			AfterByPath.Add(Path, Value);
		}

		TArray<FString> Lost;
		int32 Gained = 0;
		for (const TPair<FString, int32>& Entry : Counts)
		{
			for (int32 Copy = 0; Copy < FMath::Abs(Entry.Value); ++Copy)
			{
				if (Entry.Value > 0)
				{
					Lost.Add(Entry.Key);
				}
				else
				{
					++Gained;
				}
			}
		}
		Lost.Sort();

		if (Lost.Num() == 0)
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
		FFileHelper::SaveStringArrayToFile(Lost, *(DumpDir / BaseName + TEXT(".lost.facts")));

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
			TEXT("build safety: '%s' held %d fact(s), the rebuild produces %d; %d would be lost, %d gained."),
			*AssetPath, BeforeFacts.Num(), AfterFacts.Num(), Lost.Num(), Gained));

		// Capped like every other report here, and the cap says so. A whole family can go at once (a
		// stripped module, a recreated renderer), and a report nobody reads to the end is a report
		// nobody acts on; the dump above is what a reader works from at that size.
		constexpr int32 MaxReported = 200;
		for (int32 Index = 0; Index < FMath::Min(Lost.Num(), MaxReported); ++Index)
		{
			FString Path;
			FString OldValue;
			SplitFact(Lost[Index], Path, OldValue);

			// Three shapes, and the difference between them is what the reader decides on: a value the
			// rebuild replaced, an address the rebuild has nothing for, and one copy of a repeated
			// fact that the rebuild no longer carries.
			const FString* NewValue = AfterByPath.Find(Path);
			const FString Change = NewValue == nullptr
				? FString::Printf(TEXT("%s -> (missing)"), *Truncate(OldValue, 240))
				: (*NewValue == OldValue
					? FString::Printf(TEXT("%s -> one copy fewer"), *Truncate(OldValue, 240))
					: FString::Printf(TEXT("%s -> %s"), *Truncate(OldValue, 240), *Truncate(*NewValue, 240)));

			Report(FString::Printf(TEXT("            lost | %s : %s"), *Truncate(Path, 300), *Change));
		}

		if (Lost.Num() > MaxReported)
		{
			Report(FString::Printf(
				TEXT("            lost | ... and %d more; the complete list is beside the other two"),
				Lost.Num() - MaxReported));
		}

		const FString DumpPattern = DumpDir / BaseName + TEXT(".{before,after,lost}.facts");

		if (!bForceFromCommandLine)
		{
			Diagnostics.Error(TEXT("DFX8017"), Location, FString::Printf(
				TEXT("'%s' was not saved: rebuilding it would drop %d fact(s) this source cannot express, and a save would destroy them. They are listed above, and the asset still holds every one of them. -Force writes anyway; full lists in %s"),
				*AssetPath, Lost.Num(), *DumpPattern));
			return false;
		}

		Diagnostics.Warning(TEXT("DFX8017"), Location, FString::Printf(
			TEXT("'%s' was saved with -Force after dropping %d fact(s) this source cannot express -- the asset no longer holds them. They are listed above; full lists in %s"),
			*AssetPath, Lost.Num(), *DumpPattern));
		return true;
	}
}
