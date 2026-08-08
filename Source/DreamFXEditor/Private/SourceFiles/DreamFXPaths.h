#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/** One `DFX/` source tree: the project's own, or one belonging to an enabled plugin. */
	struct FSourceRoot
	{
		/** Absolute path of the DFX directory. */
		FString Directory;
		/** The token an author writes in `Root="..."`. Empty for the project root. */
		FString RootToken;
		/** Content mount point the token resolves to, e.g. "/Game" or "/MoonToon". */
		FString MountPoint;
	};

	/**
	 * Source discovery and the `Root="Plugin.X"` addressing scheme, shared by the generator, the
	 * commandlet and the workspace view. Mirrors DreamShader's layout so a team already using .dsm
	 * files finds .dfs files exactly where it expects them.
	 */
	class FDreamFXPaths
	{
	public:
		/**
		 * The one content directory a decompiled export rebuilds into, under whichever mount point it
		 * was read from: `/Game/FX/NS_X` exports to a source whose `Name=` is `Decompiled/FX/NS_X`,
		 * and so rebuilds `/Game/Decompiled/FX/NS_X`.
		 *
		 * plan-v4 V1. What makes an export safe to build is structural: it cannot name the asset it
		 * came from, so no amount of editing or rebuilding can reach that asset. That is what lets the
		 * whole Decompiled tree be first-class source -- watched, built on save, linted, CI'd --
		 * instead of excluded from discovery the way plan-v3 had it.
		 */
		static constexpr const TCHAR* DecompiledNamespace = TEXT("Decompiled");

		/** Project `DFX/` plus every enabled plugin's `DFX/`, in that order. */
		static const TArray<FSourceRoot>& GetSourceRoots();

		/** Forces the next GetSourceRoots call to rescan. Used by the file watcher on plugin changes. */
		static void InvalidateSourceRoots();

		/** Every .dfs / .dfe / .dfm file under every source root, decompiled exports included. */
		static void FindSourceFiles(TArray<FString>& OutFiles);

		/** True if the extension is one DreamFX owns. */
		static bool IsSourceFile(const FString& FilePath);

		/**
		 * True when the file sits under `DecompiledOutputDirectory`.
		 *
		 * No longer an exclusion: an export is ordinary source, discovered and built like any other.
		 * What it still marks is where the structural gate applies -- a file here whose `Name=` is
		 * outside the `Decompiled/` namespace predates plan-v4, still names the asset it was read
		 * from, and is refused rather than allowed to overwrite it (DFX8013).
		 */
		static bool IsDecompiledExport(const FString& FilePath);

		/**
		 * "FX/NS_X" -> "Decompiled/FX/NS_X", relative to a mount point either way.
		 *
		 * Idempotent, and load-bearing that it is: re-exporting a mirror asset has to reproduce the
		 * mirror's own source, not nest a second copy under `Decompiled/Decompiled/`.
		 */
		static FString ToDecompiledNamespace(const FString& MountRelativePath);

		/** True for a full package path inside the mirror namespace, e.g. "/Game/Decompiled/FX/NS_X". */
		static bool IsDecompiledNamespaceAsset(const FString& PackagePath);

		/**
		 * Where an export of `PackagePath` is written on disk, extension included.
		 *
		 * The package path is kept whole, mount point and all: two assets called NS_Spark in different
		 * plugins are two different effects, and flattening them would have one silently replace the
		 * other on the next export.
		 */
		static FString DecompiledSourcePathFor(const FString& PackagePath, const TCHAR* Extension);

		/**
		 * The `Root="..."` token that addresses whichever mount point an asset lives under.
		 *
		 * Deliberately derived from the asset rather than chosen: an asset under `/MoonToon` written
		 * with `Root="Game"` would build into `/Game` and quietly leave the original behind.
		 */
		static bool ResolveRootTokenForPackage(const FString& PackagePath, FString& OutRootToken,
			FString& OutMountPoint, FString& OutError);

		/** Maps `Root="Plugin.MoonToon"` (or "", or "Game") to a content mount point. */
		static bool ResolveRootMountPoint(const FString& Root, FString& OutMountPoint, FString& OutError);

		/** Finds the source root that owns a file, so a bare `Root=` can default sensibly. */
		static bool FindOwningRoot(const FString& FilePath, FSourceRoot& OutRoot);

		/**
		 * Resolves an asset reference written in source into a full object path.
		 *
		 * Accepted spellings:
		 *   "/Niagara/Modules/Update/Forces/GravityForce"   already absolute, used verbatim
		 *   "Plugin.MoonToon:Materials/FX/M_Spark"          explicit root prefix
		 *   "Materials/FX/M_Spark"                          relative to DefaultRoot
		 */
		static bool ResolveAssetPath(const FString& Reference, const FString& DefaultRoot,
			FString& OutPackagePath, FString& OutError);

		/** "/Game/FX/NS_Spark" -> package path "/Game/FX", asset name "NS_Spark". */
		static void SplitPackagePath(const FString& FullPath, FString& OutPackagePath, FString& OutAssetName);

		/** "/Game/FX/NS_Spark" -> "/Game/FX/NS_Spark.NS_Spark", the form LoadObject wants. */
		static FString ToObjectPath(const FString& PackagePath);

		/**
		 * Resolves a `from "..."` reference to a source file on disk.
		 *
		 * Tried relative to the referencing file first, then against every DFX root, so both
		 * `from "../Emitters/E_Flash"` and `from "DFX/Emitters/E_Flash"` work. The extension is
		 * optional.
		 */
		static bool ResolveSourceReference(const FString& Reference, const FString& ReferencingFile,
			const TCHAR* Extension, FString& OutFullPath, FString& OutError);
	};
}
