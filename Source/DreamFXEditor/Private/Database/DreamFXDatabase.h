#pragma once

#include "CoreMinimal.h"
#include "SQLiteDatabase.h"

namespace UE::DreamFX::Editor
{
	/**
	 * One of the two SQLite files DreamFX writes for tools that are not this editor (Plan/DreamSource, sections 13.3
	 * and 13.4). Both are a contract with dreamfx-language-support and dream-mcp:
	 *
	 *   Intermediate/DreamSource/fx.db           (EKind::Session -- rebuilt by the editor, so disposable)
	 *     meta(key, value)                       schema, producer, revision, diagnostics_revision, diagnostics_updated_utc
	 *     diagnostics(seq, file, json)           the bridge's last response, one diagnostic per row, in order
	 *
	 *   <source directory>/.dreamfx/index.db     (EKind::Index -- only `dfx index` rebuilds it, and the quarantine
	 *                                            list cost a crashed engine per row, so it is never dropped)
	 *     meta(key, value)                       schema, producer, revision, modules_revision, modules_updated_utc,
	 *                                            and the old file's header: version, generated_utc, project,
	 *                                            inputs_probed, engine, plugins, search_paths
	 *     modules(ord, path, name, kind, json)   one module or dynamic input per row, in the old array's order
	 *     probe_journal(id = 1, package, ...)    the module being probed right now, if any
	 *     quarantine(package, quarantined_utc)   the modules whose probe ended a run
	 *
	 * A schema this build does not know: fx.db drops its tables and starts over (never deletes the file -- on Windows a
	 * reader holding it open makes a delete fail); index.db refuses to open instead of losing the quarantine.
	 */
	class FDreamFXDatabase
	{
	public:
		enum class EKind : uint8
		{
			Session,
			Index,
		};

		/** The schema this build writes, for either file. Readers compare it with meta.schema. */
		static constexpr int32 SchemaVersion = 1;

		FDreamFXDatabase() = default;
		~FDreamFXDatabase();

		FDreamFXDatabase(const FDreamFXDatabase&) = delete;
		FDreamFXDatabase& operator=(const FDreamFXDatabase&) = delete;

		/** Opens (creating the directory and file), sets the pragmas, makes the schema current. False, with OutError, on failure. */
		bool Open(const FString& InPath, EKind InKind, FString* OutError = nullptr);
		void Close();
		bool IsOpen() const { return Database.IsValid(); }

		FSQLiteDatabase& Get() { return Database; }

		bool Begin();
		bool Commit();
		void Rollback();

		/** Runs one statement with no result rows. */
		bool Execute(const TCHAR* InStatement);

		/** Empty when the key is not there. */
		FString GetMeta(const TCHAR* InKey);
		bool SetMeta(const TCHAR* InKey, const FString& InValue);

		/** Adds one to `<InPart>_revision` and to `revision`, and stamps `<InPart>_updated_utc`. Inside the caller's transaction. */
		bool Bump(const TCHAR* InPart, const FString& InUpdatedUtc = FString());

		/** Path + statement + error, for a log line. */
		FString DescribeError(const TCHAR* InWhat) const;

		/** One diagnostic as fx.db stores it. */
		struct FDiagnosticRow
		{
			FString File;
			FString Json;
		};

		/** Replaces fx.db's `diagnostics` in one transaction. False, logged, on failure. */
		static bool WriteDiagnostics(const FString& InDatabasePath, const TArray<FDiagnosticRow>& InRows, const FString& InGeneratedUtc);

		/** fx.db's `diagnostics`, in order. For tests and tools. */
		static bool ReadDiagnostics(const FString& InDatabasePath, TArray<FDiagnosticRow>& OutRows, FString* OutGeneratedUtc = nullptr);

	private:
		bool EnsureSchema(FString& OutError);

		FSQLiteDatabase Database;
		FString Path;
		EKind Kind = EKind::Session;
	};
}
