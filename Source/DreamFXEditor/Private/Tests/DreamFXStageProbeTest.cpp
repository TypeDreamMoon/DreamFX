#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "NiagaraSystem.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "UObject/GCObjectScopeGuard.h"

/**
 * The plan-stages V1 risk probe: does the ZERO usage id work as a TIME SLICE for many stages?
 *
 * The event handler proved the zero id makes one off-stack script addressable through the external
 * edit API's existing rails (its references hard-code `FindScriptGroup(Usage, FGuid())`). Stages are
 * many per emitter, each with its own usage id, so the design leases the zero id one stage at a
 * time: Begin parks a stage on it, the rails write the stack, End moves the stage to its durable id
 * (the merge id, the engine's own convention). This test is that lease exercised end to end, plus
 * the risks the plan told it to probe: that a re-idded stage compiles, that regeneration lands on
 * the same identity, that the slice discipline refuses a second Begin, and that removal converges.
 *
 * Probe semantics: the system lives in /Temp and is never saved, same as the event probe.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSimStageZeroUsageIdProbe,
	"DreamFX.Probe.SimStageZeroUsageId",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSimStageZeroUsageIdProbe::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;

	TArray<FString> Errors;
	bool bCreated = false;
	UNiagaraSystem* System = FNiagaraAdapter::AcquireSystem(
		TEXT("/Temp/DreamFX"), TEXT("DreamFXStageProbe"), bCreated, Errors);
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
	if (!TestTrue(TEXT("add Sim emitter"),
		FNiagaraAdapter::AddEmitter(System, TEXT("Sim"), Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	const FStackAddress Emitter = SystemAddress.WithEmitter(TEXT("Sim"));

	// Stages are a GPU feature; on a CPU emitter Niagara refuses them at compile time.
	Errors.Reset();
	if (!TestTrue(TEXT("switch Sim to GPU"),
		FNiagaraAdapter::SetEmitterProperties(Emitter,
			TEXT("{\"SimTarget\":\"GPUComputeSim\",\"bFixedBounds\":true}"), Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	FModuleLibrary Modules;
	FString FindError;
	UNiagaraScript* Module = Modules.FindModule(TEXT("ScaleColor"), FindError);
	if (!TestNotNull(TEXT("ScaleColor module"), Module))
	{
		AddError(FindError);
		return false;
	}
	const FStackAddress StageStack = Emitter.WithScript(TEXT("ParticleSimulationStageScript"));

	// --- Slice one: StageA -------------------------------------------------------------------
	UE::DreamFX::FSimulationStageSpec SpecA;
	SpecA.Name = TEXT("StageA");
	Errors.Reset();
	if (!TestTrue(TEXT("Begin StageA"),
		FNiagaraAdapter::BeginSimulationStageEdit(Emitter, SpecA, /*DeclarationIndex=*/0, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	// Slice discipline: a second Begin while StageA holds the zero id must refuse.
	{
		UE::DreamFX::FSimulationStageSpec SpecX;
		SpecX.Name = TEXT("StageX");
		TArray<FString> RefusalErrors;
		TestFalse(TEXT("second Begin refused while the slice is held"),
			FNiagaraAdapter::BeginSimulationStageEdit(Emitter, SpecX, 1, RefusalErrors));
	}

	// The core claim: the zero-id stage stack resolves through the ordinary rails.
	FName AddedName;
	Errors.Reset();
	if (!TestTrue(TEXT("AddModule through the stage stack address"),
		FNiagaraAdapter::AddModule(StageStack, Module, AddedName, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	FScriptStackInfo StackInfo;
	Errors.Reset();
	if (TestTrue(TEXT("GetScriptStackInfo on the stage stack"),
		FNiagaraAdapter::GetScriptStackInfo(StageStack, StackInfo, Errors)))
	{
		TestEqual(TEXT("stage stack module count"), StackInfo.Modules.Num(), 1);
	}

	Errors.Reset();
	if (!TestTrue(TEXT("End StageA"),
		FNiagaraAdapter::EndSimulationStageEdit(Emitter, SpecA, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	// --- Slice two: StageB, DirectSet-free particles iteration with a count ------------------
	UE::DreamFX::FSimulationStageSpec SpecB;
	SpecB.Name = TEXT("StageB");
	SpecB.NumIterations = 4;
	Errors.Reset();
	if (!TestTrue(TEXT("Begin StageB"),
		FNiagaraAdapter::BeginSimulationStageEdit(Emitter, SpecB, 1, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	Errors.Reset();
	if (!TestTrue(TEXT("AddModule into StageB"),
		FNiagaraAdapter::AddModule(StageStack, Module, AddedName, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	Errors.Reset();
	if (!TestTrue(TEXT("End StageB"),
		FNiagaraAdapter::EndSimulationStageEdit(Emitter, SpecB, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}

	// Both stages durable, in declaration order, nobody on the zero id.
	TArray<FNiagaraAdapter::FSimulationStageSummary> Stages;
	Errors.Reset();
	FNiagaraAdapter::GetEmitterSimulationStages(Emitter, Stages, Errors);
	if (TestEqual(TEXT("stage count after two slices"), Stages.Num(), 2))
	{
		TestEqual(TEXT("stage order [0]"), Stages[0].StageName.ToString(), FString(TEXT("StageA")));
		TestEqual(TEXT("stage order [1]"), Stages[1].StageName.ToString(), FString(TEXT("StageB")));
		TestTrue(TEXT("StageA off the zero id"), Stages[0].ScriptUsageId != FGuid());
		TestTrue(TEXT("StageB off the zero id"), Stages[1].ScriptUsageId != FGuid());
		TestNotEqual(TEXT("distinct durable ids"), Stages[0].ScriptUsageId, Stages[1].ScriptUsageId);
		TestEqual(TEXT("StageB iteration count"), Stages[1].NumIterationsText, FString(TEXT("4")));
	}

	// Risk: a re-idded stage script must survive the compiler (VM half; GPU shaders are an
	// editor-only concern the commandlet's null RHI cannot see either way).
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

	// Risk: regeneration lands on the same identity. Re-editing StageA must leave two stages, and
	// its rebuilt stack must hold exactly what the new slice wrote.
	Errors.Reset();
	if (TestTrue(TEXT("Begin StageA again (regeneration)"),
		FNiagaraAdapter::BeginSimulationStageEdit(Emitter, SpecA, 0, Errors)))
	{
		TArray<FString> ClearErrors;
		FNiagaraAdapter::ClearScriptStack(StageStack, ClearErrors);
		Errors.Reset();
		TestTrue(TEXT("AddModule after regeneration"),
			FNiagaraAdapter::AddModule(StageStack, Module, AddedName, Errors));
		Errors.Reset();
		TestTrue(TEXT("End regenerated StageA"),
			FNiagaraAdapter::EndSimulationStageEdit(Emitter, SpecA, Errors));

		Stages.Reset();
		FNiagaraAdapter::GetEmitterSimulationStages(Emitter, Stages, Errors);
		TestEqual(TEXT("still two stages"), Stages.Num(), 2);
	}
	else
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
	}

	// The read half of the slice: focusing one stage exposes exactly its stack to the rails.
	Errors.Reset();
	if (TestTrue(TEXT("FocusSimulationStageForRead(1)"),
		FNiagaraAdapter::FocusSimulationStageForRead(Emitter, 1, Errors)))
	{
		FScriptStackInfo FocusedInfo;
		Errors.Reset();
		if (TestTrue(TEXT("stack info on the focused stage"),
			FNiagaraAdapter::GetScriptStackInfo(StageStack, FocusedInfo, Errors)))
		{
			TestEqual(TEXT("focused stage module count"), FocusedInfo.Modules.Num(), 1);
		}
		// Park everyone back on durable ids so removal sees a clean state.
		Stages.Reset();
		FNiagaraAdapter::GetEmitterSimulationStages(Emitter, Stages, Errors);
		if (Stages.Num() == 2)
		{
			TestTrue(TEXT("focused stage sits on the zero id"), Stages[1].ScriptUsageId == FGuid());
		}
		Errors.Reset();
		TestTrue(TEXT("unfocus by focusing an index then restoring"),
			FNiagaraAdapter::FocusSimulationStageForRead(Emitter, 0, Errors));
		Errors.Reset();
		UE::DreamFX::FSimulationStageSpec Refocus = SpecA;
		TestTrue(TEXT("End the focus-parked StageA"),
			FNiagaraAdapter::EndSimulationStageEdit(Emitter, Refocus, Errors));
	}
	else
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
	}

	// Removal converges: a source that stops declaring StageB actually deletes it.
	int32 Removed = 0;
	Errors.Reset();
	if (TestTrue(TEXT("RemoveUndeclaredSimulationStages"),
		FNiagaraAdapter::RemoveUndeclaredSimulationStages(Emitter,
			{ FName(TEXT("StageA")) }, Removed, Errors)))
	{
		TestEqual(TEXT("one stage removed"), Removed, 1);
		Stages.Reset();
		FNiagaraAdapter::GetEmitterSimulationStages(Emitter, Stages, Errors);
		if (TestEqual(TEXT("one stage left"), Stages.Num(), 1))
		{
			TestEqual(TEXT("the survivor is StageA"),
				Stages[0].StageName.ToString(), FString(TEXT("StageA")));
		}
	}
	else
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
