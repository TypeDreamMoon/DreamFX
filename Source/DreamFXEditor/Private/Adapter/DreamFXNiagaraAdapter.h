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

	/**
	 * A Niagara name as DreamFXLang source: bare when it is already an identifier, back-quoted when
	 * the language has no other way to hold it.
	 *
	 * Niagara names come from a UI that allows anything. This project's own content has a user
	 * parameter called `PillarPower(0~1)` and a module input called `Ring/DiscDistributionMode`;
	 * written bare, either turns the export into a file that will not parse. Dots are left bare so
	 * that `User.Speed` stays readable -- only a name that needs quoting gets it.
	 */
	FString ToNameToken(const FString& Name);

	struct FModuleSchema
	{
		TArray<FInputSchema> Inputs;

		/**
		 * What the script writes. For a dynamic input there is exactly one, and its type is what
		 * decides where the dynamic input may be plugged in -- which is what the E4-1 probe needs in
		 * order to build a host for it.
		 */
		TArray<FInputSchema> Outputs;

		const FInputSchema* FindInput(FName Name) const;

		/** Exact match first, then the normalised comparison described above. */
		const FInputSchema* FindInputByIdentifier(const FString& Identifier) const;

		/**
		 * Every input one written identifier could mean, best match first.
		 *
		 * Normalising spaces away is what lets an author write `LoopDuration` for `Loop Duration`, and
		 * it is also what makes two different inputs collide: a value input `Scale RGB` and the inline
		 * edit condition `ScaleRGB` that gates it are one identifier to the DSL. Picking the first is
		 * a coin flip, and losing it means writing a Vector3 into a checkbox. The caller resolves the
		 * ambiguity with what it knows and this does not: the value being assigned.
		 */
		void FindInputsByIdentifier(const FString& Identifier, TArray<const FInputSchema*>& OutMatches) const;
	};

	/** One level of a live dynamic input chain: what the stack really exposes, not what the asset declares. */
	struct FDynamicInputChild
	{
		FName Name;
		FNiagaraTypeDefinition Type;
		bool bVisible = true;
		bool bEditable = true;

		/**
		 * A compile-time switch on the dynamic input script.
		 *
		 * Only the live chain reports this -- the asset-level schema does not carry it at all, which
		 * is why plan-v2 left `dynamic input static switch` as a coverage gap on 7 assets.
		 */
		bool bStaticSwitch = false;
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

	/**
	 * A module or dynamic input asset's exposed version (R7).
	 *
	 * The GUID is what the provenance stamp compares, because major/minor are not unique: the same
	 * version number can be authored on two branches and mean different things.
	 */
	struct FScriptVersion
	{
		int32 Major = 0;
		int32 Minor = 0;
		FGuid Guid;
		/** False for an asset that never opted into versioning; its single version is implicit. */
		bool bVersioningEnabled = false;

		bool IsValid() const { return Guid.IsValid(); }

		/** "1.2:0A1B..." -- the form the stamp stores. */
		FString ToStampString() const;
		static bool FromStampString(const FString& Text, FScriptVersion& OutVersion);

		/** "1.2" -- the form an author writes after `@`. */
		FString ToLabel() const { return FString::Printf(TEXT("%d.%d"), Major, Minor); }
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

		/**
		 * Shares one edit context across a burst of reads of the same system.
		 *
		 * Every call into the external edit API constructs an `FNiagaraExternalEditContext`, and that
		 * constructor builds a whole `FNiagaraSystemViewModel` -- the entire stack, as UObjects. It is
		 * what a read actually costs: decompiling one third-party system spent minutes building and
		 * discarding view models, and grew the process by about 120 MB per second doing it.
		 *
		 * Inside this scope the reads listed under "topology reads" and "value reads" share a single
		 * context instead.
		 *
		 * **Only wrap code that does not mutate this system.** That is the entire soundness argument:
		 * a shared view model is safe exactly as long as nothing changes underneath it. Mutating calls
		 * always build their own context, so wrapping a mutation would not fail loudly -- it would
		 * leave the shared view model describing a system that no longer exists that way. Reads of a
		 * *different* system (the schema probe, for one) are unaffected; the sharing is per system.
		 */
		class FReadScope
		{
		public:
			explicit FReadScope(UNiagaraSystem* InSystem);
			~FReadScope();

			FReadScope(const FReadScope&) = delete;
			FReadScope& operator=(const FReadScope&) = delete;

		private:
			UNiagaraSystem* System = nullptr;
			bool bOwns = false;
		};

		/**
		 * Shares one edit context across a burst of *writes* to the same system, in structural epochs.
		 *
		 * FReadScope above cannot be reused for this, and its comment says why: a shared view model is
		 * sound only while nothing changes underneath it. But that argument is about *structure*, not
		 * about change in general. Writing a value into an input that already exists does not move any
		 * stack entry -- the view model still describes this system correctly -- so the warning does
		 * not apply to it, and value writes are where the cost is.
		 *
		 * So the scope divides an ApplyPlan into epochs. Adding a module, adding an emitter, adding a
		 * renderer, rebinding a script version, flipping a static switch: each changes which entries
		 * exist, ends the epoch, and the next call builds a fresh context. Every value write in
		 * between shares one. That takes the cost from O(inputs x system size) to
		 * O(structural operations x system size), and there are far fewer modules than inputs.
		 *
		 * Refreshing in place instead was measured and rejected: FNiagaraSystemViewModel::RefreshAll
		 * is the only exported refresh, and it calls CompileSystem -- so refreshing at every boundary
		 * would compile the system once per module. ResetStack routes through RefreshAll too. Dropping
		 * the context and letting the next call rebuild is both cheaper and simpler to reason about.
		 *
		 * The two scopes must not overlap on one system: TNiagaraViewModelManager refuses to register
		 * a second live view model for the same system, so the check below is an assert rather than a
		 * convention.
		 */
		class FWriteScope
		{
		public:
			explicit FWriteScope(UNiagaraSystem* InSystem);
			~FWriteScope();

			FWriteScope(const FWriteScope&) = delete;
			FWriteScope& operator=(const FWriteScope&) = delete;

		private:
			UNiagaraSystem* System = nullptr;
			bool bOwns = false;
		};

		/**
		 * Ends the current structural epoch on this system.
		 *
		 * Every mutator in this file that changes the shape of the stack calls it already. It is
		 * public for the one case the adapter cannot see on its own: writing a static switch changes
		 * which *other* inputs are visible, and only the caller knows from the module schema that a
		 * given input is one. Harmless outside a write scope.
		 */
		static void EndStructuralEpoch(UNiagaraSystem* System);

		/**
		 * Turns the write scope off, so every write builds its own context as it did before P1.
		 *
		 * It exists to be measured against. A before-and-after taken from two different binaries also
		 * varies by whatever else changed between them; this way the comparison is one build, one
		 * machine, one asset, one flag. `dfx.ps1 build -NoWriteScope` sets it.
		 */
		static void SetWriteScopeEnabled(bool bEnabled);

		/** Zeroes the counters reported by ReportStats. */
		static void ResetStats();

		/**
		 * One line naming what the run cost: contexts built (each is a whole system view model),
		 * structural versus value calls, and epoch boundaries crossed.
		 *
		 * Contexts-built is the number that matters. Before the write scope it tracked the call count;
		 * with it, it should track the module count.
		 */
		static FString ReportStats();

		/**
		 * Collects garbage when the process has grown past a threshold, and otherwise does nothing.
		 *
		 * Every call through the external edit API builds a stack view model out of UObjects, and in a
		 * commandlet nothing collects them. The decompiler hit this first -- 120 MB/s, 38 GB on one
		 * asset -- and R1b brought the same shape to the generate path: rebinding a module's script
		 * version marks its graph for resynchronisation, so every following call rebuilds more of the
		 * view model than it otherwise would. Measured on the four content packs before this call
		 * existed: 5 GB per minute, still climbing at 30 GB.
		 *
		 * A memory threshold rather than a call counter is what keeps it free on the small systems
		 * that never approach it: those finish before the first check ever trips.
		 */
		static void CollectIfHeavy();

		/**
		 * Regenerates the sample tables of every curve data interface the system owns.
		 *
		 * A curve is written the way every data interface is written -- a JSON blob through
		 * SetStackInputData -- which sets the curve's keys and nothing else. But a curve DI is
		 * evaluated from `ShaderLUT`, a table baked from those keys, and nothing in that write path
		 * bakes it: the editor normally regenerates it from PostEditChangeProperty, which a
		 * programmatic write never raises.
		 *
		 * The symptom is a log line rather than an error -- `CopyToInternal` regenerates the copy's
		 * table, compares it against the source's, and says "Post CopyToInternal LUT generation is out
		 * of sync. Please investigate." -- and it appeared only on DreamFX-built mirrors, never on the
		 * hand-authored assets they were read from. Exactly the shape of bug an L1/L2 gate cannot see:
		 * the asset builds, compiles and diffs clean, and the curve reads from a stale table.
		 *
		 * Called once after a system's writes are finished rather than per input, because the tables
		 * only have to be right before anything reads them, and one sweep is cheaper than finding the
		 * object behind every curve-valued input (which the external edit API has no read path for).
		 */
		static void RefreshCurveLookupTables(UNiagaraSystem* System);

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

		/**
		 * Adds a copy of an existing emitter asset.
		 *
		 * The same call AddEmitter makes, with the caller's asset as the template instead of a blank
		 * one. plan-v3 E2 uses it to give the decompiler a system to read a standalone
		 * `UNiagaraEmitter` through -- there is no read path that takes a bare emitter, and the copy
		 * lands in a transient system that is thrown away immediately after.
		 */
		static bool AddEmitterFromTemplate(UNiagaraSystem* System, UNiagaraEmitter* Template,
			FName EmitterName, TArray<FString>& OutErrors);
		static bool RemoveEmitter(const FStackAddress& EmitterAddress, TArray<FString>& OutErrors);

		/**
		 * Renames an emitter in place, keeping its handle.
		 *
		 * R4: an emitter's name is a stable key, not a display name -- Niagara stores literal module
		 * inputs under an emitter-prefixed rapid-iteration alias. Renaming in source and rebuilding
		 * would drop the old emitter and add a new one, taking its handle GUID with it. Renaming the
		 * asset *first* means the following rebuild matches by the new name and reuses the handle.
		 *
		 * FNiagaraEmitterHandle::SetName does the alias rewrite itself, which is why this is a
		 * two-line function and not the rapid-iteration surgery plan 2.5 anticipated.
		 */
		static bool RenameEmitter(UNiagaraSystem* System, FName OldName, FName NewName, TArray<FString>& OutErrors);

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

		/**
		 * Gives a renderer the engine's stock material when it has none.
		 *
		 * `UNiagaraSpriteRendererProperties`'s CDO has a null Material, and AddRenderer builds from the
		 * CDO -- so a generated sprite renderer draws absolutely nothing, with no error anywhere. The
		 * Niagara editor does not hit this because it assigns the default itself when the user adds a
		 * renderer (NiagaraSystemViewModel.cpp:646 hardcodes the same path). DreamFX matches that
		 * behaviour rather than leaving an invisible effect behind.
		 *
		 * Reports what it applied through OutAppliedMaterial so the build can say so out loud; an
		 * empty result with a Material property still null means nothing was known to apply.
		 */
		static bool EnsureRendererMaterial(const FStackAddress& RendererAddress, FString& OutAppliedMaterial,
			bool& bOutStillMissing, TArray<FString>& OutErrors);

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
		 * One level of a dynamic-input chain: each direct input's name, type, editability and whether
		 * it is a static switch.
		 *
		 * Editability matters to the decompiler as much as to the generator. An input whose
		 * EditCondition is false is refused by SetStackInputData, so exporting it produces a file that
		 * does not rebuild.
		 */
		static bool GetDynamicInputChildren(const FStackAddress& InputAddress,
			TArray<FDynamicInputChild>& OutChildren, TArray<FString>& OutErrors);

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
		/**
		 * @param bIncludingGpuShaders  also block until the compute shaders are built.
		 *
		 * Only a system with a GPU emitter needs the second wait, and it is not free -- so it is asked
		 * for rather than assumed. Without it a GPU emitter's compile state is read while its compute
		 * shader is still building, and the build reports success on a system that has not finished
		 * compiling the half most likely to fail.
		 */
		static bool CompileAndWait(UNiagaraSystem* System, bool bIncludingGpuShaders,
			FCompileStateInfo& OutState, TArray<FString>& OutErrors);

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
		 * R7. The version a module or dynamic input asset currently exposes.
		 *
		 * Read from UNiagaraScript, not from the external edit API: that API has no notion of versions
		 * at all -- AddModule takes a bare asset pointer, no topology struct reports which version a
		 * stack entry uses, and nothing in it selects one. So a version can be recorded and compared,
		 * which is what detects the drift R7 is about, but not chosen. The plan-v2 W3 probe conclusion
		 * is written up in Plan/plan-v2.md.
		 *
		 * UNiagaraScript::GetExposedVersion and IsVersioningEnabled are NIAGARA_API on the runtime type,
		 * so this stays inside the portability boundary.
		 */
		static FScriptVersion GetScriptVersion(const UNiagaraScript* Asset);

		// --- script version selection (plan-v5 R1b) ------------------------------------------
		//
		// The one place DreamFX steps outside the external edit API, and it is not optional.
		//
		// `UNiagaraExternalEditUtilities::AddModule` pins every module it adds to the asset's newest
		// version: `Args.VersionGuid = ModuleAsset->GetLatestScriptData()->Version.VersionGuid;
		// //TODO: allow old versions?`. Real content does not sit on the newest version -- a module
		// revision renames inputs (`StartFrame` became `StartFrameOffset`) and changes their types
		// (`Write Parameter Index 0` went from a bool to a six-entry enum), and content authored
		// before the revision keeps the old signature forever. Measured on the four content packs:
		// 895 of 1229 rebuild failures were one module, at one version, on each side.
		//
		// So a rebuild that can only ever add the newest version cannot reproduce the asset it was
		// exported from -- not approximately, but structurally, and silently, because the newest
		// version accepts a *different* set of inputs rather than failing.
		//
		// The three calls below read and set the version through UNiagaraNodeFunctionCall, whose
		// SelectedScriptVersion is a public UPROPERTY and whose ChangeScriptVersion carries
		// NIAGARAEDITOR_API -- both in NiagaraEditor's Public folder, which this module already
		// depends on. No engine source is modified, which is what principle 4 actually requires; the
		// audited-surface rule in this file's header gets these three entries added to it, with the
		// justification above standing in for the upstream comparison.

		/** Every version a script asset offers, in the order the asset lists them. */
		static TArray<FScriptVersion> GetAvailableScriptVersions(const UNiagaraScript* Asset);

		/**
		 * The version a module already in a stack is bound to.
		 *
		 * Not the asset's exposed version: that is what a *new* module would get. This is what the
		 * one being read is actually compiled against, and therefore what its input list means.
		 */
		static bool GetModuleScriptVersion(const FStackAddress& ModuleAddress, FScriptVersion& OutVersion,
			TArray<FString>& OutErrors);

		/**
		 * Rebinds a module already in a stack to a different version of its own script.
		 *
		 * Runs the engine's own version-change path, with the Python upgrade scripts skipped: those
		 * exist to migrate a user's authored values forward, and DreamFX is about to write every
		 * input from source anyway. Override pins that the target version does not have are dropped
		 * by the engine, which is what makes the following topology read describe the right module.
		 */
		static bool SetModuleScriptVersion(const FStackAddress& ModuleAddress, const FGuid& VersionGuid,
			TArray<FString>& OutErrors);

		/**
		 * Writes a dynamic input into an input and binds the node it creates to a script version.
		 *
		 * Dynamic inputs are versioned exactly as modules are, and the revisions are just as
		 * disruptive: `MakeFloatFromLinearColor`'s `Channel` moved from one enum asset to a different
		 * one, so an authored `Channel = R` and a freshly created call's `Channel = Red` are not the
		 * same input taking different spellings -- they are different types.
		 *
		 * The node is identified by what appeared: the graph's function-call nodes are counted before
		 * the write and after, and the one that is new is the one this call made. That is exact even
		 * when the same dynamic input already appears elsewhere in the graph, which name matching and
		 * script matching are not.
		 */
		static bool SetDynamicInputAtVersion(const FStackAddress& InputAddress, UNiagaraScript* DynamicInput,
			const FGuid& VersionGuid, TArray<FString>& OutErrors);

		/**
		 * The version the calls to a given dynamic input in this emitter's graph are bound to.
		 *
		 * Fails when they disagree rather than picking one. There is no read path from a chain address
		 * to its node -- the external edit API reports a chain as values, not as graph nodes, and the
		 * override-pin lookup that would resolve it is not exported -- so the answer is "what every
		 * call to this script in this graph is using", and it is only an answer while that is one
		 * thing. In authored content it is: the calls were all made at the same time.
		 */
		static bool GetDynamicInputScriptVersion(const FStackAddress& EmitterAddress,
			const UNiagaraScript* DynamicInput, FScriptVersion& OutVersion, TArray<FString>& OutErrors);

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

		/**
		 * For an array-of-struct property such as `Meshes` or `OverrideMaterials`, the one field inside
		 * each element that holds an asset reference, plus that element type's default JSON.
		 *
		 * plan-v3 E4-3. Found by reflection rather than by a per-property table: `Meshes` elements are
		 * FNiagaraMeshRendererMeshProperties and `OverrideMaterials` elements are
		 * FNiagaraMeshMaterialOverride, and each happens to carry exactly one object property -- but a
		 * hard-coded pair of names would be wrong the first time a renderer adds a third such array.
		 * Fails when the element carries zero or several object fields, because then "the reference"
		 * is not a well-defined thing and guessing one would write the wrong field.
		 *
		 * @param JsonPropertyName  the JSON spelling, i.e. the UPROPERTY name with a lowercase initial
		 * @param OutReferenceField the JSON spelling of the reference field, e.g. "mesh"
		 * @param OutElementDefaults a default-constructed element, serialised, for gap detection
		 */
		static bool GetArrayElementReferenceField(const UClass* RendererClass, const FString& JsonPropertyName,
			FString& OutReferenceField, FString& OutElementDefaultsJson, TArray<FString>& OutErrors);
	};
}
