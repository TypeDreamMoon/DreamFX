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
		return TEXT("1.0");
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
