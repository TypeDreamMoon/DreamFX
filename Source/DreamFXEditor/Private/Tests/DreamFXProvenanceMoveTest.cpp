#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/FileHelper.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "Generation/DreamFXProvenance.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "SourceFiles/DreamFXPaths.h"
#include "UObject/GCObjectScopeGuard.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSourceMoveRegression,
	"DreamFX.Regression.Provenance.SourceMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSourceMoveRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	const TArray<FSourceRoot>& Roots = FDreamFXPaths::GetSourceRoots();
	if (!TestTrue(TEXT("test host has source roots"), !Roots.IsEmpty())) { return false; }
	const FString Folder = Roots[0].Directory / TEXT("DreamFXAutomation");
	for (bool bModule : { false, true })
	{
		const FString Extension = bModule ? TEXT("dfm") : TEXT("dfs");
		const FString OriginalPath = Folder / (TEXT("Original.") + Extension);
		const FString MovedPath = Folder / TEXT("Moved") / (TEXT("Renamed.") + Extension);
		const FString AssetName = TEXT("DreamFXAutomation/Move_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString AssetFile = FPackageName::LongPackageNameToFilename(TEXT("/Game/") + AssetName, FPackageName::GetAssetPackageExtension());
		if (!TestFalse(TEXT("unique fixture must not overwrite an asset"), IFileManager::Get().FileExists(*AssetFile))) { return false; }
		ON_SCOPE_EXIT { IFileManager::Get().Delete(*AssetFile, false, true); };
		const FString Text = bModule
			? FString::Printf(TEXT("Module(Name=\"%s\",Root=\"Game\") { Settings={Usage=ParticleUpdate;} Body={Particles.SpriteRotation=1.0;} }"), *AssetName)
			: FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { Emitter E {} }"), *AssetName);
		FDocument Document;
		FDiagnosticSink Diagnostics;
		if (!TestTrue(TEXT("source parses"), FParser::ParseText(Text, OriginalPath, Document, Diagnostics)))
		{
			AddError(Diagnostics.FormatAll()); return false;
		}
		FGenerateOptions Options;
		// Standalone-module lookup loads saved packages. Exercise the same path as an ordinary CLI
		// build rather than relying on an unsaved object that cannot subsequently be verified.
		Options.bSave = true;
		auto Generate = [&](bool& bSkipped) -> UObject*
		{
			Diagnostics.Reset();
			if (bModule)
			{
				const FModuleGenerateResult Result = FModuleGenerator::Generate(Document, Options, Diagnostics);
				bSkipped = Result.bSkipped;
				if (!Result.bSucceeded) { AddError(Diagnostics.FormatAll()); return nullptr; }
				return Result.Script;
			}
			const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
			bSkipped = Result.bSkipped;
			if (!Result.bSucceeded) { AddError(Diagnostics.FormatAll()); return nullptr; }
			return Result.System;
		};
		bool bSkipped = false;
		UObject* Asset = Generate(bSkipped);
		if (!TestNotNull(TEXT("original asset generated"), Asset)) { return false; }
		FGCObjectScopeGuard AssetGuard(Asset);
		FProvenanceStamp OriginalStamp;
		TestTrue(TEXT("original provenance recorded"), FProvenance::Read(Asset, OriginalStamp));
		TestEqual(TEXT("original absolute path recorded"), OriginalStamp.SourceFullPath, OriginalPath);
		TestTrue(TEXT("same location is current"), FProvenance::IsSourceLocationCurrent(Asset, OriginalPath));
		Document.SourceFilePath = MovedPath;
		TestFalse(TEXT("moved source is not current"), FProvenance::IsSourceLocationCurrent(Asset, MovedPath));

		Options.bVerifyOnly = true;
		const FMD5Hash BeforeVerify = FMD5Hash::HashFile(*AssetFile);
		if (!Generate(bSkipped)) { return false; }
		FProvenanceStamp VerifyStamp;
		FProvenance::Read(Asset, VerifyStamp);
		TestEqual(TEXT("verify does not rewrite provenance"), VerifyStamp.SourceFullPath, OriginalPath);
		TestTrue(TEXT("verify leaves saved asset bytes unchanged"), FMD5Hash::HashFile(*AssetFile) == BeforeVerify);

		Options.bVerifyOnly = false;
		TestTrue(TEXT("move rebuild retains asset identity"), Generate(bSkipped) == Asset);
		TestFalse(TEXT("unchanged text at a new location must not skip"), bSkipped);
		FProvenanceStamp MovedStamp;
		TestTrue(TEXT("moved provenance recorded"), FProvenance::Read(Asset, MovedStamp));
		TestEqual(TEXT("absolute path refreshed"), MovedStamp.SourceFullPath, MovedPath);
		TestTrue(TEXT("ordinary build persists the new source path"), FMD5Hash::HashFile(*AssetFile) != BeforeVerify);
		TestTrue(TEXT("relative path refreshed"), MovedStamp.SourceRelativePath.EndsWith(TEXT("Moved/Renamed.") + Extension));
		TestEqual(TEXT("source contents remain identical"), MovedStamp.SourceHash, OriginalStamp.SourceHash);
		TestTrue(TEXT("following unchanged build keeps asset"), Generate(bSkipped) == Asset);
		TestTrue(TEXT("following unchanged build skips"), bSkipped);

		// Another checkout of the same tree: same root, same root-relative path, different absolute
		// path. It is the same source, so it must neither rebuild nor lose its way to the file.
		TestFalse(TEXT("owning root recorded"), MovedStamp.SourceRoot.IsEmpty());
		if (!TestTrue(TEXT("moved source written for resolution"), FFileHelper::SaveStringToFile(Text, *MovedPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) { return false; }
		ON_SCOPE_EXIT { IFileManager::Get().Delete(*MovedPath, false, true); IFileManager::Get().DeleteDirectory(*FPaths::GetPath(MovedPath)); IFileManager::Get().DeleteDirectory(*Folder); };
		FProvenanceStamp OtherCheckout = MovedStamp;
		OtherCheckout.SourceFullPath = TEXT("Z:/AnotherCheckout/DFX/DreamFXAutomation/Moved/Renamed.") + Extension;
		FProvenance::Write(Asset, OtherCheckout);
		TestTrue(TEXT("another checkout of the same source is current"), FProvenance::IsSourceLocationCurrent(Asset, MovedPath));
		TestTrue(TEXT("another checkout keeps asset"), Generate(bSkipped) == Asset);
		TestTrue(TEXT("another checkout's unchanged build skips"), bSkipped);
		FString Resolved;
		TestTrue(TEXT("a foreign absolute path resolves through root and relative path"),
			FProvenance::ResolveSourceFile(OtherCheckout, Resolved) && FPaths::IsSamePath(Resolved, MovedPath));
		FProvenanceStamp OtherRoot = OtherCheckout;
		OtherRoot.SourceRoot = TEXT("Plugin.DreamFXNoSuchRoot");
		FProvenance::Write(Asset, OtherRoot);
		TestFalse(TEXT("the same relative path under another root is a different source"), FProvenance::IsSourceLocationCurrent(Asset, MovedPath));
	}
	return true;
}

#endif
