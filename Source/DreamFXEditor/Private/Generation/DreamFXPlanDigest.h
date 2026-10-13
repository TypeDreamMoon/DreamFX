#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * What one built stack held, in a form the next build can compare against without reading the
	 * asset back.
	 *
	 * Inputs are keyed `<module index>|<path>` with the path joined by '/', so a nested dynamic-input
	 * argument has a key of its own. The value is `<kind>:<hash>`, the kind being one letter for the
	 * value mode the input was written with: the incremental writer needs to know whether an input was
	 * a dynamic input, because replacing or trimming a chain has to reset it first, and a hash alone
	 * cannot say that.
	 */
	struct FStackDigest
	{
		/** Modules, their order, versions, instance names, enabled state and Set Parameters entries. */
		FString Structure;

		/** Every static switch write. A switch decides which other inputs exist, so it is structure too. */
		FString Switches;

		/** The node names the build landed the modules on, in stack order. */
		TArray<FString> NodeNames;

		/** The module asset names, for the node-name collision check across stacks of one graph. */
		TArray<FString> ModuleBases;

		TMap<FString, FString> Inputs;
	};

	struct FEmitterDigest
	{
		/** Parent emitter path, version and saved package hash; empty for an emitter with no parent. */
		FString Parent;
		FString Properties;
		FString SimTarget;
		FString Renderers;
		int32 RendererCount = 0;
		FString EventHandlers;
		FString SimulationStages;
		FString ParameterDefaults;

		/** The four main stacks the source declares, keyed by script name. */
		TMap<FString, FStackDigest> Stacks;

		/** Node names of event-handler and simulation-stage stacks, which are only ever rebuilt whole. */
		TMap<FString, TArray<FString>> AuxNodeNames;
	};

	/**
	 * The plan a successful build applied, stored in the asset next to its provenance stamp.
	 *
	 * It hashes the flattened plan rather than the source text: `.dfe` merges, inheritance and module
	 * versions are already resolved there, so two texts that build the same asset digest the same, and
	 * one text whose module moved underneath it does not.
	 */
	struct FPlanDigest
	{
		static constexpr int32 CurrentVersion = 1;

		int32 Version = CurrentVersion;
		FString GeneratorVersion;

		/** Which write path built the asset. The fast-edit and stock paths do not produce identical graphs. */
		FString Backend;

		/** The asset's build-safety facts right before it was saved. Anything else means it was edited since. */
		FString FactsHash;

		FString SystemProperties;
		FString UserVariables;

		/** Some data interface refers to an emitter or renderer of this system by position. */
		bool bHasSelfReferences = false;

		TMap<FString, FStackDigest> SystemStacks;
		TArray<FString> EmitterOrder;
		TMap<FString, FEmitterDigest> Emitters;

		FString ToJson() const;
		static bool FromJson(const FString& Json, FPlanDigest& OutDigest, FString& OutError);
	};

	enum class EStackAction : uint8
	{
		/** Nothing in the stack changed. */
		Keep,
		/** Only argument values changed: write those, reset what the source no longer sets. */
		Values,
		/** Clear the stack and build it again from the plan. */
		Rebuild,
		/** The source stopped declaring the stack: clear it and leave it empty. */
		Clear,
	};

	struct FStackChange
	{
		EStackAction Action = EStackAction::Keep;
		TArray<FString> Changed;
		TArray<FString> Added;
		TArray<FString> Removed;
	};

	enum class EEmitterAction : uint8
	{
		Keep,
		/** Some parts change; the rest of the emitter is left as the last build made it. */
		Partial,
		/** Clear the emitter and build it again, exactly as the full rebuild does. */
		Rebuild,
		/** New in this build. */
		Add,
	};

	struct FEmitterChange
	{
		EEmitterAction Action = EEmitterAction::Keep;
		bool bProperties = false;
		bool bRenderers = false;
		bool bEventHandlers = false;
		bool bSimulationStages = false;
		bool bParameterDefaults = false;
		TMap<FString, FStackChange> Stacks;

		bool IsWholeEmitter() const { return Action == EEmitterAction::Rebuild || Action == EEmitterAction::Add; }

		/** A stack of this emitter is cleared or rebuilt, so constants of the modules it lost are stale. */
		bool HasStructuralStackChange() const;
	};

	/** What an incremental build is allowed to touch. Anything not named here is left exactly as it is. */
	struct FIncrementalScope
	{
		const FPlanDigest* Old = nullptr;

		bool bUserVariables = false;
		bool bSystemProperties = false;
		TMap<FString, FStackChange> SystemStacks;
		TMap<FString, FEmitterChange> Emitters;
		TArray<FString> RemovedEmitters;

		const FEmitterChange* FindEmitter(FName Name) const { return Emitters.Find(Name.ToString()); }
		const FStackChange* FindSystemStack(FName ScriptName) const { return SystemStacks.Find(ScriptName.ToString()); }

		/** One line for the log: what this build is about to do. */
		FString Describe() const;
	};

	/**
	 * Works out what changed between the last build's digest and this one.
	 *
	 * Returns false when the change is one the incremental path does not reproduce exactly, with the
	 * reason in OutWhyNot; the caller then runs the full rebuild. Every refusal here is a case where
	 * a partial write could leave the asset different from a full rebuild of the same text.
	 */
	bool DecideIncrementalScope(const FPlanDigest& Old, const FPlanDigest& New, FIncrementalScope& OutScope,
		FString& OutWhyNot);

	/** Stack keys of the auxiliary node-name table: one per event handler or simulation stage stack. */
	FString MakeEventStackKey(int32 HandlerIndex, FName ScriptName);
	FString MakeStageStackKey(int32 StageIndex, FName ScriptName);
}
