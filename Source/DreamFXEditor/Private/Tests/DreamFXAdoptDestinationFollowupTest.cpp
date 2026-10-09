#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DreamFXWorkspaceTestFiles.h"
#include "Misc/ScopeExit.h"
#include "NiagaraSystem.h"
#include "UI/DreamFXAssetCommands.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXAdoptDestinationRegression,
	"DreamFX.Followups.AdoptPreservesExistingSource", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXAdoptDestinationRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::WorkspaceFollowups;
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DFX"));
	const bool bRootExisted = IFileManager::Get().DirectoryExists(*SourceRoot);
	ON_SCOPE_EXIT { if (!bRootExisted) { IFileManager::Get().DeleteDirectory(*SourceRoot, false, false); } };
	FFiles Files;
	const FString Folder = TEXT("DreamFXAdopt_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Files.Directory = SourceRoot / Folder;
	const FString PackagePath = TEXT("/Game/") + Folder + TEXT("/NS_Adopt");
	UPackage* Package = CreatePackage(*PackagePath);
	// No Niagara graph is needed: the public command must reject the destination before decompiling.
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, TEXT("NS_Adopt"), RF_Public | RF_Standalone);
	FGCObjectScopeGuard SystemGuard(System);
	const TArray<FString> ExistingSources = {
		TEXT("System(Name=\"DifferentAsset\",Root=\"Game\") {}\n"),
		TEXT("unfinished source { // preserve my edits\n")
	};
	for (const FString& Original : ExistingSources)
	{
		const FString Source = Files.Write(TEXT("NS_Adopt.dfs"), Original);
		if (!TestFalse(TEXT("existing source fixture written"), Source.IsEmpty())) { return false; }
		FDiagnosticSink Diagnostics;
		TestFalse(TEXT("existing destination refuses Adopt regardless of parse/name"),
			FDreamFXCommands::ValidateAdoptDestination(PackagePath, Source, Diagnostics));
		TestTrue(TEXT("refusal explains source conflict"), Diagnostics.FormatAll().Contains(TEXT("DFX8011")));
		AddExpectedError(TEXT("DFX8011"), EAutomationExpectedErrorFlags::Contains, 1);
		FDreamFXCommands::AdoptSystem(System, true);
		FString After;
		TestTrue(TEXT("refused source remains readable"), FFileHelper::LoadFileToString(After, *Source));
		TestEqual(TEXT("public Adopt command preserves author's existing bytes"), After, Original);
		FString Error;
		TestFalse(TEXT("publish also refuses a target created after validation"),
			FDreamFXCommands::WriteNewAdoptSource(Source, TEXT("replacement"), Error));
		FFileHelper::LoadFileToString(After, *Source);
		TestEqual(TEXT("publication cannot overwrite the existing source"), After, Original);
	}
	const FString NewSource = Files.Directory / TEXT("NewSource.dfs");
	Files.Track(NewSource);
	FDiagnosticSink NewDiagnostics;
	TestTrue(TEXT("unused source destination is allowed"), FDreamFXCommands::ValidateAdoptDestination(
		TEXT("/Game/") + Folder + TEXT("/NewSource"), NewSource, NewDiagnostics));
	FString WriteError;
	const FString NewText = TEXT("System(Name=\"NewSource\",Root=\"Game\") {}\n");
	TestTrue(TEXT("new source publishes successfully"), FDreamFXCommands::WriteNewAdoptSource(NewSource, NewText, WriteError));
	FString Written;
	TestTrue(TEXT("new source is readable"), FFileHelper::LoadFileToString(Written, *NewSource));
	TestEqual(TEXT("new source has full intended contents"), Written, NewText);
	TArray<FString> TemporaryFiles;
	IFileManager::Get().FindFiles(TemporaryFiles, *(Files.Directory / TEXT("*.tmp")), true, false);
	TestEqual(TEXT("publication leaves no staging files"), TemporaryFiles.Num(), 0);
	return true;
}

#endif
