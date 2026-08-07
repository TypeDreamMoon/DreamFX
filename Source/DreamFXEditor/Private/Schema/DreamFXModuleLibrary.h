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

		/** Cached schema read from the module asset alone. Coarser than GetStackSchema; see above. */
		const FModuleSchema* GetModuleSchema(const UNiagaraScript* Module, FString& OutError);

		/** Cached schema for a dynamic input asset. */
		const FModuleSchema* GetDynamicInputSchema(const UNiagaraScript* DynamicInput, FString& OutError);

		/** Every search path currently in effect, for error messages. */
		const TArray<FString>& GetSearchPaths() const { return SearchPaths; }

		/**
		 * Every module (or dynamic input) the search paths expose, as "Name -> /Package/Path".
		 * Drives the commandlet's -List mode, which is how an author discovers what is available
		 * without opening the content browser.
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

		TArray<FString> SearchPaths;
		bool bIndexBuilt = false;
		/** Lowercased short name -> every package path that declares it. */
		TMap<FString, TArray<FString>> PackagesByName;
		TMap<FString, TWeakObjectPtr<UNiagaraScript>> ModuleCache;
		TMap<FString, TWeakObjectPtr<UNiagaraScript>> DynamicInputCache;
		TMap<TWeakObjectPtr<const UNiagaraScript>, FModuleSchema> SchemaCache;

		TObjectPtr<UNiagaraSystem> ProbeSystem;
		bool bProbeSystemFailed = false;
		TMap<TPair<TWeakObjectPtr<const UNiagaraScript>, EStackKind>, FModuleSchema> StackSchemaCache;
	};
}
