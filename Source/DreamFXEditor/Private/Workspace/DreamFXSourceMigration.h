#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * The deprecation notice for a project still keeping its sources in `DFX/`, and the button that moves them to
	 * `DreamSource/FX/` (or wherever UDreamFXSourceSettings points).
	 *
	 * `DFX/` keeps working for good -- this only tells the author it is deprecated and that Dream sources are managed
	 * together under DreamSource/. Logged once per session (FDreamFXPaths::LogDeprecatedSourceDirectoriesOnce); the
	 * notification only in an interactive editor, and not again once the author said so (EditorPerProjectUserSettings
	 * `[DreamSource] bHideFXMigrationNotice`, this machine only). Plan/DreamSource/dreamsource-layout-spec.md, 13.6.
	 */
	struct FDreamFXSourceMigration
	{
		/** Logs the deprecations after engine init; with bInShowNotice, also offers the migration. */
		static void Register(bool bInShowNotice);
		static void Unregister();

		/**
		 * Moves the legacy `DFX/` -- whole, a `.git` inside it included -- to the configured directory, imports its old
		 * index files into `.dreamfx/index.db`, rewrites the DreamFX.code-workspace in it so its relative folders still
		 * point where they did, and deletes the old bridge folder (`Saved/DreamFX/Bridge`). Refuses when the configured
		 * directory already exists. The running editor keeps reading the old place -- the source watcher was set up at
		 * startup -- so the caller asks for a restart. False with OutError on failure; whatever could not be moved is
		 * left where it was.
		 */
		static bool Migrate(FString& OutError);

		/**
		 * The workspace text with every relative folder path that was written against InOldDirectory rewritten against
		 * InNewDirectory. False when the text is not a workspace file.
		 */
		static bool RebaseWorkspaceFolders(const FString& InText, const FString& InOldDirectory, const FString& InNewDirectory, FString& OutText);
	};
}
