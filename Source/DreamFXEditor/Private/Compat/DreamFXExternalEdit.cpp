// The external-edit API, re-implemented for engines that do not ship it. See the header for why this
// exists and why the types are named the way they are.
//
// The whole file is guarded rather than excluded from the build: UnrealBuildTool compiles every .cpp
// under a module directory, and an empty translation unit is the cheapest way to say "not this engine".

#if !DREAMFX_HAS_NIAGARA_EXTERNAL_EDIT

#include "NiagaraExternalSystemEditorUtilities.h"

#include "DreamFXModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "EdGraphSchema_Niagara.h"
#include "JsonObjectConverter.h"
#include "Misc/EngineVersionComparison.h"
#include "NiagaraConstants.h"
#include "NiagaraDataInterface.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "NiagaraVariant.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "DreamFXExternalEditCompat"

namespace
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

	/**
	 * ENiagaraScriptUsage from the name a stack reference carries.
	 *
	 * Both spellings are accepted -- "ParticleUpdateScript" and "ENiagaraScriptUsage::ParticleUpdateScript"
	 * -- because the API this mirrors is itself inconsistent about which it produces, and the adapter
	 * has a matching tolerance on its side. Rejecting the qualified form here would break exactly the
	 * read-then-write round trip that tolerance exists for.
	 */
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

	FName NameForScriptUsage(ENiagaraScriptUsage Usage)
	{
		const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
		return UsageEnum != nullptr ? UsageEnum->GetNameByValue(static_cast<int64>(Usage)) : NAME_None;
	}

	FNiagaraEmitterHandle* FindEmitterHandle(UNiagaraSystem* System, FName EmitterName)
	{
		if (System == nullptr || EmitterName.IsNone())
		{
			return nullptr;
		}

		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.GetName() == EmitterName)
			{
				return &Handle;
			}
		}
		return nullptr;
	}

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

	// ---- resolution against a reference -------------------------------------------------------

	UNiagaraStackScriptItemGroup* ResolveScript(const FDFXExt_StackItemReference& Ref,
		FDreamFXExternalEditContext& Context)
	{
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Context.GetSystemViewModel();
		if (!ViewModel.IsValid())
		{
			Context.Error(LOCTEXT("NoViewModel", "No system to resolve this reference against."));
			return nullptr;
		}

		ENiagaraScriptUsage Usage;
		if (!ScriptUsageFromName(Ref.ScriptName, Usage))
		{
			Context.Error(FText::Format(
				LOCTEXT("BadScriptName", "Invalid script name '{0}'. Expected an ENiagaraScriptUsage entry."),
				FText::FromName(Ref.ScriptName)));
			return nullptr;
		}

		UNiagaraStackScriptItemGroup* Group = FindScriptGroup(*ViewModel, Ref.EmitterName, Usage);
		if (Group == nullptr)
		{
			Context.Error(FText::Format(
				LOCTEXT("ScriptNotFound", "Script '{0}' not found on '{1}'."),
				FText::FromName(Ref.ScriptName), FText::FromName(Ref.EmitterName)));
		}
		return Group;
	}

	UNiagaraStackModuleItem* ResolveModule(const FDFXExt_StackItemReference& Ref,
		FDreamFXExternalEditContext& Context)
	{
		UNiagaraStackScriptItemGroup* Group = ResolveScript(Ref, Context);
		if (Group == nullptr)
		{
			return nullptr;
		}

		UNiagaraStackModuleItem* Module = FindModuleItem(*Group, Ref.ModuleName);
		if (Module == nullptr)
		{
			Context.Error(FText::Format(
				LOCTEXT("ModuleNotFound", "Module '{0}' not found in script '{1}'."),
				FText::FromName(Ref.ModuleName), FText::FromName(Ref.ScriptName)));
		}
		return Module;
	}

	UNiagaraStackFunctionInput* ResolveInput(const FDFXExt_StackItemReference& Ref,
		FDreamFXExternalEditContext& Context)
	{
		if (Ref.InputNameStack.Num() == 0)
		{
			Context.Error(LOCTEXT("NoInputName", "No input name in stack reference."));
			return nullptr;
		}

		UNiagaraStackModuleItem* Module = ResolveModule(Ref, Context);
		if (Module == nullptr)
		{
			return nullptr;
		}

		// Name 0 resolves against the module; names 1..N drill the dynamic input chain, and each step
		// but the last has to be a dynamic input for the next name to mean anything.
		UNiagaraStackEntry* SearchRoot = Module;
		UNiagaraStackFunctionInput* Found = nullptr;
		for (int32 Index = 0; Index < Ref.InputNameStack.Num(); ++Index)
		{
			if (Index > 0 && Found->GetValueMode() != UNiagaraStackFunctionInput::EValueMode::Dynamic)
			{
				Context.Error(FText::Format(
					LOCTEXT("InputNotDynamic", "Input '{0}' is not a dynamic input, so it exposes no chain."),
					FText::FromName(Ref.InputNameStack[Index - 1])));
				return nullptr;
			}

			Found = FindInputByName(SearchRoot, Ref.InputNameStack[Index]);
			if (Found == nullptr)
			{
				Context.Error(FText::Format(
					LOCTEXT("InputNotFound", "Input '{0}' not found on module '{1}'."),
					FText::FromName(Ref.InputNameStack[Index]), FText::FromName(Ref.ModuleName)));
				return nullptr;
			}
			SearchRoot = Found;
		}
		return Found;
	}

	UNiagaraRendererProperties* ResolveRenderer(const FDFXExt_StackItemReference& Ref,
		FDreamFXExternalEditContext& Context)
	{
		FNiagaraEmitterHandle* Handle = FindEmitterHandle(Ref.System, Ref.EmitterName);
		if (Handle == nullptr)
		{
			Context.Error(FText::Format(
				LOCTEXT("EmitterNotFound", "Emitter '{0}' not found."), FText::FromName(Ref.EmitterName)));
			return nullptr;
		}

		FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
		if (EmitterData == nullptr || !EmitterData->GetRenderers().IsValidIndex(Ref.RendererIndex))
		{
			Context.Error(FText::Format(
				LOCTEXT("RendererNotFound", "Renderer {0} not found on emitter '{1}'."),
				FText::AsNumber(Ref.RendererIndex), FText::FromName(Ref.EmitterName)));
			return nullptr;
		}
		return EmitterData->GetRenderers()[Ref.RendererIndex];
	}

	// ---- property JSON ------------------------------------------------------------------------

	/**
	 * The default property provider, which is all this layer ever needs.
	 *
	 * The engine's default is JsonUtilities-based and so is this: the two have to agree, because a
	 * blob written on MoonEngine is read back here and both sides feed the same applier in DreamFX.
	 */
	const FNiagaraExternalEditPropertyProvider& GetDefaultProvider()
	{
		static FNiagaraExternalEditPropertyProvider Provider = []
		{
			FNiagaraExternalEditPropertyProvider Default;

			Default.ListStructProperties = [](const UStruct* Struct, bool) -> FString
			{
				// No schema generator outside 5.8. Nothing in DreamFX reads this -- the adapter builds
				// its own property lists by reflection -- so an empty object is honest rather than wrong.
				return Struct != nullptr ? TEXT("{}") : FString();
			};

			Default.GetObjectProperties = [](const UObject* Object, const TArray<FName>& PropertyNames) -> FString
			{
				if (Object == nullptr)
				{
					return FString();
				}

				TSharedRef<FJsonObject> JsonObject = MakeShared<FJsonObject>();
				for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
				{
					if (PropertyNames.Num() > 0 && !PropertyNames.Contains(It->GetFName()))
					{
						continue;
					}

					const void* Value = It->ContainerPtrToValuePtr<void>(Object);
					TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(*It, Value);
					if (JsonValue.IsValid())
					{
						JsonObject->SetField(It->GetName(), JsonValue);
					}
				}

				FString Json;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
				FJsonSerializer::Serialize(JsonObject, Writer);
				return Json;
			};

			Default.SetObjectProperties = [](UObject* Object, const FString& PropertiesJson) -> bool
			{
				if (Object == nullptr || PropertiesJson.IsEmpty())
				{
					return false;
				}

				TSharedPtr<FJsonObject> JsonObject;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PropertiesJson);
				if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
				{
					return false;
				}

				Object->Modify();
				const bool bApplied = FJsonObjectConverter::JsonObjectToUStruct(
					JsonObject.ToSharedRef(), Object->GetClass(), Object, 0, 0);
				Object->PostEditChange();
				return bApplied;
			};

			return Default;
		}();

		return Provider;
	}

	FString ObjectPropertiesToJson(const UObject* Object)
	{
		return Object != nullptr ? GetDefaultProvider().GetObjectProperties(Object, TArray<FName>()) : FString();
	}

	/** The emitter property blob, which describes the emitter DATA rather than the UObject wrapper. */
	FString EmitterDataToJson(FVersionedNiagaraEmitterData* EmitterData)
	{
		if (EmitterData == nullptr)
		{
			return FString();
		}

		FString Json;
		FJsonObjectConverter::UStructToJsonObjectString(
			FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, Json, 0, 0);
		return Json;
	}

	EDFXExt_ScriptCompileStatus ToCompileStatus(ENiagaraScriptCompileStatus Status)
	{
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Dirty:                       return EDFXExt_ScriptCompileStatus::Dirty;
		case ENiagaraScriptCompileStatus::NCS_Error:                       return EDFXExt_ScriptCompileStatus::Error;
		case ENiagaraScriptCompileStatus::NCS_UpToDate:                    return EDFXExt_ScriptCompileStatus::UpToDate;
		case ENiagaraScriptCompileStatus::NCS_BeingCreated:                return EDFXExt_ScriptCompileStatus::BeingCreated;
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:        return EDFXExt_ScriptCompileStatus::UpToDateWithWarnings;
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings: return EDFXExt_ScriptCompileStatus::ComputeUpToDateWithWarnings;
		default:                                                           return EDFXExt_ScriptCompileStatus::Unknown;
		}
	}

	EDFXExt_CompileEventSeverity ToEventSeverity(FNiagaraCompileEventSeverity Severity)
	{
		switch (Severity)
		{
		case FNiagaraCompileEventSeverity::Error:   return EDFXExt_CompileEventSeverity::Error;
		case FNiagaraCompileEventSeverity::Warning: return EDFXExt_CompileEventSeverity::Warning;
		case FNiagaraCompileEventSeverity::Display: return EDFXExt_CompileEventSeverity::Display;
		default:                                    return EDFXExt_CompileEventSeverity::Log;
		}
	}

	/** Fills one input's topology entry. Shared by the module walk and the dynamic input chain walk. */
	void FillInputTopology(UNiagaraStackFunctionInput& Input, FDFXExt_StackInputTopology& Out)
	{
		Out.Name = Input.GetInputParameterHandle().GetName();
		Out.Type = Input.GetInputType();
		Out.bIsStaticSwitch = Input.IsStaticParameter();
		Out.bIsDynamic = Input.GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Dynamic;

		// The three gating axes, composed exactly as GetStackInputTopology composes them: the runtime
		// hidden flag (static switch and conditional logic), VisibleCondition, and EditCondition. Hidden
		// or VisibleCondition-false is neither visible nor editable; EditCondition-false is visible but
		// not editable, which is why editable is an AND rather than a copy.
		//
		// Not GetShouldShowInStack and GetIsEnabled, which were the earlier reading of the same three
		// gates and are neither: on 5.6 and 5.7 the first answers only the stack's show-only-modified
		// filter and the second the owning node's enabled state, so every input read back visible and
		// editable. That was harmless while the walk stopped at the top level -- nothing hides a
		// top-level input -- and stops being harmless the moment conditional children are collected,
		// because a hierarchy child of an unselected branch is present and hidden. The decompiler is the
		// reader that would have been fooled: it skips !bVisible || !bEditable, and would otherwise have
		// written out inputs the engine considers gated off.
		const bool bHidden = Input.GetIsHidden();
		const bool bVisibleConditionPasses = Input.GetHasVisibleCondition() ? Input.GetVisibleConditionEnabled() : true;
		const bool bEditConditionPasses = Input.GetHasEditCondition() ? Input.GetEditConditionEnabled() : true;
		Out.bIsVisible = !bHidden && bVisibleConditionPasses;
		Out.bIsEditable = Out.bIsVisible && bEditConditionPasses;
	}

	void FillModuleTopology(UNiagaraStackModuleItem& Module, FDFXExt_ModuleTopology& Out)
	{
		UNiagaraNodeFunctionCall& Node = Module.GetModuleNode();
		Out.ModuleName = FName(*Node.GetFunctionName());
		Out.Enabled = Module.GetIsEnabled();
		Out.ModuleScript = Node.FunctionScript;
		Out.bIsSetParametersModule = Node.IsA<UNiagaraNodeAssignment>();

		TArray<UNiagaraStackFunctionInput*> Inputs;
		CollectInputs(&Module, Inputs);
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			FillInputTopology(*Input, Out.Inputs.AddDefaulted_GetRef());
		}
	}

	/** Reads one input's current value into the instanced payload the adapter expects. */
	void FillInputValue(UNiagaraStackFunctionInput& Input, FDFXExt_StackInputValue& Out)
	{
		switch (Input.GetValueMode())
		{
		case UNiagaraStackFunctionInput::EValueMode::Local:
		{
			TSharedPtr<const FStructOnScope> Local = Input.GetLocalValueStruct();
			if (Local.IsValid() && Local->GetStruct() != nullptr)
			{
				const UScriptStruct* AsScriptStruct = Cast<const UScriptStruct>(Local->GetStruct());
				if (AsScriptStruct != nullptr)
				{
					// An enum-typed input reads back as its raw int here. The adapter's own schema pass
					// is what turns that back into a name, exactly as it does for the engine's path when
					// the engine reports a literal.
					Out.InitializeAs(AsScriptStruct, Local->GetStructMemory());
					return;
				}
			}
			Out.InitializeAs<FDFXExt_StackInputData_Unsupported>();
			return;
		}

		case UNiagaraStackFunctionInput::EValueMode::Linked:
		{
			FDFXExt_StackInputData_Linked& Data = Out.InitializeAs<FDFXExt_StackInputData_Linked>();
			const FNiagaraVariableBase& Linked = Input.GetLinkedParameterValue();
			Data.LinkedVariable.Name = Linked.GetName();
			Data.LinkedVariable.Type = Linked.GetType();
			return;
		}

		case UNiagaraStackFunctionInput::EValueMode::Dynamic:
		{
			FDFXExt_StackInputData_DynamicInput& Data = Out.InitializeAs<FDFXExt_StackInputData_DynamicInput>();
			if (UNiagaraNodeFunctionCall* DynamicNode = Input.GetDynamicInputNode())
			{
				Data.DynamicInputAsset = DynamicNode->FunctionScript;
			}
			return;
		}

		case UNiagaraStackFunctionInput::EValueMode::Expression:
		{
			FDFXExt_StackInputData_HlslExpression& Data = Out.InitializeAs<FDFXExt_StackInputData_HlslExpression>();
			Data.HlslExpression = Input.GetCustomExpressionText().ToString();
			return;
		}

		case UNiagaraStackFunctionInput::EValueMode::Data:
		{
			FDFXExt_StackInputData_DataInterface& Data = Out.InitializeAs<FDFXExt_StackInputData_DataInterface>();
			Data.PropertyValues = ObjectPropertiesToJson(Input.GetDataValueObject());
			return;
		}

		default:
			// ObjectAsset, DefaultFunction, InvalidOverride, None: nothing this layer can express as a
			// value. Unsupported is the payload the adapter already reads as "leave it alone", which is
			// the right answer for a default nobody set.
			Out.InitializeAs<FDFXExt_StackInputData_Unsupported>();
			return;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Context

FDreamFXExternalEditContext::FDreamFXExternalEditContext(UNiagaraSystem* InSystem)
	: System(InSystem)
{
}

FDreamFXExternalEditContext::~FDreamFXExternalEditContext() = default;

TSharedPtr<FNiagaraSystemViewModel> FDreamFXExternalEditContext::GetSystemViewModel()
{
	if (!SystemViewModel.IsValid() && System != nullptr)
	{
		SystemViewModel = CreateSystemViewModel(*System);
	}
	return SystemViewModel;
}

// ------------------------------------------------------------------------------------------------
// Values

void FDFXExt_VariableValue::Get(FNiagaraVariant& OutVariant, FDreamFXExternalEditContext& Context) const
{
	if (const FDFXExt_VariableValue_DataInterface* AsDataInterface = GetPtr<FDFXExt_VariableValue_DataInterface>())
	{
		OutVariant = FNiagaraVariant(AsDataInterface->DataInterface.Get());
		return;
	}
	if (const FDFXExt_VariableValue_Object* AsObject = GetPtr<FDFXExt_VariableValue_Object>())
	{
		OutVariant = FNiagaraVariant(AsObject->Object.Get());
		return;
	}
	if (IsValid())
	{
		OutVariant = FNiagaraVariant(GetMemory(), GetScriptStruct()->GetStructureSize());
		return;
	}
	OutVariant = FNiagaraVariant();
}

void FDFXExt_VariableValue::Set(const FNiagaraTypeDefinition& TypeDef, const FNiagaraVariant& Variant)
{
	if (Variant.GetMode() == ENiagaraVariantMode::DataInterface)
	{
		FDFXExt_VariableValue_DataInterface& Data = InitializeAs<FDFXExt_VariableValue_DataInterface>();
		Data.DataInterface = Variant.GetDataInterface();
		Data.DataInterfaceClass = Data.DataInterface != nullptr ? Data.DataInterface->GetClass() : nullptr;
		return;
	}
	if (Variant.GetMode() == ENiagaraVariantMode::Object)
	{
		FDFXExt_VariableValue_Object& Data = InitializeAs<FDFXExt_VariableValue_Object>();
		Data.Object = Variant.GetUObject();
		Data.ObjectClass = Data.Object != nullptr ? Data.Object->GetClass() : nullptr;
		return;
	}
	if (Variant.GetMode() == ENiagaraVariantMode::Bytes && TypeDef.GetScriptStruct() != nullptr)
	{
		InitializeAs(TypeDef.GetScriptStruct(), Variant.GetBytes());
		return;
	}
	Reset();
}

// ------------------------------------------------------------------------------------------------
// Creation

UNiagaraSystem* FDreamFXExternalEditUtilities::CreateNiagaraSystem(const FString& AssetName,
	const FString& AssetPath, UNiagaraSystem* TemplateSystem, FDreamFXExternalEditContext& Context)
{
	if (AssetName.IsEmpty() || AssetPath.IsEmpty())
	{
		Context.Error(LOCTEXT("CreateNoName", "A new system needs both a name and a package path."));
		return nullptr;
	}

	const FString PackageName = AssetPath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (Package == nullptr)
	{
		Context.Error(FText::Format(
			LOCTEXT("CreateNoPackage", "Could not create package '{0}'."), FText::FromString(PackageName)));
		return nullptr;
	}
	Package->FullyLoad();

	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (System == nullptr)
	{
		Context.Error(FText::Format(
			LOCTEXT("CreateFailed", "Could not create system '{0}'."), FText::FromString(AssetName)));
		return nullptr;
	}

	if (TemplateSystem != nullptr)
	{
		// Templates are not part of the path DreamFX takes -- it always builds from empty and adds --
		// so rather than half-implement a copy that nothing exercises, say so.
		Context.Error(LOCTEXT("CreateTemplateUnsupported",
			"Creating a system from a template is not supported on an engine without the external edit API."));
		return nullptr;
	}

	UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
	System->RequestCompile(false);
	return System;
}

void FDreamFXExternalEditUtilities::GetAvailableDynamicInputs(const FNiagaraTypeDefinition& Type,
	TArray<UNiagaraScript*>& OutDynamicInputScripts, FDreamFXExternalEditContext& Context)
{
	const FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;

	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets)
	{
		UNiagaraScript* Script = Cast<UNiagaraScript>(Asset.GetAsset());
		if (Script == nullptr || !Script->IsEquivalentUsage(ENiagaraScriptUsage::DynamicInput))
		{
			continue;
		}

		// Type filtering is the caller's job in the adapter (it compares the schema's output type), so
		// an unfiltered list here is a superset rather than a wrong answer.
		OutDynamicInputScripts.Add(Script);
	}
}

const FNiagaraExternalEditPropertyProvider& FDreamFXExternalEditUtilities::GetPropertyProvider()
{
	return GetDefaultProvider();
}

// ------------------------------------------------------------------------------------------------
// Schema

namespace
{
	/**
	 * A module asset's inputs, read from its graph rather than from a stack.
	 *
	 * GetStackFunctionInputs is the engine's own enumerator and is exported from 5.6 on -- which is
	 * precisely the line below which this whole layer stops being possible, because on 5.3 through 5.5
	 * the same declaration carries no export macro.
	 */
	void FillModuleSchema(const UNiagaraScript* ModuleAsset, FDFXExt_ModuleSchema& OutSchema,
		FDreamFXExternalEditContext& Context)
	{
		if (ModuleAsset == nullptr)
		{
			Context.Error(LOCTEXT("SchemaNoAsset", "No script asset to read a schema from."));
			return;
		}

		OutSchema.Asset = ModuleAsset;

		UNiagaraScriptSource* Source =
			Cast<UNiagaraScriptSource>(const_cast<UNiagaraScript*>(ModuleAsset)->GetLatestSource());
		UNiagaraGraph* Graph = Source != nullptr ? Source->NodeGraph : nullptr;
		if (Graph == nullptr)
		{
			Context.Error(FText::Format(
				LOCTEXT("SchemaNoGraph", "Script '{0}' has no graph."), FText::FromString(ModuleAsset->GetName())));
			return;
		}

		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);

		TMap<FNiagaraVariable, FNiagaraVariableMetaData> MetaDataByVariable;
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Graph->GetAllMetaData())
		{
			if (Entry.Value != nullptr)
			{
				MetaDataByVariable.Add(Entry.Key, Entry.Value->Metadata);
			}
		}

		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			for (const FNiagaraVariable& Output : OutputNode->Outputs)
			{
				FDFXExt_Variable& OutVariable = OutSchema.Outputs.AddDefaulted_GetRef();
				OutVariable.Name = Output.GetName();
				OutVariable.Type = Output.GetType();
			}
		}

		// Module inputs are the graph's Module.* parameters. Reading them from the parameter map
		// rather than from a stack is what lets the schema be read without an owning system, which is
		// the whole reason this overload exists.
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Graph->GetAllMetaData())
		{
			const FNiagaraVariable& Variable = Entry.Key;

			// Matched on the namespace prefix rather than FNiagaraConstants, whose spelling of this
			// one constant moved between the engines this file has to build against. The prefix has
			// not moved since Niagara shipped and is what the namespace helpers compare anyway.
			if (!Variable.GetName().ToString().StartsWith(TEXT("Module.")))
			{
				continue;
			}

			FDFXExt_StackInputSchema& Input = OutSchema.Inputs.AddDefaulted_GetRef();
			Input.Name = Variable.GetName();
			Input.Type = Variable.GetType();
			Input.bSupportsExpressions = Variable.GetType().IsValid() && !Variable.GetType().IsDataInterface();
			if (Entry.Value != nullptr)
			{
				// Category is left empty on purpose: the field the engine fills it from is deprecated
				// on 5.6 through 5.8 alike and populated only on assets old enough to still carry it.
				// It groups inputs in an export and nothing reads it back, so an empty one is cosmetic.
				Input.MetaData = Entry.Value->Metadata;
			}
		}
	}
}

void FDreamFXExternalEditUtilities::GetModuleSchema(const UNiagaraScript* ModuleAsset,
	FDFXExt_ModuleSchema& OutSchema, FDreamFXExternalEditContext& Context)
{
	FillModuleSchema(ModuleAsset, OutSchema, Context);
}

void FDreamFXExternalEditUtilities::GetDynamicInputSchema(const UNiagaraScript* ModuleAsset,
	FDFXExt_DynamicInputSchema& OutSchema, FDreamFXExternalEditContext& Context)
{
	FillModuleSchema(ModuleAsset, OutSchema, Context);
}

void FDreamFXExternalEditUtilities::GetStackInputSchema(const FDFXExt_StackItemReference& InputReference,
	FDFXExt_StackInputSchema& OutSchema, FDreamFXExternalEditContext& Context)
{
	UNiagaraStackFunctionInput* Input = ResolveInput(InputReference, Context);
	if (Input == nullptr)
	{
		return;
	}

	OutSchema.Name = Input->GetInputParameterHandle().GetName();
	OutSchema.Type = Input->GetInputType();
	OutSchema.bSupportsExpressions = !Input->GetInputType().IsDataInterface();

	const TOptional<FNiagaraVariableMetaData> MetaData = Input->GetInputMetaData();
	if (MetaData.IsSet())
	{
		// Category deliberately not filled -- see the note in FillModuleSchema.
		OutSchema.MetaData = MetaData.GetValue();
	}
}

// ------------------------------------------------------------------------------------------------
// Summary and topology

void FDreamFXExternalEditUtilities::GetSystemSummary(UNiagaraSystem* System,
	FDFXExt_SystemSummary& OutSummary, FDreamFXExternalEditContext& Context)
{
	if (System == nullptr)
	{
		Context.Error(LOCTEXT("SummaryNoSystem", "No system to summarise."));
		return;
	}

	OutSummary.SystemName = System->GetFName();

	FDFXExt_UserVariables UserVariables;
	GetUserVariables(System, UserVariables, Context);
	OutSummary.UserVariables = MoveTemp(UserVariables.UserVariables);

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FDFXExt_EmitterSummary& Summary = OutSummary.Emitters.AddDefaulted_GetRef();
		Summary.EmitterName = Handle.GetName();
		Summary.bEnabled = Handle.GetIsEnabled();

		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetInstance().GetEmitterData())
		{
			Summary.SimTarget = EmitterData->SimTarget;
			for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
			{
				if (Renderer != nullptr)
				{
					Summary.RendererClasses.AddUnique(Renderer->GetClass());
				}
			}
		}
	}
}

void FDreamFXExternalEditUtilities::GetScriptStackTopology(const FDFXExt_StackItemReference& ScriptRef,
	FDFXExt_ScriptStackTopology& OutTopology, FDreamFXExternalEditContext& Context)
{
	UNiagaraStackScriptItemGroup* Group = ResolveScript(ScriptRef, Context);
	if (Group == nullptr)
	{
		return;
	}

	OutTopology.ScriptName = NameForScriptUsage(Group->GetScriptUsage());

	TArray<UNiagaraStackModuleItem*> Modules;
	CollectDescendants<UNiagaraStackModuleItem>(Group, Modules, /*bStopAtT=*/true);
	for (UNiagaraStackModuleItem* Module : Modules)
	{
		FillModuleTopology(*Module, OutTopology.Modules.AddDefaulted_GetRef());
	}
}

void FDreamFXExternalEditUtilities::GetModuleTopology(const FDFXExt_StackItemReference& ModuleRef,
	FDFXExt_ModuleTopology& OutTopology, FDreamFXExternalEditContext& Context)
{
	if (UNiagaraStackModuleItem* Module = ResolveModule(ModuleRef, Context))
	{
		FillModuleTopology(*Module, OutTopology);
	}
}

void FDreamFXExternalEditUtilities::GetEmitterTopology(const FDFXExt_StackItemReference& EmitterRef,
	FDFXExt_EmitterTopology& OutTopology, FDreamFXExternalEditContext& Context)
{
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(EmitterRef.System, EmitterRef.EmitterName);
	if (Handle == nullptr)
	{
		Context.Error(FText::Format(
			LOCTEXT("TopologyNoEmitter", "Emitter '{0}' not found."), FText::FromName(EmitterRef.EmitterName)));
		return;
	}

	OutTopology.EmitterName = Handle->GetName();
	OutTopology.bEnabled = Handle->GetIsEnabled();

	if (FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData())
	{
		OutTopology.SimTarget = EmitterData->SimTarget;

		const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
		for (int32 Index = 0; Index < Renderers.Num(); ++Index)
		{
			if (Renderers[Index] == nullptr)
			{
				continue;
			}
			OutTopology.RendererClasses.AddUnique(Renderers[Index]->GetClass());

			FDFXExt_RendererRef& Ref = OutTopology.Renderers.AddDefaulted_GetRef();
			Ref.RendererIndex = Index;
			Ref.RendererClass = Renderers[Index]->GetClass();
		}
	}

	struct FStackSlot
	{
		ENiagaraScriptUsage Usage;
		FDFXExt_ScriptStackTopology* Out;
	};
	const FStackSlot Slots[] =
	{
		{ ENiagaraScriptUsage::EmitterSpawnScript,  &OutTopology.EmitterSpawnScript  },
		{ ENiagaraScriptUsage::EmitterUpdateScript, &OutTopology.EmitterUpdateScript },
		{ ENiagaraScriptUsage::ParticleSpawnScript, &OutTopology.ParticleSpawnScript },
		{ ENiagaraScriptUsage::ParticleUpdateScript,&OutTopology.ParticleUpdateScript},
	};

	for (const FStackSlot& Slot : Slots)
	{
		FDFXExt_StackItemReference ScriptRef(EmitterRef.System, EmitterRef.EmitterName, NameForScriptUsage(Slot.Usage));
		GetScriptStackTopology(ScriptRef, *Slot.Out, Context);
	}
}

// ------------------------------------------------------------------------------------------------
// Values

void FDreamFXExternalEditUtilities::GetModuleInputValues(const FDFXExt_StackItemReference& ModuleRef,
	FDFXExt_ModuleInputValues& OutValues, FDreamFXExternalEditContext& Context)
{
	UNiagaraStackModuleItem* Module = ResolveModule(ModuleRef, Context);
	if (Module == nullptr)
	{
		return;
	}

	OutValues.ModuleName = FName(*Module->GetModuleNode().GetFunctionName());

	TArray<UNiagaraStackFunctionInput*> Inputs;
	CollectInputs(Module, Inputs);
	for (UNiagaraStackFunctionInput* Input : Inputs)
	{
		FDFXExt_StackInputValueEntry& Entry = OutValues.Inputs.AddDefaulted_GetRef();
		Entry.Name = Input->GetInputParameterHandle().GetName();
		FillInputValue(*Input, Entry.Value);
	}
}

void FDreamFXExternalEditUtilities::GetStackInputData(const FDFXExt_StackItemReference& StackInputRef,
	FDFXExt_StackInputValue& OutData, FDreamFXExternalEditContext& Context)
{
	if (UNiagaraStackFunctionInput* Input = ResolveInput(StackInputRef, Context))
	{
		FillInputValue(*Input, OutData);
	}
}

void FDreamFXExternalEditUtilities::SetStackInputData(const FDFXExt_StackItemReference& StackItemRef,
	const FDFXExt_StackInputValue& InData, FDreamFXExternalEditContext& Context)
{
	UNiagaraStackFunctionInput* Input = ResolveInput(StackItemRef, Context);
	if (Input == nullptr)
	{
		return;
	}

	if (const FDFXExt_StackInputData_Linked* Linked = InData.GetPtr<FDFXExt_StackInputData_Linked>())
	{
		Input->SetLinkedParameterValue(FNiagaraVariableBase(Linked->LinkedVariable.Type, Linked->LinkedVariable.Name));
		return;
	}
	if (const FDFXExt_StackInputData_HlslExpression* Expression = InData.GetPtr<FDFXExt_StackInputData_HlslExpression>())
	{
		Input->SetCustomExpression(Expression->HlslExpression);
		return;
	}
	if (const FDFXExt_StackInputData_DynamicInput* Dynamic = InData.GetPtr<FDFXExt_StackInputData_DynamicInput>())
	{
		Input->SetDynamicInput(Dynamic->DynamicInputAsset);
		return;
	}
	if (const FDFXExt_StackInputData_DataInterface* DataInterface = InData.GetPtr<FDFXExt_StackInputData_DataInterface>())
	{
		// The class comes from the input's own type: a data interface input can only ever hold the
		// interface it declares, and the payload carries the configuration rather than the class.
		//
		// The setter itself is the one place 5.6 and 5.7 genuinely disagree inside this layer: 5.6
		// takes an instance to copy from, 5.7 takes the class and news one up. The class default
		// object is the right instance for the 5.6 form -- it is what the 5.7 form constructs.
		if (UClass* InterfaceClass = Input->GetInputType().GetClass())
		{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
			if (const UNiagaraDataInterface* Template =
				Cast<UNiagaraDataInterface>(InterfaceClass->GetDefaultObject()))
			{
				Input->SetDataInterfaceValue(*Template);
			}
#else
			Input->SetDataInterfaceValue(InterfaceClass);
#endif
		}
		if (UNiagaraDataInterface* Instance = Input->GetDataValueObject())
		{
			if (!DataInterface->PropertyValues.IsEmpty())
			{
				GetDefaultProvider().SetObjectProperties(Instance, DataInterface->PropertyValues);
			}
		}
		return;
	}
	if (const FDFXExt_StackInputData_Enum* EnumData = InData.GetPtr<FDFXExt_StackInputData_Enum>())
	{
		if (EnumData->Enum == nullptr)
		{
			Context.Error(LOCTEXT("EnumNoType", "An enum input value carried no enum type."));
			return;
		}

		const int64 Value = EnumData->Enum->GetValueByName(EnumData->EnumName);
		if (Value == INDEX_NONE)
		{
			Context.Error(FText::Format(
				LOCTEXT("EnumNoEntry", "'{0}' is not an entry of enum '{1}'."),
				FText::FromName(EnumData->EnumName), FText::FromString(EnumData->Enum->GetName())));
			return;
		}

		// Niagara stores an enum input as its int32, so the write is a local value like any other.
		FNiagaraInt32 AsInt;
		AsInt.Value = static_cast<int32>(Value);
		TSharedRef<FStructOnScope> Local = MakeShared<FStructOnScope>(FNiagaraTypeDefinition::GetIntStruct());
		FMemory::Memcpy(Local->GetStructMemory(), &AsInt, sizeof(FNiagaraInt32));
		Input->SetLocalValue(Local);
		return;
	}

	if (InData.IsValid())
	{
		const UScriptStruct* ValueStruct = InData.GetScriptStruct();
		TSharedRef<FStructOnScope> Local = MakeShared<FStructOnScope>(ValueStruct);
		ValueStruct->CopyScriptStruct(Local->GetStructMemory(), InData.GetMemory());
		Input->SetLocalValue(Local);
		return;
	}

	Context.Error(LOCTEXT("SetInputUnset", "Attempted to write an unset input value."));
}

void FDreamFXExternalEditUtilities::GetDynamicInputChain(const FDFXExt_StackItemReference& StackInputRef,
	FDFXExt_DynamicInputChainRef& OutChain, FDreamFXExternalEditContext& Context)
{
	UNiagaraStackFunctionInput* Input = ResolveInput(StackInputRef, Context);
	if (Input == nullptr)
	{
		return;
	}

	if (Input->GetValueMode() != UNiagaraStackFunctionInput::EValueMode::Dynamic)
	{
		Context.Error(FText::Format(
			LOCTEXT("ChainNotDynamic", "Input '{0}' is not a dynamic input."),
			FText::FromName(Input->GetInputParameterHandle().GetName())));
		return;
	}

	// One level, iteratively deepened by the caller: the adapter walks a chain by pushing the next
	// name onto InputNameStack and asking again, so a recursive fill here would duplicate work the
	// caller already paces itself.
	auto FillChainEntry = [](UNiagaraStackFunctionInput& From, FDFXExt_DynamicInputChain& To)
	{
		To.Name = From.GetInputParameterHandle().GetName();
		To.Type = From.GetInputType();
		// Same composition as FillInputTopology; see the note there.
		const bool bHidden = From.GetIsHidden();
		const bool bVisibleConditionPasses = From.GetHasVisibleCondition() ? From.GetVisibleConditionEnabled() : true;
		const bool bEditConditionPasses = From.GetHasEditCondition() ? From.GetEditConditionEnabled() : true;
		To.bIsVisible = !bHidden && bVisibleConditionPasses;
		To.bIsEditable = To.bIsVisible && bEditConditionPasses;
		To.bIsStaticSwitch = From.IsStaticParameter();
		FillInputValue(From, To.Value);
	};

	FDFXExt_DynamicInputChain& Chain = OutChain.GetMutable();
	FillChainEntry(*Input, Chain);

	TArray<UNiagaraStackFunctionInput*> Children;
	CollectInputs(Input, Children);
	for (UNiagaraStackFunctionInput* Child : Children)
	{
		FDFXExt_DynamicInputChainRef& ChildRef = Chain.Inputs.AddDefaulted_GetRef();
		FillChainEntry(*Child, ChildRef.GetMutable());
	}
}

// ------------------------------------------------------------------------------------------------
// User variables

void FDreamFXExternalEditUtilities::GetUserVariables(UNiagaraSystem* System,
	FDFXExt_UserVariables& OutVariables, FDreamFXExternalEditContext& Context)
{
	if (System == nullptr)
	{
		Context.Error(LOCTEXT("UserVarsNoSystem", "No system to read user parameters from."));
		return;
	}

	const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

	TArray<FNiagaraVariable> Parameters;
	Store.GetParameters(Parameters);

	for (const FNiagaraVariable& Parameter : Parameters)
	{
		FDFXExt_UserVariable& Out = OutVariables.UserVariables.AddDefaulted_GetRef();
		Out.Name = Parameter.GetName();
		Out.Type = Parameter.GetType();

		if (Parameter.GetType().IsDataInterface())
		{
			FDFXExt_VariableValue_DataInterface& Value =
				Out.DefaultValue.InitializeAs<FDFXExt_VariableValue_DataInterface>();
			Value.DataInterface = const_cast<FNiagaraUserRedirectionParameterStore&>(Store)
				.GetDataInterface(Parameter);
			Value.DataInterfaceClass = Value.DataInterface != nullptr ? Value.DataInterface->GetClass() : nullptr;
			continue;
		}
		if (Parameter.GetType().IsUObject())
		{
			FDFXExt_VariableValue_Object& Value = Out.DefaultValue.InitializeAs<FDFXExt_VariableValue_Object>();
			Value.Object = const_cast<FNiagaraUserRedirectionParameterStore&>(Store).GetUObject(Parameter);
			Value.ObjectClass = Parameter.GetType().GetClass();
			continue;
		}

		if (const uint8* Data = Store.GetParameterData(Parameter))
		{
			Out.DefaultValue.InitializeAs(Parameter.GetType().GetScriptStruct(), Data);
		}
	}
}

void FDreamFXExternalEditUtilities::AddUserVariable(UNiagaraSystem* System,
	const FDFXExt_UserVariable& Variable, FDreamFXExternalEditContext& Context)
{
	if (System == nullptr)
	{
		Context.Error(LOCTEXT("AddUserVarNoSystem", "No system to add a user parameter to."));
		return;
	}

	FNiagaraVariable Parameter(Variable.Type, Variable.Name);
	if (!Parameter.GetType().IsValid())
	{
		Context.Error(FText::Format(
			LOCTEXT("AddUserVarNoType", "User parameter '{0}' has no valid type."), FText::FromName(Variable.Name)));
		return;
	}

	System->Modify();
	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	Store.AddParameter(Parameter, /*bInitialize=*/true, /*bTriggerRebind=*/true);

	// The value is a second step deliberately: AddParameter with an uninitialised variable is what
	// silently dropped user parameter values on the stock path once already.
	if (Variable.DefaultValue.IsValid())
	{
		FNiagaraVariant Variant;
		Variable.DefaultValue.Get(Variant, Context);

		switch (Variant.GetMode())
		{
		case ENiagaraVariantMode::DataInterface:
			Store.SetDataInterface(Variant.GetDataInterface(), Parameter);
			break;
		case ENiagaraVariantMode::Object:
			Store.SetUObject(Variant.GetUObject(), Parameter);
			break;
		case ENiagaraVariantMode::Bytes:
			if (Variant.GetNumBytes() == Parameter.GetType().GetSize())
			{
				Store.SetParameterData(Variant.GetBytes(), Parameter, /*bAdd=*/false);
			}
			break;
		default:
			break;
		}
	}
}

void FDreamFXExternalEditUtilities::RemoveUserVariable(UNiagaraSystem* System,
	const FDFXExt_Variable& Variable, FDreamFXExternalEditContext& Context)
{
	if (System == nullptr)
	{
		Context.Error(LOCTEXT("RemoveUserVarNoSystem", "No system to remove a user parameter from."));
		return;
	}

	System->Modify();
	System->GetExposedParameters().RemoveParameter(FNiagaraVariable(Variable.Type, Variable.Name));
}

// ------------------------------------------------------------------------------------------------
// Property blobs

void FDreamFXExternalEditUtilities::GetSystemData(UNiagaraSystem* System, FDFXExt_SystemData& OutData,
	FDreamFXExternalEditContext& Context)
{
	if (System == nullptr)
	{
		Context.Error(LOCTEXT("SystemDataNoSystem", "No system to read properties from."));
		return;
	}
	OutData.PropertyValues = ObjectPropertiesToJson(System);
}

void FDreamFXExternalEditUtilities::SetSystemData(UNiagaraSystem* System, const FDFXExt_SystemData& InData,
	FDreamFXExternalEditContext& Context)
{
	if (System == nullptr)
	{
		Context.Error(LOCTEXT("SetSystemDataNoSystem", "No system to write properties to."));
		return;
	}
	if (!GetDefaultProvider().SetObjectProperties(System, InData.PropertyValues))
	{
		Context.Error(LOCTEXT("SetSystemDataFailed", "Could not apply the system property values."));
	}
}

void FDreamFXExternalEditUtilities::GetEmitterData(const FDFXExt_StackItemReference& EmitterRef,
	FDFXExt_EmitterData& OutData, FDreamFXExternalEditContext& Context)
{
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(EmitterRef.System, EmitterRef.EmitterName);
	if (Handle == nullptr)
	{
		Context.Error(FText::Format(
			LOCTEXT("EmitterDataNoEmitter", "Emitter '{0}' not found."), FText::FromName(EmitterRef.EmitterName)));
		return;
	}
	OutData.PropertyValues = EmitterDataToJson(Handle->GetEmitterData());
}

void FDreamFXExternalEditUtilities::SetEmitterData(const FDFXExt_StackItemReference& EmitterRef,
	const FDFXExt_EmitterData& InData, FDreamFXExternalEditContext& Context)
{
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(EmitterRef.System, EmitterRef.EmitterName);
	if (Handle == nullptr)
	{
		Context.Error(FText::Format(
			LOCTEXT("SetEmitterDataNoEmitter", "Emitter '{0}' not found."), FText::FromName(EmitterRef.EmitterName)));
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
	UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
	if (EmitterData == nullptr || Emitter == nullptr)
	{
		Context.Error(LOCTEXT("SetEmitterDataNoData", "Emitter has no data to write to."));
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InData.PropertyValues);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		Context.Error(LOCTEXT("SetEmitterDataBadJson", "Emitter property values were not valid JSON."));
		return;
	}

	Emitter->Modify();
	if (!FJsonObjectConverter::JsonObjectToUStruct(
		JsonObject.ToSharedRef(), FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, 0, 0))
	{
		Context.Error(LOCTEXT("SetEmitterDataFailed", "Could not apply the emitter property values."));
		return;
	}
	Emitter->PostEditChange();
}

void FDreamFXExternalEditUtilities::GetRendererData(const FDFXExt_StackItemReference& RendererRef,
	FDFXExt_RendererData& OutData, FDreamFXExternalEditContext& Context)
{
	if (UNiagaraRendererProperties* Renderer = ResolveRenderer(RendererRef, Context))
	{
		OutData.PropertyValues = ObjectPropertiesToJson(Renderer);
	}
}

void FDreamFXExternalEditUtilities::SetRendererData(const FDFXExt_StackItemReference& RendererRef,
	const FDFXExt_RendererData& InData, FDreamFXExternalEditContext& Context)
{
	UNiagaraRendererProperties* Renderer = ResolveRenderer(RendererRef, Context);
	if (Renderer == nullptr)
	{
		return;
	}
	if (!GetDefaultProvider().SetObjectProperties(Renderer, InData.PropertyValues))
	{
		Context.Error(LOCTEXT("SetRendererDataFailed", "Could not apply the renderer property values."));
	}
}

// ------------------------------------------------------------------------------------------------
// Structural edits

void FDreamFXExternalEditUtilities::AddEmitter(UNiagaraEmitter* TemplateEmitter, FName EmitterName,
	FDFXExt_EmitterTopology& OutTopology, FDreamFXExternalEditContext& Context)
{
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Context.GetSystemViewModel();
	if (!ViewModel.IsValid())
	{
		Context.Error(LOCTEXT("AddEmitterNoSystem", "No system to add an emitter to."));
		return;
	}

	UNiagaraEmitter* Template = TemplateEmitter;
	if (Template == nullptr)
	{
		// The adapter's own AddEmitter news up an empty emitter and initialises it through the factory
		// before calling here, so a null template is the "make me a bare one" case rather than an error.
		Template = NewObject<UNiagaraEmitter>(GetTransientPackage(), NAME_None, RF_Transactional);
		UNiagaraEmitterFactoryNew::InitializeEmitter(Template, /*bAddDefaultModulesAndRenderers=*/false);
	}

	TSharedPtr<FNiagaraEmitterHandleViewModel> HandleViewModel =
		ViewModel->AddEmitter(*Template, Template->GetExposedVersion().VersionGuid);
	if (!HandleViewModel.IsValid())
	{
		Context.Error(LOCTEXT("AddEmitterFailed", "The system refused the new emitter."));
		return;
	}

	if (!EmitterName.IsNone())
	{
		HandleViewModel->SetName(EmitterName);
	}

	FDFXExt_StackItemReference EmitterRef(&ViewModel->GetSystem(), HandleViewModel->GetName());
	GetEmitterTopology(EmitterRef, OutTopology, Context);
}

void FDreamFXExternalEditUtilities::RemoveEmitter(const FDFXExt_StackItemReference& EmitterRef,
	FDreamFXExternalEditContext& Context)
{
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Context.GetSystemViewModel();
	if (!ViewModel.IsValid())
	{
		Context.Error(LOCTEXT("RemoveEmitterNoSystem", "No system to remove an emitter from."));
		return;
	}

	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& HandleViewModel : ViewModel->GetEmitterHandleViewModels())
	{
		if (HandleViewModel->GetName() == EmitterRef.EmitterName)
		{
			ViewModel->DeleteEmitters({ HandleViewModel->GetId() });
			return;
		}
	}

	Context.Error(FText::Format(
		LOCTEXT("RemoveEmitterNotFound", "Emitter '{0}' not found."), FText::FromName(EmitterRef.EmitterName)));
}

void FDreamFXExternalEditUtilities::AddModule(const FDFXExt_StackItemReference& NewModuleLocationRef,
	const UNiagaraScript* ModuleAsset, FDFXExt_ModuleTopology& OutTopology, FDreamFXExternalEditContext& Context)
{
	UNiagaraStackScriptItemGroup* Group = ResolveScript(NewModuleLocationRef, Context);
	if (Group == nullptr)
	{
		return;
	}
	if (ModuleAsset == nullptr)
	{
		Context.Error(LOCTEXT("AddModuleNoAsset", "No module script to add."));
		return;
	}

	UNiagaraNodeOutput* OutputNode = Group->GetScriptOutputNode();
	if (OutputNode == nullptr)
	{
		Context.Error(LOCTEXT("AddModuleNoOutput", "The target script stack has no output node."));
		return;
	}

	// The same two steps the engine's own AddModule takes: put the node in the graph, then rebuild the
	// group's children so the new module is addressable. Skipping the second leaves a module that
	// exists in the graph and cannot be reached through the stack -- which is every following call on
	// it. The engine reaches the rebuild through ItemAdded, which is exported but private; the public
	// RefreshChildren is what ItemAdded ends in.
	UNiagaraNodeFunctionCall* AddedNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		const_cast<UNiagaraScript*>(ModuleAsset), *OutputNode);
	if (AddedNode == nullptr)
	{
		Context.Error(FText::Format(
			LOCTEXT("AddModuleRefused", "The stack refused module '{0}'."),
			FText::FromString(ModuleAsset->GetName())));
		return;
	}

	Group->RefreshChildren();

	FDFXExt_StackItemReference ModuleRef = NewModuleLocationRef;
	ModuleRef.ModuleName = FName(*AddedNode->GetFunctionName());
	GetModuleTopology(ModuleRef, OutTopology, Context);
}

void FDreamFXExternalEditUtilities::AddSetParametersModule(const FDFXExt_StackItemReference& NewModuleLocationRef,
	const TArray<FDFXExt_SetParameterEntry>& Parameters, FDFXExt_ModuleTopology& OutTopology,
	FDreamFXExternalEditContext& Context)
{
	UNiagaraStackScriptItemGroup* Group = ResolveScript(NewModuleLocationRef, Context);
	if (Group == nullptr)
	{
		return;
	}

	UNiagaraNodeOutput* OutputNode = Group->GetScriptOutputNode();
	if (OutputNode == nullptr)
	{
		Context.Error(LOCTEXT("AddSetParamsNoOutput", "The target script stack has no output node."));
		return;
	}

	TArray<FNiagaraVariable> Variables;
	TArray<FString> DefaultValues;
	Variables.Reserve(Parameters.Num());
	DefaultValues.Reserve(Parameters.Num());

	for (const FDFXExt_SetParameterEntry& Entry : Parameters)
	{
		FNiagaraVariable Variable(Entry.Variable.Type, Entry.Variable.Name);
		if (!Variable.GetType().IsValid())
		{
			Context.Error(FText::Format(
				LOCTEXT("SetParamNoType", "Parameter '{0}' has no valid type."),
				FText::FromName(Entry.Variable.Name)));
			return;
		}

		// AddParameterModuleToStack takes defaults as the pin's own string encoding, so the value has
		// to go through the variable it belongs to rather than being formatted here.
		FString DefaultValue;
		if (Entry.DefaultValue.IsValid() && Entry.DefaultValue.GetScriptStruct() == Variable.GetType().GetScriptStruct())
		{
			Variable.AllocateData();
			Variable.SetData(Entry.DefaultValue.GetMemory());

			const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
			if (Schema != nullptr)
			{
				Schema->TryGetPinDefaultValueFromNiagaraVariable(Variable, DefaultValue);
			}
		}

		Variables.Add(Variable);
		DefaultValues.Add(DefaultValue);
	}

	UNiagaraNodeAssignment* AddedNode = FNiagaraStackGraphUtilities::AddParameterModuleToStack(
		Variables, *OutputNode, INDEX_NONE, DefaultValues);
	if (AddedNode == nullptr)
	{
		Context.Error(LOCTEXT("AddSetParamsRefused", "The stack refused the set-parameters module."));
		return;
	}

	Group->RefreshChildren();

	FDFXExt_StackItemReference ModuleRef = NewModuleLocationRef;
	ModuleRef.ModuleName = FName(*AddedNode->GetFunctionName());
	GetModuleTopology(ModuleRef, OutTopology, Context);
}

void FDreamFXExternalEditUtilities::RemoveModule(const FDFXExt_StackItemReference& ModuleRef,
	FDreamFXExternalEditContext& Context)
{
	UNiagaraStackModuleItem* Module = ResolveModule(ModuleRef, Context);
	if (Module == nullptr)
	{
		return;
	}

	FText Message;
	if (!Module->TestCanDeleteWithMessage(Message))
	{
		Context.Error(Message);
		return;
	}
	Module->Delete();
}

void FDreamFXExternalEditUtilities::SetModuleEnabled(const FDFXExt_StackItemReference& ModuleRef,
	bool bEnabled, FDreamFXExternalEditContext& Context)
{
	if (UNiagaraStackModuleItem* Module = ResolveModule(ModuleRef, Context))
	{
		FNiagaraStackGraphUtilities::SetModuleIsEnabled(Module->GetModuleNode(), bEnabled);
	}
}

void FDreamFXExternalEditUtilities::AddRenderer(const FDFXExt_StackItemReference& NewRendererLocation,
	const TSubclassOf<UNiagaraRendererProperties> RendererClass, FDFXExt_RendererRef& OutRef,
	FDreamFXExternalEditContext& Context)
{
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(NewRendererLocation.System, NewRendererLocation.EmitterName);
	if (Handle == nullptr)
	{
		Context.Error(FText::Format(
			LOCTEXT("AddRendererNoEmitter", "Emitter '{0}' not found."),
			FText::FromName(NewRendererLocation.EmitterName)));
		return;
	}
	if (RendererClass == nullptr)
	{
		Context.Error(LOCTEXT("AddRendererNoClass", "No renderer class to add."));
		return;
	}

	UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
	if (Emitter == nullptr)
	{
		Context.Error(LOCTEXT("AddRendererNoInstance", "Emitter has no instance to add a renderer to."));
		return;
	}

	Emitter->Modify();
	UNiagaraRendererProperties* Renderer =
		NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, NAME_None, RF_Transactional);
	Emitter->AddRenderer(Renderer, Handle->GetInstance().Version);

	FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
	OutRef.RendererIndex = EmitterData != nullptr ? EmitterData->GetRenderers().Find(Renderer) : INDEX_NONE;
	OutRef.RendererClass = RendererClass;
}

void FDreamFXExternalEditUtilities::RemoveRenderer(const FDFXExt_StackItemReference& RendererRef,
	FDreamFXExternalEditContext& Context)
{
	UNiagaraRendererProperties* Renderer = ResolveRenderer(RendererRef, Context);
	if (Renderer == nullptr)
	{
		return;
	}

	FNiagaraEmitterHandle* Handle = FindEmitterHandle(RendererRef.System, RendererRef.EmitterName);
	UNiagaraEmitter* Emitter = Handle != nullptr ? Handle->GetInstance().Emitter : nullptr;
	if (Emitter == nullptr)
	{
		Context.Error(LOCTEXT("RemoveRendererNoInstance", "Emitter has no instance to remove a renderer from."));
		return;
	}

	Emitter->Modify();
	Emitter->RemoveRenderer(Renderer, Handle->GetInstance().Version);
}

// ------------------------------------------------------------------------------------------------
// Diagnostics

void FDreamFXExternalEditUtilities::GetSystemCompileState(UNiagaraSystem* System,
	FDFXExt_SystemCompileState& OutState, FDreamFXExternalEditContext& Context)
{
	if (System == nullptr)
	{
		Context.Error(LOCTEXT("CompileStateNoSystem", "No system to read compile state from."));
		return;
	}

	OutState.bIsCompiling = System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/true);

	auto AddScript = [&OutState](UNiagaraScript* Script, FName EmitterName)
	{
		if (Script == nullptr)
		{
			return;
		}

		FDFXExt_ScriptCompileInfo& Info = OutState.Scripts.AddDefaulted_GetRef();
		Info.EmitterName = EmitterName;
		Info.ScriptName = NameForScriptUsage(Script->GetUsage());
		Info.LastCompileStatus = ToCompileStatus(Script->GetLastCompileStatus());
		Info.ErrorSummary = Script->GetVMExecutableData().ErrorMsg;

		for (const FNiagaraCompileEvent& Event : Script->GetVMExecutableData().LastCompileEvents)
		{
			FDFXExt_CompileEvent& OutEvent = Info.CompileEvents.AddDefaulted_GetRef();
			OutEvent.Severity = ToEventSeverity(Event.Severity);
			OutEvent.Message = Event.Message;
			OutEvent.ShortDescription = Event.ShortDescription;
			OutEvent.NodeGuid = Event.NodeGuid;
			OutEvent.PinGuid = Event.PinGuid;

			if (OutEvent.Severity == EDFXExt_CompileEventSeverity::Error)
			{
				OutState.bHasErrors = true;
			}
			else if (OutEvent.Severity == EDFXExt_CompileEventSeverity::Warning)
			{
				OutState.bHasWarnings = true;
			}
		}

		if (Info.LastCompileStatus == EDFXExt_ScriptCompileStatus::Error)
		{
			OutState.bHasErrors = true;
		}
		if (Info.LastCompileStatus == EDFXExt_ScriptCompileStatus::Dirty)
		{
			OutState.bIsStale = true;
		}
	};

	AddScript(System->GetSystemSpawnScript(), NAME_None);
	AddScript(System->GetSystemUpdateScript(), NAME_None);

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetInstance().GetEmitterData();
		if (EmitterData == nullptr)
		{
			continue;
		}

		TArray<UNiagaraScript*> Scripts;
		EmitterData->GetScripts(Scripts, /*bCompilableOnly=*/true);
		for (UNiagaraScript* Script : Scripts)
		{
			AddScript(Script, Handle.GetName());
		}
	}

	OutState.bIsStale |= OutState.bIsCompiling;
	OutState.AggregateStatus = OutState.bHasErrors
		? EDFXExt_ScriptCompileStatus::Error
		: (OutState.bIsStale
			? EDFXExt_ScriptCompileStatus::Dirty
			: (OutState.bHasWarnings
				? EDFXExt_ScriptCompileStatus::UpToDateWithWarnings
				: EDFXExt_ScriptCompileStatus::UpToDate));
}

void FDreamFXExternalEditUtilities::GetStackIssues(UNiagaraSystem* System,
	FDFXExt_StackIssues& OutIssues, FDreamFXExternalEditContext& Context)
{
	// The engine reads issues from a SECOND, non-data-only view model, because a data-only one clears
	// its issue arrays on purpose. Building that second model per call is the single most expensive
	// thing this layer could do, and DreamFX already treats stack issues as optional: the adapter
	// gates every caller on IsStackIssueReadingAvailable and reports "unavailable" rather than "none",
	// so an empty answer here cannot be mistaken for a clean stack.
	Context.Error(LOCTEXT("StackIssuesUnavailable",
		"Stack issue reading is not available on an engine without the external edit API."));
}

#undef LOCTEXT_NAMESPACE

#endif // !DREAMFX_HAS_NIAGARA_EXTERNAL_EDIT
