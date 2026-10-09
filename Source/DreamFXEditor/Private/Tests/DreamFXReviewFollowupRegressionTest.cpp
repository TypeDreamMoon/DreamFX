#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Commandlet/DreamFXCommandlet.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "Diff/DreamFXAssetFacts.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "Materials/MaterialInterface.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::DreamFX::Editor::RoundTripRegression
{
	UNiagaraSystem* Build(FAutomationTestBase& Test, const FString& Source);
	FDecompileResult Export(FAutomationTestBase& Test, UNiagaraSystem* System);
}

namespace UE::DreamFX::Editor::ReviewFollowupRegression
{
	TArray<FString> SystemSettingsFacts(UNiagaraSystem* System)
	{
		TArray<FString> Facts;
		DescribeSystemFacts(System, Facts);
		Facts.RemoveAll([](const FString& Fact) { return !Fact.StartsWith(TEXT("system ")); });
		Facts.Sort();
		return Facts;
	}

	struct FRegisteredTestAsset
	{
		UNiagaraSystem* System;
		explicit FRegisteredTestAsset(UNiagaraSystem* InSystem) : System(InSystem)
		{
			FAssetRegistryModule::AssetCreated(System);
		}
		~FRegisteredTestAsset() { FAssetRegistryModule::AssetDeleted(System); }
	};

	struct FAssetDiffSummaryCapture : FOutputDevice
	{
		FString Summary;
		FAssetDiffSummaryCapture() { GLog->AddOutputDevice(this); }
		~FAssetDiffSummaryCapture() { GLog->RemoveOutputDevice(this); }
		FString ReadSummary() { GLog->FlushThreadedLogs(); return Summary; }
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (FCString::Strstr(Message, TEXT("=== asset diff:")) != nullptr) { Summary = Message; }
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDescriptionEscapingRegression,
	"DreamFX.Regression.Followups.DescriptionEscaping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDescriptionEscapingRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	UNiagaraSystem* Original = Build(*this, TEXT("System(Name=\"Unused\",Root=\"Game\") { Emitter E {} }"));
	if (!TestNotNull(TEXT("original"), Original)) { return false; }
	FGCObjectScopeGuard OriginalGuard(Original);
	const FString Description = TEXT("A \"quoted\" effect; C:\\FX\\test\nnext line\rcarriage\ttab");
	TArray<FString> Errors;
	if (!TestTrue(TEXT("author description on Niagara asset"), FNiagaraAdapter::AddUserVariable(
		Original, TEXT("Gain"), FNiagaraTypeDefinition::GetFloatDef(), Description, FInputValue(), Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | "))); return false;
	}
	const FDecompileResult Exported = Export(*this, Original);
	TestTrue(TEXT("description control characters are escaped"), Exported.Source.Contains(TEXT("\\nnext line\\rcarriage\\ttab")));
	UNiagaraSystem* Mirror = Build(*this, Exported.Source);
	if (!TestNotNull(TEXT("description export parses and builds"), Mirror)) { return false; }
	FGCObjectScopeGuard MirrorGuard(Mirror);
	TArray<FUserVariableInfo> Variables;
	if (!TestTrue(TEXT("read rebuilt user metadata"), FNiagaraAdapter::GetUserVariables(Mirror, Variables, Errors))) { return false; }
	const FUserVariableInfo* Gain = Variables.FindByPredicate([](const FUserVariableInfo& Variable)
		{ return Variable.Name == TEXT("User.Gain"); });
	if (TestNotNull(TEXT("rebuilt parameter"), Gain)) { TestEqual(TEXT("description bytes survive round trip"), Gain->Description, Description); }
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXNestedMaterialBindingRegression,
	"DreamFX.Regression.Followups.NestedMaterialBinding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXNestedMaterialBindingRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	UNiagaraSystem* Original = Build(*this, TEXT(R"(
System(Name="Unused",Root="Game") {
    Properties = { Object<MaterialInterface> Material; }
    Emitter E { MeshRenderer Mesh { Meshes=["/Engine/BasicShapes/Cube"]; bOverrideMaterials=true; } }
})"));
	if (!TestNotNull(TEXT("original"), Original)) { return false; }
	FGCObjectScopeGuard OriginalGuard(Original);
	auto MeshRenderer = [](UNiagaraSystem* System) -> UNiagaraMeshRendererProperties*
	{
		const FVersionedNiagaraEmitterData* Data = System->GetEmitterHandles()[0].GetEmitterData();
		return Data != nullptr && Data->GetRenderers().Num() == 1 ? Cast<UNiagaraMeshRendererProperties>(Data->GetRenderers()[0]) : nullptr;
	};
	UNiagaraMeshRendererProperties* Mesh = MeshRenderer(Original);
	if (!TestNotNull(TEXT("mesh renderer"), Mesh)) { return false; }
	Mesh->OverrideMaterials.AddDefaulted();
	Mesh->OverrideMaterials[0].UserParamBinding.Parameter = FNiagaraVariable(FNiagaraTypeDefinition::GetUMaterialDef(), TEXT("User.Material"));
	Mesh->OverrideMaterials[0].ExplicitMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	// Include an unset entry: the array importer must preserve its index and constructor type.
	Mesh->OverrideMaterials.AddDefaulted();
	const FDecompileResult Exported = Export(*this, Original);
	TestTrue(TEXT("nested binding represented"), Exported.Source.Contains(TEXT("OverrideMaterials =")) && Exported.Source.Contains(TEXT("User.Material")));
	TArray<FString> ExportLines;
	Exported.Source.ParseIntoArrayLines(ExportLines);
	const FString* MaterialLine = ExportLines.FindByPredicate([](const FString& Line) { return Line.Contains(TEXT("OverrideMaterials =")); });
	if (!TestNotNull(TEXT("material override source statement"), MaterialLine)) { return false; }
	TestFalse(TEXT("nested material binding excludes transient registry handles"), MaterialLine->Contains(TEXT("typeDefHandle")));
	TestFalse(TEXT("nested material binding excludes process-local registry indices"), MaterialLine->Contains(TEXT("registeredTypeIndex")));
	UNiagaraSystem* Mirror = Build(*this, Exported.Source);
	if (!TestNotNull(TEXT("mirror"), Mirror)) { return false; }
	FGCObjectScopeGuard MirrorGuard(Mirror);
	UNiagaraMeshRendererProperties* Rebuilt = MeshRenderer(Mirror);
	if (!TestNotNull(TEXT("rebuilt mesh renderer"), Rebuilt) || !TestEqual(TEXT("material array positions retained"), Rebuilt->OverrideMaterials.Num(), 2)) { return false; }
	TestEqual(TEXT("runtime material parameter retained"), Rebuilt->OverrideMaterials[0].UserParamBinding.Parameter.GetName(), FName(TEXT("User.Material")));
	TestTrue(TEXT("material type restored from element constructor"), Rebuilt->OverrideMaterials[0].UserParamBinding.Parameter.GetType() == FNiagaraTypeDefinition::GetUMaterialDef());
	TestTrue(TEXT("fallback material retained"), Rebuilt->OverrideMaterials[0].ExplicitMat == Mesh->OverrideMaterials[0].ExplicitMat);
	TestTrue(TEXT("unset entry retains material type"), Rebuilt->OverrideMaterials[1].UserParamBinding.Parameter.GetType() == FNiagaraTypeDefinition::GetUMaterialDef());
	TestTrue(TEXT("unset entry remains unset"), Rebuilt->OverrideMaterials[1].UserParamBinding.Parameter.GetName().IsNone());
	const FDecompileResult Reexported = Export(*this, Mirror);
	TestTrue(TEXT("nested binding re-exports identically"), Reexported.Source.Contains(*MaterialLine));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSystemSettingsFactsRegression,
	"DreamFX.Regression.Followups.SystemSettingsFacts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSystemSettingsFactsRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	using namespace UE::DreamFX::Editor::ReviewFollowupRegression;
	// Bounds remain authored configuration when the system is using dynamic bounds.
	UNiagaraSystem* Original = Build(*this, TEXT("System(Name=\"Unused\",Root=\"Game\") { Settings={FixedBounds=box(-50,-60,-70,50,60,70);} Emitter E {} }"));
	if (!TestNotNull(TEXT("original"), Original)) { return false; }
	FGCObjectScopeGuard OriginalGuard(Original);
	const FBoolProperty* FixedBoundsEnabled = FindFProperty<FBoolProperty>(Original->GetClass(), TEXT("bFixedBounds"));
	if (!TestNotNull(TEXT("fixed bounds enable property"), FixedBoundsEnabled)) { return false; }
	TestFalse(TEXT("fixture has authored bounds with fixed bounds disabled"), FixedBoundsEnabled->GetPropertyValue_InContainer(Original));
	const TArray<FString> Baseline = SystemSettingsFacts(Original);
	TestTrue(TEXT("facts include authored system configuration"), Baseline.Num() > 0);
	const FDecompileResult Exported = Export(*this, Original);
	TestTrue(TEXT("disabled authored bounds are present in the export"), Exported.Source.Contains(TEXT("FixedBounds = box(")));
	UNiagaraSystem* Mirror = Build(*this, Exported.Source);
	if (!TestNotNull(TEXT("mirror"), Mirror)) { return false; }
	FGCObjectScopeGuard MirrorGuard(Mirror);
	TestFalse(TEXT("bounds remain disabled after round trip"), FixedBoundsEnabled->GetPropertyValue_InContainer(Mirror));
	TestTrue(TEXT("fresh round trip has the same system settings despite a new asset identity"), Baseline == SystemSettingsFacts(Mirror));
	struct FSetting { const TCHAR* Name; const TCHAR* Value; };
	const FSetting Settings[] = {
		{ TEXT("bFixedTickDelta"), TEXT("True") },
		{ TEXT("FixedTickDeltaTime"), TEXT("0.125") },
		{ TEXT("WarmupTime"), TEXT("2.0") },
		{ TEXT("WarmupTickCount"), TEXT("31") },
		{ TEXT("WarmupTickDelta"), TEXT("0.25") },
		{ TEXT("bFixedBounds"), TEXT("True") },
		{ TEXT("FixedBounds"), TEXT("(Min=(X=-321,Y=-654,Z=-987),Max=(X=321,Y=654,Z=987),IsValid=1)") },
		{ TEXT("bOverrideScalabilitySettings"), TEXT("True") },
		{ TEXT("bSupportLargeWorldCoordinates"), TEXT("False") },
	};
	for (const FSetting& Setting : Settings)
	{
		FProperty* Property = FindFProperty<FProperty>(Mirror->GetClass(), Setting.Name);
		if (!TestNotNull(Setting.Name, Property)) { continue; }
		void* Data = Property->ContainerPtrToValuePtr<void>(Mirror);
		FString Previous;
		Property->ExportTextItem_Direct(Previous, Data, nullptr, Mirror, PPF_None);
		if (!TestNotNull(TEXT("change runtime setting"), Property->ImportText_Direct(Setting.Value, Data, Mirror, PPF_None))) { continue; }
		TestTrue(FString::Printf(TEXT("asset facts detect %s"), Setting.Name), Baseline != SystemSettingsFacts(Mirror));
		Property->ImportText_Direct(*Previous, Data, Mirror, PPF_None);
		TestTrue(TEXT("restoring setting restores facts"), Baseline == SystemSettingsFacts(Mirror));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXLightweightExportGapRegression,
	"DreamFX.Regression.Followups.LightweightExportGap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXLightweightExportGapRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	UNiagaraSystem* System = Build(*this, TEXT("System(Name=\"Unused\",Root=\"Game\") { Emitter E {} }"));
	if (!TestNotNull(TEXT("system"), System)) { return false; }
	FGCObjectScopeGuard Guard(System);
	System->GetEmitterHandles()[0].SetEmitterMode(*System, ENiagaraEmitterMode::Stateless);
	FDiagnosticSink Diagnostics;
	const FDecompileResult Exported = FDecompiler::Decompile(System, TEXT("Game"), Diagnostics);
	TestTrue(TEXT("partial export remains available for inspection"), Exported.bSucceeded);
	TestTrue(TEXT("lightweight is an explicit coverage gap"), Exported.UnsupportedFeatures.ContainsByPredicate(
		[](const FString& Gap) { return Gap.Contains(TEXT("Lightweight/Stateless")); }));
	TestTrue(TEXT("gap includes actionable diagnostic"), Diagnostics.GetDiagnostics().ContainsByPredicate(
		[](const FDiagnostic& Entry) { return Entry.Code == TEXT("DFX8017"); }));
	TestFalse(TEXT("unsupported emitter is not emitted as an ordinary empty block"), Exported.Source.Contains(TEXT("Emitter E {")));
	TestTrue(TEXT("gap is visible in exported text"), Exported.Source.Contains(TEXT("Lightweight/Stateless")));
	TestTrue(TEXT("export does not replace lightweight mode"), System->GetEmitterHandles()[0].GetEmitterMode() == ENiagaraEmitterMode::Stateless);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXAssetDiffFailureExitRegression,
	"DreamFX.Regression.Followups.AssetDiffFailureExit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXAssetDiffFailureExitRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::RoundTripRegression;
	using namespace UE::DreamFX::Editor::ReviewFollowupRegression;
	UDreamFXCommandlet* Commandlet = NewObject<UDreamFXCommandlet>();
	FGCObjectScopeGuard CommandletGuard(Commandlet);
	AddExpectedError(TEXT("Asset diff cannot resolve content root"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("unmounted root fails the command"), Commandlet->Main(TEXT("-AssetDiff -Path=/DreamFXUnregisteredRoot -NoCompile")) != 0);
	AddExpectedError(TEXT("Asset diff has no valid content roots"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("delimiter-only path cannot silently compare every root"), Commandlet->Main(TEXT("-AssetDiff -Path=+++ -NoCompile")) != 0);
	using namespace UE::DreamFX;
	const FString RelativeRoot = TEXT("DreamFXAutomation/AssetDiff_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ModuleName = RelativeRoot / TEXT("M_Body");
	FDiagnosticSink Diagnostics;
	FDocument ModuleDocument;
	if (!FParser::ParseText(FString::Printf(TEXT("Module(Name=\"%s\",Root=\"Game\") { Settings={Usage=ParticleSpawn;} Body={Particles.SpriteRotation=12.0;} }"),
		*ModuleName), TEXT("AssetDiffModule.dfm"), ModuleDocument, Diagnostics)) { AddError(Diagnostics.FormatAll()); return false; }
	FGenerateOptions Options;
	Options.bSave = false;
	Options.bForce = true;
	const FModuleGenerateResult Module = FModuleGenerator::Generate(ModuleDocument, Options, Diagnostics);
	if (!TestTrue(TEXT("fixture module generated"), Module.bSucceeded) || !TestNotNull(TEXT("module"), Module.Script)) { AddError(Diagnostics.FormatAll()); return false; }
	FGCObjectScopeGuard ModuleGuard(Module.Script);
	FDocument SystemDocument;
	if (!FParser::ParseText(FString::Printf(TEXT("System(Name=\"%s/NS_Original\",Root=\"Game\") { Emitter E { ParticleSpawn={`/Game/%s`();} SpriteRenderer R {} } }"),
		*RelativeRoot, *ModuleName), TEXT("AssetDiffSystem.dfs"), SystemDocument, Diagnostics)) { AddError(Diagnostics.FormatAll()); return false; }
	const FGenerateResult Generated = FGenerator::Generate(SystemDocument, Options, Diagnostics);
	UNiagaraSystem* Original = Generated.System;
	if (!TestTrue(TEXT("fixture compiles initially"), Generated.bSucceeded) || !TestNotNull(TEXT("original without mirror"), Original)) { AddError(Diagnostics.FormatAll()); return false; }
	FGCObjectScopeGuard OriginalGuard(Original);
	FRegisteredTestAsset Registered(Original);
	const FString Arguments = TEXT("-AssetDiff -Path=/Game/") + RelativeRoot;
	FAssetDiffSummaryCapture Capture;
	TestEqual(TEXT("missing mirror fails the command"), Commandlet->Main(Arguments + TEXT(" -NoCompile")), 1);
	FString Summary = Capture.ReadSummary();
	TestTrue(TEXT("missing mirror never counts SAME"), Summary.Contains(TEXT("0 same")) && Summary.Contains(TEXT("1 missing")));
	UPackage* MirrorPackage = CreatePackage(*(TEXT("/Game/Decompiled/") + RelativeRoot / TEXT("NS_Original")));
	UNiagaraSystem* Mirror = DuplicateObject<UNiagaraSystem>(Original, MirrorPackage, TEXT("NS_Original"));
	FGCObjectScopeGuard MirrorGuard(Mirror);
	FRegisteredTestAsset RegisteredMirror(Mirror);
	TestEqual(TEXT("identical compiled mirrors compare successfully"), Commandlet->Main(Arguments + TEXT(" -NoCompile")), 0);
	TestTrue(TEXT("successful comparison counts SAME"), Capture.ReadSummary().Contains(TEXT("1 same")));

	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Module.Script->GetLatestSource());
	if (!TestNotNull(TEXT("module source"), Source) || !TestNotNull(TEXT("module graph"), Source->NodeGraph.Get())) { return false; }
	bool bChanged = false;
	for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
	{
		if (UNiagaraNodeCustomHlsl* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Node))
		{
			const FStrProperty* Body = FindFProperty<FStrProperty>(Hlsl->GetClass(), TEXT("CustomHlsl"));
			if (Body != nullptr)
			{
				FString Text = Body->GetPropertyValue_InContainer(Hlsl);
				if (Text.ReplaceInline(TEXT("12.0"), TEXT("DreamFXMissingFunction()")) > 0)
				{
					Body->SetPropertyValue_InContainer(Hlsl, Text);
					Hlsl->MarkNodeRequiresSynchronization(TEXT("DreamFX asset-diff regression: invalid function"), true);
					bChanged = true;
				}
			}
		}
	}
	if (!TestTrue(TEXT("fixture now contains a real compile error"), bChanged)) { return false; }
	AddExpectedError(TEXT("DreamFXMissingFunction|Error compiling|Compile failed"), EAutomationExpectedErrorFlags::Contains, 0);
	TestEqual(TEXT("failed recompilation fails the command"), Commandlet->Main(Arguments), 1);
	Summary = Capture.ReadSummary();
	TestTrue(TEXT("failed compilation cannot compare stale facts as SAME"),
		Summary.Contains(TEXT("0 same")) && Summary.Contains(TEXT("0 different")) && Summary.Contains(TEXT("1 failed")));
	return true;
}

#endif
