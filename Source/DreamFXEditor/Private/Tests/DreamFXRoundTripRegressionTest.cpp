#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"

namespace UE::DreamFX::Editor::RoundTripRegression
{
	void ReportErrors(FAutomationTestBase& Test, const FDiagnosticSink& Diagnostics)
	{
		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			Test.AddError(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
		}
	}

	UNiagaraSystem* Build(FAutomationTestBase& Test, const FString& Source)
	{
		FDocument Document;
		FDiagnosticSink Diagnostics;
		if (!FParser::ParseText(Source, TEXT("RoundTripRegression.dfs"), Document, Diagnostics))
		{
			ReportErrors(Test, Diagnostics);
			return nullptr;
		}
		Document.Name = TEXT("DreamFXRegression/NS_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Document.Root = TEXT("Game");
		FGenerateOptions Options;
		Options.bSave = false;
		Options.bForce = true;
		const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		if (!Result.bSucceeded)
		{
			ReportErrors(Test, Diagnostics);
			return nullptr;
		}
		return Result.System;
	}

	FDecompileResult Export(FAutomationTestBase& Test, UNiagaraSystem* System)
	{
		FDiagnosticSink Diagnostics;
		FDecompileResult Result = FDecompiler::Decompile(System, TEXT("Game"), Diagnostics);
		if (!Result.bSucceeded) { ReportErrors(Test, Diagnostics); }
		Test.TestEqual(TEXT("supported regression asset has no export gaps"), Result.UnsupportedFeatures.Num(), 0);
		return Result;
	}

	UNiagaraSpriteRendererProperties* Sprite(UNiagaraSystem* System)
	{
		if (System == nullptr || System->GetEmitterHandles().Num() != 1) { return nullptr; }
		const FVersionedNiagaraEmitterData* Data = System->GetEmitterHandles()[0].GetEmitterData();
		return Data != nullptr && Data->GetRenderers().Num() == 1
			? Cast<UNiagaraSpriteRendererProperties>(Data->GetRenderers()[0]) : nullptr;
	}

	UNiagaraSimulationStageGeneric* Stage(UNiagaraSystem* System)
	{
		if (System == nullptr || System->GetEmitterHandles().Num() != 1) { return nullptr; }
		const FVersionedNiagaraEmitterData* Data = System->GetEmitterHandles()[0].GetEmitterData();
		return Data != nullptr && Data->GetSimulationStages().Num() == 1
			? Cast<UNiagaraSimulationStageGeneric>(Data->GetSimulationStages()[0]) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDisabledAssignmentRoundTripTest,
	"DreamFX.Regression.DisabledAssignmentRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDisabledAssignmentRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	const FString Source = TEXT(R"(
System(Name="Unused", Root="Game") {
    Emitter Motes {
        ParticleSpawn = {
            Color Particles.Color = (1, 1, 1, 1);
            disabled Color Particles.Color = (1, 0, 0, 1);
            float Particles.RegressionValue = 2.0;
        }
    }
})");
	UNiagaraSystem* Original = Build(*this, Source);
	if (!TestNotNull(TEXT("original"), Original)) { return false; }
	FGCObjectScopeGuard OriginalGuard(Original);
	auto CheckEnabledRuns = [this](UNiagaraSystem* System)
	{
		FScriptStackInfo Stack;
		TArray<FString> Errors;
		if (!TestTrue(TEXT("read assignment stack"), FNiagaraAdapter::GetScriptStackInfo(
			FStackAddress(System).WithEmitter(TEXT("Motes")).WithScript(TEXT("ParticleSpawnScript")), Stack, Errors)))
		{
			AddError(FString::Join(Errors, TEXT(" | ")));
			return;
		}
		if (TestEqual(TEXT("enabled changes split the assignment run"), Stack.Modules.Num(), 3))
		{
			TestTrue(TEXT("first run enabled"), Stack.Modules[0].bEnabled);
			TestFalse(TEXT("second run disabled"), Stack.Modules[1].bEnabled);
			TestTrue(TEXT("third run enabled"), Stack.Modules[2].bEnabled);
			for (const FModuleInfo& Module : Stack.Modules)
			{
				TestTrue(TEXT("assignment remains a Set Parameters node"), Module.bIsSetParameters);
			}
		}
	};
	CheckEnabledRuns(Original);
	const FDecompileResult Exported = Export(*this, Original);
	TestTrue(TEXT("disabled typed assignment survives export"), Exported.Source.Contains(TEXT("disabled Color Particles.Color =")));
	UNiagaraSystem* Mirror = Build(*this, Exported.Source);
	if (!TestNotNull(TEXT("mirror"), Mirror)) { return false; }
	FGCObjectScopeGuard MirrorGuard(Mirror);
	CheckEnabledRuns(Mirror);

	FDocument BadDefaults;
	FDiagnosticSink Diagnostics;
	TestFalse(TEXT("defaults cannot have an execution state"), FParser::ParseText(
		TEXT("System(Name=\"Unused\") { Emitter Motes { Defaults = { disabled float Particles.Value = 1.0; } } }"),
		TEXT("DisabledDefaults.dfs"), BadDefaults, Diagnostics));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXMaterialBindingRoundTripTest,
	"DreamFX.Regression.MaterialBindingRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXMaterialBindingRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	UNiagaraSystem* Original = Build(*this, TEXT(R"(
System(Name="Unused", Root="Game") {
    Properties = { Object<MaterialInterface> Material; }
    Emitter Motes { SpriteRenderer Core { } }
})"));
	if (!TestNotNull(TEXT("original"), Original)) { return false; }
	FGCObjectScopeGuard OriginalGuard(Original);
	UNiagaraSpriteRendererProperties* OriginalSprite = Sprite(Original);
	if (!TestNotNull(TEXT("sprite renderer"), OriginalSprite)) { return false; }
	// Author the engine property directly so the test starts independently of our exporter.
	OriginalSprite->MaterialUserParamBinding.Parameter = FNiagaraVariable(
		FNiagaraTypeDefinition(UMaterialInterface::StaticClass()), TEXT("User.Material"));
	const FDecompileResult Exported = Export(*this, Original);
	TestTrue(TEXT("non-attribute binding is represented"), Exported.Source.Contains(TEXT("MaterialUserParamBinding =")));
	TestFalse(TEXT("binding omits the process-local type handle"), Exported.Source.Contains(TEXT("typeDefHandle")));
	TestFalse(TEXT("binding omits the process-local registry index"), Exported.Source.Contains(TEXT("registeredTypeIndex")));
	UNiagaraSystem* Mirror = Build(*this, Exported.Source);
	if (!TestNotNull(TEXT("mirror"), Mirror)) { return false; }
	FGCObjectScopeGuard MirrorGuard(Mirror);
	UNiagaraSpriteRendererProperties* MirrorSprite = Sprite(Mirror);
	if (TestNotNull(TEXT("rebuilt sprite renderer"), MirrorSprite))
	{
		TestEqual(TEXT("runtime material selection binding survives"),
			MirrorSprite->MaterialUserParamBinding.Parameter.GetName(), FName(TEXT("User.Material")));
		TestTrue(TEXT("material binding retains its renderer-defined type"),
			MirrorSprite->MaterialUserParamBinding.Parameter.GetType() == FNiagaraTypeDefinition::GetUMaterialDef());
		TArray<FString> ExportedLines;
		Exported.Source.ParseIntoArrayLines(ExportedLines);
		const FString* BindingLine = ExportedLines.FindByPredicate([](const FString& Line)
			{ return Line.Contains(TEXT("MaterialUserParamBinding =")); });
		const FDecompileResult Reexported = Export(*this, Mirror);
		TestTrue(TEXT("material binding re-exports identically"),
			BindingLine != nullptr && Reexported.Source.Contains(*BindingLine));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXEmitterEnabledRoundTripTest,
	"DreamFX.Regression.EmitterEnabledRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXEmitterEnabledRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	UNiagaraSystem* System = Build(*this, TEXT(R"(
System(Name="Unused", Root="Game") { Emitter Motes { Settings = { Enabled = false; } } }
)"));
	if (!TestNotNull(TEXT("system"), System)) { return false; }
	FGCObjectScopeGuard SystemGuard(System);
	if (!TestEqual(TEXT("one emitter"), System->GetEmitterHandles().Num(), 1)) { return false; }
	TestFalse(TEXT("Enabled=false reaches handle on either backend"), System->GetEmitterHandles()[0].GetIsEnabled());
	const FDecompileResult Exported = Export(*this, System);
	TestTrue(TEXT("disabled handle is exported"), Exported.Source.Contains(TEXT("Enabled = false;")));
	TArray<FString> Errors;
	TestTrue(TEXT("handle can be re-enabled"), FNiagaraAdapter::SetEmitterProperties(
		FStackAddress(System).WithEmitter(TEXT("Motes")), TEXT("{\"bIsEnabled\":true}"), Errors));
	TestTrue(TEXT("Enabled=true reaches handle"), System->GetEmitterHandles()[0].GetIsEnabled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXStageExecutionRoundTripTest,
	"DreamFX.Regression.StageExecutionRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXStageExecutionRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	const FString BaseSource = TEXT(R"(
System(Name="Unused", Root="Game") {
    Emitter Motes { Settings = { SimTarget = GPU; FixedBounds = box(-100,-100,-100,100,100,100); } }
})");
	UNiagaraSystem* Original = Build(*this, BaseSource);
	if (!TestNotNull(TEXT("original"), Original)) { return false; }
	FGCObjectScopeGuard OriginalGuard(Original);
	FSimulationStageSpec Spec;
	Spec.Name = TEXT("Dispatch");
	Spec.Iteration = TEXT("DirectSet");
	Spec.Execution.DirectDispatchType = TEXT("ThreeD");
	Spec.Execution.DirectDispatchElementType = TEXT("NumGroups");
	Spec.Execution.ElementCountX.Value = 128;
	Spec.Execution.ElementCountX.Binding = TEXT("Emitter.DispatchCount");
	Spec.Execution.ElementCountY.Value = 16;
	Spec.Execution.ElementCountZ.Value = 4;
	Spec.Execution.OverrideGpuDispatchNumThreads = true;
	Spec.Execution.OverrideGpuDispatchNumThreadsX.Value = 8;
	Spec.Execution.OverrideGpuDispatchNumThreadsX.Binding = TEXT("Emitter.ThreadCount");
	Spec.Execution.OverrideGpuDispatchNumThreadsY.Value = 4;
	Spec.Execution.OverrideGpuDispatchNumThreadsY.Binding = TEXT("Emitter.ThreadCountY");
	Spec.Execution.OverrideGpuDispatchNumThreadsZ.Value = 2;
	Spec.Execution.OverrideGpuDispatchNumThreadsZ.Binding = TEXT("Emitter.ThreadCountZ");
	Spec.Execution.GpuDispatchForceLinear = true;
	Spec.Execution.DisablePartialParticleUpdate = true;
	Spec.Execution.ParticleIterationStateEnabled = true;
	Spec.Execution.ParticleIterationStateBinding = TEXT("Particles.CustomState");
	Spec.Execution.ParticleIterationStateRange = FIntPoint(-2, 3);
	auto ApplyStage = [this](UNiagaraSystem* System, const FSimulationStageSpec& Value)
	{
		const FStackAddress Address = FStackAddress(System).WithEmitter(TEXT("Motes"));
		TArray<FString> Errors;
		const bool bOk = FNiagaraAdapter::BeginSimulationStageEdit(Address, Value, 0, Errors)
			&& FNiagaraAdapter::EndSimulationStageEdit(Address, Value, Errors);
		if (!bOk) { AddError(FString::Join(Errors, TEXT(" | "))); }
		return bOk;
	};
	if (!ApplyStage(Original, Spec)) { return false; }
	const FDecompileResult Exported = Export(*this, Original);
	FDocument Document;
	FDiagnosticSink Diagnostics;
	if (!TestTrue(TEXT("stage execution options parse after export"),
		FParser::ParseText(Exported.Source, TEXT("StageExecution.dfs"), Document, Diagnostics)))
	{
		ReportErrors(*this, Diagnostics);
		return false;
	}
	const FStack* ExportedStage = Document.Emitters.Num() == 1
		? Document.Emitters[0].Stacks.FindByPredicate([](const FStack& Stack) { return Stack.Kind == EStackKind::SimulationStage; })
		: nullptr;
	if (!TestNotNull(TEXT("export contains stage"), ExportedStage)) { return false; }
	UNiagaraSystem* Mirror = Build(*this, BaseSource);
	if (!TestNotNull(TEXT("mirror"), Mirror)) { return false; }
	FGCObjectScopeGuard MirrorGuard(Mirror);
	if (!ApplyStage(Mirror, ExportedStage->Stage)) { return false; }
	UNiagaraSimulationStageGeneric* Rebuilt = Stage(Mirror);
	if (!TestNotNull(TEXT("rebuilt generic stage"), Rebuilt)) { return false; }
	TestEqual(TEXT("dispatch X fallback"), Rebuilt->ElementCountX.GetDefaultValue<int32>(), 128);
	TestEqual(TEXT("dispatch Y fallback"), Rebuilt->ElementCountY.GetDefaultValue<int32>(), 16);
	TestEqual(TEXT("dispatch Z fallback"), Rebuilt->ElementCountZ.GetDefaultValue<int32>(), 4);
	TestEqual(TEXT("dispatch X parameter"), Rebuilt->ElementCountX.AliasedParameter.GetName(), FName(TEXT("Emitter.DispatchCount")));
	TestEqual(TEXT("thread X fallback"), Rebuilt->OverrideGpuDispatchNumThreadsX.GetDefaultValue<int32>(), 8);
	TestEqual(TEXT("thread Y fallback"), Rebuilt->OverrideGpuDispatchNumThreadsY.GetDefaultValue<int32>(), 4);
	TestEqual(TEXT("thread Z fallback"), Rebuilt->OverrideGpuDispatchNumThreadsZ.GetDefaultValue<int32>(), 2);
	TestEqual(TEXT("thread X parameter"), Rebuilt->OverrideGpuDispatchNumThreadsX.AliasedParameter.GetName(), FName(TEXT("Emitter.ThreadCount")));
	TestTrue(TEXT("element counts use dynamic int bindings"),
		Rebuilt->ElementCountX.AliasedParameter.GetType() == FNiagaraTypeDefinition::GetIntDef());
	const FNiagaraTypeDefinition StaticInt = FNiagaraTypeDefinition::GetIntDef().ToStaticDef();
	for (const FNiagaraParameterBindingWithValue* Binding : { &Rebuilt->OverrideGpuDispatchNumThreadsX,
		&Rebuilt->OverrideGpuDispatchNumThreadsY, &Rebuilt->OverrideGpuDispatchNumThreadsZ })
	{
		TestTrue(TEXT("thread-group aliased parameter retains static int type"), Binding->AliasedParameter.GetType() == StaticInt);
		TestTrue(TEXT("thread-group resolved parameter retains static int type"), Binding->ResolvedParameter.GetType() == StaticInt);
	}
	TestTrue(TEXT("thread override"), Rebuilt->bOverrideGpuDispatchNumThreads != 0);
	TestTrue(TEXT("linear dispatch override"), Rebuilt->bGpuDispatchForceLinear != 0);
	TestTrue(TEXT("partial particle update disabled"), Rebuilt->bDisablePartialParticleUpdate != 0);
	TestTrue(TEXT("particle-state filtering enabled"), Rebuilt->bParticleIterationStateEnabled != 0);
	TestEqual(TEXT("particle-state range"), Rebuilt->ParticleIterationStateRange, FIntPoint(-2, 3));
	TestEqual(TEXT("dispatch dimensions"), Rebuilt->DirectDispatchType, ENiagaraGpuDispatchType::ThreeD);
	TestEqual(TEXT("dispatch element interpretation"), Rebuilt->DirectDispatchElementType, ENiagaraDirectDispatchElementType::NumGroups);
	TArray<FNiagaraAdapter::FSimulationStageSummary> Summaries;
	TArray<FString> Errors;
	FNiagaraAdapter::GetEmitterSimulationStages(FStackAddress(Mirror).WithEmitter(TEXT("Motes")), Summaries, Errors);
	if (TestEqual(TEXT("one stage summary"), Summaries.Num(), 1))
	{
		TestTrue(TEXT("particle-state binding survives"), Summaries[0].Execution.ParticleIterationStateBinding.Contains(TEXT("CustomState")));
	}

	// Omitting options on a subsequent build must restore defaults, not retain the old config.
	FSimulationStageSpec DefaultSpec;
	DefaultSpec.Name = Spec.Name;
	if (!ApplyStage(Mirror, DefaultSpec)) { return false; }
	Rebuilt = Stage(Mirror);
	TestEqual(TEXT("removed dispatch fallback resets"), Rebuilt->ElementCountX.GetDefaultValue<int32>(), 0);
	TestTrue(TEXT("removed dispatch parameter resets"), Rebuilt->ElementCountX.AliasedParameter.GetName().IsNone());
	TestFalse(TEXT("removed filter resets"), Rebuilt->bParticleIterationStateEnabled != 0);
	TestFalse(TEXT("removed thread override resets"), Rebuilt->bOverrideGpuDispatchNumThreads != 0);

	DefaultSpec.Execution.ParticleIterationStateBinding = TEXT("None");
	if (!ApplyStage(Mirror, DefaultSpec)) { return false; }
	const FDecompileResult ClearedExport = Export(*this, Mirror);
	TestTrue(TEXT("cleared particle state binding is explicit"),
		ClearedExport.Source.Contains(TEXT("ParticleIterationStateBinding = None")));
	FDocument ClearedDocument;
	FDiagnosticSink ClearedDiagnostics;
	if (!TestTrue(TEXT("cleared particle state binding parses"), FParser::ParseText(
		ClearedExport.Source, TEXT("ClearedStage.dfs"), ClearedDocument, ClearedDiagnostics))) { return false; }
	const FStack* ClearedStage = ClearedDocument.Emitters.Num() == 1
		? ClearedDocument.Emitters[0].Stacks.FindByPredicate([](const FStack& Stack) { return Stack.Kind == EStackKind::SimulationStage; })
		: nullptr;
	if (!TestNotNull(TEXT("cleared stage exported"), ClearedStage) || !ApplyStage(Original, ClearedStage->Stage)) { return false; }
	Summaries.Reset();
	FNiagaraAdapter::GetEmitterSimulationStages(FStackAddress(Original).WithEmitter(TEXT("Motes")), Summaries, Errors);
	if (TestEqual(TEXT("one cleared stage"), Summaries.Num(), 1))
	{
		TestTrue(TEXT("cleared particle state binding survives rebuild"), Summaries[0].Execution.ParticleIterationStateBinding.IsEmpty());
	}
	return true;
}

#endif
