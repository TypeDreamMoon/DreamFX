#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"
#include "UObject/UnrealType.h"
#include "WriteBack/DreamFXPull.h"

/**
 * Write-back behaviour that only shows end to end: the safety gate's refusal in an editor session,
 * the fixed-bounds flag across a round trip, and the encoding a pull writes. The gate's rules
 * themselves are covered on synthetic fact sets by DreamFX.Corpus.WriteBack.
 */
namespace UE::DreamFX::Editor::WriteBackIntegration
{
	struct FAsset
	{
		FString Name;
		FString PackageName;
		FString ObjectPath;
		FString PackageFile;

		explicit FAsset(const TCHAR* Stem)
		{
			Name = FString::Printf(TEXT("DreamFXAutomation/%s_%s"), Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			PackageName = TEXT("/Game/") + Name;
			ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
			PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		}

		~FAsset()
		{
			IFileManager::Get().Delete(*PackageFile, false, true, true);
			const FString DumpBase = FPaths::ProjectSavedDir() / TEXT("DreamFX/BuildSafety") / FPackageName::GetShortName(PackageName);
			for (const TCHAR* Suffix : { TEXT(".before.facts"), TEXT(".after.facts"), TEXT(".lost.facts") })
			{
				IFileManager::Get().Delete(*(DumpBase + Suffix), false, true, true);
			}
		}
	};

	bool Build(FAutomationTestBase& Test, const FString& Text, const FGenerateOptions& Options,
		FGenerateResult& OutResult, FDiagnosticSink& Diagnostics, bool bExpectSuccess = true)
	{
		FDocument Document;
		if (!FParser::ParseText(Text, FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation/WriteBack.dfs"), Document, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll());
			return false;
		}
		OutResult = FGenerator::Generate(Document, Options, Diagnostics);
		if (OutResult.bSucceeded != bExpectSuccess)
		{
			Test.AddError(FString::Printf(TEXT("Build %s unexpectedly:\n%s"),
				OutResult.bSucceeded ? TEXT("succeeded") : TEXT("failed"), *Diagnostics.FormatAll()));
			return false;
		}
		return true;
	}

	UNiagaraRendererProperties* FirstRenderer(UNiagaraSystem* System)
	{
		if (System == nullptr || System->GetEmitterHandles().IsEmpty())
		{
			return nullptr;
		}
		const FVersionedNiagaraEmitterData* Data = System->GetEmitterHandles()[0].GetEmitterData();
		return Data != nullptr && !Data->GetRenderers().IsEmpty() ? Data->GetRenderers()[0] : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXGateRefusalRestoresAsset,
	"DreamFX.Regression.BuildSafety.RefusalRestoresAsset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXGateRefusalRestoresAsset::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::WriteBackIntegration;

	const FAsset Asset(TEXT("NS_GateRefusal"));
	const FString Text = FString::Printf(
		TEXT("System(Name=\"%s\",Root=\"Game\"){Emitter E {ParticleSpawn={Particles.SpriteRotation=0.5;} SpriteRenderer R {}}}"),
		*Asset.Name);

	FGenerateOptions Options;
	Options.bSave = true;
	Options.bForce = true;
	FDiagnosticSink Diagnostics;
	FGenerateResult Built;
	if (!Build(*this, Text, Options, Built, Diagnostics)) { return false; }

	// What an editor slider leaves behind: a renderer value the text does not declare, saved.
	UNiagaraRendererProperties* Tuned = FirstRenderer(Built.System);
	if (!TestNotNull(TEXT("sprite renderer built"), Tuned)) { return false; }
	Tuned->Modify();
	Tuned->SortOrderHint = 7;
	TArray<FString> Errors;
	if (!TestTrue(TEXT("tuned asset saved"), FNiagaraAdapter::SaveSystem(Built.System, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	const FMD5Hash SavedBytes = FMD5Hash::HashFile(*Asset.PackageFile);

	// The same text again. The rebuild recreates the renderer, so the tuned value would be lost. The
	// gate logs the refusal at error severity, which is the behaviour under test.
	AddExpectedError(TEXT("build safety: '.*NS_GateRefusal_"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("lost \\| emitter E renderer 0:"), EAutomationExpectedErrorFlags::Contains, 0);
	Diagnostics.Reset();
	FGenerateResult Refused;
	if (!Build(*this, Text, Options, Refused, Diagnostics, /*bExpectSuccess=*/false)) { return false; }
	TestTrue(TEXT("the refusal names the safety gate"), Diagnostics.FormatAll().Contains(TEXT("DFX8018")));
	TestTrue(TEXT("nothing reached the disk"), FMD5Hash::HashFile(*Asset.PackageFile) == SavedBytes);

	// The session must hold the saved asset again: Save All or autosave would otherwise write the
	// refused rebuild, and an ordinary build would skip it as up to date.
	UNiagaraSystem* Current = FindObject<UNiagaraSystem>(nullptr, *Asset.ObjectPath);
	UNiagaraRendererProperties* Kept = FirstRenderer(Current);
	TestTrue(TEXT("the session holds the tuned value again"), Kept != nullptr && Kept->SortOrderHint == 7);
	TestFalse(TEXT("nothing is left for Save All"), Current == nullptr || Current->GetOutermost()->IsDirty());

	// -Force on a command line is the explicit override, and it applies the text.
	Diagnostics.Reset();
	Options.bForceLossyRebuild = true;
	FGenerateResult Forced;
	if (!Build(*this, Text, Options, Forced, Diagnostics)) { return false; }
	UNiagaraRendererProperties* Reset = FirstRenderer(Forced.System);
	TestTrue(TEXT("a forced rebuild applies the text"), Reset != nullptr && Reset->SortOrderHint == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXFixedBoundsFlagRoundTrip,
	"DreamFX.Regression.Settings.UseFixedBounds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXFixedBoundsFlagRoundTrip::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::WriteBackIntegration;

	const FBoolProperty* Flag = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bFixedBounds"));
	if (!TestNotNull(TEXT("bFixedBounds is reflected"), Flag)) { return false; }

	struct FCase
	{
		const TCHAR* Settings;
		bool bFlag;
		bool bExportsFlag;
	};
	// A declared box turns the override on; an explicit flag wins; the flag alone needs no box. The
	// export spells the flag whenever it is on, and when it is off beside a box it would otherwise imply.
	const FCase Cases[] = {
		{ TEXT("FixedBounds = box(-50, -50, -50, 50, 50, 50);"), true, true },
		{ TEXT("FixedBounds = box(-50, -50, -50, 50, 50, 50); UseFixedBounds = false;"), false, true },
		{ TEXT("UseFixedBounds = true;"), true, true },
	};
	for (const FCase& Case : Cases)
	{
		const FAsset Asset(TEXT("NS_FixedBounds"));
		const FString Text = FString::Printf(
			TEXT("System(Name=\"%s\",Root=\"Game\"){Settings={%s} Emitter E {ParticleSpawn={Particles.SpriteRotation=0.5;} SpriteRenderer R {}}}"),
			*Asset.Name, Case.Settings);
		FGenerateOptions Options;
		Options.bSave = false;
		Options.bForce = true;
		FDiagnosticSink Diagnostics;
		FGenerateResult Built;
		if (!Build(*this, Text, Options, Built, Diagnostics)) { return false; }
		TestEqual(FString::Printf(TEXT("%s: built flag"), Case.Settings),
			Flag->GetPropertyValue_InContainer(Built.System), Case.bFlag);

		FDiagnosticSink ExportDiagnostics;
		const FDecompileResult Export = FDecompiler::Decompile(Built.System, FString(), ExportDiagnostics);
		if (!TestTrue(TEXT("system exports"), Export.bSucceeded)) { AddError(ExportDiagnostics.FormatAll()); return false; }
		TestEqual(FString::Printf(TEXT("%s: export spells the flag"), Case.Settings),
			Export.Source.Contains(TEXT("UseFixedBounds")), Case.bExportsFlag);
		if (Case.bExportsFlag)
		{
			TestTrue(FString::Printf(TEXT("%s: exported flag value"), Case.Settings),
				Export.Source.Contains(Case.bFlag ? TEXT("UseFixedBounds = true;") : TEXT("UseFixedBounds = false;")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXPullKeepsEncoding,
	"DreamFX.Regression.Pull.KeepsUtf8",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXPullKeepsEncoding::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::WriteBackIntegration;

	const FAsset Asset(TEXT("NS_PullEncoding"));
	// LocalSpace is stored as bLocalSpace, and the box's IsValid is a bool on the asset and 1 in the
	// lowered text: an unchanged box must not be rewritten, and a renamed setting must still be found.
	auto Source = [&Asset](int32 Seed, bool bLocalSpace)
	{
		return FString::Printf(
			TEXT("// 粒子：%s\nSystem(Name=\"%s\",Root=\"Game\")\n{\n    Emitter E\n    {\n        Settings = { RandomSeed = %d; LocalSpace = %s; FixedBounds = box(-150, -150, -150, 150, 150, 150); }\n        ParticleSpawn = { Particles.SpriteRotation = 0.5; }\n        SpriteRenderer R {}\n    }\n}\n"),
			TEXT("中文注释"), *Asset.Name, Seed, bLocalSpace ? TEXT("true") : TEXT("false"));
	};

	FGenerateOptions Options;
	Options.bSave = true;
	Options.bForce = true;
	FDiagnosticSink Diagnostics;
	FGenerateResult Built;
	if (!Build(*this, Source(3, true), Options, Built, Diagnostics)) { return false; }

	// The text drifts from the asset, and carries text outside ASCII.
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString File = FPaths::ConvertRelativePathToFull(Directory / TEXT("PullEncoding.dfs"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };
	if (!TestTrue(TEXT("source written"), FFileHelper::SaveStringToFile(Source(5, false), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
	{
		return false;
	}

	FPullOptions PullOptions;
	PullOptions.bApply = true;
	PullOptions.bUseBaseline = false;
	FDiagnosticSink PullDiagnostics;
	const FPullResult Result = FPuller::PullFile(File, PullOptions, PullDiagnostics);
	ON_SCOPE_EXIT
	{
		for (const FString& Written : { Result.BackupPath, Result.BaselinePath, Result.ReportPath })
		{
			if (!Written.IsEmpty()) { IFileManager::Get().Delete(*Written, false, true, true); }
		}
	};
	if (!TestTrue(TEXT("pull ran"), Result.bSucceeded && Result.bWroteFile)) { AddError(PullDiagnostics.FormatAll()); return false; }

	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *File);
	const bool bUtf16 = Bytes.Num() >= 2 && ((Bytes[0] == 0xFF && Bytes[1] == 0xFE) || (Bytes[0] == 0xFE && Bytes[1] == 0xFF));
	const bool bUtf8Bom = Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF;
	TestFalse(TEXT("pull keeps UTF-8, not UTF-16"), bUtf16);
	TestFalse(TEXT("pull adds no BOM the file did not have"), bUtf8Bom);
	FString Rewritten;
	FFileHelper::LoadFileToString(Rewritten, *File);
	TestEqual(TEXT("only the drifted literals changed"), Rewritten, Source(3, true));
	TestEqual(TEXT("exactly the two drifted values were written"), Result.Changed, 2);

	TArray<uint8> Backup;
	const FTCHARToUTF8 Original(*Source(5, false));
	TestTrue(TEXT("the backup is the file's exact bytes"), FFileHelper::LoadFileToArray(Backup, *Result.BackupPath)
		&& Backup == TArray<uint8>(reinterpret_cast<const uint8*>(Original.Get()), Original.Length()));
	return true;
}

#endif
