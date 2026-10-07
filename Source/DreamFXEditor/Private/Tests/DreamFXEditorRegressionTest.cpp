#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXProvenance.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "UI/DreamFXAssetCommands.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Workspace/DreamFXSourceDependencies.h"

namespace
{
	/** Every file and directory removed here was created by this test under its unique directory. */
	struct FEditorRegressionFiles
	{
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
			/ TEXT(".dreamfx-test-tmp") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TArray<FString> Files;
		TArray<FString> Directories;

		FString Write(const FString& Name, const FString& Text)
		{
			const FString Path = Directory / Name;
			const FString Parent = FPaths::GetPath(Path);
			IFileManager::Get().MakeDirectory(*Parent, /*Tree=*/true);
			Directories.AddUnique(Parent);
			if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return FString();
			}
			Files.AddUnique(Path);
			return Path;
		}

		~FEditorRegressionFiles()
		{
			for (const FString& File : Files)
			{
				IFileManager::Get().Delete(*File);
			}
			Directories.Sort([](const FString& Left, const FString& Right) { return Left.Len() > Right.Len(); });
			for (const FString& Parent : Directories)
			{
				IFileManager::Get().DeleteDirectory(*Parent, /*RequireExists=*/false, /*Tree=*/false);
			}
			IFileManager::Get().DeleteDirectory(*Directory, false, false);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSourceDependencyRegressionTest,
	"DreamFX.Editor.SourceDependencies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSourceDependencyRegressionTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	FEditorRegressionFiles Files;
	const FString Shared = Files.Write(TEXT("One/Shared.dfe"), TEXT("Emitter(Name=\"Shared\") {}"));
	const FString OtherShared = Files.Write(TEXT("Two/Shared.dfe"), TEXT("Emitter(Name=\"Shared\") {}"));
	const FString HostA = Files.Write(TEXT("One/A.dfs"),
		TEXT("System(Name=\"Test/A\") { Emitter First from \"./Shared\" {} Emitter Second from \"Shared.dfe\" {} }"));
	const FString HostB = Files.Write(TEXT("One/B.dfs"),
		TEXT("System(Name=\"Test/B\") { Emitter First from \"Shared\" {} }"));
	const FString HostC = Files.Write(TEXT("Two/C.dfs"),
		TEXT("System(Name=\"Test/C\") { Emitter First from \"Shared\" {} }"));
	if (Shared.IsEmpty() || OtherShared.IsEmpty() || HostA.IsEmpty() || HostB.IsEmpty() || HostC.IsEmpty())
	{
		AddError(TEXT("Could not create dependency test sources."));
		return false;
	}

	FSourceDependencyIndex Index;
	const TArray<FString> Sources = { HostA, HostB, HostC };
	Index.Refresh(Sources);
	TSet<FString> Dependents;
	Index.FindDependents({ Shared }, Dependents);
	TestEqual(TEXT("one emitter invalidates both hosts, once each"), Dependents.Num(), 2);
	TestTrue(TEXT("first host invalidated"), Dependents.Contains(HostA));
	TestTrue(TEXT("second host invalidated"), Dependents.Contains(HostB));
	TestFalse(TEXT("same basename in a different directory is unrelated"), Dependents.Contains(HostC));

	// Deletion notifications must use the previous index before refreshing resolution.
	IFileManager::Get().Delete(*Shared);
	Dependents.Reset();
	Index.FindDependents({ Shared }, Dependents);
	TestEqual(TEXT("deleted dependency still invalidates its hosts"), Dependents.Num(), 2);
	Index.Refresh(Sources);
	Files.Write(TEXT("One/Shared.dfe"), TEXT("Emitter(Name=\"Shared\") {}"));
	Index.Refresh(Sources);
	Dependents.Reset();
	Index.FindDependents({ Shared }, Dependents);
	TestEqual(TEXT("recreated dependency is rediscovered"), Dependents.Num(), 2);

	Files.Write(TEXT("One/A.dfs"), TEXT("System(Name=\"Test/A\") {}"));
	Index.Refresh({ HostA, HostC }); // B was removed from the source tree.
	Dependents.Reset();
	Index.FindDependents({ Shared }, Dependents);
	TestEqual(TEXT("removed references and deleted hosts do not remain in the index"), Dependents.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXVerifyBatchRegressionTest,
	"DreamFX.Editor.VerifyBatchIsReadOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXVerifyBatchRegressionTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	FEditorRegressionFiles Files;
	const FString Name = TEXT("DreamFXTests/NS_VerifyMissing_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Source = Files.Write(TEXT("Missing.dfs"), FString::Printf(TEXT("System(Name=\"%s\", Root=\"Game\") {}"), *Name));
	const FString Broken = Files.Write(TEXT("Broken.dfs"), TEXT("System("));
	const FString Emitter = Files.Write(TEXT("Shared.dfe"), TEXT("Emitter(Name=\"Shared\") {}"));
	if (Source.IsEmpty() || Broken.IsEmpty() || Emitter.IsEmpty())
	{
		AddError(TEXT("Could not create verification test sources."));
		return false;
	}
	const FString AssetPath = TEXT("/Game/") + Name;
	const FString PackageFile = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	TestFalse(TEXT("test asset does not exist before verification"), FPaths::FileExists(PackageFile));

	const FVerifyBatchResult Result = FDreamFXCommands::VerifySources({ Source, Broken, Emitter });
	TestEqual(TEXT("only independently generated sources are checked"), Result.Checked, 2);
	TestEqual(TEXT("missing asset reports drift"), Result.Drifted, 1);
	TestEqual(TEXT("both drift and parsing failure are counted"), Result.Failed, 2);
	TestFalse(TEXT("batch reports failure instead of queue acceptance"), Result.IsSuccessful());
	TestFalse(TEXT("verification must not generate or save the missing asset"), FPaths::FileExists(PackageFile));
	TestTrue(TEXT("missing-asset diagnostics keep their source path"), Result.Diagnostics.GetDiagnostics().ContainsByPredicate(
		[&Source](const FDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("DFX7001") && FPaths::IsSamePath(Diagnostic.File, Source);
		}));
	TestTrue(TEXT("parse diagnostics keep their separate source path"), Result.Diagnostics.GetDiagnostics().ContainsByPredicate(
		[&Broken](const FDiagnostic& Diagnostic)
		{
			return Diagnostic.Severity == EDiagnosticSeverity::Error && FPaths::IsSamePath(Diagnostic.File, Broken);
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXVerifyExistingAssetRegressionTest,
	"DreamFX.Editor.VerifyExistingAssetIsReadOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXVerifyExistingAssetRegressionTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	FEditorRegressionFiles Files;
	const FString Name = TEXT("DreamFXTests/VerifyReadOnly_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/NS_VerifyReadOnly");
	const FString PackageName = TEXT("/Game/") + Name;
	const FString PackageFile = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const FString PackageDirectory = FPaths::GetPath(PackageFile);
	if (!TestFalse(TEXT("test package is new"), FPaths::FileExists(PackageFile))
		|| !TestFalse(TEXT("test package directory is new"), IFileManager::Get().DirectoryExists(*PackageDirectory)))
	{
		return false;
	}
	if (!TestTrue(TEXT("create unique package directory"), IFileManager::Get().MakeDirectory(*PackageDirectory, true)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		// Verification loads the saved package, whose linker can keep its file open on Windows.
		// Detach only this test package, after all read-only assertions have completed.
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			ResetLoaders(Package);
		}
		// These exact paths were absent above. Never recursively clean Content or its parents.
		TestTrue(TEXT("remove test-owned package"), IFileManager::Get().Delete(*PackageFile, false));
		TestTrue(TEXT("remove empty test-owned directory"), IFileManager::Get().DeleteDirectory(*PackageDirectory, false, false));
	};
	const auto SourceText = [&Name](bool bChanged)
	{
		return FString::Printf(TEXT(R"(System(Name="%s", Root="Game") {
            Settings = { WarmupTime = %s; }
            Emitter E {
                Settings = { LocalSpace = %s; }
                EmitterUpdate = { EmitterState(); SpawnRate(SpawnRate=1.0); }
                ParticleSpawn = { SystemLocation(); }
                ParticleUpdate = { ParticleState(); SolveForcesAndVelocity(); }
                SpriteRenderer Core {}
            }
        })"), *Name, bChanged ? TEXT("0.75") : TEXT("0.25"), bChanged ? TEXT("true") : TEXT("false"));
	};
	const FString Source = Files.Write(TEXT("Existing.dfs"), SourceText(false));
	if (!TestFalse(TEXT("write initial source"), Source.IsEmpty())) { return false; }
	FGenerateOptions BuildOptions;
	BuildOptions.bSave = true;
	FDiagnosticSink BuildDiagnostics;
	const FGenerateResult Built = FGenerator::GenerateFromFile(Source, BuildOptions, BuildDiagnostics);
	if (!TestTrue(TEXT("save initial generated asset"), Built.bSucceeded) || !Built.System)
	{
		AddError(BuildDiagnostics.FormatAll());
		return false;
	}
	FGCObjectScopeGuard SystemGuard(Built.System);
	if (!TestTrue(TEXT("generated package exists on disk"), FPaths::FileExists(PackageFile))
		|| !TestEqual(TEXT("generated emitter count"), Built.System->GetEmitterHandles().Num(), 1))
	{
		return false;
	}

	// Give the file an old timestamp so an accidental save cannot hide in filesystem time granularity.
	IFileManager::Get().SetTimeStamp(*PackageFile, FDateTime(2001, 1, 1));
	const FDateTime TimestampBefore = IFileManager::Get().GetTimeStamp(*PackageFile);
	const FMD5Hash HashBefore = FMD5Hash::HashFile(*PackageFile);
	if (!TestTrue(TEXT("read saved asset hash"), HashBefore.IsValid())) { return false; }
	const float WarmupBefore = Built.System->GetWarmupTime();
	const bool bLocalSpaceBefore = Built.System->GetEmitterHandles()[0].GetEmitterData()->bLocalSpace;
	const FGuid EmitterIdBefore = Built.System->GetEmitterHandles()[0].GetId();
	const bool bDirtyBefore = Built.System->GetOutermost()->IsDirty();
	FProvenanceStamp StampBefore;
	if (!TestTrue(TEXT("initial asset has provenance"), FProvenance::Read(Built.System, StampBefore))) { return false; }
	if (!TestFalse(TEXT("edit source only"), Files.Write(TEXT("Existing.dfs"), SourceText(true)).IsEmpty())) { return false; }

	const FVerifyBatchResult Result = FDreamFXCommands::VerifySources({ Source });
	TestEqual(TEXT("existing source checked"), Result.Checked, 1);
	TestEqual(TEXT("stale asset reports drift"), Result.Drifted, 1);
	TestEqual(TEXT("stale asset fails verification"), Result.Failed, 1);
	TestFalse(TEXT("stale batch is unsuccessful"), Result.IsSuccessful());
	TestTrue(TEXT("drift diagnostic points to edited source"), Result.Diagnostics.GetDiagnostics().ContainsByPredicate(
		[&Source](const FDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("DFX7002") && FPaths::IsSamePath(Diagnostic.File, Source);
		}));
	TestTrue(TEXT("verification preserves package bytes"), FMD5Hash::HashFile(*PackageFile) == HashBefore);
	TestEqual(TEXT("verification preserves package timestamp"), IFileManager::Get().GetTimeStamp(*PackageFile), TimestampBefore);
	TestEqual(TEXT("verification preserves system setting in memory"), Built.System->GetWarmupTime(), WarmupBefore);
	TestEqual(TEXT("verification preserves emitter count"), Built.System->GetEmitterHandles().Num(), 1);
	if (Built.System->GetEmitterHandles().Num() == 1)
	{
		TestEqual(TEXT("verification preserves emitter setting in memory"),
			bool(Built.System->GetEmitterHandles()[0].GetEmitterData()->bLocalSpace), bLocalSpaceBefore);
		TestEqual(TEXT("verification preserves emitter identity"), Built.System->GetEmitterHandles()[0].GetId(), EmitterIdBefore);
	}
	TestEqual(TEXT("verification preserves package dirty state"), Built.System->GetOutermost()->IsDirty(), bDirtyBefore);
	FProvenanceStamp StampAfter;
	if (TestTrue(TEXT("verification preserves provenance"), FProvenance::Read(Built.System, StampAfter)))
	{
		TestEqual(TEXT("provenance source path unchanged"), StampAfter.SourceRelativePath, StampBefore.SourceRelativePath);
		TestEqual(TEXT("provenance full path unchanged"), StampAfter.SourceFullPath, StampBefore.SourceFullPath);
		TestEqual(TEXT("provenance source hash unchanged"), StampAfter.SourceHash, StampBefore.SourceHash);
		TestEqual(TEXT("provenance generator version unchanged"), StampAfter.GeneratorVersion, StampBefore.GeneratorVersion);
		TestTrue(TEXT("provenance dependencies unchanged"), StampAfter.ModuleDependencies == StampBefore.ModuleDependencies);
		TestTrue(TEXT("provenance module versions unchanged"), StampAfter.ModuleVersions.OrderIndependentCompareEqual(StampBefore.ModuleVersions));
	}
	return true;
}

#endif
