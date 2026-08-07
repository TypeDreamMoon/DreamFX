#include "DreamFXCommandlet.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Lint/DreamFXLint.h"
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
