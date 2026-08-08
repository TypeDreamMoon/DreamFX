#include "DreamFXProvenance.h"

#include "UObject/MetaData.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		const TCHAR* const KeySourcePath     = TEXT("DreamFX.SourcePath");
		const TCHAR* const KeySourceFullPath = TEXT("DreamFX.SourceFullPath");
		const TCHAR* const KeySourceHash     = TEXT("DreamFX.SourceHash");
		const TCHAR* const KeyGenerator      = TEXT("DreamFX.GeneratorVersion");
		const TCHAR* const KeyModules        = TEXT("DreamFX.ModuleDependencies");
		const TCHAR* const KeyModuleVersions = TEXT("DreamFX.ModuleVersions");

		FMetaData* GetPackageMetaData(const UObject* Asset)
		{
			if (Asset == nullptr)
			{
				return nullptr;
			}
			UPackage* Package = Asset->GetOutermost();
			return Package ? &Package->GetMetaData() : nullptr;
		}
	}

	const TCHAR* FProvenance::GetGeneratorVersion()
	{
		// Format: <language revision>.<generator revision>. The language part changes when source that
		// used to compile no longer means the same thing; the generator part when the emitted asset
		// shape changes. Either one invalidates every cached asset.
		//
		// 1.1 -- stamp v2 (plan-v2 W3): module version GUIDs. Bumped because an asset stamped by 1.0
		// has no versions recorded, and treating "absent" as "unchanged" would make the check useless
		// on exactly the assets that predate it.
		return TEXT("1.1");
	}

	void FProvenance::Write(UObject* Asset, const FProvenanceStamp& Stamp)
	{
		FMetaData* MetaData = GetPackageMetaData(Asset);
		if (MetaData == nullptr)
		{
			return;
		}

		MetaData->SetValue(Asset, KeySourcePath, *Stamp.SourceRelativePath);
		MetaData->SetValue(Asset, KeySourceFullPath, *Stamp.SourceFullPath);
		MetaData->SetValue(Asset, KeySourceHash, *Stamp.SourceHash);
		MetaData->SetValue(Asset, KeyGenerator, *Stamp.GeneratorVersion);
		MetaData->SetValue(Asset, KeyModules, *FString::Join(Stamp.ModuleDependencies, TEXT(";")));

		// "path=1.0:guid" pairs. Sorted, so an unchanged build produces an unchanged stamp and the
		// asset does not show up as modified in a diff for no reason.
		TArray<FString> VersionPairs;
		VersionPairs.Reserve(Stamp.ModuleVersions.Num());
		for (const TPair<FString, FString>& Entry : Stamp.ModuleVersions)
		{
			VersionPairs.Add(FString::Printf(TEXT("%s=%s"), *Entry.Key, *Entry.Value));
		}
		VersionPairs.Sort();
		MetaData->SetValue(Asset, KeyModuleVersions, *FString::Join(VersionPairs, TEXT(";")));
	}

	bool FProvenance::Read(const UObject* Asset, FProvenanceStamp& OutStamp)
	{
		FMetaData* MetaData = GetPackageMetaData(Asset);
		if (MetaData == nullptr || !MetaData->HasValue(Asset, KeySourceHash))
		{
			return false;
		}

		OutStamp.SourceRelativePath = MetaData->GetValue(Asset, KeySourcePath);
		OutStamp.SourceFullPath = MetaData->GetValue(Asset, KeySourceFullPath);
		OutStamp.SourceHash = MetaData->GetValue(Asset, KeySourceHash);
		OutStamp.GeneratorVersion = MetaData->GetValue(Asset, KeyGenerator);

		const FString Modules = MetaData->GetValue(Asset, KeyModules);
		OutStamp.ModuleDependencies.Reset();
		if (!Modules.IsEmpty())
		{
			Modules.ParseIntoArray(OutStamp.ModuleDependencies, TEXT(";"), /*InCullEmpty=*/true);
		}

		OutStamp.ModuleVersions.Reset();
		const FString Versions = MetaData->GetValue(Asset, KeyModuleVersions);
		if (!Versions.IsEmpty())
		{
			TArray<FString> VersionPairs;
			Versions.ParseIntoArray(VersionPairs, TEXT(";"), /*InCullEmpty=*/true);
			for (const FString& Pair : VersionPairs)
			{
				FString Path;
				FString Version;
				if (Pair.Split(TEXT("="), &Path, &Version))
				{
					OutStamp.ModuleVersions.Add(Path, Version);
				}
			}
		}

		return true;
	}

	void FProvenance::Clear(UObject* Asset)
	{
		if (FMetaData* MetaData = GetPackageMetaData(Asset))
		{
			MetaData->RemoveValue(Asset, KeySourcePath);
			MetaData->RemoveValue(Asset, KeySourceFullPath);
			MetaData->RemoveValue(Asset, KeySourceHash);
			MetaData->RemoveValue(Asset, KeyGenerator);
			MetaData->RemoveValue(Asset, KeyModules);
			MetaData->RemoveValue(Asset, KeyModuleVersions);
		}
	}

	bool FProvenance::IsUpToDate(const UObject* Asset, const FString& SourceHash)
	{
		FProvenanceStamp Stamp;
		if (!Read(Asset, Stamp))
		{
			return false;
		}

		// A generator upgrade invalidates the cache even when the source is byte-identical: the same
		// text can lower to a different asset after a generator change.
		return Stamp.SourceHash == SourceHash && Stamp.GeneratorVersion == GetGeneratorVersion();
	}
}
