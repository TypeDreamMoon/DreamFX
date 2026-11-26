#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Database/DreamFXDatabase.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Schema/DreamFXIndexExport.h"
#include "SourceFiles/DreamFXPaths.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Workspace/DreamFXSourceMigration.h"

namespace UE::DreamFX::Editor::DreamSourceTests
{
	FString MakeScratchDirectory()
	{
		const FString Directory = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation/DreamSource") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);
		return Directory;
	}

	/** One text column of every row, in order. */
	TArray<FString> Column(const FString& DatabasePath, const TCHAR* Query)
	{
		TArray<FString> Out;
		FSQLiteDatabase Database;
		if (!Database.Open(*DatabasePath, ESQLiteDatabaseOpenMode::ReadOnly))
		{
			return Out;
		}
		{
			FSQLitePreparedStatement Statement(Database, Query);
			while (Statement.IsValid() && Statement.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Statement.GetColumnValueByIndex(0, Out.AddDefaulted_GetRef());
			}
		}
		Database.Close();
		return Out;
	}

	FString Meta(const FString& DatabasePath, const FString& Key)
	{
		const TArray<FString> Values = Column(DatabasePath,
			*FString::Printf(TEXT("SELECT value FROM meta WHERE key = '%s';"), *Key));
		return Values.Num() > 0 ? Values[0] : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDreamSourceDatabaseTest,
	"DreamFX.DreamSource.Database", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDreamSourceDatabaseTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::DreamSourceTests;

	const FString Scratch = MakeScratchDirectory();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };
	const FString FxDb = Scratch / TEXT("fx.db");

	// fx.db: the bridge's last diagnostics, one row each, in order, whole-table replaced.
	TArray<FDreamFXDatabase::FDiagnosticRow> Rows;
	Rows.Add({ TEXT("A.dfs"), TEXT("{\"file\":\"A.dfs\",\"line\":1,\"column\":2,\"severity\":\"error\",\"code\":\"DFX0001\",\"message\":\"a\"}") });
	Rows.Add({ TEXT("B.dfe"), TEXT("{\"file\":\"B.dfe\",\"line\":3,\"column\":4,\"severity\":\"warning\",\"code\":\"DFX0002\",\"message\":\"b\"}") });
	TestTrue(TEXT("diagnostics written"), FDreamFXDatabase::WriteDiagnostics(FxDb, Rows, TEXT("2026-11-26T00:00:00.000Z")));
	TestEqual(TEXT("schema stamped"), Meta(FxDb, TEXT("schema")), FString(TEXT("1")));
	TestTrue(TEXT("producer stamped"), Meta(FxDb, TEXT("producer")).StartsWith(TEXT("DreamFX@")));
	TestEqual(TEXT("diagnostics revision"), Meta(FxDb, TEXT("diagnostics_revision")), FString(TEXT("1")));
	TestEqual(TEXT("generatedUtc kept"), Meta(FxDb, TEXT("diagnostics_updated_utc")), FString(TEXT("2026-11-26T00:00:00.000Z")));

	TArray<FDreamFXDatabase::FDiagnosticRow> Read;
	FString GeneratedUtc;
	TestTrue(TEXT("diagnostics read"), FDreamFXDatabase::ReadDiagnostics(FxDb, Read, &GeneratedUtc));
	TestEqual(TEXT("row count"), Read.Num(), 2);
	if (Read.Num() == 2)
	{
		TestEqual(TEXT("order kept"), Read[0].File, FString(TEXT("A.dfs")));
		TestEqual(TEXT("json kept verbatim"), Read[1].Json, Rows[1].Json);
	}

	TestTrue(TEXT("empty replacement written"), FDreamFXDatabase::WriteDiagnostics(FxDb, {}, TEXT("2026-11-26T00:00:01.000Z")));
	FDreamFXDatabase::ReadDiagnostics(FxDb, Read);
	TestEqual(TEXT("whole table replaced"), Read.Num(), 0);
	TestEqual(TEXT("revision bumped per write"), Meta(FxDb, TEXT("revision")), FString(TEXT("2")));

	// A session database with an unknown schema is rebuilt in place.
	{
		FSQLiteDatabase Raw;
		Raw.Open(*FxDb, ESQLiteDatabaseOpenMode::ReadWrite);
		Raw.Execute(TEXT("UPDATE meta SET value = '99' WHERE key = 'schema';"));
		Raw.Close();
	}
	TestTrue(TEXT("unknown session schema rebuilt"), FDreamFXDatabase::WriteDiagnostics(FxDb, Rows, TEXT("2026-11-26T00:00:02.000Z")));
	TestEqual(TEXT("rebuilt schema"), Meta(FxDb, TEXT("schema")), FString(TEXT("1")));
	TestEqual(TEXT("rebuilt revision restarts"), Meta(FxDb, TEXT("revision")), FString(TEXT("1")));

	// An index database with an unknown schema is refused, never dropped: its quarantine cost a crash per row.
	const FString IndexDb = Scratch / TEXT(".dreamfx/index.db");
	{
		FDreamFXDatabase Index;
		TestTrue(TEXT("index opens"), Index.Open(IndexDb, FDreamFXDatabase::EKind::Index));
		Index.Execute(TEXT("INSERT INTO quarantine(package, quarantined_utc) VALUES('/Niagara/Bad', 'x');"));
		Index.SetMeta(TEXT("schema"), TEXT("2"));
	}
	{
		AddExpectedError(TEXT("schema 2"), EAutomationExpectedErrorFlags::Contains, 0);
		FDreamFXDatabase Index;
		FString Error;
		TestFalse(TEXT("newer index schema refused"), Index.Open(IndexDb, FDreamFXDatabase::EKind::Index, &Error));
		TestTrue(TEXT("refusal says to update"), Error.Contains(TEXT("Update the DreamFX plugin")));
	}
	TestEqual(TEXT("quarantine kept"), Column(IndexDb, TEXT("SELECT package FROM quarantine;")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDreamSourceIndexImportTest,
	"DreamFX.DreamSource.IndexImport", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDreamSourceIndexImportTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::DreamSourceTests;

	const FString Source = MakeScratchDirectory();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Source, false, true); };

	const FString Legacy = Source / TEXT(".dfx-index.json");
	FFileHelper::SaveStringToFile(
		TEXT("{\"version\":1,\"generatedUtc\":\"2026-08-13T07:58:00.000Z\",\"project\":\"DevTest\",\"engine\":\"E:/UE/\",")
		TEXT("\"plugins\":[\"Niagara\"],\"searchPaths\":[\"/Niagara\"],\"modules\":[")
		TEXT("{\"name\":\"GravityForce\",\"path\":\"/Niagara/Modules/GravityForce\",\"kind\":\"module\",\"stacks\":[\"ParticleUpdate\"],\"inputs\":[]},")
		TEXT("{\"name\":\"ConeMask\",\"path\":\"/Niagara/Modules/Masks/ConeMask\",\"kind\":\"module\",\"stacks\":[],\"inputsUnavailable\":\"q\"},")
		TEXT("{\"name\":\"RandomRange\",\"path\":\"/Niagara/DynamicInputs/RandomRange\",\"kind\":\"dynamicInput\",\"stacks\":[]}]}"),
		*Legacy);
	FFileHelper::SaveStringToFile(TEXT("/Niagara/Modules/Masks/ConeMask\n"), *(Legacy + TEXT(".quarantine")));
	FFileHelper::SaveStringToFile(TEXT("/Niagara/Modules/Crashy"), *(Legacy + TEXT(".inflight")));

	FString Error;
	TestTrue(TEXT("legacy index imported"), FIndexExport::ImportLegacyIndex(Source, Error));
	const FString IndexDb = FIndexExport::GetIndexDatabasePath(Source);
	TestTrue(TEXT("index.db under .dreamfx"), IndexDb.EndsWith(TEXT("/.dreamfx/index.db")));

	const TArray<FString> Names = Column(IndexDb, TEXT("SELECT name FROM modules ORDER BY ord;"));
	TestEqual(TEXT("module count"), Names.Num(), 3);
	if (Names.Num() == 3)
	{
		TestEqual(TEXT("order kept"), Names[2], FString(TEXT("RandomRange")));
	}
	TestEqual(TEXT("kind column"),
		FString::Join(Column(IndexDb, TEXT("SELECT kind FROM modules WHERE name = 'RandomRange';")), TEXT("|")),
		FString(TEXT("dynamicInput")));
	TestEqual(TEXT("header: project"), Meta(IndexDb, TEXT("project")), FString(TEXT("DevTest")));
	TestEqual(TEXT("header: generated"), Meta(IndexDb, TEXT("generated_utc")), FString(TEXT("2026-08-13T07:58:00.000Z")));
	TestEqual(TEXT("header: plugins as JSON"), Meta(IndexDb, TEXT("plugins")), FString(TEXT("[\"Niagara\"]")));
	TestEqual(TEXT("header: inputsProbed absent stays absent"), Meta(IndexDb, TEXT("inputs_probed")), FString());
	TestEqual(TEXT("modules revision"), Meta(IndexDb, TEXT("modules_revision")), FString(TEXT("1")));
	TestEqual(TEXT("quarantine includes the in-flight module"),
		FString::Join(Column(IndexDb, TEXT("SELECT package FROM quarantine ORDER BY package;")), TEXT("|")),
		FString(TEXT("/Niagara/Modules/Crashy|/Niagara/Modules/Masks/ConeMask")));
	TestFalse(TEXT("legacy index deleted"), FPaths::FileExists(Legacy));
	TestFalse(TEXT("legacy quarantine deleted"), FPaths::FileExists(Legacy + TEXT(".quarantine")));
	TestFalse(TEXT("legacy journal deleted"), FPaths::FileExists(Legacy + TEXT(".inflight")));

	// A database that already has modules is newer than any file beside it: only the quarantine merges.
	FFileHelper::SaveStringToFile(TEXT("{\"version\":1,\"modules\":[]}"), *Legacy);
	FFileHelper::SaveStringToFile(TEXT("/Niagara/Modules/Another"), *(Legacy + TEXT(".quarantine")));
	TestTrue(TEXT("second import"), FIndexExport::ImportLegacyIndex(Source, Error));
	TestEqual(TEXT("modules untouched"), Column(IndexDb, TEXT("SELECT name FROM modules;")).Num(), 3);
	TestEqual(TEXT("quarantine merged"), Column(IndexDb, TEXT("SELECT package FROM quarantine;")).Num(), 3);
	TestFalse(TEXT("stale legacy index deleted"), FPaths::FileExists(Legacy));

	TestTrue(TEXT("nothing to import is success"), FIndexExport::ImportLegacyIndex(Source, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDreamSourceWorkspaceRebaseTest,
	"DreamFX.DreamSource.WorkspaceRebase", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDreamSourceWorkspaceRebaseTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;

	const FString Text =
		TEXT("{\n\t\"folders\": [\n")
		TEXT("\t\t{ \"name\": \"DreamFX Source\", \"path\": \".\" },\n")
		TEXT("\t\t{ \"name\": \"Plugin: MoonToon\", \"path\": \"../Plugins/MoonToon/DFX\" },\n")
		TEXT("\t\t{ \"name\": \"Plugin: Engine\", \"path\": \"F:/UE/Engine/Plugins/X/DFX\" }\n")
		TEXT("\t]\n}\n");
	FString Rebased;
	TestTrue(TEXT("workspace parsed"), FDreamFXSourceMigration::RebaseWorkspaceFolders(
		Text, TEXT("C:/Project/DFX"), TEXT("C:/Project/DreamSource/FX"), Rebased));
	TestTrue(TEXT("relative plugin folder rebased"), Rebased.Contains(TEXT("\"../../Plugins/MoonToon/DFX\"")));
	TestTrue(TEXT("project folder unchanged"), Rebased.Contains(TEXT("\"path\": \".\"")));
	TestTrue(TEXT("absolute folder unchanged"), Rebased.Contains(TEXT("\"F:/UE/Engine/Plugins/X/DFX\"")));
	TestTrue(TEXT("layout kept"), Rebased.StartsWith(TEXT("{\n\t\"folders\": [\n")));

	FString Ignored;
	TestFalse(TEXT("not a workspace"), FDreamFXSourceMigration::RebaseWorkspaceFolders(TEXT("[]"), TEXT("a"), TEXT("b"), Ignored));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDreamSourceLayoutTest,
	"DreamFX.DreamSource.Layout", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDreamSourceLayoutTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;

	const FProjectSourceDirectory Project = FDreamFXPaths::ResolveProjectSourceDirectory();
	TestTrue(TEXT("configured directory ends with the configured value"),
		Project.ConfiguredDirectory.EndsWith(Project.ConfiguredRelative));
	TestTrue(TEXT("legacy directory is DFX"), Project.LegacyDirectory.EndsWith(TEXT("/DFX")));
	TestEqual(TEXT("in use is configured or legacy"), Project.Directory,
		Project.bUsingLegacy ? Project.LegacyDirectory : Project.ConfiguredDirectory);
	TestFalse(TEXT("legacy is not both used and ignored"), Project.bUsingLegacy && Project.bLegacyIgnored);
	TestEqual(TEXT("memoized"), FDreamFXPaths::GetProjectSourceDirectory(), Project.Directory);

	TestEqual(TEXT("index database follows the sources"), FDreamFXPaths::GetIndexDatabasePath(),
		Project.Directory / TEXT(".dreamfx/index.db"));
	TestTrue(TEXT("fx.db under Intermediate/DreamSource"),
		FDreamFXPaths::GetFXDatabasePath().EndsWith(TEXT("Intermediate/DreamSource/fx.db")));
	TestTrue(TEXT("FX intermediate"), FDreamFXPaths::GetFXIntermediateDirectory().EndsWith(TEXT("Intermediate/DreamSource/FX")));

	const FString Decompiled = FDreamFXPaths::GetDecompiledOutputDirectory();
	TestTrue(TEXT("decompiled output is absolute"), !FPaths::IsRelative(Decompiled));

	// A `from "DFX/..."` written before the move still resolves against the source root it now sits in.
	for (const FSourceRoot& Root : FDreamFXPaths::GetSourceRoots())
	{
		TArray<FString> Candidates;
		FDreamFXPaths::GetSourceReferenceCandidates(TEXT("DFX/Emitters/E_Flash"), FString(), TEXT(".dfe"), Candidates);
		TestTrue(*FString::Printf(TEXT("DFX/ prefix dropped for %s"), *Root.Directory),
			Candidates.Contains(FPaths::ConvertRelativePathToFull(Root.Directory / TEXT("Emitters/E_Flash.dfe"))));
		Candidates.Reset();
		FDreamFXPaths::GetSourceReferenceCandidates(Project.ConfiguredRelative / TEXT("Emitters/E_Flash"), FString(),
			TEXT(".dfe"), Candidates);
		TestTrue(*FString::Printf(TEXT("configured prefix dropped for %s"), *Root.Directory),
			Candidates.Contains(FPaths::ConvertRelativePathToFull(Root.Directory / TEXT("Emitters/E_Flash.dfe"))));
	}

	bool bLegacy = true;
	const FString Plugin = FDreamFXPaths::GetPluginSourceDirectory(FPaths::ProjectSavedDir() / TEXT("NoSuchPlugin"), &bLegacy);
	TestFalse(TEXT("a plugin with neither is not legacy"), bLegacy);
	TestTrue(TEXT("a plugin with neither gets DreamSource/FX"), Plugin.EndsWith(TEXT("NoSuchPlugin/DreamSource/FX")));
	return true;
}

#endif
