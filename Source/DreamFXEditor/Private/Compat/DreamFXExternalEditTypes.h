#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"
#include "NiagaraTypes.h"
#include "NiagaraVariableMetaData.h"
#include "StructUtils/InstancedStruct.h"
#include "Templates/SubclassOf.h"

#include "DreamFXExternalEditTypes.generated.h"

class UNiagaraDataInterface;
class UNiagaraRendererProperties;
class UNiagaraScript;
class UNiagaraSystem;
struct FNiagaraVariant;
struct FDreamFXExternalEditContext;

/**
 * The data layer of Niagara's external edit API, re-declared for engines that do not ship it.
 *
 * Epic introduced NiagaraExternalSystemEditorUtilities.h in 5.8. On 5.6 and 5.7 it is the ONE header
 * of the 93 DreamFX includes that is absent -- everything else the adapter touches is present and
 * exported. So the cheapest faithful way to run there is not a second adapter but this: the same
 * types, and an implementation written on the same view models the engine's own version uses.
 *
 * WHY THE NAMES ARE DIFFERENT HERE. Each type below mirrors an FNiagaraExt_* struct, and the sibling
 * header aliases the engine spelling onto it so the adapter compiles unchanged. The mirrors could
 * simply have been given the engine's own names -- but UnrealHeaderTool parses every header under a
 * module directory whether or not it is on the include path, so on 5.8 these types would be built and
 * registered ALONGSIDE the engine's, two distinct C++ definitions sharing one name in one binary.
 * That is an ODR violation, and the path it would put at risk is the shipped one. A rename costs a
 * block of aliases and takes the risk to zero, so the mirrors are named and the aliases do the work.
 *
 * Field-for-field fidelity with the 5.8 header is deliberate and load-bearing: the adapter reads these
 * fields by name, and a mirror that quietly renamed one would compile on 5.8 and fail only here.
 */

/** Mirror of ENiagaraExt_ScriptCompileStatus. Order matches ENiagaraScriptCompileStatus. */
UENUM()
enum class EDFXExt_ScriptCompileStatus : uint8
{
	Unknown,
	Dirty,
	Error,
	UpToDate,
	BeingCreated,
	UpToDateWithWarnings,
	ComputeUpToDateWithWarnings,
};

/** Mirror of ENiagaraExt_CompileEventSeverity. */
UENUM()
enum class EDFXExt_CompileEventSeverity : uint8
{
	Log,
	Display,
	Warning,
	Error,
};

/** Mirror of ENiagaraExt_StackIssueSeverity. */
UENUM()
enum class EDFXExt_StackIssueSeverity : uint8
{
	Error,
	Warning,
	Info,
	None,
};

/** Mirror of ENiagaraExt_StackIssueFixStyle. */
UENUM()
enum class EDFXExt_StackIssueFixStyle : uint8
{
	Fix,
	Link,
};

/** Mirror of FNiagaraExt_InstancedValue. */
USTRUCT()
struct FDFXExt_InstancedValue : public FInstancedStruct
{
	GENERATED_BODY()
};

/** Mirror of FNiagaraExt_VariableValueBase. */
USTRUCT()
struct FDFXExt_VariableValueBase
{
	GENERATED_BODY()
};

/** Mirror of FNiagaraExt_VariableValue_Enum. */
USTRUCT()
struct FDFXExt_VariableValue_Enum : public FDFXExt_VariableValueBase
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UEnum> Enum = nullptr;

	UPROPERTY()
	FName EnumName;

	UPROPERTY()
	FText DisplayName;
};

/** Mirror of FNiagaraExt_VariableValue_DataInterface. */
USTRUCT()
struct FDFXExt_VariableValue_DataInterface : public FDFXExt_VariableValueBase
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UNiagaraDataInterface> DataInterfaceClass;

	UPROPERTY()
	TObjectPtr<UNiagaraDataInterface> DataInterface = nullptr;
};

/** Mirror of FNiagaraExt_VariableValue_Object. */
USTRUCT()
struct FDFXExt_VariableValue_Object : public FDFXExt_VariableValueBase
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UObject> ObjectClass;

	UPROPERTY()
	TObjectPtr<UObject> Object = nullptr;
};

/**
 * Mirror of FNiagaraExt_VariableValue.
 *
 * Holds either one of the three wrapper structs above or a raw typed literal (FNiagaraFloat,
 * FVector3f, FLinearColor, ...), which is why it is an FInstancedStruct rather than a union: the
 * adapter reads a literal back out as GetScriptStruct()/GetMemory() and hands the pair straight to
 * its own FInputValue::MakeLiteral.
 */
USTRUCT()
struct FDFXExt_VariableValue : public FDFXExt_InstancedValue
{
	GENERATED_BODY()

	/** Fills OutVariant from this value, using TypeDef to decide how a literal is packed. */
	void Get(FNiagaraVariant& OutVariant, FDreamFXExternalEditContext& Context) const;

	/** Stores Variant as this value: object and data interface variants become the wrappers above. */
	void Set(const FNiagaraTypeDefinition& TypeDef, const FNiagaraVariant& Variant);
};

/** Mirror of FNiagaraExt_Variable. */
USTRUCT()
struct FDFXExt_Variable
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	UPROPERTY()
	FNiagaraTypeDefinition Type;
};

/** Mirror of FNiagaraExt_UserVariable. */
USTRUCT()
struct FDFXExt_UserVariable : public FDFXExt_Variable
{
	GENERATED_BODY()

	UPROPERTY()
	FDFXExt_VariableValue DefaultValue;

	UPROPERTY()
	FText Description;
};

/** Mirror of FNiagaraExt_UserVariables. */
USTRUCT()
struct FDFXExt_UserVariables
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FDFXExt_UserVariable> UserVariables;
};

/** Mirror of FNiagaraExt_SetParameterEntry. */
USTRUCT()
struct FDFXExt_SetParameterEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FDFXExt_Variable Variable;

	UPROPERTY()
	FDFXExt_VariableValue DefaultValue;
};

/** Mirror of FNiagaraExt_StackInputData -- the base every input payload derives from. */
USTRUCT()
struct FDFXExt_StackInputData
{
	GENERATED_BODY()
};

/** Mirror of FNiagaraExt_StackInputData_Enum. */
USTRUCT()
struct FDFXExt_StackInputData_Enum : public FDFXExt_StackInputData
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UEnum> Enum = nullptr;

	UPROPERTY()
	FName EnumName;

	UPROPERTY()
	FText DisplayName;
};

/** Mirror of FNiagaraExt_StackInputData_Linked. */
USTRUCT()
struct FDFXExt_StackInputData_Linked : public FDFXExt_StackInputData
{
	GENERATED_BODY()

	UPROPERTY()
	FDFXExt_Variable LinkedVariable;
};

/** Mirror of FNiagaraExt_StackInputData_HlslExpression. */
USTRUCT()
struct FDFXExt_StackInputData_HlslExpression : public FDFXExt_StackInputData
{
	GENERATED_BODY()

	UPROPERTY()
	FString HlslExpression;
};

/** Mirror of FNiagaraExt_StackInputData_DataInterface. */
USTRUCT()
struct FDFXExt_StackInputData_DataInterface : public FDFXExt_StackInputData
{
	GENERATED_BODY()

	UPROPERTY()
	FString PropertyValues;
};

/** Mirror of FNiagaraExt_StackInputData_DynamicInput. */
USTRUCT()
struct FDFXExt_StackInputData_DynamicInput : public FDFXExt_StackInputData
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UNiagaraScript> DynamicInputAsset = nullptr;
};

/** Mirror of FNiagaraExt_StackInputData_Unsupported. */
USTRUCT()
struct FDFXExt_StackInputData_Unsupported : public FDFXExt_StackInputData
{
	GENERATED_BODY()
};

/** Mirror of FNiagaraExt_StackInputValue. */
USTRUCT()
struct FDFXExt_StackInputValue : public FDFXExt_InstancedValue
{
	GENERATED_BODY()
};

/** Mirror of FNiagaraExt_StackInputTopology. */
USTRUCT()
struct FDFXExt_StackInputTopology : public FDFXExt_Variable
{
	GENERATED_BODY()

	UPROPERTY()
	bool bIsVisible = true;

	UPROPERTY()
	bool bIsEditable = true;

	UPROPERTY()
	bool bIsDynamic = false;

	UPROPERTY()
	bool bIsStaticSwitch = false;
};

/** Mirror of FNiagaraExt_StackInputValueEntry. */
USTRUCT()
struct FDFXExt_StackInputValueEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	UPROPERTY()
	FDFXExt_StackInputValue Value;
};

/** Mirror of FNiagaraExt_ModuleInputValues. */
USTRUCT()
struct FDFXExt_ModuleInputValues
{
	GENERATED_BODY()

	UPROPERTY()
	FName ModuleName;

	UPROPERTY()
	TArray<FDFXExt_StackInputValueEntry> Inputs;
};

/** Mirror of FNiagaraExt_ModuleTopology. */
USTRUCT()
struct FDFXExt_ModuleTopology
{
	GENERATED_BODY()

	UPROPERTY()
	FName ModuleName;

	UPROPERTY()
	bool Enabled = true;

	UPROPERTY()
	TObjectPtr<UNiagaraScript> ModuleScript = nullptr;

	UPROPERTY()
	bool bIsSetParametersModule = false;

	UPROPERTY()
	TArray<FDFXExt_StackInputTopology> Inputs;
};

/** Mirror of FNiagaraExt_ScriptStackTopology. */
USTRUCT()
struct FDFXExt_ScriptStackTopology
{
	GENERATED_BODY()

	UPROPERTY()
	FName ScriptName;

	UPROPERTY()
	TArray<FDFXExt_ModuleTopology> Modules;
};

/** Mirror of FNiagaraExt_RendererRef. */
USTRUCT()
struct FDFXExt_RendererRef
{
	GENERATED_BODY()

	UPROPERTY()
	int32 RendererIndex = INDEX_NONE;

	UPROPERTY()
	TSubclassOf<UNiagaraRendererProperties> RendererClass;
};

/** Mirror of FNiagaraExt_EmitterSummary. */
USTRUCT()
struct FDFXExt_EmitterSummary
{
	GENERATED_BODY()

	UPROPERTY()
	FName EmitterName;

	UPROPERTY()
	bool bEnabled = true;

	UPROPERTY()
	ENiagaraSimTarget SimTarget = ENiagaraSimTarget::CPUSim;

	UPROPERTY()
	TArray<TSubclassOf<UNiagaraRendererProperties>> RendererClasses;
};

/** Mirror of FNiagaraExt_SystemSummary. */
USTRUCT()
struct FDFXExt_SystemSummary
{
	GENERATED_BODY()

	UPROPERTY()
	FName SystemName;

	UPROPERTY()
	TArray<FDFXExt_UserVariable> UserVariables;

	UPROPERTY()
	TArray<FDFXExt_EmitterSummary> Emitters;
};

/** Mirror of FNiagaraExt_EmitterTopology. */
USTRUCT()
struct FDFXExt_EmitterTopology
{
	GENERATED_BODY()

	UPROPERTY()
	FName EmitterName;

	UPROPERTY()
	bool bEnabled = true;

	UPROPERTY()
	ENiagaraSimTarget SimTarget = ENiagaraSimTarget::CPUSim;

	UPROPERTY()
	TArray<TSubclassOf<UNiagaraRendererProperties>> RendererClasses;

	UPROPERTY()
	FDFXExt_ScriptStackTopology EmitterSpawnScript;

	UPROPERTY()
	FDFXExt_ScriptStackTopology EmitterUpdateScript;

	UPROPERTY()
	FDFXExt_ScriptStackTopology ParticleSpawnScript;

	UPROPERTY()
	FDFXExt_ScriptStackTopology ParticleUpdateScript;

	UPROPERTY()
	TArray<FDFXExt_RendererRef> Renderers;
};

/** Mirror of FNiagaraExt_SystemData. */
USTRUCT()
struct FDFXExt_SystemData
{
	GENERATED_BODY()

	UPROPERTY()
	FString PropertyValues;
};

/** Mirror of FNiagaraExt_EmitterData. */
USTRUCT()
struct FDFXExt_EmitterData
{
	GENERATED_BODY()

	UPROPERTY()
	FString PropertyValues;
};

/** Mirror of FNiagaraExt_RendererData. */
USTRUCT()
struct FDFXExt_RendererData
{
	GENERATED_BODY()

	UPROPERTY()
	FString PropertyValues;
};

/** Mirror of FNiagaraExt_StackInputSchema. */
USTRUCT()
struct FDFXExt_StackInputSchema
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	UPROPERTY()
	FNiagaraTypeDefinition Type;

	UPROPERTY()
	FText Category;

	UPROPERTY()
	bool bSupportsExpressions = false;

	UPROPERTY()
	FNiagaraVariableMetaData MetaData;
};

/** Mirror of FNiagaraExt_ModuleSchema. */
USTRUCT()
struct FDFXExt_ModuleSchema
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<const UNiagaraScript> Asset = nullptr;

	UPROPERTY()
	TArray<FDFXExt_StackInputSchema> Inputs;

	UPROPERTY()
	TArray<FDFXExt_Variable> Outputs;
};

/** Mirror of FNiagaraExt_DynamicInputSchema. */
USTRUCT()
struct FDFXExt_DynamicInputSchema : public FDFXExt_ModuleSchema
{
	GENERATED_BODY()
};

/**
 * Mirror of FNiagaraExt_StackItemReference.
 *
 * The engine's version caches the resolved view model entries on itself and clears the cache on copy.
 * This one does not cache at all: resolution here is a walk of a stack view model that the context
 * already holds, the walk is cheap next to building that view model, and a cache that must be
 * invalidated correctly is exactly the kind of state an unverified compatibility layer should not own.
 */
USTRUCT()
struct FDFXExt_StackItemReference
{
	GENERATED_BODY()

	FDFXExt_StackItemReference() = default;

	FDFXExt_StackItemReference(UNiagaraSystem* InSystem, FName InEmitterName = NAME_None,
		FName InScriptName = NAME_None, FName InModuleName = NAME_None)
		: System(InSystem), EmitterName(InEmitterName), ScriptName(InScriptName), ModuleName(InModuleName)
	{
	}

	UPROPERTY()
	TObjectPtr<UNiagaraSystem> System = nullptr;

	UPROPERTY()
	FName EmitterName;

	UPROPERTY()
	FName ScriptName;

	UPROPERTY()
	FName ModuleName;

	UPROPERTY()
	int32 RendererIndex = INDEX_NONE;

	UPROPERTY()
	TArray<FName> InputNameStack;
};

/** Mirror of FNiagaraExt_CompileEvent. */
USTRUCT()
struct FDFXExt_CompileEvent
{
	GENERATED_BODY()

	UPROPERTY()
	EDFXExt_CompileEventSeverity Severity = EDFXExt_CompileEventSeverity::Log;

	UPROPERTY()
	FString Message;

	UPROPERTY()
	FString ShortDescription;

	UPROPERTY()
	FGuid NodeGuid;

	UPROPERTY()
	FGuid PinGuid;

	UPROPERTY()
	bool bFromScriptDependency = false;
};

/** Mirror of FNiagaraExt_ScriptCompileInfo. */
USTRUCT()
struct FDFXExt_ScriptCompileInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FName EmitterName;

	UPROPERTY()
	FName ScriptName;

	UPROPERTY()
	EDFXExt_ScriptCompileStatus LastCompileStatus = EDFXExt_ScriptCompileStatus::Unknown;

	UPROPERTY()
	FString ErrorSummary;

	UPROPERTY()
	TArray<FDFXExt_CompileEvent> CompileEvents;
};

/** Mirror of FNiagaraExt_SystemCompileState. */
USTRUCT()
struct FDFXExt_SystemCompileState
{
	GENERATED_BODY()

	UPROPERTY()
	EDFXExt_ScriptCompileStatus AggregateStatus = EDFXExt_ScriptCompileStatus::Unknown;

	UPROPERTY()
	bool bIsCompiling = false;

	UPROPERTY()
	bool bIsStale = false;

	UPROPERTY()
	bool bHasErrors = false;

	UPROPERTY()
	bool bHasWarnings = false;

	UPROPERTY()
	TArray<FDFXExt_ScriptCompileInfo> Scripts;
};

/** Mirror of FNiagaraExt_StackIssueFix. */
USTRUCT()
struct FDFXExt_StackIssueFix
{
	GENERATED_BODY()

	UPROPERTY()
	FString FixId;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	EDFXExt_StackIssueFixStyle Style = EDFXExt_StackIssueFixStyle::Fix;
};

/** Mirror of FNiagaraExt_StackIssue. */
USTRUCT()
struct FDFXExt_StackIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString IssueId;

	UPROPERTY()
	EDFXExt_StackIssueSeverity Severity = EDFXExt_StackIssueSeverity::None;

	UPROPERTY()
	FString ShortDescription;

	UPROPERTY()
	FString LongDescription;

	UPROPERTY()
	bool bCanBeDismissed = false;

	UPROPERTY()
	bool bIsDismissed = false;

	UPROPERTY()
	FDFXExt_StackItemReference Location;

	UPROPERTY()
	FString StackDisplayPath;

	UPROPERTY()
	TArray<FDFXExt_StackIssueFix> Fixes;
};

/** Mirror of FNiagaraExt_StackIssues. */
USTRUCT()
struct FDFXExt_StackIssues
{
	GENERATED_BODY()

	UPROPERTY()
	int32 NumErrors = 0;

	UPROPERTY()
	int32 NumWarnings = 0;

	UPROPERTY()
	int32 NumInfos = 0;

	UPROPERTY()
	TArray<FDFXExt_StackIssue> Issues;
};

struct FDFXExt_DynamicInputChain;

/**
 * Mirror of FNiagaraExt_DynamicInputChainRef -- the wrapper that lets a chain entry hold its children.
 *
 * The three members are defined inline at the bottom of this file rather than in the implementation.
 * They have to be: UnrealHeaderTool registers this struct on EVERY engine, including the ones that
 * have the real API and compile the implementation away to nothing, and the generated registration
 * calls the default constructor. Defining it in the guarded translation unit linked on 5.6 and 5.7 and
 * failed on 5.8 with an unresolved external that named the generated code as its only caller.
 */
USTRUCT()
struct FDFXExt_DynamicInputChainRef : public FDFXExt_InstancedValue
{
	GENERATED_BODY()

	FDFXExt_DynamicInputChainRef();

	const FDFXExt_DynamicInputChain& Get() const;
	FDFXExt_DynamicInputChain& GetMutable();
};

/** Mirror of FNiagaraExt_DynamicInputChain. */
USTRUCT()
struct FDFXExt_DynamicInputChain
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	UPROPERTY()
	FNiagaraTypeDefinition Type;

	UPROPERTY()
	bool bIsVisible = true;

	UPROPERTY()
	bool bIsEditable = true;

	UPROPERTY()
	bool bIsStaticSwitch = false;

	UPROPERTY()
	FDFXExt_StackInputValue Value;

	UPROPERTY()
	TArray<FDFXExt_DynamicInputChainRef> Inputs;
};

// Defined here rather than above because the payload type has to be complete first, and defined in the
// header rather than the implementation because the reflection registration needs them on every engine.

inline FDFXExt_DynamicInputChainRef::FDFXExt_DynamicInputChainRef()
{
	InitializeAs<FDFXExt_DynamicInputChain>();
}

inline const FDFXExt_DynamicInputChain& FDFXExt_DynamicInputChainRef::Get() const
{
	return FInstancedStruct::Get<FDFXExt_DynamicInputChain>();
}

inline FDFXExt_DynamicInputChain& FDFXExt_DynamicInputChainRef::GetMutable()
{
	return FInstancedStruct::GetMutable<FDFXExt_DynamicInputChain>();
}
