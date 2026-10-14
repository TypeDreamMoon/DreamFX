#include "Adapter/DreamFXStackLookup.h"

#include "NiagaraSystem.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"
#include "NiagaraNodeFunctionCall.h"

namespace UE::DreamFX::Editor::StackLookup
{
	TSharedPtr<FNiagaraSystemViewModel> CreateSystemViewModel(UNiagaraSystem& System)
	{
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();

		FNiagaraSystemViewModelOptions Options;
		Options.bCanModifyEmittersFromTimeline = false;
		Options.bCompileForEdit = false;
		Options.bCanSimulate = false;
		Options.bCanAutoCompile = false;
		Options.bIsForDataProcessingOnly = true;
		Options.MessageLogGuid = System.GetAssetGuid();

		ViewModel->Initialize(System, Options);
		return ViewModel;
	}

	bool ScriptUsageFromName(FName ScriptName, ENiagaraScriptUsage& OutUsage)
	{
		const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
		if (UsageEnum == nullptr || ScriptName.IsNone())
		{
			return false;
		}

		FString Name = ScriptName.ToString();
		int32 ColonIndex;
		if (Name.FindLastChar(TEXT(':'), ColonIndex))
		{
			Name = Name.RightChop(ColonIndex + 1);
		}

		const int64 Value = UsageEnum->GetValueByNameString(Name);
		if (Value == INDEX_NONE)
		{
			return false;
		}

		OutUsage = static_cast<ENiagaraScriptUsage>(Value);
		return true;
	}

	UNiagaraStackEntry* GetStackRoot(FNiagaraSystemViewModel& ViewModel, FName EmitterName)
	{
		if (EmitterName.IsNone())
		{
			UNiagaraStackViewModel* SystemStack = ViewModel.GetSystemStackViewModel();
			return SystemStack != nullptr ? SystemStack->GetRootEntry() : nullptr;
		}

		for (const TSharedRef<FNiagaraEmitterHandleViewModel>& HandleViewModel : ViewModel.GetEmitterHandleViewModels())
		{
			if (HandleViewModel->GetName() == EmitterName)
			{
				UNiagaraStackViewModel* EmitterStack = HandleViewModel->GetEmitterStackViewModel();
				return EmitterStack != nullptr ? EmitterStack->GetRootEntry() : nullptr;
			}
		}
		return nullptr;
	}

	UNiagaraStackScriptItemGroup* FindScriptGroup(FNiagaraSystemViewModel& ViewModel, FName EmitterName,
		ENiagaraScriptUsage Usage)
	{
		UNiagaraStackEntry* Root = GetStackRoot(ViewModel, EmitterName);
		if (Root == nullptr)
		{
			return nullptr;
		}

		TArray<UNiagaraStackScriptItemGroup*> Groups;
		CollectDescendants<UNiagaraStackScriptItemGroup>(Root, Groups, /*bStopAtT=*/true);
		for (UNiagaraStackScriptItemGroup* Group : Groups)
		{
			if (Group->GetScriptUsage() == Usage)
			{
				return Group;
			}
		}
		return nullptr;
	}

	UNiagaraStackModuleItem* FindModuleItem(UNiagaraStackScriptItemGroup& Group, FName ModuleName)
	{
		TArray<UNiagaraStackModuleItem*> Modules;
		CollectDescendants<UNiagaraStackModuleItem>(&Group, Modules, /*bStopAtT=*/true);
		for (UNiagaraStackModuleItem* Module : Modules)
		{
			if (FName(*Module->GetModuleNode().GetFunctionName()) == ModuleName)
			{
				return Module;
			}
		}
		return nullptr;
	}

	void CollectInputs(UNiagaraStackEntry* Entry, TArray<UNiagaraStackFunctionInput*>& Out)
	{
		if (Entry == nullptr)
		{
			return;
		}

		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
			{
				Out.Add(Input);
				if (Input->GetValueMode() != UNiagaraStackFunctionInput::EValueMode::Dynamic)
				{
					CollectInputs(Input, Out);
				}
			}
			else
			{
				// A category, a value collection, or the hierarchy root a dynamic input hangs its chain
				// from -- containers the flat namespace reads through.
				CollectInputs(Child, Out);
			}
		}
	}

	UNiagaraStackFunctionInput* FindInputByName(UNiagaraStackEntry* SearchRoot, FName InputName)
	{
		TArray<UNiagaraStackFunctionInput*> Inputs;
		CollectInputs(SearchRoot, Inputs);
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			if (Input->GetInputParameterHandle().GetName() == InputName)
			{
				return Input;
			}
		}
		return nullptr;
	}
}
