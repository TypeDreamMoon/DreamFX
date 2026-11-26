#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Exports every module and dynamic input, with its input signature.
	 *
	 * Its own translation unit rather than a commandlet detail, because it has two callers that share
	 * nothing else: `dfx index`, which boots an engine to produce the index, and the editor bridge,
	 * which produces the same index from an engine that is already running. The second is the whole
	 * point -- a rebuild that costs 8 seconds instead of 8 seconds plus a 13-second boot.
	 *
	 * Where it goes (Plan/DreamSource, section 13.4): `<source directory>/.dreamfx/index.db`, which the
	 * language extension and dream-mcp read. Only an explicit `-Out=<file>` still writes the old JSON
	 * file, for a person or CI to look at.
	 */
	class FIndexExport
	{
	public:
		/**
		 * @param OutputPath          a JSON file to write instead of the index database, or empty for
		 *                            `<source directory>/.dreamfx/index.db`.
		 * @param bSkipInputs         list the modules without probing their signatures. Fast, and
		 *                            enough to answer "what exists"; not enough for completion inside
		 *                            an argument list.
		 * @param bRetryQuarantined   clear the quarantine list and probe everything again, for after
		 *                            an engine upgrade.
		 * @param OutDestination      the path actually written.
		 * @return 0 on success, non-zero on failure.
		 *
		 * The walk is resumable: engine content exists that overflows the stack when probed, and a
		 * stack overflow ends the process rather than throwing. The module about to be probed is
		 * recorded before the probe, so whatever is still recorded when the next run starts is what
		 * killed the last one, and it is quarantined instead of retried.
		 */
		static int32 Run(const FString& OutputPath, bool bSkipInputs, bool bRetryQuarantined,
			FString& OutDestination);

		/**
		 * Moves the pre-DreamSource index files of one source directory -- `.dfx-index.json`, and the
		 * `.inflight` / `.quarantine` files beside it -- into `<SourceDirectory>/.dreamfx/index.db`, then
		 * deletes them. The quarantine is always merged in (each entry cost a crashed engine); the
		 * modules only when the database has none yet. True when there was nothing to import too.
		 */
		static bool ImportLegacyIndex(const FString& SourceDirectory, FString& OutError);

		/** `<SourceDirectory>/.dreamfx/index.db`. */
		static FString GetIndexDatabasePath(const FString& SourceDirectory);
	};
}
