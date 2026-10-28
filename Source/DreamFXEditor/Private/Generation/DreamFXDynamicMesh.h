#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"
#include "DreamFXTypes.h"
#include "Generation/DreamFXGenerator.h"

namespace UE::DreamFX::Editor
{
	/** What a system's `DynamicMesh` blocks lower to (Docs/language/dynamic-mesh.md). */
	struct FDynamicMeshLowering
	{
		/** The system with every DynamicMesh block replaced by the user parameter and emitters it means. */
		FDocument System;

		/**
		 * The generated modules, as parsed `.dfm` documents: the shared grid topology module, the
		 * visibility module when a mesh has `Visible`, and one vertex module per patch. Each carries the
		 * .dfs as its source file and the patch body as its body location, so a compile error points at
		 * the text the author wrote.
		 */
		TArray<FDocument> Modules;

		/** The `.dfm` text of each module, parallel to Modules. */
		TArray<FString> ModuleTexts;

		/** Where each module's text is written for inspection, parallel to Modules. */
		TArray<FString> ModuleDumpPaths;
	};

	struct FDynamicMeshLowerOptions
	{
		/**
		 * Look assets up (materials). Off for lint, which answers from the text alone; the build turns
		 * it on so a misspelt material is an error at the block rather than a grey mesh.
		 */
		bool bResolveAssets = true;
	};

	/**
	 * Lowers every DynamicMesh block of a (flattened) system document. Fails with diagnostics when a
	 * block is malformed; leaves the document unchanged apart from the expansion otherwise. A document
	 * with no blocks lowers to itself.
	 */
	bool LowerDynamicMeshes(const FDocument& System, const FDynamicMeshLowerOptions& LowerOptions,
		FDynamicMeshLowering& OutLowering, FDiagnosticSink& Diagnostics);

	/**
	 * Generates (or, with bVerifyOnly or on an engine with no module backend, checks) the modules a
	 * lowering needs, through the ordinary .dfm generator, and writes their text under
	 * Saved/DreamFX/DynamicMesh. Run before the system's plan, which resolves the modules by path.
	 */
	bool BuildDynamicMeshModules(const FDynamicMeshLowering& Lowering, const FGenerateOptions& Options,
		FDiagnosticSink& Diagnostics);

	/**
	 * True when this engine's dynamic mesh data interface addresses texture coordinates as
	 * `Vertex*2 + TexCoord*2` (UE 5.8) instead of the interleaved layout its renderer reads.
	 * Read once from the engine's shader template.
	 */
	bool DynamicMeshTexCoordsNeedVertexStride();
}
