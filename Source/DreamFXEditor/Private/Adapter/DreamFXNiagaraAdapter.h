#pragma once

#include "CoreMinimal.h"
#include "DreamFXTypes.h"
#include "NiagaraTypes.h"

class UNiagaraDataInterface;
class UNiagaraEmitter;
class UNiagaraRendererProperties;
class UNiagaraScript;
class UNiagaraSystem;

/**
 * The one and only place DreamFX talks to UNiagaraExternalEditUtilities.
 *
 * Why this file exists (plan doc R2): the external edit API carries a triple EXPERIMENTAL warning and
 * has no other caller anywhere in the engine. Confining every call to one translation unit means an
 * engine upgrade that reshapes it costs one file, not a lowering pass sprayed with FNiagaraExt_*.
 *
 * It is also the portability boundary for design principle 4 (zero external code modification). Only
 * symbols that exist in a stock engine may be used here. Audited surface, all from
 * NiagaraExternalSystemEditorUtilities.h and NiagaraEmitterFactoryNew.h:
 *
 *   CreateNiagaraSystem, AddUserVariable, RemoveUserVariable, GetUserVariables,
 *   AddEmitter, RemoveEmitter, GetEmitterTopology, GetEmitterSummary, GetSystemSummary,
 *   GetScriptStackTopology, GetModuleTopology,
 *   AddModule, RemoveModule, SetModuleEnabled, AddSetParametersModule, AddSetParameterEntry,
 *   AddRenderer, RemoveRenderer, SetRendererData, GetRendererData, SetEmitterData, GetEmitterData,
 *   GetModuleSchema(asset), GetDynamicInputSchema(asset), GetStackInputSchema, GetAvailableDynamicInputs,
 *   GetStackInputData, SetStackInputData, GetModuleInputValues, GetDynamicInputChain,
 *   GetSystemCompileState, GetStackIssues,
 *   UNiagaraEmitterFactoryNew::InitializeEmitter
 *
 * No symbol outside that list may be added without re-running the upstream comparison in plan 2.5.
 * DreamFX types never leak Niagara editor structs: everything crossing this boundary is either a
 * plain struct declared below or a Niagara *runtime* type (FNiagaraTypeDefinition, UNiagaraScript),
 * which is stable public API.
 */
namespace UE::DreamFX::Editor
{
	/** Where in a system something lives. Mirrors FNiagaraExt_StackItemReference without exposing it. */
	struct FStackAddress
	{
		UNiagaraSystem* System = nullptr;
		FName EmitterName;
		FName ScriptName;
		FName ModuleName;
		int32 RendererIndex = INDEX_NONE;
		TArray<FName> InputNameStack;

		FStackAddress() = default;
		explicit FStackAddress(UNiagaraSystem* InSystem) : System(InSystem) {}

		FStackAddress WithEmitter(FName InEmitterName) const;
		FStackAddress WithScript(FName InScriptName) const;
		FStackAddress WithModule(FName InModuleName) const;
		FStackAddress WithRenderer(int32 InRendererIndex) const;
		FStackAddress WithInput(FName InInputName) const;
		FStackAddress WithInputPath(const TArray<FName>& InPath) const;
	};

	enum class EInputValueMode : uint8
	{
		Unset,
		/** Typed literal held as raw struct bytes -- FNiagaraFloat, FVector3f, FLinearColor, ... */
		Literal,
		Enum,
		Linked,
		Hlsl,
		DynamicInput,
		DataInterface,
	};

	/** A value bound for a module input, in the shape the adapter can translate on its own. */
	struct FInputValue
	{
		EInputValueMode Mode = EInputValueMode::Unset;

		const UScriptStruct* LiteralStruct = nullptr;
		TArray<uint8> LiteralBytes;

		UEnum* EnumType = nullptr;
		FName EnumEntryName;

		FNiagaraVariableBase LinkedVariable;

		FString HlslExpression;

		UNiagaraScript* DynamicInputAsset = nullptr;

		/** JSON blob of data interface properties, matching GetDataInterfaceSchema. */
		FString DataInterfaceJson;
		UClass* DataInterfaceClass = nullptr;

		bool IsSet() const { return Mode != EInputValueMode::Unset; }

		/**
		 * Value equality, used by the decompiler to decide whether an input is worth printing.
		 * R8: the API reports resolved values with no "was this explicitly set" flag, so comparing
		 * against a pristine baseline is the only way to keep an export readable.
		 */
		bool Equals(const FInputValue& Other) const;

		static FInputValue MakeLiteral(const UScriptStruct* Struct, const void* SourceMemory);
		static FInputValue MakeEnum(UEnum* Enum, FName EntryName);
		static FInputValue MakeLinked(const FNiagaraVariableBase& Variable);
		static FInputValue MakeHlsl(const FString& Expression);
		static FInputValue MakeDynamicInput(UNiagaraScript* Asset);
		static FInputValue MakeDataInterface(UClass* Class, const FString& Json);
	};

	/** Static description of one module input, from the module asset alone (no owning system needed). */
	struct FInputSchema
	{
		FName Name;
		FNiagaraTypeDefinition Type;
		FString Category;
		FString Description;
		bool bSupportsExpressions = false;
		bool bIsStaticSwitch = false;
	};

	/**
	 * Niagara input names contain spaces ("Loop Duration", "Sprite Size Mode"), which no DSL
	 * identifier can. Normalising both sides -- lowercase, spaces and underscores removed -- lets an
	 * author write `LoopDuration` and still address the real input, without inventing a per-module
	 * alias table that would rot the moment a module is revised.
	 */
	FString NormalizeInputIdentifier(const FString& Name);

	/** The identifier an author writes for a given Niagara input name: "Loop Duration" -> "LoopDuration". */
	FString ToInputIdentifier(FName InputName);

	struct FModuleSchema
	{
		TArray<FInputSchema> Inputs;

		const FInputSchema* FindInput(FName Name) const;

		/** Exact match first, then the normalised comparison described above. */
		const FInputSchema* FindInputByIdentifier(const FString& Identifier) const;
	};

	/** Live per-input state on a module that has already been added to a stack. */
	struct FInputInfo
	{
		FName Name;
		FNiagaraTypeDefinition Type;
		bool bVisible = true;
		bool bEditable = true;
		bool bDynamic = false;
		bool bStaticSwitch = false;
	};

	struct FModuleInfo
	{
		FName ModuleName;
		bool bEnabled = true;
		UNiagaraScript* Script = nullptr;
		bool bIsSetParameters = false;
		TArray<FInputInfo> Inputs;

		const FInputInfo* FindInput(FName Name) const;
	};

	struct FScriptStackInfo
	{
		FName ScriptName;
		TArray<FModuleInfo> Modules;
	};

	struct FRendererInfo
	{
		int32 Index = INDEX_NONE;
		UClass* Class = nullptr;
	};

	struct FEmitterInfo
	{
		FName Name;
		bool bEnabled = true;
		TArray<FScriptStackInfo> Stacks;
		TArray<FRendererInfo> Renderers;

		const FScriptStackInfo* FindStack(FName ScriptName) const;
	};

	struct FCompileEventInfo
	{
		/** 0 Log, 1 Display, 2 Warning, 3 Error -- mirrors ENiagaraExt_CompileEventSeverity. */
		int32 Severity = 0;
		FString Message;
		FString ShortDescription;
		FName EmitterName;
		FName ScriptName;
		bool bFromDependency = false;
	};

	struct FCompileStateInfo
	{
		FString StatusName;
		bool bHasErrors = false;
		bool bHasWarnings = false;
		bool bIsStale = false;
		TArray<FCompileEventInfo> Events;
	};

	struct FStackIssueInfo
	{
		/** 0 Error, 1 Warning, 2 Info, 3 None -- mirrors ENiagaraExt_StackIssueSeverity. */
		int32 Severity = 3;
		FString ShortDescription;
		FString LongDescription;
		FString DisplayPath;
		FName EmitterName;
		FName ScriptName;
		FName ModuleName;
	};

	/** One user-exposed parameter as stored on the asset. Names come back namespace-qualified. */
	struct FUserVariableInfo
	{
		FName Name;
		FNiagaraTypeDefinition Type;
		FString Description;
	};

	class FNiagaraAdapter
	{
	public:
		// --- system lifecycle ----------------------------------------------------------------

		/**
		 * Returns the system at PackagePath/AssetName, creating it if absent.
		 *
		 * R9: an existing package MUST be fully loaded before CreateNiagaraSystem touches the path,
		 * otherwise SavePackage's ValidatePackage raises appError and takes the process down. That is
		 * the normal DreamFX case (regeneration), so the fully-load happens unconditionally here.
		 */
		static UNiagaraSystem* AcquireSystem(const FString& PackagePath, const FString& AssetName,
			bool& bOutCreated, TArray<FString>& OutErrors);

		static bool SaveSystem(UNiagaraSystem* System, TArray<FString>& OutErrors);

		// --- topology reads ------------------------------------------------------------------

		static bool GetEmitterNames(UNiagaraSystem* System, TArray<FName>& OutNames, TArray<FString>& OutErrors);
		static bool GetEmitterInfo(const FStackAddress& EmitterAddress, FEmitterInfo& OutInfo, TArray<FString>& OutErrors);

		/**
		 * One script stack's modules in execution order. Needed for the two system-scope stacks, which
		 * GetEmitterInfo cannot reach because they have no owning emitter.
		 */
		static bool GetScriptStackInfo(const FStackAddress& ScriptAddress, FScriptStackInfo& OutInfo, TArray<FString>& OutErrors);
		static bool GetModuleInfo(const FStackAddress& ModuleAddress, FModuleInfo& OutInfo, TArray<FString>& OutErrors);
		static bool GetUserVariables(UNiagaraSystem* System, TArray<FUserVariableInfo>& OutVariables, TArray<FString>& OutErrors);

		// --- structural writes ---------------------------------------------------------------

		static bool AddUserVariable(UNiagaraSystem* System, FName Name, const FNiagaraTypeDefinition& Type,
			const FString& Description, const FInputValue& DefaultValue, TArray<FString>& OutErrors);
		static bool RemoveUserVariable(UNiagaraSystem* System, FName Name, const FNiagaraTypeDefinition& Type,
			TArray<FString>& OutErrors);

		/**
		 * Adds an emitter. AddEmitter rejects a null template outright, so a transient emitter is
		 * synthesised through the engine's own UNiagaraEmitterFactoryNew::InitializeEmitter with
		 * bAddDefaultModulesAndRenderers = false: DreamFX declares every module itself, and inheriting
		 * a default stack would mean the text no longer describes the asset.
		 */
		static bool AddEmitter(UNiagaraSystem* System, FName EmitterName, TArray<FString>& OutErrors);
		static bool RemoveEmitter(const FStackAddress& EmitterAddress, TArray<FString>& OutErrors);

		static bool AddModule(const FStackAddress& StackAddress, UNiagaraScript* ModuleAsset,
			FName& OutModuleName, TArray<FString>& OutErrors);
		static bool RemoveModule(const FStackAddress& ModuleAddress, TArray<FString>& OutErrors);
		static bool SetModuleEnabled(const FStackAddress& ModuleAddress, bool bEnabled, TArray<FString>& OutErrors);

		/** Creates a Set Parameters module holding the given entries, and reports its module name. */
		static bool AddSetParametersModule(const FStackAddress& StackAddress,
			const TArray<TTuple<FName, FNiagaraTypeDefinition, FInputValue>>& Entries,
			FName& OutModuleName, TArray<FString>& OutErrors);

		static bool AddRenderer(const FStackAddress& EmitterAddress, UClass* RendererClass,
			int32& OutRendererIndex, TArray<FString>& OutErrors);
		static bool RemoveRenderer(const FStackAddress& RendererAddress, TArray<FString>& OutErrors);

		// --- value writes --------------------------------------------------------------------

		static bool SetInput(const FStackAddress& InputAddress, const FInputValue& Value, TArray<FString>& OutErrors);
		static bool SetRendererProperties(const FStackAddress& RendererAddress, const FString& PropertiesJson,
			TArray<FString>& OutErrors);

		/**
		 * Points a renderer's attribute binding at a parameter: `Bind SpriteSize -> Particles.SpriteSize`.
		 *
		 * Not routed through SetRendererProperties because FNiagaraVariableAttributeBinding is a
		 * derived struct -- writing its serialised fields as JSON would leave the cached display name,
		 * data-set name and source-mode flags inconsistent. Its own SetValue() recomputes all of them,
		 * so the property is found by reflection and that function is called instead.
		 *
		 * @param BindingName  the DSL-facing name, e.g. "SpriteSize"; the "Binding" suffix is optional
		 */
		static bool SetRendererBinding(const FStackAddress& RendererAddress, const FString& BindingName,
			FName TargetParameter, TArray<FString>& OutErrors);

		/** Every attribute binding a renderer class exposes, in DSL spelling. For error messages. */
		static void ListRendererBindings(const UClass* RendererClass, TArray<FString>& OutNames);

		/** Reads a renderer's current bindings as {DSL name, bound parameter}. For the decompiler. */
		static bool GetRendererBindings(const FStackAddress& RendererAddress,
			TArray<TPair<FString, FName>>& OutBindings, TArray<FString>& OutErrors);
		static bool SetEmitterProperties(const FStackAddress& EmitterAddress, const FString& PropertiesJson,
			TArray<FString>& OutErrors);
		static bool SetSystemProperties(UNiagaraSystem* System, const FString& PropertiesJson,
			TArray<FString>& OutErrors);

		// --- value reads (decompiler) --------------------------------------------------------

		static bool GetInput(const FStackAddress& InputAddress, FInputValue& OutValue, TArray<FString>& OutErrors);
		static bool GetRendererProperties(const FStackAddress& RendererAddress, FString& OutJson, TArray<FString>& OutErrors);
		static bool GetEmitterProperties(const FStackAddress& EmitterAddress, FString& OutJson, TArray<FString>& OutErrors);
		static bool GetSystemProperties(UNiagaraSystem* System, FString& OutJson, TArray<FString>& OutErrors);

		/** Resolved values for every input on a module, in the same order as FModuleInfo::Inputs. */
		static bool GetModuleInputValues(const FStackAddress& ModuleAddress,
			TArray<TTuple<FName, FInputValue>>& OutValues, TArray<FString>& OutErrors);

		/**
		 * One level of a dynamic-input chain: each direct input's name and whether it can be written.
		 *
		 * Editability matters to the decompiler as much as to the generator. An input whose
		 * EditCondition is false is refused by SetStackInputData, so exporting it produces a file that
		 * does not rebuild.
		 */
		static bool GetDynamicInputChildren(const FStackAddress& InputAddress,
			TArray<TPair<FName, bool>>& OutChildren, TArray<FString>& OutErrors);

		// --- schema --------------------------------------------------------------------------

		static bool GetModuleSchema(const UNiagaraScript* ModuleAsset, FModuleSchema& OutSchema, TArray<FString>& OutErrors);

		/** Schema of one live input, used to learn whether it accepts an HLSL expression. */
		static bool GetInputSchema(const FStackAddress& InputAddress, FInputSchema& OutSchema, TArray<FString>& OutErrors);
		static bool GetDynamicInputSchema(const UNiagaraScript* Asset, FModuleSchema& OutSchema, TArray<FString>& OutErrors);
		static void GetAvailableDynamicInputs(const FNiagaraTypeDefinition& Type, TArray<UNiagaraScript*>& OutScripts);

		// --- compile + diagnostics -----------------------------------------------------------

		/**
		 * Requests a compile and blocks until it finishes, then reports the state.
		 *
		 * Deliberately NOT PollForCompilationComplete: that forwards to QueryCompileComplete, which
		 * returns false both while a compile is in flight and once it has finished (empty
		 * ActiveCompilations), so it cannot distinguish success from "still running". The blocking
		 * wait plus the reported status is the only sound gate.
		 */
		static bool CompileAndWait(UNiagaraSystem* System, FCompileStateInfo& OutState, TArray<FString>& OutErrors);

		/**
		 * False when stack issues cannot be read in this process.
		 *
		 * GetStackIssues is the one call here that is not headless-safe: it routes through
		 * GetDiagnosticsSystemViewModel, which builds a non-data-only view model and ends up
		 * constructing Slate widgets. Callers must check this rather than treating a failed read as a
		 * build error.
		 */
		static bool IsStackIssueReadingAvailable();

		static bool GetStackIssues(UNiagaraSystem* System, TArray<FStackIssueInfo>& OutIssues, TArray<FString>& OutErrors);

		// --- naming helpers ------------------------------------------------------------------

		/**
		 * The FName spelling of an ENiagaraScriptUsage value, e.g. "ParticleUpdateScript".
		 * Derived from reflection rather than hardcoded so it stays in lockstep with the API's own
		 * ScriptUsageFromName lookup.
		 */
		static FName ScriptNameForStack(EStackKind Kind);
		static bool StackForScriptName(FName ScriptName, EStackKind& OutKind);

		/** Resolves `SpriteRenderer` and friends to the matching UNiagaraRendererProperties subclass. */
		static UClass* FindRendererClass(const FString& TypeName);
		/** Inverse of FindRendererClass, for the decompiler. */
		static FString RendererTypeNameForClass(const UClass* Class);
	};
}
