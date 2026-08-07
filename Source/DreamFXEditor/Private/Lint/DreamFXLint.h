#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Static checks over parsed source (plan 4.7).
	 *
	 * These are the red herrings a Niagara compile happily accepts and a profiler finds three weeks
	 * later. Every rule here is answerable from the AST alone -- no asset library, no compile -- which
	 * is what makes lint cheap enough to run on every build.
	 *
	 * Rules emit warnings, never errors: a lint finding is a strong suggestion about an effect that
	 * does work, and turning judgement calls into build failures teaches people to disable the tool.
	 */
	class FLint
	{
	public:
		static void Run(const FDocument& Document, FDiagnosticSink& Diagnostics);
	};
}
