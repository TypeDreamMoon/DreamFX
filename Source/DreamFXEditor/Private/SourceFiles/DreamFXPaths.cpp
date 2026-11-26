#include "DreamFXPaths.h"

#include "DreamFXModule.h"
#include "Settings/DreamFXEditorSettings.h"
#include "Settings/DreamFXSourceSettings.h"

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

		/** Forward slashes, trimmed, no trailing slash. */
		FString DreamFXPaths_CleanSettingPath(FString InPath)
		{
			InPath.TrimStartAndEndInline();
			InPath.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (InPath.EndsWith(TEXT("/")))
			{
				InPath.LeftChopInline(1);
			}
			return InPath;
		}

		/** Absolute, normalized, no trailing slash. */
		FString DreamFXPaths_FullDirectory(const FString& InPath)
		{
			FString Directory = FPaths::ConvertRelativePathToFull(InPath);
			FPaths::NormalizeDirectoryName(Directory);
			Directory.RemoveFromEnd(TEXT("/"));
			return Directory;
		}

		/** `SourceDirectory` as configured, relative to the project; the default when unset, absolute or empty. */
		FString DreamFXPaths_ConfiguredRelativeSourceDirectory()
		{
			const UDreamFXSourceSettings* Settings = GetDefault<UDreamFXSourceSettings>();
			const FString Configured = DreamFXPaths_CleanSettingPath(Settings ? Settings->SourceDirectory.Path : FString());
			// Absolute paths are refused: the extensions and every other checkout have to reproduce the same answer.
			if (Configured.IsEmpty() || !FPaths::IsRelative(Configured))
			{
				return UDreamFXSourceSettings::GetDefaultSourceDirectory();
			}
			return Configured;
		}

		struct DreamFXPaths_FProjectSourceDirectoryMemo
		{
			FString ConfiguredRelative;
			FProjectSourceDirectory Resolved;
			bool bInitialized = false;
		};

		DreamFXPaths_FProjectSourceDirectoryMemo DreamFXPaths_GProjectSourceDirectoryMemo;

		FProjectSourceDirectory DreamFXPaths_ResolveProjectSourceDirectoryUncached(const FString& InConfiguredRelative)
		{
			FProjectSourceDirectory Out;
			Out.ConfiguredRelative = InConfiguredRelative;
			Out.ConfiguredDirectory = DreamFXPaths_FullDirectory(FPaths::Combine(FPaths::ProjectDir(), InConfiguredRelative));
			Out.LegacyDirectory = DreamFXPaths_FullDirectory(FPaths::Combine(FPaths::ProjectDir(), FDreamFXPaths::LegacySourceDirectoryName));

			const bool bConfiguredExists = IFileManager::Get().DirectoryExists(*Out.ConfiguredDirectory);
			// A configured directory that IS `DFX` is not a legacy one: it is what was asked for.
			const bool bSame = Out.ConfiguredDirectory.Equals(Out.LegacyDirectory, ESearchCase::IgnoreCase);
			const bool bLegacyExists = !bSame && IFileManager::Get().DirectoryExists(*Out.LegacyDirectory);

			if (bConfiguredExists)
			{
				Out.Directory = Out.ConfiguredDirectory;
				Out.bLegacyIgnored = bLegacyExists;
			}
			else if (bLegacyExists)
			{
				Out.Directory = Out.LegacyDirectory;
				Out.bUsingLegacy = true;
			}
			else
			{
				Out.Directory = Out.ConfiguredDirectory;
			}
			return Out;
		}
	}

	FProjectSourceDirectory FDreamFXPaths::ResolveProjectSourceDirectory()
	{
		const FString ConfiguredRelative = DreamFXPaths_ConfiguredRelativeSourceDirectory();
		DreamFXPaths_FProjectSourceDirectoryMemo& Memo = DreamFXPaths_GProjectSourceDirectoryMemo;
		// Re-resolved only when the setting itself changes, never because a directory appeared: a migration in this
		// session must not flip the answer under the watcher that was set up on the old one.
		if (!Memo.bInitialized || !Memo.ConfiguredRelative.Equals(ConfiguredRelative, ESearchCase::IgnoreCase))
		{
			Memo.ConfiguredRelative = ConfiguredRelative;
			Memo.Resolved = DreamFXPaths_ResolveProjectSourceDirectoryUncached(ConfiguredRelative);
			Memo.bInitialized = true;
		}
		return Memo.Resolved;
	}

	FString FDreamFXPaths::GetProjectSourceDirectory()
	{
		return ResolveProjectSourceDirectory().Directory;
	}

	FString FDreamFXPaths::GetPluginSourceDirectory(const FString& PluginBaseDir, bool* bOutLegacy)
	{
		const FString Current = DreamFXPaths_FullDirectory(FPaths::Combine(PluginBaseDir, PluginSourceDirectory));
		const FString Legacy = DreamFXPaths_FullDirectory(FPaths::Combine(PluginBaseDir, LegacySourceDirectoryName));
		const bool bLegacy = !IFileManager::Get().DirectoryExists(*Current) && IFileManager::Get().DirectoryExists(*Legacy);
		if (bOutLegacy)
		{
			*bOutLegacy = bLegacy;
		}
		return bLegacy ? Legacy : Current;
	}

	FString FDreamFXPaths::GetDreamSourceIntermediateDirectory()
	{
		return DreamFXPaths_FullDirectory(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("DreamSource")));
	}

	FString FDreamFXPaths::GetFXIntermediateDirectory()
	{
		return FPaths::Combine(GetDreamSourceIntermediateDirectory(), TEXT("FX"));
	}

	FString FDreamFXPaths::GetFXDatabasePath()
	{
		return FPaths::Combine(GetDreamSourceIntermediateDirectory(), TEXT("fx.db"));
	}

	FString FDreamFXPaths::GetIndexDatabasePath()
	{
		return FPaths::Combine(GetProjectSourceDirectory(), TEXT(".dreamfx/index.db"));
	}

	FString FDreamFXPaths::GetDecompiledOutputDirectory()
	{
		const UDreamFXEditorSettings* Settings = GetDefault<UDreamFXEditorSettings>();
		const FString Configured = DreamFXPaths_CleanSettingPath(Settings ? Settings->DecompiledOutputDirectory : FString());
		// `DFX/Decompiled` was the default every saved DefaultEditor.ini spells out. While the project is still on `DFX/`
		// the two answers are the same directory; after a migration only this one follows the sources.
		if (Configured.IsEmpty() || Configured.Equals(TEXT("DFX/Decompiled"), ESearchCase::IgnoreCase))
		{
			return FPaths::Combine(GetProjectSourceDirectory(), TEXT("Decompiled"));
		}
		return DreamFXPaths_FullDirectory(FPaths::Combine(FPaths::ProjectDir(), Configured));
	}

	void FDreamFXPaths::LogDeprecatedSourceDirectoriesOnce()
	{
		static TSet<FString> Logged;
		const FProjectSourceDirectory Project = ResolveProjectSourceDirectory();
		if (Project.bUsingLegacy && !Logged.Contains(Project.LegacyDirectory))
		{
			Logged.Add(Project.LegacyDirectory);
			UE_LOG(LogDreamFX, Warning,
				TEXT("DreamFX: '%s' is deprecated. Move it to '%s' -- Dream sources are managed together under DreamSource/. ")
				TEXT("It is still read until then."),
				*Project.LegacyDirectory, *Project.ConfiguredDirectory);
		}
		if (Project.bLegacyIgnored && !Logged.Contains(Project.LegacyDirectory))
		{
			Logged.Add(Project.LegacyDirectory);
			UE_LOG(LogDreamFX, Warning,
				TEXT("DreamFX: '%s' is ignored because '%s' exists. Merge what it still holds into '%s' and delete it."),
				*Project.LegacyDirectory, *Project.ConfiguredDirectory, *Project.ConfiguredDirectory);
		}
		for (const FSourceRoot& Root : GetSourceRoots())
		{
			if (Root.bLegacy && !Root.RootToken.IsEmpty() && !Logged.Contains(Root.Directory))
			{
				Logged.Add(Root.Directory);
				UE_LOG(LogDreamFX, Warning,
					TEXT("DreamFX: %s keeps its sources in the deprecated '%s'. Move them to '%s'."),
					*Root.RootToken, *Root.Directory,
					*FPaths::Combine(FPaths::GetPath(Root.Directory), PluginSourceDirectory));
			}
		}
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

		const FProjectSourceDirectory Project = ResolveProjectSourceDirectory();
		if (IFileManager::Get().DirectoryExists(*Project.Directory))
		{
			FSourceRoot Root;
			Root.Directory = Project.Directory;
			Root.RootToken = FString();
			Root.MountPoint = TEXT("/Game");
			Root.bLegacy = Project.bUsingLegacy;
			GCachedRoots.Add(MoveTemp(Root));
		}

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			bool bLegacy = false;
			const FString PluginDirectory = GetPluginSourceDirectory(Plugin->GetBaseDir(), &bLegacy);
			if (!IFileManager::Get().DirectoryExists(*PluginDirectory))
			{
				continue;
			}

			FSourceRoot Root;
			Root.bLegacy = bLegacy;
			Root.Directory = PluginDirectory;
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
					// Decompiled exports are included (plan-v4 V1-3). They used to be filtered out here
					// because their `Name=` pointed at the asset they were read from, so building one
					// overwrote third-party content; now it points into the `Decompiled/` namespace, so
					// the worst a build can do is rewrite the mirror. The cost of the old exclusion was
					// that editing an export and saving did nothing at all, with no message.
					OutFiles.AddUnique(FPaths::ConvertRelativePathToFull(File));
				}
			}
		}

		OutFiles.Sort();
	}

	bool FDreamFXPaths::IsDecompiledExport(const FString& FilePath)
	{
		FString Directory = GetDecompiledOutputDirectory();
		if (!Directory.EndsWith(TEXT("/")))
		{
			Directory += TEXT("/");
		}

		return FPaths::ConvertRelativePathToFull(FilePath).StartsWith(Directory, ESearchCase::IgnoreCase);
	}

	FString FDreamFXPaths::ToDecompiledNamespace(const FString& MountRelativePath)
	{
		FString Relative = MountRelativePath;
		Relative.RemoveFromStart(TEXT("/"));

		const FString Prefix = FString(DecompiledNamespace) + TEXT("/");
		if (Relative.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return Relative;
		}
		return Prefix + Relative;
	}

	bool FDreamFXPaths::IsDecompiledNamespaceAsset(const FString& PackagePath)
	{
		// The namespace sits directly under the mount point, so what matters is the second segment:
		// "/Game/Decompiled/FX/NS_X" yes, "/Game/FX/Decompiled/NS_X" no.
		FString Remainder = PackagePath;
		if (!Remainder.RemoveFromStart(TEXT("/")))
		{
			return false;
		}

		int32 SlashIndex;
		if (!Remainder.FindChar(TEXT('/'), SlashIndex))
		{
			return false;
		}

		return Remainder.RightChop(SlashIndex + 1)
			.StartsWith(FString(DecompiledNamespace) + TEXT("/"), ESearchCase::IgnoreCase);
	}

	FString FDreamFXPaths::DecompiledSourcePathFor(const FString& PackagePath, const TCHAR* Extension)
	{
		FString Relative = PackagePath;
		Relative.RemoveFromStart(TEXT("/"));

		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(GetDecompiledOutputDirectory(), Relative + Extension));
	}

	bool FDreamFXPaths::ResolveRootTokenForPackage(const FString& PackagePath, FString& OutRootToken,
		FString& OutMountPoint, FString& OutError)
	{
		const FString MountPointName = FPackageName::GetPackageMountPoint(PackagePath).ToString();
		if (MountPointName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("'%s' is not under any mounted content root."), *PackagePath);
			return false;
		}

		OutMountPoint = FString::Printf(TEXT("/%s"), *MountPointName);

		if (MountPointName == TEXT("Game"))
		{
			OutRootToken = TEXT("Game");
			return true;
		}

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			if (Plugin->GetMountedAssetPath().LeftChop(1) == OutMountPoint)
			{
				OutRootToken = FString::Printf(TEXT("Plugin.%s"), *Plugin->GetName());
				return true;
			}
		}

		OutError = FString::Printf(
			TEXT("'%s' is mounted at '%s', which belongs to no enabled plugin. DreamFX can only address /Game and plugin content."),
			*PackagePath, *OutMountPoint);
		return false;
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

	void FDreamFXPaths::GetSourceReferenceCandidates(const FString& Reference, const FString& ReferencingFile,
		const TCHAR* Extension, TArray<FString>& OutCandidates)
	{
		FString Relative = Reference.TrimStartAndEnd();
		if (Relative.IsEmpty()) { return; }
		if (!FPaths::GetExtension(Relative).Equals(FString(Extension).RightChop(1), ESearchCase::IgnoreCase))
		{
			Relative += Extension;
		}
		if (!ReferencingFile.IsEmpty())
		{
			OutCandidates.AddUnique(FPaths::ConvertRelativePathToFull(FPaths::GetPath(ReferencingFile), Relative));
		}
		// The directory name an author may have written in front: the legacy `DFX/` (every source before the DreamSource
		// layout), and the project's configured one. Migrating moves the files and never edits them, so both must keep
		// resolving against whichever root the file now sits in.
		const FString ConfiguredPrefix = ResolveProjectSourceDirectory().ConfiguredRelative + TEXT("/");
		const FString Prefixes[] = { FString(LegacySourceDirectoryName) + TEXT("/"), ConfiguredPrefix };
		for (const FSourceRoot& Root : GetSourceRoots())
		{
			OutCandidates.AddUnique(FPaths::ConvertRelativePathToFull(Root.Directory / Relative));
			for (const FString& Prefix : Prefixes)
			{
				FString WithoutPrefix = Relative;
				if (WithoutPrefix.RemoveFromStart(Prefix, ESearchCase::IgnoreCase))
				{
					OutCandidates.AddUnique(FPaths::ConvertRelativePathToFull(Root.Directory / WithoutPrefix));
				}
			}
		}
	}

	bool FDreamFXPaths::ResolveSourceReference(const FString& Reference, const FString& ReferencingFile,
		const TCHAR* Extension, FString& OutFullPath, FString& OutError)
	{
		TArray<FString> Candidates;
		GetSourceReferenceCandidates(Reference, ReferencingFile, Extension, Candidates);
		if (Candidates.IsEmpty()) { OutError = TEXT("Empty source reference."); return false; }
		for (const FString& Candidate : Candidates)
		{
			if (FPaths::FileExists(Candidate))
			{
				OutFullPath = Candidate;
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Could not find '%s'. Looked in: %s"),
			*Reference, *FString::Join(Candidates, TEXT(", ")));
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
