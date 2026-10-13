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
	 *
	 * plan-v3 E1 routes the editor menus through the same queue rather than opening a second build
	 * path: a menu that could succeed where a save fails would be a bug generator.
	 */
	class FSourceWatcher
	{
	public:
		static void Register();
		static void Unregister();

		/** Reconciles directory watches with current source roots, including newly created DFX folders. */
		static void RefreshSourceRoots(bool bQueueNewSources = true);

		/** Rebuilds every source queued by a save, ignoring the debounce. Used by the "build now" path. */
		static void FlushPending();

		/**
		 * Queues one file as if it had just been saved.
		 *
		 * @param bAnnounceSuccess  toast on success too. A save should stay silent when it worked --
		 *                          the open asset editor already shows the result -- but a menu command
		 *                          that produced no visible reaction reads as a broken menu.
		 */
		static void QueueFile(const FString& FilePath, bool bAnnounceSuccess = false);

		/** Queues every .dfs/.dfe/.dfm under every source root. Returns how many were queued. */
		static int32 QueueAllSources(bool bAnnounceSuccess = true);

		/**
		 * Declares that the editor itself just wrote `File` and that its content hashes to `ContentHash`
		 * (FOwnWriteLedger::HashBytes / HashFile), so the change notification that follows is not a
		 * save by the author and must not queue a rebuild.
		 *
		 * Write-back is the caller: it rewrites a .dfs from the asset's values, and rebuilding the asset
		 * from that text would be the editor answering its own write. Only a change that leaves the file
		 * with exactly this content is skipped. The author editing the file again changes the hash, and
		 * that save rebuilds as usual. Notifications that never arrive cost nothing -- the declaration
		 * expires on its own (FOwnWriteLedger::DefaultLifetimeSeconds).
		 */
		static void IgnoreOwnWrite(const FString& File, const FString& ContentHash);

		/** The same, hashing the file as it stands now. Call right after the write, before anything else can touch it. */
		static void IgnoreOwnWrite(const FString& File);

		/**
		 * The question the watch session asks about every changed file: is this the write that
		 * IgnoreOwnWrite declared? Exposed so a caller can see that its declaration took.
		 */
		static bool IsOwnWrite(const FString& File);
	};
}
