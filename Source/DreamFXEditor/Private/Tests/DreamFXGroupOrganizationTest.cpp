#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DataHierarchyViewModelBase.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXParser.h"
#include "DreamFXTypes.h"
#include "Generation/DreamFXGenerator.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "ViewModels/HierarchyEditor/NiagaraUserParametersHierarchyViewModel.h"

/**
 * Where [Group=..] / [SortPriority=..] reach the asset.
 *
 * The parser folds Group("Name") { ... } scopes into per-parameter attributes (pinned by
 * DreamFX.Lang.ParameterGroups.GroupScope), and the generator rebuilds the system's user parameter
 * hierarchy from them -- the tree UNiagaraSystemEditorData::UserParameterHierarchy holds, which is
 * what the parameters panel reads in 5.8 (the per-variable CategoryName / EditorSortPriority
 * metadata is deprecated engine-side).
 *
 * Pinned here, against an in-memory generated system (bSave=false, nothing touches disk):
 *   * each group becomes a UHierarchyCategory under the root, named as written;
 *   * group members appear as hierarchy leaves in plan order;
 *   * nested scopes compose into nested categories ("Outer|Inner" shapes the tree the same way);
 *   * loose top-level parameters stay at the root, after the groups.
 *
 * UNiagaraHierarchyUserParameter exposes no API in stock 5.8, so the leaves are inspected through
 * the same reflection the adapter writes them with -- class lookup plus the one UPROPERTY
 * reference -- which keeps the test honest about exactly what the asset holds.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXGroupOrganizationTest,
	"DreamFX.Lang.ParameterGroups.GroupOrganization",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace UE::DreamFX::Editor::GroupOrganizationTests
{
	/** The source every assertion below runs against. */
	static const TCHAR* GroupOrganizationSource = TEXT(R"(
System(Name="Corpus/NS_GroupOrganization", Root="Plugin.DreamFX")
{
    Properties = {
        Group("Surface") {
            float A = 1.0;
            float B = 2.0;
        }
        Group("Look") {
            float C = 3.0;
        }
        float Loose = 4.0;
        Group("Surface") {
            Group("SS") {
                float E = 5.0;
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

	/** Generates in memory; nullptr (with the diagnostics reported) on any failure. */
	static UNiagaraSystem* GenerateSystem(const FString& Source, FDiagnosticSink& Diagnostics)
	{
		FDocument Document;
		if (!FParser::ParseText(Source, TEXT("GroupOrganization.dfs"), Document, Diagnostics))
		{
			return nullptr;
		}

		FGenerateOptions Options;
		Options.bSave = false;
		Options.bForce = true;
		const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		if (!Result.bSucceeded || Result.System == nullptr)
		{
			return nullptr;
		}
		return Result.System;
	}

	/** The user parameter name a hierarchy leaf carries, or empty when it is not one. */
	static FString GetItemParameterName(UHierarchyElement* Element, UClass* ItemType)
	{
		if (Element == nullptr || Element->GetClass() != ItemType)
		{
			return FString();
		}
		if (const FObjectProperty* Reference =
			CastField<FObjectProperty>(ItemType->FindPropertyByName(TEXT("UserParameterScriptVariable"))))
		{
			if (const UNiagaraScriptVariable* ScriptVariable =
				Cast<UNiagaraScriptVariable>(Reference->GetObjectPropertyValue_InContainer(Element)))
			{
				return ScriptVariable->Variable.GetName().ToString();
			}
		}
		return FString();
	}

	/** A readable dump for failure messages: the tree as "[Category] / User.Name" lines. */
	static FString DescribeHierarchy(const UHierarchyRoot* Root, UClass* ItemType)
	{
		FString Out;
		TFunction<void(const UHierarchyElement*, int32)> Walk = [&](const UHierarchyElement* Element, int32 Depth)
		{
			for (const TObjectPtr<UHierarchyElement>& ChildPtr : Element->GetChildren())
			{
				const UHierarchyElement* Child = ChildPtr;
				FString Indent;
				for (int32 Level = 0; Level <= Depth; ++Level)
				{
					Indent += TEXT("  ");
				}
				if (Child == nullptr)
				{
					Out += Indent + TEXT("<null>\n");
					continue;
				}
				if (const UHierarchyCategory* Category = Cast<UHierarchyCategory>(Child))
				{
					Out += FString::Printf(TEXT("%s[Category] %s\n"), *Indent, *Category->GetCategoryName().ToString());
					Walk(Category, Depth + 1);
				}
				else
				{
					Out += FString::Printf(TEXT("%s<%s> %s\n"), *Indent, *Child->GetClass()->GetName(),
						*GetItemParameterName(const_cast<UHierarchyElement*>(Child), ItemType));
				}
			}
		};
		Walk(Root, 0);
		return Out;
	}

	/** First child at Index, checked; nullptr (error reported) on shape mismatch. */
	static UHierarchyElement* GetChild(FAutomationTestBase& Test, UHierarchyElement* Parent, int32 Index, int32 ExpectedCount)
	{
		const TArray<TObjectPtr<UHierarchyElement>>& Children = Parent->GetChildrenMutable();
		if (!Test.TestEqual(TEXT("child count"), Children.Num(), ExpectedCount))
		{
			return nullptr;
		}
		return Children[Index];
	}
}

bool FDreamFXGroupOrganizationTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor::GroupOrganizationTests;

	UClass* ItemType = FindFirstObjectSafe<UClass>(TEXT("NiagaraHierarchyUserParameter"));
	if (ItemType == nullptr)
	{
		AddError(TEXT("The engine has no NiagaraHierarchyUserParameter class; the hierarchy cannot be inspected."));
		return false;
	}

	FDiagnosticSink Diagnostics;
	UNiagaraSystem* System = GenerateSystem(GroupOrganizationSource, Diagnostics);
	if (System == nullptr)
	{
		AddError(FString::Printf(TEXT("Group-organization source failed to generate:\n%s"), *Diagnostics.FormatAll()));
		return false;
	}

	UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData());
	if (EditorData == nullptr || EditorData->UserParameterHierarchy == nullptr)
	{
		AddError(TEXT("The generated system carries no user parameter hierarchy."));
		return false;
	}
	UHierarchyRoot* Root = EditorData->UserParameterHierarchy;

	// -- top level: two groups in first-appearance order, then the loose parameter --------------

	if (UHierarchyElement* SurfaceElement = GetChild(*this, Root, 0, 3))
	{
		if (const UHierarchyCategory* Surface = Cast<UHierarchyCategory>(SurfaceElement))
		{
			TestEqual(TEXT("first group is 'Surface'"), Surface->GetCategoryName().ToString(), FString(TEXT("Surface")));
		}
		else
		{
			AddError(FString::Printf(TEXT("Expected a category at index 0, found:\n%s"), *DescribeHierarchy(Root, ItemType)));
		}
	}

	if (UHierarchyElement* LookElement = GetChild(*this, Root, 1, 3))
	{
		if (const UHierarchyCategory* Look = Cast<UHierarchyCategory>(LookElement))
		{
			TestEqual(TEXT("second group is 'Look'"), Look->GetCategoryName().ToString(), FString(TEXT("Look")));
		}
		else
		{
			AddError(FString::Printf(TEXT("Expected a category at index 1, found:\n%s"), *DescribeHierarchy(Root, ItemType)));
		}
	}

	if (UHierarchyElement* LooseElement = GetChild(*this, Root, 2, 3))
	{
		TestEqual(TEXT("loose parameter stays at the root"), GetItemParameterName(LooseElement, ItemType),
			FString(TEXT("User.Loose")));
	}

	// -- group members, in plan order -----------------------------------------------------------

	if (UHierarchyElement* SurfaceElement = Root->GetChildrenMutable()[0])
	{
		if (UHierarchyElement* ItemA = GetChild(*this, SurfaceElement, 0, 3))
		{
			TestEqual(TEXT("'Surface' holds User.A first"), GetItemParameterName(ItemA, ItemType),
				FString(TEXT("User.A")));
		}
		if (UHierarchyElement* ItemB = GetChild(*this, SurfaceElement, 1, 3))
		{
			TestEqual(TEXT("'Surface' holds User.B second"), GetItemParameterName(ItemB, ItemType),
				FString(TEXT("User.B")));
		}
		// The second "Surface" block composes "Surface|SS" as a nested category.
		if (UHierarchyElement* NestedElement = GetChild(*this, SurfaceElement, 2, 3))
		{
			if (const UHierarchyCategory* Nested = Cast<UHierarchyCategory>(NestedElement))
			{
				TestEqual(TEXT("nested scope is a child category 'SS'"), Nested->GetCategoryName().ToString(),
					FString(TEXT("SS")));
				if (UHierarchyElement* ItemE = GetChild(*this, NestedElement, 0, 1))
				{
					TestEqual(TEXT("'Surface|SS' holds User.E"), GetItemParameterName(ItemE, ItemType),
						FString(TEXT("User.E")));
				}
			}
			else
			{
				AddError(FString::Printf(TEXT("Expected a nested category under 'Surface', found:\n%s"), *DescribeHierarchy(Root, ItemType)));
			}
		}
	}

	// Idempotence note: each Generate creates a fresh system, so duplication cannot be asserted
	// here -- it is the real build path (the same asset object reused across rebuilds) that
	// exercises it, and the full-tree rebuild (EmptyAllData + re-add) is what keeps that honest.

	return true;
}

#endif
