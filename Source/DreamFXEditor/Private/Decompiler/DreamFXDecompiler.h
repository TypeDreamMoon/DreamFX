#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"

class UNiagaraEmitter;
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

	struct FDecompileOptions
	{
		/**
		 * Write `Name=` into the `Decompiled/` namespace instead of naming the asset that was read
		 * (plan-v4 V1-1).
		 *
		 * This is the difference between *Export* and *Adopt*, and it is now structural rather than
		 * procedural. Export produces source that rebuilds a mirror beside the original, so the
		 * original cannot be reached however the file is edited; Adopt means "this text is now the
		 * asset's source of truth" and keeps naming the asset.
		 */
		bool bDecompiledNamespace = false;

		/**
		 * Lift scripts that live inside the asset out into packages of their own, so the export can
		 * name them (plan-v5 R3, route A).
		 *
		 * Off by default because it is the one part of reading that writes. `coverage` counts what an
		 * export would lose and must leave the tree exactly as it found it; Export and Adopt produce a
		 * file that has to rebuild, and a scratch pad script that was never lifted out cannot.
		 */
		bool bMaterializeEmbeddedScripts = false;
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
			FDiagnosticSink& Diagnostics, const FDecompileOptions& Options = FDecompileOptions());

		/**
		 * A standalone `UNiagaraEmitter` asset to a .dfe document (plan-v3 E2).
		 *
		 * There is no read path in the external edit API that takes a bare emitter -- every reader
		 * addresses through an owning system -- so the emitter is copied into a transient system and
		 * read from there. The copy is what makes this honest rather than a guess: what comes back is
		 * exactly what the emitter contributes when it is used.
		 *
		 * @param AssetPath  the `Name="..."` to write, relative to RootToken
		 */
		static FDecompileResult DecompileEmitter(UNiagaraEmitter* Emitter, const FString& RootToken,
			FDiagnosticSink& Diagnostics, const FDecompileOptions& Options = FDecompileOptions());
	};
}
