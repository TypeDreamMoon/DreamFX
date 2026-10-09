#pragma once

#include "Generation/DreamFXGenerator.h"

namespace UE::DreamFX::Editor
{
	/** Interactive builds must not leave a Niagara script editor holding an obsolete editable copy. */
	class FEditorGeneration
	{
	public:
		static FGenerateResult GenerateFromFile(const FString& FilePath, const FGenerateOptions& Options,
			FDiagnosticSink& Diagnostics);
	};
}
