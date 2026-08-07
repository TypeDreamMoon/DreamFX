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
	};

	struct FGenerateResult
	{
		bool bSucceeded = false;
		/** True when the provenance hash matched and no work was done. */
		bool bSkipped = false;
		/** True when -verify found the asset out of step with its source. */
		bool bDrifted = false;

		UNiagaraSystem* System = nullptr;
		FString AssetPath;
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
	};
}
