#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DirectoryWatcherModule.h"
#include "DreamFXWorkspaceTestFiles.h"
#include "Generation/DreamFXGenerator.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "NiagaraSystem.h"
#include "UI/DreamFXAssetCommands.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Workspace/DreamFXOwnWriteLedger.h"
#include "Workspace/DreamFXPullOnSave.h"
#include "Workspace/DreamFXSourceWatchSession.h"
#include "Workspace/DreamFXSourceWatcher.h"

/**
 * The editor side of write-back: the rules that keep the editor from answering its own writes. The
 * pull itself is covered by DreamFX.Corpus.WriteBack; what is checked here is who is allowed to start
 * one and what the source watcher does with the file it leaves behind.
 */
namespace UE::DreamFX::Editor::PullUiTests
{
	/** A generated asset under /Game/DreamFXAutomation and the .dfs it is built from, both removed on exit. */
	struct FBuiltAsset
	{
		FString Name;
		FString PackageName;
		FString PackageFile;
		FString Directory;
		FString SourceFile;
		TArray<FString> Leftovers;

		explicit FBuiltAsset(const TCHAR* Stem)
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Name = FString::Printf(TEXT("DreamFXAutomation/%s_%s"), Stem, *Id);
			PackageName = TEXT("/Game/") + Name;
			PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation") / Id);
			SourceFile = Directory / TEXT("Source.dfs");
		}

		~FBuiltAsset()
		{
			IFileManager::Get().Delete(*PackageFile, false, true, true);
			for (const FString& File : Leftovers)
			{
				if (!File.IsEmpty()) { IFileManager::Get().Delete(*File, false, true, true); }
			}
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}

		FString Text(const int32 Seed) const
		{
			return FString::Printf(
				TEXT("// 粒子\nSystem(Name=\"%s\",Root=\"Game\")\n{\n    Emitter E\n    {\n        Settings = { RandomSeed = %d; }\n        ParticleSpawn = { Particles.SpriteRotation = 0.5; }\n        SpriteRenderer R {}\n    }\n}\n"),
				*Name, Seed);
		}

		bool WriteSource(const int32 Seed) const
		{
			return FFileHelper::SaveStringToFile(Text(Seed), *SourceFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	};

	bool BuildFromSource(FAutomationTestBase& Test, const FBuiltAsset& Asset, FGenerateResult& OutResult)
	{
		FGenerateOptions Options;
		Options.bSave = true;
		Options.bForce = true;
		FDiagnosticSink Diagnostics;
		OutResult = FGenerator::GenerateFromFile(Asset.SourceFile, Options, Diagnostics);
		if (!OutResult.bSucceeded || OutResult.System == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("Build failed:\n%s"), *Diagnostics.FormatAll()));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXOwnWriteLedgerRules,
	"DreamFX.Followups.PullUi.OwnWriteLedger",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXOwnWriteLedgerRules::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;

	// The file's content is a string here: the ledger only ever compares hashes.
	FString OnDisk;
	int32 Reads = 0;
	auto HashNow = [&](FString& OutHash)
	{
		++Reads;
		OutHash = OnDisk;
		return !OnDisk.IsEmpty();
	};

	FOwnWriteLedger Ledger;
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation/Ledger.dfs"));

	// A file nobody declared costs nothing -- not even a read of the file.
	OnDisk = TEXT("h1");
	TestFalse(TEXT("an undeclared file is not an own write"), Ledger.IsOwnWrite(File, 0.0, HashNow));
	TestEqual(TEXT("an undeclared file is not even hashed"), Reads, 0);

	// One declaration swallows every notification for that write, not just the first: the OS reports a
	// write more than once and the watcher batches by tick, so the repeats can land in different batches.
	Ledger.Expect(File, TEXT("h1"), 100.0, 10.0);
	TestTrue(TEXT("the first notification is skipped"), Ledger.IsOwnWrite(File, 100.1, HashNow));
	TestTrue(TEXT("a second notification for the same write is skipped too"), Ledger.IsOwnWrite(File, 100.9, HashNow));
	TestTrue(TEXT("and a third, in a later batch"), Ledger.IsOwnWrite(File, 103.0, HashNow));

	// The author saves again: the content is not what was written, so this one rebuilds -- and the
	// declaration is over, so a repeat of that save does too.
	OnDisk = TEXT("h2");
	TestFalse(TEXT("different content is the author's edit"), Ledger.IsOwnWrite(File, 103.5, HashNow));
	OnDisk = TEXT("h1");
	TestFalse(TEXT("the declaration ended with the edit, so it does not come back"), Ledger.IsOwnWrite(File, 103.6, HashNow));

	// No notification ever arrives: the declaration goes away by itself and cannot excuse a later save
	// of the same bytes forever.
	Ledger.Expect(File, TEXT("h1"), 200.0, 10.0);
	TestEqual(TEXT("one live entry"), Ledger.Num(205.0), 1);
	TestEqual(TEXT("it expires without a notification"), Ledger.Num(210.5), 0);
	TestFalse(TEXT("an expired declaration excuses nothing"), Ledger.IsOwnWrite(File, 211.0, HashNow));

	// A file that cannot be read any more is not the content that was written.
	Ledger.Expect(File, TEXT("h1"), 300.0, 10.0);
	OnDisk.Reset();
	TestFalse(TEXT("an unreadable file is not an own write"), Ledger.IsOwnWrite(File, 300.5, HashNow));
	OnDisk = TEXT("h1");
	TestFalse(TEXT("and the declaration is gone"), Ledger.IsOwnWrite(File, 300.6, HashNow));

	// Declaring again replaces the entry: the second write, not the first, is the one on disk.
	Ledger.Expect(File, TEXT("old"), 400.0, 10.0);
	Ledger.Expect(File, TEXT("h1"), 401.0, 10.0);
	TestTrue(TEXT("the latest declaration wins"), Ledger.IsOwnWrite(File, 401.5, HashNow));
	TestEqual(TEXT("one entry per file"), Ledger.Num(401.5), 1);

	// The watcher and the writer spell one file differently.
	FOwnWriteLedger Spellings;
	Spellings.Expect(File, TEXT("h1"), 0.0, 10.0);
	FString Backslashes = File;
	Backslashes.ReplaceInline(TEXT("/"), TEXT("\\"));
	TestTrue(TEXT("slash direction does not matter"), Spellings.IsOwnWrite(Backslashes, 1.0, HashNow));
	TestTrue(TEXT("case does not matter on this platform"), Spellings.IsOwnWrite(File.ToUpper(), 1.0, HashNow));

	// Hashing a real file agrees with hashing the same bytes.
	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".txt"));
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Scratch, false, true, true); };
	if (TestTrue(TEXT("scratch written"), FFileHelper::SaveStringToFile(TEXT("abc"), *Scratch,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Scratch);
		FString Hash;
		TestTrue(TEXT("file hashed"), FOwnWriteLedger::HashFile(Scratch, Hash));
		TestEqual(TEXT("file and bytes hash alike"), Hash, FOwnWriteLedger::HashBytes(Bytes));
	}
	FString Missing;
	TestFalse(TEXT("a missing file has no hash"), FOwnWriteLedger::HashFile(Scratch + TEXT(".missing"), Missing));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXPullOnSaveDecision,
	"DreamFX.Followups.PullUi.SaveDecision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXPullOnSaveDecision::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;

	FPullOnSaveFacts Base;
	Base.bSettingOn = true;
	TestEqual(TEXT("an author's save with the setting on pulls"), FPullOnSave::Decide(Base), EPullOnSaveSkip::None);

	auto With = [&Base](TFunctionRef<void(FPullOnSaveFacts&)> Change)
	{
		FPullOnSaveFacts Facts = Base;
		Change(Facts);
		return FPullOnSave::Decide(Facts);
	};

	TestEqual(TEXT("setting off"), With([](FPullOnSaveFacts& F) { F.bSettingOn = false; }), EPullOnSaveSkip::SettingOff);
	TestEqual(TEXT("DreamFX's own save"), With([](FPullOnSaveFacts& F) { F.bSelfSave = true; }), EPullOnSaveSkip::SelfSave);
	TestEqual(TEXT("commandlet"), With([](FPullOnSaveFacts& F) { F.bCommandlet = true; }), EPullOnSaveSkip::Commandlet);
	TestEqual(TEXT("automation run"), With([](FPullOnSaveFacts& F) { F.bAutomation = true; }), EPullOnSaveSkip::Automation);
	TestEqual(TEXT("cook"), With([](FPullOnSaveFacts& F) { F.bProceduralSave = true; }), EPullOnSaveSkip::ProceduralSave);
	TestEqual(TEXT("autosave"), With([](FPullOnSaveFacts& F) { F.bAutoSave = true; }), EPullOnSaveSkip::AutoSave);
	TestEqual(TEXT("play session"), With([](FPullOnSaveFacts& F) { F.bPlaySession = true; }), EPullOnSaveSkip::PlaySession);

	// A test that drives the hook does not need the setting or to dodge the automation rule -- but the
	// self-save rule is the one under test and nothing may bypass it.
	TestEqual(TEXT("driven by a test, no setting needed"), With([](FPullOnSaveFacts& F)
		{ F.bSettingOn = false; F.bAutomation = true; F.bDrivenByTest = true; }), EPullOnSaveSkip::None);
	TestEqual(TEXT("driven by a test, a self save is still refused"), With([](FPullOnSaveFacts& F)
		{ F.bDrivenByTest = true; F.bSelfSave = true; }), EPullOnSaveSkip::SelfSave);
	TestEqual(TEXT("driven by a test, a cook is still refused"), With([](FPullOnSaveFacts& F)
		{ F.bDrivenByTest = true; F.bProceduralSave = true; }), EPullOnSaveSkip::ProceduralSave);

	// The guard nests: a save inside a save leaves the outer one open.
	TestFalse(TEXT("not saving"), FPullOnSave::IsSelfSaving());
	{
		const FPullOnSave::FSelfSaveScope Outer;
		{
			const FPullOnSave::FSelfSaveScope Inner;
			TestTrue(TEXT("inside"), FPullOnSave::IsSelfSaving());
		}
		TestTrue(TEXT("still inside after the inner scope"), FPullOnSave::IsSelfSaving());
	}
	TestFalse(TEXT("closed again"), FPullOnSave::IsSelfSaving());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBuildSaveDoesNotPull,
	"DreamFX.Followups.PullUi.BuildSaveDoesNotPull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBuildSaveDoesNotPull::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::PullUiTests;

	FBuiltAsset Asset(TEXT("NS_PullOnSave"));
	if (!TestTrue(TEXT("source written"), (IFileManager::Get().MakeDirectory(*Asset.Directory, true), Asset.WriteSource(3)))) { return false; }

	// What the hook would have scheduled, and how many times the engine really announced this package
	// as saved -- without the second number a hook that never fired would pass the first.
	TArray<FString> Scheduled;
	int32 SavesAnnounced = 0;
	FPullOnSave::FTestScope Scope([&Scheduled](UNiagaraSystem*, const FString& SourceFile) { Scheduled.Add(SourceFile); });
	const FDelegateHandle Counter = UPackage::PackageSavedWithContextEvent.AddLambda(
		[&](const FString&, UPackage* Package, FObjectPostSaveContext)
		{
			if (Package != nullptr && Package->GetName() == Asset.PackageName) { ++SavesAnnounced; }
		});
	ON_SCOPE_EXIT { UPackage::PackageSavedWithContextEvent.Remove(Counter); };

	FGenerateResult Built;
	if (!BuildFromSource(*this, Asset, Built)) { return false; }
	TestTrue(TEXT("the build saved the asset and the engine said so"), SavesAnnounced >= 1);
	TestEqual(TEXT("a DreamFX build's own save starts no pull"), Scheduled.Num(), 0);

	// An incremental rebuild saves through the same path.
	{
		Asset.WriteSource(4);
		FGenerateOptions Options;
		Options.bSave = true;
		Options.bIncremental = true;
		FDiagnosticSink Diagnostics;
		const int32 Before = SavesAnnounced;
		const FGenerateResult Again = FGenerator::GenerateFromFile(Asset.SourceFile, Options, Diagnostics);
		if (!TestTrue(TEXT("incremental rebuild"), Again.bSucceeded)) { AddError(Diagnostics.FormatAll()); return false; }
		TestTrue(TEXT("the rebuild saved the asset"), SavesAnnounced > Before);
		TestEqual(TEXT("an incremental rebuild's save starts no pull"), Scheduled.Num(), 0);
	}

	// The same asset saved by anyone else -- the editor's Save, which is what the setting is for.
	UNiagaraSystem* System = Built.System;
	UPackage* Package = System->GetOutermost();
	Package->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GWarn;
	const int32 BeforeUserSave = SavesAnnounced;
	if (!TestTrue(TEXT("the author's save succeeds"), UPackage::SavePackage(Package, System, *Asset.PackageFile, SaveArgs))) { return false; }
	TestTrue(TEXT("the engine announced it"), SavesAnnounced > BeforeUserSave);
	if (TestEqual(TEXT("the author's save starts a pull"), Scheduled.Num(), 1))
	{
		TestTrue(TEXT("for the source the asset was built from"), FPaths::IsSamePath(Scheduled[0], Asset.SourceFile));
	}

	// And a source that is not a .dfs, or is gone, is nothing to pull into.
	Scheduled.Reset();
	IFileManager::Get().Delete(*Asset.SourceFile, false, true, true);
	Package->MarkPackageDirty();
	UPackage::SavePackage(Package, System, *Asset.PackageFile, SaveArgs);
	TestEqual(TEXT("a save whose source is gone starts no pull"), Scheduled.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXPullCommandTellsTheWatcher,
	"DreamFX.Followups.PullUi.CommandTellsTheWatcher",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXPullCommandTellsTheWatcher::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::PullUiTests;

	FBuiltAsset Asset(TEXT("NS_PullCommand"));
	if (!TestTrue(TEXT("source written"), (IFileManager::Get().MakeDirectory(*Asset.Directory, true), Asset.WriteSource(3)))) { return false; }
	FGenerateResult Built;
	if (!BuildFromSource(*this, Asset, Built)) { return false; }

	// The text moves on without the asset -- a hand edit the asset has not been rebuilt from.
	if (!TestTrue(TEXT("drifted source written"), Asset.WriteSource(9))) { return false; }
	TestFalse(TEXT("nothing has been declared yet"), FSourceWatcher::IsOwnWrite(Asset.SourceFile));

	// The first pull has no baseline, so it compares every declared value and the asset's win.
	const FPullIntoSourceResult Pulled = FDreamFXCommands::PullAssetIntoSource(Built.System, /*bStructure=*/false);
	Asset.Leftovers = { Pulled.Pull.BackupPath, Pulled.Pull.BaselinePath, Pulled.Pull.ReportPath };
	if (!TestTrue(TEXT("the pull ran"), Pulled.bAttempted && Pulled.bSucceeded)) { AddError(Pulled.Diagnostics.FormatAll()); return false; }
	TestTrue(TEXT("and wrote the file"), Pulled.Pull.bWroteFile);

	FString Rewritten;
	FFileHelper::LoadFileToString(Rewritten, *Asset.SourceFile);
	TestEqual(TEXT("the asset's value went back, the comment and layout stayed"), Rewritten, Asset.Text(3));
	TestFalse(TEXT("the pull reports a report file"), Pulled.Pull.ReportPath.IsEmpty());

	// The watcher was told, about exactly these bytes.
	TestTrue(TEXT("the write is declared to the watcher"), FSourceWatcher::IsOwnWrite(Asset.SourceFile));
	TestTrue(TEXT("and stays declared for repeated notifications"), FSourceWatcher::IsOwnWrite(Asset.SourceFile));
	if (!TestTrue(TEXT("author edits the file again"), Asset.WriteSource(11))) { return false; }
	TestFalse(TEXT("an edit after the write is not excused"), FSourceWatcher::IsOwnWrite(Asset.SourceFile));

	// Nothing to write, nothing to declare.
	if (!TestTrue(TEXT("source matches the asset again"), Asset.WriteSource(3))) { return false; }
	const FPullIntoSourceResult Quiet = FDreamFXCommands::PullAssetIntoSource(Built.System, false);
	TestTrue(TEXT("a pull with nothing to write succeeds"), Quiet.bAttempted && Quiet.bSucceeded);
	TestFalse(TEXT("and writes nothing"), Quiet.Pull.bWroteFile);
	TestFalse(TEXT("and declares nothing"), FSourceWatcher::IsOwnWrite(Asset.SourceFile));

	// Not every asset has a .dfs to write into.
	const FPullIntoSourceResult NotASystem = FDreamFXCommands::PullAssetIntoSource(GetTransientPackage(), false);
	TestFalse(TEXT("something that is not a system is not attempted"), NotASystem.bAttempted);
	TestFalse(TEXT("and says why"), NotASystem.Message.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXWatchSessionSkipsOwnWrite,
	"DreamFX.Followups.PullUi.WatcherSkipsOwnWrite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXWatchSessionSkipsOwnWrite::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::WorkspaceFollowups;

	IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
	if (!TestNotNull(TEXT("real DirectoryWatcher available"), Watcher)) { return false; }

	FFiles Files;
	const FString Base = Files.Write(TEXT("DFX/Base.dfs"), TEXT("System(Name=\"Base\") {}"));
	const FString Child = Files.Write(TEXT("DFX/Child.dfs"), TEXT("System(Name=\"Child\",Parent=\"Base\") {}"));
	const FString Other = Files.Write(TEXT("DFX/Other.dfs"), TEXT("System(Name=\"Other\") {}"));
	if (Base.IsEmpty() || Child.IsEmpty() || Other.IsEmpty()) { return TestTrue(TEXT("fixtures written"), false); }

	TSet<FString> Queued;
	FOwnWriteLedger Ledger;
	FSourceWatchSession Session(*Watcher, [&Queued](const TSet<FString>& Sources) { Queued.Append(Sources); });
	Session.SetOwnWriteFilter([&Ledger](const FString& File)
	{
		return Ledger.IsOwnWrite(File, FPlatformTime::Seconds(), [&File](FString& Hash) { return FOwnWriteLedger::HashFile(File, Hash); });
	});
	Session.Refresh({ Files.Directory / TEXT("DFX") }, /*bQueueNewSources=*/false);

	// The engine's directory watcher is never ticked here, so the only notifications are the ones this
	// test injects -- the file writes below cannot add their own.
	auto Notify = [&](const FString& File)
	{
		Queued.Reset();
		Session.HandleChanges({ FFileChangeData(File, FFileChangeData::FCA_Modified) });
		Session.ProcessPendingChanges();
	};
	auto DeclareCurrentContent = [&](const FString& File)
	{
		FString Hash;
		FOwnWriteLedger::HashFile(File, Hash);
		Ledger.Expect(File, Hash, FPlatformTime::Seconds());
	};

	// Control: with nothing declared a changed file rebuilds itself and the file that inherits from it.
	Notify(Base);
	TestTrue(TEXT("an ordinary change queues the file"), Queued.Contains(Base));
	TestTrue(TEXT("and its dependent"), Queued.Contains(Child));

	// Write-back rewrote Base: it does not rebuild itself, but Child inherits from it and does.
	Files.Write(TEXT("DFX/Base.dfs"), TEXT("System(Name=\"Base\") { Emitter E {} }"));
	DeclareCurrentContent(Base);
	Notify(Base);
	TestFalse(TEXT("the file the editor wrote is not queued"), Queued.Contains(Base));
	TestTrue(TEXT("what inherits from it still is"), Queued.Contains(Child));

	// A second notification for the same write, in a later batch, is skipped too.
	Notify(Base);
	TestFalse(TEXT("a repeated notification for the same write is skipped"), Queued.Contains(Base));

	// The author edits it again: that is a save like any other.
	Files.Write(TEXT("DFX/Base.dfs"), TEXT("System(Name=\"Base\") { Emitter F {} }"));
	Notify(Base);
	TestTrue(TEXT("an edit after the write queues the file"), Queued.Contains(Base));
	TestTrue(TEXT("and its dependent"), Queued.Contains(Child));

	// Declaring one file excuses only that file.
	Files.Write(TEXT("DFX/Other.dfs"), TEXT("System(Name=\"Other\") { Emitter G {} }"));
	DeclareCurrentContent(Base);
	Notify(Other);
	TestTrue(TEXT("a declaration for one file does not excuse another"), Queued.Contains(Other));

	// A directory event covering a declared file is not a write by us: a move or a rescan says nothing
	// about the content, so the file is queued like any other.
	DeclareCurrentContent(Base);
	Queued.Reset();
	Session.HandleChanges({
		FFileChangeData(Base, FFileChangeData::FCA_Modified),
		FFileChangeData(Files.Directory / TEXT("DFX"), FFileChangeData::FCA_Unknown) });
	Session.ProcessPendingChanges();
	TestTrue(TEXT("a rescan queues a declared file anyway"), Queued.Contains(Base));
	return true;
}

#endif
