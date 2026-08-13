#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Exports every module and dynamic input, with its input signature, as JSON.
	 *
	 * Its own translation unit rather than a commandlet detail, because it has two callers that share
	 * nothing else: `dfx index`, which boots an engine to produce the file, and the editor bridge,
	 * which produces the same file from an engine that is already running. The second is the whole
	 * point -- a rebuild that costs 8 seconds instead of 8 seconds plus a 13-second boot.
	 */
	class FIndexExport
	{
	public:
		/**
		 * @param OutputPath          where to write, or empty for `<Project>/DFX/.dfx-index.json`.
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
	};
}
