#include "DreamFXPaths.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		TArray<FSourceRoot> GCachedRoots;
		bool bGRootsCached = false;

		const TCHAR* const SourceExtensions[] = { TEXT("dfs"), TEXT("dfe"), TEXT("dfm") };
	}

	void FDreamFXPaths::InvalidateSourceRoots()
	{
		bGRootsCached = false;
		GCachedRoots.Reset();
	}

	const TArray<FSourceRoot>& FDreamFXPaths::GetSourceRoots()
	{
		if (bGRootsCached)
		{
			return GCachedRoots;
		}

		GCachedRoots.Reset();

		const FString ProjectDfx = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DFX"));
		if (IFileManager::Get().DirectoryExists(*ProjectDfx))
		{
			FSourceRoot Root;
			Root.Directory = ProjectDfx;
			Root.RootToken = FString();
			Root.MountPoint = TEXT("/Game");
			GCachedRoots.Add(MoveTemp(Root));
		}

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			const FString PluginDfx = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("DFX"));
			if (!IFileManager::Get().DirectoryExists(*PluginDfx))
			{
				continue;
			}

			FSourceRoot Root;
			Root.Directory = PluginDfx;
			Root.RootToken = FString::Printf(TEXT("Plugin.%s"), *Plugin->GetName());
			Root.MountPoint = Plugin->GetMountedAssetPath().LeftChop(1); // "/MoonToon/" -> "/MoonToon"
			GCachedRoots.Add(MoveTemp(Root));
		}

		bGRootsCached = true;
		return GCachedRoots;
	}

	void FDreamFXPaths::FindSourceFiles(TArray<FString>& OutFiles)
	{
		for (const FSourceRoot& Root : GetSourceRoots())
		{
			for (const TCHAR* Extension : SourceExtensions)
			{
				TArray<FString> Found;
				IFileManager::Get().FindFilesRecursive(Found, *Root.Directory,
					*FString::Printf(TEXT("*.%s"), Extension), /*Files=*/true, /*Directories=*/false,
					/*bClearFileNames=*/false);
				for (const FString& File : Found)
				{
					OutFiles.AddUnique(FPaths::ConvertRelativePathToFull(File));
				}
			}
		}

		OutFiles.Sort();
	}

	bool FDreamFXPaths::IsSourceFile(const FString& FilePath)
	{
		const FString Extension = FPaths::GetExtension(FilePath);
		for (const TCHAR* Candidate : SourceExtensions)
		{
			if (Extension.Equals(Candidate, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool FDreamFXPaths::ResolveRootMountPoint(const FString& Root, FString& OutMountPoint, FString& OutError)
	{
		if (Root.IsEmpty() || Root.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
		{
			OutMountPoint = TEXT("/Game");
			return true;
		}

		if (Root.StartsWith(TEXT("Plugin."), ESearchCase::IgnoreCase))
		{
			const FString PluginName = Root.RightChop(7);
			if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName))
			{
				if (!Plugin->IsEnabled())
				{
					OutError = FString::Printf(
						TEXT("Root=\"%s\" names plugin '%s', which is installed but not enabled for this project."),
						*Root, *PluginName);
					return false;
				}
				if (!Plugin->CanContainContent())
				{
					OutError = FString::Printf(
						TEXT("Root=\"%s\" names plugin '%s', which has no content directory. Set \"CanContainContent\": true in its .uplugin."),
						*Root, *PluginName);
					return false;
				}
				OutMountPoint = Plugin->GetMountedAssetPath().LeftChop(1);
				return true;
			}

			OutError = FString::Printf(TEXT("Root=\"%s\" names plugin '%s', which was not found."), *Root, *PluginName);
			return false;
		}

		OutError = FString::Printf(
			TEXT("Root=\"%s\" is not a recognised root. Expected \"Game\" or \"Plugin.<PluginName>\"."), *Root);
		return false;
	}

	bool FDreamFXPaths::FindOwningRoot(const FString& FilePath, FSourceRoot& OutRoot)
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(FilePath);

		// Longest match wins: a plugin nested under the project directory would otherwise be claimed by
		// the project root.
		int32 BestLength = -1;
		for (const FSourceRoot& Root : GetSourceRoots())
		{
			if (FullPath.StartsWith(Root.Directory, ESearchCase::IgnoreCase) && Root.Directory.Len() > BestLength)
			{
				BestLength = Root.Directory.Len();
				OutRoot = Root;
			}
		}
		return BestLength >= 0;
	}

	bool FDreamFXPaths::ResolveAssetPath(const FString& Reference, const FString& DefaultRoot,
		FString& OutPackagePath, FString& OutError)
	{
		const FString Trimmed = Reference.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("Empty asset reference.");
			return false;
		}

		if (Trimmed.StartsWith(TEXT("/")))
		{
			// Already a content path. Strip any "Package.Object" suffix so callers get a uniform shape.
			FString PackagePath = Trimmed;
			int32 DotIndex;
			if (PackagePath.FindLastChar(TEXT('.'), DotIndex))
			{
				const FString AfterDot = PackagePath.RightChop(DotIndex + 1);
				const FString BeforeDot = PackagePath.Left(DotIndex);
				if (BeforeDot.EndsWith(FString::Printf(TEXT("/%s"), *AfterDot)))
				{
					PackagePath = BeforeDot;
				}
			}
			OutPackagePath = PackagePath;
			return true;
		}

		FString RootToken = DefaultRoot;
		FString Relative = Trimmed;

		int32 ColonIndex;
		if (Trimmed.FindChar(TEXT(':'), ColonIndex))
		{
			RootToken = Trimmed.Left(ColonIndex);
			Relative = Trimmed.RightChop(ColonIndex + 1);
		}

		FString MountPoint;
		if (!ResolveRootMountPoint(RootToken, MountPoint, OutError))
		{
			return false;
		}

		Relative.RemoveFromStart(TEXT("/"));
		OutPackagePath = MountPoint / Relative;
		return true;
	}

	void FDreamFXPaths::SplitPackagePath(const FString& FullPath, FString& OutPackagePath, FString& OutAssetName)
	{
		int32 SlashIndex;
		if (FullPath.FindLastChar(TEXT('/'), SlashIndex))
		{
			OutPackagePath = FullPath.Left(SlashIndex);
			OutAssetName = FullPath.RightChop(SlashIndex + 1);
		}
		else
		{
			OutPackagePath = FString();
			OutAssetName = FullPath;
		}
	}

	bool FDreamFXPaths::ResolveSourceReference(const FString& Reference, const FString& ReferencingFile,
		const TCHAR* Extension, FString& OutFullPath, FString& OutError)
	{
		FString Relative = Reference.TrimStartAndEnd();
		if (Relative.IsEmpty())
		{
			OutError = TEXT("Empty source reference.");
			return false;
		}

		if (!FPaths::GetExtension(Relative).Equals(FString(Extension).RightChop(1), ESearchCase::IgnoreCase))
		{
			Relative += Extension;
		}

		TArray<FString> Candidates;

		// Relative to the referencing file first: that is what a reader assumes a bare path means.
		if (!ReferencingFile.IsEmpty())
		{
			Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::GetPath(ReferencingFile), Relative));
		}

		for (const FSourceRoot& Root : GetSourceRoots())
		{
			Candidates.Add(FPaths::ConvertRelativePathToFull(Root.Directory / Relative));

			// `DFX/Emitters/E_Flash` names the root directory explicitly; accept that spelling too,
			// because it is how the plan document writes it.
			FString WithoutDfxPrefix = Relative;
			if (WithoutDfxPrefix.RemoveFromStart(TEXT("DFX/"), ESearchCase::IgnoreCase))
			{
				Candidates.Add(FPaths::ConvertRelativePathToFull(Root.Directory / WithoutDfxPrefix));
			}
		}

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::FileExists(Candidate))
			{
				OutFullPath = Candidate;
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Could not find '%s'. Looked in: %s"),
			*Relative, *FString::Join(Candidates, TEXT(", ")));
		return false;
	}

	FString FDreamFXPaths::ToObjectPath(const FString& PackagePath)
	{
		FString Package;
		FString AssetName;
		SplitPackagePath(PackagePath, Package, AssetName);
		return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	}
}
