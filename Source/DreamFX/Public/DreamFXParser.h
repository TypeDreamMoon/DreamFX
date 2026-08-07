#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"

namespace UE::DreamFX
{
	/**
	 * DreamFXLang front end. Parsing is pure: no asset access, no engine state, no Niagara. Everything
	 * that needs the asset library (module schemas, type checking, L6 lowering) happens later in
	 * DreamFXEditor, which keeps the corpus Parse tests runnable without a content-loaded editor.
	 */
	class DREAMFX_API FParser
	{
	public:
		/** Parses source text. Returns false if any error diagnostic was raised. */
		static bool ParseText(const FString& SourceText, const FString& SourceFilePath,
			FDocument& OutDocument, FDiagnosticSink& Diagnostics);

		/** Loads and parses a file, filling in SourceFilePath and SourceHash. */
		static bool ParseFile(const FString& FilePath, FDocument& OutDocument, FDiagnosticSink& Diagnostics);

		/** Maps a file extension (with or without a dot) to the document kind it declares. */
		static bool DocumentKindFromExtension(const FString& Extension, EDocumentKind& OutKind);

		/** ".dfs" / ".dfe" / ".dfm" for the given kind. */
		static const TCHAR* ExtensionForDocumentKind(EDocumentKind Kind);
	};
}
