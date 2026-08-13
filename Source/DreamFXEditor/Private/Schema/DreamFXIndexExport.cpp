#include "Schema/DreamFXIndexExport.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXTypes.h"
#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Generation/DreamFXValueLowering.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "Serialization/JsonWriter.h"

using namespace UE::DreamFX;

namespace UE::DreamFX::Editor
{
	namespace
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

		void WriteInputSchema(const TSharedRef<TJsonWriter<>>& Writer, const FInputSchema& Input)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), ToInputIdentifier(Input.Name));
			Writer->WriteValue(TEXT("type"), Input.Type.GetName());
			if (Input.bIsStaticSwitch)
			{
				// Worth its own flag rather than a note in the type: source order is write order on a
				// module, so a switch has to be written before whatever it gates.
				Writer->WriteValue(TEXT("staticSwitch"), true);
			}
			if (Input.bSupportsExpressions)
			{
				Writer->WriteValue(TEXT("expressions"), true);
			}
			if (!Input.Category.IsEmpty())
			{
				Writer->WriteValue(TEXT("category"), Input.Category);
			}
			if (!Input.Description.IsEmpty())
			{
				Writer->WriteValue(TEXT("description"), Input.Description);
			}

			TArray<FString> EnumEntries;
			CollectEnumEntries(Input.Type, EnumEntries);
			if (EnumEntries.Num() > 0)
			{
				Writer->WriteArrayStart(TEXT("enum"));
				for (const FString& Entry : EnumEntries)
				{
					Writer->WriteValue(Entry);
				}
				Writer->WriteArrayEnd();
			}
			Writer->WriteObjectEnd();
		}

		/**
		 * Exports every module and dynamic input the search paths expose, with its input signature.
		 *
		 * One command rather than the `-Json` flags the plan first proposed, and the reason is arithmetic:
		 * a per-module `schema -Json` would cost one engine boot each, and there are hundreds of modules.
		 * Anything an editor integration wants to do per keystroke has to read a file, so the file has to
		 * be producible in a single boot.
		 *
		 * Two costs are traded deliberately. The stacks a module may sit in are read from its declared
		 * usage bitmask, which is free -- it is the same list the engine's own stack UI filters by. The
		 * input signature is *probed*, because the asset-level schema misses inline edit conditions and
		 * static switches entirely, and a completion list missing every enum-shaped input would be worse
		 * than none. The probe runs once per module, in the first stack the module says it belongs to.
		 */
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
		 */
		/**
		 * Two files, with one job between them.
		 *
		 * The journal holds exactly one line: the module about to be probed, overwritten each time. The
		 * quarantine list accumulates the ones that were still in the journal when the next run started
		 * -- which is the definition of "this module ended the process".
		 *
		 * Split that way because a single append-only log would need the walk to record a completion for
		 * every module, and the one place it can never record anything is the case this exists for.
		 */
		void MarkProbeInFlight(const FString& JournalPath, const FString& PackageName)
		{
			FFileHelper::SaveStringToFile(PackageName, *JournalPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		/** Promotes an unfinished probe into the quarantine list, and returns everything quarantined. */
		TSet<FString> CollectQuarantined(const FString& JournalPath, const FString& QuarantinePath)
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

	}

	int32 FIndexExport::Run(const FString& OutputPath, bool bSkipInputs, bool bRetryQuarantined, FString& OutDestination)
	{
		const double StartedAt = FPlatformTime::Seconds();

		FModuleLibrary Library;

		const FString Destination = OutputPath.IsEmpty()
			? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("DFX/.dfx-index.json")))
			: FPaths::ConvertRelativePathToFull(OutputPath);
		OutDestination = Destination;
		const FString JournalPath = Destination + TEXT(".inflight");
		const FString QuarantinePath = Destination + TEXT(".quarantine");

		TSet<FString> Quarantined;
		if (bRetryQuarantined)
		{
			IFileManager::Get().Delete(*JournalPath);
			IFileManager::Get().Delete(*QuarantinePath);
		}
		else
		{
			Quarantined = CollectQuarantined(JournalPath, QuarantinePath);
		}

		FString IndexText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&IndexText);

		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("version"), 1);
		Writer->WriteValue(TEXT("generatedUtc"), FDateTime::UtcNow().ToIso8601());
		Writer->WriteValue(TEXT("project"), FApp::GetProjectName());

		// Whether the signatures in this file were probed at all. Without it, an index written
		// without probing is indistinguishable from one where every module happens to take no
		// inputs -- the same "absent and empty must not look alike" rule the per-module
		// `inputsUnavailable` follows, one level up.
		Writer->WriteValue(TEXT("inputsProbed"), !bSkipInputs);

		// The fingerprint. An index goes stale when the engine changes, when a content plugin is
		// enabled or disabled, or when the search paths move -- and none of those touch the clock, so
		// a timestamp alone would keep serving a wrong answer.
		Writer->WriteValue(TEXT("engine"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));

		Writer->WriteArrayStart(TEXT("plugins"));
		TArray<FString> EnabledPlugins;
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			EnabledPlugins.Add(Plugin->GetName());
		}
		EnabledPlugins.Sort();
		for (const FString& Name : EnabledPlugins)
		{
			Writer->WriteValue(Name);
		}
		Writer->WriteArrayEnd();

		Writer->WriteArrayStart(TEXT("searchPaths"));
		for (const FString& Path : Library.GetSearchPaths())
		{
			Writer->WriteValue(Path);
		}
		Writer->WriteArrayEnd();

		int32 Probed = 0;
		int32 Unprobed = 0;
		int32 Skipped = 0;

		Writer->WriteArrayStart(TEXT("modules"));
		for (const bool bDynamicInput : { false, true })
		{
			TArray<FModuleLibrary::FModuleListing> Listings;
			Library.ListAvailableDetailed(bDynamicInput, Listings);

			for (const FModuleLibrary::FModuleListing& Listing : Listings)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("name"), Listing.AssetName);
				Writer->WriteValue(TEXT("path"), Listing.PackageName);
				Writer->WriteValue(TEXT("kind"), bDynamicInput ? TEXT("dynamicInput") : TEXT("module"));

				int32 UsageBitmask = 0;
				if (Listing.Script != nullptr)
				{
					Listing.Script->CheckVersionDataAvailable();
					if (const FVersionedNiagaraScriptData* Data = Listing.Script->GetLatestScriptData())
					{
						UsageBitmask = Data->ModuleUsageBitmask;
						if (!Data->Category.IsEmpty())
						{
							Writer->WriteValue(TEXT("category"), Data->Category.ToString());
						}
						if (!Data->Description.IsEmpty())
						{
							Writer->WriteValue(TEXT("description"), Data->Description.ToString());
						}
					}
				}

				TArray<EStackKind> DeclaredStacks;
				Writer->WriteArrayStart(TEXT("stacks"));
				for (const FStackUsage& Stack : IndexStacks)
				{
					if ((UsageBitmask & (1 << static_cast<int32>(Stack.Usage))) != 0)
					{
						DeclaredStacks.Add(Stack.Kind);
						Writer->WriteValue(LexStackKind(Stack.Kind));
					}
				}
				Writer->WriteArrayEnd();

				if (Quarantined.Contains(Listing.PackageName))
				{
					++Skipped;
					Writer->WriteValue(TEXT("inputsUnavailable"),
						TEXT("probing this module ended a previous run without returning; it is quarantined"));
				}
				else if (!bSkipInputs && Listing.Script != nullptr)
				{
					// Written BEFORE the probe, not after, and that ordering is the whole mechanism:
					// nothing that runs after the probe gets a chance to name the module that killed
					// the process.
					MarkProbeInFlight(JournalPath, Listing.PackageName);
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
								Writer->WriteValue(TEXT("probedIn"), LexStackKind(Stack));
								break;
							}
						}
					}

					if (Schema != nullptr)
					{
						++Probed;
						Writer->WriteArrayStart(TEXT("inputs"));
						for (const FInputSchema& Input : Schema->Inputs)
						{
							WriteInputSchema(Writer, Input);
						}
						Writer->WriteArrayEnd();
					}
					else
					{
						// Named rather than dropped: a module with no inputs and a module that could
						// not be probed look identical from the outside, and a consumer that cannot
						// tell them apart will present the second as the first.
						++Unprobed;
						Writer->WriteValue(TEXT("inputsUnavailable"),
							Error.IsEmpty() ? TEXT("the module declares no stack this build supports") : *Error);
					}
				}

				Writer->WriteObjectEnd();
			}
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
		Writer->Close();

		if (!FFileHelper::SaveStringToFile(IndexText, *Destination,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogDreamFX, Error, TEXT("Could not write the index to '%s'."), *Destination);
			return 1;
		}

		// The walk finished, so nothing is mid-probe. The quarantine list stays -- it is the record
		// of which modules cannot be probed on this engine, and it is what makes the next run fast.
		IFileManager::Get().Delete(*JournalPath);

		UE_LOG(LogDreamFX, Display,
			TEXT("=== DreamFX index: %d probed, %d unprobed, %d quarantined, %.1f KB -> %s (%.1fs) ==="),
			Probed, Unprobed, Skipped, IndexText.Len() / 1024.0f, *Destination,
			FPlatformTime::Seconds() - StartedAt);
		return 0;
	}
}
