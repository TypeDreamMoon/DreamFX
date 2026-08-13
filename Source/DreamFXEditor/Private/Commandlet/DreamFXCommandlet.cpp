#include "DreamFXCommandlet.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "Diff/DreamFXAssetFacts.h"
#include "Generation/DreamFXGenerator.h"
#include "Lint/DreamFXLint.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Algo/StableSort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Generation/DreamFXValueLowering.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "UObject/UObjectHash.h"

using namespace UE::DreamFX;
using namespace UE::DreamFX::Editor;

namespace
{
	/**
	 * Prints a module's input signature. The DSL's whole type-checking story rests on these names, so
	 * being able to read them without opening the editor is what makes authoring by text practical --
	 * and it is the lookup the diagnose skill needs when a DFX3003 says "no input named X".
	 */
	int32 DumpSchema(const FString& ModuleName, const FString& StackName)
	{
		FModuleLibrary Library;
		FString Error;

		bool bDynamicInput = false;
		UNiagaraScript* Script = Library.FindModule(ModuleName, Error);
		if (Script == nullptr)
		{
			FString DynamicError;
			Script = Library.FindDynamicInput(ModuleName, DynamicError);
			bDynamicInput = Script != nullptr;
		}

		if (Script == nullptr)
		{
			UE_LOG(LogDreamFX, Error, TEXT("%s"), *Error);
			return 1;
		}

		if (bDynamicInput)
		{
			// A dynamic input is not added to a stack, so there is no live topology to probe.
			const FModuleSchema* Schema = Library.GetDynamicInputSchema(Script, Error);
			if (Schema == nullptr)
			{
				UE_LOG(LogDreamFX, Error, TEXT("Could not read schema: %s"), *Error);
				return 1;
			}
			UE_LOG(LogDreamFX, Display, TEXT("DynamicInput '%s' -> %s"), *ModuleName, *Script->GetPathName());
			UE_LOG(LogDreamFX, Display, TEXT("  %d input(s):"), Schema->Inputs.Num());
			for (const FInputSchema& Input : Schema->Inputs)
			{
				UE_LOG(LogDreamFX, Display, TEXT("    %-40s %-24s%s"),
					*ToInputIdentifier(Input.Name), *Input.Type.GetName(),
					Input.bSupportsExpressions ? TEXT(" [hlsl-ok]") : TEXT(""));
			}
			return 0;
		}

		// Probed per stack, because that is what the generator type-checks against: static switches
		// and inline edit conditions only exist on a live module. Reporting the asset-level schema
		// here would show a different, smaller input list than the one a build actually accepts.
		TArray<EStackKind> Candidates;
		EStackKind Requested;
		if (!StackName.IsEmpty() && ParseStackKind(StackName, Requested))
		{
			Candidates.Add(Requested);
		}
		else
		{
			if (!StackName.IsEmpty())
			{
				UE_LOG(LogDreamFX, Warning, TEXT("Unknown stack '%s'; probing every stack instead."), *StackName);
			}
			Candidates = {
				EStackKind::ParticleUpdate, EStackKind::ParticleSpawn,
				EStackKind::EmitterUpdate, EStackKind::EmitterSpawn,
				EStackKind::SystemUpdate, EStackKind::SystemSpawn,
			};
		}

		for (EStackKind Stack : Candidates)
		{
			FString StackError;
			const FModuleSchema* Schema = Library.GetStackSchema(Script, Stack, StackError);
			if (Schema == nullptr)
			{
				continue;
			}

			UE_LOG(LogDreamFX, Display, TEXT("Module '%s' -> %s   [as it appears in %s]"),
				*ModuleName, *Script->GetPathName(), LexStackKind(Stack));
			UE_LOG(LogDreamFX, Display, TEXT("  %d input(s):"), Schema->Inputs.Num());
			for (const FInputSchema& Input : Schema->Inputs)
			{
				UE_LOG(LogDreamFX, Display, TEXT("    %-40s %-24s%s%s"),
					*ToInputIdentifier(Input.Name),
					*Input.Type.GetName(),
					Input.bIsStaticSwitch ? TEXT(" [static-switch]") : TEXT(""),
					Input.bSupportsExpressions ? TEXT(" [hlsl-ok]") : TEXT(""));
			}
			return 0;
		}

		UE_LOG(LogDreamFX, Error,
			TEXT("Module '%s' could not be probed in any stack. It may only be valid in a stack DreamFX does not support yet."),
			*ModuleName);
		return 1;
	}

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

	int32 DumpIndex(const FString& OutputPath, bool bSkipInputs, bool bRetryQuarantined)
	{
		const double StartedAt = FPlatformTime::Seconds();

		FModuleLibrary Library;

		const FString Destination = OutputPath.IsEmpty()
			? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("DFX/.dfx-index.json")))
			: FPaths::ConvertRelativePathToFull(OutputPath);
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

namespace
{
	/**
	 * The content roots a `-Path=` names, or every project mount point when it names none.
	 *
	 * Several are accepted, separated by `+` or `,`: booting the editor is most of what a scan costs,
	 * and plan-v4's four content packs are one question, not four.
	 */
	TArray<FString> ParseContentRoots(const FString& PathSpec)
	{
		TArray<FString> Roots;

		if (!PathSpec.IsEmpty())
		{
			// Both delimiters in one pass: ParseIntoArray empties its output array first, so splitting
			// on '+' and then on ',' in a loop keeps only whatever the last iteration produced.
			const TCHAR* Delimiters[] = { TEXT("+"), TEXT(",") };
			TArray<FString> Parts;
			PathSpec.ParseIntoArray(Parts, Delimiters, UE_ARRAY_COUNT(Delimiters), /*InCullEmpty=*/true);

			for (FString& Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (!Part.IsEmpty())
				{
					Roots.Add(Part.LeftChop(Part.EndsWith(TEXT("/")) ? 1 : 0));
				}
			}

			if (Roots.Num() > 0)
			{
				return Roots;
			}
		}

		// Every mounted content root: plan-v2 W0 asks what fraction of the *project's* VFX round-trips,
		// and a project's effects are as likely to live in a plugin as in /Game. Engine and script
		// mounts are excluded -- /Niagara's own sample systems are not this project's content, and
		// counting them would flatter the number with assets nobody here maintains.
		TArray<FString> MountPoints;
		FPackageName::QueryRootContentPaths(MountPoints);
		for (const FString& MountPoint : MountPoints)
		{
			const FString Trimmed = MountPoint.LeftChop(MountPoint.EndsWith(TEXT("/")) ? 1 : 0);
			if (Trimmed == TEXT("/Engine") || Trimmed == TEXT("/Script") || Trimmed == TEXT("/Temp")
				|| Trimmed.StartsWith(TEXT("/Niagara")))
			{
				continue;
			}
			Roots.Add(Trimmed);
		}
		Roots.Sort();
		return Roots;
	}

	/**
	 * Every Niagara system under those roots, in path order.
	 *
	 * @param bIncludeMirrors  keep assets in the `Decompiled/` namespace. Off for anything that
	 *                         counts or exports, because a mirror is this pipeline's own output:
	 *                         counting it doubles every figure, and exporting it would export an
	 *                         export. On only for MirrorDiff, which is about mirrors.
	 */
	void FindSystems(const TArray<FString>& Roots, const bool bIncludeMirrors, TArray<FAssetData>& OutAssets)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")).Get();

		AssetRegistry.ScanPathsSynchronous(Roots, /*bForceRescan=*/true, /*bIgnoreDenyListScanFilters=*/true);
		AssetRegistry.WaitForCompletion();

		FARFilter Filter;
		Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
		for (const FString& Root : Roots)
		{
			Filter.PackagePaths.Add(FName(*Root));
		}
		Filter.bRecursivePaths = true;

		AssetRegistry.GetAssets(Filter, OutAssets);

		if (!bIncludeMirrors)
		{
			OutAssets.RemoveAll([](const FAssetData& Asset)
			{
				return FDreamFXPaths::IsDecompiledNamespaceAsset(Asset.PackageName.ToString());
			});
		}

		OutAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.PackageName.LexicalLess(Right.PackageName);
		});
	}

	/** Exports one Niagara system back to source, to a file or to the log. */
	int32 RunDecompile(const FString& AssetPath, const FString& OutputPath, const FString& RootToken,
		bool bIncludeDefaultedInputs = false)
	{
		FString PackagePath;
		FString ResolveError;
		if (!FDreamFXPaths::ResolveAssetPath(AssetPath, RootToken, PackagePath, ResolveError))
		{
			UE_LOG(LogDreamFX, Error, TEXT("%s"), *ResolveError);
			return 1;
		}

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *FDreamFXPaths::ToObjectPath(PackagePath));
		if (System == nullptr)
		{
			UE_LOG(LogDreamFX, Error, TEXT("No Niagara System at '%s'."), *PackagePath);
			return 1;
		}

		// Same `Name=` as right-click *Export .dfs* (plan-v4 V1-2): the two ways of asking for the
		// same file have to produce the same file, or a headless export is a trap that overwrites
		// the asset the interactive one protects.
		FDecompileOptions DecompileOptions;
		DecompileOptions.bDecompiledNamespace = true;
		// R3. Set on every path that has to produce a file which rebuilds -- which includes
		// mirror-diff, because L1 compares the original's export against the mirror's and an original
		// that dropped its scratch pad modules would differ from a mirror that has them. Extraction is
		// idempotent, so the diff reuses what the export already wrote rather than writing again.
		// `coverage` is deliberately not in this list: it reports and must not touch the tree.
		DecompileOptions.bMaterializeEmbeddedScripts = true;
		// Diagnostic only: prints inputs the baseline would suppress. See FDecompileOptions.
		DecompileOptions.bIncludeDefaultedInputs = bIncludeDefaultedInputs;

		FDiagnosticSink Diagnostics;
		const FDecompileResult Result = FDecompiler::Decompile(System, RootToken, Diagnostics,
			DecompileOptions);

		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			UE_LOG(LogDreamFX, Warning, TEXT("%s"), *Diagnostic.Format());
		}

		if (!Result.bSucceeded)
		{
			return 1;
		}

		for (const FString& Feature : Result.UnsupportedFeatures)
		{
			UE_LOG(LogDreamFX, Warning, TEXT("Not represented in the export: %s"), *Feature);
		}

		if (OutputPath.IsEmpty())
		{
			UE_LOG(LogDreamFX, Display, TEXT("%s"), *Result.Source);
			return 0;
		}

		if (!FFileHelper::SaveStringToFile(Result.Source, *OutputPath))
		{
			UE_LOG(LogDreamFX, Error, TEXT("Could not write '%s'."), *OutputPath);
			return 1;
		}

		UE_LOG(LogDreamFX, Display, TEXT("Wrote %s"), *OutputPath);
		return 0;
	}

	/**
	 * Decompiles every Niagara system it can find and reports what fraction came back whole.
	 *
	 * Plan Phase 5 asks for this so v2's feature order is decided by what the project actually
	 * contains, rather than by which gap is most annoying to think about.
	 */
	int32 RunCoverage(const FString& SearchRoot)
	{
		const TArray<FString> Roots = ParseContentRoots(SearchRoot);

		// Mirrors excluded (plan-v4 V1-5): a `/<mount>/Decompiled/` asset is this pipeline's own
		// round-trip product, so counting it would report every gap twice and call the total coverage.
		TArray<FAssetData> Assets;
		FindSystems(Roots, /*bIncludeMirrors=*/false, Assets);

		UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX coverage over %d Niagara system(s) under %s ==="),
			Assets.Num(), *FString::Join(Roots, TEXT(", ")));

		int32 Exported = 0;
		int32 Failed = 0;
		TMap<FString, int32> FeatureCounts;
		TMap<FString, TArray<FString>> FeatureOwners;

		for (const FAssetData& Asset : Assets)
		{
			UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset.GetAsset());
			if (System == nullptr)
			{
				++Failed;
				continue;
			}

			FDiagnosticSink Diagnostics;
			const FDecompileResult Result = FDecompiler::Decompile(System, TEXT("Game"), Diagnostics);
			if (!Result.bSucceeded)
			{
				++Failed;
				UE_LOG(LogDreamFX, Warning, TEXT("  FAILED  %s"), *Asset.PackageName.ToString());
				continue;
			}

			++Exported;
			for (const FString& Feature : Result.UnsupportedFeatures)
			{
				++FeatureCounts.FindOrAdd(Feature);
				FeatureOwners.FindOrAdd(Feature).AddUnique(Asset.PackageName.ToString());
			}
			UE_LOG(LogDreamFX, Display, TEXT("  ok      %s%s"),
				*Asset.PackageName.ToString(),
				Result.UnsupportedFeatures.Num() > 0
					? *FString::Printf(TEXT("  (%d gap(s))"), Result.UnsupportedFeatures.Num())
					: TEXT(""));
		}

		UE_LOG(LogDreamFX, Display, TEXT("=== %d exported, %d failed ==="), Exported, Failed);

		if (FeatureCounts.Num() > 0)
		{
			FeatureCounts.ValueSort([](int32 Left, int32 Right) { return Left > Right; });
			UE_LOG(LogDreamFX, Display, TEXT("Gaps, most common first:"));
			for (const TPair<FString, int32>& Entry : FeatureCounts)
			{
				UE_LOG(LogDreamFX, Display, TEXT("  %4d x  %s"), Entry.Value, *Entry.Key);

				// The assets behind each number, because a count decides nothing on its own: five
				// occurrences in one throwaway test asset and five across the whole library are the
				// same figure and opposite conclusions about whether the gap is worth closing.
				if (const TArray<FString>* Owners = FeatureOwners.Find(Entry.Key))
				{
					for (const FString& Owner : *Owners)
					{
						UE_LOG(LogDreamFX, Display, TEXT("           %s"), *Owner);
					}
				}
			}
		}

		return Failed;
	}

	/**
	 * plan-v4 V2. Exports every system under `-Path=` into the decompiled tree, in one editor boot.
	 *
	 * The same per-asset route right-click *Export .dfs* takes, so what this writes is what a human
	 * would have got file by file -- including the `Decompiled/` namespace, which is what makes
	 * building the result harmless to the assets it was read from.
	 */
	int32 RunDecompileAll(const FString& SearchRoot)
	{
		const TArray<FString> Roots = ParseContentRoots(SearchRoot);

		TArray<FAssetData> Assets;
		FindSystems(Roots, /*bIncludeMirrors=*/false, Assets);

		UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX export over %d Niagara system(s) under %s ==="),
			Assets.Num(), *FString::Join(Roots, TEXT(", ")));

		FDecompileOptions DecompileOptions;
		DecompileOptions.bDecompiledNamespace = true;
		// R3. Set on every path that has to produce a file which rebuilds -- which includes
		// mirror-diff, because L1 compares the original's export against the mirror's and an original
		// that dropped its scratch pad modules would differ from a mirror that has them. Extraction is
		// idempotent, so the diff reuses what the export already wrote rather than writing again.
		// `coverage` is deliberately not in this list: it reports and must not touch the tree.
		DecompileOptions.bMaterializeEmbeddedScripts = true;

		int32 Written = 0;
		int32 Failed = 0;

		for (const FAssetData& Asset : Assets)
		{
			const FString PackagePath = Asset.PackageName.ToString();

			UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset.GetAsset());
			if (System == nullptr)
			{
				++Failed;
				UE_LOG(LogDreamFX, Error, TEXT("  FAILED  %s (not a Niagara system)"), *PackagePath);
				continue;
			}

			FString RootToken;
			FString MountPoint;
			FString RootError;
			if (!FDreamFXPaths::ResolveRootTokenForPackage(PackagePath, RootToken, MountPoint, RootError))
			{
				++Failed;
				UE_LOG(LogDreamFX, Error, TEXT("  FAILED  %s: %s"), *PackagePath, *RootError);
				continue;
			}

			FDiagnosticSink Diagnostics;
			const FDecompileResult Result = FDecompiler::Decompile(System, RootToken, Diagnostics,
				DecompileOptions);
			for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
			{
				UE_LOG(LogDreamFX, Warning, TEXT("%s"), *Diagnostic.Format());
			}

			if (!Result.bSucceeded)
			{
				++Failed;
				UE_LOG(LogDreamFX, Error, TEXT("  FAILED  %s"), *PackagePath);
				continue;
			}

			const FString OutputPath = FDreamFXPaths::DecompiledSourcePathFor(PackagePath, TEXT(".dfs"));
			if (!FFileHelper::SaveStringToFile(Result.Source, *OutputPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				++Failed;
				UE_LOG(LogDreamFX, Error, TEXT("  FAILED  could not write '%s'"), *OutputPath);
				continue;
			}

			++Written;
			UE_LOG(LogDreamFX, Display, TEXT("  wrote   %s%s"), *OutputPath,
				Result.UnsupportedFeatures.Num() > 0
					? *FString::Printf(TEXT("  (%d gap(s))"), Result.UnsupportedFeatures.Num())
					: TEXT(""));
		}

		UE_LOG(LogDreamFX, Display, TEXT("=== %d written, %d failed ==="), Written, Failed);
		return Failed;
	}

	/**
	 * plan-v4 V2, level 1: is the mirror the same effect as the original, as far as text can tell?
	 *
	 * Decompiling both and comparing is a stronger check than the RoundTrip corpus, which reads back
	 * what DreamFX itself wrote. Here the left-hand side is somebody else's asset: every gap in the
	 * reader and every gap in the writer has to cancel out exactly, or the two texts differ.
	 *
	 * Only the provenance line differs by construction -- it names the asset each side was read from
	 * -- so it is dropped from both. The `Name=` lines already agree: rehoming into `Decompiled/` is
	 * idempotent, so the mirror re-exports under the mirror's own name.
	 */
	int32 RunMirrorDiff(const FString& SearchRoot, const bool bCheckCompile)
	{
		const TArray<FString> Roots = ParseContentRoots(SearchRoot);

		TArray<FAssetData> Assets;
		FindSystems(Roots, /*bIncludeMirrors=*/false, Assets);

		UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX mirror diff over %d Niagara system(s) under %s ==="),
			Assets.Num(), *FString::Join(Roots, TEXT(", ")));

		FDecompileOptions DecompileOptions;
		DecompileOptions.bDecompiledNamespace = true;
		// R3. Set on every path that has to produce a file which rebuilds -- which includes
		// mirror-diff, because L1 compares the original's export against the mirror's and an original
		// that dropped its scratch pad modules would differ from a mirror that has them. Extraction is
		// idempotent, so the diff reuses what the export already wrote rather than writing again.
		// `coverage` is deliberately not in this list: it reports and must not touch the tree.
		DecompileOptions.bMaterializeEmbeddedScripts = true;

		/**
		 * The document without its opening comment block.
		 *
		 * L1 asks whether the original and its mirror describe the same system, and the header is the
		 * one part that is *supposed* to differ: it records what the export could not carry, so an
		 * original with gaps carries a list and its mirror -- which genuinely does not have those
		 * features -- carries none. Comparing the two whole files marked every gap-bearing asset as a
		 * text mismatch, which is why the previous run read 30 pass / 13 fail: all thirteen differed
		 * at line 3 or line 10, inside the header, and none of them differed anywhere else.
		 *
		 * The gap counts are reported next to the verdict instead, where they say something.
		 */
		auto WithoutHeader = [](const FString& Source)
		{
			TArray<FString> Lines;
			Source.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

			int32 First = 0;
			while (Lines.IsValidIndex(First))
			{
				const FString Trimmed = Lines[First].TrimStart();
				if (!Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("//")))
				{
					break;
				}
				++First;
			}
			return FString::Join(TArrayView<const FString>(Lines).RightChop(First), LINE_TERMINATOR);
		};

		auto CountGapLines = [](const FString& Source)
		{
			TArray<FString> Lines;
			Source.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
			int32 Gaps = 0;
			for (const FString& Line : Lines)
			{
				if (!Line.TrimStart().StartsWith(TEXT("//")))
				{
					break;
				}
				if (Line.TrimStart().StartsWith(TEXT("//   - ")))
				{
					++Gaps;
				}
			}
			return Gaps;
		};

		int32 Passed = 0;
		int32 Failed = 0;
		int32 Missing = 0;
		int32 CompilePassed = 0;
		int32 CompileFailed = 0;

		for (const FAssetData& Asset : Assets)
		{
			const FString PackagePath = Asset.PackageName.ToString();

			FString RootToken;
			FString MountPoint;
			FString RootError;
			if (!FDreamFXPaths::ResolveRootTokenForPackage(PackagePath, RootToken, MountPoint, RootError))
			{
				++Failed;
				UE_LOG(LogDreamFX, Error, TEXT("  L1 FAIL     %s: %s"), *PackagePath, *RootError);
				continue;
			}

			const FString MirrorPath = MountPoint / FDreamFXPaths::ToDecompiledNamespace(
				PackagePath.RightChop(MountPoint.Len() + 1));

			UNiagaraSystem* Original = Cast<UNiagaraSystem>(Asset.GetAsset());
			UNiagaraSystem* Mirror = LoadObject<UNiagaraSystem>(nullptr,
				*FDreamFXPaths::ToObjectPath(MirrorPath));

			if (Mirror == nullptr)
			{
				++Missing;
				UE_LOG(LogDreamFX, Warning, TEXT("  L1 MISSING  %s -> %s was never built"),
					*PackagePath, *MirrorPath);
				continue;
			}
			if (Original == nullptr)
			{
				++Failed;
				UE_LOG(LogDreamFX, Error, TEXT("  L1 FAIL     %s could not be loaded"), *PackagePath);
				continue;
			}

			FDiagnosticSink LeftDiagnostics;
			FDiagnosticSink RightDiagnostics;
			const FDecompileResult Left = FDecompiler::Decompile(Original, RootToken, LeftDiagnostics,
				DecompileOptions);
			const FDecompileResult Right = FDecompiler::Decompile(Mirror, RootToken, RightDiagnostics,
				DecompileOptions);

			if (!Left.bSucceeded || !Right.bSucceeded)
			{
				++Failed;
				UE_LOG(LogDreamFX, Error, TEXT("  L1 FAIL     %s: %s could not be decompiled"),
					*PackagePath, Left.bSucceeded ? TEXT("the mirror") : TEXT("the original"));
				continue;
			}

			const FString LeftText = WithoutHeader(Left.Source);
			const FString RightText = WithoutHeader(Right.Source);
			const int32 LeftGaps = CountGapLines(Left.Source);

			if (LeftText == RightText)
			{
				++Passed;
				if (LeftGaps > 0)
				{
					UE_LOG(LogDreamFX, Display,
						TEXT("  L1 PASS     %s (bodies identical; the original records %d gap(s) its mirror does not have)"),
						*PackagePath, LeftGaps);
				}
				else
				{
					UE_LOG(LogDreamFX, Display, TEXT("  L1 PASS     %s"), *PackagePath);
				}
			}
			else
			{
				++Failed;

				// Everything at once, as sets, not the first mismatched line. Reporting only the
				// first difference cost three fix-and-rerun rounds on one asset, and positional
				// comparison misattributes: one inserted line makes every later line "differ", which
				// is how a mirror that *gained* AdvancedAgingRate was read as the original having
				// lost AnalyticalCollisionPlaneSpace. Count both sides first, then say who has what.
				TArray<FString> LeftLines;
				TArray<FString> RightLines;
				LeftText.ParseIntoArrayLines(LeftLines, /*InCullEmpty=*/false);
				RightText.ParseIntoArrayLines(RightLines, /*InCullEmpty=*/false);

				int32 Prefix = 0;
				while (Prefix < LeftLines.Num() && Prefix < RightLines.Num()
					&& LeftLines[Prefix] == RightLines[Prefix])
				{
					++Prefix;
				}
				int32 Suffix = 0;
				while (Suffix < LeftLines.Num() - Prefix && Suffix < RightLines.Num() - Prefix
					&& LeftLines[LeftLines.Num() - 1 - Suffix] == RightLines[RightLines.Num() - 1 - Suffix])
				{
					++Suffix;
				}

				UE_LOG(LogDreamFX, Error,
					TEXT("  L1 FAIL     %s: original %d line(s), mirror %d line(s); first difference at line %d, %d differing line(s) in the middle"),
					*PackagePath, LeftLines.Num(), RightLines.Num(), Prefix + 1,
					FMath::Max(LeftLines.Num(), RightLines.Num()) - Prefix - Suffix);

				// The multiset difference of the disagreeing middle: a line both sides carry the same
				// number of times is agreement however the lines are ordered, so what remains is
				// exactly "who has what the other does not".
				TMap<FString, int32> MiddleCounts;
				for (int32 Index = Prefix; Index < LeftLines.Num() - Suffix; ++Index)
				{
					MiddleCounts.FindOrAdd(LeftLines[Index])++;
				}
				for (int32 Index = Prefix; Index < RightLines.Num() - Suffix; ++Index)
				{
					MiddleCounts.FindOrAdd(RightLines[Index])--;
				}

				TArray<FString> OnlyOriginal;
				TArray<FString> OnlyMirror;
				for (const TPair<FString, int32>& Entry : MiddleCounts)
				{
					for (int32 Copy = 0; Copy < FMath::Abs(Entry.Value); ++Copy)
					{
						(Entry.Value > 0 ? OnlyOriginal : OnlyMirror).Add(Entry.Key.TrimStart());
					}
				}
				OnlyOriginal.Sort();
				OnlyMirror.Sort();

				// Capped, and the cap says so -- a silent cap reads as "that was everything".
				constexpr int32 MaxReported = 40;
				auto ReportSide = [&](const TCHAR* Side, const TArray<FString>& Lines)
				{
					UE_LOG(LogDreamFX, Error, TEXT("                %s has %d line(s) the other side does not:"),
						Side, Lines.Num());
					for (int32 Index = 0; Index < FMath::Min(Lines.Num(), MaxReported); ++Index)
					{
						UE_LOG(LogDreamFX, Error, TEXT("                  %s | %s"), Side, *Lines[Index]);
					}
					if (Lines.Num() > MaxReported)
					{
						UE_LOG(LogDreamFX, Error, TEXT("                  %s | ... and %d more"),
							Side, Lines.Num() - MaxReported);
					}
				};
				if (OnlyOriginal.Num() > 0)
				{
					ReportSide(TEXT("original"), OnlyOriginal);
				}
				if (OnlyMirror.Num() > 0)
				{
					ReportSide(TEXT("mirror"), OnlyMirror);
				}
				if (OnlyOriginal.Num() == 0 && OnlyMirror.Num() == 0)
				{
					// Same lines, different order (or only line endings): still a text difference,
					// and naming the shape stops it being chased as a content loss.
					UE_LOG(LogDreamFX, Error,
						TEXT("                both sides carry the same lines; they differ only in order or line endings"));
				}
			}

			// L2. The generator already compiles what it builds, so this is not the primary gate --
			// it is what makes one report answer "is the mirror sound?" without a second run, and it
			// catches a mirror that was built before a module changed underneath it.
			if (bCheckCompile)
			{
				// VM scripts only. A commandlet's RHI is always Null, so waiting on compute shaders
				// here waits for something that cannot finish; GPU emitters are L3's job, in the
				// editor.
				FCompileStateInfo CompileState;
				TArray<FString> Errors;
				const bool bCompiled = FNiagaraAdapter::CompileAndWait(Mirror, /*bIncludingGpuShaders=*/false,
					CompileState, Errors);
				if (bCompiled && !CompileState.bHasErrors)
				{
					++CompilePassed;
					UE_LOG(LogDreamFX, Display, TEXT("  L2 PASS     %s (%s)"), *MirrorPath, *CompileState.StatusName);
				}
				else
				{
					++CompileFailed;
					UE_LOG(LogDreamFX, Error, TEXT("  L2 FAIL     %s (%s)%s"), *MirrorPath,
						*CompileState.StatusName,
						Errors.Num() > 0 ? *FString::Printf(TEXT(": %s"), *FString::Join(Errors, TEXT(" | "))) : TEXT(""));
				}
			}
		}

		UE_LOG(LogDreamFX, Display, TEXT("=== mirror diff L1: %d passed, %d failed, %d never built ==="),
			Passed, Failed, Missing);
		if (bCheckCompile)
		{
			UE_LOG(LogDreamFX, Display, TEXT("=== mirror diff L2: %d compiled clean, %d failed ==="),
				CompilePassed, CompileFailed);
		}
		return Failed + Missing + CompileFailed;
	}

	// ---------------------------------------------------------------- asset diff
	//
	// The facts themselves live in Diff/DreamFXAssetFacts.cpp -- the round-trip corpus needs the
	// same walk, for the same reason this command exists.

	/**
	 * `-AssetDiff`: compare every original against its mirror at the asset level, as fact multisets.
	 *
	 * The independent mirror the round of 2026-08-11 asked for: it does not touch the exporter, so
	 * an export-side loss that L1 cannot see (both L1 sides being the exporter's own output) lands
	 * here as a fact one side has and the other does not.
	 */
	int32 RunAssetDiff(const FString& SearchRoot, bool bCompileFirst)
	{
		const TArray<FString> Roots = ParseContentRoots(SearchRoot);
		TArray<FAssetData> Assets;
		FindSystems(Roots, /*bIncludeMirrors=*/false, Assets);

		UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX asset diff over %d Niagara system(s) under %s ==="),
			Assets.Num(), *FString::Join(Roots, TEXT(", ")));

		int32 Same = 0;
		int32 Different = 0;
		int32 Missing = 0;

		for (const FAssetData& Asset : Assets)
		{
			const FString PackagePath = Asset.PackageName.ToString();

			FString RootToken;
			FString MountPoint;
			FString RootError;
			if (!FDreamFXPaths::ResolveRootTokenForPackage(PackagePath, RootToken, MountPoint, RootError))
			{
				continue;
			}
			const FString MirrorPath = MountPoint / FDreamFXPaths::ToDecompiledNamespace(
				PackagePath.RightChop(MountPoint.Len() + 1));

			UNiagaraSystem* Original = Cast<UNiagaraSystem>(Asset.GetAsset());
			UNiagaraSystem* Mirror = LoadObject<UNiagaraSystem>(nullptr,
				*FDreamFXPaths::ToObjectPath(MirrorPath));
			if (Mirror == nullptr)
			{
				++Missing;
				UE_LOG(LogDreamFX, Warning, TEXT("  MISSING %s -> %s was never built"), *PackagePath, *MirrorPath);
				continue;
			}
			if (Original == nullptr)
			{
				++Missing;
				UE_LOG(LogDreamFX, Warning, TEXT("  MISSING %s could not be loaded"), *PackagePath);
				continue;
			}

			// Both sides are compiled before they are described, because the compiled facts are the
			// compiler's view and there is no such view without running the compiler. Loading alone
			// does not give one: PostLoad throws away a cached VM whose stored id does not match the
			// graph it was built from, so an authored asset last compiled by an older engine reads as
			// having no stages, no data interfaces and no written attributes -- which is a statement
			// about that asset's compile history, not about its content, and would have made this
			// whole family a false-difference generator.
			//
			// Forced, for the reason RequestCompileAsync's comment gives: a stored id can lie (NE_C
			// carried the right graph's id over bytecode from a different graph), and a channel whose
			// job is to be the last honest witness cannot take an id's word for it. Requested on both
			// sides before either is waited on, so the two translations overlap.
			if (bCompileFirst)
			{
				FNiagaraAdapter::RequestCompileAsync(Original, /*bForce=*/true);
				FNiagaraAdapter::RequestCompileAsync(Mirror, /*bForce=*/true);

				FCompileStateInfo CompileState;
				TArray<FString> CompileErrors;
				FNiagaraAdapter::WaitAndCollect(Original, /*bIncludingGpuShaders=*/false,
					CompileState, CompileErrors);
				FNiagaraAdapter::WaitAndCollect(Mirror, /*bIncludingGpuShaders=*/false,
					CompileState, CompileErrors);
			}

			TArray<FString> LeftFacts;
			TArray<FString> RightFacts;
			DescribeSystemFacts(Original, LeftFacts);
			DescribeSystemFacts(Mirror, RightFacts);

			// -DreamFXDumpFacts writes both sides' full fact lists to Saved/DreamFX/. The console
			// report truncates every fact to 400 characters, which is exactly wrong for chasing a
			// difference that lives past that mark.
			if (FParse::Param(FCommandLine::Get(), TEXT("DreamFXDumpFacts")))
			{
				const FString DumpDir = FPaths::ProjectSavedDir() / TEXT("DreamFX");
				const FString BaseName = FPackageName::GetShortName(PackagePath);
				FFileHelper::SaveStringArrayToFile(LeftFacts, *(DumpDir / BaseName + TEXT(".original.facts")));
				FFileHelper::SaveStringArrayToFile(RightFacts, *(DumpDir / BaseName + TEXT(".mirror.facts")));
			}

			TMap<FString, int32> Counts;
			for (const FString& Fact : LeftFacts)
			{
				Counts.FindOrAdd(Fact)++;
			}
			for (const FString& Fact : RightFacts)
			{
				Counts.FindOrAdd(Fact)--;
			}

			TArray<FString> OnlyOriginal;
			TArray<FString> OnlyMirror;
			for (const TPair<FString, int32>& Entry : Counts)
			{
				for (int32 Copy = 0; Copy < FMath::Abs(Entry.Value); ++Copy)
				{
					(Entry.Value > 0 ? OnlyOriginal : OnlyMirror).Add(Entry.Key);
				}
			}
			OnlyOriginal.Sort();
			OnlyMirror.Sort();

			if (OnlyOriginal.Num() == 0 && OnlyMirror.Num() == 0)
			{
				++Same;
				UE_LOG(LogDreamFX, Display, TEXT("  SAME    %s (%d fact(s))"), *PackagePath, LeftFacts.Num());
				continue;
			}

			++Different;
			UE_LOG(LogDreamFX, Error,
				TEXT("  DIFF    %s: original %d fact(s), mirror %d; %d only-original, %d only-mirror"),
				*PackagePath, LeftFacts.Num(), RightFacts.Num(), OnlyOriginal.Num(), OnlyMirror.Num());

			constexpr int32 MaxReported = 60;
			auto ReportSide = [](const TCHAR* Side, const TArray<FString>& Facts)
			{
				for (int32 Index = 0; Index < FMath::Min(Facts.Num(), MaxReported); ++Index)
				{
					UE_LOG(LogDreamFX, Error, TEXT("            %s | %s"), Side, *Facts[Index].Left(400));
				}
				if (Facts.Num() > MaxReported)
				{
					UE_LOG(LogDreamFX, Error, TEXT("            %s | ... and %d more"),
						Side, Facts.Num() - MaxReported);
				}
			};
			ReportSide(TEXT("original"), OnlyOriginal);
			ReportSide(TEXT("mirror  "), OnlyMirror);
		}

		UE_LOG(LogDreamFX, Display, TEXT("=== asset diff: %d same, %d different, %d missing ==="),
			Same, Different, Missing);
		return Different;
	}

	/**
	 * R4's safe rename. `-Rename=<asset>:<old>:<new>` renames the emitter on the asset, keeping its
	 * handle, so the source edit plus a rebuild reuses it instead of building a new one.
	 */
	int32 RunRename(const FString& Spec)
	{
		TArray<FString> Parts;
		Spec.ParseIntoArray(Parts, TEXT(":"), /*InCullEmpty=*/false);

		// The asset path itself may contain a root prefix with a colon, so the last two fields are
		// the names and everything before them is the path.
		if (Parts.Num() < 3)
		{
			UE_LOG(LogDreamFX, Error,
				TEXT("-Rename needs <asset>:<oldName>:<newName>, e.g. -Rename=/Game/FX/NS_Spark:Sparks:Embers"));
			return 1;
		}

		const FString NewName = Parts.Pop();
		const FString OldName = Parts.Pop();
		const FString AssetPath = FString::Join(Parts, TEXT(":"));

		FString PackagePath;
		FString ResolveError;
		if (!FDreamFXPaths::ResolveAssetPath(AssetPath, TEXT("Game"), PackagePath, ResolveError))
		{
			UE_LOG(LogDreamFX, Error, TEXT("%s"), *ResolveError);
			return 1;
		}

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *FDreamFXPaths::ToObjectPath(PackagePath));
		if (System == nullptr)
		{
			UE_LOG(LogDreamFX, Error, TEXT("No Niagara System at '%s'."), *PackagePath);
			return 1;
		}

		TArray<FString> Errors;
		if (!FNiagaraAdapter::RenameEmitter(System, FName(*OldName), FName(*NewName), Errors))
		{
			for (const FString& Error : Errors)
			{
				UE_LOG(LogDreamFX, Error, TEXT("%s"), *Error);
			}
			return 1;
		}

		Errors.Reset();
		if (!FNiagaraAdapter::SaveSystem(System, Errors))
		{
			for (const FString& Error : Errors)
			{
				UE_LOG(LogDreamFX, Error, TEXT("%s"), *Error);
			}
			return 1;
		}

		UE_LOG(LogDreamFX, Display,
			TEXT("Renamed '%s' to '%s' on %s. Now change the name in the .dfs and rebuild -- the rebuild will reuse this emitter's handle."),
			*OldName, *NewName, *PackagePath);
		return 0;
	}

	/** Lists what each source file depends on, so a module change's blast radius is visible. */
	int32 RunGraph()
	{
		TArray<FString> SourceFiles;
		FDreamFXPaths::FindSourceFiles(SourceFiles);

		UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX dependencies: %d source file(s) ==="), SourceFiles.Num());

		int32 Errors = 0;
		for (const FString& SourceFile : SourceFiles)
		{
			FDiagnosticSink Diagnostics;
			FDocument Document;
			if (!FParser::ParseFile(SourceFile, Document, Diagnostics))
			{
				UE_LOG(LogDreamFX, Error, TEXT("%s: parse failed"), *SourceFile);
				++Errors;
				continue;
			}

			TArray<FString> Modules;
			TArray<FString> References;

			auto GatherStack = [&Modules](const FStack& Stack)
			{
				for (const FStatement& Statement : Stack.Statements)
				{
					if (Statement.Kind == EStatementKind::ModuleCall)
					{
						Modules.AddUnique(Statement.Name);
					}
				}
			};

			for (const FStack& Stack : Document.Stacks)
			{
				GatherStack(Stack);
			}
			for (const FEmitter& Emitter : Document.Emitters)
			{
				if (!Emitter.FromPath.IsEmpty())
				{
					References.AddUnique(Emitter.FromPath);
				}
				for (const FStack& Stack : Emitter.Stacks)
				{
					GatherStack(Stack);
				}
			}
			for (const FStack& Stack : Document.EmitterDefinition.Stacks)
			{
				GatherStack(Stack);
			}

			Modules.Sort();
			References.Sort();

			UE_LOG(LogDreamFX, Display, TEXT("%s"), *FPaths::GetCleanFilename(SourceFile));
			for (const FString& Reference : References)
			{
				UE_LOG(LogDreamFX, Display, TEXT("    from  %s"), *Reference);
			}
			for (const FString& Module : Modules)
			{
				UE_LOG(LogDreamFX, Display, TEXT("    uses  %s"), *Module);
			}
		}

		return Errors;
	}
}

UDreamFXCommandlet::UDreamFXCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

int32 UDreamFXCommandlet::Main(const FString& Params)
{
	// plan-v6 P2. An editor open on this project writes the same packages this run is about to write,
	// and the loser of that race is whichever one saves second. A warning rather than a refusal: this
	// cannot tell *which* project the editor has open, and being wrong about that should cost a line
	// of log rather than a refused build.
	if (FPlatformProcess::IsApplicationRunning(TEXT("UnrealEditor.exe")))
	{
		UE_LOG(LogDreamFX, Warning,
			TEXT("An Unreal editor is running. If it has this project open, it and this run will fight ")
			TEXT("over the same package files -- close it before a full build."));
	}

	FString SchemaQuery;
	if (FParse::Value(*Params, TEXT("Schema="), SchemaQuery))
	{
		FString StackName;
		FParse::Value(*Params, TEXT("Stack="), StackName);
		return DumpSchema(SchemaQuery, StackName);
	}

	if (FParse::Param(*Params, TEXT("Index")))
	{
		FString IndexOut;
		FParse::Value(*Params, TEXT("Out="), IndexOut);
		return DumpIndex(IndexOut, FParse::Param(*Params, TEXT("NoInputs")),
			FParse::Param(*Params, TEXT("Retry")));
	}

	if (FParse::Param(*Params, TEXT("ListModules")) || FParse::Param(*Params, TEXT("ListDynamicInputs")))
	{
		const bool bDynamicInputs = FParse::Param(*Params, TEXT("ListDynamicInputs"));
		FModuleLibrary Library;
		TArray<FString> Entries;
		Library.ListAvailable(bDynamicInputs, Entries);

		UE_LOG(LogDreamFX, Display, TEXT("%d %s available:"),
			Entries.Num(), bDynamicInputs ? TEXT("dynamic input(s)") : TEXT("module(s)"));
		for (const FString& Entry : Entries)
		{
			UE_LOG(LogDreamFX, Display, TEXT("  %s"), *Entry);
		}
		return 0;
	}

	FString RootToken;
	FParse::Value(*Params, TEXT("Root="), RootToken);

	FString DecompileTarget;
	if (FParse::Value(*Params, TEXT("Decompile="), DecompileTarget))
	{
		FString OutputPath;
		FParse::Value(*Params, TEXT("Out="), OutputPath);
		return RunDecompile(DecompileTarget, OutputPath, RootToken.IsEmpty() ? TEXT("Game") : RootToken,
			FParse::Param(*Params, TEXT("NoDefaults")));
	}

	if (FParse::Param(*Params, TEXT("Coverage")))
	{
		FString SearchRoot;
		FParse::Value(*Params, TEXT("Path="), SearchRoot);
		return RunCoverage(SearchRoot);
	}

	if (FParse::Param(*Params, TEXT("DecompileAll")))
	{
		FString SearchRoot;
		FParse::Value(*Params, TEXT("Path="), SearchRoot);
		return RunDecompileAll(SearchRoot);
	}

	if (FParse::Param(*Params, TEXT("AssetDiff")))
	{
		FString SearchRoot;
		FParse::Value(*Params, TEXT("Path="), SearchRoot);
		return RunAssetDiff(SearchRoot, /*bCompileFirst=*/!FParse::Param(*Params, TEXT("NoCompile")));
	}

	if (FParse::Param(*Params, TEXT("MirrorDiff")))
	{
		FString SearchRoot;
		FParse::Value(*Params, TEXT("Path="), SearchRoot);
		return RunMirrorDiff(SearchRoot, /*bCheckCompile=*/!FParse::Param(*Params, TEXT("NoCompile")));
	}

	FString RenameSpec;
	if (FParse::Value(*Params, TEXT("Rename="), RenameSpec))
	{
		return RunRename(RenameSpec);
	}

	if (FParse::Param(*Params, TEXT("Graph")))
	{
		return RunGraph();
	}

	const bool bLintOnly = FParse::Param(*Params, TEXT("Lint"));

	FGenerateOptions Options;
	Options.bVerifyOnly = FParse::Param(*Params, TEXT("Verify"));
	Options.bStrictVersions = FParse::Param(*Params, TEXT("StrictVersions"));
	Options.bForce = FParse::Param(*Params, TEXT("Force"));
	Options.bSave = !FParse::Param(*Params, TEXT("NoSave")) && !Options.bVerifyOnly;

	// -Window=N: how many systems may sit between their compile request and their finalize. Source
	// N+1 generates on the game thread while N's scripts compile on the task pool; 1 restores the
	// fully serial behaviour. The bound is memory, not scheduling -- each in-flight system stays
	// fully loaded until its save.
	int32 PipelineWindow = 4;
	FParse::Value(*Params, TEXT("Window="), PipelineWindow);
	PipelineWindow = FMath::Clamp(PipelineWindow, 1, 16);

	// plan-v6 P0: the baseline half of the benchmark. Off, every write builds its own system view
	// model again, which is what the numbers before P1 were measured on.
	const bool bNoWriteScope = FParse::Param(*Params, TEXT("NoWriteScope"));
	FNiagaraAdapter::SetWriteScopeEnabled(!bNoWriteScope);

	// plan-v6 P3: the same, one epoch further in. Off, a structural mutator keeps the context and
	// trusts the engine's own in-place refresh of the group it changed.
	FNiagaraAdapter::SetRebuildContextOnStructural(FParse::Param(*Params, TEXT("RebuildOnStructural")));

	// Switch-refresh round: off, a static-switch write keeps the context and trusts the engine's
	// synchronous refresh of the module item it changed.
	FNiagaraAdapter::SetRebuildContextOnSwitch(FParse::Param(*Params, TEXT("RebuildOnSwitch")));

	// AddModule batching round: off, every add pays the engine's per-add stack refresh again.
	FNiagaraAdapter::SetBatchAddRefresh(!FParse::Param(*Params, TEXT("RebuildPerAdd")));
	FNiagaraAdapter::ResetStats();

	FString SingleFile;
	FParse::Value(*Params, TEXT("File="), SingleFile);

	TArray<FString> SourceFiles;
	if (!SingleFile.IsEmpty())
	{
		if (FPaths::IsRelative(SingleFile))
		{
			SingleFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SingleFile);
		}
		SourceFiles.Add(SingleFile);
	}
	else
	{
		FDreamFXPaths::FindSourceFiles(SourceFiles);
	}

	if (SourceFiles.Num() == 0)
	{
		const TArray<FSourceRoot>& Roots = FDreamFXPaths::GetSourceRoots();
		TArray<FString> RootPaths;
		for (const FSourceRoot& Root : Roots)
		{
			RootPaths.Add(Root.Directory);
		}
		UE_LOG(LogDreamFX, Warning,
			TEXT("No DreamFX source files found. Searched %d root(s): %s"),
			Roots.Num(), RootPaths.Num() > 0 ? *FString::Join(RootPaths, TEXT(", ")) : TEXT("(none)"));
		return 0;
	}

	// Modules before systems. A .dfs resolves the modules it calls out of the asset library at build
	// time, so a .dfm and the .dfs that uses it, committed together, only work if the module asset is
	// on disk first. Emitters sit in the middle: they generate nothing themselves, but a .dfs that
	// pulls one in with `from` should see its diagnostics before its own.
	auto BuildOrder = [](const FString& File)
	{
		EDocumentKind Kind = EDocumentKind::System;
		FParser::DocumentKindFromExtension(FPaths::GetExtension(File), Kind);
		switch (Kind)
		{
		case EDocumentKind::Module:
		case EDocumentKind::DynamicInput: return 0;
		case EDocumentKind::Emitter:      return 1;
		default:                          return 2;
		}
	};
	Algo::StableSortBy(SourceFiles, BuildOrder);

	const TCHAR* const ModeLabel = bLintOnly ? TEXT("lint") : (Options.bVerifyOnly ? TEXT("verify") : TEXT("build"));
	UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX %s: %d source file(s) ==="), ModeLabel, SourceFiles.Num());

	int32 TotalErrors = 0;
	int32 TotalWarnings = 0;
	int32 Built = 0;
	int32 Skipped = 0;
	int32 Failed = 0;

	auto FlushDiagnostics = [&TotalErrors, &TotalWarnings](const FDiagnosticSink& Diagnostics)
	{
		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			switch (Diagnostic.Severity)
			{
			case EDiagnosticSeverity::Error:
				UE_LOG(LogDreamFX, Error, TEXT("%s"), *Diagnostic.Format());
				break;
			case EDiagnosticSeverity::Warning:
				UE_LOG(LogDreamFX, Warning, TEXT("%s"), *Diagnostic.Format());
				break;
			default:
				UE_LOG(LogDreamFX, Display, TEXT("%s"), *Diagnostic.Format());
				break;
			}
		}

		TotalErrors += Diagnostics.NumErrors();
		TotalWarnings += Diagnostics.NumWarnings();
	};

	// Systems whose compile is in flight, oldest first. Their diagnostics are held with them so each
	// file's block still prints contiguously, at finalize.
	struct FInFlightBuild
	{
		TSharedPtr<FPendingBuild> Pending;
		TUniquePtr<FDiagnosticSink> Diagnostics;
	};
	TArray<FInFlightBuild> InFlight;

	auto FinalizeFront = [&]()
	{
		FInFlightBuild Front = MoveTemp(InFlight[0]);
		InFlight.RemoveAt(0);
		if (FGenerator::Finalize(Front.Pending, *Front.Diagnostics))
		{
			++Built;
		}
		else
		{
			++Failed;
		}
		FlushDiagnostics(*Front.Diagnostics);
	};

	for (const FString& SourceFile : SourceFiles)
	{
		// The one place the object-count collection gate is armed: between source files, where no
		// build is in flight to purge. A stock-engine tree run exhausts the 25M UObject cap without
		// this -- and regressed nondeterministically with the gate armed anywhere deeper.
		UE::DreamFX::Editor::FNiagaraAdapter::CollectIfHeavy(/*bIncludeObjectCountGate=*/true);

		// .dfs and .dfm produce assets. A .dfe does not -- it is merged into its host by copy (R3), so
		// it is parsed and linted only, which still fails the gate on a broken one rather than waiting
		// until something references it.
		EDocumentKind Kind = EDocumentKind::System;
		FParser::DocumentKindFromExtension(FPaths::GetExtension(SourceFile), Kind);

		const bool bGenerates = Kind == EDocumentKind::System
			|| Kind == EDocumentKind::Module
			|| Kind == EDocumentKind::DynamicInput;

		if (bLintOnly || !bGenerates)
		{
			FDiagnosticSink Diagnostics;
			FDocument Document;
			if (FParser::ParseFile(SourceFile, Document, Diagnostics))
			{
				FLint::Run(Document, Diagnostics);
			}
			FlushDiagnostics(Diagnostics);
			continue;
		}

		// Systems go through the pipeline; modules and dynamic inputs stay synchronous -- they are
		// the dependencies the systems behind them resolve at plan time, and the sort above put them
		// first for exactly that reason.
		FGenerateOptions FileOptions = Options;
		FileOptions.bDeferCompile = PipelineWindow > 1 && Kind == EDocumentKind::System && !Options.bVerifyOnly;

		TUniquePtr<FDiagnosticSink> Diagnostics = MakeUnique<FDiagnosticSink>();
		const FGenerateResult Result = FGenerator::GenerateFromFile(SourceFile, FileOptions, *Diagnostics);

		if (Result.Pending.IsValid())
		{
			InFlight.Add({Result.Pending, MoveTemp(Diagnostics)});

			// Oldest-first and opportunistic: whatever already compiled finalizes now, so its logs
			// land next to its build and its save runs while the next generation is still cheap.
			while (InFlight.Num() > 0 && FGenerator::IsCompileComplete(InFlight[0].Pending))
			{
				FinalizeFront();
			}
			while (InFlight.Num() >= PipelineWindow)
			{
				FinalizeFront();
			}
			continue;
		}

		if (Result.bSkipped)
		{
			++Skipped;
		}
		else if (Result.bSucceeded)
		{
			++Built;
		}
		else
		{
			++Failed;
		}
		FlushDiagnostics(*Diagnostics);
	}

	// Drain the tail: the last few systems' compiles overlap each other even with nothing left to
	// generate.
	while (InFlight.Num() > 0)
	{
		FinalizeFront();
	}

	UE_LOG(LogDreamFX, Display,
		TEXT("=== DreamFX done: %d %s, %d up to date, %d failed | %d error(s), %d warning(s) ==="),
		Built, Options.bVerifyOnly ? TEXT("verified") : TEXT("built"),
		Skipped, Failed, TotalErrors, TotalWarnings);

	if (!Options.bVerifyOnly && !bLintOnly)
	{
		FString Baselines;
		if (bNoWriteScope) { Baselines += TEXT(" [write scope OFF]"); }
		if (FParse::Param(*Params, TEXT("RebuildOnStructural"))) { Baselines += TEXT(" [rebuild on structural]"); }

		UE_LOG(LogDreamFX, Display, TEXT("=== %s%s ==="), *FNiagaraAdapter::ReportStats(), *Baselines);
		FNiagaraAdapter::ReportOperationTimings();
	}

	return TotalErrors;
}
