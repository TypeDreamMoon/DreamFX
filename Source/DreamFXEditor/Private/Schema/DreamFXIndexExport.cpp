#include "Schema/DreamFXIndexExport.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXTypes.h"
#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Database/DreamFXDatabase.h"
#include "Dom/JsonObject.h"
#include "Generation/DreamFXValueLowering.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SourceFiles/DreamFXPaths.h"
#include "SQLitePreparedStatement.h"

using namespace UE::DreamFX;

namespace UE::DreamFX::Editor
{
	namespace IndexExportLocal
	{
		// ---------------------------------------------------------------- index export

		/**
		 * The six stacks, in the order the schema probe should try them.
		 *
		 * Mirrors the adapter's own ScriptUsageForStack, which lives in an anonymous namespace there.
		 * Six lines of duplication rather than a new export, and the mapping is not one that can drift
		 * quietly: getting it wrong makes the probe ask for a stack the module does not declare, which
		 * fails loudly rather than producing a plausible wrong answer.
		 */
		struct FStackUsage
		{
			EStackKind Kind;
			ENiagaraScriptUsage Usage;
		};

		const FStackUsage IndexStacks[] = {
			{ EStackKind::ParticleUpdate, ENiagaraScriptUsage::ParticleUpdateScript },
			{ EStackKind::ParticleSpawn,  ENiagaraScriptUsage::ParticleSpawnScript  },
			{ EStackKind::EmitterUpdate,  ENiagaraScriptUsage::EmitterUpdateScript  },
			{ EStackKind::EmitterSpawn,   ENiagaraScriptUsage::EmitterSpawnScript   },
			{ EStackKind::SystemUpdate,   ENiagaraScriptUsage::SystemUpdateScript   },
			{ EStackKind::SystemSpawn,    ENiagaraScriptUsage::SystemSpawnScript    },
		};

		/** The pre-DreamSource file names, beside (and later inside) the source directory. */
		const TCHAR* const LegacyIndexFileName = TEXT(".dfx-index.json");
		const TCHAR* const JournalSuffix = TEXT(".inflight");
		const TCHAR* const QuarantineSuffix = TEXT(".quarantine");

		/** Every enum entry, spelled the way an author writes it. Empty for a non-enum type. */
		void CollectEnumEntries(const FNiagaraTypeDefinition& Type, TArray<FString>& OutEntries)
		{
			const UEnum* Enum = Type.GetEnum();
			if (Enum == nullptr)
			{
				return;
			}

			// The trailing entry of a UENUM is the generated _MAX sentinel; it is not a real value.
			const int32 Count = Enum->NumEnums() - 1;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				// Through the generator's own spelling function, not the raw name: a user-defined enum
				// asset stores NewEnumerator0 internally and keeps the real name in display text, and the
				// generator is the thing that decides which of the two a source file may write.
				const FString Token = FValueLowering::EnumEntryToSourceToken(Enum, Index);
				if (!Token.IsEmpty())
				{
					OutEntries.AddUnique(Token);
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FString& Value : Values)
			{
				Out.Add(MakeShared<FJsonValueString>(Value));
			}
			return Out;
		}

		TSharedRef<FJsonObject> InputSchemaToJson(const FInputSchema& Input)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), ToInputIdentifier(Input.Name));
			Object->SetStringField(TEXT("type"), Input.Type.GetName());
			if (Input.bIsStaticSwitch)
			{
				// Worth its own flag rather than a note in the type: source order is write order on a
				// module, so a switch has to be written before whatever it gates.
				Object->SetBoolField(TEXT("staticSwitch"), true);
			}
			if (Input.bSupportsExpressions)
			{
				Object->SetBoolField(TEXT("expressions"), true);
			}
			if (!Input.Category.IsEmpty())
			{
				Object->SetStringField(TEXT("category"), Input.Category);
			}
			if (!Input.Description.IsEmpty())
			{
				Object->SetStringField(TEXT("description"), Input.Description);
			}

			TArray<FString> EnumEntries;
			CollectEnumEntries(Input.Type, EnumEntries);
			if (EnumEntries.Num() > 0)
			{
				Object->SetArrayField(TEXT("enum"), ToJsonStrings(EnumEntries));
			}
			return Object;
		}

		FString ToCondensedJson(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
		}

		FString ToCondensedJson(const TArray<FString>& Values)
		{
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(ToJsonStrings(Values), Writer);
			return Text;
		}

		/** What one run found, before it is stored: the old file's header, and its modules in order. */
		struct FIndexContent
		{
			FString GeneratedUtc;
			FString Project;
			bool bInputsProbed = false;
			bool bHasInputsProbed = true;
			FString Engine;
			TArray<FString> Plugins;
			TArray<FString> SearchPaths;
			TArray<TSharedRef<FJsonObject>> Modules;
		};

		/** Header + modules into an open index database, in one transaction. */
		bool StoreContent(FDreamFXDatabase& Database, const FIndexContent& Content)
		{
			if (!Database.Begin())
			{
				return false;
			}
			bool bOk = Database.Execute(TEXT("DELETE FROM modules;"));
			if (bOk)
			{
				FSQLitePreparedStatement Insert(Database.Get(),
					TEXT("INSERT INTO modules(ord, path, name, kind, json) VALUES(?1, ?2, ?3, ?4, ?5);"),
					ESQLitePreparedStatementFlags::Persistent);
				bOk = Insert.IsValid();
				for (int32 Index = 0; bOk && Index < Content.Modules.Num(); ++Index)
				{
					const TSharedRef<FJsonObject>& Module = Content.Modules[Index];
					Insert.Reset();
					Insert.ClearBindings();
					bOk = Insert.SetBindingValueByIndex(1, static_cast<int64>(Index))
						&& Insert.SetBindingValueByIndex(2, Module->GetStringField(TEXT("path")))
						&& Insert.SetBindingValueByIndex(3, Module->GetStringField(TEXT("name")))
						&& Insert.SetBindingValueByIndex(4, Module->GetStringField(TEXT("kind")))
						&& Insert.SetBindingValueByIndex(5, ToCondensedJson(Module))
						&& Insert.Step() == ESQLitePreparedStatementStepResult::Done;
				}
			}

			bOk = bOk
				&& Database.SetMeta(TEXT("version"), TEXT("1"))
				&& Database.SetMeta(TEXT("generated_utc"), Content.GeneratedUtc)
				&& Database.SetMeta(TEXT("project"), Content.Project)
				&& Database.SetMeta(TEXT("engine"), Content.Engine)
				&& Database.SetMeta(TEXT("plugins"), ToCondensedJson(Content.Plugins))
				&& Database.SetMeta(TEXT("search_paths"), ToCondensedJson(Content.SearchPaths));
			if (bOk && Content.bHasInputsProbed)
			{
				bOk = Database.SetMeta(TEXT("inputs_probed"), Content.bInputsProbed ? TEXT("true") : TEXT("false"));
			}
			else if (bOk)
			{
				// An imported file from before the flag existed: absent, which is not the same as false.
				bOk = Database.Execute(TEXT("DELETE FROM meta WHERE key = 'inputs_probed';"));
			}
			bOk = bOk && Database.Bump(TEXT("modules"), Content.GeneratedUtc) && Database.Commit();
			if (!bOk)
			{
				UE_LOG(LogDreamFX, Error, TEXT("DreamFX: %s"), *Database.DescribeError(TEXT("writing the module index")));
				Database.Rollback();
			}
			return bOk;
		}

		/** The old JSON file's text: the same shape the language tools read before the index moved into SQLite. */
		FString ContentToJsonFile(const FIndexContent& Content)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("version"), 1);
			Root->SetStringField(TEXT("generatedUtc"), Content.GeneratedUtc);
			Root->SetStringField(TEXT("project"), Content.Project);
			Root->SetBoolField(TEXT("inputsProbed"), Content.bInputsProbed);
			Root->SetStringField(TEXT("engine"), Content.Engine);
			Root->SetArrayField(TEXT("plugins"), ToJsonStrings(Content.Plugins));
			Root->SetArrayField(TEXT("searchPaths"), ToJsonStrings(Content.SearchPaths));
			TArray<TSharedPtr<FJsonValue>> Modules;
			for (const TSharedRef<FJsonObject>& Module : Content.Modules)
			{
				Modules.Add(MakeShared<FJsonValueObject>(Module));
			}
			Root->SetArrayField(TEXT("modules"), Modules);

			FString Text;
			FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
			return Text;
		}

		/**
		 * The set of modules a previous run started probing and never finished.
		 *
		 * Measured, on stock engine content: `/Niagara/Modules/Masks/ConeMask` makes the engine's own
		 * `UNiagaraGraph::ReferencesStaticVariable` recurse without bound, and the resulting stack
		 * overflow takes the process with it -- `dfx schema` on that module has always done this, long
		 * before there was an index to walk. A plugin cannot add a visited-set to engine code, and a
		 * stack overflow is not catchable in any way worth trusting inside a process holding engine
		 * state. So the walk is made *resumable* instead: the journal records the intent to probe before
		 * probing, and a name that is still open when the next run starts is a name that killed the last
		 * one. The driver re-runs until the walk completes, and each run quarantines one more.
		 *
		 * Two records, with one job between them. The journal holds at most one entry: the module about
		 * to be probed, overwritten each time. The quarantine list accumulates the ones that were still
		 * in the journal when the next run started -- which is the definition of "this module ended the
		 * process". Split that way because a single append-only log would need the walk to record a
		 * completion for every module, and the one place it can never record anything is the case this
		 * exists for.
		 *
		 * Kept either in the index database (probe_journal / quarantine, written with synchronous=FULL so
		 * the journal is on disk before the probe that may end the process), or -- for an explicit
		 * `-Out=<file>` -- in the two files beside that file, as before.
		 */
		class FProbeLedger
		{
		public:
			/** Database mode. */
			explicit FProbeLedger(FDreamFXDatabase& InDatabase) : Database(&InDatabase) {}

			/** File mode, for `-Out=<file>`. */
			explicit FProbeLedger(const FString& InOutputPath)
				: JournalPath(InOutputPath + JournalSuffix)
				, QuarantinePath(InOutputPath + QuarantineSuffix)
			{
			}

			void Clear()
			{
				if (Database)
				{
					Database->Execute(TEXT("DELETE FROM probe_journal;"));
					Database->Execute(TEXT("DELETE FROM quarantine;"));
					return;
				}
				IFileManager::Get().Delete(*JournalPath);
				IFileManager::Get().Delete(*QuarantinePath);
			}

			/** Promotes an unfinished probe into the quarantine list, and returns everything quarantined. */
			TSet<FString> CollectQuarantined()
			{
				return Database ? CollectFromDatabase() : CollectFromFiles();
			}

			/**
			 * Written BEFORE the probe, not after, and that ordering is the whole mechanism: nothing that
			 * runs after the probe gets a chance to name the module that killed the process.
			 */
			void MarkInFlight(const FString& PackageName)
			{
				if (Database)
				{
					FSQLitePreparedStatement Statement(Database->Get(),
						TEXT("INSERT OR REPLACE INTO probe_journal(id, package, started_utc) VALUES(1, ?1, ?2);"));
					if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, PackageName)
						|| !Statement.SetBindingValueByIndex(2, FDateTime::UtcNow().ToIso8601())
						|| Statement.Step() != ESQLitePreparedStatementStepResult::Done)
					{
						UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: %s"), *Database->DescribeError(TEXT("recording the probe")));
					}
					return;
				}
				FFileHelper::SaveStringToFile(PackageName, *JournalPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}

			/** The walk finished, so nothing is mid-probe. The quarantine stays: it is what makes the next run fast. */
			void ClearInFlight()
			{
				if (Database)
				{
					Database->Execute(TEXT("DELETE FROM probe_journal;"));
					return;
				}
				IFileManager::Get().Delete(*JournalPath);
			}

		private:
			TSet<FString> CollectFromFiles()
			{
				TSet<FString> Quarantined;

				FString Existing;
				if (FFileHelper::LoadFileToString(Existing, *QuarantinePath))
				{
					TArray<FString> Lines;
					Existing.ParseIntoArrayLines(Lines);
					Quarantined.Append(Lines);
				}

				FString InFlight;
				if (FFileHelper::LoadFileToString(InFlight, *JournalPath))
				{
					InFlight.TrimStartAndEndInline();
					if (!InFlight.IsEmpty() && !Quarantined.Contains(InFlight))
					{
						UE_LOG(LogDreamFX, Warning,
							TEXT("'%s' ended the previous index run without returning; quarantining it."), *InFlight);
						Quarantined.Add(InFlight);

						TArray<FString> Sorted = Quarantined.Array();
						Sorted.Sort();
						FFileHelper::SaveStringToFile(FString::Join(Sorted, TEXT("\n")), *QuarantinePath,
							FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					}
					IFileManager::Get().Delete(*JournalPath);
				}

				return Quarantined;
			}

			TSet<FString> CollectFromDatabase()
			{
				FString InFlight;
				{
					FSQLitePreparedStatement Select(Database->Get(), TEXT("SELECT package FROM probe_journal WHERE id = 1;"));
					if (Select.IsValid() && Select.Step() == ESQLitePreparedStatementStepResult::Row)
					{
						Select.GetColumnValueByIndex(0, InFlight);
					}
				}
				InFlight.TrimStartAndEndInline();

				TSet<FString> Quarantined;
				{
					FSQLitePreparedStatement Select(Database->Get(), TEXT("SELECT package FROM quarantine;"));
					while (Select.IsValid() && Select.Step() == ESQLitePreparedStatementStepResult::Row)
					{
						FString Package;
						Select.GetColumnValueByIndex(0, Package);
						Quarantined.Add(Package);
					}
				}

				if (!InFlight.IsEmpty())
				{
					if (!Quarantined.Contains(InFlight))
					{
						UE_LOG(LogDreamFX, Warning,
							TEXT("'%s' ended the previous index run without returning; quarantining it."), *InFlight);
						FSQLitePreparedStatement Insert(Database->Get(),
							TEXT("INSERT OR IGNORE INTO quarantine(package, quarantined_utc) VALUES(?1, ?2);"));
						if (!Insert.IsValid() || !Insert.SetBindingValueByIndex(1, InFlight)
							|| !Insert.SetBindingValueByIndex(2, FDateTime::UtcNow().ToIso8601())
							|| Insert.Step() != ESQLitePreparedStatementStepResult::Done)
						{
							UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: %s"), *Database->DescribeError(TEXT("quarantining")));
						}
						Quarantined.Add(InFlight);
					}
					Database->Execute(TEXT("DELETE FROM probe_journal;"));
				}
				return Quarantined;
			}

			FDreamFXDatabase* Database = nullptr;
			FString JournalPath;
			FString QuarantinePath;
		};

		/** Reads one legacy `.dfx-index.json` into content. False when it is not there or not an index. */
		bool ReadLegacyIndexFile(const FString& Path, FIndexContent& OutContent)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Root;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
			{
				return false;
			}
			Root->TryGetStringField(TEXT("generatedUtc"), OutContent.GeneratedUtc);
			Root->TryGetStringField(TEXT("project"), OutContent.Project);
			Root->TryGetStringField(TEXT("engine"), OutContent.Engine);
			OutContent.bHasInputsProbed = Root->TryGetBoolField(TEXT("inputsProbed"), OutContent.bInputsProbed);
			Root->TryGetStringArrayField(TEXT("plugins"), OutContent.Plugins);
			Root->TryGetStringArrayField(TEXT("searchPaths"), OutContent.SearchPaths);

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (!Root->TryGetArrayField(TEXT("modules"), Modules))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Modules)
			{
				const TSharedPtr<FJsonObject>* Module = nullptr;
				if (Value.IsValid() && Value->TryGetObject(Module) && Module && Module->IsValid())
				{
					OutContent.Modules.Add(Module->ToSharedRef());
				}
			}
			return true;
		}

		int64 CountModules(FDreamFXDatabase& Database)
		{
			int64 Count = 0;
			FSQLitePreparedStatement Select(Database.Get(), TEXT("SELECT COUNT(*) FROM modules;"));
			if (Select.IsValid() && Select.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Select.GetColumnValueByIndex(0, Count);
			}
			return Count;
		}

		/** ImportLegacyIndex, on a database the caller already holds open. */
		bool ImportLegacyInto(FDreamFXDatabase& Database, const FString& SourceDirectory, FString& OutError)
		{
			const FString IndexFile = FPaths::Combine(SourceDirectory, LegacyIndexFileName);
			const FString JournalFile = IndexFile + JournalSuffix;
			const FString QuarantineFile = IndexFile + QuarantineSuffix;
			IFileManager& Files = IFileManager::Get();
			const bool bIndex = Files.FileExists(*IndexFile);
			const bool bJournal = Files.FileExists(*JournalFile);
			const bool bQuarantine = Files.FileExists(*QuarantineFile);
			if (!bIndex && !bJournal && !bQuarantine)
			{
				return true;
			}

			// The quarantine first, and whatever was in flight with it: both mean "this module ended a run".
			TArray<FString> Quarantined;
			FString Text;
			if (bQuarantine && FFileHelper::LoadFileToString(Text, *QuarantineFile))
			{
				Text.ParseIntoArrayLines(Quarantined);
			}
			if (bJournal && FFileHelper::LoadFileToString(Text, *JournalFile))
			{
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					Quarantined.Add(Text);
				}
			}

			if (!Database.Begin())
			{
				OutError = Database.DescribeError(TEXT("BEGIN"));
				return false;
			}
			bool bOk = true;
			{
				FSQLitePreparedStatement Insert(Database.Get(),
					TEXT("INSERT OR IGNORE INTO quarantine(package, quarantined_utc) VALUES(?1, ?2);"),
					ESQLitePreparedStatementFlags::Persistent);
				bOk = Insert.IsValid();
				const FString Now = FDateTime::UtcNow().ToIso8601();
				for (int32 Index = 0; bOk && Index < Quarantined.Num(); ++Index)
				{
					const FString Package = Quarantined[Index].TrimStartAndEnd();
					if (Package.IsEmpty())
					{
						continue;
					}
					Insert.Reset();
					Insert.ClearBindings();
					bOk = Insert.SetBindingValueByIndex(1, Package)
						&& Insert.SetBindingValueByIndex(2, Now)
						&& Insert.Step() == ESQLitePreparedStatementStepResult::Done;
				}
			}
			bOk = bOk && Database.Commit();
			if (!bOk)
			{
				OutError = Database.DescribeError(TEXT("importing the quarantine"));
				Database.Rollback();
				return false;
			}

			// The modules only into an empty index: a database that already has some is newer than the file.
			bool bIndexConsumed = !bIndex;
			if (bIndex)
			{
				FIndexContent Content;
				if (CountModules(Database) > 0)
				{
					bIndexConsumed = true;
				}
				else if (ReadLegacyIndexFile(IndexFile, Content))
				{
					bIndexConsumed = StoreContent(Database, Content);
					if (!bIndexConsumed)
					{
						OutError = Database.DescribeError(TEXT("importing the module index"));
						return false;
					}
				}
				else
				{
					UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: '%s' is not a module index; it is left where it is."), *IndexFile);
				}
			}

			Files.Delete(*JournalFile);
			Files.Delete(*QuarantineFile);
			if (bIndexConsumed)
			{
				Files.Delete(*IndexFile);
			}
			UE_LOG(LogDreamFX, Display, TEXT("DreamFX: imported the index files in '%s' into '%s'."),
				*SourceDirectory, *FIndexExport::GetIndexDatabasePath(SourceDirectory));
			return true;
		}
	}

	FString FIndexExport::GetIndexDatabasePath(const FString& SourceDirectory)
	{
		return FPaths::Combine(SourceDirectory, TEXT(".dreamfx/index.db"));
	}

	bool FIndexExport::ImportLegacyIndex(const FString& SourceDirectory, FString& OutError)
	{
		using namespace IndexExportLocal;
		const FString IndexFile = FPaths::Combine(SourceDirectory, LegacyIndexFileName);
		IFileManager& Files = IFileManager::Get();
		if (!Files.FileExists(*IndexFile) && !Files.FileExists(*(IndexFile + JournalSuffix))
			&& !Files.FileExists(*(IndexFile + QuarantineSuffix)))
		{
			return true;
		}
		FDreamFXDatabase Database;
		if (!Database.Open(GetIndexDatabasePath(SourceDirectory), FDreamFXDatabase::EKind::Index, &OutError))
		{
			return false;
		}
		return ImportLegacyInto(Database, SourceDirectory, OutError);
	}

	/**
	 * Exports every module and dynamic input the search paths expose, with its input signature.
	 *
	 * One command rather than the `-Json` flags the plan first proposed, and the reason is arithmetic:
	 * a per-module `schema -Json` would cost one engine boot each, and there are hundreds of modules.
	 * Anything an editor integration wants to do per keystroke has to read stored data, so the data has
	 * to be producible in a single boot.
	 *
	 * Two costs are traded deliberately. The stacks a module may sit in are read from its declared
	 * usage bitmask, which is free -- it is the same list the engine's own stack UI filters by. The
	 * input signature is *probed*, because the asset-level schema misses inline edit conditions and
	 * static switches entirely, and a completion list missing every enum-shaped input would be worse
	 * than none. The probe runs once per module, in the first stack the module says it belongs to.
	 */
	int32 FIndexExport::Run(const FString& OutputPath, bool bSkipInputs, bool bRetryQuarantined, FString& OutDestination)
	{
		using namespace IndexExportLocal;
		const double StartedAt = FPlatformTime::Seconds();

		FModuleLibrary Library;

		// Held open for the whole walk: the journal is written once per module, and opening the file for each
		// of several hundred writes would cost more than the probes it guards.
		FDreamFXDatabase Database;
		TUniquePtr<FProbeLedger> Ledger;
		const bool bToFile = !OutputPath.IsEmpty();
		if (bToFile)
		{
			OutDestination = FPaths::ConvertRelativePathToFull(OutputPath);
			Ledger = MakeUnique<FProbeLedger>(OutDestination);
		}
		else
		{
			const FString SourceDirectory = FDreamFXPaths::GetProjectSourceDirectory();
			OutDestination = GetIndexDatabasePath(SourceDirectory);
			FString Error;
			if (!Database.Open(OutDestination, FDreamFXDatabase::EKind::Index, &Error))
			{
				UE_LOG(LogDreamFX, Error, TEXT("Could not open the module index: %s"), *Error);
				return 1;
			}
			// A project upgraded from before the DreamSource layout keeps the quarantine it already paid for.
			if (!ImportLegacyInto(Database, SourceDirectory, Error))
			{
				UE_LOG(LogDreamFX, Warning, TEXT("DreamFX: the old index files were not imported: %s"), *Error);
			}
			Ledger = MakeUnique<FProbeLedger>(Database);
		}

		TSet<FString> Quarantined;
		if (bRetryQuarantined)
		{
			Ledger->Clear();
		}
		else
		{
			Quarantined = Ledger->CollectQuarantined();
		}

		FIndexContent Content;
		Content.GeneratedUtc = FDateTime::UtcNow().ToIso8601();
		Content.Project = FApp::GetProjectName();

		// Whether the signatures in this index were probed at all. Without it, an index written
		// without probing is indistinguishable from one where every module happens to take no
		// inputs -- the same "absent and empty must not look alike" rule the per-module
		// `inputsUnavailable` follows, one level up.
		Content.bInputsProbed = !bSkipInputs;

		// The fingerprint. An index goes stale when the engine changes, when a content plugin is
		// enabled or disabled, or when the search paths move -- and none of those touch the clock, so
		// a timestamp alone would keep serving a wrong answer.
		Content.Engine = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			Content.Plugins.Add(Plugin->GetName());
		}
		Content.Plugins.Sort();
		Content.SearchPaths = Library.GetSearchPaths();

		int32 Probed = 0;
		int32 Unprobed = 0;
		int32 Skipped = 0;

		for (const bool bDynamicInput : { false, true })
		{
			TArray<FModuleLibrary::FModuleListing> Listings;
			Library.ListAvailableDetailed(bDynamicInput, Listings);

			for (const FModuleLibrary::FModuleListing& Listing : Listings)
			{
				const TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
				Module->SetStringField(TEXT("name"), Listing.AssetName);
				Module->SetStringField(TEXT("path"), Listing.PackageName);
				Module->SetStringField(TEXT("kind"), bDynamicInput ? TEXT("dynamicInput") : TEXT("module"));

				int32 UsageBitmask = 0;
				if (Listing.Script != nullptr)
				{
					Listing.Script->CheckVersionDataAvailable();
					if (const FVersionedNiagaraScriptData* Data = Listing.Script->GetLatestScriptData())
					{
						UsageBitmask = Data->ModuleUsageBitmask;
						if (!Data->Category.IsEmpty())
						{
							Module->SetStringField(TEXT("category"), Data->Category.ToString());
						}
						if (!Data->Description.IsEmpty())
						{
							Module->SetStringField(TEXT("description"), Data->Description.ToString());
						}
					}
				}

				TArray<EStackKind> DeclaredStacks;
				TArray<FString> StackNames;
				for (const FStackUsage& Stack : IndexStacks)
				{
					if ((UsageBitmask & (1 << static_cast<int32>(Stack.Usage))) != 0)
					{
						DeclaredStacks.Add(Stack.Kind);
						StackNames.Add(LexStackKind(Stack.Kind));
					}
				}
				Module->SetArrayField(TEXT("stacks"), ToJsonStrings(StackNames));

				if (Quarantined.Contains(Listing.PackageName))
				{
					++Skipped;
					Module->SetStringField(TEXT("inputsUnavailable"),
						TEXT("probing this module ended a previous run without returning; it is quarantined"));
				}
				else if (!bSkipInputs && Listing.Script != nullptr)
				{
					Ledger->MarkInFlight(Listing.PackageName);
					UE_LOG(LogDreamFX, Verbose, TEXT("  probing %s (%s)"), *Listing.AssetName, *Listing.PackageName);

					FString Error;
					const FModuleSchema* Schema = nullptr;

					if (bDynamicInput)
					{
						// A dynamic input is not added to a stack, so there is no live topology to
						// probe and the asset-level schema is the whole answer.
						Schema = Library.GetDynamicInputSchema(Listing.Script, Error);
					}
					else
					{
						for (EStackKind Stack : DeclaredStacks)
						{
							Schema = Library.GetStackSchema(Listing.Script, Stack, Error);
							if (Schema != nullptr)
							{
								Module->SetStringField(TEXT("probedIn"), LexStackKind(Stack));
								break;
							}
						}
					}

					if (Schema != nullptr)
					{
						++Probed;
						TArray<TSharedPtr<FJsonValue>> Inputs;
						for (const FInputSchema& Input : Schema->Inputs)
						{
							Inputs.Add(MakeShared<FJsonValueObject>(InputSchemaToJson(Input)));
						}
						Module->SetArrayField(TEXT("inputs"), Inputs);
					}
					else
					{
						// Named rather than dropped: a module with no inputs and a module that could
						// not be probed look identical from the outside, and a consumer that cannot
						// tell them apart will present the second as the first.
						++Unprobed;
						Module->SetStringField(TEXT("inputsUnavailable"),
							Error.IsEmpty() ? TEXT("the module declares no stack this build supports") : *Error);
					}
				}

				Content.Modules.Add(Module);
			}
		}

		int64 WrittenBytes = 0;
		if (bToFile)
		{
			const FString IndexText = ContentToJsonFile(Content);
			if (!FFileHelper::SaveStringToFile(IndexText, *OutDestination, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogDreamFX, Error, TEXT("Could not write the index to '%s'."), *OutDestination);
				return 1;
			}
			WrittenBytes = IndexText.Len();
		}
		else if (!StoreContent(Database, Content))
		{
			return 1;
		}

		// The walk finished, so nothing is mid-probe. The quarantine list stays -- it is the record
		// of which modules cannot be probed on this engine, and it is what makes the next run fast.
		Ledger->ClearInFlight();

		UE_LOG(LogDreamFX, Display,
			TEXT("=== DreamFX index: %d probed, %d unprobed, %d quarantined, %d entries%s -> %s (%.1fs) ==="),
			Probed, Unprobed, Skipped, Content.Modules.Num(),
			bToFile ? *FString::Printf(TEXT(", %.1f KB"), WrittenBytes / 1024.0f) : TEXT(""),
			*OutDestination, FPlatformTime::Seconds() - StartedAt);
		return 0;
	}
}
