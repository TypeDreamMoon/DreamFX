#include "DreamFXCommandlet.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "Generation/DreamFXGenerator.h"
#include "Lint/DreamFXLint.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Schema/DreamFXModuleLibrary.h"

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
}

namespace
{
	/** Exports one Niagara system back to source, to a file or to the log. */
	int32 RunDecompile(const FString& AssetPath, const FString& OutputPath, const FString& RootToken)
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

		FDiagnosticSink Diagnostics;
		const FDecompileResult Result = FDecompiler::Decompile(System, RootToken, Diagnostics);

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
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")).Get();

		const FString Root = SearchRoot.IsEmpty() ? TEXT("/Game") : SearchRoot;
		AssetRegistry.ScanPathsSynchronous({ Root }, /*bForceRescan=*/true, /*bIgnoreDenyListScanFilters=*/true);
		AssetRegistry.WaitForCompletion();

		FARFilter Filter;
		Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(*Root));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);

		UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX coverage over %d Niagara system(s) under %s ==="),
			Assets.Num(), *Root);

		int32 Exported = 0;
		int32 Failed = 0;
		TMap<FString, int32> FeatureCounts;

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
			}
		}

		return Failed;
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
	FString SchemaQuery;
	if (FParse::Value(*Params, TEXT("Schema="), SchemaQuery))
	{
		FString StackName;
		FParse::Value(*Params, TEXT("Stack="), StackName);
		return DumpSchema(SchemaQuery, StackName);
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
		return RunDecompile(DecompileTarget, OutputPath, RootToken.IsEmpty() ? TEXT("Game") : RootToken);
	}

	if (FParse::Param(*Params, TEXT("Coverage")))
	{
		FString SearchRoot;
		FParse::Value(*Params, TEXT("Path="), SearchRoot);
		return RunCoverage(SearchRoot);
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
	Options.bForce = FParse::Param(*Params, TEXT("Force"));
	Options.bSave = !FParse::Param(*Params, TEXT("NoSave")) && !Options.bVerifyOnly;

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

	const TCHAR* const ModeLabel = bLintOnly ? TEXT("lint") : (Options.bVerifyOnly ? TEXT("verify") : TEXT("build"));
	UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX %s: %d source file(s) ==="), ModeLabel, SourceFiles.Num());

	int32 TotalErrors = 0;
	int32 TotalWarnings = 0;
	int32 Built = 0;
	int32 Skipped = 0;
	int32 Failed = 0;

	for (const FString& SourceFile : SourceFiles)
	{
		// Only .dfs produces an asset today. Modules and emitters are parsed for syntax checking so a
		// broken .dfm still fails the gate rather than waiting until something references it.
		EDocumentKind Kind = EDocumentKind::System;
		FParser::DocumentKindFromExtension(FPaths::GetExtension(SourceFile), Kind);

		FDiagnosticSink Diagnostics;

		if (bLintOnly || Kind != EDocumentKind::System)
		{
			FDocument Document;
			if (FParser::ParseFile(SourceFile, Document, Diagnostics))
			{
				FLint::Run(Document, Diagnostics);
			}
		}
		else
		{
			const FGenerateResult Result = FGenerator::GenerateFromFile(SourceFile, Options, Diagnostics);
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
		}

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
	}

	UE_LOG(LogDreamFX, Display,
		TEXT("=== DreamFX done: %d %s, %d up to date, %d failed | %d error(s), %d warning(s) ==="),
		Built, Options.bVerifyOnly ? TEXT("verified") : TEXT("built"),
		Skipped, Failed, TotalErrors, TotalWarnings);

	return TotalErrors;
}
