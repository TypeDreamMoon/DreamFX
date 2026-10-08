#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Diff/DreamFXAssetFacts.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXParser.h"
#include "DreamFXTypes.h"
#include "Generation/DreamFXGenerator.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "WriteBack/DreamFXPull.h"

/**
 * The write-back corpus (write-back ② and ③; Docs/tools/pull.md).
 *
 * Every other suite here asks what a build produces. This one asks the opposite question, and it asks
 * it on an asset that was BUILT IN MEMORY, which is what makes the interesting half testable at all:
 * the "tuned in the editor" case needs an asset the text disagrees with, and the only honest way to
 * get one is to move the asset after the build -- exactly what the editor does -- rather than to
 * check in a binary nobody can diff.
 *
 * The cases are code rather than comment directives, unlike the Parse suite, because their subject is
 * not a diagnostic: it is a byte-for-byte property of a file plus a fact-set comparison of two
 * assets, and both of those need the adapter to set up. What each fixture is FOR is written in the
 * fixture's own header comment, next to the content that gives it teeth.
 *
 * Nothing here writes a package: the builds run with bSave off and the fixture text is never touched,
 * so a run leaves the working tree exactly as it found it.
 */
namespace UE::DreamFX::Editor::CorpusTests
{
	/**
	 * The corpus path helpers and the fixture runner, defined by DreamFXCorpusTest.cpp and shared so
	 * both suites agree on where the corpus is and how a fixture is built. Declared here rather than in
	 * a header because a header for five functions is a third place for them to drift.
	 */
	FString GetCorpusRoot();
	FString ToFullPath(const FString& TestName);
	FString ToDisplayName(const FString& RelativePath);
	void RunPipeline(const FString& FilePath, FDiagnosticSink& Diagnostics, FGenerateResult& OutResult);
	FString FormatDiagnostics(const FDiagnosticSink& Diagnostics);
	FString DiffFirstLines(const FString& Left, const FString& Right, int32 MaxReported);
}

namespace UE::DreamFX::Editor::WriteBackTests
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using CorpusTests::ToFullPath;
	using CorpusTests::FormatDiagnostics;

	/**
	 * The corpus diff, with the count this suite always wants.
	 *
	 * Forwarded rather than defaulted: the corpus and this suite land in the same unity translation
	 * unit, and a default argument written in both a definition and a declaration of one function is a
	 * redefinition there.
	 */
	FString DiffFirstLines(const FString& Left, const FString& Right)
	{
		return CorpusTests::DiffFirstLines(Left, Right, 6);
	}

	/** How many lines the two texts disagree on, position by position. */
	int32 CountDifferingLines(const FString& Left, const FString& Right)
	{
		TArray<FString> LeftLines;
		TArray<FString> RightLines;
		Left.ParseIntoArrayLines(LeftLines, /*InCullEmpty=*/false);
		Right.ParseIntoArrayLines(RightLines, /*InCullEmpty=*/false);

		int32 Differing = FMath::Abs(LeftLines.Num() - RightLines.Num());
		const int32 Shared = FMath::Min(LeftLines.Num(), RightLines.Num());
		for (int32 Index = 0; Index < Shared; ++Index)
		{
			if (LeftLines[Index] != RightLines[Index])
			{
				++Differing;
			}
		}
		return Differing;
	}

	/** The line holding a needle, or an empty string. For "which line actually changed". */
	FString LineContaining(const FString& Text, const FString& Needle)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
		for (const FString& Line : Lines)
		{
			if (Line.Contains(Needle))
			{
				return Line;
			}
		}
		return FString();
	}

	/**
	 * The first non-comment line holding a needle.
	 *
	 * A fixture's own header comment names the module its structural case moves -- that is what the
	 * comment is for -- so a search that took the first match would compare the comment against the
	 * statement and report a pull that wrote nothing as a pass.
	 */
	FString StatementLineContaining(const FString& Text, const FString& Needle)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
		for (const FString& Line : Lines)
		{
			if (Line.TrimStartAndEnd().StartsWith(TEXT("//")))
			{
				continue;
			}
			if (Line.Contains(Needle))
			{
				return Line;
			}
		}
		return FString();
	}

	/**
	 * Everything the pull said, for a failure message.
	 *
	 * The commandlet logs these through `LogDiagnostics`, whose Info severity is below the default log
	 * verbosity -- so a corpus failure that does not carry them is a failure with no reason attached,
	 * which is how a suite teaches people to stop reading it.
	 */
	FString ReportOf(const FPullResult& Result)
	{
		return Result.Report.Num() > 0
			? TEXT("\n") + FString::Join(Result.Report, TEXT("\n"))
			: FString(TEXT("\n  (the run produced no messages)"));
	}

	/**
	 * Moves the built package out of the way before a rebuild takes its place.
	 *
	 * Both builds name the same asset, so without this the rebuild lands on the very object the first
	 * build is holding and a fact comparison would compare a system against itself. The same technique
	 * the RoundTrip suite uses, for the same reason.
	 */
	void ParkPackage(UNiagaraSystem* System)
	{
		if (System == nullptr)
		{
			return;
		}
		if (UPackage* Package = System->GetOutermost())
		{
			const FName Parked = MakeUniqueObjectName(
				nullptr, UPackage::StaticClass(), FName(*(Package->GetName() + TEXT("_Parked"))));
			Package->Rename(*Parked.ToString(), nullptr,
				REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
		}
	}

	/** Builds a fixture without saving anything, or reports why it could not. */
	UNiagaraSystem* BuildFixture(FAutomationTestBase& Test, const FString& Fixture,
		const FString& SourceText, FDiagnosticSink& Diagnostics)
	{
		FDocument Document;
		if (!FParser::ParseText(SourceText, Fixture, Document, Diagnostics))
		{
			Test.AddError(FString::Printf(TEXT("%s: the fixture does not parse.\n%s"),
				*Fixture, *FormatDiagnostics(Diagnostics)));
			return nullptr;
		}

		FGenerateOptions Options;
		Options.bSave = false;
		Options.bForce = true;
		const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		if (!Result.bSucceeded || Result.System == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s: the fixture does not build.\n%s"),
				*Fixture, *FormatDiagnostics(Diagnostics)));
			return nullptr;
		}
		return Result.System;
	}

	/** One `-Apply`-style pull over text in memory, with the baseline the caller hands in. */
	bool PullInMemory(FAutomationTestBase& Test, const FString& Fixture, const FString& SourceText,
		UNiagaraSystem* System, const FPullOptions& Options, FPullBaseline& Baseline,
		FPullResult& OutResult, FString& OutNewText)
	{
		FDiagnosticSink Diagnostics;
		Diagnostics.SetFile(Fixture);
		const bool bOk = FPuller::PullText(SourceText, Fixture, System, Options, Baseline,
			Diagnostics, OutResult, OutNewText);
		if (!bOk)
		{
			Test.AddError(FString::Printf(TEXT("%s: the pull could not run.\n%s"),
				*Fixture, *FormatDiagnostics(Diagnostics)));
		}
		return bOk;
	}

	/** One fact split into the structure it lives under and the property inside it. */
	struct FAddressedFact
	{
		FString Scope;    /** The structure, usage label included. */
		FString Store;    /** The same structure with the script label dropped. */
		FString Subject;
		FString Value;
	};

	/**
	 * Splits a fact the way the build safety gate does, plus a store-only variant.
	 *
	 * `Store` exists for the report and not for the decision: a constant can legitimately live in more
	 * than one script's store -- an emitter's update modules appear in both the system spawn and the
	 * system update script -- and a rebuild that collapses two identical copies into one has destroyed
	 * nothing. The gate treats that as a loss of one copy, which is a difference between the gate and a
	 * plain reading of the asset; this function reports it under its own name so the difference is
	 * visible rather than averaged away.
	 */
	FAddressedFact AddressFact(const FString& Fact)
	{
		FAddressedFact Out;
		const int32 Assign = Fact.Find(TEXT(" = "));
		const FString Head = Assign == INDEX_NONE ? Fact : Fact.Left(Assign);
		Out.Value = Assign == INDEX_NONE ? FString() : Fact.Mid(Assign + 3);

		if (Fact.StartsWith(TEXT("ri ")))
		{
			const int32 At = Head.Find(TEXT("Constants."));
			if (At != INDEX_NONE)
			{
				FString Tail = Head.Mid(At);
				const int32 Paren = Tail.Find(TEXT(" ("), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (Paren != INDEX_NONE)
				{
					Tail = Tail.Left(Paren);
				}
				const int32 Dot = Tail.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				Out.Scope = Head.Left(At) + (Dot == INDEX_NONE ? Tail : Tail.Left(Dot));
				Out.Subject = Dot == INDEX_NONE ? FString() : Tail.Mid(Dot + 1);

				// `<emitter> <usage> Constants.<emitter>.<node>` -> `<emitter> Constants.<emitter>.<node>`
				Out.Store = Out.Scope;
				const int32 FirstSpace = Out.Store.Find(TEXT(" "));
				const int32 SecondSpace = FirstSpace == INDEX_NONE
					? INDEX_NONE : Out.Store.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstSpace + 1);
				if (SecondSpace != INDEX_NONE)
				{
					Out.Store = Out.Store.Left(FirstSpace + 1) + Out.Store.Mid(SecondSpace + 1);
				}
				return Out;
			}
		}

		Out.Scope = Head;
		Out.Store = Head;
		return Out;
	}

	/**
	 * Rebuilds the SAME asset from the text a pull just produced, and reports what the rebuild lost.
	 *
	 * This is the acceptance's closure condition written as an assertion. The build safety gate
	 * (DFX8017) refuses a save when a rebuild drops a fact the text cannot express, so "no fact
	 * disappeared that the rebuild does not still carry" is very nearly "the gate would not have
	 * fired".
	 *
	 * Three outcomes, and they are deliberately separated rather than lumped into one "diff":
	 *
	 *   * the value is still there under the same address -- the ordinary case;
	 *   * the ADDRESS is gone -- a module the text removed, or a node the structural edit dropped. The
	 *     text's doing, and the gate's own rule 1 ignores it too;
	 *   * the address is gone but the same address WITHOUT its script label survives, with the same
	 *     value. Nothing was destroyed: the rebuild collapsed two identical copies into one. Reported,
	 *     not failed on, because the copy count is not state -- but reported, because the gate does not
	 *     make that distinction and a reader should know where the two disagree.
	 *
	 * `compiled` facts are left out entirely: they are the compiler's own view of a script, a recompile
	 * rewrites the line, and the gate treats that family as self-scoped for the same reason.
	 */
	void RebuildInPlaceAndCheckForLosses(FAutomationTestBase& Test, const FString& CaseName,
		const FString& Path, const FString& NewText, const FString& SourceText, UNiagaraSystem* System,
		const FString& Context)
	{
		if (System == nullptr)
		{
			return;
		}

		TArray<FString> BeforeRaw;
		DescribeSystemFacts(System, BeforeRaw);
		TArray<FString> Before;
		for (const FString& Fact : BeforeRaw)
		{
			if (!Fact.StartsWith(TEXT("compiled ")))
			{
				Before.Add(Fact);
			}
		}

		FDocument Rebuilt;
		FDiagnosticSink RebuildDiagnostics;
		if (!FParser::ParseText(NewText, Path, Rebuilt, RebuildDiagnostics))
		{
			Test.AddError(FString::Printf(TEXT("%s: the rewritten text does not parse.\n%s"),
				*CaseName, *FormatDiagnostics(RebuildDiagnostics)));
			return;
		}

		FGenerateOptions RebuildOptions;
		RebuildOptions.bSave = false;
		RebuildOptions.bForce = true;
		const FGenerateResult Result = FGenerator::Generate(Rebuilt, RebuildOptions, RebuildDiagnostics);
		if (!Result.bSucceeded || Result.System == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s: the rewritten text does not build.\n%s"),
				*CaseName, *FormatDiagnostics(RebuildDiagnostics)));
			return;
		}
		if (Result.System != System)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the rebuild produced a different asset object, so this comparison measured two assets rather than one rebuild."),
				*CaseName));
			return;
		}

		TArray<FString> AfterRaw;
		DescribeSystemFacts(System, AfterRaw);
		TArray<FString> After;
		for (const FString& Fact : AfterRaw)
		{
			if (!Fact.StartsWith(TEXT("compiled ")))
			{
				After.Add(Fact);
			}
		}

		// Exact matches first, then the address of what is left over.
		TMap<FString, int32> AfterByFact;
		for (const FString& Fact : After)
		{
			++AfterByFact.FindOrAdd(Fact);
		}

		TSet<FString> AfterAddresses;
		TSet<FString> AfterStoreValues;
		for (const FString& Fact : After)
		{
			const FAddressedFact Addressed = AddressFact(Fact);
			AfterAddresses.Add(Addressed.Scope + TEXT("\x1f") + Addressed.Subject);
			AfterStoreValues.Add(Addressed.Store + TEXT("\x1f") + Addressed.Subject + TEXT("\x1f") + Addressed.Value);
		}

		TArray<FString> Lost;
		TArray<FString> Collapsed;
		TArray<FString> StructuresGone;
		for (const FString& Fact : Before)
		{
			int32& Count = AfterByFact.FindOrAdd(Fact);
			if (Count > 0)
			{
				--Count;
				continue;
			}

			const FAddressedFact Addressed = AddressFact(Fact);
			if (AfterAddresses.Contains(Addressed.Scope + TEXT("\x1f") + Addressed.Subject))
			{
				Lost.Add(Fact);
				continue;
			}
			if (AfterStoreValues.Contains(
				Addressed.Store + TEXT("\x1f") + Addressed.Subject + TEXT("\x1f") + Addressed.Value))
			{
				Collapsed.Add(Fact); // the same value, one copy fewer: nothing was destroyed
				continue;
			}
			StructuresGone.Add(Fact); // the text removed the structure this lived in
		}

		const FString DumpDir = FPaths::ProjectSavedDir() / TEXT("DreamFX/WriteBack");
		IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/true);
		FFileHelper::SaveStringArrayToFile(BeforeRaw, *(DumpDir / CaseName + TEXT(".before.facts")));
		FFileHelper::SaveStringArrayToFile(AfterRaw, *(DumpDir / CaseName + TEXT(".after.facts")));
		FFileHelper::SaveStringToFile(NewText, *(DumpDir / CaseName + TEXT(".pulled.dfs")));

		if (Lost.Num() > 0)
		{
			FString Report;
			constexpr int32 MaxReported = 10;
			for (int32 Index = 0; Index < FMath::Min(Lost.Num(), MaxReported); ++Index)
			{
				Report += FString::Printf(TEXT("\n  lost | %s"), *Lost[Index].Left(260));
			}
			Test.AddError(FString::Printf(
				TEXT("%s: %s dropped %d fact(s) whose address the rebuild still has -- which is what the build safety gate refuses. Lists: %s and %s.%s"),
				*CaseName, *Context, Lost.Num(),
				*(DumpDir / CaseName + TEXT(".before.facts")), *(DumpDir / CaseName + TEXT(".after.facts")),
				*Report));
		}

		Test.AddInfo(FString::Printf(
			TEXT("%s: rebuilt in place from the pulled text -- %d fact(s) before, %d after, %d lost, %d duplicate copy/copies collapsed, %d structure(s) the text removed (the %d-line text diff is above)."),
			*CaseName, Before.Num(), After.Num(), Lost.Num(), Collapsed.Num(), StructuresGone.Num(),
			CountDifferingLines(SourceText, NewText)));
	}

	/** The stack address of one emitter stack. */
	FStackAddress StackAddressOf(UNiagaraSystem* System, const TCHAR* Emitter, EStackKind StackKind)
	{
		FStackAddress Address(System);
		if (Emitter != nullptr && *Emitter != 0)
		{
			Address = Address.WithEmitter(FName(Emitter));
		}
		return Address.WithScript(FNiagaraAdapter::ScriptNameForStack(StackKind));
	}

	/**
	 * The multiset difference of two fact sets, reported as a test failure with both lists on disk.
	 *
	 * The lists are written rather than truncated into the log: a fact difference is what the closure
	 * claim is made of, and a reader chasing one needs the whole set, not the first eight lines the
	 * console filter happened to keep. Paths go into the message so the failure is self-contained.
	 */
	void CompareFacts(FAutomationTestBase& Test, const FString& CaseName, const FString& Context,
		const TArray<FString>& Left, const TArray<FString>& Right,
		const TCHAR* LeftLabel, const TCHAR* RightLabel)
	{
		TMap<FString, int32> Counts;
		for (const FString& Fact : Left)
		{
			++Counts.FindOrAdd(Fact);
		}
		for (const FString& Fact : Right)
		{
			--Counts.FindOrAdd(Fact);
		}

		TArray<FString> OnlyLeft;
		TArray<FString> OnlyRight;
		for (const TPair<FString, int32>& Entry : Counts)
		{
			for (int32 Copy = 0; Copy < FMath::Abs(Entry.Value); ++Copy)
			{
				(Entry.Value > 0 ? OnlyLeft : OnlyRight).Add(Entry.Key);
			}
		}
		OnlyLeft.Sort();
		OnlyRight.Sort();

		if (OnlyLeft.Num() == 0 && OnlyRight.Num() == 0)
		{
			return;
		}

		const FString DumpDir = FPaths::ProjectSavedDir() / TEXT("DreamFX/WriteBack");
		IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/true);
		FFileHelper::SaveStringArrayToFile(Left, *(DumpDir / CaseName + TEXT(".left.facts")));
		FFileHelper::SaveStringArrayToFile(Right, *(DumpDir / CaseName + TEXT(".right.facts")));

		FString Report;
		constexpr int32 MaxReported = 10;
		for (int32 Index = 0; Index < FMath::Min(OnlyLeft.Num(), MaxReported); ++Index)
		{
			Report += FString::Printf(TEXT("\n  %s only | %s"), LeftLabel, *OnlyLeft[Index].Left(260));
		}
		for (int32 Index = 0; Index < FMath::Min(OnlyRight.Num(), MaxReported); ++Index)
		{
			Report += FString::Printf(TEXT("\n  %s only | %s"), RightLabel, *OnlyRight[Index].Left(260));
		}
		Test.AddError(FString::Printf(
			TEXT("%s: %d fact(s) only %s, %d only %s. Both lists: %s and %s.%s"),
			*Context, OnlyLeft.Num(), LeftLabel, OnlyRight.Num(), RightLabel,
			*(DumpDir / CaseName + TEXT(".left.facts")), *(DumpDir / CaseName + TEXT(".right.facts")),
			*Report));
	}

	/** Sets one float module input, the way a drag in the editor's stack does. */
	bool SetFloatInput(UNiagaraSystem* System, const TCHAR* Emitter, EStackKind Kind,
		const TCHAR* Module, const TCHAR* Input, float Value, FString& OutError)
	{
		const FNiagaraTypeDefinition FloatType = FNiagaraTypeDefinition::GetFloatDef();
		const FInputValue AsValue = FInputValue::MakeLiteral(FloatType.GetScriptStruct(), &Value);

		TArray<FString> Errors;
		FNiagaraAdapter::FWriteScope WriteScope(System);
		const FStackAddress Address = StackAddressOf(System, Emitter, Kind)
			.WithModule(FName(Module)).WithInput(FName(Input));
		if (!FNiagaraAdapter::SetInput(Address, AsValue, Errors))
		{
			OutError = FString::Join(Errors, TEXT(" | "));
			return false;
		}
		return true;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDreamFXWriteBackTest, "DreamFX.Corpus.WriteBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

void FDreamFXWriteBackTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	// One entry per acceptance property of the write-back, named for the property rather than for the
	// fixture: a failure should say which guarantee broke.
	static const TCHAR* const Cases[] =
	{
		TEXT("NoOp"),
		TEXT("ModuleInput"),
		TEXT("RendererProperty"),
		TEXT("SystemSetting"),
		TEXT("Assignment"),
		TEXT("DirtySet"),
		TEXT("StructureAdd"),
		TEXT("StructureRemove"),
		TEXT("StructureRefusedWithoutSwitch"),
	};

	for (const TCHAR* Case : Cases)
	{
		OutBeautifiedNames.Add(Case);
		OutTestCommands.Add(Case);
	}
}

bool FDreamFXWriteBackTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::WriteBackTests;

	auto FixturePath = [](const TCHAR* Name)
	{
		return ToFullPath(FString::Printf(TEXT("WriteBack/%s.dfs"), Name));
	};

	auto LoadFixture = [&](const TCHAR* Name, FString& OutText) -> FString
	{
		const FString Path = FixturePath(Name);
		if (!FFileHelper::LoadFileToString(OutText, *Path))
		{
			AddError(FString::Printf(TEXT("Could not read '%s'."), *Path));
			return FString();
		}
		return Path;
	};

	// -------------------------------------------------------------------------------------------
	// NoOp: a pull with nothing to do does not touch a byte.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("NoOp"))
	{
		FString SourceText;
		const FString Path = LoadFixture(TEXT("NoOp"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		FPullOptions Options;
		FPullBaseline Baseline;
		FPullResult Result;
		FString NewText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Result, NewText))
		{
			return false;
		}

		if (Result.Edits != 0 || NewText != SourceText)
		{
			AddError(FString::Printf(
				TEXT("a dry pull over an asset that was just built from this text produced %d edit(s); it must produce none.\n%s"),
				Result.Edits, *DiffFirstLines(SourceText, NewText)));
		}

		Options.bApply = true;
		FPullResult Applied;
		FString AppliedText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Applied, AppliedText))
		{
			return false;
		}

		// The invariant, stated as bytes rather than as "no edit was recorded": the two have been
		// different before, and the one that matters is the file.
		if (AppliedText != SourceText)
		{
			AddError(FString::Printf(
				TEXT("-Apply on a text that already holds every stored value changed it.\n%s"),
				*DiffFirstLines(SourceText, AppliedText)));
		}
		if (CountDifferingLines(SourceText, AppliedText) != 0)
		{
			AddError(FString::Printf(TEXT("the no-op write-back differs on %d line(s)."),
				CountDifferingLines(SourceText, AppliedText)));
		}

		AddInfo(FString::Printf(TEXT("no-op: %d compared, %d changed, %d not declared, %d withheld, %d edit(s)."),
			Applied.Compared, Applied.Changed, Applied.Undeclared, Applied.Withheld, Applied.Edits));
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// ModuleInput: a value the editor moved, written into the argument that declares it.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("ModuleInput"))
	{
		FString SourceText;
		const FString Path = LoadFixture(TEXT("Tuning"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		FString Error;
		if (!SetFloatInput(System, TEXT("Motes"), EStackKind::EmitterUpdate, TEXT("SpawnRate"),
			TEXT("SpawnRate"), 42.0f, Error))
		{
			AddError(FString::Printf(TEXT("the fixture's SpawnRate could not be moved on the asset: %s"), *Error));
			return false;
		}

		FPullOptions Options;
		Options.bApply = true; // the rewritten text is what this case is about
		FPullBaseline Baseline;
		FPullResult Result;
		FString NewText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Result, NewText))
		{
			return false;
		}

		if (Result.Changed != 1 || Result.Edits != 1)
		{
			AddError(FString::Printf(TEXT("expected exactly one value to be written; the run reported %d changed of %d edit(s), %d compared, %d unwritable, %d unaddressable."),
				Result.Changed, Result.Edits, Result.Compared, Result.Unwritable, Result.Unaddressable));
		}

		// Invariant 2: the file's diff is the number of values that moved, and nothing else.
		const int32 DifferingLines = CountDifferingLines(SourceText, NewText);
		if (DifferingLines != 1)
		{
			AddError(FString::Printf(TEXT("one value moved but %d line(s) of the file changed.\n%s%s"),
				DifferingLines, *DiffFirstLines(SourceText, NewText), *ReportOf(Result)));
		}

		const FString ChangedLine = LineContaining(NewText, TEXT("SpawnRate"));
		if (!ChangedLine.Contains(TEXT("42")))
		{
			AddError(FString::Printf(TEXT("the changed line does not carry the asset's value: '%s'"), *ChangedLine));
		}
		if (!ChangedLine.TrimStartAndEnd().EndsWith(TEXT(";")))
		{
			AddError(FString::Printf(TEXT("the changed line lost its semicolon: '%s'"), *ChangedLine));
		}
		if (NewText.Contains(TEXT("\r\n")) != SourceText.Contains(TEXT("\r\n")))
		{
			AddError(TEXT("the line ending style changed."));
		}

		// Idempotence, and the only real proof that the comparison is by value: a second pull over the
		// text it just produced must find nothing.
		{
			FPullBaseline Second;
			FPullResult SecondResult;
			FString SecondText;
			if (PullInMemory(*this, Path, NewText, System, Options, Second, SecondResult, SecondText)
				&& SecondResult.Edits != 0)
			{
				AddError(FString::Printf(TEXT("pulling the text pull had just written produced %d more edit(s).\n%s"),
					SecondResult.Edits, *DiffFirstLines(NewText, SecondText)));
			}
		}

		// The closure, and it is the thing the acceptance actually asks for: rebuild the SAME asset --
		// the one the editor moved, in place, with no parking and no second object -- from the text pull
		// just wrote, and check that the rebuild destroyed nothing.
		//
		// Comparing two different objects would answer a different question. `DescribeSystemFacts` reads
		// an asset's own state, and the state that matters is the one a following `dfx build` leaves
		// behind: that is what the build safety gate (DFX8017) looks at, one capture on either side of
		// its own rebuild. So this is that comparison, made directly.
		RebuildInPlaceAndCheckForLosses(*this, Parameters, Path, NewText, SourceText, System,
			TEXT("the rebuild pull's text asked for"));
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// RendererProperty: a renderer property the editor changed, rewritten in place.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("RendererProperty"))
	{
		FString SourceText;
		const FString Path = LoadFixture(TEXT("Tuning"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		TArray<FString> Errors;
		FNiagaraAdapter::FWriteScope WriteScope(System);
		const FStackAddress RendererAddress = StackAddressOf(System, TEXT("Motes"), EStackKind::ParticleUpdate)
			.WithRenderer(0);
		if (!FNiagaraAdapter::SetRendererProperties(RendererAddress, TEXT("{\"SortOrderHint\":7}"), Errors))
		{
			AddError(FString::Printf(TEXT("the fixture's renderer property could not be moved: %s"),
				*FString::Join(Errors, TEXT(" | "))));
			return false;
		}

		FPullOptions Options;
		Options.bApply = true; // the rewritten text is what this case is about
		FPullBaseline Baseline;
		FPullResult Result;
		FString NewText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Result, NewText))
		{
			return false;
		}

		const int32 DifferingLines = CountDifferingLines(SourceText, NewText);
		if (Result.Changed != 1 || DifferingLines != 1)
		{
			AddError(FString::Printf(
				TEXT("a moved renderer property should be one changed line; the run reported %d changed of %d edit(s) and the diff is %d line(s).\n%s%s"),
				Result.Changed, Result.Edits, DifferingLines, *DiffFirstLines(SourceText, NewText),
				*ReportOf(Result)));
		}

		const FString ChangedLine = LineContaining(NewText, TEXT("SortOrderHint"));
		if (!ChangedLine.Contains(TEXT("7")))
		{
			AddError(FString::Printf(TEXT("the changed line does not carry the asset's value: '%s'"), *ChangedLine));
		}

		AddInfo(FString::Printf(TEXT("renderer property: %d line(s) changed, %d compared."), DifferingLines, Result.Compared));
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// SystemSetting: a `Settings` key, read through the same table the plan writes through.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("SystemSetting"))
	{
		FString SourceText;
		const FString Path = LoadFixture(TEXT("Tuning"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		TArray<FString> Errors;
		if (!FNiagaraAdapter::SetSystemProperties(System, TEXT("{\"WarmupTime\":3.25}"), Errors))
		{
			AddError(FString::Printf(TEXT("the fixture's WarmupTime could not be moved: %s"),
				*FString::Join(Errors, TEXT(" | "))));
			return false;
		}

		FPullOptions Options;
		Options.bApply = true; // the rewritten text is what this case is about
		FPullBaseline Baseline;
		FPullResult Result;
		FString NewText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Result, NewText))
		{
			return false;
		}

		const int32 DifferingLines = CountDifferingLines(SourceText, NewText);
		if (Result.Changed != 1 || DifferingLines != 1)
		{
			AddError(FString::Printf(
				TEXT("a moved system setting should be one changed line; the run reported %d changed of %d edit(s) and the diff is %d line(s).\n%s"),
				Result.Changed, Result.Edits, DifferingLines, *DiffFirstLines(SourceText, NewText)));
		}

		const FString ChangedLine = LineContaining(NewText, TEXT("WarmupTime"));
		if (!ChangedLine.Contains(TEXT("3.25")))
		{
			AddError(FString::Printf(TEXT("the changed line does not carry the asset's value: '%s'"), *ChangedLine));
		}

		AddInfo(FString::Printf(TEXT("system setting: %d line(s) changed, %d compared."), DifferingLines, Result.Compared));
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// Assignment: the folded run, whose entries are the Set Parameters node's constants.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("Assignment"))
	{
		FString SourceText;
		const FString Path = LoadFixture(TEXT("Tuning"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		// `Particles.Drag = 0.5;` is the fixture's folded assignment. A Set Parameters module is named
		// `SetVariables_<guid>` and regenerated every build, so the address has to be read off the
		// asset rather than written down -- which is also the thing this case is checking.
		FString SetParametersNode;
		{
			FScriptStackInfo StackInfo;
			TArray<FString> Errors;
			if (!FNiagaraAdapter::GetScriptStackInfo(
				StackAddressOf(System, TEXT("Motes"), EStackKind::ParticleUpdate), StackInfo, Errors))
			{
				AddError(FString::Printf(TEXT("the fixture's ParticleUpdate stack could not be read: %s"),
					*FString::Join(Errors, TEXT(" | "))));
				return false;
			}
			for (const FModuleInfo& Module : StackInfo.Modules)
			{
				if (Module.bIsSetParameters)
				{
					SetParametersNode = Module.ModuleName.ToString();
					break;
				}
			}
		}

		if (SetParametersNode.IsEmpty())
		{
			AddError(TEXT("the fixture's folded assignment did not become a Set Parameters module, so this case cannot run."));
			return false;
		}

		FString Error;
		if (!SetFloatInput(System, TEXT("Motes"), EStackKind::ParticleUpdate, *SetParametersNode,
			TEXT("Particles.Drag"), 0.25f, Error))
		{
			AddError(FString::Printf(TEXT("the fixture's assignment could not be moved: %s"), *Error));
			return false;
		}

		FPullOptions Options;
		Options.bApply = true; // the rewritten text is what this case is about
		FPullBaseline Baseline;
		FPullResult Result;
		FString NewText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Result, NewText))
		{
			return false;
		}

		const int32 DifferingLines = CountDifferingLines(SourceText, NewText);
		if (Result.Changed != 1 || DifferingLines != 1)
		{
			AddError(FString::Printf(
				TEXT("a moved folded assignment should be one changed line; the run reported %d changed of %d edit(s) and the diff is %d line(s).\n%s"),
				Result.Changed, Result.Edits, DifferingLines, *DiffFirstLines(SourceText, NewText)));
		}

		const FString ChangedLine = LineContaining(NewText, TEXT("Particles.Drag"));
		if (!ChangedLine.Contains(TEXT("0.25")))
		{
			AddError(FString::Printf(TEXT("the changed line does not carry the asset's value: '%s'"), *ChangedLine));
		}

		AddInfo(FString::Printf(TEXT("assignment: %d line(s) changed, %d compared, node '%s'."),
			DifferingLines, Result.Compared, *SetParametersNode));
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// DirtySet: only what the asset actually moved.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("DirtySet"))
	{
		FString SourceText;
		const FString Path = LoadFixture(TEXT("Tuning"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		FString Error;
		if (!SetFloatInput(System, TEXT("Motes"), EStackKind::EmitterUpdate, TEXT("SpawnRate"),
			TEXT("SpawnRate"), 42.0f, Error))
		{
			AddError(FString::Printf(TEXT("the fixture's SpawnRate could not be moved: %s"), *Error));
			return false;
		}

		// The first apply, with no baseline: every declared value is compared, which is the documented
		// degradation, and the run records what the asset holds.
		FPullOptions Options;
		Options.bApply = true;
		FPullBaseline Baseline;
		FPullResult First;
		FString FirstText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, First, FirstText))
		{
			return false;
		}
		if (First.Changed != 1)
		{
			AddError(FString::Printf(TEXT("the first (baseline-free) apply should have written one value; it wrote %d."),
				First.Changed));
		}

		// What the caller does with a successful apply: keep what the run observed.
		Baseline.Records = First.ObservedRecords;
		Baseline.bLoaded = true;

		// Now the TEXT moves and the asset does not. That is the whole point of the dirty set: the text
		// is the newer decision, so pull must leave it alone -- even though the two still disagree.
		const FString EditedText = FirstText.Replace(TEXT("SpawnRate = 42"), TEXT("SpawnRate = 7"));
		if (EditedText == FirstText)
		{
			AddError(TEXT("the dirty-set case could not edit the text it was given; the value it looks for is not there."));
			return false;
		}

		FPullResult Second;
		FString SecondText;
		if (!PullInMemory(*this, Path, EditedText, System, Options, Baseline, Second, SecondText))
		{
			return false;
		}

		if (Second.Changed != 0 || SecondText != EditedText)
		{
			AddError(FString::Printf(
				TEXT("a value the asset has not moved since the baseline must not be written back; %d value(s) were, and the text changed.\n%s"),
				Second.Changed, *DiffFirstLines(EditedText, SecondText)));
		}
		if (Second.Withheld != 1)
		{
			AddError(FString::Printf(TEXT("expected the one differing value to be reported as withheld; %d were."),
				Second.Withheld));
		}

		// And the other half: the asset moves again, so the same comparison now has something to say.
		if (!SetFloatInput(System, TEXT("Motes"), EStackKind::EmitterUpdate, TEXT("SpawnRate"),
			TEXT("SpawnRate"), 99.0f, Error))
		{
			AddError(FString::Printf(TEXT("the fixture's SpawnRate could not be moved a second time: %s"), *Error));
			return false;
		}

		FPullResult Third;
		FString ThirdText;
		if (!PullInMemory(*this, Path, EditedText, System, Options, Baseline, Third, ThirdText))
		{
			return false;
		}
		if (Third.Changed != 1 || !LineContaining(ThirdText, TEXT("SpawnRate")).Contains(TEXT("99")))
		{
			AddError(FString::Printf(TEXT("an asset that moved again should be written back; %d value(s) were.\n%s"),
				Third.Changed, *DiffFirstLines(EditedText, ThirdText)));
		}

		AddInfo(FString::Printf(TEXT("dirty set: first apply %d written / %d compared, then %d withheld, then %d written again."),
			First.Changed, First.Compared, Second.Withheld, Third.Changed));
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// StructureAdd / StructureRemove: the editor moved the structure.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("StructureAdd") || Parameters == TEXT("StructureRemove"))
	{
		const bool bAdd = Parameters == TEXT("StructureAdd");

		FString SourceText;
		const FString Path = LoadFixture(TEXT("Structure"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		const FStackAddress StackAddress = StackAddressOf(System, TEXT("Motes"), EStackKind::ParticleUpdate);
		const TCHAR* const ModuleName = bAdd ? TEXT("ScaleColor") : TEXT("Drag");

		TArray<FString> Errors;
		{
			FNiagaraAdapter::FWriteScope WriteScope(System);
			if (bAdd)
			{
				FModuleLibrary Library;
				FString ModuleError;
				UNiagaraScript* ModuleAsset = Library.FindModule(ModuleName, ModuleError);
				if (ModuleAsset == nullptr)
				{
					AddError(FString::Printf(TEXT("the fixture's module '%s' does not resolve: %s"),
						ModuleName, *ModuleError));
					return false;
				}

				FName AddedName;
				if (!FNiagaraAdapter::AddModule(StackAddress, ModuleAsset, AddedName, Errors,
					/*bDeferStackRefresh=*/false))
				{
					AddError(FString::Printf(TEXT("the module could not be added to the asset: %s"),
						*FString::Join(Errors, TEXT(" | "))));
					return false;
				}
			}
			else
			{
				// The node to delete: the one whose module asset is `SolveForcesAndVelocity`.
				FScriptStackInfo StackInfo;
				Errors.Reset();
				if (!FNiagaraAdapter::GetScriptStackInfo(StackAddress, StackInfo, Errors))
				{
					AddError(FString::Printf(TEXT("the fixture's ParticleUpdate stack could not be read: %s"),
						*FString::Join(Errors, TEXT(" | "))));
					return false;
				}

				FName Target = NAME_None;
				for (const FModuleInfo& Module : StackInfo.Modules)
				{
					if (!Module.bIsSetParameters && Module.ModuleName.ToString() == ModuleName)
					{
						Target = Module.ModuleName;
						break;
					}
				}
				if (Target.IsNone())
				{
					AddError(FString::Printf(TEXT("the fixture's asset has no '%s' node to remove."), ModuleName));
					return false;
				}

				Errors.Reset();
				if (!FNiagaraAdapter::RemoveModule(StackAddress.WithModule(Target), Errors))
				{
					AddError(FString::Printf(TEXT("the module could not be removed from the asset: %s"),
						*FString::Join(Errors, TEXT(" | "))));
					return false;
				}

				// Deleting a node does not delete the rapid-iteration constants it had materialised --
				// that is the generator's CleanUpStaleParameters, and a rebuild runs it before the stack
				// is re-applied. Without it the asset would still hold the dead node's constants and the
				// fact comparison at the end of this case would report them as a loss the pull caused,
				// which is exactly the kind of measurement error the rest of this suite exists to avoid.
				Errors.Reset();
				if (!FNiagaraAdapter::CleanUpStaleParameters(StackAddress, Errors))
				{
					AddError(FString::Printf(TEXT("the asset's stale parameters could not be cleaned up: %s"),
						*FString::Join(Errors, TEXT(" | "))));
					return false;
				}
			}
		}

		FPullOptions Options;
		Options.bApply = true;
		Options.bStructure = true; // this case is the switch's whole reason for existing
		FPullBaseline Baseline;
		FPullResult Result;
		FString NewText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Result, NewText))
		{
			return false;
		}

		if (Result.StructureRefused != 0)
		{
			AddError(FString::Printf(TEXT("the structural difference was refused (%d stack(s)); every structural case here is unambiguous by construction."),
				Result.StructureRefused));
			return false;
		}

		// An insertion ADDS a line, so every line below it shifts: the count of positions that differ is
		// not the number of edits, and the assertion has to be about the file's line count and the
		// statement that appeared.
		TArray<FString> BeforeLines;
		TArray<FString> AfterLines;
		SourceText.ParseIntoArrayLines(BeforeLines, /*InCullEmpty=*/false);
		NewText.ParseIntoArrayLines(AfterLines, /*InCullEmpty=*/false);
		const int32 Delta = AfterLines.Num() - BeforeLines.Num();

		if (bAdd && (Result.Added != 1 || Result.Removed != 0 || Delta != 1))
		{
			AddError(FString::Printf(TEXT("expected one added line; the run reported %d added, %d removed and the file went from %d to %d line(s).\n%s%s"),
				Result.Added, Result.Removed, BeforeLines.Num(), AfterLines.Num(),
				*DiffFirstLines(SourceText, NewText), *ReportOf(Result)));
		}
		if (!bAdd && (Result.Removed != 1 || Result.Added != 0 || Delta != -1))
		{
			AddError(FString::Printf(TEXT("expected exactly one removed line and no additions; the run reported %d removed, %d added and the file went from %d to %d line(s).\n%s"),
				Result.Removed, Result.Added, BeforeLines.Num(), AfterLines.Num(),
				*ReportOf(Result)));
		}

		if (bAdd)
		{
			// The STATEMENT, not the first line that mentions the module: this fixture's own header
			// comment names it too, and a test that matched the comment would pass on a pull that wrote
			// nothing at all.
			const FString AddedLine = StatementLineContaining(NewText, FString(ModuleName));
			if (AddedLine.IsEmpty() || !AddedLine.TrimStartAndEnd().EndsWith(TEXT(";")))
			{
				AddError(FString::Printf(TEXT("the added line is not a statement: '%s'"), *AddedLine));
			}
			else
			{
				// The indentation has to come from the file, not from this code.
				const FString NeighbourIndent = TEXT("            ");
				if (!AddedLine.StartsWith(NeighbourIndent))
				{
					AddError(FString::Printf(
						TEXT("the added line does not carry the indentation of the statements around it: '%s'"),
						*AddedLine));
				}
				// A whole line, not a fragment glued onto another statement: what follows the added
				// text on its line has to be a line ending. (`AddedLine` came out of the file already,
				// so this is about what is next to it -- and the check cannot be `Contains(AddedLine +
				// "\n")`, because the file's lines end with `\r\n` and that substring is not in one.)
				const int32 At = NewText.Find(AddedLine, ESearchCase::CaseSensitive);
				const bool bEndsItsOwnLine = At != INDEX_NONE && At + AddedLine.Len() < NewText.Len()
					&& (NewText[At + AddedLine.Len()] == TEXT('\n') || NewText[At + AddedLine.Len()] == TEXT('\r'));
				if (!bEndsItsOwnLine)
				{
					AddError(TEXT("the added line is not a line of the file: it has no newline after it."));
				}
			}
		}
		else if (!LineContaining(NewText, FString(ModuleName) + TEXT("(")).IsEmpty())
		{
			AddError(FString::Printf(TEXT("the line the asset no longer has is still there: '%s'"),
				*LineContaining(NewText, FString(ModuleName) + TEXT("("))));
		}

		// The closure: rebuild the same asset, in place, from the text the structural edit produced, and
		// check that the rebuild destroyed nothing. That is the claim the acceptance makes -- pull, then
		// build, then the safety gate stays quiet -- and it is made here on the asset the editor left
		// behind rather than on a second object built from scratch.
		RebuildInPlaceAndCheckForLosses(*this, Parameters, Path, NewText, SourceText, System,
			bAdd ? TEXT("the rebuild after a module was added to the text")
			     : TEXT("the rebuild after a module was removed from the text"));
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// StructureRefusedWithoutSwitch: the default is the report.
	// -------------------------------------------------------------------------------------------
	if (Parameters == TEXT("StructureRefusedWithoutSwitch"))
	{
		FString SourceText;
		const FString Path = LoadFixture(TEXT("Structure"), SourceText);
		if (Path.IsEmpty())
		{
			return false;
		}

		FDiagnosticSink BuildDiagnostics;
		UNiagaraSystem* System = BuildFixture(*this, Path, SourceText, BuildDiagnostics);
		if (System == nullptr)
		{
			return false;
		}

		TArray<FString> Errors;
		{
			FNiagaraAdapter::FWriteScope WriteScope(System);
			FModuleLibrary Library;
			FString ModuleError;
			UNiagaraScript* ModuleAsset = Library.FindModule(TEXT("ScaleColor"), ModuleError);
			if (ModuleAsset == nullptr)
			{
				AddError(FString::Printf(TEXT("the fixture's module does not resolve: %s"), *ModuleError));
				return false;
			}

			FName AddedName;
			if (!FNiagaraAdapter::AddModule(StackAddressOf(System, TEXT("Motes"), EStackKind::ParticleUpdate),
				ModuleAsset, AddedName, Errors, /*bDeferStackRefresh=*/false))
			{
				AddError(FString::Printf(TEXT("the module could not be added to the asset: %s"),
					*FString::Join(Errors, TEXT(" | "))));
				return false;
			}
		}

		FPullOptions Options;
		Options.bApply = true; // -Apply and no -Structure: values yes, lines no
		FPullBaseline Baseline;
		FPullResult Result;
		FString NewText;
		if (!PullInMemory(*this, Path, SourceText, System, Options, Baseline, Result, NewText))
		{
			return false;
		}

		if (Result.Added != 0 || Result.Removed != 0)
		{
			AddError(FString::Printf(TEXT("without -Structure nothing may be added or removed; the run reported %d added, %d removed."),
				Result.Added, Result.Removed));
		}
		if (Result.StructureRefused != 1)
		{
			AddError(FString::Printf(TEXT("the structural difference should have been reported once; it was reported %d time(s)."),
				Result.StructureRefused));
		}

		// The run must not have written ANY of the stack: the node positions it would address values
		// through are exactly what could not be pinned down.
		if (CountDifferingLines(SourceText, NewText) != 0)
		{
			AddError(FString::Printf(
				TEXT("without -Structure the text must be untouched, and it is not.\n%s"),
				*DiffFirstLines(SourceText, NewText)));
		}

		AddInfo(FString::Printf(TEXT("structure refused: %d refused, %d added, %d removed, %d unaddressable."),
			Result.StructureRefused, Result.Added, Result.Removed, Result.Unaddressable));
		return true;
	}

	AddError(FString::Printf(TEXT("no write-back case is registered under '%s'."), *Parameters));
	return false;
}

#endif // WITH_AUTOMATION_TESTS
