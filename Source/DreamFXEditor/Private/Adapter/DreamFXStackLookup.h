#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"

class FNiagaraSystemViewModel;
class UNiagaraStackFunctionInput;
class UNiagaraStackModuleItem;
class UNiagaraStackScriptItemGroup;
class UNiagaraSystem;

/**
 * The walk from a system view model down to one stack entry: build the view model, find the script
 * group, find the module, find the input.
 *
 * Two callers need it and for the same reason. The compatibility layer re-implements the external
 * edit API on engines that do not ship it, and the adapter reaches the same entries on 5.8, where the
 * API's own reference resolution (`FNiagaraExt_StackItemReference::GetInput`) and its context's view
 * model accessor are not exported, so an operation the API has no entry point for -- a reset to default
 * -- cannot borrow them. Every symbol used here carries NIAGARAEDITOR_API in the stock 5.8 install, and
 * it is the same set the compatibility layer already links against on 5.6 and 5.7.
 *
 * The functions are the compatibility layer's own, moved here unchanged, so there is one walk to keep
 * correct rather than two that agree today.
 */
namespace UE::DreamFX::Editor::StackLookup
{
	/**
	 * A view model configured exactly as the engine configures its own for external edits.
	 *
	 * Every flag matters in a commandlet. Data-processing-only keeps the model out of edit mode,
	 * and the other three keep it from starting work nothing will ever service: there is no editor
	 * tick to finish a simulation, and an auto-compile launched per view model is how a build turns
	 * into hundreds of aborted compiles. bCompileForEdit false additionally avoids the engine's own
	 * documented ensure when OnSystemCompiled lands on a partially destructed model.
	 */
	TSharedPtr<FNiagaraSystemViewModel> CreateSystemViewModel(UNiagaraSystem& System);

	/**
	 * ENiagaraScriptUsage from the name a stack reference carries.
	 *
	 * Both spellings are accepted -- "ParticleUpdateScript" and "ENiagaraScriptUsage::ParticleUpdateScript"
	 * -- because the API this mirrors is itself inconsistent about which it produces, and the adapter
	 * has a matching tolerance on its side. Rejecting the qualified form here would break exactly the
	 * read-then-write round trip that tolerance exists for.
	 */
	bool ScriptUsageFromName(FName ScriptName, ENiagaraScriptUsage& OutUsage);

	/** Every child of Entry, recursively, that is a T. Categories and value collections are walked through. */
	template <typename T>
	void CollectDescendants(UNiagaraStackEntry* Entry, TArray<T*>& Out, bool bStopAtT)
	{
		if (Entry == nullptr)
		{
			return;
		}

		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (T* Typed = Cast<T>(Child))
			{
				Out.Add(Typed);
				if (bStopAtT)
				{
					continue;
				}
			}
			CollectDescendants<T>(Child, Out, bStopAtT);
		}
	}

	/**
	 * The stack root for a system reference (EmitterName none) or an emitter reference.
	 *
	 * The engine reaches these through FNiagaraStackRootQuery, which is 5.8-only. The underlying walk
	 * is the same one written out: the system's own stack view model, or the emitter handle view
	 * model's, and their root entry.
	 */
	UNiagaraStackEntry* GetStackRoot(FNiagaraSystemViewModel& ViewModel, FName EmitterName);

	UNiagaraStackScriptItemGroup* FindScriptGroup(FNiagaraSystemViewModel& ViewModel, FName EmitterName,
		ENiagaraScriptUsage Usage);

	UNiagaraStackModuleItem* FindModuleItem(UNiagaraStackScriptItemGroup& Group, FName ModuleName);

	/**
	 * The inputs a module or a dynamic input exposes, flattened, in walk order.
	 *
	 * Descends INTO a function input's own children, and that is the whole point: a module's inputs are
	 * a *hierarchy*, and an input a static switch reveals is a child of the switch rather than a sibling
	 * of it (NiagaraStackFunctionInput.cpp, the HierarchyScriptParameter block -- one
	 * UNiagaraStackFunctionInput per hierarchy child whose variable the current configuration uses).
	 * `InitializeParticle`'s `Lifetime` hangs off `LifetimeMode`, `UniformSpriteSize` off
	 * `SpriteSizeMode`. A walk that stopped at the first input found the thirteen top-level entries and
	 * none of the conditional ones, so writing the switch appeared to do nothing and every argument it
	 * reveals was reported as a typo.
	 *
	 * Stops at a Dynamic-mode input, whose children are the dynamic input chain: those are addressed by
	 * pushing a name onto InputNameStack and belong to GetDynamicInputChain, not to this module's flat
	 * namespace. Same two rules as the engine's ForEachFunctionInput with
	 * { bRecurseIntoInputs = true, bRecurseIntoChainChildren = false }, which is what every 5.8 topology,
	 * value and resolve endpoint walks with.
	 */
	void CollectInputs(UNiagaraStackEntry* Entry, TArray<UNiagaraStackFunctionInput*>& Out);

	UNiagaraStackFunctionInput* FindInputByName(UNiagaraStackEntry* SearchRoot, FName InputName);
}
