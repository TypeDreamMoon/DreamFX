#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "Diff/DreamFXAssetFacts.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXParser.h"
#include "DreamFXTypes.h"
#include "Generation/DreamFXGenerator.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lint/DreamFXLint.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"

/**
 * The corpus runner (plan-v2 W2).
 *
 * Every behaviour DreamFX has was established by experiment against a specific engine, and almost
 * none of it is guaranteed by a type. The decompiler's shortest-unambiguous module names, the schema
 * probe's identifier normalisation, the order static switches must be written in -- change any of
 * them and the code still compiles, still runs, and quietly produces something different. Files under
 * Tests/Corpus are what turns each of those into a failing test instead.
 *
 * Three suites, one per corpus directory, each a complex automation test so the file is the test case
 * and a failure names it:
 *
 *   DreamFX.Corpus.Parse      -- a source and the diagnostics it must produce, by code and position
 *   DreamFX.Corpus.Generate   -- a source and the asset topology it must produce, as checked-in JSON
 *   DreamFX.Corpus.RoundTrip  -- a source that must survive decompile -> rebuild -> decompile unchanged
 *
 * Nothing here writes a package. Generation runs with bSave off, so the assets live and die in memory
 * and a test run leaves the working tree exactly as it found it.
 */
namespace UE::DreamFX::Editor::CorpusTests
{
	FString GetCorpusRoot()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamFX"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Tests/Corpus"));
	}

	/**
	 * @param Wildcard  a filename pattern such as `*.dfs`, not a bare extension.
	 *
	 * bClearFileNames is false so successive calls accumulate; the default would have each call throw
	 * away the last one's results.
	 */
	void FindCorpusFiles(const TCHAR* SubDirectory, const TCHAR* Wildcard, TArray<FString>& OutFiles)
	{
		const FString Root = GetCorpusRoot();
		if (Root.IsEmpty())
		{
			return;
		}
		IFileManager::Get().FindFilesRecursive(OutFiles, *(Root / SubDirectory), Wildcard,
			/*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/false);
		OutFiles.Sort();
	}

	/** Corpus paths are reported relative to Tests/Corpus so a test name is stable across machines. */
	FString ToTestName(const FString& FullPath)
	{
		FString Relative = FullPath;
		FPaths::MakePathRelativeTo(Relative, *(GetCorpusRoot() / TEXT("")));
		return Relative.Replace(TEXT("\\"), TEXT("/"));
	}

	FString ToFullPath(const FString& TestName)
	{
		return GetCorpusRoot() / TestName;
	}

	/**
	 * The name the automation hierarchy shows.
	 *
	 * The controller splits on '.', so a path like `Parse/BadInputName.bad.dfs` would present as a
	 * three-level tree whose leaf is called "dfs" -- forty tests all named "dfs". Directory separators
	 * become the hierarchy and the remaining dots become underscores, which keeps the leaf the name of
	 * the fixture. The command string stays the real relative path, which is what RunTest reopens.
	 */
	FString ToDisplayName(const FString& RelativePath)
	{
		FString Display = RelativePath;
		Display.ReplaceInline(TEXT("."), TEXT("_"));
		Display.ReplaceInline(TEXT("/"), TEXT("."));
		return Display;
	}

	/** One `// EXPECT DFX3003 (12,13)` directive. Position is optional; code is not. */
	struct FExpectation
	{
		FString Code;
		int32 Line = INDEX_NONE;
		int32 Column = INDEX_NONE;
		bool bMatched = false;

		FString Describe() const
		{
			return Line == INDEX_NONE
				? Code
				: FString::Printf(TEXT("%s at (%d,%d)"), *Code, Line, Column);
		}
	};

	/**
	 * Reads the expectations out of a fixture's own comments.
	 *
	 * Keeping them in the file rather than in a side table is what makes a fixture readable on its
	 * own: the source that provokes the diagnostic and the diagnostic it must provoke are three lines
	 * apart, and a diff that changes one shows the other.
	 */
	TArray<FExpectation> ParseExpectations(const FString& SourceText)
	{
		TArray<FExpectation> Expectations;

		TArray<FString> Lines;
		SourceText.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.StartsWith(TEXT("// EXPECT")))
			{
				continue;
			}

			FString Remainder = Trimmed.Mid(9).TrimStartAndEnd();

			FExpectation Expectation;
			int32 Space = INDEX_NONE;
			if (Remainder.FindChar(TEXT(' '), Space))
			{
				Expectation.Code = Remainder.Left(Space);
				Remainder = Remainder.Mid(Space + 1).TrimStartAndEnd();
			}
			else
			{
				Expectation.Code = Remainder;
				Remainder.Empty();
			}

			// `(12,13)` -- the position the diagnostic has to point at. A fixture that only cares which
			// code fires leaves it off.
			int32 Open = INDEX_NONE;
			int32 Close = INDEX_NONE;
			if (Remainder.FindChar(TEXT('('), Open) && Remainder.FindChar(TEXT(')'), Close) && Close > Open)
			{
				const FString Inside = Remainder.Mid(Open + 1, Close - Open - 1);
				FString LineText;
				FString ColumnText;
				if (Inside.Split(TEXT(","), &LineText, &ColumnText))
				{
					Expectation.Line = FCString::Atoi(*LineText.TrimStartAndEnd());
					Expectation.Column = FCString::Atoi(*ColumnText.TrimStartAndEnd());
				}
			}

			if (!Expectation.Code.IsEmpty())
			{
				Expectations.Add(Expectation);
			}
		}

		return Expectations;
	}

	/**
	 * Runs a fixture the way a build would, without writing anything.
	 *
	 * Lint is not run here: FGenerator::Generate already runs it for every kind of document it can
	 * generate, and running it twice would report every warning twice -- which reads as a rule firing
	 * more often than it should.
	 */
	void RunPipeline(const FString& FilePath, FDiagnosticSink& Diagnostics, FGenerateResult& OutResult)
	{
		FDocument Document;
		if (!FParser::ParseFile(FilePath, Document, Diagnostics))
		{
			return;
		}

		if (Document.Kind == EDocumentKind::Emitter)
		{
			// A .dfe generates nothing on its own, so nothing else would lint it.
			FLint::Run(Document, Diagnostics);
			return;
		}

		FGenerateOptions Options;
		Options.bSave = false;
		Options.bForce = true;
		OutResult = FGenerator::Generate(Document, Options, Diagnostics);
	}

	/**
	 * The first few differing lines of two texts.
	 *
	 * Dumping both sides in full is what a diff exists to avoid: two hundred identical lines around
	 * the one that changed, in a log, is not a report anyone reads.
	 */
	FString DiffFirstLines(const FString& Left, const FString& Right, int32 MaxReported = 6)
	{
		TArray<FString> LeftLines;
		TArray<FString> RightLines;
		Left.ParseIntoArrayLines(LeftLines, /*InCullEmpty=*/false);
		Right.ParseIntoArrayLines(RightLines, /*InCullEmpty=*/false);

		TArray<FString> Report;
		const int32 Count = FMath::Max(LeftLines.Num(), RightLines.Num());
		for (int32 Index = 0; Index < Count && Report.Num() < MaxReported; ++Index)
		{
			const FString LeftLine = LeftLines.IsValidIndex(Index) ? LeftLines[Index] : TEXT("<missing>");
			const FString RightLine = RightLines.IsValidIndex(Index) ? RightLines[Index] : TEXT("<missing>");
			if (LeftLine != RightLine)
			{
				Report.Add(FString::Printf(TEXT("  line %d:\n    first:  %s\n    second: %s"),
					Index + 1, *LeftLine, *RightLine));
			}
		}

		if (Report.Num() == 0)
		{
			return FString::Printf(TEXT("  (no differing line; lengths %d vs %d -- trailing whitespace?)"),
				Left.Len(), Right.Len());
		}
		return FString::Join(Report, TEXT("\n"));
	}

	FString FormatDiagnostics(const FDiagnosticSink& Diagnostics)
	{
		TArray<FString> Lines;
		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			Lines.Add(TEXT("    ") + Diagnostic.Format());
		}
		return Lines.Num() > 0 ? FString::Join(Lines, TEXT("\n")) : TEXT("    (none)");
	}

	/**
	 * Serialises what a build produced, in the shape the golden files compare against.
	 *
	 * Module order, the boundaries Set Parameters folds at, renderer order and which stacks were left
	 * alone -- all of it is decided by rules with no other test, and all of it is invisible in a
	 * successful build. Writing it down is what makes a change to any of them show up as a diff.
	 */
	FString DescribeTopology(UNiagaraSystem* System)
	{
		TArray<FString> Lines;
		TArray<FString> Errors;

		auto DescribeStack = [&](const FStackAddress& ScopeAddress, EStackKind Stack, const TCHAR* Indent)
		{
			FScriptStackInfo StackInfo;
			const FStackAddress ScriptAddress =
				ScopeAddress.WithScript(FNiagaraAdapter::ScriptNameForStack(Stack));
			if (!FNiagaraAdapter::GetScriptStackInfo(ScriptAddress, StackInfo, Errors)
				|| StackInfo.Modules.Num() == 0)
			{
				return;
			}

			Lines.Add(FString::Printf(TEXT("%s%s:"), Indent, LexStackKind(Stack)));

			int32 SetParametersOrdinal = 0;
			for (const FModuleInfo& Module : StackInfo.Modules)
			{
				// A Set Parameters module is named SetVariables_<guid>, freshly generated every build.
				// Printing it verbatim would make the golden fail on the next run for a reason that has
				// nothing to do with the topology, so it is numbered by position instead -- which is the
				// part that actually carries meaning.
				const FString DisplayName = Module.bIsSetParameters
					? FString::Printf(TEXT("SetParameters#%d"), SetParametersOrdinal++)
					: Module.ModuleName.ToString();

				Lines.Add(FString::Printf(TEXT("%s  - %s%s"), Indent, *DisplayName,
					Module.bEnabled ? TEXT("") : TEXT("  [disabled]")));

				// A Set Parameters module is where L2's folding rule becomes visible: its entries are
				// exactly the run of consecutive assignments the source wrote, and the boundary between
				// two such modules is where a module call interrupted them.
				if (Module.bIsSetParameters)
				{
					for (const FInputInfo& Entry : Module.Inputs)
					{
						Lines.Add(FString::Printf(TEXT("%s      = %s"), Indent, *Entry.Name.ToString()));
					}
				}
			}
		};

		TArray<FUserVariableInfo> UserVariables;
		if (FNiagaraAdapter::GetUserVariables(System, UserVariables, Errors))
		{
			for (const FUserVariableInfo& Variable : UserVariables)
			{
				Lines.Add(FString::Printf(TEXT("user %s : %s"),
					*Variable.Name.ToString(), *Variable.Type.GetName()));
			}
		}

		const FStackAddress SystemAddress(System);
		DescribeStack(SystemAddress, EStackKind::SystemSpawn, TEXT(""));
		DescribeStack(SystemAddress, EStackKind::SystemUpdate, TEXT(""));

		TArray<FName> EmitterNames;
		FNiagaraAdapter::GetEmitterNames(System, EmitterNames, Errors);

		for (const FName& EmitterName : EmitterNames)
		{
			Lines.Add(FString::Printf(TEXT("emitter %s"), *EmitterName.ToString()));

			const FStackAddress EmitterAddress = SystemAddress.WithEmitter(EmitterName);

			DescribeStack(EmitterAddress, EStackKind::EmitterSpawn, TEXT("  "));
			DescribeStack(EmitterAddress, EStackKind::EmitterUpdate, TEXT("  "));
			DescribeStack(EmitterAddress, EStackKind::ParticleSpawn, TEXT("  "));
			DescribeStack(EmitterAddress, EStackKind::ParticleUpdate, TEXT("  "));

			FEmitterInfo EmitterInfo;
			if (FNiagaraAdapter::GetEmitterInfo(EmitterAddress, EmitterInfo, Errors))
			{
				// Declaration order is renderer order (L8), and there is no other addressing scheme --
				// a reordering here silently repaints the effect, so the index is part of the golden.
				for (const FRendererInfo& Renderer : EmitterInfo.Renderers)
				{
					Lines.Add(FString::Printf(TEXT("  renderer %d: %s"), Renderer.Index,
						Renderer.Class != nullptr
							? *FNiagaraAdapter::RendererTypeNameForClass(Renderer.Class)
							: TEXT("<null>")));
				}
			}
		}

		return FString::Join(Lines, TEXT("\n")) + TEXT("\n");
	}
}

// ---------------------------------------------------------------------------------------------
// Parse: the diagnostics a source must produce
// ---------------------------------------------------------------------------------------------

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDreamFXParseCorpusTest, "DreamFX.Corpus.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

void FDreamFXParseCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	using namespace UE::DreamFX::Editor::CorpusTests;

	TArray<FString> Files;
	FindCorpusFiles(TEXT("Parse"), TEXT("*.dfs"), Files);
	FindCorpusFiles(TEXT("Parse"), TEXT("*.dfe"), Files);
	FindCorpusFiles(TEXT("Parse"), TEXT("*.dfm"), Files);

	for (const FString& File : Files)
	{
		const FString Name = ToTestName(File);
		OutBeautifiedNames.Add(ToDisplayName(Name));
		OutTestCommands.Add(Name);
	}
}

bool FDreamFXParseCorpusTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::CorpusTests;

	const FString FilePath = ToFullPath(Parameters);

	FString SourceText;
	if (!FFileHelper::LoadFileToString(SourceText, *FilePath))
	{
		AddError(FString::Printf(TEXT("Could not read '%s'."), *FilePath));
		return false;
	}

	TArray<FExpectation> Expectations = ParseExpectations(SourceText);
	if (Expectations.Num() == 0)
	{
		AddError(FString::Printf(
			TEXT("'%s' declares no expectations. Every corpus fixture needs at least one `// EXPECT DFXnnnn (line,col)` line, or it asserts nothing."),
			*Parameters));
		return false;
	}

	FDiagnosticSink Diagnostics;
	FGenerateResult Result;
	RunPipeline(FilePath, Diagnostics, Result);

	for (FExpectation& Expectation : Expectations)
	{
		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			if (Diagnostic.Code != Expectation.Code)
			{
				continue;
			}
			if (Expectation.Line != INDEX_NONE
				&& (Diagnostic.Location.Line != Expectation.Line || Diagnostic.Location.Column != Expectation.Column))
			{
				continue;
			}
			Expectation.bMatched = true;
			break;
		}

		if (!Expectation.bMatched)
		{
			AddError(FString::Printf(TEXT("%s: expected %s, which was not reported. Diagnostics were:\n%s"),
				*Parameters, *Expectation.Describe(), *FormatDiagnostics(Diagnostics)));
		}
	}

	// A `.bad.*` fixture is a negative case, so a run that produced no error at all means the check it
	// exists for has stopped working even if some warning happened to match.
	if (Parameters.Contains(TEXT(".bad.")) && !Diagnostics.HasErrors())
	{
		AddError(FString::Printf(TEXT("%s is a negative fixture but the pipeline reported no error."), *Parameters));
	}

	return true;
}

// ---------------------------------------------------------------------------------------------
// Generate: the asset topology a source must produce
// ---------------------------------------------------------------------------------------------

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDreamFXGenerateCorpusTest, "DreamFX.Corpus.Generate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

void FDreamFXGenerateCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	using namespace UE::DreamFX::Editor::CorpusTests;

	TArray<FString> Files;
	FindCorpusFiles(TEXT("Generate"), TEXT("*.dfs"), Files);

	for (const FString& File : Files)
	{
		const FString Name = ToTestName(File);
		OutBeautifiedNames.Add(ToDisplayName(Name));
		OutTestCommands.Add(Name);
	}
}

bool FDreamFXGenerateCorpusTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::CorpusTests;

	const FString FilePath = ToFullPath(Parameters);
	const FString GoldenPath = FPaths::ChangeExtension(FilePath, TEXT("topology"));

	FDiagnosticSink Diagnostics;
	FGenerateResult Result;
	RunPipeline(FilePath, Diagnostics, Result);

	if (!Result.bSucceeded || Result.System == nullptr)
	{
		AddError(FString::Printf(TEXT("%s did not build. Diagnostics were:\n%s"),
			*Parameters, *FormatDiagnostics(Diagnostics)));
		return false;
	}

	const FString Actual = DescribeTopology(Result.System);

	FString Expected;
	if (!FFileHelper::LoadFileToString(Expected, *GoldenPath))
	{
		AddError(FString::Printf(
			TEXT("%s has no golden topology at '%s'. Write one with this content:\n%s"),
			*Parameters, *GoldenPath, *Actual));
		return false;
	}

	// Normalised so a checkout that flipped line endings does not read as a topology change.
	Expected.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	FString Normalised = Actual;
	Normalised.ReplaceInline(TEXT("\r\n"), TEXT("\n"));

	if (Normalised != Expected)
	{
		AddError(FString::Printf(TEXT("%s: topology changed.\n--- expected ---\n%s\n--- actual ---\n%s"),
			*Parameters, *Expected, *Normalised));
	}

	return true;
}

// ---------------------------------------------------------------------------------------------
// RoundTrip: decompile -> rebuild -> decompile has to be a fixed point
// ---------------------------------------------------------------------------------------------

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDreamFXRoundTripCorpusTest, "DreamFX.Corpus.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

void FDreamFXRoundTripCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	using namespace UE::DreamFX::Editor::CorpusTests;

	TArray<FString> Files;
	FindCorpusFiles(TEXT("RoundTrip"), TEXT("*.dfs"), Files);

	for (const FString& File : Files)
	{
		const FString Name = ToTestName(File);
		OutBeautifiedNames.Add(ToDisplayName(Name));
		OutTestCommands.Add(Name);
	}
}

bool FDreamFXRoundTripCorpusTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::CorpusTests;

	const FString FilePath = ToFullPath(Parameters);

	FDiagnosticSink BuildDiagnostics;
	FGenerateResult First;
	RunPipeline(FilePath, BuildDiagnostics, First);

	if (!First.bSucceeded || First.System == nullptr)
	{
		AddError(FString::Printf(TEXT("%s did not build. Diagnostics were:\n%s"),
			*Parameters, *FormatDiagnostics(BuildDiagnostics)));
		return false;
	}

	FDiagnosticSink FirstExport;
	const FDecompileResult ExportOne = FDecompiler::Decompile(First.System, TEXT("Plugin.DreamFX"), FirstExport);
	if (!ExportOne.bSucceeded)
	{
		AddError(FString::Printf(TEXT("%s: first decompile failed.\n%s"),
			*Parameters, *FormatDiagnostics(FirstExport)));
		return false;
	}

	// Snapshotted BEFORE the rebuild, not after. The rebuild targets the same asset path the
	// fixture named, so it may legitimately land on the very object First is holding -- rebuilding
	// an existing system in place is the normal case, not an edge one -- and facts read from that
	// object afterwards would be the rebuild's own, compared against themselves.
	TArray<FString> FactsFromFixture;
	DescribeSystemFacts(First.System, FactsFromFixture);

	// The fixture's build is moved out of the way before the rebuild takes its place.
	//
	// Both builds name the same asset, so without this they share a package -- and a rebuild that
	// lands on an existing system leaves the previous run's data interface subobjects parented to
	// it. That is Niagara's own bookkeeping and harmless in the editor, but here it is fatal to the
	// measurement in the worst way: the stale interface still carries the fixture's values, so a
	// tangent the export DROPPED is still found on the rebuilt side and the comparison reports no
	// loss. Measured, not feared -- with the pre-fix exporter restored, the curve fixture passed.
	//
	// Renaming the package rather than deleting anything: the old objects stay alive and valid for
	// as long as this test needs them, simply somewhere the walk will not reach.
	if (UPackage* FixturePackage = First.System->GetOutermost())
	{
		const FName Parked = MakeUniqueObjectName(
			nullptr, UPackage::StaticClass(), FName(*(FixturePackage->GetName() + TEXT("_FixtureBuild"))));
		FixturePackage->Rename(*Parked.ToString(), nullptr,
			REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
	}

	// Rebuild from the export rather than from the fixture: the fixed point being asserted is the
	// decompiler's, and feeding it its own output is the only way to see it.
	FDocument Rebuilt;
	FDiagnosticSink RebuildDiagnostics;
	if (!FParser::ParseText(ExportOne.Source, FilePath, Rebuilt, RebuildDiagnostics))
	{
		AddError(FString::Printf(TEXT("%s: the decompiled source does not parse.\n%s\n--- source ---\n%s"),
			*Parameters, *FormatDiagnostics(RebuildDiagnostics), *ExportOne.Source));
		return false;
	}

	FGenerateOptions Options;
	Options.bSave = false;
	Options.bForce = true;
	const FGenerateResult Second = FGenerator::Generate(Rebuilt, Options, RebuildDiagnostics);

	if (!Second.bSucceeded || Second.System == nullptr)
	{
		AddError(FString::Printf(TEXT("%s: the decompiled source does not rebuild.\n%s"),
			*Parameters, *FormatDiagnostics(RebuildDiagnostics)));
		return false;
	}

	FDiagnosticSink SecondExport;
	const FDecompileResult ExportTwo = FDecompiler::Decompile(Second.System, TEXT("Plugin.DreamFX"), SecondExport);
	if (!ExportTwo.bSucceeded)
	{
		AddError(FString::Printf(TEXT("%s: second decompile failed.\n%s"),
			*Parameters, *FormatDiagnostics(SecondExport)));
		return false;
	}

	if (ExportOne.Source != ExportTwo.Source)
	{
		AddError(FString::Printf(TEXT("%s: decompile is not idempotent.\n%s"),
			*Parameters, *DiffFirstLines(ExportOne.Source, ExportTwo.Source)));
	}

	// The two exports agreeing is necessary and not sufficient, and the difference is the whole
	// reason this second check exists. Both sides of that comparison are the SAME exporter's
	// output, so anything the exporter drops is missing from both and the texts match perfectly
	// while the assets do not. Every curve tangent and every stage binding lost before 2026-08-12
	// was invisible in exactly this way -- symmetric, and therefore silent.
	//
	// The assets are not: First was built from the fixture and holds what the fixture said; Second
	// was built from the export and holds only what the export carried. Comparing them as fact
	// multisets is what turns a symmetric export loss into a failing test, and it is why a fixture
	// here has to SAY the thing it is guarding (write the Break tangent, bind the stage) rather
	// than merely exercise the feature.
	{
		TArray<FString> FactsFromExport;
		DescribeSystemFacts(Second.System, FactsFromExport);

		TMap<FString, int32> Counts;
		for (const FString& Fact : FactsFromFixture)
		{
			Counts.FindOrAdd(Fact)++;
		}
		for (const FString& Fact : FactsFromExport)
		{
			Counts.FindOrAdd(Fact)--;
		}

		TArray<FString> OnlyFirst;
		TArray<FString> OnlySecond;
		for (const TPair<FString, int32>& Entry : Counts)
		{
			for (int32 Copy = 0; Copy < FMath::Abs(Entry.Value); ++Copy)
			{
				(Entry.Value > 0 ? OnlyFirst : OnlySecond).Add(Entry.Key);
			}
		}
		OnlyFirst.Sort();
		OnlySecond.Sort();

		if (OnlyFirst.Num() > 0 || OnlySecond.Num() > 0)
		{
			constexpr int32 MaxReported = 12;
			FString Report;
			for (int32 Index = 0; Index < FMath::Min(OnlyFirst.Num(), MaxReported); ++Index)
			{
				Report += FString::Printf(TEXT("\n  built-from-source only | %s"), *OnlyFirst[Index].Left(300));
			}
			for (int32 Index = 0; Index < FMath::Min(OnlySecond.Num(), MaxReported); ++Index)
			{
				Report += FString::Printf(TEXT("\n  built-from-export only | %s"), *OnlySecond[Index].Left(300));
			}
			AddError(FString::Printf(
				TEXT("%s: the export loses asset state. %d fact(s) only in the asset built from the fixture, %d only in the asset rebuilt from its export.%s"),
				*Parameters, OnlyFirst.Num(), OnlySecond.Num(), *Report));
		}
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
