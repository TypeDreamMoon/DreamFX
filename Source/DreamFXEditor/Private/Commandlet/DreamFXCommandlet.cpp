#include "DreamFXCommandlet.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
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
	int32 DumpSchema(const FString& ModuleName)
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

		const FModuleSchema* Schema = bDynamicInput
			? Library.GetDynamicInputSchema(Script, Error)
			: Library.GetModuleSchema(Script, Error);
		if (Schema == nullptr)
		{
			UE_LOG(LogDreamFX, Error, TEXT("Could not read schema: %s"), *Error);
			return 1;
		}

		UE_LOG(LogDreamFX, Display, TEXT("%s '%s' -> %s"),
			bDynamicInput ? TEXT("DynamicInput") : TEXT("Module"), *ModuleName, *Script->GetPathName());
		UE_LOG(LogDreamFX, Display, TEXT("  %d input(s):"), Schema->Inputs.Num());
		for (const FInputSchema& Input : Schema->Inputs)
		{
			UE_LOG(LogDreamFX, Display, TEXT("    %-40s %-24s%s%s"),
				*Input.Name.ToString(),
				*Input.Type.GetName(),
				Input.bSupportsExpressions ? TEXT(" [hlsl-ok]") : TEXT(""),
				Input.Category.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Input.Category));
		}
		return 0;
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
		return DumpSchema(SchemaQuery);
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

	UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX %s: %d source file(s) ==="),
		Options.bVerifyOnly ? TEXT("verify") : TEXT("build"), SourceFiles.Num());

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

		if (Kind != EDocumentKind::System)
		{
			FDocument Document;
			FParser::ParseFile(SourceFile, Document, Diagnostics);
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
		TEXT("=== DreamFX done: %d built, %d up to date, %d failed | %d error(s), %d warning(s) ==="),
		Built, Skipped, Failed, TotalErrors, TotalWarnings);

	return TotalErrors;
}
