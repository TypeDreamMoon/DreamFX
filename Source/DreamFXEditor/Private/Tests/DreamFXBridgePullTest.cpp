#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Bridge/DreamFXBridgeService.h"
#include "Dom/JsonObject.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GCObjectScopeGuard.h"

/**
 * The bridge's `pull` action, driven the way a client drives it: a request object in, a response string
 * out. The values it moves are covered by DreamFX.Corpus.WriteBack and DreamFX.Regression.Pull; what is
 * pinned here is the part only the bridge adds -- the text-versus-file rule, and that the command line
 * and an editor buffer share one baseline.
 */
namespace UE::DreamFX::Editor::BridgePullTests
{
	/**
	 * A system built from `SourceFor(AssetName, 3)` and a directory to keep sources in.
	 *
	 * The asset is built but not saved, which is the state the action exists for: it is in this
	 * process and nowhere else, and `pull` has to read it from memory.
	 */
	struct FFixture
	{
		FString AssetName;
		FString PackageName;
		FString Directory;
		FString SourceFile;
		FString ReportPath;

		UNiagaraSystem* System = nullptr;
		TUniquePtr<FGCObjectScopeGuard> Guard;

		FFixture(const TCHAR* Stem)
		{
			AssetName = FString::Printf(TEXT("DreamFXAutomation/%s_%s"), Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			PackageName = TEXT("/Game/") + AssetName;
			Directory = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
			SourceFile = Directory / TEXT("BridgePull.dfs");
		}

		~FFixture()
		{
			// Everything pull leaves beside a source file is named from the report path it returns.
			if (!ReportPath.IsEmpty())
			{
				IFileManager::Get().Delete(*ReportPath, false, true, true);
				IFileManager::Get().Delete(*BaselinePath(), false, true, true);
				IFileManager::Get().Delete(*BackupPath(), false, true, true);
			}
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
			IFileManager::Get().Delete(
				*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()),
				false, true, true);
		}

		/** `<Saved>/DreamFX/Pull/<source path>` without its extension; what pull's side files hang off. */
		FString PullStem() const { return ReportPath.LeftChop(FString(TEXT(".report.txt")).Len()); }
		FString BaselinePath() const { return PullStem() + TEXT(".baseline.txt"); }
		FString BackupPath() const { return PullStem() + TEXT(".dfs"); }

		/**
		 * The source at a given seed: UTF-8 text outside ASCII, CRLF endings, and a comment after the
		 * literal that has to survive being rewritten.
		 */
		FString SourceFor(int32 Seed) const
		{
			const FString Lf = FString::Printf(
				TEXT("// 粒子：写回\nSystem(Name=\"%s\",Root=\"Game\")\n{\n    Emitter E\n    {\n        Settings = { RandomSeed = %d; LocalSpace = true; } // hand-tuned, keep this\n        ParticleSpawn = { Particles.SpriteRotation = 0.5; }\n        SpriteRenderer R {}\n    }\n}\n"),
				*AssetName, Seed);
			return Lf.Replace(TEXT("\n"), TEXT("\r\n"));
		}

		bool Build(FAutomationTestBase& Test, int32 Seed)
		{
			FDocument Document;
			FDiagnosticSink Diagnostics;
			if (!FParser::ParseText(SourceFor(Seed), SourceFile, Document, Diagnostics))
			{
				Test.AddError(Diagnostics.FormatAll());
				return false;
			}
			FGenerateOptions Options;
			Options.bSave = false;
			Options.bForce = true;
			const FGenerateResult Built = FGenerator::Generate(Document, Options, Diagnostics);
			if (!Built.bSucceeded || Built.System == nullptr)
			{
				Test.AddError(FString::Printf(TEXT("build failed:\n%s"), *Diagnostics.FormatAll()));
				return false;
			}
			System = Built.System;
			Guard = MakeUnique<FGCObjectScopeGuard>(System);
			return true;
		}

		bool WriteSource(FAutomationTestBase& Test, int32 Seed) const
		{
			return Test.TestTrue(TEXT("source file written"), FFileHelper::SaveStringToFile(
				SourceFor(Seed), *SourceFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		}

		FString ReadSource() const
		{
			FString Text;
			FFileHelper::LoadFileToString(Text, *SourceFile);
			return Text;
		}
	};

	/** One `pull` round trip: what was asked, and the decoded answer. */
	struct FPullReply
	{
		TSharedPtr<FJsonObject> Response;
		TSharedPtr<FJsonObject> Pull;
		bool bOk = false;
		FString Message;
	};

	struct FPullAsk
	{
		FString SourceFile;
		TOptional<FString> Text;
		bool bApply = false;
		bool bStructure = false;
	};

	FPullReply Send(FAutomationTestBase& Test, const FPullAsk& Asked)
	{
		const TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("requestId"), TEXT("bridge-pull-test"));
		Request->SetStringField(TEXT("action"), TEXT("pull"));
		Request->SetStringField(TEXT("sourceFile"), Asked.SourceFile);
		if (Asked.Text.IsSet()) { Request->SetStringField(TEXT("text"), Asked.Text.GetValue()); }
		if (Asked.bApply) { Request->SetBoolField(TEXT("apply"), true); }
		if (Asked.bStructure) { Request->SetBoolField(TEXT("structure"), true); }

		FPullReply Reply;
		const FString ResponseText = FBridgeService::ExecuteRequest(Request);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseText);
		if (!Test.TestTrue(TEXT("pull response is JSON"), FJsonSerializer::Deserialize(Reader, Reply.Response) && Reply.Response.IsValid()))
		{
			return Reply;
		}
		Reply.Response->TryGetBoolField(TEXT("ok"), Reply.bOk);
		Reply.Response->TryGetStringField(TEXT("message"), Reply.Message);
		const TSharedPtr<FJsonObject>* Pull = nullptr;
		if (Reply.Response->TryGetObjectField(TEXT("pull"), Pull)) { Reply.Pull = *Pull; }
		Test.AddInfo(FString::Printf(TEXT("pull -> ok=%d, %s"), Reply.bOk, *Reply.Message));
		return Reply;
	}

	int32 Count(const FPullReply& Reply, const TCHAR* Field)
	{
		int32 Value = -1;
		if (Reply.Pull.IsValid()) { Reply.Pull->TryGetNumberField(Field, Value); }
		return Value;
	}

	bool Flag(const FPullReply& Reply, const TCHAR* Field)
	{
		bool bValue = false;
		if (Reply.Pull.IsValid()) { Reply.Pull->TryGetBoolField(Field, bValue); }
		return bValue;
	}

	FString NewText(const FPullReply& Reply)
	{
		FString Value;
		if (Reply.Pull.IsValid()) { Reply.Pull->TryGetStringField(TEXT("newText"), Value); }
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBridgePullBuffer,
	"DreamFX.Bridge.Pull.Buffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBridgePullBuffer::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::BridgePullTests;

	// The asset holds seed 3; the buffer (and the file under it) says 5.
	FFixture Fixture(TEXT("NS_BridgePullBuffer"));
	if (!Fixture.Build(*this, 3) || !Fixture.WriteSource(*this, 5)) { return false; }
	const FString Drifted = Fixture.SourceFor(5);
	const FString Pulled = Fixture.SourceFor(3);

	// A buffer that differs from the file on disk: the unsaved text is what is pulled, not the file.
	const FString Unsaved = Drifted + TEXT("// not saved yet\r\n");
	const FString UnsavedPulled = Pulled + TEXT("// not saved yet\r\n");

	// --- preview -----------------------------------------------------------------------------------
	FPullAsk Preview;
	Preview.SourceFile = Fixture.SourceFile;
	Preview.Text = Unsaved;
	const FPullReply First = Send(*this, Preview);
	if (First.Pull.IsValid()) { First.Pull->TryGetStringField(TEXT("reportPath"), Fixture.ReportPath); }
	if (!TestTrue(TEXT("preview succeeds"), First.bOk) || !TestTrue(TEXT("preview carries a pull object"), First.Pull.IsValid())) { return false; }

	TestEqual(TEXT("exactly the drifted value is rewritten, comment and line endings untouched"),
		NewText(First), UnsavedPulled);
	TestTrue(TEXT("preview reports a change"), Flag(First, TEXT("changed")));
	TestEqual(TEXT("one value differs"), Count(First, TEXT("changedValues")), 1);
	TestEqual(TEXT("one edit"), Count(First, TEXT("edits")), 1);
	TestTrue(TEXT("the values were compared"), Count(First, TEXT("compared")) >= 1);
	TestFalse(TEXT("no file written"), Flag(First, TEXT("wroteFile")));
	TestFalse(TEXT("a preview records no baseline"), Flag(First, TEXT("baselineWritten")));
	TestFalse(TEXT("there was no baseline to use"), Flag(First, TEXT("baselineUsed")));
	TestTrue(TEXT("the message says it is a preview"), First.Message.Contains(TEXT("preview")));
	TestTrue(TEXT("the report names the value"), [&]
	{
		const TArray<TSharedPtr<FJsonValue>>* Report = nullptr;
		return First.Pull->TryGetArrayField(TEXT("report"), Report) && Report->Num() > 0;
	}());
	TestEqual(TEXT("the source file still holds what was written"), Fixture.ReadSource(), Drifted);
	TestFalse(TEXT("no baseline file after a preview"), FPaths::FileExists(Fixture.BaselinePath()));

	// --- apply the buffer --------------------------------------------------------------------------
	FPullAsk Apply = Preview;
	Apply.bApply = true;
	const FPullReply Applied = Send(*this, Apply);
	if (!TestTrue(TEXT("apply succeeds"), Applied.bOk)) { return false; }
	TestEqual(TEXT("apply returns the same text"), NewText(Applied), UnsavedPulled);
	TestFalse(TEXT("apply with text never writes the file"), Flag(Applied, TEXT("wroteFile")));
	TestEqual(TEXT("the file on disk is untouched"), Fixture.ReadSource(), Drifted);
	TestTrue(TEXT("apply records the baseline"), Flag(Applied, TEXT("baselineWritten")));
	TestTrue(TEXT("the baseline exists"), FPaths::FileExists(Fixture.BaselinePath()));
	TestFalse(TEXT("no backup is made for a buffer"), FPaths::FileExists(Fixture.BackupPath()));

	// --- after the apply ---------------------------------------------------------------------------
	// The buffer the client now holds is the pulled one: nothing more to say about it.
	FPullAsk Again;
	Again.SourceFile = Fixture.SourceFile;
	Again.Text = UnsavedPulled;
	const FPullReply Second = Send(*this, Again);
	if (!TestTrue(TEXT("second preview succeeds"), Second.bOk)) { return false; }
	TestFalse(TEXT("nothing left to change"), Flag(Second, TEXT("changed")));
	TestEqual(TEXT("the text comes back byte for byte"), NewText(Second), UnsavedPulled);
	TestEqual(TEXT("no edits"), Count(Second, TEXT("edits")), 0);
	TestTrue(TEXT("the recorded baseline was used"), Flag(Second, TEXT("baselineUsed")));

	// The old text again (an undo, say): the asset has not moved since the apply, so the text is the
	// newer decision and is left alone. That is the baseline doing its job through the bridge.
	FPullAsk Undone = Again;
	Undone.Text = Drifted;
	const FPullReply Third = Send(*this, Undone);
	if (!TestTrue(TEXT("third preview succeeds"), Third.bOk)) { return false; }
	TestFalse(TEXT("the text wins over an asset that has not moved"), Flag(Third, TEXT("changed")));
	TestEqual(TEXT("one value withheld"), Count(Third, TEXT("withheld")), 1);
	TestEqual(TEXT("the text is returned as it was"), NewText(Third), Drifted);
	TestTrue(TEXT("the message says why"), Third.Message.Contains(TEXT("withheld")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBridgePullFile,
	"DreamFX.Bridge.Pull.File",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBridgePullFile::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::BridgePullTests;

	FFixture Fixture(TEXT("NS_BridgePullFile"));
	if (!Fixture.Build(*this, 3) || !Fixture.WriteSource(*this, 5)) { return false; }
	const FString Drifted = Fixture.SourceFor(5);
	const FString Pulled = Fixture.SourceFor(3);

	// Without 'text' the action is the command line: a preview leaves the file alone...
	FPullAsk Preview;
	Preview.SourceFile = Fixture.SourceFile;
	const FPullReply Dry = Send(*this, Preview);
	if (Dry.Pull.IsValid()) { Dry.Pull->TryGetStringField(TEXT("reportPath"), Fixture.ReportPath); }
	if (!TestTrue(TEXT("file preview succeeds"), Dry.bOk) || !TestTrue(TEXT("with a pull object"), Dry.Pull.IsValid())) { return false; }
	TestFalse(TEXT("a file preview does not return text"), Dry.Pull->HasField(TEXT("newText")));
	TestFalse(TEXT("a file preview reports no change"), Flag(Dry, TEXT("changed")));
	TestEqual(TEXT("the value still differs"), Count(Dry, TEXT("changedValues")), 1);
	TestEqual(TEXT("file untouched"), Fixture.ReadSource(), Drifted);
	TestFalse(TEXT("no baseline after a dry run"), FPaths::FileExists(Fixture.BaselinePath()));

	// ...and apply rewrites it, with a backup of what was there.
	FPullAsk Apply = Preview;
	Apply.bApply = true;
	const FPullReply Wrote = Send(*this, Apply);
	if (!TestTrue(TEXT("file apply succeeds"), Wrote.bOk)) { return false; }
	TestTrue(TEXT("the file was written"), Flag(Wrote, TEXT("wroteFile")));
	TestTrue(TEXT("and that is the change"), Flag(Wrote, TEXT("changed")));
	TestEqual(TEXT("the file holds the pulled text"), Fixture.ReadSource(), Pulled);
	TestTrue(TEXT("the baseline was recorded"), Flag(Wrote, TEXT("baselineWritten")) && FPaths::FileExists(Fixture.BaselinePath()));
	FString Backup;
	TestTrue(TEXT("the backup is the file as it was"),
		FFileHelper::LoadFileToString(Backup, *Fixture.BackupPath()) && Backup == Drifted);

	// The baseline the file apply left is the one a buffer request reads: the same file, one record.
	FPullAsk Buffer = Preview;
	Buffer.Text = Drifted;
	const FPullReply Shared = Send(*this, Buffer);
	if (!TestTrue(TEXT("buffer preview after a file apply succeeds"), Shared.bOk)) { return false; }
	TestTrue(TEXT("the buffer request used the baseline the file apply recorded"), Flag(Shared, TEXT("baselineUsed")));
	TestEqual(TEXT("so the older text stands"), Count(Shared, TEXT("withheld")), 1);

	// Applying again changes nothing.
	const FPullReply Idle = Send(*this, Apply);
	TestTrue(TEXT("a second file apply succeeds"), Idle.bOk);
	TestFalse(TEXT("and writes nothing"), Flag(Idle, TEXT("wroteFile")));
	TestEqual(TEXT("file unchanged"), Fixture.ReadSource(), Pulled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBridgePullRejects,
	"DreamFX.Bridge.Pull.Rejects",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBridgePullRejects::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::BridgePullTests;

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };

	auto ExpectRejected = [&](const TCHAR* What, const FPullAsk& Request, const TCHAR* Fragment)
	{
		const FPullReply Reply = Send(*this, Request);
		TestFalse(FString::Printf(TEXT("%s: not ok"), What), Reply.bOk);
		TestTrue(FString::Printf(TEXT("%s: message mentions '%s' (was '%s')"), What, Fragment, *Reply.Message),
			Reply.Message.Contains(Fragment));
		TestFalse(FString::Printf(TEXT("%s: no pull object"), What), Reply.Pull.IsValid());
	};

	// A .dfe is merged into its host by copy and has no asset of its own; a buffer does not make it one.
	FPullAsk Emitter;
	Emitter.SourceFile = Directory / TEXT("Thing.dfe");
	Emitter.Text = TEXT("Emitter E {}");
	ExpectRejected(TEXT("emitter document"), Emitter, TEXT(".dfs"));

	FPullAsk Module;
	Module.SourceFile = Directory / TEXT("Thing.dfm");
	ExpectRejected(TEXT("module document"), Module, TEXT(".dfs"));

	FPullAsk Other;
	Other.SourceFile = Directory / TEXT("notes.txt");
	Other.Text = TEXT("System(Name=\"X\",Root=\"Game\") {}");
	ExpectRejected(TEXT("unrelated file"), Other, TEXT(".dfs"));

	FPullAsk NoFile;
	ExpectRejected(TEXT("no source file"), NoFile, TEXT("sourceFile"));

	// Without text the file is what is read, so it has to exist; with text it does not.
	FPullAsk Missing;
	Missing.SourceFile = Directory / TEXT("Missing.dfs");
	ExpectRejected(TEXT("missing file"), Missing, TEXT("No such source file"));

	// A buffer that does not parse is an answer, not a crash: ok=false with the diagnostics, no text.
	FPullAsk Broken;
	Broken.SourceFile = Directory / TEXT("Broken.dfs");
	Broken.Text = TEXT("System(Name=");
	const FPullReply Reply = Send(*this, Broken);
	TestFalse(TEXT("unparseable buffer: not ok"), Reply.bOk);
	TestTrue(TEXT("unparseable buffer: diagnostics say why"), [&]
	{
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		return Reply.Response.IsValid() && Reply.Response->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics->Num() > 0;
	}());
	TestFalse(TEXT("unparseable buffer: no change claimed"), Flag(Reply, TEXT("changed")));
	TestEqual(TEXT("unparseable buffer: the text is handed back as it came"), NewText(Reply), Broken.Text.GetValue());
	return true;
}

#endif
