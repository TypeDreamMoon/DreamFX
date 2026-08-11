#include "DreamFXCommandlet.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "Generation/DreamFXGenerator.h"
#include "Lint/DreamFXLint.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Algo/StableSort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
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
	// The house rule's enforcer (plan.md design principle 5): proving anything about an asset takes
	// asset-level evidence, and until this existed the only routine comparison channel WAS the
	// export -- both sides of L1 are the same lossy exporter's output, so a loss on the export side
	// is structurally invisible there. This walks the assets themselves by reflection and compares
	// facts as multisets: what each side has that the other does not, never "first difference".

	/**
	 * Identity noise scrubbed out of a fact, so two assets that differ only in who they are compare
	 * equal: 32-hex guids, `_123` autoname suffixes (subobject names, node names), and references to
	 * the asset's own package. Everything else -- values, real names, resolution state -- survives.
	 */
	FString ScrubIdentity(const FString& Text, const FString& SelfPackage)
	{
		FString Out;
		Out.Reserve(Text.Len());

		int32 Index = 0;
		while (Index < Text.Len())
		{
			if (!SelfPackage.IsEmpty() && FCString::Strncmp(&Text[Index], *SelfPackage, SelfPackage.Len()) == 0)
			{
				Out += TEXT("<self>");
				Index += SelfPackage.Len();
				continue;
			}

			const TCHAR Character = Text[Index];

			auto IsHex = [](TCHAR C) { return FChar::IsDigit(C) || (C >= 'A' && C <= 'F'); };
			// Only a MAXIMAL run of exactly 32 hex characters is a guid. FLT_MAX prints as 39
			// decimal digits, whose 32-character tail this used to eat.
			if (IsHex(Character) && (Index == 0 || !IsHex(Text[Index - 1])))
			{
				int32 Run = Index;
				while (Run < Text.Len() && IsHex(Text[Run]))
				{
					++Run;
				}
				if (Run - Index == 32)
				{
					Out += TEXT("<guid>");
					Index = Run;
					continue;
				}
			}

			if (Character == TEXT('_') && Index + 1 < Text.Len() && FChar::IsDigit(Text[Index + 1]))
			{
				int32 Run = Index + 1;
				while (Run < Text.Len() && FChar::IsDigit(Text[Run]))
				{
					++Run;
				}
				if (Run >= Text.Len() || !FChar::IsAlpha(Text[Run]))
				{
					Out += TEXT("~");
					Index = Run;
					continue;
				}
			}

			Out.AppendChar(Character);
			++Index;
		}
		return Out;
	}

	/** One fact per top-level reflected property, skipping transients and an explicit list. */
	void AppendPropertyFacts(const FString& Prefix, const void* Container, const UStruct* Struct,
		const TSet<FName>& Skip, const FString& SelfPackage, TArray<FString>& OutFacts)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated)
				|| Skip.Contains(Property->GetFName()))
			{
				continue;
			}
			FString Value;
			Property->ExportText_InContainer(0, Value, Container, Container, nullptr, PPF_None);
			OutFacts.Add(FString::Printf(TEXT("%s %s = %s"),
				*Prefix, *Property->GetName(), *ScrubIdentity(Value, SelfPackage)));
		}
	}

	/**
	 * Rapid-iteration and exposed parameter stores, one fact per parameter with its value bytes.
	 *
	 * The module segment of a name has its trailing digits stripped ("Collision001" and "Collision"
	 * are one module authored twice under Niagara's uniquifier), which is what let this tool see
	 * that Down_Root's original DOES carry Advanced Aging Rate -- the claim "the original has no
	 * value at all" had been read off the lossy export.
	 */
	void AppendParameterStoreFacts(const FString& Prefix, FNiagaraParameterStore& Store,
		TArray<FString>& OutFacts)
	{
		TArrayView<const FNiagaraVariableWithOffset> Variables = Store.ReadParameterVariables();
		const TArray<uint8>& Data = Store.GetParameterDataArray();

		for (const FNiagaraVariableWithOffset& Variable : Variables)
		{
			FString Name = Variable.GetName().ToString();
			TArray<FString> Segments;
			Name.ParseIntoArray(Segments, TEXT("."));
			if (Segments.Num() >= 3)
			{
				FString& Module = Segments[2];
				while (Module.Len() > 0 && FChar::IsDigit(Module[Module.Len() - 1]))
				{
					Module.LeftChopInline(1, EAllowShrinking::No);
				}
				Name = FString::Join(Segments, TEXT("."));
			}

			FString Value;
			if (Variable.IsDataInterface() || Variable.IsUObject())
			{
				Value = TEXT("<object>");
			}
			else if (Variable.Offset >= 0 && Variable.Offset + Variable.GetSizeInBytes() <= Data.Num())
			{
				Value = BytesToHex(Data.GetData() + Variable.Offset, Variable.GetSizeInBytes());
			}
			else
			{
				Value = TEXT("<no data>");
			}

			OutFacts.Add(FString::Printf(TEXT("%s %s (%s) = %s"),
				*Prefix, *Name, *Variable.GetType().GetName(), *Value));
		}
	}

	/** Every comparable fact about one system, unsorted; the caller compares as a multiset. */
	void DescribeSystem(UNiagaraSystem* System, TArray<FString>& OutFacts)
	{
		const FString SelfPackage = System->GetOutermost()->GetName();

		// Identity, wiring and editor bookkeeping. Graphs and scripts are deliberately absent as
		// objects -- their observable content arrives through the parameter stores here, the export
		// (L1) and the simulation (L3); their node soup is all identity.
		static const TSet<FName> EmitterDataSkip = {
			TEXT("GraphSource"), TEXT("SpawnScriptProps"), TEXT("UpdateScriptProps"),
			TEXT("EventHandlerScriptProps"), TEXT("SimulationStages"), TEXT("GPUComputeScript"),
			TEXT("EmitterSpawnScriptProps"), TEXT("EmitterUpdateScriptProps"),
			TEXT("ScratchPads"), TEXT("ParentScratchPads"),
			TEXT("VersionedParent"), TEXT("VersionedParentAtLastMerge"),
			TEXT("RendererProperties"), TEXT("MessageStore"), TEXT("Version"),
		};
		static const TSet<FName> RendererSkip = { TEXT("MessageStore"), TEXT("StackMessages") };

		if (UNiagaraScript* SystemSpawn = System->GetSystemSpawnScript())
		{
			AppendParameterStoreFacts(TEXT("ri system-spawn"), SystemSpawn->RapidIterationParameters, OutFacts);
		}
		if (UNiagaraScript* SystemUpdate = System->GetSystemUpdateScript())
		{
			AppendParameterStoreFacts(TEXT("ri system-update"), SystemUpdate->RapidIterationParameters, OutFacts);
		}
		AppendParameterStoreFacts(TEXT("user"), System->GetExposedParameters(), OutFacts);

		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FString EmitterName = Handle.GetName().ToString();
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Data == nullptr)
			{
				OutFacts.Add(FString::Printf(TEXT("emitter %s has no data"), *EmitterName));
				continue;
			}

			OutFacts.Add(FString::Printf(TEXT("emitter %s enabled = %s"),
				*EmitterName, Handle.GetIsEnabled() ? TEXT("true") : TEXT("false")));

			// Parentage is a first-class fact, not a property to scrub: an inheriting original and
			// its flattened mirror SHOULD read differently here, and this line is where that shows.
			const FVersionedNiagaraEmitter Parent = Data->GetParent();
			OutFacts.Add(FString::Printf(TEXT("emitter %s parent = %s"),
				*EmitterName, Parent.Emitter != nullptr ? *Parent.Emitter->GetPathName() : TEXT("none")));

			// Event handlers as dedicated facts, source spoken by NAME. The generic property walk
			// used to SKIP EventHandlerScriptProps -- which is precisely how a mirror with no event
			// handlers at all sailed through this tool while its event-spawned emitters rendered
			// nothing (2026-08-11, Descend/Up). The raw struct would compare on the source handle
			// guid and the script object path, both identity; the name form compares on meaning.
			{
				TArray<FNiagaraAdapter::FEventHandlerSummary> Handlers;
				TArray<FString> HandlerErrors;
				FNiagaraAdapter::GetEmitterEventHandlers(
					FStackAddress(System).WithEmitter(Handle.GetName()), Handlers, HandlerErrors);
				for (const FNiagaraAdapter::FEventHandlerSummary& Handler : Handlers)
				{
					OutFacts.Add(FString::Printf(TEXT("emitter %s event handler: '%s' from '%s' (%s x%d)"),
						*EmitterName, *Handler.SourceEventName.ToString(), *Handler.SourceEmitterName,
						*Handler.ExecutionMode, Handler.SpawnNumber));
				}
			}

			// Simulation stages as dedicated facts, for the same reason as the event handlers above:
			// the raw property walk skips SimulationStages because the array itself is subobject
			// identity, which is exactly how a mirror with no stages at all would sail through this
			// tool while a Grid3D fluid renders nothing. One fact per stage -- class plus full
			// property text, the shape the data interfaces use below. The script object is identity
			// (its content arrives through the export and the simulation), the outer version guid too.
			//
			// DataInterface is reduced to its bound NAME. The raw struct text prints the binding's
			// type handle, and that handle is serialization residue: on authored content it resolves
			// to an unrelated type in any later session (GroomRods' PressureGrid reads as Vector4f)
			// while the asset simulates fine, because the engine resolves the grid by name at
			// compile time and never reads the stored type. Comparing the handle would fail every
			// rebuilt mirror against noise the engine itself ignores.
			{
				static const TSet<FName> StageSkip =
					{ TEXT("Script"), TEXT("OuterEmitterVersion"), TEXT("DataInterface") };
				int32 StageIndex = 0;
				for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
				{
					if (Stage != nullptr)
					{
						TArray<FString> StageFacts;
						AppendPropertyFacts(TEXT(""), Stage, Stage->GetClass(), StageSkip, SelfPackage, StageFacts);
						if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
						{
							StageFacts.Add(FString::Printf(TEXT("DataInterface = %s"),
								*Generic->DataInterface.BoundVariable.GetName().ToString()));
						}
						StageFacts.Sort();
						OutFacts.Add(FString::Printf(TEXT("emitter %s simulation stage %d:%s { %s }"),
							*EmitterName, StageIndex, *Stage->GetClass()->GetName(),
							*FString::Join(StageFacts, TEXT("; "))));
					}
					++StageIndex;
				}
			}

			AppendPropertyFacts(FString::Printf(TEXT("emitter %s"), *EmitterName),
				Data, FVersionedNiagaraEmitterData::StaticStruct(), EmitterDataSkip, SelfPackage, OutFacts);

			TArray<UNiagaraScript*> Scripts;
			Data->GetScripts(Scripts, /*bCompilableOnly=*/true);
			for (UNiagaraScript* Script : Scripts)
			{
				if (Script == nullptr)
				{
					continue;
				}
				const FString Prefix = FString::Printf(TEXT("ri %s %s"),
					*EmitterName, *StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(
						static_cast<int64>(Script->GetUsage())));
				AppendParameterStoreFacts(Prefix, Script->RapidIterationParameters, OutFacts);
			}

			int32 RendererIndex = 0;
			Data->ForEachRenderer([&](UNiagaraRendererProperties* Renderer)
			{
				if (Renderer != nullptr)
				{
					AppendPropertyFacts(
						FString::Printf(TEXT("emitter %s renderer %d:%s"),
							*EmitterName, RendererIndex, *Renderer->GetClass()->GetName()),
						Renderer, Renderer->GetClass(), RendererSkip, SelfPackage, OutFacts);
				}
				++RendererIndex;
			});
		}

		// Data interfaces live as subobjects wherever a module input placed them. One fact per
		// interface, class plus full property text, so a DI whose configuration fails to round-trip
		// shows up as a one-in/one-out pair instead of hiding behind an object path.
		TArray<UObject*> Inner;
		GetObjectsWithOuter(System->GetOutermost(), Inner, /*bIncludeNestedObjects=*/true);
		for (UObject* Object : Inner)
		{
			UNiagaraDataInterface* Interface = Cast<UNiagaraDataInterface>(Object);
			if (Interface == nullptr)
			{
				continue;
			}
			// The cooked LUT family is derived from Curve at save time; comparing it repeats the
			// curve comparison with extra baked noise.
			static const TSet<FName> InterfaceSkip = {
				TEXT("CurveCookedEditorCache"), TEXT("ShaderLUT"), TEXT("LUTMinTime"),
				TEXT("LUTMaxTime"), TEXT("LUTInvTimeRange"), TEXT("LUTNumSamplesMinusOne"),
			};
			TArray<FString> InterfaceFacts;
			AppendPropertyFacts(TEXT(""), Interface, Interface->GetClass(), InterfaceSkip, SelfPackage, InterfaceFacts);
			InterfaceFacts.Sort();
			OutFacts.Add(FString::Printf(TEXT("di %s { %s }"),
				*Interface->GetClass()->GetName(), *FString::Join(InterfaceFacts, TEXT("; "))));
		}
	}

	/**
	 * `-AssetDiff`: compare every original against its mirror at the asset level, as fact multisets.
	 *
	 * The independent mirror the round of 2026-08-11 asked for: it does not touch the exporter, so
	 * an export-side loss that L1 cannot see (both L1 sides being the exporter's own output) lands
	 * here as a fact one side has and the other does not.
	 */
	int32 RunAssetDiff(const FString& SearchRoot)
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

			TArray<FString> LeftFacts;
			TArray<FString> RightFacts;
			DescribeSystem(Original, LeftFacts);
			DescribeSystem(Mirror, RightFacts);

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
		FParse::Value(*Params, TEXT("-Path="), SearchRoot);
		return RunAssetDiff(SearchRoot);
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
