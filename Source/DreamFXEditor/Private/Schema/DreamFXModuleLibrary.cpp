#include "DreamFXModuleLibrary.h"

#include "DreamFXModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::DreamFX::Editor
{
	FModuleLibrary::FModuleLibrary()
	{
		AddDefaultSearchPaths();
	}

	FModuleLibrary::~FModuleLibrary()
	{
		if (ProbeSystem != nullptr)
		{
			ProbeSystem->RemoveFromRoot();
			ProbeSystem = nullptr;
		}
	}

	void FModuleLibrary::AddDefaultSearchPaths()
	{
		SearchPaths.AddUnique(TEXT("/Niagara/Modules"));
		SearchPaths.AddUnique(TEXT("/Niagara/DynamicInputs"));
		SearchPaths.AddUnique(TEXT("/Niagara/Functions"));
	}

	void FModuleLibrary::SetSearchPaths(const TArray<FString>& InSearchPaths)
	{
		SearchPaths.Reset();
		for (const FString& Path : InSearchPaths)
		{
			SearchPaths.AddUnique(Path);
		}
		AddDefaultSearchPaths();
		ModuleCache.Reset();
		DynamicInputCache.Reset();
		PackagesByName.Reset();
		bIndexBuilt = false;
	}

	void FModuleLibrary::BuildIndex()
	{
		if (bIndexBuilt)
		{
			return;
		}
		bIndexBuilt = true;

		for (const FString& SearchPath : SearchPaths)
		{
			FString Directory;
			if (!FPackageName::TryConvertLongPackageNameToFilename(SearchPath / TEXT(""), Directory))
			{
				// A search path naming an unmounted root is the author's mistake, but it must not abort
				// the whole lookup: the other paths may well hold what they asked for.
				UE_LOG(LogDreamFX, Warning,
					TEXT("Module search path '%s' does not resolve to a mounted content directory; ignoring it."),
					*SearchPath);
				continue;
			}

			Directory = FPaths::ConvertRelativePathToFull(Directory);
			if (!IFileManager::Get().DirectoryExists(*Directory))
			{
				continue;
			}

			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.uasset"),
				/*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/false);

			for (const FString& File : Files)
			{
				FString PackageName;
				if (!FPackageName::TryConvertFilenameToLongPackageName(File, PackageName))
				{
					continue;
				}
				const FString ShortName = FPaths::GetBaseFilename(File).ToLower();
				PackagesByName.FindOrAdd(ShortName).AddUnique(PackageName);
			}
		}
	}

	UNiagaraScript* FModuleLibrary::FindScript(const FString& Name, bool bDynamicInput, FString& OutError)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("Empty module name.");
			return nullptr;
		}

		TMap<FString, TWeakObjectPtr<UNiagaraScript>>& Cache = bDynamicInput ? DynamicInputCache : ModuleCache;
		if (const TWeakObjectPtr<UNiagaraScript>* Cached = Cache.Find(Trimmed))
		{
			if (Cached->IsValid())
			{
				return Cached->Get();
			}
			Cache.Remove(Trimmed);
		}

		// A dynamic input position also accepts a plain Function script. Niagara does -- this project's
		// content plugs `/Niagara/Functions/Localspace/SimulationPosition` into one, and the asset
		// compiles -- and refusing it made two chains unexportable and unbuildable for no reason other
		// than a stricter reading of "usage" than the engine's own (plan-v5 R3).
		auto UsageAccepted = [bDynamicInput](ENiagaraScriptUsage Usage)
		{
			return bDynamicInput
				? (Usage == ENiagaraScriptUsage::DynamicInput || Usage == ENiagaraScriptUsage::Function)
				: (Usage == ENiagaraScriptUsage::Module);
		};
		const TCHAR* const KindLabel = bDynamicInput ? TEXT("dynamic input") : TEXT("module");

		auto LoadByPackage = [](const FString& PackageName) -> UNiagaraScript*
		{
			FString AssetName = PackageName;
			int32 SlashIndex;
			if (PackageName.FindLastChar(TEXT('/'), SlashIndex))
			{
				AssetName = PackageName.RightChop(SlashIndex + 1);
			}
			return LoadObject<UNiagaraScript>(nullptr, *FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName));
		};

		auto Accept = [&](UNiagaraScript* Script) -> UNiagaraScript*
		{
			if (Script == nullptr)
			{
				return nullptr;
			}
			if (!UsageAccepted(Script->GetUsage()))
			{
				OutError = FString::Printf(
					TEXT("'%s' resolves to a Niagara script whose usage is '%s', but a %s was expected here."),
					*Trimmed,
					*StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script->GetUsage())),
					KindLabel);
				return nullptr;
			}
			Cache.Add(Trimmed, Script);
			return Script;
		};

		// A leading slash means the author wrote the full content path; take them at their word so a
		// deliberate disambiguation is never second-guessed.
		if (Trimmed.StartsWith(TEXT("/")))
		{
			UNiagaraScript* Script = LoadByPackage(Trimmed);
			if (Script == nullptr)
			{
				OutError = FString::Printf(TEXT("No Niagara script asset at '%s'."), *Trimmed);
				return nullptr;
			}
			return Accept(Script);
		}

		BuildIndex();

		// A partial path like "Update/Forces/GravityForce" narrows by suffix; the last segment is the
		// asset name the index is keyed on.
		FString ShortName = Trimmed;
		FString PathHint;
		int32 SlashIndex;
		if (Trimmed.FindLastChar(TEXT('/'), SlashIndex))
		{
			ShortName = Trimmed.RightChop(SlashIndex + 1);
			PathHint = Trimmed.Left(SlashIndex);
		}

		const TArray<FString>* Packages = PackagesByName.Find(ShortName.ToLower());
		if (Packages == nullptr)
		{
			OutError = FString::Printf(
				TEXT("No %s named '%s' was found (%d asset(s) indexed under the search paths). Searched: %s. Add its folder to Settings.ModulePaths, or write the full asset path."),
				KindLabel, *Trimmed, PackagesByName.Num(), *FString::Join(SearchPaths, TEXT(", ")));
			return nullptr;
		}

		// A partial path anchors at the end, not anywhere in the string: "Spawn/Initialization/
		// InitializeParticle" must select exactly that asset and not its "…/Initialization/V2/…"
		// sibling, which a substring test would also accept.
		const FString Suffix = PathHint.IsEmpty()
			? FString()
			: FString::Printf(TEXT("%s/%s"), *PathHint, *ShortName);

		TArray<FString> Filtered;
		for (const FString& PackageName : *Packages)
		{
			if (Suffix.IsEmpty() || PackageName.EndsWith(Suffix, ESearchCase::IgnoreCase))
			{
				Filtered.Add(PackageName);
			}
		}

		if (Filtered.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("'%s' matched no asset whose path contains '%s'. Candidates named '%s': %s"),
				*Trimmed, *PathHint, *ShortName, *FString::Join(*Packages, TEXT(", ")));
			return nullptr;
		}

		// Filter by usage before declaring ambiguity: the same short name can exist as both a module
		// and a dynamic input, and a name that is unique among modules is not really ambiguous.
		TArray<UNiagaraScript*> Matching;
		for (const FString& PackageName : Filtered)
		{
			if (UNiagaraScript* Script = LoadByPackage(PackageName))
			{
				if (UsageAccepted(Script->GetUsage()))
				{
					Matching.Add(Script);
				}
			}
		}

		if (Matching.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("'%s' matched %d asset(s) but none is a %s: %s"),
				*Trimmed, Filtered.Num(), KindLabel, *FString::Join(Filtered, TEXT(", ")));
			return nullptr;
		}

		// A declared DynamicInput outranks a plain Function script that merely *may* sit in the same
		// position. Accepting both as equals (the first cut of the SimulationPosition fix) made
		// `RandomRangeFloat` ambiguous between /Niagara/DynamicInputs/UniformRange/V2 and
		// /Niagara/Functions -- a name every hand-written sample in this repo already used, and which
		// had resolved unambiguously for as long as it had existed. Widening a lookup must not
		// reclassify names that already resolved.
		if (bDynamicInput && Matching.Num() > 1)
		{
			TArray<UNiagaraScript*> Declared = Matching.FilterByPredicate([](const UNiagaraScript* Script)
			{
				return Script->GetUsage() == ENiagaraScriptUsage::DynamicInput;
			});
			if (Declared.Num() > 0)
			{
				Matching = MoveTemp(Declared);
			}
		}

		if (Matching.Num() > 1)
		{
			TArray<FString> Paths;
			for (const UNiagaraScript* Script : Matching)
			{
				Paths.Add(Script->GetPathName());
			}
			OutError = FString::Printf(
				TEXT("'%s' is ambiguous -- %d %ss match. Write the full path instead: %s"),
				*Trimmed, Matching.Num(), KindLabel, *FString::Join(Paths, TEXT(", ")));
			return nullptr;
		}

		return Accept(Matching[0]);
	}

	FString FModuleLibrary::FindAddressableName(UNiagaraScript* Script, bool bDynamicInput)
	{
		if (Script == nullptr)
		{
			return FString();
		}

		const FString PackageName = Script->GetOutermost()->GetName();

		TArray<FString> Segments;
		PackageName.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty=*/true);

		// Grow the candidate one leading segment at a time: "Name", then "Folder/Name", and so on.
		for (int32 Take = 1; Take < Segments.Num(); ++Take)
		{
			TArray<FString> Tail;
			for (int32 Index = Segments.Num() - Take; Index < Segments.Num(); ++Index)
			{
				Tail.Add(Segments[Index]);
			}

			const FString Candidate = FString::Join(Tail, TEXT("/"));
			FString Error;
			const UNiagaraScript* Resolved = bDynamicInput ? FindDynamicInput(Candidate, Error) : FindModule(Candidate, Error);
			if (Resolved == Script)
			{
				return Candidate;
			}
		}

		// The full path, but only if it really loads back to this script. It does not for a script
		// stored inside another asset, where the package names the owner.
		{
			FString Error;
			const UNiagaraScript* Resolved = bDynamicInput ? FindDynamicInput(PackageName, Error) : FindModule(PackageName, Error);
			if (Resolved == Script)
			{
				return PackageName;
			}
		}

		return FString();
	}

	FString FModuleLibrary::GetUnambiguousName(UNiagaraScript* Script, bool bDynamicInput)
	{
		if (Script == nullptr)
		{
			return FString();
		}

		const FString Addressable = FindAddressableName(Script, bDynamicInput);
		return Addressable.IsEmpty() ? Script->GetOutermost()->GetName() : Addressable;
	}

	UNiagaraScript* FModuleLibrary::MaterializeEmbeddedScript(UNiagaraScript* Script, const FString& PackagePath,
		const FString& AssetName, FString& OutError)
	{
		if (Script == nullptr)
		{
			OutError = TEXT("Cannot extract a null script.");
			return nullptr;
		}

		const FString FullPackageName = PackagePath / AssetName;
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FullPackageName, *AssetName);

		// A previous export of the same system already lifted it out. Reusing that keeps re-exporting
		// idempotent, which is the whole basis of the mirror-diff contract.
		if (UNiagaraScript* Existing = LoadObject<UNiagaraScript>(nullptr, *ObjectPath))
		{
			return Existing;
		}

		UPackage* Package = CreatePackage(*FullPackageName);
		if (Package == nullptr)
		{
			OutError = FString::Printf(TEXT("Could not create package '%s'."), *FullPackageName);
			return nullptr;
		}
		Package->FullyLoad();

		UNiagaraScript* Copy = Cast<UNiagaraScript>(
			StaticDuplicateObject(Script, Package, FName(*AssetName)));
		if (Copy == nullptr)
		{
			OutError = FString::Printf(TEXT("Could not copy script '%s' out of '%s'."),
				*Script->GetName(), *Script->GetOutermost()->GetName());
			return nullptr;
		}

		Copy->SetFlags(RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Copy);
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			FullPackageName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Copy, *FileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("SavePackage failed for '%s'."), *FileName);
			return nullptr;
		}

		// Deliberately *not* registered in PackagesByName. The index is what short names resolve
		// through, and it is built from the search paths -- which `Decompiled/` is not one of. A
		// registration here would let this process resolve a short name that no other process can,
		// and the export would name the script that way. Callers reference an extracted script by its
		// full package path, which needs no index at all.

		UE_LOG(LogDreamFX, Display, TEXT("  extracted embedded script '%s' -> %s"),
			*Script->GetName(), *FullPackageName);
		return Copy;
	}

	void FModuleLibrary::ListAvailable(bool bDynamicInput, TArray<FString>& OutEntries)
	{
		BuildIndex();

		const ENiagaraScriptUsage RequiredUsage = bDynamicInput
			? ENiagaraScriptUsage::DynamicInput : ENiagaraScriptUsage::Module;

		for (const TPair<FString, TArray<FString>>& Entry : PackagesByName)
		{
			for (const FString& PackageName : Entry.Value)
			{
				FString AssetName = PackageName;
				int32 SlashIndex;
				if (PackageName.FindLastChar(TEXT('/'), SlashIndex))
				{
					AssetName = PackageName.RightChop(SlashIndex + 1);
				}

				const UNiagaraScript* Script = LoadObject<UNiagaraScript>(
					nullptr, *FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName));
				if (Script != nullptr && Script->GetUsage() == RequiredUsage)
				{
					OutEntries.Add(FString::Printf(TEXT("%-44s %s"), *AssetName, *PackageName));
				}
			}
		}

		OutEntries.Sort();
	}

	UNiagaraScript* FModuleLibrary::FindModule(const FString& Name, FString& OutError)
	{
		return FindScript(Name, /*bDynamicInput=*/false, OutError);
	}

	UNiagaraScript* FModuleLibrary::FindDynamicInput(const FString& Name, FString& OutError)
	{
		return FindScript(Name, /*bDynamicInput=*/true, OutError);
	}

	bool FModuleLibrary::EnsureProbeSystem(FString& OutError)
	{
		if (ProbeSystem != nullptr)
		{
			return true;
		}
		if (bProbeSystemFailed)
		{
			OutError = TEXT("The probe system could not be created earlier in this build.");
			return false;
		}

		TArray<FString> Errors;
		bool bCreated = false;

		// /Temp is the engine's scratch mount: nothing under it is ever written to disk, so the probe
		// leaves no trace even if the build crashes partway through.
		ProbeSystem = FNiagaraAdapter::AcquireSystem(TEXT("/Temp/DreamFX"), TEXT("DreamFXSchemaProbe"), bCreated, Errors);
		if (ProbeSystem == nullptr)
		{
			bProbeSystemFailed = true;
			OutError = FString::Join(Errors, TEXT(" | "));
			return false;
		}

		// Rooted for the library's lifetime: a long read collects garbage between modules, and this
		// member is not a reference GC can see. Without the root the probe is destroyed mid-build and
		// the pointer keeps looking valid.
		ProbeSystem->AddToRoot();

		Errors.Reset();
		if (!FNiagaraAdapter::AddEmitter(ProbeSystem, TEXT("Probe"), Errors))
		{
			bProbeSystemFailed = true;
			ProbeSystem->RemoveFromRoot();
			ProbeSystem = nullptr;
			OutError = FString::Join(Errors, TEXT(" | "));
			return false;
		}

		return true;
	}

	namespace
	{
		/**
		 * A switch assignment, spelled so that two assignments are the same string exactly when they
		 * would produce the same module.
		 *
		 * The literal case hexes the bytes rather than printing them: a switch is an int, a bool or an
		 * enum, so the blob is four bytes, and comparing the raw memory is the only comparison that is
		 * right for every struct without a per-type table.
		 */
		FString DescribeValueForCacheKey(const FInputValue& Value)
		{
			switch (Value.Mode)
			{
			case EInputValueMode::Literal:
				return FString::Printf(TEXT("L%s:%s"),
					Value.LiteralStruct != nullptr ? *Value.LiteralStruct->GetName() : TEXT("?"),
					*BytesToHex(Value.LiteralBytes.GetData(), Value.LiteralBytes.Num()));

			case EInputValueMode::Enum:
				return FString::Printf(TEXT("E%s:%s"),
					Value.EnumType != nullptr ? *Value.EnumType->GetPathName() : TEXT("?"),
					*Value.EnumEntryName.ToString());

			default:
				// Nothing else can drive a static switch, but a key that silently collapsed two
				// different values into one entry would hand back the wrong schema.
				return FString::Printf(TEXT("M%d"), static_cast<int32>(Value.Mode));
			}
		}

		FString MakeStackSchemaKey(const UNiagaraScript* Module, EStackKind Stack,
			TArrayView<const TPair<FName, FInputValue>> SwitchValues, const FGuid& VersionGuid)
		{
			FString Key = FString::Printf(TEXT("%s|%d|%s"), *Module->GetPathName(), static_cast<int32>(Stack),
				*VersionGuid.ToString(EGuidFormats::Digits));
			// In application order, not sorted: the order is what the caller derived from the source,
			// and a switch gated by another switch only writes in one of the two orders.
			for (const TPair<FName, FInputValue>& Switch : SwitchValues)
			{
				Key += FString::Printf(TEXT("|%s=%s"), *Switch.Key.ToString(), *DescribeValueForCacheKey(Switch.Value));
			}
			return Key;
		}
	}

	const FModuleSchema* FModuleLibrary::GetStackSchema(UNiagaraScript* Module, EStackKind Stack, FString& OutError)
	{
		return GetStackSchema(Module, Stack, TArrayView<const TPair<FName, FInputValue>>(), FGuid(), OutError);
	}

	const FModuleSchema* FModuleLibrary::GetStackSchema(UNiagaraScript* Module, EStackKind Stack,
		TArrayView<const TPair<FName, FInputValue>> SwitchValues, FString& OutError)
	{
		return GetStackSchema(Module, Stack, SwitchValues, FGuid(), OutError);
	}

	const FModuleSchema* FModuleLibrary::GetStackSchema(UNiagaraScript* Module, EStackKind Stack,
		TArrayView<const TPair<FName, FInputValue>> SwitchValues, const FGuid& VersionGuid, FString& OutError)
	{
		if (Module == nullptr)
		{
			OutError = TEXT("Cannot read the schema of a null module.");
			return nullptr;
		}

		const FString Key = MakeStackSchemaKey(Module, Stack, SwitchValues, VersionGuid);
		if (const TUniquePtr<FModuleSchema>* Cached = StackSchemaCache.Find(Key))
		{
			return Cached->Get();
		}

		FString ProbeError;
		if (!EnsureProbeSystem(ProbeError))
		{
			// Falling back rather than failing: the coarser asset schema still catches most mistakes,
			// and refusing to build at all because a scratch system could not be made would be worse.
			UE_LOG(LogDreamFX, Warning,
				TEXT("Falling back to the asset-level schema for '%s': %s"), *Module->GetName(), *ProbeError);
			return GetModuleSchema(Module, OutError);
		}

		// A probe is a module add, a recompile and a topology read -- the same debris every other
		// adapter call leaves, and a build probes once per distinct configuration.
		FNiagaraAdapter::CollectIfHeavy();

		const FName ScriptName = FNiagaraAdapter::ScriptNameForStack(Stack);
		FStackAddress Address(ProbeSystem);
		if (!IsSystemScopeStack(Stack))
		{
			Address = Address.WithEmitter(TEXT("Probe"));
		}
		Address = Address.WithScript(ScriptName);

		TArray<FString> Errors;
		FName AddedName;
		if (!FNiagaraAdapter::AddModule(Address, Module, AddedName, Errors))
		{
			OutError = FString::Printf(TEXT("Could not probe module '%s' in the %s stack: %s"),
				*Module->GetName(), LexStackKind(Stack), *FString::Join(Errors, TEXT(" | ")));
			return nullptr;
		}

		const FStackAddress ModuleAddress = Address.WithModule(AddedName);

		// R1b. Before the switches, because the version decides which switches the module even has.
		// A failure here is fatal to the probe rather than a warning: reading the newest version's
		// topology and calling it the pinned version's is precisely the silent wrong answer this
		// whole mechanism exists to stop.
		if (VersionGuid.IsValid())
		{
			Errors.Reset();
			if (!FNiagaraAdapter::SetModuleScriptVersion(ModuleAddress, VersionGuid, Errors))
			{
				OutError = FString::Printf(TEXT("Could not probe module '%s' at the pinned script version: %s"),
					*Module->GetName(), *FString::Join(Errors, TEXT(" | ")));

				TArray<FString> RemoveErrors;
				FNiagaraAdapter::RemoveModule(ModuleAddress, RemoveErrors);
				return nullptr;
			}
		}

		// R1. Written before the topology is read, and in the caller's order, because each one
		// recompiles the module and changes what the next read reports.
		//
		// A refused write is logged rather than fatal: the switch may be hidden by another switch the
		// source did not set, and the reads below then report a module missing the inputs that switch
		// would have revealed -- which is exactly the DFX3003 the author needs to see, naming the input
		// rather than an internal probe failure.
		for (const TPair<FName, FInputValue>& Switch : SwitchValues)
		{
			Errors.Reset();
			if (!FNiagaraAdapter::SetInput(ModuleAddress.WithInput(Switch.Key), Switch.Value, Errors))
			{
				UE_LOG(LogDreamFX, Warning,
					TEXT("Could not set static switch '%s' while probing module '%s' in the %s stack: %s. The inputs it reveals will not be visible to the type check."),
					*Switch.Key.ToString(), *Module->GetName(), LexStackKind(Stack),
					*FString::Join(Errors, TEXT(" | ")));
			}
		}

		FModuleInfo Info;
		FModuleSchema Schema;
		bool bReadTopology = false;
		{
			// Everything from here to the closing brace only reads, and the per-input call below made
			// this the most expensive loop in a build: one whole probe-system view model per input, on
			// every module of every source. Sharing one across the burst is what FReadScope is for, and
			// the switch writes above are deliberately outside it -- they change what these reads
			// report, which is the one thing a shared view model cannot survive.
			FNiagaraAdapter::FReadScope ReadScope(ProbeSystem);

			Errors.Reset();
			bReadTopology = FNiagaraAdapter::GetModuleInfo(ModuleAddress, Info, Errors);

			if (bReadTopology)
			{
				for (const FInputInfo& Input : Info.Inputs)
				{
					FInputSchema InputSchema;
					InputSchema.Name = Input.Name;
					InputSchema.Type = Input.Type;
					InputSchema.bIsStaticSwitch = Input.bStaticSwitch;

					// bSupportsExpressions is only knowable per live input, and it gates whether an
					// `hlsl { }` block is legal here (Phase 3), so it is worth the extra call.
					TArray<FString> InputErrors;
					FInputSchema Detail;
					if (FNiagaraAdapter::GetInputSchema(ModuleAddress.WithInput(Input.Name), Detail, InputErrors))
					{
						InputSchema.bSupportsExpressions = Detail.bSupportsExpressions;
						InputSchema.Category = Detail.Category;
						InputSchema.Description = Detail.Description;
					}

					Schema.Inputs.Add(MoveTemp(InputSchema));
				}
			}
		}

		// Always remove, even when the topology read failed: a probe left in the stack would change
		// what the next module sees.
		Errors.Reset();
		FNiagaraAdapter::RemoveModule(ModuleAddress, Errors);

		if (!bReadTopology)
		{
			OutError = FString::Printf(TEXT("Could not read the topology of probed module '%s': %s"),
				*Module->GetName(), *FString::Join(Errors, TEXT(" | ")));
			return nullptr;
		}

		return StackSchemaCache.Add(Key, MakeUnique<FModuleSchema>(MoveTemp(Schema))).Get();
	}

	const TMap<FName, FInputValue>* FModuleLibrary::GetStackDefaults(UNiagaraScript* Module, EStackKind Stack, FString& OutError)
	{
		return GetStackDefaults(Module, Stack, FGuid(), OutError);
	}

	const TMap<FName, FInputValue>* FModuleLibrary::GetStackDefaults(UNiagaraScript* Module, EStackKind Stack,
		const FGuid& VersionGuid, FString& OutError)
	{
		if (Module == nullptr)
		{
			OutError = TEXT("Cannot read the defaults of a null module.");
			return nullptr;
		}

		const FString Key = MakeStackSchemaKey(Module, Stack, TArrayView<const TPair<FName, FInputValue>>(), VersionGuid);
		if (const TUniquePtr<TMap<FName, FInputValue>>* Cached = StackDefaultsCache.Find(Key))
		{
			return Cached->Get();
		}

		if (!EnsureProbeSystem(OutError))
		{
			return nullptr;
		}

		FStackAddress Address(ProbeSystem);
		if (!IsSystemScopeStack(Stack))
		{
			Address = Address.WithEmitter(TEXT("Probe"));
		}
		Address = Address.WithScript(FNiagaraAdapter::ScriptNameForStack(Stack));

		TArray<FString> Errors;
		FName AddedName;
		if (!FNiagaraAdapter::AddModule(Address, Module, AddedName, Errors))
		{
			OutError = FString::Printf(TEXT("Could not probe module '%s' for defaults: %s"),
				*Module->GetName(), *FString::Join(Errors, TEXT(" | ")));
			return nullptr;
		}

		const FStackAddress ModuleAddress = Address.WithModule(AddedName);

		if (VersionGuid.IsValid())
		{
			Errors.Reset();
			if (!FNiagaraAdapter::SetModuleScriptVersion(ModuleAddress, VersionGuid, Errors))
			{
				OutError = FString::Printf(TEXT("Could not probe module '%s' for defaults at its own script version: %s"),
					*Module->GetName(), *FString::Join(Errors, TEXT(" | ")));

				TArray<FString> RemoveErrors;
				FNiagaraAdapter::RemoveModule(ModuleAddress, RemoveErrors);
				return nullptr;
			}
		}

		TArray<TTuple<FName, FInputValue>> Values;
		Errors.Reset();
		const bool bRead = FNiagaraAdapter::GetModuleInputValues(ModuleAddress, Values, Errors);

		TMap<FName, FInputValue> Defaults;
		for (TTuple<FName, FInputValue>& Entry : Values)
		{
			Defaults.Add(Entry.Get<0>(), MoveTemp(Entry.Get<1>()));
		}

		Errors.Reset();
		FNiagaraAdapter::RemoveModule(ModuleAddress, Errors);

		if (!bRead)
		{
			OutError = FString::Printf(TEXT("Could not read default values for '%s'."), *Module->GetName());
			return nullptr;
		}

		return StackDefaultsCache.Add(Key, MakeUnique<TMap<FName, FInputValue>>(MoveTemp(Defaults))).Get();
	}

	const FString* FModuleLibrary::GetRendererDefaults(UClass* RendererClass, FString& OutError)
	{
		if (RendererClass == nullptr)
		{
			OutError = TEXT("Cannot read the defaults of a null renderer class.");
			return nullptr;
		}

		if (const TUniquePtr<FString>* Cached = RendererDefaultsCache.Find(RendererClass))
		{
			return Cached->Get();
		}

		if (!EnsureProbeSystem(OutError))
		{
			return nullptr;
		}

		const FStackAddress EmitterAddress = FStackAddress(ProbeSystem).WithEmitter(TEXT("Probe"));

		TArray<FString> Errors;
		int32 Index = INDEX_NONE;
		if (!FNiagaraAdapter::AddRenderer(EmitterAddress, RendererClass, Index, Errors))
		{
			OutError = FString::Printf(TEXT("Could not probe renderer '%s': %s"),
				*RendererClass->GetName(), *FString::Join(Errors, TEXT(" | ")));
			return nullptr;
		}

		FString Json;
		Errors.Reset();
		const bool bRead = FNiagaraAdapter::GetRendererProperties(EmitterAddress.WithRenderer(Index), Json, Errors);

		Errors.Reset();
		FNiagaraAdapter::RemoveRenderer(EmitterAddress.WithRenderer(Index), Errors);

		if (!bRead)
		{
			OutError = FString::Printf(TEXT("Could not read default properties for '%s'."), *RendererClass->GetName());
			return nullptr;
		}

		return RendererDefaultsCache.Add(RendererClass, MakeUnique<FString>(MoveTemp(Json))).Get();
	}

	bool FModuleLibrary::GetRendererBindingDefaults(UClass* RendererClass,
		TArray<TPair<FString, FName>>& OutBindings, FString& OutError)
	{
		if (RendererClass == nullptr)
		{
			OutError = TEXT("Cannot read the bindings of a null renderer class.");
			return false;
		}

		if (const TArray<TPair<FString, FName>>* Cached = RendererBindingDefaultsCache.Find(RendererClass))
		{
			OutBindings = *Cached;
			return true;
		}

		if (!EnsureProbeSystem(OutError))
		{
			return false;
		}

		const FStackAddress EmitterAddress = FStackAddress(ProbeSystem).WithEmitter(TEXT("Probe"));

		TArray<FString> Errors;
		int32 Index = INDEX_NONE;
		if (!FNiagaraAdapter::AddRenderer(EmitterAddress, RendererClass, Index, Errors))
		{
			OutError = FString::Join(Errors, TEXT(" | "));
			return false;
		}

		TArray<TPair<FString, FName>> Bindings;
		Errors.Reset();
		const bool bRead = FNiagaraAdapter::GetRendererBindings(EmitterAddress.WithRenderer(Index), Bindings, Errors);

		Errors.Reset();
		FNiagaraAdapter::RemoveRenderer(EmitterAddress.WithRenderer(Index), Errors);

		if (!bRead)
		{
			OutError = TEXT("Could not read default bindings.");
			return false;
		}

		OutBindings = Bindings;
		RendererBindingDefaultsCache.Add(RendererClass, MoveTemp(Bindings));
		return true;
	}

	const FString* FModuleLibrary::GetEmitterDefaults(FString& OutError)
	{
		if (EmitterDefaults.IsSet())
		{
			return &EmitterDefaults.GetValue();
		}

		if (!EnsureProbeSystem(OutError))
		{
			return nullptr;
		}

		FString Json;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetEmitterProperties(FStackAddress(ProbeSystem).WithEmitter(TEXT("Probe")), Json, Errors))
		{
			OutError = FString::Join(Errors, TEXT(" | "));
			return nullptr;
		}

		EmitterDefaults = MoveTemp(Json);
		return &EmitterDefaults.GetValue();
	}

	const FModuleSchema* FModuleLibrary::GetModuleSchema(const UNiagaraScript* Module, FString& OutError)
	{
		if (Module == nullptr)
		{
			OutError = TEXT("Cannot read the schema of a null module.");
			return nullptr;
		}

		if (const TUniquePtr<FModuleSchema>* Cached = SchemaCache.Find(Module))
		{
			return Cached->Get();
		}

		FModuleSchema Schema;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetModuleSchema(Module, Schema, Errors))
		{
			OutError = FString::Join(Errors, TEXT(" | "));
			return nullptr;
		}

		return SchemaCache.Add(Module, MakeUnique<FModuleSchema>(MoveTemp(Schema))).Get();
	}

	const FModuleSchema* FModuleLibrary::GetDynamicInputSchema(const UNiagaraScript* DynamicInput, FString& OutError)
	{
		if (DynamicInput == nullptr)
		{
			OutError = TEXT("Cannot read the schema of a null dynamic input.");
			return nullptr;
		}

		if (const TUniquePtr<FModuleSchema>* Cached = SchemaCache.Find(DynamicInput))
		{
			return Cached->Get();
		}

		FModuleSchema Schema;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetDynamicInputSchema(DynamicInput, Schema, Errors))
		{
			OutError = FString::Join(Errors, TEXT(" | "));
			return nullptr;
		}

		return SchemaCache.Add(DynamicInput, MakeUnique<FModuleSchema>(MoveTemp(Schema))).Get();
	}

	const FModuleSchema* FModuleLibrary::GetDynamicInputStackSchema(UNiagaraScript* DynamicInput,
		const FNiagaraTypeDefinition& HostType, FString& OutError)
	{
		return GetDynamicInputStackSchema(DynamicInput, HostType, FGuid(), OutError);
	}

	const FModuleSchema* FModuleLibrary::GetDynamicInputStackSchema(UNiagaraScript* DynamicInput,
		const FNiagaraTypeDefinition& HostType, const FGuid& VersionGuid, FString& OutError)
	{
		if (DynamicInput == nullptr)
		{
			OutError = TEXT("Cannot read the schema of a null dynamic input.");
			return nullptr;
		}

		// The host type is part of the key as well as the version: the same dynamic input plugged into
		// a float and into a Vector3 is probed through different Set Parameters entries.
		const FString Key = FString::Printf(TEXT("%s|%s|%s"),
			*DynamicInput->GetPathName(),
			HostType.IsValid() ? *HostType.GetName() : TEXT("-"),
			*VersionGuid.ToString(EGuidFormats::Digits));

		if (const TUniquePtr<FModuleSchema>* Cached = DynamicInputStackSchemaCache.Find(Key))
		{
			return Cached->Get();
		}

		// The asset schema first: it is what the probe falls back to.
		const FModuleSchema* AssetSchema = GetDynamicInputSchema(DynamicInput, OutError);
		if (AssetSchema == nullptr)
		{
			return nullptr;
		}

		// Copied by value up front: the probe below reads other schemas, and even with the cache boxed
		// a fallback has to own its copy rather than alias whatever AssetSchema pointed at.
		const FModuleSchema Fallback = *AssetSchema;

		auto FallBack = [this, DynamicInput, &Fallback, &Key](const FString& Reason) -> const FModuleSchema*
		{
			UE_LOG(LogDreamFX, Warning,
				TEXT("Falling back to the asset-level schema for dynamic input '%s': %s Static switches on it stay invisible."),
				*DynamicInput->GetName(), *Reason);
			return DynamicInputStackSchemaCache.Add(Key, MakeUnique<FModuleSchema>(Fallback)).Get();
		};

		if (!HostType.IsValid())
		{
			// Deliberately not cached: a later call from a site that does know the host type should
			// still get the real probe rather than this degraded answer.
			return AssetSchema;
		}

		FString ProbeError;
		if (!EnsureProbeSystem(ProbeError))
		{
			return FallBack(ProbeError);
		}

		// A Set Parameters entry, because it is the one module that can be created with an input of
		// any given type -- there is no stock module with an input of every dynamic input's output.
		const FStackAddress StackAddress = FStackAddress(ProbeSystem)
			.WithEmitter(TEXT("Probe"))
			.WithScript(FNiagaraAdapter::ScriptNameForStack(EStackKind::ParticleUpdate));

		const FName ProbeParameter(TEXT("Particles.DreamFXDynamicInputProbe"));
		TArray<TTuple<FName, FNiagaraTypeDefinition, FInputValue>> Entries;
		Entries.Emplace(ProbeParameter, HostType, FInputValue());

		// Traced because each of these steps recompiles the probe emitter, and a step that does not
		// come back leaves no other evidence of which one it was.
		UE_LOG(LogDreamFX, Verbose, TEXT("        probe: adding a Set Parameters entry of type %s"),
			*HostType.GetName());

		TArray<FString> Errors;
		FName AddedName;
		if (!FNiagaraAdapter::AddSetParametersModule(StackAddress, Entries, AddedName, Errors))
		{
			return FallBack(*FString::Join(Errors, TEXT(" | ")));
		}

		const FStackAddress ModuleAddress = StackAddress.WithModule(AddedName);
		const FStackAddress InputAddress = ModuleAddress.WithInput(ProbeParameter);

		FModuleSchema Probed;
		bool bProbed = false;

		// R1b: bound to the pinned version as it is written, because the version decides which inputs
		// the chain has and what type each one is.
		Errors.Reset();
		if (FNiagaraAdapter::SetDynamicInputAtVersion(InputAddress, DynamicInput, VersionGuid, Errors))
		{
			// Read-only from here, and per-input again: one view model per child without the scope.
			// Opened after the write above, which is what put the chain there to be read.
			FNiagaraAdapter::FReadScope ReadScope(ProbeSystem);

			TArray<FDynamicInputChild> Children;
			Errors.Reset();
			if (FNiagaraAdapter::GetDynamicInputChildren(InputAddress, Children, Errors) && Children.Num() > 0)
			{
				for (const FDynamicInputChild& Child : Children)
				{
					FInputSchema InputSchema;
					InputSchema.Name = Child.Name;
					InputSchema.Type = Child.Type;
					InputSchema.bIsStaticSwitch = Child.bStaticSwitch;

					// bSupportsExpressions is only knowable per live input, and it gates whether an
					// `hlsl { }` block is legal here -- same reason GetStackSchema pays for this call.
					TArray<FString> InputErrors;
					FInputSchema Detail;
					if (FNiagaraAdapter::GetInputSchema(InputAddress.WithInput(Child.Name), Detail, InputErrors))
					{
						InputSchema.bSupportsExpressions = Detail.bSupportsExpressions;
						InputSchema.Category = Detail.Category;
						InputSchema.Description = Detail.Description;
					}

					Probed.Inputs.Add(MoveTemp(InputSchema));
				}
				bProbed = true;
			}
		}

		// Always remove, even when the probe failed: a leftover Set Parameters module would change
		// what the next module added to this stack sees.
		Errors.Reset();
		if (!FNiagaraAdapter::RemoveModule(ModuleAddress, Errors))
		{
			UE_LOG(LogDreamFX, Warning, TEXT("Could not remove the dynamic input probe module: %s"),
				*FString::Join(Errors, TEXT(" | ")));
		}

		if (!bProbed)
		{
			return FallBack(TEXT("the probe chain could not be read."));
		}

		return DynamicInputStackSchemaCache.Add(Key, MakeUnique<FModuleSchema>(MoveTemp(Probed))).Get();
	}
}
