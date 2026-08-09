#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"

class UNiagaraSystem;

namespace UE::DreamFX::Editor
{
	struct FGenerateOptions
	{
		/** Save the package after a successful build. */
		bool bSave = true;

		/** Rebuild even when the provenance hash says the asset is already current. */
		bool bForce = false;

		/**
		 * Plan-doc 4.6-2. Reports what would happen and whether the asset has drifted, but writes
		 * nothing -- neither the asset nor the provenance stamp.
		 */
		bool bVerifyOnly = false;

		/**
		 * R7. A module whose exposed version moved since the asset was built is a warning by default
		 * and an error here.
		 *
		 * Warning is the right default because the usual cause is an engine upgrade, which every
		 * source in the tree hits at once and which a rebuild resolves; failing the gate on it would
		 * stop work for something a build fixes. Error is the right behaviour for a release branch,
		 * where "these assets were built against different modules than the text describes" is exactly
		 * what must not ship.
		 */
		bool bStrictVersions = false;

		/**
		 * Pipeline mode: issue the Niagara compile and return without waiting for it. The result
		 * carries a pending handle the caller must pass to FGenerator::Finalize -- that is where the
		 * compile diagnostics, the provenance stamp and the save happen. Everything before the
		 * compile behaves exactly as the synchronous path, and a build that fails before the compile
		 * returns no handle.
		 */
		bool bDeferCompile = false;
	};

	/** A deferred build between its compile request and its finalize. Opaque outside the generator. */
	struct FPendingBuild;

	struct FGenerateResult
	{
		bool bSucceeded = false;
		/** True when the provenance hash matched and no work was done. */
		bool bSkipped = false;
		/** True when -verify found the asset out of step with its source. */
		bool bDrifted = false;

		UNiagaraSystem* System = nullptr;
		FString AssetPath;

		/** Set when Options.bDeferCompile reached the compile request; consumed by FGenerator::Finalize. */
		TSharedPtr<FPendingBuild> Pending;
	};

	/**
	 * Text to Niagara asset.
	 *
	 * Failure ordering matters (plan 4.5): everything that can fail -- parsing, module resolution,
	 * input name checks, value lowering -- runs to completion against an in-memory plan before the
	 * first mutation. DreamShader's regeneration notes call out the opposite ordering as a real
	 * defect: a syntax error after the old graph was cleared leaves the asset empty. Here a failed
	 * build leaves the previous asset untouched.
	 */
	class FGenerator
	{
	public:
		static FGenerateResult GenerateFromFile(const FString& FilePath, const FGenerateOptions& Options,
			FDiagnosticSink& Diagnostics);

		static FGenerateResult Generate(const FDocument& Document, const FGenerateOptions& Options,
			FDiagnosticSink& Diagnostics);

		/** Completes a deferred build: waits for its compile, reports, stamps and saves. */
		static bool Finalize(const TSharedPtr<FPendingBuild>& Pending, FDiagnosticSink& Diagnostics);

		/** Non-blocking: advances a deferred build's compile one step; true when Finalize would not wait. */
		static bool IsCompileComplete(const TSharedPtr<FPendingBuild>& Pending);
	};
}
