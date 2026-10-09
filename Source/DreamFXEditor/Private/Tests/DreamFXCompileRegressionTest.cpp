#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UnrealType.h"

namespace UE::DreamFX::Editor::CompileRegression
{
	bool Parse(FAutomationTestBase& Test, const FString& Text, const FString& File, FDocument& Out)
	{
		FDiagnosticSink Diagnostics;
		if (!FParser::ParseText(Text, File, Out, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll());
			return false;
		}
		return true;
	}

	bool HasFloatLiteral(const FNiagaraVMExecutableData& VM, float Expected)
	{
		for (const FNiagaraVariable& Variable : VM.InternalParameters.Parameters)
		{
			if (Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef()
				&& Variable.IsDataAllocated() && Variable.GetValue<float>() == Expected)
			{
				return true;
			}
		}
		return false;
	}

	TArray<uint8> InternalConstantBytes(const FNiagaraVMExecutableData& VM)
	{
		TArray<uint8> Bytes;
		for (const FNiagaraVariable& Variable : VM.InternalParameters.Parameters)
		{
			if (Variable.IsDataAllocated()) { Bytes.Append(Variable.GetData(), Variable.GetSizeInBytes()); }
		}
		return Bytes;
	}

	UNiagaraScript* ParticleSpawn(UNiagaraSystem* System)
	{
		if (System == nullptr || System->GetEmitterHandles().Num() != 1) { return nullptr; }
		const FVersionedNiagaraEmitterData* Data = System->GetEmitterHandles()[0].GetEmitterData();
		return Data == nullptr ? nullptr : Data->SpawnScriptProps.Script.Get();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXHeadlessVmRebuildRegression,
	"DreamFX.Regression.Compile.ModuleChangeUpdatesVm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXHeadlessVmRebuildRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::CompileRegression;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ModuleName = TEXT("DreamFXAutomation/M_Vm_") + Suffix;
	FDocument ModuleDocument;
	if (!Parse(*this, FString::Printf(TEXT(
		"Module(Name=\"%s\",Root=\"Game\") { Settings={Usage=ParticleSpawn;} Body={Particles.SpriteRotation=123.25;} }"),
		*ModuleName), TEXT("VmModule.dfm"), ModuleDocument)) { return false; }
	FGenerateOptions Options;
	Options.bSave = false;
	Options.bForce = true;
	FDiagnosticSink Diagnostics;
	const FModuleGenerateResult Module = FModuleGenerator::Generate(ModuleDocument, Options, Diagnostics);
	if (!TestTrue(TEXT("module generated"), Module.bSucceeded) || !TestNotNull(TEXT("module script"), Module.Script))
	{
		AddError(Diagnostics.FormatAll());
		return false;
	}
	FGCObjectScopeGuard ModuleGuard(Module.Script);
	FDocument SystemDocument;
	if (!Parse(*this, FString::Printf(TEXT(
		"System(Name=\"DreamFXAutomation/NS_Vm_%s\",Root=\"Game\") { Emitter E { ParticleSpawn={`/Game/%s`();} SpriteRenderer R {} } }"),
		*Suffix, *ModuleName), TEXT("VmSystem.dfs"), SystemDocument)) { return false; }
	Diagnostics.Reset();
	const FGenerateResult Initial = FGenerator::Generate(SystemDocument, Options, Diagnostics);
	if (!TestTrue(TEXT("initial system compiled"), Initial.bSucceeded) || !TestNotNull(TEXT("system"), Initial.System))
	{
		AddError(Diagnostics.FormatAll());
		return false;
	}
	FGCObjectScopeGuard SystemGuard(Initial.System);
	UNiagaraScript* Spawn = ParticleSpawn(Initial.System);
	if (!TestNotNull(TEXT("particle spawn executable"), Spawn)) { return false; }
	const FNiagaraVMExecutableData& FirstVM = Spawn->GetVMExecutableData();
	TestTrue(TEXT("initial VM has executable bytecode"), FirstVM.HasByteCode());
	TestTrue(TEXT("initial compiled VM contains original module constant"), HasFloatLiteral(FirstVM, 123.25f));
	TestFalse(TEXT("initial compiled VM has no revised constant"), HasFloatLiteral(FirstVM, 987.5f));
	const TArray<uint8> FirstConstants = InternalConstantBytes(FirstVM);
	const FNiagaraVMExecutableDataId FirstId = Spawn->GetVMExecutableDataCompilationId();

	// Mutate the actual module asset's HLSL independently of system generation. This is the same
	// referenced graph a rebuilt .dfm changes; updating the system must install a different VM.
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Module.Script->GetLatestSource());
	if (!TestNotNull(TEXT("module source"), Source) || !TestNotNull(TEXT("module graph"), Source->NodeGraph.Get())) { return false; }
	bool bChangedBody = false;
	for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
	{
		if (UNiagaraNodeCustomHlsl* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Node))
		{
			const FStrProperty* Body = FindFProperty<FStrProperty>(Hlsl->GetClass(), TEXT("CustomHlsl"));
			if (!TestNotNull(TEXT("HLSL property"), Body)) { return false; }
			FString Text = Body->GetPropertyValue_InContainer(Hlsl);
			if (Text.ReplaceInline(TEXT("123.25"), TEXT("987.5")) > 0)
			{
				Body->SetPropertyValue_InContainer(Hlsl, Text);
				Hlsl->MarkNodeRequiresSynchronization(TEXT("DreamFX VM regression: module constant changed"), true);
				bChangedBody = true;
			}
		}
	}
	if (!TestTrue(TEXT("module logic changed"), bChangedBody)) { return false; }
	Module.Script->RequestCompile(FGuid(), true);
	Diagnostics.Reset();
	const FGenerateResult Revised = FGenerator::Generate(SystemDocument, Options, Diagnostics);
	if (!TestTrue(TEXT("revised system compiled"), Revised.bSucceeded))
	{
		AddError(Diagnostics.FormatAll());
		return false;
	}
	TestTrue(TEXT("rebuild keeps the system identity"), Revised.System == Initial.System);
	Spawn = ParticleSpawn(Revised.System);
	if (!TestNotNull(TEXT("revised spawn executable"), Spawn)) { return false; }
	const FNiagaraVMExecutableData& RevisedVM = Spawn->GetVMExecutableData();
	TestTrue(TEXT("revised VM has executable bytecode"), RevisedVM.HasByteCode());
	TestTrue(TEXT("revised compiled VM contains the new module constant"), HasFloatLiteral(RevisedVM, 987.5f));
	TestFalse(TEXT("revised compiled VM no longer contains the old module constant"), HasFloatLiteral(RevisedVM, 123.25f));
	// Constant-only edits can keep the optimized opcode stream unchanged. The runtime's
	// executable constant table must still change, independently of provenance or timestamps.
	TestTrue(TEXT("runtime VM constant bytes change, not only the asset timestamp"),
		InternalConstantBytes(RevisedVM) != FirstConstants);
	TestTrue(TEXT("installed executable identifies the revised graph"), Spawn->GetVMExecutableDataCompilationId() != FirstId);
	TestTrue(TEXT("installed VM is synchronized"), Spawn->AreScriptAndSourceSynchronized());

	// A historical UpToDate status and matching id alone must not bless missing executable data.
	const TArray<uint8> SavedByteCode = Spawn->GetVMExecutableData().ExperimentalContextData;
	Spawn->GetVMExecutableData().ExperimentalContextData.Reset();
	FCompileStateInfo State;
	TArray<FString> Errors;
	const bool bAcceptedMissingVm = FNiagaraAdapter::WaitAndCollect(Revised.System, false, State, Errors);
	Spawn->GetVMExecutableData().ExperimentalContextData = SavedByteCode;
	TestFalse(TEXT("missing VM cannot be reported as a successful compile"), bAcceptedMissingVm);
	TestTrue(TEXT("missing VM is marked stale"), State.bIsStale);
	TestTrue(TEXT("missing VM failure explains the affected script"), FString::Join(Errors, TEXT(" ")).Contains(Spawn->GetName()));
	return true;
}

#endif
