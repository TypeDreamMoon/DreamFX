#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"
#include "Generation/DreamFXGenerator.h"

class UNiagaraScript;

namespace UE::DreamFX::Editor
{
	struct FModuleGenerateResult
	{
		bool bSucceeded = false;
		/** The provenance hash matched, so nothing was rebuilt. */
		bool bSkipped = false;
		/** -Verify found the asset out of step with its source. */
		bool bDrifted = false;

		UNiagaraScript* Script = nullptr;
		FString AssetPath;
	};

	/**
	 * `.dfm` to UNiagaraScript -- plan 3.3 tier one: the whole Body becomes one UNiagaraNodeCustomHlsl.
	 *
	 * This is the plan-v2 W1 unblock. Phase 4 stopped here because nothing in NiagaraEditor's export
	 * surface accepts HLSL source; option A resolves that by exporting four declarations in MoonEngine
	 * and probing for them at build time (see DreamFXEditor.Build.cs). The probe is what keeps the
	 * engine patch an optional enhancement rather than a hard dependency: on any engine without it,
	 * DREAMFX_HAS_CUSTOMHLSL_WRITE is 0, IsAvailable() returns false, and .dfm keeps the Phase 4
	 * behaviour -- parsed, validated, DFX5100 at generation time. Generated assets are ordinary
	 * UNiagaraScripts, so a prebuilt engine still loads, references and cooks them; it just cannot
	 * make new ones.
	 *
	 * Graph shape, and why it is this small:
	 *
	 *     UNiagaraNodeInput(MapIn : ParameterMap) -> CustomHlsl "Map" in
	 *     CustomHlsl "Map" out                    -> UNiagaraNodeOutput      (module)
	 *     CustomHlsl "CustomHLSLOutput" out       -> UNiagaraNodeOutput      (dynamic input)
	 *
	 * No parameter map get/set nodes. A custom HLSL node that has a parameter map pin gets its
	 * namespaced tokens rewritten by the translator: FinalResolveNamespacedTokens turns
	 * `Particles.SpriteRotation` into `Context.Map.Particles_SpriteRotation`, which is an lvalue, so
	 * the body reads *and* writes attributes directly, and UNiagaraNodeCustomHlsl::BuildParameterMapHistory
	 * registers each one. Module inputs work the same way through the `Module.` namespace, with their
	 * defaults and tooltips carried by graph script variables. Sparing the map nodes is what keeps the
	 * engine's dependency surface at four declarations instead of pulling in two private headers.
	 */
	class FModuleGenerator
	{
	public:
		/** True when this build can write HLSL onto a Niagara custom node. */
		static bool IsAvailable();

		/**
		 * Why generation is off, phrased for DFX5100. Only meaningful when IsAvailable() is false.
		 */
		static FString DescribeUnavailability();

		static FModuleGenerateResult Generate(const FDocument& Document, const FGenerateOptions& Options,
			FDiagnosticSink& Diagnostics);

		/**
		 * The degraded path, and the reason a prebuilt engine still runs a meaningful gate: the committed
		 * module asset is checked against the source it claims to come from. Matching stamp means the
		 * build is fine and nothing needs doing here; a mismatch is DFX5107, which says to regenerate on
		 * MoonEngine rather than telling the reader to run a build that cannot work.
		 */
		static FModuleGenerateResult CheckWithoutGenerating(const FDocument& Document,
			FDiagnosticSink& Diagnostics);
	};
}
