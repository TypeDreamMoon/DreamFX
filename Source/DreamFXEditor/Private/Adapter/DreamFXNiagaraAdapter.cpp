#include "DreamFXNiagaraAdapter.h"

#include "DreamFXModule.h"

#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraVariant.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Drains an edit context's error list into the adapter's plain-string error channel. */
		bool Drain(FNiagaraExternalEditContext& Context, TArray<FString>& OutErrors)
		{
			const bool bHadErrors = Context.HasErrors();
			for (const FText& Error : Context.Errors)
			{
				OutErrors.Add(Error.ToString());
			}
			return !bHadErrors;
		}

		FNiagaraExt_StackItemReference ToReference(const FStackAddress& Address)
		{
			FNiagaraExt_StackItemReference Reference(Address.System, Address.EmitterName, Address.ScriptName, Address.ModuleName);
			Reference.RendererIndex = Address.RendererIndex;
			Reference.InputNameStack = Address.InputNameStack;
			return Reference;
		}

		/** Fills a stack input value struct from the adapter's neutral representation. */
		bool ToStackInputValue(const FInputValue& Value, FNiagaraExt_StackInputValue& OutValue, TArray<FString>& OutErrors)
		{
			switch (Value.Mode)
			{
			case EInputValueMode::Literal:
				if (Value.LiteralStruct == nullptr || Value.LiteralBytes.Num() == 0)
				{
					OutErrors.Add(TEXT("Internal error: literal input value has no struct or no data."));
					return false;
				}
				OutValue.InitializeAs(Value.LiteralStruct, Value.LiteralBytes.GetData());
				return true;

			case EInputValueMode::Enum:
			{
				FNiagaraExt_StackInputData_Enum& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_Enum>();
				Data.Enum = Value.EnumType;
				Data.EnumName = Value.EnumEntryName;
				return true;
			}

			case EInputValueMode::Linked:
			{
				FNiagaraExt_StackInputData_Linked& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_Linked>();
				Data.LinkedVariable.Name = Value.LinkedVariable.GetName();
				Data.LinkedVariable.Type = Value.LinkedVariable.GetType();
				return true;
			}

			case EInputValueMode::Hlsl:
			{
				FNiagaraExt_StackInputData_HlslExpression& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_HlslExpression>();
				Data.HlslExpression = Value.HlslExpression;
				return true;
			}

			case EInputValueMode::DynamicInput:
			{
				FNiagaraExt_StackInputData_DynamicInput& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_DynamicInput>();
				Data.DynamicInputAsset = Value.DynamicInputAsset;
				return true;
			}

			case EInputValueMode::DataInterface:
			{
				FNiagaraExt_StackInputData_DataInterface& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_DataInterface>();
				Data.PropertyValues = Value.DataInterfaceJson;
				return true;
			}

			default:
				OutErrors.Add(TEXT("Internal error: attempted to write an unset input value."));
				return false;
			}
		}

		/** Inverse of ToStackInputValue, for the decompiler. */
		void FromStackInputValue(const FNiagaraExt_StackInputValue& In, FInputValue& OutValue)
		{
			if (const FNiagaraExt_StackInputData_Enum* Data = In.GetPtr<FNiagaraExt_StackInputData_Enum>())
			{
				OutValue = FInputValue::MakeEnum(Data->Enum, Data->EnumName);
				return;
			}
			if (const FNiagaraExt_StackInputData_Linked* Data = In.GetPtr<FNiagaraExt_StackInputData_Linked>())
			{
				OutValue = FInputValue::MakeLinked(FNiagaraVariableBase(Data->LinkedVariable.Type, Data->LinkedVariable.Name));
				return;
			}
			if (const FNiagaraExt_StackInputData_HlslExpression* Data = In.GetPtr<FNiagaraExt_StackInputData_HlslExpression>())
			{
				OutValue = FInputValue::MakeHlsl(Data->HlslExpression);
				return;
			}
			if (const FNiagaraExt_StackInputData_DynamicInput* Data = In.GetPtr<FNiagaraExt_StackInputData_DynamicInput>())
			{
				OutValue = FInputValue::MakeDynamicInput(Data->DynamicInputAsset);
				return;
			}
			if (const FNiagaraExt_StackInputData_DataInterface* Data = In.GetPtr<FNiagaraExt_StackInputData_DataInterface>())
			{
				OutValue = FInputValue::MakeDataInterface(nullptr, Data->PropertyValues);
				return;
			}
			if (In.GetPtr<FNiagaraExt_StackInputData_Unsupported>() != nullptr)
			{
				OutValue = FInputValue();
				return;
			}
			if (In.IsValid())
			{
				OutValue = FInputValue::MakeLiteral(In.GetScriptStruct(), In.GetMemory());
				return;
			}
			OutValue = FInputValue();
		}

		/** Builds the FNiagaraVariant that FNiagaraExt_VariableValue::Set expects for a user variable. */
		bool ToVariableValue(const FInputValue& Value, const FNiagaraTypeDefinition& Type,
			FNiagaraExt_VariableValue& OutValue, TArray<FString>& OutErrors)
		{
			switch (Value.Mode)
			{
			case EInputValueMode::Literal:
			{
				if (Value.LiteralBytes.Num() == 0)
				{
					OutErrors.Add(TEXT("Internal error: literal default has no data."));
					return false;
				}
				const FNiagaraVariant Variant(Value.LiteralBytes.GetData(), Value.LiteralBytes.Num());
				OutValue.Set(Type, Variant);
				return true;
			}

			case EInputValueMode::Enum:
			{
				const int32 EnumValue = Value.EnumType ? static_cast<int32>(Value.EnumType->GetValueByName(Value.EnumEntryName)) : 0;
				const FNiagaraVariant Variant(&EnumValue, sizeof(int32));
				OutValue.Set(Type, Variant);
				return true;
			}

			case EInputValueMode::DataInterface:
			{
				// Declaration only: the DI instance is created by the engine and fed at runtime (3.5).
				FNiagaraVariant Variant;
				Variant.SetDataInterface(nullptr);
				OutValue.Set(Type, Variant);
				return true;
			}

			default:
				OutErrors.Add(TEXT("Internal error: user variable defaults must be literals, enums or data interfaces."));
				return false;
			}
		}
	}

	// -------------------------------------------------------------------------------------------
	// FStackAddress
	// -------------------------------------------------------------------------------------------

	FStackAddress FStackAddress::WithEmitter(FName InEmitterName) const
	{
		FStackAddress Copy = *this;
		Copy.EmitterName = InEmitterName;
		return Copy;
	}

	FStackAddress FStackAddress::WithScript(FName InScriptName) const
	{
		FStackAddress Copy = *this;
		Copy.ScriptName = InScriptName;
		return Copy;
	}

	FStackAddress FStackAddress::WithModule(FName InModuleName) const
	{
		FStackAddress Copy = *this;
		Copy.ModuleName = InModuleName;
		return Copy;
	}

	FStackAddress FStackAddress::WithRenderer(int32 InRendererIndex) const
	{
		FStackAddress Copy = *this;
		Copy.RendererIndex = InRendererIndex;
		return Copy;
	}

	FStackAddress FStackAddress::WithInput(FName InInputName) const
	{
		FStackAddress Copy = *this;
		Copy.InputNameStack.Add(InInputName);
		return Copy;
	}

	FStackAddress FStackAddress::WithInputPath(const TArray<FName>& InPath) const
	{
		FStackAddress Copy = *this;
		Copy.InputNameStack = InPath;
		return Copy;
	}

	// -------------------------------------------------------------------------------------------
	// FInputValue
	// -------------------------------------------------------------------------------------------

	FInputValue FInputValue::MakeLiteral(const UScriptStruct* Struct, const void* SourceMemory)
	{
		FInputValue Value;
		if (Struct == nullptr || SourceMemory == nullptr)
		{
			return Value;
		}
		Value.Mode = EInputValueMode::Literal;
		Value.LiteralStruct = Struct;
		Value.LiteralBytes.SetNumUninitialized(Struct->GetStructureSize());
		FMemory::Memcpy(Value.LiteralBytes.GetData(), SourceMemory, Struct->GetStructureSize());
		return Value;
	}

	FInputValue FInputValue::MakeEnum(UEnum* Enum, FName EntryName)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::Enum;
		Value.EnumType = Enum;
		Value.EnumEntryName = EntryName;
		return Value;
	}

	FInputValue FInputValue::MakeLinked(const FNiagaraVariableBase& Variable)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::Linked;
		Value.LinkedVariable = Variable;
		return Value;
	}

	FInputValue FInputValue::MakeHlsl(const FString& Expression)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::Hlsl;
		Value.HlslExpression = Expression;
		return Value;
	}

	FInputValue FInputValue::MakeDynamicInput(UNiagaraScript* Asset)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::DynamicInput;
		Value.DynamicInputAsset = Asset;
		return Value;
	}

	FInputValue FInputValue::MakeDataInterface(UClass* Class, const FString& Json)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::DataInterface;
		Value.DataInterfaceClass = Class;
		Value.DataInterfaceJson = Json;
		return Value;
	}

	FString NormalizeInputIdentifier(const FString& Name)
	{
		FString Result = Name;
		Result.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
		Result.ReplaceInline(TEXT("_"), TEXT(""), ESearchCase::CaseSensitive);
		Result.ToLowerInline();
		// Inline edit conditions come back namespace-qualified ("Module.WriteLifetime") while ordinary
		// inputs do not ("Lifetime"). Inside a module call the namespace is implied, so it is dropped
		// on both sides rather than forcing authors to know which inputs happen to be checkboxes.
		Result.RemoveFromStart(TEXT("module."), ESearchCase::CaseSensitive);
		return Result;
	}

	FString ToInputIdentifier(FName InputName)
	{
		FString Result = InputName.ToString();
		Result.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
		Result.RemoveFromStart(TEXT("Module."), ESearchCase::IgnoreCase);
		return Result;
	}

	const FInputSchema* FModuleSchema::FindInput(FName Name) const
	{
		return Inputs.FindByPredicate([Name](const FInputSchema& Input) { return Input.Name == Name; });
	}

	const FInputSchema* FModuleSchema::FindInputByIdentifier(const FString& Identifier) const
	{
		if (const FInputSchema* Exact = Inputs.FindByPredicate(
			[&Identifier](const FInputSchema& Input) { return Input.Name.ToString() == Identifier; }))
		{
			return Exact;
		}

		const FString Normalized = NormalizeInputIdentifier(Identifier);
		return Inputs.FindByPredicate([&Normalized](const FInputSchema& Input)
		{
			return NormalizeInputIdentifier(Input.Name.ToString()) == Normalized;
		});
	}

	const FInputInfo* FModuleInfo::FindInput(FName Name) const
	{
		return Inputs.FindByPredicate([Name](const FInputInfo& Input) { return Input.Name == Name; });
	}

	const FScriptStackInfo* FEmitterInfo::FindStack(FName ScriptName) const
	{
		return Stacks.FindByPredicate([ScriptName](const FScriptStackInfo& Stack) { return Stack.ScriptName == ScriptName; });
	}

	// -------------------------------------------------------------------------------------------
	// System lifecycle
	// -------------------------------------------------------------------------------------------

	UNiagaraSystem* FNiagaraAdapter::AcquireSystem(const FString& PackagePath, const FString& AssetName,
		bool& bOutCreated, TArray<FString>& OutErrors)
	{
		bOutCreated = false;
		const FString PackageName = PackagePath / AssetName;

		if (FPackageName::DoesPackageExist(PackageName))
		{
			// R9. CreateNiagaraSystem does no collision handling at all; a partially loaded package
			// later trips ValidatePackage's appError inside SavePackage and kills the process. Loading
			// fully first is what makes regeneration -- the normal case -- survivable.
			UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			if (Package == nullptr)
			{
				OutErrors.Add(FString::Printf(TEXT("Package '%s' exists on disk but could not be loaded."), *PackageName));
				return nullptr;
			}
			Package->FullyLoad();

			if (UNiagaraSystem* Existing = FindObject<UNiagaraSystem>(Package, *AssetName))
			{
				// Reusing the object, not recreating it, is what keeps the 4.5 identity contract:
				// every level actor, blueprint and sequencer reference survives regeneration.
				return Existing;
			}

			OutErrors.Add(FString::Printf(
				TEXT("Package '%s' exists but does not contain a Niagara System named '%s'. Refusing to overwrite it."),
				*PackageName, *AssetName));
			return nullptr;
		}

		FNiagaraExternalEditContext Context;
		UNiagaraSystem* System = UNiagaraExternalEditUtilities::CreateNiagaraSystem(AssetName, PackagePath, nullptr, Context);
		Drain(Context, OutErrors);

		if (System == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("Failed to create Niagara System '%s' at '%s'."), *AssetName, *PackagePath));
			return nullptr;
		}

		bOutCreated = true;
		return System;
	}

	bool FNiagaraAdapter::SaveSystem(UNiagaraSystem* System, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot save a null system."));
			return false;
		}

		UPackage* Package = System->GetOutermost();
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (!UPackage::SavePackage(Package, System, *FileName, SaveArgs))
		{
			OutErrors.Add(FString::Printf(TEXT("SavePackage failed for '%s'."), *FileName));
			return false;
		}
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// Topology reads
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::GetEmitterNames(UNiagaraSystem* System, TArray<FName>& OutNames, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read emitters from a null system."));
			return false;
		}

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_SystemSummary Summary;
		UNiagaraExternalEditUtilities::GetSystemSummary(System, Summary, Context);

		for (const FNiagaraExt_EmitterSummary& Emitter : Summary.Emitters)
		{
			OutNames.Add(Emitter.EmitterName);
		}
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetEmitterInfo(const FStackAddress& EmitterAddress, FEmitterInfo& OutInfo, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(EmitterAddress.System);
		FNiagaraExt_EmitterTopology Topology;
		UNiagaraExternalEditUtilities::GetEmitterTopology(ToReference(EmitterAddress), Topology, Context);

		OutInfo.Name = Topology.EmitterName;
		OutInfo.bEnabled = Topology.bEnabled;

		const FNiagaraExt_ScriptStackTopology* Stacks[] =
		{
			&Topology.EmitterSpawnScript, &Topology.EmitterUpdateScript,
			&Topology.ParticleSpawnScript, &Topology.ParticleUpdateScript,
		};

		for (const FNiagaraExt_ScriptStackTopology* Stack : Stacks)
		{
			FScriptStackInfo StackInfo;
			StackInfo.ScriptName = Stack->ScriptName;
			for (const FNiagaraExt_ModuleTopology& Module : Stack->Modules)
			{
				FModuleInfo ModuleInfo;
				ModuleInfo.ModuleName = Module.ModuleName;
				ModuleInfo.bEnabled = Module.Enabled;
				ModuleInfo.Script = Module.ModuleScript;
				ModuleInfo.bIsSetParameters = Module.bIsSetParametersModule;
				for (const FNiagaraExt_StackInputTopology& Input : Module.Inputs)
				{
					FInputInfo InputInfo;
					InputInfo.Name = Input.Name;
					InputInfo.Type = Input.Type;
					InputInfo.bVisible = Input.bIsVisible;
					InputInfo.bEditable = Input.bIsEditable;
					InputInfo.bDynamic = Input.bIsDynamic;
					InputInfo.bStaticSwitch = Input.bIsStaticSwitch;
					ModuleInfo.Inputs.Add(MoveTemp(InputInfo));
				}
				StackInfo.Modules.Add(MoveTemp(ModuleInfo));
			}
			OutInfo.Stacks.Add(MoveTemp(StackInfo));
		}

		for (const FNiagaraExt_RendererRef& Renderer : Topology.Renderers)
		{
			FRendererInfo RendererInfo;
			RendererInfo.Index = Renderer.RendererIndex;
			RendererInfo.Class = Renderer.RendererClass.Get();
			OutInfo.Renderers.Add(RendererInfo);
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetScriptStackInfo(const FStackAddress& ScriptAddress, FScriptStackInfo& OutInfo, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(ScriptAddress.System);
		FNiagaraExt_ScriptStackTopology Topology;
		UNiagaraExternalEditUtilities::GetScriptStackTopology(ToReference(ScriptAddress), Topology, Context);

		OutInfo.ScriptName = Topology.ScriptName;
		for (const FNiagaraExt_ModuleTopology& Module : Topology.Modules)
		{
			FModuleInfo ModuleInfo;
			ModuleInfo.ModuleName = Module.ModuleName;
			ModuleInfo.bEnabled = Module.Enabled;
			ModuleInfo.Script = Module.ModuleScript;
			ModuleInfo.bIsSetParameters = Module.bIsSetParametersModule;
			for (const FNiagaraExt_StackInputTopology& Input : Module.Inputs)
			{
				FInputInfo InputInfo;
				InputInfo.Name = Input.Name;
				InputInfo.Type = Input.Type;
				InputInfo.bVisible = Input.bIsVisible;
				InputInfo.bEditable = Input.bIsEditable;
				InputInfo.bDynamic = Input.bIsDynamic;
				InputInfo.bStaticSwitch = Input.bIsStaticSwitch;
				ModuleInfo.Inputs.Add(MoveTemp(InputInfo));
			}
			OutInfo.Modules.Add(MoveTemp(ModuleInfo));
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetModuleInfo(const FStackAddress& ModuleAddress, FModuleInfo& OutInfo, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(ModuleAddress.System);
		FNiagaraExt_ModuleTopology Topology;
		UNiagaraExternalEditUtilities::GetModuleTopology(ToReference(ModuleAddress), Topology, Context);

		OutInfo.ModuleName = Topology.ModuleName;
		OutInfo.bEnabled = Topology.Enabled;
		OutInfo.Script = Topology.ModuleScript;
		OutInfo.bIsSetParameters = Topology.bIsSetParametersModule;
		for (const FNiagaraExt_StackInputTopology& Input : Topology.Inputs)
		{
			FInputInfo InputInfo;
			InputInfo.Name = Input.Name;
			InputInfo.Type = Input.Type;
			InputInfo.bVisible = Input.bIsVisible;
			InputInfo.bEditable = Input.bIsEditable;
			InputInfo.bDynamic = Input.bIsDynamic;
			InputInfo.bStaticSwitch = Input.bIsStaticSwitch;
			OutInfo.Inputs.Add(MoveTemp(InputInfo));
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetUserVariables(UNiagaraSystem* System, TArray<FUserVariableInfo>& OutVariables, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read user variables from a null system."));
			return false;
		}

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_UserVariables Variables;
		UNiagaraExternalEditUtilities::GetUserVariables(System, Variables, Context);

		for (const FNiagaraExt_UserVariable& Variable : Variables.UserVariables)
		{
			FUserVariableInfo Info;
			// Names come back namespace-qualified ("User.SparkCount") even though the write took a
			// bare name. Callers must compare on the qualified form.
			Info.Name = Variable.Name;
			Info.Type = Variable.Type;
			Info.Description = Variable.Description.ToString();
			OutVariables.Add(MoveTemp(Info));
		}

		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Structural writes
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::AddUserVariable(UNiagaraSystem* System, FName Name, const FNiagaraTypeDefinition& Type,
		const FString& Description, const FInputValue& DefaultValue, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add a user variable to a null system."));
			return false;
		}

		FNiagaraExternalEditContext Context(System);

		FNiagaraExt_UserVariable Variable;
		Variable.Name = Name;
		Variable.Type = Type;
		Variable.Description = FText::FromString(Description);

		if (DefaultValue.IsSet() && !ToVariableValue(DefaultValue, Type, Variable.DefaultValue, OutErrors))
		{
			return false;
		}

		UNiagaraExternalEditUtilities::AddUserVariable(System, Variable, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::RemoveUserVariable(UNiagaraSystem* System, FName Name, const FNiagaraTypeDefinition& Type,
		TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot remove a user variable from a null system."));
			return false;
		}

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_Variable Variable;
		Variable.Name = Name;
		Variable.Type = Type;
		UNiagaraExternalEditUtilities::RemoveUserVariable(System, Variable, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::AddEmitter(UNiagaraSystem* System, FName EmitterName, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add an emitter to a null system."));
			return false;
		}

		// AddEmitter rejects a null template ("template emitter is null"), so there is no
		// create-from-nothing path. InitializeEmitter is the engine's own answer: it builds the four
		// script stacks. bAddDefaultModulesAndRenderers stays false so the DSL remains the sole
		// description of what the emitter contains.
		UNiagaraEmitter* Template = NewObject<UNiagaraEmitter>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UNiagaraEmitter::StaticClass(),
				TEXT("DreamFXTemplateEmitter")), RF_Transactional);
		UNiagaraEmitterFactoryNew::InitializeEmitter(Template, /*bAddDefaultModulesAndRenderers=*/false);

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_EmitterTopology Topology;
		UNiagaraExternalEditUtilities::AddEmitter(Template, EmitterName, Topology, Context);

		if (!Drain(Context, OutErrors))
		{
			return false;
		}

		if (Topology.EmitterName != EmitterName)
		{
			// Niagara uniquifies clashing names. Silently accepting a renamed emitter would break R4's
			// stable-key rule and orphan every rapid iteration parameter, so this is fatal.
			OutErrors.Add(FString::Printf(
				TEXT("Emitter '%s' was renamed to '%s' by the system (name collision). Emitter names are stable keys and must be unique."),
				*EmitterName.ToString(), *Topology.EmitterName.ToString()));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::RemoveEmitter(const FStackAddress& EmitterAddress, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(EmitterAddress.System);
		UNiagaraExternalEditUtilities::RemoveEmitter(ToReference(EmitterAddress), Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::AddModule(const FStackAddress& StackAddress, UNiagaraScript* ModuleAsset,
		FName& OutModuleName, TArray<FString>& OutErrors)
	{
		if (ModuleAsset == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add a null module asset."));
			return false;
		}

		FNiagaraExternalEditContext Context(StackAddress.System);
		FNiagaraExt_ModuleTopology Topology;
		UNiagaraExternalEditUtilities::AddModule(ToReference(StackAddress), ModuleAsset, Topology, Context);

		OutModuleName = Topology.ModuleName;
		if (!Drain(Context, OutErrors))
		{
			return false;
		}
		if (OutModuleName == NAME_None)
		{
			OutErrors.Add(FString::Printf(TEXT("AddModule returned no module name for '%s'."), *ModuleAsset->GetName()));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::RemoveModule(const FStackAddress& ModuleAddress, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(ModuleAddress.System);
		UNiagaraExternalEditUtilities::RemoveModule(ToReference(ModuleAddress), Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::SetModuleEnabled(const FStackAddress& ModuleAddress, bool bEnabled, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(ModuleAddress.System);
		UNiagaraExternalEditUtilities::SetModuleEnabled(ToReference(ModuleAddress), bEnabled, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::AddSetParametersModule(const FStackAddress& StackAddress,
		const TArray<TTuple<FName, FNiagaraTypeDefinition, FInputValue>>& Entries,
		FName& OutModuleName, TArray<FString>& OutErrors)
	{
		TArray<FNiagaraExt_SetParameterEntry> Parameters;
		Parameters.Reserve(Entries.Num());

		for (const TTuple<FName, FNiagaraTypeDefinition, FInputValue>& Entry : Entries)
		{
			FNiagaraExt_SetParameterEntry Parameter;
			Parameter.Variable.Name = Entry.Get<0>();
			Parameter.Variable.Type = Entry.Get<1>();

			const FInputValue& Value = Entry.Get<2>();
			// Non-literal entry values (dynamic input, HLSL, linked) cannot ride along on the create
			// call -- FNiagaraExt_SetParameterEntry only carries a plain variable value. The module is
			// created with a placeholder and the real value is written afterwards through SetInput,
			// which addresses the entry as an input on the Set Parameters module.
			if (Value.Mode == EInputValueMode::Literal || Value.Mode == EInputValueMode::Enum)
			{
				if (!ToVariableValue(Value, Entry.Get<1>(), Parameter.DefaultValue, OutErrors))
				{
					return false;
				}
			}

			Parameters.Add(MoveTemp(Parameter));
		}

		FNiagaraExternalEditContext Context(StackAddress.System);
		FNiagaraExt_ModuleTopology Topology;
		UNiagaraExternalEditUtilities::AddSetParametersModule(ToReference(StackAddress), Parameters, Topology, Context);

		OutModuleName = Topology.ModuleName;
		if (!Drain(Context, OutErrors))
		{
			return false;
		}
		if (OutModuleName == NAME_None)
		{
			OutErrors.Add(TEXT("AddSetParametersModule returned no module name."));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::AddRenderer(const FStackAddress& EmitterAddress, UClass* RendererClass,
		int32& OutRendererIndex, TArray<FString>& OutErrors)
	{
		if (RendererClass == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add a renderer with no class."));
			return false;
		}

		FNiagaraExternalEditContext Context(EmitterAddress.System);
		FNiagaraExt_RendererRef Ref;
		UNiagaraExternalEditUtilities::AddRenderer(ToReference(EmitterAddress), RendererClass, Ref, Context);

		OutRendererIndex = Ref.RendererIndex;
		if (!Drain(Context, OutErrors))
		{
			return false;
		}
		if (OutRendererIndex == INDEX_NONE)
		{
			OutErrors.Add(FString::Printf(TEXT("AddRenderer returned no index for '%s'."), *RendererClass->GetName()));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::RemoveRenderer(const FStackAddress& RendererAddress, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(RendererAddress.System);
		UNiagaraExternalEditUtilities::RemoveRenderer(ToReference(RendererAddress), Context);
		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Value writes
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::SetInput(const FStackAddress& InputAddress, const FInputValue& Value, TArray<FString>& OutErrors)
	{
		FNiagaraExt_StackInputValue StackValue;
		if (!ToStackInputValue(Value, StackValue, OutErrors))
		{
			return false;
		}

		FNiagaraExternalEditContext Context(InputAddress.System);
		UNiagaraExternalEditUtilities::SetStackInputData(ToReference(InputAddress), StackValue, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::SetRendererProperties(const FStackAddress& RendererAddress, const FString& PropertiesJson,
		TArray<FString>& OutErrors)
	{
		if (PropertiesJson.IsEmpty())
		{
			return true;
		}

		FNiagaraExternalEditContext Context(RendererAddress.System);
		FNiagaraExt_RendererData Data;
		Data.PropertyValues = PropertiesJson;
		UNiagaraExternalEditUtilities::SetRendererData(ToReference(RendererAddress), Data, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::SetEmitterProperties(const FStackAddress& EmitterAddress, const FString& PropertiesJson,
		TArray<FString>& OutErrors)
	{
		if (PropertiesJson.IsEmpty())
		{
			return true;
		}

		FNiagaraExternalEditContext Context(EmitterAddress.System);
		FNiagaraExt_EmitterData Data;
		Data.PropertyValues = PropertiesJson;
		UNiagaraExternalEditUtilities::SetEmitterData(ToReference(EmitterAddress), Data, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::SetSystemProperties(UNiagaraSystem* System, const FString& PropertiesJson, TArray<FString>& OutErrors)
	{
		if (System == nullptr || PropertiesJson.IsEmpty())
		{
			return System != nullptr;
		}

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_SystemData Data;
		Data.PropertyValues = PropertiesJson;
		UNiagaraExternalEditUtilities::SetSystemData(System, Data, Context);
		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Value reads
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::GetInput(const FStackAddress& InputAddress, FInputValue& OutValue, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(InputAddress.System);
		FNiagaraExt_StackInputValue StackValue;
		UNiagaraExternalEditUtilities::GetStackInputData(ToReference(InputAddress), StackValue, Context);
		FromStackInputValue(StackValue, OutValue);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetRendererProperties(const FStackAddress& RendererAddress, FString& OutJson, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(RendererAddress.System);
		FNiagaraExt_RendererData Data;
		UNiagaraExternalEditUtilities::GetRendererData(ToReference(RendererAddress), Data, Context);
		OutJson = Data.PropertyValues;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetEmitterProperties(const FStackAddress& EmitterAddress, FString& OutJson, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(EmitterAddress.System);
		FNiagaraExt_EmitterData Data;
		UNiagaraExternalEditUtilities::GetEmitterData(ToReference(EmitterAddress), Data, Context);
		OutJson = Data.PropertyValues;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetSystemProperties(UNiagaraSystem* System, FString& OutJson, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read properties from a null system."));
			return false;
		}

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_SystemData Data;
		UNiagaraExternalEditUtilities::GetSystemData(System, Data, Context);
		OutJson = Data.PropertyValues;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetModuleInputValues(const FStackAddress& ModuleAddress,
		TArray<TTuple<FName, FInputValue>>& OutValues, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(ModuleAddress.System);
		FNiagaraExt_ModuleInputValues Values;
		UNiagaraExternalEditUtilities::GetModuleInputValues(ToReference(ModuleAddress), Values, Context);

		for (const FNiagaraExt_StackInputValueEntry& Entry : Values.Inputs)
		{
			FInputValue Value;
			FromStackInputValue(Entry.Value, Value);
			OutValues.Emplace(Entry.Name, MoveTemp(Value));
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetDynamicInputChildNames(const FStackAddress& InputAddress,
		TArray<FName>& OutChildNames, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(InputAddress.System);
		FNiagaraExt_DynamicInputChainRef ChainRef;
		UNiagaraExternalEditUtilities::GetDynamicInputChain(ToReference(InputAddress), ChainRef, Context);

		if (Context.HasErrors())
		{
			return Drain(Context, OutErrors);
		}

		const FNiagaraExt_DynamicInputChain& Chain = ChainRef.Get();
		for (const FNiagaraExt_DynamicInputChainRef& Child : Chain.Inputs)
		{
			OutChildNames.Add(Child.Get().Name);
		}
		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Schema
	// -------------------------------------------------------------------------------------------

	namespace
	{
		void CopySchema(const FNiagaraExt_ModuleSchema& In, FModuleSchema& Out)
		{
			for (const FNiagaraExt_StackInputSchema& Input : In.Inputs)
			{
				FInputSchema Schema;
				Schema.Name = Input.Name;
				Schema.Type = Input.Type;
				Schema.Category = Input.Category.ToString();
				Schema.Description = Input.MetaData.Description.ToString();
				Schema.bSupportsExpressions = Input.bSupportsExpressions;
				// FNiagaraExt_StackInputSchema carries no static-switch flag, and the one on
				// FNiagaraVariableMetaData is deprecated (the live flag moved to UNiagaraScriptVariable).
				// Only FNiagaraExt_StackInputTopology::bIsStaticSwitch reports it, which is why R5's
				// two-pass lowering must add the module first and read topology back, not pre-plan from
				// the schema alone.
				Schema.bIsStaticSwitch = false;
				Out.Inputs.Add(MoveTemp(Schema));
			}
		}
	}

	bool FNiagaraAdapter::GetModuleSchema(const UNiagaraScript* ModuleAsset, FModuleSchema& OutSchema, TArray<FString>& OutErrors)
	{
		if (ModuleAsset == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read the schema of a null module asset."));
			return false;
		}

		FNiagaraExternalEditContext Context;
		FNiagaraExt_ModuleSchema Schema;
		UNiagaraExternalEditUtilities::GetModuleSchema(ModuleAsset, Schema, Context);
		CopySchema(Schema, OutSchema);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetInputSchema(const FStackAddress& InputAddress, FInputSchema& OutSchema, TArray<FString>& OutErrors)
	{
		FNiagaraExternalEditContext Context(InputAddress.System);
		FNiagaraExt_StackInputSchema Schema;
		UNiagaraExternalEditUtilities::GetStackInputSchema(ToReference(InputAddress), Schema, Context);

		OutSchema.Name = Schema.Name;
		OutSchema.Type = Schema.Type;
		OutSchema.Category = Schema.Category.ToString();
		OutSchema.Description = Schema.MetaData.Description.ToString();
		OutSchema.bSupportsExpressions = Schema.bSupportsExpressions;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetDynamicInputSchema(const UNiagaraScript* Asset, FModuleSchema& OutSchema, TArray<FString>& OutErrors)
	{
		if (Asset == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read the schema of a null dynamic input asset."));
			return false;
		}

		FNiagaraExternalEditContext Context;
		FNiagaraExt_DynamicInputSchema Schema;
		UNiagaraExternalEditUtilities::GetDynamicInputSchema(Asset, Schema, Context);
		CopySchema(Schema, OutSchema);
		return Drain(Context, OutErrors);
	}

	void FNiagaraAdapter::GetAvailableDynamicInputs(const FNiagaraTypeDefinition& Type, TArray<UNiagaraScript*>& OutScripts)
	{
		FNiagaraExternalEditContext Context;
		UNiagaraExternalEditUtilities::GetAvailableDynamicInputs(Type, OutScripts, Context);
	}

	// -------------------------------------------------------------------------------------------
	// Compile + diagnostics
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::CompileAndWait(UNiagaraSystem* System, FCompileStateInfo& OutState, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot compile a null system."));
			return false;
		}

		System->RequestCompile(/*bForce=*/false);
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/false, /*bShowProgress=*/false);

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_SystemCompileState State;
		UNiagaraExternalEditUtilities::GetSystemCompileState(System, State, Context);

		const UEnum* StatusEnum = StaticEnum<ENiagaraExt_ScriptCompileStatus>();
		OutState.StatusName = StatusEnum
			? StatusEnum->GetNameStringByValue(static_cast<int64>(State.AggregateStatus))
			: FString::FromInt(static_cast<int32>(State.AggregateStatus));
		OutState.bHasErrors = State.bHasErrors;
		OutState.bHasWarnings = State.bHasWarnings;
		OutState.bIsStale = State.bIsStale;

		for (const FNiagaraExt_ScriptCompileInfo& Script : State.Scripts)
		{
			for (const FNiagaraExt_CompileEvent& Event : Script.CompileEvents)
			{
				FCompileEventInfo Info;
				Info.Severity = static_cast<int32>(Event.Severity);
				Info.Message = Event.Message;
				Info.ShortDescription = Event.ShortDescription;
				Info.EmitterName = Script.EmitterName;
				Info.ScriptName = Script.ScriptName;
				Info.bFromDependency = Event.bFromScriptDependency;
				OutState.Events.Add(MoveTemp(Info));
			}
		}

		const bool bStatusOk =
			State.AggregateStatus == ENiagaraExt_ScriptCompileStatus::UpToDate ||
			State.AggregateStatus == ENiagaraExt_ScriptCompileStatus::UpToDateWithWarnings ||
			State.AggregateStatus == ENiagaraExt_ScriptCompileStatus::ComputeUpToDateWithWarnings;

		Drain(Context, OutErrors);
		return bStatusOk && !State.bHasErrors;
	}

	bool FNiagaraAdapter::IsStackIssueReadingAvailable()
	{
		// GetStackIssues does NOT go through the data-only view model the rest of this API uses. It
		// calls FNiagaraExternalEditContext::GetDiagnosticsSystemViewModel, which deliberately builds a
		// *non*-data-only FNiagaraSystemViewModel so the stack-issue arrays are populated -- and that
		// path runs the full detail-customisation machinery, which constructs Slate widgets
		// (FNiagaraUserParameterBindingCustomization::CustomizeHeader -> SComboButton). With no Slate
		// application there is nothing to construct against and the process dies.
		//
		// So this one function is editor-only, unlike every other call in this file. Headless builds
		// fall back to compile events, which carry the errors that actually matter for a CI gate.
		return !IsRunningCommandlet() && FSlateApplication::IsInitialized();
	}

	bool FNiagaraAdapter::GetStackIssues(UNiagaraSystem* System, TArray<FStackIssueInfo>& OutIssues, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read stack issues from a null system."));
			return false;
		}

		if (!IsStackIssueReadingAvailable())
		{
			return false;
		}

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_StackIssues Issues;
		UNiagaraExternalEditUtilities::GetStackIssues(System, Issues, Context);

		for (const FNiagaraExt_StackIssue& Issue : Issues.Issues)
		{
			if (Issue.bIsDismissed)
			{
				continue;
			}
			FStackIssueInfo Info;
			Info.Severity = static_cast<int32>(Issue.Severity);
			Info.ShortDescription = Issue.ShortDescription;
			Info.LongDescription = Issue.LongDescription;
			Info.DisplayPath = Issue.StackDisplayPath;
			Info.EmitterName = Issue.Location.EmitterName;
			Info.ScriptName = Issue.Location.ScriptName;
			Info.ModuleName = Issue.Location.ModuleName;
			OutIssues.Add(MoveTemp(Info));
		}

		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Naming helpers
	// -------------------------------------------------------------------------------------------

	namespace
	{
		bool ScriptUsageForStack(EStackKind Kind, ENiagaraScriptUsage& OutUsage)
		{
			switch (Kind)
			{
			case EStackKind::SystemSpawn:    OutUsage = ENiagaraScriptUsage::SystemSpawnScript;    return true;
			case EStackKind::SystemUpdate:   OutUsage = ENiagaraScriptUsage::SystemUpdateScript;   return true;
			case EStackKind::EmitterSpawn:   OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;   return true;
			case EStackKind::EmitterUpdate:  OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;  return true;
			case EStackKind::ParticleSpawn:  OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;  return true;
			case EStackKind::ParticleUpdate: OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; return true;
			default: return false;
			}
		}
	}

	FName FNiagaraAdapter::ScriptNameForStack(EStackKind Kind)
	{
		ENiagaraScriptUsage Usage;
		if (!ScriptUsageForStack(Kind, Usage))
		{
			return NAME_None;
		}

		// Reflection rather than a literal: the API resolves ScriptName through
		// StaticEnum<ENiagaraScriptUsage>()->GetValueByName, so deriving the spelling from the same
		// table means a rename upstream cannot silently desynchronise the two.
		const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
		return UsageEnum ? UsageEnum->GetNameByValue(static_cast<int64>(Usage)) : NAME_None;
	}

	bool FNiagaraAdapter::StackForScriptName(FName ScriptName, EStackKind& OutKind)
	{
		static const EStackKind AllKinds[] =
		{
			EStackKind::SystemSpawn, EStackKind::SystemUpdate,
			EStackKind::EmitterSpawn, EStackKind::EmitterUpdate,
			EStackKind::ParticleSpawn, EStackKind::ParticleUpdate,
		};

		for (EStackKind Kind : AllKinds)
		{
			if (ScriptNameForStack(Kind) == ScriptName)
			{
				OutKind = Kind;
				return true;
			}
		}
		return false;
	}

	UClass* FNiagaraAdapter::FindRendererClass(const FString& TypeName)
	{
		// Discovered by reflection rather than a fixed table so renderers added by other plugins get
		// DSL support for free -- which is the whole point of L8's schema-driven property block.
		const FString Trimmed = TypeName.TrimStartAndEnd();
		const FString Candidate = FString::Printf(TEXT("Niagara%sProperties"), *Trimmed);

		for (TObjectIterator<UClass> ClassIterator; ClassIterator; ++ClassIterator)
		{
			UClass* Class = *ClassIterator;
			if (!Class->IsChildOf(UNiagaraRendererProperties::StaticClass())
				|| Class == UNiagaraRendererProperties::StaticClass()
				|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			const FString ClassName = Class->GetName();
			if (ClassName.Equals(Candidate, ESearchCase::IgnoreCase)
				|| ClassName.Equals(Trimmed, ESearchCase::IgnoreCase)
				|| ClassName.Equals(FString::Printf(TEXT("%sProperties"), *Trimmed), ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}
		return nullptr;
	}

	FString FNiagaraAdapter::RendererTypeNameForClass(const UClass* Class)
	{
		if (Class == nullptr)
		{
			return FString();
		}

		FString Name = Class->GetName();
		Name.RemoveFromStart(TEXT("Niagara"), ESearchCase::CaseSensitive);
		Name.RemoveFromEnd(TEXT("Properties"), ESearchCase::CaseSensitive);
		return Name;
	}
}
