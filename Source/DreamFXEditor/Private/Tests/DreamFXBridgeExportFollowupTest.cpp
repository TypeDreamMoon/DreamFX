#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Bridge/DreamFXBridgeService.h"
#include "Dom/JsonObject.h"
#include "DreamFXParser.h"
#include "DreamFXWorkspaceTestFiles.h"
#include "Engine/Texture2D.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/DreamFXEditorSettings.h"
#include "SourceFiles/DreamFXPaths.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

namespace UE::DreamFX::Editor::RoundTripRegression
{
	UNiagaraSystem* Build(FAutomationTestBase& Test, const FString& Source);
}

namespace UE::DreamFX::Editor::NativeInheritanceRegression
{
	UNiagaraEmitter* MakeParent(FAutomationTestBase& Test, const FString& Suffix);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBridgeExportResultRegression,
	"DreamFX.Followups.BridgeExportResult",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBridgeExportResultRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::WorkspaceFollowups;
	FFiles Files;
	UDreamFXEditorSettings* Settings = GetMutableDefault<UDreamFXEditorSettings>();
	const FString OriginalOutputDirectory = Settings->DecompiledOutputDirectory;
	ON_SCOPE_EXIT { FDreamFXPaths::InvalidateSourceRoots(); };
	FString RelativeOutput = Files.Directory;
	FPaths::MakePathRelativeTo(RelativeOutput, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);

	auto Decode = [&](const FString& Response) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response);
		if (!TestTrue(TEXT("bridge returns valid JSON"), FJsonSerializer::Deserialize(Reader, Object)) || !Object.IsValid()) { return nullptr; }
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		TestTrue(TEXT("response always contains structured diagnostics"), Object->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
		return Object;
	};
	auto Export = [&](const FString& AssetPath) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("requestId"), RequestId);
		Request->SetStringField(TEXT("action"), TEXT("decompile"));
		Request->SetStringField(TEXT("assetPath"), AssetPath);
		const FString ResponseText = FBridgeService::ExecuteRequest(Request);
		AddInfo(FString::Printf(TEXT("Bridge decompile %s: %s"), *AssetPath, *ResponseText));
		TSharedPtr<FJsonObject> Response = Decode(ResponseText);
		if (Response.IsValid())
		{
			FString ReturnedId;
			TestTrue(TEXT("response preserves request identity"), Response->TryGetStringField(TEXT("requestId"), ReturnedId) && ReturnedId == RequestId);
		}
		return Response;
	};
	auto CheckRejected = [&](const TSharedPtr<FJsonObject>& Response, const TCHAR* MessageFragment)
	{
		if (!TestTrue(TEXT("rejection response exists"), Response.IsValid())) { return; }
		bool bOk = true;
		FString Message;
		TestTrue(TEXT("rejection reports ok=false"), Response->TryGetBoolField(TEXT("ok"), bOk) && !bOk);
		TestFalse(TEXT("rejection does not claim an output file"), Response->HasField(TEXT("outputPath")));
		TestTrue(TEXT("rejection explains the cause"), Response->TryGetStringField(TEXT("message"), Message) && Message.Contains(MessageFragment));
	};

	{
		// CDO values are restored in memory; no SaveConfig or editor settings notification occurs.
		TGuardValue<FString> OutputGuard(Settings->DecompiledOutputDirectory, RelativeOutput);
		FDreamFXPaths::InvalidateSourceRoots();
		CheckRejected(Decode(FBridgeService::ExecuteRequest(TSharedPtr<FJsonObject>())), TEXT("not valid JSON"));
		CheckRejected(Export(TEXT("/Game/DreamFXAutomation/Missing_") + RequestId + TEXT(".Missing_") + RequestId), TEXT("Could not load"));
		UPackage* WrongPackage = CreatePackage(*(TEXT("/Game/DreamFXAutomation/Other_") + RequestId));
		UTexture2D* WrongAsset = NewObject<UTexture2D>(WrongPackage, TEXT("Other"), RF_Public | RF_Standalone);
		FGCObjectScopeGuard WrongGuard(WrongAsset);
		CheckRejected(Export(WrongAsset->GetPathName()), TEXT("Only a Niagara system or emitter"));

		UPackage* MirrorPackage = CreatePackage(*(TEXT("/Game/Decompiled/DreamFXAutomation/Mirror_") + RequestId));
		// An uninitialized system proves mirror rejection happens before reading its graph.
		UNiagaraSystem* ExistingMirror = NewObject<UNiagaraSystem>(MirrorPackage, TEXT("Mirror"), RF_Public | RF_Standalone);
		FGCObjectScopeGuard MirrorGuard(ExistingMirror);
		const FString RefusedOutput = FDreamFXPaths::DecompiledSourcePathFor(MirrorPackage->GetName(), TEXT(".dfs"));
		Files.Track(RefusedOutput);
		CheckRejected(Export(ExistingMirror->GetPathName()), TEXT("already a DreamFX mirror"));
		TestFalse(TEXT("refusing a mirror does not create a file"), FPaths::FileExists(RefusedOutput));

		UNiagaraSystem* System = RoundTripRegression::Build(*this,
			TEXT("System(Name=\"Unused\",Root=\"Game\") { Emitter E { ParticleSpawn={float Particles.Value=3.0;} SpriteRenderer R {} } }"));
		if (!TestNotNull(TEXT("system fixture"), System)) { return false; }
		FGCObjectScopeGuard SystemGuard(System);
		UNiagaraEmitter* Emitter = NativeInheritanceRegression::MakeParent(*this, RequestId);
		if (!TestNotNull(TEXT("standalone emitter fixture"), Emitter)) { return false; }
		FGCObjectScopeGuard EmitterGuard(Emitter);
		for (UObject* Asset : { static_cast<UObject*>(System), static_cast<UObject*>(Emitter) })
		{
			const bool bSystem = Asset == System;
			const FString OriginalAssetPath = Asset->GetPathName();
			const FString ExpectedPath = FDreamFXPaths::DecompiledSourcePathFor(Asset->GetOutermost()->GetName(), bSystem ? TEXT(".dfs") : TEXT(".dfe"));
			if (!TestTrue(TEXT("fixture writes only in its private directory"), FPaths::IsUnderDirectory(ExpectedPath, Files.Directory))) { return false; }
			Files.Track(ExpectedPath);
			const TSharedPtr<FJsonObject> Response = Export(Asset->GetPathName());
			if (!TestTrue(TEXT("successful export response exists"), Response.IsValid())) { return false; }
			bool bOk = false;
			FString OutputPath;
			const FString KindLabel = bSystem ? TEXT("system") : TEXT("emitter");
			TestTrue(KindLabel + TEXT(" export reports ok=true"), Response->TryGetBoolField(TEXT("ok"), bOk) && bOk);
			TestTrue(KindLabel + TEXT(" export reports the actual output path"), Response->TryGetStringField(TEXT("outputPath"), OutputPath) && FPaths::IsSamePath(OutputPath, ExpectedPath));
			TestTrue(KindLabel + TEXT(" output path exists"), FPaths::FileExists(OutputPath));
			FString Source;
			if (TestTrue(TEXT("exported source is readable"), FFileHelper::LoadFileToString(Source, *ExpectedPath)))
			{
				FDocument Document;
				FDiagnosticSink Diagnostics;
				if (!TestTrue(TEXT("successful export contains parseable source"), FParser::ParseText(Source, ExpectedPath, Document, Diagnostics))) { AddError(Diagnostics.FormatAll()); }
				TestTrue(TEXT("output has the requested document kind"), Document.Kind == (bSystem ? EDocumentKind::System : EDocumentKind::Emitter));
				FString ExpectedDocumentName = Asset->GetOutermost()->GetName();
				ExpectedDocumentName.RemoveFromStart(TEXT("/Game/"));
				TestEqual(TEXT("output retains the original package identity"), Document.Name, TEXT("Decompiled/") + ExpectedDocumentName);
			}
			TestEqual(TEXT("export does not rename the original asset"), Asset->GetPathName(), OriginalAssetPath);
		}

		// A regular file in place of the output directory gives a deterministic write failure,
		// without filesystem permissions, global settings, or overwriting a real source file.
		const FString BlockedDirectory = Files.Write(TEXT("blocked"), TEXT("fixture sentinel"));
		if (!TestFalse(TEXT("write blocker created"), BlockedDirectory.IsEmpty())) { return false; }
		Settings->DecompiledOutputDirectory = RelativeOutput / TEXT("blocked");
		FDreamFXPaths::InvalidateSourceRoots();
		const FString FailedPath = FDreamFXPaths::DecompiledSourcePathFor(System->GetOutermost()->GetName(), TEXT(".dfs"));
		CheckRejected(Export(System->GetPathName()), TEXT("could not write"));
		TestFalse(TEXT("failed write created no output"), FPaths::FileExists(FailedPath));
		FString Sentinel;
		TestTrue(TEXT("write failure preserves blocker file"), FFileHelper::LoadFileToString(Sentinel, *BlockedDirectory));
		TestEqual(TEXT("write failure preserves existing bytes"), Sentinel, FString(TEXT("fixture sentinel")));
	}
	TestEqual(TEXT("output configuration restored without persisting changes"), Settings->DecompiledOutputDirectory, OriginalOutputDirectory);
	return true;
}

#endif
