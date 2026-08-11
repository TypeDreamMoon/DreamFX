#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "NiagaraSystem.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "UObject/GCObjectScopeGuard.h"

/**
 * The plan-events V1 risk probe: does a ZERO usage id make an event stack addressable through the
 * external edit API's existing rails?
 *
 * The whole write-side design rests on one measured fact -- the API's stack references hard-code
 * `FindScriptGroup(Usage, FGuid())`, so an event script whose usage id IS the zero guid matches the
 * only query the rails can ask. This test is that fact exercised end to end, plus the risks the plan
 * told it to probe: that a zero-id event script compiles, and that regeneration lands on the same
 * identity instead of accreting handlers and graph nodes.
 *
 * Probe semantics: the system lives in /Temp and is never saved, same as the decompiler's emitter
 * host. Reload survival is deliberately NOT probed here -- that needs a saved asset, and it is what
 * the V3 corpus round-trip cases cover once the language exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXEventHandlerZeroUsageIdProbe,
	"DreamFX.Probe.EventHandlerZeroUsageId",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXEventHandlerZeroUsageIdProbe::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;

	TArray<FString> Errors;
	bool bCreated = false;
	UNiagaraSystem* System = FNiagaraAdapter::AcquireSystem(
		TEXT("/Temp/DreamFX"), TEXT("DreamFXEventProbe"), bCreated, Errors);
	if (!TestNotNull(TEXT("probe system"), System))
	{
		return false;
	}
	FGCObjectScopeGuard SystemGuard(System);

	// A re-run in the same session finds the previous copy; start from nothing.
	{
		TArray<FName> Existing;
		TArray<FString> ReadErrors;
		if (FNiagaraAdapter::GetEmitterNames(System, Existing, ReadErrors))
		{
			for (FName Name : Existing)
			{
				TArray<FString> RemoveErrors;
				FNiagaraAdapter::RemoveEmitter(FStackAddress(System).WithEmitter(Name), RemoveErrors);
			}
		}
	}

	const FStackAddress SystemAddress(System);
	Errors.Reset();
	if (!TestTrue(TEXT("add Source emitter"),
		FNiagaraAdapter::AddEmitter(System, TEXT("Source"), Errors)))
	{
		AddErrorIfFalse(false, FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	Errors.Reset();
	if (!TestTrue(TEXT("add Receiver emitter"),
		FNiagaraAdapter::AddEmitter(System, TEXT("Receiver"), Errors)))
	{
		AddErrorIfFalse(false, FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	// An event generator reads Particles.ID, and Niagara refuses that without persistent ids -- the
	// same emitter property real sources carry and the Settings block round-trips.
	Errors.Reset();
	if (!TestTrue(TEXT("enable persistent ids on Source"),
		FNiagaraAdapter::SetEmitterProperties(SystemAddress.WithEmitter(TEXT("Source")),
			TEXT("{\"bRequiresPersistentIDs\":true}"), Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	// The source generates the events. Resolved through the module library so a renamed module
	// fails with the candidate list instead of a null.
	FModuleLibrary Modules;
	FString FindError;
	UNiagaraScript* GenerateEvent = Modules.FindModule(TEXT("GenerateLocationEvent"), FindError);
	if (!TestNotNull(TEXT("GenerateLocationEvent module"), GenerateEvent))
	{
		AddError(FindError);
		return false;
	}
	const FStackAddress SourceUpdate = SystemAddress.WithEmitter(TEXT("Source"))
		.WithScript(TEXT("ParticleUpdateScript"));
	FName AddedName;
	Errors.Reset();
	if (!TestTrue(TEXT("add GenerateLocationEvent to Source"),
		FNiagaraAdapter::AddModule(SourceUpdate, GenerateEvent, AddedName, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	// The handler itself: zero usage id, source spoken by name.
	const FStackAddress Receiver = SystemAddress.WithEmitter(TEXT("Receiver"));
	Errors.Reset();
	if (!TestTrue(TEXT("AddEventHandler"),
		FNiagaraAdapter::AddEventHandler(Receiver, TEXT("Source"), TEXT("LocationEvent"),
			TEXT("SpawnedParticles"), 1, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	// The core claim: the zero-id event stack resolves through the ordinary rails.
	UNiagaraScript* ReceiveEvent = Modules.FindModule(TEXT("ReceiveLocationEvent"), FindError);
	if (!TestNotNull(TEXT("ReceiveLocationEvent module"), ReceiveEvent))
	{
		AddError(FindError);
		return false;
	}
	const FStackAddress EventStack = Receiver.WithScript(TEXT("ParticleEventScript"));
	Errors.Reset();
	if (!TestTrue(TEXT("AddModule through the event stack address"),
		FNiagaraAdapter::AddModule(EventStack, ReceiveEvent, AddedName, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	FScriptStackInfo StackInfo;
	Errors.Reset();
	if (!TestTrue(TEXT("GetScriptStackInfo on the event stack"),
		FNiagaraAdapter::GetScriptStackInfo(EventStack, StackInfo, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	TestEqual(TEXT("event stack module count"), StackInfo.Modules.Num(), 1);

	// The read side of the summary agrees with what was written.
	TArray<FNiagaraAdapter::FEventHandlerSummary> Handlers;
	Errors.Reset();
	FNiagaraAdapter::GetEmitterEventHandlers(Receiver, Handlers, Errors);
	if (TestEqual(TEXT("handler count"), Handlers.Num(), 1))
	{
		TestEqual(TEXT("handler source"), Handlers[0].SourceEmitterName, FString(TEXT("Source")));
		TestEqual(TEXT("handler event"), Handlers[0].SourceEventName.ToString(), FString(TEXT("LocationEvent")));
		TestEqual(TEXT("handler mode"), Handlers[0].ExecutionMode, FString(TEXT("SpawnedParticles")));
		TestEqual(TEXT("handler spawn number"), Handlers[0].SpawnNumber, 1);
	}

	// Risk 2: a zero-id event script must survive the compiler.
	FCompileStateInfo CompileState;
	Errors.Reset();
	const bool bCompiled = FNiagaraAdapter::CompileAndWait(System, /*bIncludingGpuShaders=*/false,
		CompileState, Errors);
	if (!(bCompiled && !CompileState.bHasErrors))
	{
		for (const FCompileEventInfo& Event : CompileState.Events)
		{
			AddWarning(FString::Printf(TEXT("compile event [%d] %s/%s: %s"), Event.Severity,
				*Event.EmitterName.ToString(), *Event.ScriptName.ToString(), *Event.Message));
		}
	}
	TestTrue(FString::Printf(TEXT("compile clean (status %s%s)"), *CompileState.StatusName,
		Errors.Num() > 0 ? *(TEXT(": ") + FString::Join(Errors, TEXT(" | "))) : TEXT("")),
		bCompiled && !CompileState.bHasErrors);

	// Risk 3: regeneration lands on the same identity. A second AddEventHandler must leave exactly
	// one handler, and the event stack must still resolve -- but the modules are gone with the old
	// script, which is the rebuild's own job to repopulate, so only addressability is asserted.
	Errors.Reset();
	if (TestTrue(TEXT("AddEventHandler again (regeneration)"),
		FNiagaraAdapter::AddEventHandler(Receiver, TEXT("Source"), TEXT("LocationEvent"),
			TEXT("SpawnedParticles"), 2, Errors)))
	{
		Handlers.Reset();
		FNiagaraAdapter::GetEmitterEventHandlers(Receiver, Handlers, Errors);
		if (TestEqual(TEXT("still exactly one handler"), Handlers.Num(), 1))
		{
			TestEqual(TEXT("regenerated spawn number"), Handlers[0].SpawnNumber, 2);
		}

		FScriptStackInfo RegeneratedInfo;
		Errors.Reset();
		TestTrue(TEXT("event stack still addressable after regeneration"),
			FNiagaraAdapter::GetScriptStackInfo(EventStack, RegeneratedInfo, Errors));
	}
	else
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
