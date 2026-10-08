#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DirectoryWatcherModule.h"
#include "DreamFXWorkspaceTestFiles.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"
#include "Workspace/DreamFXSourceWatchSession.h"

namespace UE::DreamFX::Editor::WorkspaceFollowups
{
	struct FWatchHarness
	{
		FAutomationTestBase& Test;
		IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
		TSet<FString> Queued;
		TArray<FString> Roots;
		TUniquePtr<FSourceWatchSession> Session;
		FWatchHarness(FAutomationTestBase& InTest, const FString& Root) : Test(InTest), Roots({ Root })
		{
			if (Watcher)
			{
				Session = MakeUnique<FSourceWatchSession>(*Watcher, [this](const TSet<FString>& Files) { Queued.Append(Files); });
				Session->Refresh(Roots, false);
			}
		}
		void Pump()
		{
			Watcher->Tick(0.01f);
			Session->ProcessPendingChanges();
			Session->Refresh(Roots);
		}
		void Clear()
		{
			const double Until = FPlatformTime::Seconds() + 0.2;
			while (FPlatformTime::Seconds() < Until) { Pump(); FPlatformProcess::Sleep(0.01f); }
			Queued.Reset();
		}
		bool Wait(const TCHAR* Label, const TArray<FString>& Expected)
		{
			const double Until = FPlatformTime::Seconds() + 6.0;
			do
			{
				Pump();
				if (Expected.ContainsByPredicate([&](const FString& File) { return !Queued.Contains(File); }) == false) { return true; }
				FPlatformProcess::Sleep(0.01f);
			} while (FPlatformTime::Seconds() < Until);
			Test.AddError(FString::Printf(TEXT("%s: real DirectoryWatcher did not enqueue %s; queued %s"),
				Label, *FString::Join(Expected, TEXT(", ")), *FString::Join(Queued.Array(), TEXT(", "))));
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXExternalSourceWatchRegression,
	"DreamFX.Followups.ExternalDependencyWatcher", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXExternalSourceWatchRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::WorkspaceFollowups;
	FFiles Files;
	const FString ParentText = TEXT("System(Name=\"Base\") {Emitter E from \"SharedEmitter\" {}}");
	const FString Child = Files.Write(TEXT("DFX/Child.dfs"), TEXT("System(Name=\"Child\",Parent=\"../Shared/Base.dfs\") {}"));
	const FString Grandchild = Files.Write(TEXT("DFX/Grandchild.dfs"), TEXT("System(Name=\"Grandchild\",Parent=\"Child\") {}"));
	if (!TestFalse(TEXT("child fixture written"), Child.IsEmpty()) || Grandchild.IsEmpty()) { return false; }
	FWatchHarness Watch(*this, Files.Directory / TEXT("DFX"));
	if (!TestNotNull(TEXT("real DirectoryWatcher available"), Watch.Watcher)) { return false; }
	const TArray<FString> Expected = { Child, Grandchild };
	Watch.Clear();
	const FString Parent = Files.Write(TEXT("Shared/Base.dfs"), ParentText);
	if (!Watch.Wait(TEXT("initially missing external parent is discovered"), Expected)) { return false; }
	TestFalse(TEXT("external parent does not generate a standalone asset automatically"), Watch.Queued.Contains(Parent));
	Watch.Clear();
	// Equal-length ordinary overwrites must propagate without a rename or creation notification.
	if (!TestFalse(TEXT("modify existing external parent"), Files.Write(TEXT("Shared/Base.dfs"),
		TEXT("System(Name=\"Baze\") {Emitter E from \"SharedEmitter\" {}}")).IsEmpty())) { return false; }
	if (!Watch.Wait(TEXT("ordinary external parent modification reaches child and grandchild"), Expected)) { return false; }
	Watch.Clear();
	const FString Emitter = Files.Write(TEXT("Shared/SharedEmitter.dfe"), TEXT("Emitter(Name=\"Shared\") {}"));
	if (!Watch.Wait(TEXT("external from dependency created"), Expected)) { return false; }
	Watch.Clear();
	if (!TestTrue(TEXT("delete external dependency"), IFileManager::Get().Delete(*Emitter))) { return false; }
	if (!Watch.Wait(TEXT("external from dependency deleted"), Expected)) { return false; }
	Watch.Clear();
	Files.Write(TEXT("Shared/SharedEmitter.dfe"), TEXT("Emitter(Name=\"Shared\") {Settings={LocalSpace=true;}}"));
	if (!Watch.Wait(TEXT("deleted dependency recreated"), Expected)) { return false; }
	Watch.Clear();
	if (!TestTrue(TEXT("move external parent directory away"), Files.MoveDirectory(Files.Directory / TEXT("Shared"), Files.Directory / TEXT("Away")))) { return false; }
	if (!Watch.Wait(TEXT("removed watched directory invalidates its dependents"), Expected)) { return false; }
	Watch.Clear();
	if (!TestTrue(TEXT("restore external parent directory"), Files.MoveDirectory(Files.Directory / TEXT("Away"), Files.Directory / TEXT("Shared")))) { return false; }
	if (!Watch.Wait(TEXT("recreated external directory is watched"), Expected)) { return false; }
	Watch.Clear();
	Files.Write(TEXT("Shared/SharedEmitter.dfe"), TEXT("Emitter(Name=\"Shared\") {Settings={LocalSpace=false;}}"));
	return Watch.Wait(TEXT("restored dependency still emits real change events"), Expected);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDirectorySourceWatchRegression,
	"DreamFX.Followups.DirectoryMoveWatcher", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDirectorySourceWatchRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::WorkspaceFollowups;
	FFiles Files;
	const FString Child = Files.Write(TEXT("DFX/Child.dfs"), TEXT("System(Name=\"Child\",Parent=\"Parents/Base\") {}"));
	Files.Write(TEXT("DFX/Parents/Base.dfs"), TEXT("System(Name=\"Base\") {}"));
	Files.Write(TEXT("Incoming/New.dfs"), TEXT("System(Name=\"New\") {}"));
	const FString Root = Files.Directory / TEXT("DFX");
	FWatchHarness Watch(*this, Root);
	if (!TestNotNull(TEXT("real DirectoryWatcher available"), Watch.Watcher) || Child.IsEmpty()) { return false; }
	Watch.Clear();
	if (!TestFalse(TEXT("modify known source before any directory move"), Files.Write(TEXT("DFX/Child.dfs"),
		TEXT("System(Name=\"ChilX\",Parent=\"Parents/Base\") {}")).IsEmpty())) { return false; }
	if (!Watch.Wait(TEXT("ordinary same-length source modification enqueues the source"), { Child })) { return false; }
	Watch.Clear();
	if (!TestTrue(TEXT("rename parent directory within DFX"), Files.MoveDirectory(Root / TEXT("Parents"), Root / TEXT("Renamed")))) { return false; }
	if (!Watch.Wait(TEXT("old directory dependencies and new directory sources queued"), { Child, Root / TEXT("Renamed/Base.dfs") })) { return false; }
	Watch.Clear();
	if (!TestTrue(TEXT("move whole source directory into existing root"), Files.MoveDirectory(Files.Directory / TEXT("Incoming"), Root / TEXT("Imported")))) { return false; }
	const FString Imported = Root / TEXT("Imported/New.dfs");
	if (!Watch.Wait(TEXT("directory creation scans new descendant sources"), { Imported })) { return false; }
	Watch.Clear();
	// Both moves happen before the next refresh: only a parent subscription can observe them.
	if (!TestTrue(TEXT("move entire root away"), Files.MoveDirectory(Root, Files.Directory / TEXT("AwayDFX")))) { return false; }
	if (!TestTrue(TEXT("restore entire root before refresh"), Files.MoveDirectory(Files.Directory / TEXT("AwayDFX"), Root))) { return false; }
	if (!Watch.Wait(TEXT("root directory round trip invalidates existing sources"), { Child, Imported })) { return false; }
	Watch.Clear();
	Files.Write(TEXT("DFX/Imported/New.dfs"), TEXT("System(Name=\"New\") {Settings={Determinism=true;}}"));
	if (!Watch.Wait(TEXT("restored root still emits real file changes"), { Imported })) { return false; }
	Watch.Clear();
	// Buffer overflow is not deterministic to induce. Inject only this native rescan notification;
	// the registration, ordinary file events and directory operations above use the real backend.
	Watch.Session->HandleChanges({ FFileChangeData(Root, FFileChangeData::FCA_Unknown) });
	Watch.Session->ProcessPendingChanges();
	TestTrue(TEXT("rescan invalidates existing child"), Watch.Queued.Contains(Child));
	TestTrue(TEXT("rescan includes imported source"), Watch.Queued.Contains(Imported));
	return true;
}

#endif
