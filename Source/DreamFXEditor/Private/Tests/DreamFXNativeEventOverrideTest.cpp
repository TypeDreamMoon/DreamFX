#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "EdGraphSchema_Niagara.h"
#include "Generation/DreamFXGenerator.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace UE::DreamFX::Editor::NativeInheritanceRegression
{
	UNiagaraEmitter* MakeParent(FAutomationTestBase& Test, const FString& Suffix);
	bool HasAssignment(const FVersionedNiagaraEmitterData* Data, FName Target);
	FGenerateResult Build(FAutomationTestBase& Test, const FString& Source);
}

namespace UE::DreamFX::Editor::NativeEventOverrideRegression
{
	bool AddNativeHandler(FAutomationTestBase& Test, UNiagaraEmitter* Parent, const FGuid& SourceId,
		FName Target, TSet<FGuid>& OutEventNodes)
	{
		FVersionedNiagaraEmitterData* Data = Parent->GetLatestEmitterData();
		UNiagaraGraph* Graph = CastChecked<UNiagaraScriptSource>(Data->GraphSource)->NodeGraph;
		TSet<FGuid> Before;
		for (const UEdGraphNode* Node : Graph->Nodes) { Before.Add(Node->NodeGuid); }
		const FGuid UsageId = FGuid::NewGuid();
		FNiagaraEventScriptProperties Event;
		Event.ExecutionMode = EScriptExecutionMode::EveryParticle;
		Event.SourceEmitterID = SourceId;
		Event.SourceEventName = TEXT("LocationEvent");
		Event.Script = NewObject<UNiagaraScript>(Parent, NAME_None, RF_Transactional);
		Event.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
		Event.Script->SetUsageId(UsageId);
		Event.Script->SetLatestSource(Data->GraphSource);
		Parent->AddEventHandler(Event, Data->Version.VersionGuid);

		// Use the engine graph types directly, independently of DreamFX's zero-id writer.
		UNiagaraNodeOutput* Output = NewObject<UNiagaraNodeOutput>(Graph, NAME_None, RF_Transactional);
		Output->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
		Output->SetUsageId(UsageId);
		Output->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
		UNiagaraNodeInput* Input = NewObject<UNiagaraNodeInput>(Graph, NAME_None, RF_Transactional);
		Input->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		Input->Usage = ENiagaraInputNodeUsage::Parameter;
		for (UNiagaraNode* Node : { static_cast<UNiagaraNode*>(Input), static_cast<UNiagaraNode*>(Output) })
		{
			Graph->AddNode(Node, false, false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			if (Node->Pins.IsEmpty()) { Node->AllocateDefaultPins(); }
		}
		auto MapPin = [](UNiagaraNode* Node, EEdGraphPinDirection Direction) -> UEdGraphPin*
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction == Direction && GetDefault<UEdGraphSchema_Niagara>()->PinToTypeDefinition(Pin)
					== FNiagaraTypeDefinition::GetParameterMapDef()) { return Pin; }
			}
			return nullptr;
		};
		UEdGraphPin* InputMap = MapPin(Input, EGPD_Output);
		UEdGraphPin* OutputMap = MapPin(Output, EGPD_Input);
		if (!Test.TestNotNull(TEXT("native event input map"), InputMap)
			|| !Test.TestNotNull(TEXT("native event output map"), OutputMap)) { return false; }
		InputMap->MakeLinkTo(OutputMap);
		if (!Test.TestNotNull(TEXT("native event assignment"), FNiagaraStackGraphUtilities::AddParameterModuleToStack(
			{ FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), Target) }, *Output, INDEX_NONE, { TEXT("3.0") })))
		{
			return false;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Before.Contains(Node->NodeGuid)) { OutEventNodes.Add(Node->NodeGuid); }
		}
		return true;
	}

	const FNiagaraEmitterHandle* ChildHandle(UNiagaraSystem* System)
	{
		return System->GetEmitterHandles().FindByPredicate(
			[](const FNiagaraEmitterHandle& Handle) { return Handle.GetName() == TEXT("Child"); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXNativeEventGroupOverrideRegression,
	"DreamFX.Regression.Inheritance.NativeEventGroupOverride",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXNativeEventGroupOverrideRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::NativeInheritanceRegression;
	using namespace UE::DreamFX::Editor::NativeEventOverrideRegression;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = TEXT("NS_NativeEvents_") + Suffix;
	TArray<FString> Errors;
	bool bCreated = false;
	UNiagaraSystem* System = FNiagaraAdapter::AcquireSystem(TEXT("/Game/DreamFXAutomation"), AssetName, bCreated, Errors);
	if (!TestNotNull(TEXT("native event system"), System)) { return false; }
	FGCObjectScopeGuard SystemGuard(System);
	if (!TestTrue(TEXT("reserve stable event source GUID"), FNiagaraAdapter::AddEmitter(System, TEXT("Source"), Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | "))); return false;
	}
	const FGuid SourceId = System->GetEmitterHandles()[0].GetId();
	UNiagaraEmitter* Parent = MakeParent(*this, Suffix);
	if (!TestNotNull(TEXT("native event parent"), Parent)) { return false; }
	FGCObjectScopeGuard ParentGuard(Parent);
	TSet<FGuid> ParentEventNodeGuids;
	if (!AddNativeHandler(*this, Parent, SourceId, TEXT("Particles.ParentEventOne"), ParentEventNodeGuids)
		|| !AddNativeHandler(*this, Parent, SourceId, TEXT("Particles.ParentEventTwo"), ParentEventNodeGuids)) { return false; }
	const FGuid ParentVersion = Parent->GetExposedVersion().VersionGuid;
	auto Document = [&AssetName, Parent](const FString& Body)
	{
		return FString::Printf(TEXT(
			"System(Name=\"DreamFXAutomation/%s\",Root=\"Game\") { "
			"Emitter Source { Settings={RequiresPersistentIDs=true;} "
			"ParticleSpawn={V2/InitializeParticle();} "
			"ParticleUpdate={SolveForcesAndVelocity();GenerateLocationEvent();} } "
			"Emitter Child inherits \"%s\" { %s } }"), *AssetName, *Parent->GetPathName(), *Body);
	};
	const FGenerateResult Inherited = Build(*this, Document(FString()));
	if (!TestTrue(TEXT("omitted OnEvent compiles with inherited group"), Inherited.bSucceeded)) { return false; }
	const FNiagaraEmitterHandle* Handle = ChildHandle(System);
	if (!TestNotNull(TEXT("native event child handle"), Handle)) { return false; }
	const FGuid ChildId = Handle->GetId();
	TestEqual(TEXT("omitted OnEvent keeps both parent handlers"), Handle->GetEmitterData()->EventHandlerScriptProps.Num(), 2);
	TestTrue(TEXT("first native event assignment inherited"), HasAssignment(Handle->GetEmitterData(), TEXT("Particles.ParentEventOne")));
	TestTrue(TEXT("second native event assignment inherited"), HasAssignment(Handle->GetEmitterData(), TEXT("Particles.ParentEventTwo")));

	const FString Override = TEXT("OnEvent(Source=Source,Event=\"LocationEvent\",Mode=EveryParticle)={float Particles.ChildEvent=9.0;}");
	const FGenerateResult Replaced = Build(*this, Document(Override));
	if (!TestTrue(TEXT("explicit OnEvent group override compiles"), Replaced.bSucceeded)) { return false; }
	Handle = ChildHandle(System);
	const FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
	TestEqual(TEXT("event override keeps child handle GUID"), Handle->GetId(), ChildId);
	TestTrue(TEXT("event override keeps true parent"), Data->GetParent().Emitter == Parent);
	TestEqual(TEXT("event override keeps parent version"), Data->GetParent().Version, ParentVersion);
	if (TestEqual(TEXT("explicit event replaces entire two-handler group"), Data->EventHandlerScriptProps.Num(), 1))
	{
		TestEqual(TEXT("only DSL zero-id handler remains"), Data->EventHandlerScriptProps[0].Script->GetUsageId(), FGuid());
		TestEqual(TEXT("new event uses existing source handle"), Data->EventHandlerScriptProps[0].SourceEmitterID, SourceId);
	}
	TestTrue(TEXT("child event assignment installed"), HasAssignment(Data, TEXT("Particles.ChildEvent")));
	TestFalse(TEXT("first parent event assignment removed"), HasAssignment(Data, TEXT("Particles.ParentEventOne")));
	TestFalse(TEXT("second parent event assignment removed"), HasAssignment(Data, TEXT("Particles.ParentEventTwo")));
	TestTrue(TEXT("unrelated inherited spawn stack remains"), HasAssignment(Data, TEXT("Particles.ParentMarker")));
	const UNiagaraGraph* Graph = CastChecked<UNiagaraScriptSource>(Data->GraphSource)->NodeGraph;
	int32 EventOutputs = 0;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		TestFalse(TEXT("parent event graph leaves no exclusive orphan nodes"), ParentEventNodeGuids.Contains(Node->NodeGuid));
		if (const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);
			Output != nullptr && Output->GetUsage() == ENiagaraScriptUsage::ParticleEventScript) { ++EventOutputs; }
	}
	TestEqual(TEXT("only one event output remains"), EventOutputs, 1);
	TestEqual(TEXT("parent's own handlers were not modified"), Parent->GetLatestEmitterData()->EventHandlerScriptProps.Num(), 2);

	const FGenerateResult Restored = Build(*this, Document(FString()));
	if (!TestTrue(TEXT("removing OnEvent override compiles"), Restored.bSucceeded)) { return false; }
	Data = ChildHandle(System)->GetEmitterData();
	TestEqual(TEXT("omitting override restores both native handlers"), Data->EventHandlerScriptProps.Num(), 2);
	TestFalse(TEXT("removed override leaves no child event assignment"), HasAssignment(Data, TEXT("Particles.ChildEvent")));
	TestTrue(TEXT("restored native events retain original behavior"), HasAssignment(Data, TEXT("Particles.ParentEventOne"))
		&& HasAssignment(Data, TEXT("Particles.ParentEventTwo")));
	return true;
}

#endif
