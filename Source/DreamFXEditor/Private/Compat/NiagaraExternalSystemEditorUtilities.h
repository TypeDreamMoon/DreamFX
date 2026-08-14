#pragma once

// This file deliberately carries the engine header's name.
//
// DreamFXEditor.Build.cs puts this directory on the include path ONLY when the engine has no
// external-edit API of its own, so `#include "NiagaraExternalSystemEditorUtilities.h"` in the adapter
// and the Phase 0 commandlet resolves here on 5.6/5.7 and to Epic's header on 5.8 and MoonEngine, with
// no conditional include and no edit to either caller.
//
// It contains no UCLASS, USTRUCT or UENUM on purpose. UnrealHeaderTool would otherwise emit
// `NiagaraExternalSystemEditorUtilities.generated.h` for it, and on 5.8 that generated file would sit
// in the module's own generated directory under the same name as the engine's -- the wrong one wins by
// include order and the failure is a silently mismatched layout. The reflected types live in the
// sibling DreamFXExternalEditTypes.h under names of their own; everything here is plain C++.

#include "CoreMinimal.h"
#include "DreamFXExternalEditTypes.h"
#include "Templates/Function.h"
#include "Templates/SubclassOf.h"

class FNiagaraSystemViewModel;
class UNiagaraDataInterface;
class UNiagaraEmitter;
class UNiagaraRendererProperties;
class UNiagaraScript;
class UNiagaraSystem;
struct FNiagaraTypeDefinition;

/**
 * Mirror of FNiagaraExternalEditPropertyProvider.
 *
 * The engine ships a JsonUtilities-based default and lets a plugin install a richer one. Nothing in
 * DreamFX installs anything, so this side only ever needs the default -- but the indirection is kept
 * because the adapter reaches for the provider by name to read data interface properties.
 */
struct FNiagaraExternalEditPropertyProvider
{
	TFunction<FString(const UStruct* Struct, bool bUserVisibleOnly)> ListStructProperties;
	TFunction<FString(const UObject* Object, const TArray<FName>& PropertyNames)> GetObjectProperties;
	TFunction<bool(UObject* Object, const FString& PropertiesJson)> SetObjectProperties;
};

/**
 * Mirror of FNiagaraExternalEditContext.
 *
 * Holds the errors a call accumulates and, lazily, the system view model every call resolves through.
 * The view model is built exactly as the engine builds its own -- data-processing-only, no simulate,
 * no auto-compile, no compile-for-edit -- because that configuration is what makes it safe to build
 * one per call in a commandlet with no editor tick to service it.
 *
 * Building it lazily rather than in the constructor matters more here than it does in the engine: the
 * adapter constructs a context for operations that turn out to need no view model at all (the property
 * JSON paths reach the emitter through the handle), and a view model is the single most expensive
 * thing in this layer.
 */
struct FDreamFXExternalEditContext
{
	TArray<FText> Errors;

	FDreamFXExternalEditContext() = default;
	explicit FDreamFXExternalEditContext(UNiagaraSystem* InSystem);
	~FDreamFXExternalEditContext();

	FDreamFXExternalEditContext(const FDreamFXExternalEditContext&) = delete;
	FDreamFXExternalEditContext& operator=(const FDreamFXExternalEditContext&) = delete;

	bool HasErrors() const { return Errors.Num() > 0; }
	void Error(FText InError) { Errors.Emplace(MoveTemp(InError)); }

	UNiagaraSystem* GetSystem() const { return System; }

	/** The view model for this context's system, built on first use. Null if the system is null. */
	TSharedPtr<FNiagaraSystemViewModel> GetSystemViewModel();

	/**
	 * Same view model, and the same one the diagnostics reads use.
	 *
	 * The engine keeps a second non-data-only view model for stack issues, because a data-only one
	 * clears its issue arrays. This layer does not: GetStackIssues here reports that it is unavailable
	 * rather than build a second full view model per call, and the adapter already has a supported
	 * shape for that answer -- IsStackIssueReadingAvailable gates every caller.
	 */
	TSharedPtr<FNiagaraSystemViewModel> GetDiagnosticsSystemViewModel() { return GetSystemViewModel(); }

private:
	UNiagaraSystem* System = nullptr;
	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
};

/**
 * Mirror of UNiagaraExternalEditUtilities, minus everything DreamFX does not call.
 *
 * Not a UCLASS: every caller reaches these as static functions, so the reflected function library the
 * engine needs for Blueprint exposure buys nothing here and would cost a generated header.
 *
 * The MoonEngine additions (ClearScriptStack, RefreshScriptStack, the parameter-default pair,
 * CleanUpStaleEmitterParameters, the batch AddModule overloads, EModuleTopologyDetail) are absent by
 * design. Every one of them already sits behind DREAMFX_HAS_NIAGARA_FAST_EDIT in the adapter with a
 * stock-engine path beside it, and that probe is false on any engine that needs this file.
 */
class FDreamFXExternalEditUtilities
{
public:
	// --- creation ---------------------------------------------------------------------------
	static UNiagaraSystem* CreateNiagaraSystem(const FString& AssetName, const FString& AssetPath,
		UNiagaraSystem* TemplateSystem, FDreamFXExternalEditContext& Context);

	static void GetAvailableDynamicInputs(const FNiagaraTypeDefinition& Type,
		TArray<UNiagaraScript*>& OutDynamicInputScripts, FDreamFXExternalEditContext& Context);

	static const FNiagaraExternalEditPropertyProvider& GetPropertyProvider();

	// --- schema -----------------------------------------------------------------------------
	static void GetModuleSchema(const UNiagaraScript* ModuleAsset, FDFXExt_ModuleSchema& OutSchema,
		FDreamFXExternalEditContext& Context);
	static void GetDynamicInputSchema(const UNiagaraScript* ModuleAsset, FDFXExt_DynamicInputSchema& OutSchema,
		FDreamFXExternalEditContext& Context);
	static void GetStackInputSchema(const FDFXExt_StackItemReference& InputReference,
		FDFXExt_StackInputSchema& OutSchema, FDreamFXExternalEditContext& Context);

	// --- summary and topology ---------------------------------------------------------------
	static void GetSystemSummary(UNiagaraSystem* System, FDFXExt_SystemSummary& OutSummary,
		FDreamFXExternalEditContext& Context);
	static void GetEmitterTopology(const FDFXExt_StackItemReference& EmitterRef,
		FDFXExt_EmitterTopology& OutTopology, FDreamFXExternalEditContext& Context);
	static void GetScriptStackTopology(const FDFXExt_StackItemReference& ScriptRef,
		FDFXExt_ScriptStackTopology& OutTopology, FDreamFXExternalEditContext& Context);
	static void GetModuleTopology(const FDFXExt_StackItemReference& ModuleRef,
		FDFXExt_ModuleTopology& OutTopology, FDreamFXExternalEditContext& Context);

	// --- values -----------------------------------------------------------------------------
	static void GetModuleInputValues(const FDFXExt_StackItemReference& ModuleRef,
		FDFXExt_ModuleInputValues& OutValues, FDreamFXExternalEditContext& Context);
	static void GetDynamicInputChain(const FDFXExt_StackItemReference& StackInputRef,
		FDFXExt_DynamicInputChainRef& OutChain, FDreamFXExternalEditContext& Context);
	static void GetStackInputData(const FDFXExt_StackItemReference& StackInputRef,
		FDFXExt_StackInputValue& OutData, FDreamFXExternalEditContext& Context);
	static void SetStackInputData(const FDFXExt_StackItemReference& StackItemRef,
		const FDFXExt_StackInputValue& InData, FDreamFXExternalEditContext& Context);

	static void GetUserVariables(UNiagaraSystem* System, FDFXExt_UserVariables& OutVariables,
		FDreamFXExternalEditContext& Context);
	static void AddUserVariable(UNiagaraSystem* System, const FDFXExt_UserVariable& Variable,
		FDreamFXExternalEditContext& Context);
	static void RemoveUserVariable(UNiagaraSystem* System, const FDFXExt_Variable& Variable,
		FDreamFXExternalEditContext& Context);

	static void GetSystemData(UNiagaraSystem* System, FDFXExt_SystemData& OutData,
		FDreamFXExternalEditContext& Context);
	static void SetSystemData(UNiagaraSystem* System, const FDFXExt_SystemData& InData,
		FDreamFXExternalEditContext& Context);
	static void GetEmitterData(const FDFXExt_StackItemReference& EmitterRef, FDFXExt_EmitterData& OutData,
		FDreamFXExternalEditContext& Context);
	static void SetEmitterData(const FDFXExt_StackItemReference& EmitterRef, const FDFXExt_EmitterData& InData,
		FDreamFXExternalEditContext& Context);
	static void GetRendererData(const FDFXExt_StackItemReference& RendererRef, FDFXExt_RendererData& OutData,
		FDreamFXExternalEditContext& Context);
	static void SetRendererData(const FDFXExt_StackItemReference& RendererRef, const FDFXExt_RendererData& InData,
		FDreamFXExternalEditContext& Context);

	// --- structural edits -------------------------------------------------------------------
	static void AddEmitter(UNiagaraEmitter* TemplateEmitter, FName EmitterName,
		FDFXExt_EmitterTopology& OutTopology, FDreamFXExternalEditContext& Context);
	static void RemoveEmitter(const FDFXExt_StackItemReference& EmitterRef, FDreamFXExternalEditContext& Context);

	static void AddModule(const FDFXExt_StackItemReference& NewModuleLocationRef, const UNiagaraScript* ModuleAsset,
		FDFXExt_ModuleTopology& OutTopology, FDreamFXExternalEditContext& Context);
	static void AddSetParametersModule(const FDFXExt_StackItemReference& NewModuleLocationRef,
		const TArray<FDFXExt_SetParameterEntry>& Parameters, FDFXExt_ModuleTopology& OutTopology,
		FDreamFXExternalEditContext& Context);
	static void RemoveModule(const FDFXExt_StackItemReference& ModuleRef, FDreamFXExternalEditContext& Context);
	static void SetModuleEnabled(const FDFXExt_StackItemReference& ModuleRef, bool bEnabled,
		FDreamFXExternalEditContext& Context);

	static void AddRenderer(const FDFXExt_StackItemReference& NewRendererLocation,
		const TSubclassOf<UNiagaraRendererProperties> RendererClass, FDFXExt_RendererRef& OutRef,
		FDreamFXExternalEditContext& Context);
	static void RemoveRenderer(const FDFXExt_StackItemReference& RendererRef, FDreamFXExternalEditContext& Context);

	// --- diagnostics ------------------------------------------------------------------------
	static void GetSystemCompileState(UNiagaraSystem* System, FDFXExt_SystemCompileState& OutState,
		FDreamFXExternalEditContext& Context);
	static void GetStackIssues(UNiagaraSystem* System, FDFXExt_StackIssues& OutIssues,
		FDreamFXExternalEditContext& Context);
};

// ---------------------------------------------------------------------------------------------
// The engine spellings, so every caller compiles unchanged. See the ODR note in the types header
// for why the definitions above are not simply named this way to begin with.

using FNiagaraExternalEditContext = FDreamFXExternalEditContext;
using UNiagaraExternalEditUtilities = FDreamFXExternalEditUtilities;

using ENiagaraExt_ScriptCompileStatus = EDFXExt_ScriptCompileStatus;
using ENiagaraExt_CompileEventSeverity = EDFXExt_CompileEventSeverity;
using ENiagaraExt_StackIssueSeverity = EDFXExt_StackIssueSeverity;
using ENiagaraExt_StackIssueFixStyle = EDFXExt_StackIssueFixStyle;

using FNiagaraExt_InstancedValue = FDFXExt_InstancedValue;
using FNiagaraExt_Variable = FDFXExt_Variable;
using FNiagaraExt_VariableValue = FDFXExt_VariableValue;
using FNiagaraExt_VariableValueBase = FDFXExt_VariableValueBase;
using FNiagaraExt_VariableValue_Enum = FDFXExt_VariableValue_Enum;
using FNiagaraExt_VariableValue_DataInterface = FDFXExt_VariableValue_DataInterface;
using FNiagaraExt_VariableValue_Object = FDFXExt_VariableValue_Object;
using FNiagaraExt_UserVariable = FDFXExt_UserVariable;
using FNiagaraExt_UserVariables = FDFXExt_UserVariables;
using FNiagaraExt_SetParameterEntry = FDFXExt_SetParameterEntry;

using FNiagaraExt_StackInputData = FDFXExt_StackInputData;
using FNiagaraExt_StackInputData_Enum = FDFXExt_StackInputData_Enum;
using FNiagaraExt_StackInputData_Linked = FDFXExt_StackInputData_Linked;
using FNiagaraExt_StackInputData_HlslExpression = FDFXExt_StackInputData_HlslExpression;
using FNiagaraExt_StackInputData_DataInterface = FDFXExt_StackInputData_DataInterface;
using FNiagaraExt_StackInputData_DynamicInput = FDFXExt_StackInputData_DynamicInput;
using FNiagaraExt_StackInputData_Unsupported = FDFXExt_StackInputData_Unsupported;
using FNiagaraExt_StackInputValue = FDFXExt_StackInputValue;
using FNiagaraExt_StackInputValueEntry = FDFXExt_StackInputValueEntry;
using FNiagaraExt_StackInputTopology = FDFXExt_StackInputTopology;
using FNiagaraExt_StackInputSchema = FDFXExt_StackInputSchema;
using FNiagaraExt_ModuleInputValues = FDFXExt_ModuleInputValues;
using FNiagaraExt_ModuleTopology = FDFXExt_ModuleTopology;
using FNiagaraExt_ModuleSchema = FDFXExt_ModuleSchema;
using FNiagaraExt_DynamicInputSchema = FDFXExt_DynamicInputSchema;
using FNiagaraExt_ScriptStackTopology = FDFXExt_ScriptStackTopology;
using FNiagaraExt_EmitterTopology = FDFXExt_EmitterTopology;
using FNiagaraExt_EmitterSummary = FDFXExt_EmitterSummary;
using FNiagaraExt_SystemSummary = FDFXExt_SystemSummary;
using FNiagaraExt_RendererRef = FDFXExt_RendererRef;
using FNiagaraExt_SystemData = FDFXExt_SystemData;
using FNiagaraExt_EmitterData = FDFXExt_EmitterData;
using FNiagaraExt_RendererData = FDFXExt_RendererData;
using FNiagaraExt_StackItemReference = FDFXExt_StackItemReference;
using FNiagaraExt_CompileEvent = FDFXExt_CompileEvent;
using FNiagaraExt_ScriptCompileInfo = FDFXExt_ScriptCompileInfo;
using FNiagaraExt_SystemCompileState = FDFXExt_SystemCompileState;
using FNiagaraExt_StackIssue = FDFXExt_StackIssue;
using FNiagaraExt_StackIssueFix = FDFXExt_StackIssueFix;
using FNiagaraExt_StackIssues = FDFXExt_StackIssues;
using FNiagaraExt_DynamicInputChain = FDFXExt_DynamicInputChain;
using FNiagaraExt_DynamicInputChainRef = FDFXExt_DynamicInputChainRef;
