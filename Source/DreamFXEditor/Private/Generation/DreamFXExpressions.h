#pragma once

#include "CoreMinimal.h"
#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"

namespace UE::DreamFX::Editor
{
	/**
	 * L6: inline arithmetic and builtin calls, lowered to a single HLSL expression.
	 *
	 * The whitelist is the point of this file, not an incidental detail. DreamShader's expression
	 * backend is ~13k lines; the way DreamFX avoids re-growing it is to keep the accepted surface
	 * small and to refuse everything else loudly. Widening the list is a design decision, not a
	 * bug fix -- if a thing is not here, the answer is a `.dfm` dynamic input, which is a few
	 * hundred lines of machinery instead of thousands.
	 *
	 * Reverse direction is explicitly not promised: the decompiler emits an equivalent `hlsl { }`
	 * block, not the original arithmetic.
	 */
	class FExpressions
	{
	public:
		/** True when the value needs HLSL lowering rather than a direct value mode. */
		static bool RequiresHlslLowering(const FValue& Value);

		/** True when a call name is an L6 builtin rather than a dynamic input reference. */
		static bool IsBuiltinFunction(const FString& Name);

		/** The whitelist, for error messages. */
		static FString ListBuiltins();

		/**
		 * Renders an expression tree as one HLSL rvalue.
		 *
		 * @param TargetType  the type the expression must produce; used to pick a float/float3/... cast
		 */
		static bool Render(const FValue& Value, const FNiagaraTypeDefinition& TargetType,
			const FString& DisplayName, FDiagnosticSink& Diagnostics, FString& OutHlsl);

		/**
		 * Checks that a raw `hlsl { }` body is a single rvalue, as the input's contract demands, and
		 * returns it whitespace-collapsed.
		 */
		static bool PrepareRawBlock(const FValue& Value, const FString& DisplayName,
			FDiagnosticSink& Diagnostics, FString& OutHlsl);

		/** Serialises a `curve { }` literal into the JSON a curve data interface reads. */
		static bool RenderCurve(const FValue& Value, const FNiagaraTypeDefinition& TargetType,
			const FString& DisplayName, FDiagnosticSink& Diagnostics, FString& OutJson);
	};
}
