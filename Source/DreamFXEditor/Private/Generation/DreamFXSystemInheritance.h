#pragma once

#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Resolves System Parent="...dfs" recursively without loading or changing an asset.
	 * The result keeps the child's asset identity and source path. SourceHash includes the parent
	 * chain; inherited AST nodes keep their defining files, including relative emitter references.
	 * ParentPath is cleared in the flattened result, so resolving that result again is a no-op.
	 */
	bool ResolveSystemInheritance(const FDocument& Child, FDocument& OutFlattened, FDiagnosticSink& Diagnostics);
}
