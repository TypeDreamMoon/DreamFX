#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DreamFXParser.h"
#include "EdGraph/EdGraphPin.h"
#include "Generation/DreamFXEmitterMerge.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "Generation/DreamFXProvenance.h"
#include "HAL/FileManager.h"
#include "Lint/DreamFXLint.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UnrealType.h"

namespace UE::DreamFX::Editor::GenerationRegressionTests
{
	static FString UniqueName(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("DreamFXAutomation/%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	static bool Parse(FAutomationTestBase& Test, const FString& Source, const FString& File, FDocument& Document)
	{
		FDiagnosticSink Diagnostics;
		if (!FParser::ParseText(Source, File, Document, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll());
			return false;
		}
		return true;
	}

	static FGenerateResult Build(FAutomationTestBase& Test, const FDocument& Document, bool bVerify = false,
		bool bExpectSuccess = true)
	{
		FGenerateOptions Options;
		Options.bSave = false;
		Options.bVerifyOnly = bVerify;
		FDiagnosticSink Diagnostics;
		FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		if (Result.bSucceeded != bExpectSuccess)
		{
			Test.AddError(FString::Printf(TEXT("Unexpected generation result:\n%s"), *Diagnostics.FormatAll()));
		}
		return Result;
	}

	static const TCHAR* Stacks = TEXT(R"(
        EmitterUpdate = { EmitterState(); SpawnRate(SpawnRate=1.0); }
        ParticleSpawn = { SystemLocation(); }
        ParticleUpdate = { ParticleState(); SolveForcesAndVelocity(); }
        SpriteRenderer Core {}
)");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXEmitterOverrideRegression,
	"DreamFX.Regression.Generation.EmitterOverrides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXEmitterOverrideRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::GenerationRegressionTests;
	FDocument Base, Host;
	if (!Parse(*this, TEXT(R"(Emitter(Name="Base",Root="Game") {
        Stage A = {} Stage B = {}
        Defaults = { float Particles.Size = 1.0; float Particles.Keep = 3.0; }
    })"), TEXT("Base.dfe"), Base)
		|| !Parse(*this, TEXT(R"(System(Name="Host",Root="Game") { Emitter E from "Base" {
        Stage B(NumIterations=4) = {} Stage C = {}
        Defaults = { float Particles.Size = 2.0; int Particles.New = 7; }
    } })"), TEXT("Host.dfs"), Host)) { return false; }
	FEmitter Merged;
	FDiagnosticSink Diagnostics;
	if (!TestTrue(TEXT("merge succeeds"), MergeEmitterDefinitions(Base.EmitterDefinition, Host.Emitters[0], Merged, Diagnostics)))
	{
		AddError(Diagnostics.FormatAll()); return false;
	}
	TestEqual(TEXT("all named stages survive"), Merged.Stacks.Num(), 3);
	if (Merged.Stacks.Num() == 3)
	{
		TestEqual(TEXT("A stays first"), Merged.Stacks[0].Stage.Name, FString(TEXT("A")));
		TestEqual(TEXT("B stays second"), Merged.Stacks[1].Stage.Name, FString(TEXT("B")));
		TestEqual(TEXT("B override applied"), Merged.Stacks[1].Stage.NumIterations.GetValue(), 4);
		TestEqual(TEXT("C appended"), Merged.Stacks[2].Stage.Name, FString(TEXT("C")));
	}
	TestEqual(TEXT("default override keeps untouched and new entries"), Merged.Defaults.Num(), 3);
	if (Merged.Defaults.Num() == 3)
	{
		TestEqual(TEXT("overridden default"), Merged.Defaults[0].Value->Number, 2.0);
		TestEqual(TEXT("untouched default"), Merged.Defaults[1].Value->Number, 3.0);
		TestEqual(TEXT("new default"), Merged.Defaults[2].Value->Number, 7.0);
	}
	FEmitter EmptyBase;
	TestTrue(TEXT("new stages merge into empty base"), MergeEmitterDefinitions(EmptyBase, Host.Emitters[0], Merged, Diagnostics));
	TestEqual(TEXT("two new stages are not collapsed"), Merged.Stacks.Num(), 2);
	Host.Emitters[0].Defaults[0].TypeName = TEXT("int");
	Diagnostics.Reset();
	TestFalse(TEXT("default override cannot reinterpret base parameter type"),
		MergeEmitterDefinitions(Base.EmitterDefinition, Host.Emitters[0], Merged, Diagnostics));
	TestTrue(TEXT("type conflict identifies default override"), Diagnostics.FormatAll().Contains(TEXT("DFX3048")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSourceFingerprintRegression,
	"DreamFX.Regression.Generation.SourceFingerprint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSourceFingerprintRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	const FString HostHash = HashSourceText(TEXT("host"));
	TMap<FString, FString> First, Reordered;
	First.Add(TEXT("BaseA"), HashSourceText(TEXT("a")));
	First.Add(TEXT("BaseB"), HashSourceText(TEXT("b")));
	Reordered.Add(TEXT("BaseB"), First[TEXT("BaseB")]);
	Reordered.Add(TEXT("BaseA"), First[TEXT("BaseA")]);
	const FString Initial = FProvenance::HashWithSourceDependencies(HostHash, First);
	TestEqual(TEXT("discovery order does not affect stamp"), Initial, FProvenance::HashWithSourceDependencies(HostHash, Reordered));
	Reordered[TEXT("BaseA")] = HashSourceText(TEXT("changed"));
	TestNotEqual(TEXT("dependency change invalidates unchanged host"), Initial, FProvenance::HashWithSourceDependencies(HostHash, Reordered));
	Reordered[TEXT("BaseA")] = First[TEXT("BaseB")];
	Reordered[TEXT("BaseB")] = First[TEXT("BaseA")];
	TestNotEqual(TEXT("dependency identities matter, not just a set of content hashes"), Initial,
		FProvenance::HashWithSourceDependencies(HostHash, Reordered));
	TestEqual(TEXT("self-contained source keeps its existing hash format"), HostHash,
		FProvenance::HashWithSourceDependencies(HostHash, {}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicUsageRegression,
	"DreamFX.Regression.Generation.DynamicInputUsage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicUsageRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::GenerationRegressionTests;
	for (const TCHAR* Usage : { TEXT("[SystemSpawn, SystemUpdate]"), TEXT("DynamicInput"),
		TEXT("[]"), TEXT("[SystemSpawn, Typo]"), TEXT("SystemSpawn") })
	{
		FDocument Document;
		if (!Parse(*this, FString::Printf(TEXT("DynamicInput(Name=\"%s\",Root=\"Game\") { Settings={Usage=%s;Output=float;} Body={return 1.0;} }"),
			*UniqueName(TEXT("DI_Usage")), Usage), TEXT("Usage.dfm"), Document)) { return false; }
		FDiagnosticSink Diagnostics;
		FLint::Run(Document, Diagnostics);
		const bool bValid = FString(Usage) == TEXT("[SystemSpawn, SystemUpdate]") || FString(Usage) == TEXT("DynamicInput");
		TestEqual(FString::Printf(TEXT("Usage %s validity"), Usage), !Diagnostics.HasErrors(), bValid);
		if (bValid && Document.FindSetting(TEXT("Usage"))->Value->Kind == EValueKind::Array)
		{
			FGenerateOptions Options; Options.bSave = false;
			const FModuleGenerateResult Result = FModuleGenerator::Generate(Document, Options, Diagnostics);
			if (!TestTrue(TEXT("array usage reaches generation"), Result.bSucceeded) || Result.Script == nullptr)
			{
				AddError(Diagnostics.FormatAll()); return false;
			}
			const int32 Expected = (1 << static_cast<int32>(ENiagaraScriptUsage::SystemSpawnScript))
				| (1 << static_cast<int32>(ENiagaraScriptUsage::SystemUpdateScript));
			TestEqual(TEXT("script advertises requested system stacks"), Result.Script->GetLatestScriptData()->ModuleUsageBitmask, Expected);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSwizzleWriteRegression,
	"DreamFX.Regression.Generation.SwizzleWrite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSwizzleWriteRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::GenerationRegressionTests;
	FDocument Document;
	if (!Parse(*this, FString::Printf(TEXT("Module(Name=\"%s\",Root=\"Game\") { Settings={Usage=ParticleUpdate;} Body={Particles.Color.rgb=float3(1.0,0.0,0.0);} }"),
		*UniqueName(TEXT("M_Swizzle"))), TEXT("Swizzle.dfm"), Document)) { return false; }
	FGenerateOptions Options; Options.bSave = false;
	FDiagnosticSink Diagnostics;
	const FModuleGenerateResult Result = FModuleGenerator::Generate(Document, Options, Diagnostics);
	if (!TestTrue(TEXT("swizzle module generates"), Result.bSucceeded) || !Result.Script)
	{
		AddError(Diagnostics.FormatAll()); return false;
	}
	const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Result.Script->GetLatestSource());
	if (!TestNotNull(TEXT("generated source"), Source) || !TestNotNull(TEXT("generated graph"), Source->NodeGraph.Get())) { return false; }
	bool bFound = false;
	for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
	{
		if (const UNiagaraNodeCustomHlsl* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Node))
		{
			bFound = true;
			bool bRead = false, bWrite = false, bWriteConnected = false;
			for (const UEdGraphPin* Pin : Hlsl->Pins)
			{
				bRead |= Pin->Direction == EGPD_Input && Pin->PinName == TEXT("Read_Particles_Color");
				if (Pin->Direction == EGPD_Output && Pin->PinName == TEXT("Write_Particles_Color"))
				{
					bWrite = true;
					bWriteConnected = !Pin->LinkedTo.IsEmpty();
				}
			}
			TestTrue(TEXT("partial write reads original alpha"), bRead);
			TestTrue(TEXT("partial write has full-attribute output"), bWrite);
			TestTrue(TEXT("output is connected to parameter map write"), bWriteConnected);
			const FStrProperty* Property = CastField<FStrProperty>(Hlsl->GetClass()->FindPropertyByName(TEXT("CustomHlsl")));
			if (TestNotNull(TEXT("HLSL reflection field"), Property))
			{
				const FString Body = Property->GetPropertyValue_InContainer(Hlsl);
				TestTrue(TEXT("unmodified components seeded before swizzle write"),
					Body.Contains(TEXT("Write_Particles_Color = Read_Particles_Color;")));
				TestTrue(TEXT("body updates output swizzle"), Body.Contains(TEXT("Write_Particles_Color.rgb=")));
			}
		}
	}
	TestTrue(TEXT("generated custom node inspected"), bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXRebuildSettingsRegression,
	"DreamFX.Regression.Generation.RemovedSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXRebuildSettingsRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::GenerationRegressionTests;
	const FString Name = UniqueName(TEXT("NS_Settings"));
	const FString WithSettings = FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { Settings={WarmupTime=0.25;FixedTickDelta=true;FixedTickDeltaTime=0.02;} Emitter E { Settings={LocalSpace=true;Determinism=true;RandomSeed=17;AllocationMode=Fixed;PreAllocationCount=32;} %s } }"), *Name, Stacks);
	const FString WithoutSettings = FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { Emitter E { %s } }"), *Name, Stacks);
	FDocument Initial, Removed, Fresh;
	if (!Parse(*this, WithSettings, TEXT("Settings.dfs"), Initial)
		|| !Parse(*this, WithoutSettings, TEXT("Settings.dfs"), Removed)
		|| !Parse(*this, WithoutSettings.Replace(*Name, *UniqueName(TEXT("NS_Fresh"))), TEXT("Fresh.dfs"), Fresh)) { return false; }
	const FGenerateResult Before = Build(*this, Initial);
	if (!Before.bSucceeded || !Before.System) { return false; }
	FGCObjectScopeGuard Guard(Before.System);
	const FNiagaraEmitterHandle& InitialHandle = Before.System->GetEmitterHandles()[0];
	const FGuid HandleId = InitialHandle.GetId();
	TestTrue(TEXT("initial local-space value applied"), InitialHandle.GetEmitterData()->bLocalSpace);
	const FGenerateResult Rebuilt = Build(*this, Removed);
	const FGenerateResult New = Build(*this, Fresh);
	if (!Rebuilt.bSucceeded || !New.bSucceeded || !New.System) { return false; }
	TestEqual(TEXT("same system object reused"), Rebuilt.System, Before.System);
	TestEqual(TEXT("emitter identity preserved"), Rebuilt.System->GetEmitterHandles()[0].GetId(), HandleId);
	const FVersionedNiagaraEmitterData* Actual = Rebuilt.System->GetEmitterHandles()[0].GetEmitterData();
	const FVersionedNiagaraEmitterData* Expected = New.System->GetEmitterHandles()[0].GetEmitterData();
	TestEqual(TEXT("removed LocalSpace matches fresh build"), bool(Actual->bLocalSpace), bool(Expected->bLocalSpace));
	TestEqual(TEXT("removed Determinism matches fresh build"), bool(Actual->bDeterminism), bool(Expected->bDeterminism));
	TestEqual(TEXT("removed RandomSeed matches fresh build"), Actual->RandomSeed, Expected->RandomSeed);
	TestEqual(TEXT("removed allocation count matches fresh build"), Actual->PreAllocationCount, Expected->PreAllocationCount);
	TestEqual(TEXT("removed allocation mode matches fresh build"), Actual->AllocationMode, Expected->AllocationMode);
	TestEqual(TEXT("removed warmup matches fresh build"), Rebuilt.System->GetWarmupTime(), New.System->GetWarmupTime());
	TestEqual(TEXT("removed fixed tick interval matches fresh build"), Rebuilt.System->GetFixedTickDeltaTime(), New.System->GetFixedTickDeltaTime());
	const FBoolProperty* FixedTick = CastField<FBoolProperty>(UNiagaraSystem::StaticClass()->FindPropertyByName(TEXT("bFixedTickDelta")));
	if (TestNotNull(TEXT("fixed tick flag"), FixedTick))
	{
		TestEqual(TEXT("removed fixed tick flag matches fresh build"), FixedTick->GetPropertyValue_InContainer(Rebuilt.System), FixedTick->GetPropertyValue_InContainer(New.System));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDependencyRebuildRegression,
	"DreamFX.Regression.Generation.DependencyRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDependencyRebuildRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::GenerationRegressionTests;
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DreamFXAutomationTemp") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString BaseFile = Directory / TEXT("Base.dfe");
	IFileManager::Get().MakeDirectory(*Directory, true);
	ON_SCOPE_EXIT
	{
		// This test owns exactly this GUID directory and its single source file.
		IFileManager::Get().Delete(*BaseFile);
		IFileManager::Get().DeleteDirectory(*Directory, false, false);
	};
	auto WriteBase = [&](bool bLocal)
	{
		return FFileHelper::SaveStringToFile(FString::Printf(TEXT("Emitter(Name=\"Base\",Root=\"Game\") { Settings={LocalSpace=%s;} %s }"), bLocal ? TEXT("true") : TEXT("false"), Stacks),
			*BaseFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	if (!TestTrue(TEXT("write referenced source"), WriteBase(false))) { return false; }
	FDocument Host;
	if (!Parse(*this, FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { Emitter E from \"Base\" {} }"),
		*UniqueName(TEXT("NS_Dependency"))), Directory / TEXT("Host.dfs"), Host)) { return false; }
	const FGenerateResult Initial = Build(*this, Host);
	if (!Initial.bSucceeded || !Initial.System) { return false; }
	FGCObjectScopeGuard Guard(Initial.System);
	TestTrue(TEXT("unchanged source is skipped"), Build(*this, Host).bSkipped);
	TestTrue(TEXT("unchanged source verifies"), Build(*this, Host, true).bSucceeded);
	if (!TestTrue(TEXT("edit dependency only"), WriteBase(true))) { return false; }
	const FGenerateResult Verification = Build(*this, Host, true, false);
	TestTrue(TEXT("dependency-only edit reports drift"), Verification.bDrifted);
	TestFalse(TEXT("verify did not apply dependency edit"), Initial.System->GetEmitterHandles()[0].GetEmitterData()->bLocalSpace);
	const FGenerateResult Rebuilt = Build(*this, Host);
	TestFalse(TEXT("dependency-only edit is not skipped"), Rebuilt.bSkipped);
	if (!Rebuilt.bSucceeded || !Rebuilt.System) { return false; }
	TestTrue(TEXT("ordinary rebuild applies dependency edit"), Rebuilt.System->GetEmitterHandles()[0].GetEmitterData()->bLocalSpace);
	TestTrue(TEXT("rebuilt dependency verifies"), Build(*this, Host, true).bSucceeded);
	return true;
}

#endif
