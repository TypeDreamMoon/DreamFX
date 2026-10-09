#pragma once

#include "CoreMinimal.h"

class UClass;

namespace UE::DreamFX::Editor
{
	/**
	 * Asset property values as the language spells them -- one implementation for both directions of
	 * the round trip.
	 *
	 * The decompiler writes a renderer's changed properties out, and pull writes an asset's values back
	 * into properties the text already has. Both need the same answer to "what is this JSON value in
	 * source?", and two copies of that answer is exactly the failure this file exists to prevent: a
	 * value the reader spells one way and the writer another is a source file that changes every time
	 * it is touched. So the spelling rules live here and the decompiler calls them too.
	 *
	 * Which value mode a JSON value is in is decided by its type, in one order that is also the order
	 * of decreasing readability:
	 *
	 *   * a `{"refPath": "..."}` object   -> a quoted package path (the generator re-appends the suffix)
	 *   * an all-numeric X/Y(/Z/W) or R/G/B/A object -> a tuple, `(1.0, 2.0)`
	 *   * a `Build` array whose elements are all reference objects -> an array of quoted paths
	 *   * anything else object- or array-shaped -> a quoted, compacted JSON blob, which is lossless
	 *   * a bool, number or string -> itself, with the spelling rules the lexer needs
	 *
	 * `RenderJsonPropertyAsSource` returns false WITH a reason rather than guessing: a value with no
	 * settled spelling is a gap the caller reports, never half a literal.
	 */

	/** "/Game/FX/M_X.M_X" -> "/Game/FX/M_X". The generator re-appends the object suffix on import. */
	FString ReferenceToPackagePath(const FString& ReferencePath);

	/**
	 * The value at a dotted property path, the way the plan side writes one
	 * (`SetJsonFieldByPath`: `Platforms.QualityLevelMask` is one int32 inside an object).
	 *
	 * The inverse of `SetJsonFieldByPath`, and the reason it is here rather than in either caller:
	 * the two halves have to agree on what a path means or a declared setting reads back as absent.
	 */
	TSharedPtr<FJsonValue> FindJsonPropertyByPath(const TSharedPtr<FJsonObject>& Object, const FString& Path);

	/** The `{"refPath": "..."}` shape the external edit API round-trips object references through. */
	bool TryReadReferenceObject(const TSharedPtr<FJsonValue>& Value, FString& OutPackagePath);

	/** Any text as a DSL string literal: quoted, with backslash, quote, newline, carriage return and tab escaped. */
	FString QuoteSourceString(FString Value);

	/** JSON text as a DSL string literal: compacted onto one line and escaped. */
	bool JsonTextToSourceString(const FString& JsonText, FString& OutLiteral);

	/** A structured value carried verbatim, as a quoted JSON string. */
	bool TryWriteJsonBlob(const TSharedPtr<FJsonValue>& Value, FString& OutLiteral);

	/**
	 * A JSON object that is really a number tuple, as the `(x, y)` literal the DSL already has.
	 *
	 * A four-tuple in source carries no record of whether it meant XYZW or RGBA, so the writer has to
	 * agree with how the reader will encode it: RGBA when the property name contains "Color", XYZW
	 * otherwise. A property whose JSON disagrees with that rule is refused rather than written as a
	 * tuple that would re-import into the wrong four fields.
	 */
	bool TryWriteNumberTuple(const FString& PropertyName, const TSharedPtr<FJsonObject>& Object,
		FString& OutLiteral);

	/**
	 * An array of asset-carrying structs (`Meshes`, `OverrideMaterials`) as a plain array of quoted
	 * paths. Refused when any element carries a non-default field besides its reference, so a custom
	 * pivot, scale or material binding is never flattened away.
	 */
	bool TryWriteReferenceArray(const UClass* PropertyClass, const FString& Key,
		const TArray<TSharedPtr<FJsonValue>>& Elements, FString& OutSource);

	/**
	 * One property value as the right-hand side of `Key = <this>;`.
	 *
	 * @param PropertyClass  the class the property lives on, needed for the array-of-references rule
	 *                       (`Meshes`, `OverrideMaterials`); pass null to skip that rule.
	 * @param OutSource      the value's source text, without the trailing semicolon.
	 * @param OutWhy         why it has no spelling, when it has none.
	 * @return false when the value has no settled spelling; nothing was written to OutSource.
	 */
	bool RenderJsonPropertyAsSource(const UClass* PropertyClass, const FString& Key,
		const TSharedPtr<FJsonValue>& Value, FString& OutSource, FString& OutWhy);
}
