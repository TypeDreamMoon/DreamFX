#include "DreamFXModuleLibrary.h"

#include "DreamFXModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace UE::DreamFX::Editor
{
	FModuleLibrary::FModuleLibrary()
	{
		AddDefaultSearchPaths();
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

		const ENiagaraScriptUsage RequiredUsage = bDynamicInput
			? ENiagaraScriptUsage::DynamicInput : ENiagaraScriptUsage::Module;
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
			if (Script->GetUsage() != RequiredUsage)
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
				if (Script->GetUsage() == RequiredUsage)
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

		Errors.Reset();
		if (!FNiagaraAdapter::AddEmitter(ProbeSystem, TEXT("Probe"), Errors))
		{
			bProbeSystemFailed = true;
			ProbeSystem = nullptr;
			OutError = FString::Join(Errors, TEXT(" | "));
			return false;
		}

		return true;
	}

	const FModuleSchema* FModuleLibrary::GetStackSchema(UNiagaraScript* Module, EStackKind Stack, FString& OutError)
	{
		if (Module == nullptr)
		{
			OutError = TEXT("Cannot read the schema of a null module.");
			return nullptr;
		}

		const TPair<TWeakObjectPtr<const UNiagaraScript>, EStackKind> Key(Module, Stack);
		if (const FModuleSchema* Cached = StackSchemaCache.Find(Key))
		{
			return Cached;
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

		FModuleInfo Info;
		Errors.Reset();
		const bool bReadTopology = FNiagaraAdapter::GetModuleInfo(ModuleAddress, Info, Errors);

		FModuleSchema Schema;
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

		return &StackSchemaCache.Add(Key, MoveTemp(Schema));
	}

	const FModuleSchema* FModuleLibrary::GetModuleSchema(const UNiagaraScript* Module, FString& OutError)
	{
		if (Module == nullptr)
		{
			OutError = TEXT("Cannot read the schema of a null module.");
			return nullptr;
		}

		if (const FModuleSchema* Cached = SchemaCache.Find(Module))
		{
			return Cached;
		}

		FModuleSchema Schema;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetModuleSchema(Module, Schema, Errors))
		{
			OutError = FString::Join(Errors, TEXT(" | "));
			return nullptr;
		}

		return &SchemaCache.Add(Module, MoveTemp(Schema));
	}

	const FModuleSchema* FModuleLibrary::GetDynamicInputSchema(const UNiagaraScript* DynamicInput, FString& OutError)
	{
		if (DynamicInput == nullptr)
		{
			OutError = TEXT("Cannot read the schema of a null dynamic input.");
			return nullptr;
		}

		if (const FModuleSchema* Cached = SchemaCache.Find(DynamicInput))
		{
			return Cached;
		}

		FModuleSchema Schema;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetDynamicInputSchema(DynamicInput, Schema, Errors))
		{
			OutError = FString::Join(Errors, TEXT(" | "));
			return nullptr;
		}

		return &SchemaCache.Add(DynamicInput, MoveTemp(Schema));
	}
}
