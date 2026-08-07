#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"

class UNiagaraSystem;

namespace UE::DreamFX::Editor
{
	struct FDecompileResult
	{
		bool bSucceeded = false;
		FString Source;

		/** Features present in the asset that the DSL cannot express yet, for the coverage report. */
		TArray<FString> UnsupportedFeatures;
	};

	/**
	 * Asset back to source.
	 *
	 * The migration path for graphs that already exist, and the round-trip test that keeps the
	 * generator honest. It reuses the same read API the generator writes through, so the two stay in
	 * step by construction rather than by discipline.
	 *
	 * Explicitly not promised (plan L6, section 7): original inline arithmetic comes back as an
	 * equivalent `hlsl { }` block, and a `from "..."` emitter reference comes back inlined. Both are
	 * normalisation, not loss -- but they mean text round-trip is idempotent from the second pass on,
	 * not the first.
	 */
	class FDecompiler
	{
	public:
		/**
		 * @param RootToken  the `Root="..."` to write, e.g. "Plugin.MoonToon"; asset paths under that
		 *                   root are emitted relative to it
		 */
		static FDecompileResult Decompile(UNiagaraSystem* System, const FString& RootToken,
			FDiagnosticSink& Diagnostics);
	};
}
