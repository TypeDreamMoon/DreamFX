#pragma once

#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"

namespace UE::DreamFX::Editor
{
	/** Copy a .dfe and apply the host's overrides without changing either input document. */
	bool MergeEmitterDefinitions(const FEmitter& Base, const FEmitter& Override, FEmitter& OutMerged,
		FDiagnosticSink& Diagnostics);
}
