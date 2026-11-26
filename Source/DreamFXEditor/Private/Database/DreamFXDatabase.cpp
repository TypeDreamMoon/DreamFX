#include "Database/DreamFXDatabase.h"

#include "DreamFXModule.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "SQLitePreparedStatement.h"

namespace UE::DreamFX::Editor
{
	namespace DatabaseLocal
	{
		FString Producer()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamFX"));
			return FString::Printf(TEXT("DreamFX@%s"), Plugin.IsValid() ? *Plugin->GetDescriptor().VersionName : TEXT("unknown"));
		}

		const TCHAR* const SessionTables[] = { TEXT("diagnostics") };
	}

	FDreamFXDatabase::~FDreamFXDatabase()
	{
		Close();
	}

	bool FDreamFXDatabase::Open(const FString& InPath, EKind InKind, FString* OutError)
	{
		Close();
		Path = InPath;
		Kind = InKind;
		FString Error;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(InPath), /*Tree=*/true);
		if (!Database.Open(*InPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
		{
			Error = FString::Printf(TEXT("could not open '%s': %s"), *InPath, *Database.GetLastError());
		}
		else
		{
			// busy_timeout first: a language server may be mid-read, and every statement after this one should wait it
			// out rather than fail on SQLITE_BUSY.
			Database.Execute(TEXT("PRAGMA busy_timeout=5000;"));
			Database.Execute(TEXT("PRAGMA journal_mode=WAL;"));
			// FULL for the index: its probe journal has to be on disk before the probe that may end the process.
			Database.Execute(Kind == EKind::Index ? TEXT("PRAGMA synchronous=FULL;") : TEXT("PRAGMA synchronous=NORMAL;"));
			if (!EnsureSchema(Error))
			{
				Close();
			}
		}

		if (!Error.IsEmpty())
		{
			UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: %s"), *Error);
			if (OutError)
			{
				*OutError = Error;
			}
			return false;
		}
		return true;
	}

	void FDreamFXDatabase::Close()
	{
		if (Database.IsValid())
		{
			Database.Close();
		}
	}

	bool FDreamFXDatabase::EnsureSchema(FString& OutError)
	{
		if (!Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);")))
		{
			OutError = DescribeError(TEXT("creating meta"));
			return false;
		}

		const FString Expected = FString::FromInt(SchemaVersion);
		const FString Existing = GetMeta(TEXT("schema"));
		if (!Existing.IsEmpty() && Existing != Expected)
		{
			if (Kind == EKind::Index)
			{
				// Never dropped: the quarantine list in here is what a crashed engine taught us, one crash per row.
				// A newer schema means a newer plugin wrote it; an older one needs a migration this build would have.
				OutError = FString::Printf(
					TEXT("'%s' has schema %s; this build of DreamFX writes %d. Update the DreamFX plugin -- the index is left as it is."),
					*Path, *Existing, SchemaVersion);
				return false;
			}
			UE_LOG(LogDreamFX, Display, TEXT("DreamFX: '%s' has schema %s; this build writes %d. Rebuilding it."),
				*Path, *Existing, SchemaVersion);
			for (const TCHAR* Table : DatabaseLocal::SessionTables)
			{
				Database.Execute(*FString::Printf(TEXT("DROP TABLE IF EXISTS %s;"), Table));
			}
			Database.Execute(TEXT("DELETE FROM meta;"));
		}

		bool bCreated = false;
		if (Kind == EKind::Session)
		{
			bCreated = Database.Execute(
				TEXT("CREATE TABLE IF NOT EXISTS diagnostics(seq INTEGER PRIMARY KEY, file TEXT NOT NULL, json TEXT NOT NULL);"));
		}
		else
		{
			bCreated =
				Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS modules(ord INTEGER PRIMARY KEY, path TEXT NOT NULL, name TEXT NOT NULL, ")
					TEXT("kind TEXT NOT NULL, json TEXT NOT NULL);"))
				&& Database.Execute(TEXT("CREATE INDEX IF NOT EXISTS modules_name ON modules(name);"))
				&& Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS probe_journal(id INTEGER PRIMARY KEY CHECK (id = 1), ")
					TEXT("package TEXT NOT NULL, started_utc TEXT NOT NULL);"))
				&& Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS quarantine(package TEXT PRIMARY KEY, quarantined_utc TEXT NOT NULL);"));
		}
		if (!bCreated)
		{
			OutError = DescribeError(TEXT("creating the schema"));
			return false;
		}

		if (Existing != Expected)
		{
			SetMeta(TEXT("schema"), Expected);
		}
		const FString CurrentProducer = DatabaseLocal::Producer();
		if (GetMeta(TEXT("producer")) != CurrentProducer)
		{
			SetMeta(TEXT("producer"), CurrentProducer);
		}
		return true;
	}

	bool FDreamFXDatabase::Begin()
	{
		return Database.Execute(TEXT("BEGIN IMMEDIATE;"));
	}

	bool FDreamFXDatabase::Commit()
	{
		return Database.Execute(TEXT("COMMIT;"));
	}

	void FDreamFXDatabase::Rollback()
	{
		Database.Execute(TEXT("ROLLBACK;"));
	}

	bool FDreamFXDatabase::Execute(const TCHAR* InStatement)
	{
		return Database.Execute(InStatement);
	}

	FString FDreamFXDatabase::GetMeta(const TCHAR* InKey)
	{
		FString Value;
		FSQLitePreparedStatement Statement(Database, TEXT("SELECT value FROM meta WHERE key = ?1;"));
		if (Statement.IsValid() && Statement.SetBindingValueByIndex(1, FString(InKey))
			&& Statement.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Statement.GetColumnValueByIndex(0, Value);
		}
		return Value;
	}

	bool FDreamFXDatabase::SetMeta(const TCHAR* InKey, const FString& InValue)
	{
		FSQLitePreparedStatement Statement(Database, TEXT("INSERT OR REPLACE INTO meta(key, value) VALUES(?1, ?2);"));
		return Statement.IsValid()
			&& Statement.SetBindingValueByIndex(1, FString(InKey))
			&& Statement.SetBindingValueByIndex(2, InValue)
			&& Statement.Step() == ESQLitePreparedStatementStepResult::Done;
	}

	bool FDreamFXDatabase::Bump(const TCHAR* InPart, const FString& InUpdatedUtc)
	{
		const auto Next = [this](const FString& InName)
		{
			const int64 Current = FCString::Atoi64(*GetMeta(*InName));
			return SetMeta(*InName, LexToString(Current + 1));
		};
		return Next(FString::Printf(TEXT("%s_revision"), InPart))
			&& Next(TEXT("revision"))
			&& SetMeta(*FString::Printf(TEXT("%s_updated_utc"), InPart),
				InUpdatedUtc.IsEmpty() ? FDateTime::UtcNow().ToIso8601() : InUpdatedUtc);
	}

	FString FDreamFXDatabase::DescribeError(const TCHAR* InWhat) const
	{
		return FString::Printf(TEXT("'%s': %s failed: %s"), *Path, InWhat, *Database.GetLastError());
	}

	bool FDreamFXDatabase::WriteDiagnostics(const FString& InDatabasePath, const TArray<FDiagnosticRow>& InRows,
		const FString& InGeneratedUtc)
	{
		FDreamFXDatabase Database;
		if (!Database.Open(InDatabasePath, EKind::Session))
		{
			return false;
		}
		if (!Database.Begin())
		{
			UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: %s"), *Database.DescribeError(TEXT("BEGIN")));
			return false;
		}

		bool bOk = Database.Execute(TEXT("DELETE FROM diagnostics;"));
		if (bOk)
		{
			FSQLitePreparedStatement Insert(Database.Get(), TEXT("INSERT INTO diagnostics(seq, file, json) VALUES(?1, ?2, ?3);"),
				ESQLitePreparedStatementFlags::Persistent);
			bOk = Insert.IsValid();
			for (int32 Index = 0; bOk && Index < InRows.Num(); ++Index)
			{
				bOk = Insert.SetBindingValueByIndex(1, static_cast<int64>(Index))
					&& Insert.SetBindingValueByIndex(2, InRows[Index].File)
					&& Insert.SetBindingValueByIndex(3, InRows[Index].Json)
					&& Insert.Step() == ESQLitePreparedStatementStepResult::Done;
				Insert.Reset();
				Insert.ClearBindings();
			}
		}
		bOk = bOk && Database.Bump(TEXT("diagnostics"), InGeneratedUtc) && Database.Commit();
		if (!bOk)
		{
			UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: %s"), *Database.DescribeError(TEXT("writing diagnostics")));
			Database.Rollback();
		}
		return bOk;
	}

	bool FDreamFXDatabase::ReadDiagnostics(const FString& InDatabasePath, TArray<FDiagnosticRow>& OutRows, FString* OutGeneratedUtc)
	{
		OutRows.Reset();
		if (!FPaths::FileExists(InDatabasePath))
		{
			return false;
		}
		FDreamFXDatabase Database;
		if (!Database.Open(InDatabasePath, EKind::Session))
		{
			return false;
		}
		if (OutGeneratedUtc)
		{
			*OutGeneratedUtc = Database.GetMeta(TEXT("diagnostics_updated_utc"));
		}
		FSQLitePreparedStatement Select(Database.Get(), TEXT("SELECT file, json FROM diagnostics ORDER BY seq;"));
		if (!Select.IsValid())
		{
			return false;
		}
		while (Select.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FDiagnosticRow& Row = OutRows.AddDefaulted_GetRef();
			Select.GetColumnValueByIndex(0, Row.File);
			Select.GetColumnValueByIndex(1, Row.Json);
		}
		return true;
	}
}
