#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::DreamFX::Editor
{
	/**
	 * The provenance stamp described in plan 4.6-1: enough information on a generated asset to answer
	 * "where did this come from, and is it still in sync?".
	 */
	struct FProvenanceStamp
	{
		/** Source path relative to its DFX root, e.g. "Systems/NS_ToonHitSpark.dfs". */
		FString SourceRelativePath;
		/** Absolute source path, for diagnostics and for opening the file from the editor guardrail. */
		FString SourceFullPath;
		/** Hash of the source text at generation time. Equal hash means the rebuild can be skipped. */
		FString SourceHash;
		/** Generator version. Bumping it forces every asset to regenerate on the next build. */
		FString GeneratorVersion;
		/** Package paths of every module asset used, so R7 drift can be reported. */
		TArray<FString> ModuleDependencies;

		bool IsValid() const { return !SourceHash.IsEmpty(); }
	};

	/**
	 * Stores the stamp as package metadata.
	 *
	 * UNiagaraSystem does not implement IInterface_AssetUserData, so the AssetUserData route the plan
	 * assumed is not available on a stock engine. Package metadata is the closest equivalent that
	 * needs no engine change: it is per-object, arbitrary key/value, editor-only, and travels with the
	 * asset through save, copy and rename.
	 */
	class FProvenance
	{
	public:
		/** Bump when the generator's output changes in a way that must invalidate cached assets. */
		static const TCHAR* GetGeneratorVersion();

		static void Write(UObject* Asset, const FProvenanceStamp& Stamp);
		static bool Read(const UObject* Asset, FProvenanceStamp& OutStamp);
		static void Clear(UObject* Asset);

		/** True when the asset was generated from this source and the source has not changed since. */
		static bool IsUpToDate(const UObject* Asset, const FString& SourceHash);
	};
}
