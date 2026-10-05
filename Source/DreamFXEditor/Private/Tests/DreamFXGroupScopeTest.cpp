#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DreamFXDiagnostics.h"
#include "DreamFXParser.h"
#include "DreamFXTypes.h"

/**
 * Group("Name") { ... } parameter scopes (DreamShader parity).
 *
 * The syntax folds into plain attributes at parse time -- every parameter inside a scope inherits
 * the composed group name and an auto-incrementing SortPriority -- so these assertions read the
 * FDocument directly. Nothing here generates an asset: groups never reach one (DFX5099), which is
 * exactly why the folding has to be pinned by a test rather than by generator output.
 *
 * Semantics pinned here (each mirroring DreamShader's StampGroupedProperty):
 *   * the auto-sort counter is shared across the whole block, one continuous 10, 20, 30 ...;
 *   * an explicit Group or SortPriority on the parameter wins and does not consume an auto slot;
 *   * loose top-level parameters carry no group and no auto-sort;
 *   * nested Group("Outer") { Group("Inner") { ... } } composes into "Outer|Inner".
 *
 * The diagnostics have their own codes -- DFX2027 (name must be a quoted string), DFX2028 (name
 * must be non-empty), DFX2029 (a bare '{' in a parameter block) -- and are asserted in the second
 * test so a regression reports which contract broke.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXGroupScopeTest,
	"DreamFX.Lang.ParameterGroups.GroupScope",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXGroupScopeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;

	const FString Source = TEXT(R"(
System(Name="Corpus/NS_GroupScope", Root="Game")
{
    Properties = {
        Group("Surface") {
            float A = 1.0;
            float B = 2.0 [ Group="Manual"; ];
            float C = 3.0 [ SortPriority=7; ];
        }
        float Loose = 4.0;
        Group("Detail") {
            float D = 5.0;
        }
        Group("Surface") {
            Group("SS") {
                float E = 6.0;
            }
        }
    }

    Emitter Sparks
    {
        EmitterUpdate = { EmitterState(); SpawnRate(SpawnRate = 10.0); }
        ParticleSpawn = { SystemLocation(); }
        ParticleUpdate = { ParticleState(); SolveForcesAndVelocity(); }
        SpriteRenderer Core { }
    }
}
)");

	FDiagnosticSink Diagnostics;
	FDocument Document;
	if (!FParser::ParseText(Source, TEXT("GroupScope.dfs"), Document, Diagnostics))
	{
		FString Reported;
		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			Reported += FString::Printf(TEXT("  %s (%d,%d): %s\n"), *Diagnostic.Code,
				Diagnostic.Location.Line, Diagnostic.Location.Column, *Diagnostic.Message);
		}
		AddError(FString::Printf(TEXT("Group-scope source failed to parse:\n%s"), *Reported));
		return false;
	}

	// The two attributes the group machinery owns.
	auto FindGroup = [&Document](const TCHAR* Name) -> const FAttribute*
	{
		const FParameterDecl* Parameter = Document.Parameters.FindByPredicate(
			[Name](const FParameterDecl& Candidate) { return Candidate.Name == Name; });
		return Parameter ? Parameter->FindAttribute(TEXT("Group")) : nullptr;
	};
	auto FindSort = [&Document](const TCHAR* Name) -> const FAttribute*
	{
		const FParameterDecl* Parameter = Document.Parameters.FindByPredicate(
			[Name](const FParameterDecl& Candidate) { return Candidate.Name == Name; });
		return Parameter ? Parameter->FindAttribute(TEXT("SortPriority")) : nullptr;
	};
	auto HasParameter = [&Document](const TCHAR* Name) -> bool
	{
		return Document.Parameters.ContainsByPredicate(
			[Name](const FParameterDecl& Candidate) { return Candidate.Name == Name; });
	};

	if (!HasParameter(TEXT("A")) || !HasParameter(TEXT("B")) || !HasParameter(TEXT("C"))
		|| !HasParameter(TEXT("D")) || !HasParameter(TEXT("E")) || !HasParameter(TEXT("Loose")))
	{
		AddError(FString::Printf(TEXT("A grouped source must keep every parameter. Parsed %d."), Document.Parameters.Num()));
		return false;
	}

	// -- group stamping ------------------------------------------------------------------------

	if (const FAttribute* Group = FindGroup(TEXT("A")))
	{
		TestEqual(TEXT("A inherits group 'Surface'"), Group->Value->Text, FString(TEXT("Surface")));
	}
	else
	{
		AddError(TEXT("A carries no Group attribute."));
	}

	if (const FAttribute* Group = FindGroup(TEXT("B")))
	{
		// An explicit Group on the parameter wins over the scope it sits in.
		TestEqual(TEXT("B keeps its explicit group 'Manual'"), Group->Value->Text, FString(TEXT("Manual")));
	}
	else
	{
		AddError(TEXT("B carries no Group attribute."));
	}

	if (const FAttribute* Group = FindGroup(TEXT("E")))
	{
		// Nested scopes compose with '|', Unreal's native sub-category syntax.
		TestEqual(TEXT("E composes nested groups into 'Surface|SS'"), Group->Value->Text, FString(TEXT("Surface|SS")));
	}
	else
	{
		AddError(TEXT("E carries no Group attribute."));
	}

	TestFalse(TEXT("Loose keeps no group"), FindGroup(TEXT("Loose")) != nullptr);

	// -- auto-sort -----------------------------------------------------------------------------

	if (const FAttribute* Sort = FindSort(TEXT("A")))
	{
		TestEqual(TEXT("A takes the first auto slot (10)"), Sort->Value->Number, 10.0);
	}
	else
	{
		AddError(TEXT("A carries no SortPriority attribute."));
	}

	if (const FAttribute* Sort = FindSort(TEXT("B")))
	{
		// B wrote no SortPriority, so it takes the next auto slot even though its Group is explicit.
		TestEqual(TEXT("B takes the next auto slot (20)"), Sort->Value->Number, 20.0);
	}
	else
	{
		AddError(TEXT("B carries no SortPriority attribute."));
	}

	if (const FAttribute* Sort = FindSort(TEXT("C")))
	{
		// Explicit value wins...
		TestEqual(TEXT("C keeps its explicit SortPriority (7)"), Sort->Value->Number, 7.0);
	}
	else
	{
		AddError(TEXT("C carries no SortPriority attribute."));
	}

	if (const FAttribute* Sort = FindSort(TEXT("D")))
	{
		// The counter is global to the block, so the next group continues at 30.
		TestEqual(TEXT("D continues the shared counter (30)"), Sort->Value->Number, 30.0);
	}
	else
	{
		AddError(TEXT("D carries no SortPriority attribute."));
	}

	if (const FAttribute* Sort = FindSort(TEXT("E")))
	{
		TestEqual(TEXT("E continues the shared counter (40)"), Sort->Value->Number, 40.0);
	}
	else
	{
		AddError(TEXT("E carries no SortPriority attribute."));
	}

	TestFalse(TEXT("Loose carries no auto-sort"), FindSort(TEXT("Loose")) != nullptr);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXGroupScopeErrorsTest,
	"DreamFX.Lang.ParameterGroups.GroupScopeErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXGroupScopeErrorsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;

	// Each case: the source fragment inside Properties, and the code it must provoke.
	const TArray<TPair<FString, FString>> Cases =
	{
		// The name must be a quoted string; a bare identifier is the classic typo.
		{ TEXT("Group(Alpha) { float A = 1.0; }"), TEXT("DFX2027") },
		// The name must be non-empty.
		{ TEXT("Group(\"\") { float A = 1.0; }"), TEXT("DFX2028") },
		// No other form may open a brace inside a parameter block.
		{ TEXT("{ float A = 1.0; }"), TEXT("DFX2029") },
	};

	for (const TPair<FString, FString>& Case : Cases)
	{
		const FString Source = FString::Printf(TEXT(R"(
System(Name="Corpus/NS_GroupScopeError", Root="Game")
{
    Properties = {
        %s
    }

    Emitter Sparks
    {
        EmitterUpdate = { EmitterState(); }
        SpriteRenderer Core { }
    }
}
)"), *Case.Key);

		FDiagnosticSink Diagnostics;
		FDocument Document;
		FParser::ParseText(Source, TEXT("GroupScopeError.dfs"), Document, Diagnostics);

		bool bReported = false;
		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			if (Diagnostic.Code == Case.Value)
			{
				bReported = true;
				break;
			}
		}
		if (!bReported)
		{
			AddError(FString::Printf(TEXT("'%s' must report %s."), *Case.Key, *Case.Value));
		}
		if (!Diagnostics.HasErrors())
		{
			AddError(FString::Printf(TEXT("'%s' is a negative case but the parser reported no error."), *Case.Key));
		}
	}

	return true;
}

#endif
