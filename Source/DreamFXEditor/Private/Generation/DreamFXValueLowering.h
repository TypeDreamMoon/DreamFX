#pragma once

#include "CoreMinimal.h"
#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Turns an AST value into something the adapter can write, given the target input's Niagara type.
	 *
	 * The type is always known before lowering runs: for module inputs it comes from the module
	 * schema, for user parameters from the declared type. That is what lets L7 be enforced here rather
	 * than deferred to the Niagara compiler, where a silently truncated spawn count would surface as a
	 * wrong-looking effect instead of an error.
	 */
	class FValueLowering
	{
	public:
		/**
		 * @param Value              parsed value
		 * @param TargetType         the Niagara type the value must satisfy
		 * @param InputDisplayName   how the input is named in diagnostics, e.g. "GravityForce.Gravity"
		 */
		static bool Lower(const FValue& Value, const FNiagaraTypeDefinition& TargetType,
			const FString& InputDisplayName, FDiagnosticSink& Diagnostics, FInputValue& OutValue);

		/** Human-readable type name for diagnostics. */
		static FString DescribeType(const FNiagaraTypeDefinition& Type);
	};
}
