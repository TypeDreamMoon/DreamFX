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

		/**
		 * The DSL spelling of a type -- the inverse of ResolveDeclaredType, for the decompiler.
		 * "NiagaraFloat" is what the engine calls it; "float" is what an author writes.
		 */
		static FString DescribeDeclaredType(const FNiagaraTypeDefinition& Type);

		/**
		 * Resolves a `Properties = {}` / `Inputs = {}` type name to a Niagara type.
		 * `DI<X>` and `Texture2D` resolve to data interface types; bOutIsDataInterface reports that,
		 * because those can only be declared in v1, not given a value (plan 3.5).
		 */
		static bool ResolveDeclaredType(const FParameterDecl& Declaration, FDiagnosticSink& Diagnostics,
			FNiagaraTypeDefinition& OutType, bool& bOutIsDataInterface);

		/**
		 * Infers the Niagara type of an assignment's right-hand side, for L2's "first write declares"
		 * rule. Fails (with a diagnostic) when the value carries no type of its own -- a bare linked
		 * reference, for instance, whose type lives on the thing it points at.
		 */
		static bool InferType(const FValue& Value, const FString& TargetName, FDiagnosticSink& Diagnostics,
			FNiagaraTypeDefinition& OutType);

		/** True when the first segment of a dotted name is a Niagara parameter namespace. */
		static bool IsNamespacedName(const FString& Name);

		/**
		 * The token to write for an enum entry, guaranteed to read back as that same entry.
		 *
		 * plan-v5 R2. The decompiler used to print the entry's display label and hope: on
		 * `ENiagaraFloatFromLinearColorOptions` the label is `A` where the lookup wants `Alpha`, and
		 * 37 exports named a channel that does not exist. Rather than encode which enums spell
		 * themselves which way -- a table that is wrong the first time an artist adds an enum asset --
		 * the candidate is resolved back through the importer's own lookup, and a candidate that does
		 * not survive that round trip is rejected in favour of one that does.
		 *
		 * Returns the internal name as a last resort, which is what an author would have to write
		 * anyway; an empty string only for a null enum or an index that is not an entry.
		 */
		static FString EnumEntryToSourceToken(const UEnum* Enum, int32 Index);

		/** The same round trip, entered from the qualified FName a read hands back. */
		static FString EnumEntryToSourceToken(const UEnum* Enum, FName QualifiedName);
	};
}
