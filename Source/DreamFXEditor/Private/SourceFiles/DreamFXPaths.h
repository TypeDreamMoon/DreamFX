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
		/** Project `DFX/` plus every enabled plugin's `DFX/`, in that order. */
		static const TArray<FSourceRoot>& GetSourceRoots();

		/** Forces the next GetSourceRoots call to rescan. Used by the file watcher on plugin changes. */
		static void InvalidateSourceRoots();

		/** All .dfs / .dfe / .dfm files under every source root. */
		static void FindSourceFiles(TArray<FString>& OutFiles);

		/** True if the extension is one DreamFX owns. */
		static bool IsSourceFile(const FString& FilePath);

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
	};
}
