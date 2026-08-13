#pragma once

#include "CoreMinimal.h"
#include "Adapter/DreamFXNiagaraAdapter.h"

class UNiagaraScript;

namespace UE::DreamFX::Editor
{
	/**
	 * Resolves the module and dynamic-input names written in source to actual `UNiagaraScript` assets,
	 * and caches their schemas.
	 *
	 * R6 stands: signatures come from .uasset, not C++ reflection, so name resolution needs a loaded
	 * asset registry. That is the same constraint DreamShader lives with for textures and material
	 * functions, and the cache keeps the cost to one lookup per distinct module per build.
	 */
	class FModuleLibrary
	{
	public:
		FModuleLibrary();

		/**
		 * Releases the probe system's GC root.
		 *
		 * The probe is rooted for as long as the library lives, because a long read collects garbage
		 * as it goes (see FDecompiler) and `ProbeSystem` is a bare member of a plain class -- nothing
		 * would keep it reachable, and nothing would null it either. It would simply dangle.
		 */
		~FModuleLibrary();

		/** `Settings.ModulePaths` from the document, appended to the built-in defaults. */
		void SetSearchPaths(const TArray<FString>& InSearchPaths);

		/** Adds the engine's own module directories. Called by the constructor. */
		void AddDefaultSearchPaths();

		/**
		 * Finds a module script by the name written in source.
		 *
		 * Accepts a bare short name (`GravityForce`), a partial path (`Update/Forces/GravityForce`) or
		 * a full content path (`/Niagara/Modules/Update/Forces/GravityForce`). A short name matching
		 * more than one asset is an error listing every candidate, because silently picking one would
		 * make the build depend on asset registry ordering.
		 */
		UNiagaraScript* FindModule(const FString& Name, FString& OutError);

		/** Same lookup, restricted to assets whose usage is DynamicInput. */
		UNiagaraScript* FindDynamicInput(const FString& Name, FString& OutError);

		/**
		 * The input signature a module actually exposes once it sits in the given stack.
		 *
		 * This -- not GetModuleSchema -- is what the generator type-checks against, for two reasons.
		 * First, the asset-level schema misses inline edit conditions: it reports "Write Lifetime" as an
		 * ordinary input, but that name has no addressable stack input and writing it fails. Second, it
		 * misses static switches entirely (R5), so enum-shaped inputs like Loop Behavior are invisible.
		 * Both are only visible on a live module, so a throwaway probe system is built, each module is
		 * added to it once, its topology is read, and it is removed again. The real asset is never
		 * involved, which keeps the plan-before-mutate ordering of 4.5 intact.
		 */
		const FModuleSchema* GetStackSchema(UNiagaraScript* Module, EStackKind Stack, FString& OutError);

		/**
		 * The same signature, read after the given static switches have been set on the probe.
		 *
		 * plan-v5 R1, and the single largest read/write asymmetry the four content packs exposed: 824
		 * of 1316 rebuild failures were one `DFX3003` shape. A static switch reshapes which inputs a
		 * module has at all -- `SpriteSizeMode = Uniform` is what makes `UniformSpriteSize` exist --
		 * so the decompiler, reading an authored module, reports inputs that a freshly added one does
		 * not have. Type-checking the export against the default configuration therefore rejected an
		 * input the author really did set, on a module that really does expose it.
		 *
		 * Setting the switches on the probe first is what makes both sides read the same module. The
		 * cost is a recompile of the probe emitter per distinct configuration, which is why the cache
		 * key is the module, the stack *and* the switch assignments: the second call for the same
		 * configuration -- the common case, since a pack reuses a handful of them -- is free.
		 *
		 * @param SwitchValues  in the order they must be applied. Order is the caller's business: a
		 *                      switch can be gated by another switch, and writing the inner one first
		 *                      is refused by SetStackInputData.
		 * @param VersionGuid   which version of the module script to read. Invalid means "whichever one
		 *                      AddModule picks", i.e. the newest -- correct only for an unversioned
		 *                      asset or a source that pinned nothing (R1b).
		 */
		const FModuleSchema* GetStackSchema(UNiagaraScript* Module, EStackKind Stack,
			TArrayView<const TPair<FName, FInputValue>> SwitchValues, const FGuid& VersionGuid,
			FString& OutError);

		/** The switch-aware probe at whatever version AddModule would pick. */
		const FModuleSchema* GetStackSchema(UNiagaraScript* Module, EStackKind Stack,
			TArrayView<const TPair<FName, FInputValue>> SwitchValues, FString& OutError);

		/**
		 * The input values a freshly added module has, before anything overrides them.
		 *
		 * R8's answer. The read API reports resolved values with no "explicitly set" flag, and the
		 * plan's proposed fix -- comparing against `GetStackInputSchema.DefaultValue` -- rests on a
		 * field that does not exist. The probe already adds each module in isolation, so reading its
		 * values at that moment gives the baseline directly. "Explicitly set to the default" is
		 * indistinguishable from "not set" and is accepted as lost.
		 */
		const TMap<FName, FInputValue>* GetStackDefaults(UNiagaraScript* Module, EStackKind Stack, FString& OutError);

		/**
		 * The same baseline, taken from the version the module being read is actually bound to (R1b).
		 *
		 * "Differs from a pristine instance" is only a meaningful test when the pristine instance is
		 * the same module: at a different version an input may not exist, or may exist with another
		 * type, and the comparison then either hides a value that was set or prints one that was not.
		 */
		const TMap<FName, FInputValue>* GetStackDefaults(UNiagaraScript* Module, EStackKind Stack,
			const FGuid& VersionGuid, FString& OutError);

		/**
		 * The baseline a module has once the given static switches are set on it.
		 *
		 * An input's default can depend on a switch: EmitterState's LoopDuration reads 1.0 on a
		 * pristine module and 5.0 once LoopBehavior is Once. Judging such an input against the
		 * pristine baseline suppresses the authored 1.0 as "same as default", and the rebuild -- which
		 * does set LoopBehavior -- then produces 5.0. That was 9 of the 10 remaining L1 mismatches.
		 *
		 * ONLY for non-switch inputs. A switch compared against a baseline that already has that
		 * switch applied always matches, so it would suppress itself, the export would lose it, and
		 * the rebuild's gated inputs would cease to exist -- which is exactly the "no input named
		 * 'bUseMinDistance'" wreck a previous attempt at this produced. Switches keep the pristine
		 * baseline; the caller is responsible for that split.
		 */
		const TMap<FName, FInputValue>* GetStackDefaultsForSwitches(UNiagaraScript* Module, EStackKind Stack,
			TArrayView<const TPair<FName, FInputValue>> SwitchValues, const FGuid& VersionGuid, FString& OutError);

		/** Property JSON a renderer class has when freshly added, for the same default-suppression job. */
		const FString* GetRendererDefaults(UClass* RendererClass, FString& OutError);

		/** Attribute bindings a renderer class has when freshly added, so defaults are not exported. */
		bool GetRendererBindingDefaults(UClass* RendererClass, TArray<TPair<FString, FName>>& OutBindings,
			FString& OutError);

		/** Property JSON a freshly created emitter has. */
		const FString* GetEmitterDefaults(FString& OutError);

		/** Cached schema read from the module asset alone. Coarser than GetStackSchema; see above. */
		const FModuleSchema* GetModuleSchema(const UNiagaraScript* Module, FString& OutError);

		/** Cached schema for a dynamic input asset. Coarser than GetDynamicInputStackSchema; see below. */
		const FModuleSchema* GetDynamicInputSchema(const UNiagaraScript* DynamicInput, FString& OutError);

		/**
		 * The input signature a dynamic input actually exposes once it is plugged into a stack.
		 *
		 * The dynamic-input answer to GetStackSchema, and for the same reason: the asset-level schema
		 * carries no static-switch flag at all, so an input like `Absolute` on a range dynamic input is
		 * invisible to it. A coverage sweep found that gap on 7 of 20 systems -- the single largest
		 * remaining one in this project's own content (plan-v3 E4-1).
		 *
		 * Probed the same way modules are: a Set Parameters entry of the host type is created in the
		 * throwaway system, the dynamic input is plugged into it, the live chain is read back, and the
		 * entry is removed. Falls back to the asset schema if any of that fails, which restores exactly
		 * the previous behaviour rather than failing the build.
		 *
		 * @param HostType  the type of the input the dynamic input is being plugged into. Comes from
		 *                  the caller because `FNiagaraExt_ModuleSchema::Outputs` is empty for a
		 *                  dynamic input asset -- the external edit API reports no output type at all,
		 *                  so there is nothing on the asset to derive a host from.
		 */
		const FModuleSchema* GetDynamicInputStackSchema(UNiagaraScript* DynamicInput,
			const FNiagaraTypeDefinition& HostType, FString& OutError);

		/** The same probe, with the chain node bound to a specific script version first (R1b). */
		const FModuleSchema* GetDynamicInputStackSchema(UNiagaraScript* DynamicInput,
			const FNiagaraTypeDefinition& HostType, const FGuid& VersionGuid, FString& OutError);

		/** Every search path currently in effect, for error messages. */
		const TArray<FString>& GetSearchPaths() const { return SearchPaths; }

		/**
		 * The shortest name that resolves back to this exact asset.
		 *
		 * The decompiler cannot just print the short name: `InitializeParticle` exists twice and a
		 * re-import of that export fails as ambiguous. Suffixes are added a segment at a time until
		 * the lookup lands on the same asset, falling back to the full path.
		 */
		FString GetUnambiguousName(UNiagaraScript* Script, bool bDynamicInput);

		/**
		 * The same search, reporting failure instead of guessing.
		 *
		 * GetUnambiguousName falls back to the full package path, which reads like an answer and is
		 * one for every script that lives in a package of its own. A scratch pad script does not: it
		 * lives *inside* a system or emitter asset, so its package path names the system, and an
		 * export that wrote it produced a file whose rebuild looked for a module at a path where
		 * there is a Niagara system. That was the four packs' one silent expression error -- 35 of
		 * them -- and it only ever surfaced as a rebuild failure (plan-v5 R3).
		 *
		 * @return the shortest resolving name, or empty when the script cannot be addressed at all.
		 */
		FString FindAddressableName(UNiagaraScript* Script, bool bDynamicInput);

		/**
		 * Copies a script that lives inside another asset out into a package of its own.
		 *
		 * Route A of R3, and the reason it is the default: a scratch pad is an ordinary
		 * `UNiagaraScript` that happens to be stored inside its owner, so lifting it out costs no
		 * language surface and the result rebuilds on any engine. The copy is stamped with where it
		 * came from, and an existing copy at the same path is reused rather than duplicated again, so
		 * re-exporting a system does not fill the tree with numbered scripts.
		 *
		 * This is the only part of decompilation that writes, which is why it is a decompile *option*
		 * rather than automatic: `coverage` counts without materialising.
		 */
		UNiagaraScript* MaterializeEmbeddedScript(UNiagaraScript* Script, const FString& PackagePath,
			const FString& AssetName, FString& OutError);

		/** One entry of the available-module listing, with the script already resolved. */
		struct FModuleListing
		{
			FString AssetName;
			FString PackageName;
			UNiagaraScript* Script = nullptr;
		};

		/**
		 * Every module (or dynamic input) the search paths expose.
		 *
		 * Structured rather than formatted because the index export needs the script itself: it has
		 * already been loaded to check the usage, and loading it a second time from a re-split string
		 * would be both slower and a chance to disagree about which asset a name meant.
		 */
		void ListAvailableDetailed(bool bDynamicInput, TArray<FModuleListing>& OutEntries);

		/**
		 * The same listing as "Name -> /Package/Path" text. Drives the commandlet's -List mode, which
		 * is how an author discovers what is available without opening the content browser.
		 */
		void ListAvailable(bool bDynamicInput, TArray<FString>& OutEntries);

	private:
		UNiagaraScript* FindScript(const FString& Name, bool bDynamicInput, FString& OutError);

		/**
		 * Walks the search paths on disk and indexes every .uasset by its short name.
		 *
		 * Deliberately not the asset registry: in a commandlet the registry silently reports a partial
		 * index -- /Niagara/Functions shows up while /Niagara/Modules does not -- and a module lookup
		 * that misses is indistinguishable from a typo in the source. A directory walk is a few
		 * milliseconds, needs no scan priming, and behaves identically in the editor and headless.
		 */
		void BuildIndex();

		/** Lazily creates the transient system + emitter that GetStackSchema probes against. */
		bool EnsureProbeSystem(FString& OutError);

		/**
		 * Every cache whose getter hands out a pointer into it stores the payload indirectly.
		 *
		 * A `TMap` moves its elements when it rehashes, so a pointer returned by an earlier lookup
		 * dangles the moment a later insertion grows the map. That is not theoretical: the decompiler
		 * holds a dynamic input's schema across the loop that walks its children, and a nested chain
		 * caches its own schema inside that loop -- decompiling a third-party system with nested
		 * dynamic inputs crashed on the next child, reading address 0. Boxing the values keeps their
		 * addresses fixed for the library's lifetime, which is what every caller already assumes.
		 *
		 * RendererBindingDefaultsCache is exempt: its getter copies out, so nothing outlives the call.
		 */
		TArray<FString> SearchPaths;
		bool bIndexBuilt = false;
		/** Lowercased short name -> every package path that declares it. */
		TMap<FString, TArray<FString>> PackagesByName;
		TMap<FString, TWeakObjectPtr<UNiagaraScript>> ModuleCache;
		TMap<FString, TWeakObjectPtr<UNiagaraScript>> DynamicInputCache;
		TMap<TWeakObjectPtr<const UNiagaraScript>, TUniquePtr<FModuleSchema>> SchemaCache;

		TObjectPtr<UNiagaraSystem> ProbeSystem;
		bool bProbeSystemFailed = false;

		/**
		 * Keyed on the module, the stack and the static switch assignments, because all three change
		 * the answer (R1). A plain module+stack key was what made every switch-revealed input look
		 * like a typo.
		 */
		TMap<FString, TUniquePtr<FModuleSchema>> StackSchemaCache;
		/** Same key shape as StackSchemaCache, minus the switches: defaults are read unconfigured. */
		TMap<FString, TUniquePtr<TMap<FName, FInputValue>>> StackDefaultsCache;
		TMap<TWeakObjectPtr<const UClass>, TUniquePtr<FString>> RendererDefaultsCache;
		TMap<TWeakObjectPtr<const UClass>, TArray<TPair<FString, FName>>> RendererBindingDefaultsCache;
		TOptional<FString> EmitterDefaults;

		/** Separate from SchemaCache: same asset, different (richer) answer, and keyed by version too. */
		TMap<FString, TUniquePtr<FModuleSchema>> DynamicInputStackSchemaCache;
	};
}
