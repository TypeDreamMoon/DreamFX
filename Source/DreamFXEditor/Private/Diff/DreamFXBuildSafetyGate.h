#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"

class UNiagaraSystem;

namespace UE::DreamFX::Editor
{
	/**
	 * What one asset held when a build opened it.
	 *
	 * Held as TEXT rather than as a system pointer on purpose: the build mutates the very object these
	 * facts describe, so a snapshot that read through to the asset later would answer with the
	 * rebuild's own state and find nothing missing -- which is exactly how the loss this exists to
	 * catch stayed silent for so long.
	 */
	struct FBuildSafetySnapshot
	{
		TArray<FString> Facts;

		/**
		 * False when there was no asset to read. A first generation has nothing to lose: the gate is
		 * about a rebuild REPLACING content the source cannot express, and the first build of a path
		 * replaces nothing. Refusing it would only stop the one case that is safe by construction.
		 */
		bool bCaptured = false;
	};

	/**
	 * The gate between a rebuild and the facts it would drop (write-back ①; write-back-coverage.md 6.1).
	 *
	 * `FGenerator` builds by replaying text onto an asset, and text is not a complete description of a
	 * Niagara system. A module input the decompiler suppressed (R8: only inputs that differ from a
	 * pristine module are printed), a renderer binding a commandlet patched in after the last build, a
	 * property no setting table carries -- none of them are in the source, and a rebuild silently
	 * replaces whatever they held. Three measured cases: 41 stored module-input constants on one
	 * asset, `MeshYaw` -90 -> 0 on another, 61 on a third -- every one of them with L1 and L2 green,
	 * because both sides of an L1 comparison are the same lossy exporter's output.
	 *
	 * So this reads the asset by reflection before the build touches it, reads it again when the build
	 * is about to be written, and refuses the save when the second set is missing anything the first
	 * had. No text is consulted anywhere: a loss the exporter makes on both sides is precisely what a
	 * text-level gate cannot see.
	 */
	class FBuildSafetyGate
	{
	public:
		/** Reads the asset's facts. Call before the build's first write to it. */
		static FBuildSafetySnapshot Capture(UNiagaraSystem* System);

		/**
		 * Compares the snapshot against the asset as it stands now -- after the build, after the
		 * compile, immediately before the save -- and reports every fact that is about to be lost.
		 *
		 * @param bForceFromCommandLine  `-Force`: report the losses and write anyway, instead of
		 *                               refusing. Overriding a refusal is allowed because the losses
		 *                               are still logged at warning severity with the same detail, so
		 *                               the run is accountable afterwards either way.
		 * @return true when the caller may save. False means nothing was written, so the asset still
		 *         holds every fact the diagnostics name.
		 */
		static bool CheckBeforeSave(UNiagaraSystem* System, const FBuildSafetySnapshot& Before,
			bool bForceFromCommandLine, const FString& AssetPath, const FSourceLocation& Location,
			FDiagnosticSink& Diagnostics);
	};
}
