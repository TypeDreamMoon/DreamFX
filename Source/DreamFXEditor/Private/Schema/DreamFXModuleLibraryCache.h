#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	class FModuleLibrary;

	/**
	 * Keeps module libraries alive between builds in one process.
	 *
	 * A build used to make its own FModuleLibrary, so every editor save of a .dfs paid again for
	 * what the previous one had already learned: a disk walk of the Niagara content folders, a
	 * fresh probe system, and one probe -- module add, switch writes, topology read, remove -- per
	 * distinct module configuration. Measured on a 1230-line system that was most of two seconds,
	 * all of it repeatable. Nothing in it depends on the source being built, only on the Niagara
	 * scripts under the search paths, so it can be kept for exactly as long as those do not change.
	 *
	 * Libraries are keyed by their effective search paths -- the document's `ModulePaths` plus the
	 * built-in defaults, ignoring order -- because two path sets resolve the same name to different
	 * assets. A handful are kept, most recently used first.
	 *
	 * Invalidation is deliberately blunt: any change to a Niagara script anywhere drops every
	 * library, because a wrong cached schema is a build that type-checks against the wrong module,
	 * while a dropped one costs a single slow build. Dropping is safe while a build is running, since
	 * a caller holds its library by shared reference; it simply finishes on the old one.
	 *
	 * Game thread only, apart from the registry notifications, which may arrive from elsewhere and
	 * so only record that a drop is owed.
	 */
	class FModuleLibraryCache
	{
	public:
		/** The library for this set of extra search paths, built on first use and reused after. */
		static TSharedRef<FModuleLibrary> Get(const TArray<FString>& SearchPaths);

		/**
		 * Drops every cached library.
		 *
		 * For a writer that changes Niagara scripts without going through a save the cache already
		 * watches -- the .dfm generator edits a module in place and may not save it at all.
		 */
		static void Invalidate(const TCHAR* Reason);

		/**
		 * Drops every library and stops listening.
		 *
		 * Called from the editor module's ShutdownModule. A library roots its probe system, and a
		 * root left behind past that point is released after the engine has stopped caring.
		 */
		static void Shutdown();
	};
}
