#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Rebuilds a source file shortly after it is saved (plan 4.7).
	 *
	 * This is the whole iteration loop. DreamShader needed a preview renderer and a WebSocket bridge
	 * to get live feedback; Niagara's asset editor already refreshes on RequestCompile, so an open
	 * generated system plus a saved .dfs *is* the preview -- at roughly a tenth of the cost, and with
	 * no second source of truth to keep in sync.
	 */
	class FSourceWatcher
	{
	public:
		static void Register();
		static void Unregister();

		/** Rebuilds every source queued by a save, ignoring the debounce. Used by the "build now" path. */
		static void FlushPending();
	};
}
