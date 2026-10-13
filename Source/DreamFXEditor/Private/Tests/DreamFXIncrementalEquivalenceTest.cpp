#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "Diff/DreamFXAssetFacts.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "PackageTools.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogDreamFXIncremental, Log, All);

/**
 * Acceptance suite for incremental builds: an incremental build has to leave the asset a full
 * in-place rebuild of the same source would leave.
 *
 * Two assets, A and B, are built from the same text and then follow the same history of edits.
 * A is built incrementally at every step and B is rebuilt in full, so after each step the two have
 * been given identical text and differ only in how they got there. Mutations accumulate: step k is
 * applied on top of step k-1, which is what catches residue that only builds up over several
 * incremental builds (a stale constant, a node name that drifted, a stack that kept a module the
 * text dropped two edits ago).
 *
 * Two tests share the runs:
 *
 *   DreamFX.Incremental.Equivalence  fails on any difference between A and B, and on a build that
 *                                    one track refuses and the other accepts.
 *   DreamFX.Incremental.Coverage     the same runs, plus an assertion per mutation that the build was
 *                                    applied incrementally where the mutation table says it should
 *                                    be, naming each one that fell back and why.
 *
 * The runs are made once per process: whichever test comes first builds the assets, and the other
 * reads the same record, so asking for both costs no more than asking for one.
 *
 * "The same" means four things:
 *
 *   facts    the multiset DescribeSystemFacts reports, which is also what the build safety gate
 *            compares. Rapid-iteration names there have the engine's trailing node-number stripped.
 *   names    the raw rapid-iteration parameter names and the module node names per stack. The facts
 *            above cannot see these: `Foo001` and `Foo` are one module to them. They are checked
 *            here because the engine hands out those numbers by add order, and a build that rebuilds
 *            only one of two stacks sharing a module asset can number its nodes differently from a
 *            build that rebuilds both.
 *   inputs   every module input as the adapter reads it back: link targets, hlsl text and the
 *            dynamic input chains, which are graph state and not rapid-iteration constants, so the
 *            facts do not carry them.
 *   export   the decompiler's text, compared once after the whole history. It is a second reader of
 *            the same state, and being the same lossy reader on both sides it can only add checks,
 *            never remove one. It is not run per step because it costs more than everything else.
 *
 * A difference is reported at the step that made it. Both tracks are then built again from new
 * assets, so that the next step is judged on its own and the same line is not reported sixty times.
 *
 * For narrowing a failure down: -DreamFXIncrementalDump=<dir> writes each step's facts, names and
 * inputs for both tracks; -DreamFXIncrementalFixture=<name> and -DreamFXIncrementalSteps=<a|b>
 * keep one fixture and the mutations whose names contain a word (a + stands for a space);
 * -DreamFXIncrementalNoReload keeps every asset in memory; -LogCmds="LogDreamFXIncremental Verbose"
 * logs each phase of a step.
 */
namespace UE::DreamFX::Editor::IncrementalEquivalence
{
	// -----------------------------------------------------------------------------------------
	// The mutation table
	// -----------------------------------------------------------------------------------------

	/**
	 * How much of the asset a mutation changes, which is what decides whether an incremental build
	 * is expected to handle it without falling back.
	 */
	enum class EScope : uint8
	{
		/** Inputs gain, lose or change a value; no module, renderer or emitter appears or goes. */
		Values,
		/** One stack's module list or one module's input topology changes. */
		Stack,
		/** One emitter's renderers, settings or event handler change; the emitter itself stays. */
		Emitter,
		/** System-level state: user parameters, system settings. */
		System,
		/** An emitter is added or removed. */
		Emitters,
		/** Emitters change places. Their order is part of the plan's identity, so this falls back. */
		Reordered,
		/** The source is invalid. Both tracks must fail with the same codes and leave the asset alone. */
		Rejected,
		/**
		 * The build writes the asset and then reports errors (a stack issue the compile finds). Both
		 * tracks must fail with the same codes and hold the same asset; the text does not carry over,
		 * so the next step recovers from a broken asset.
		 */
		Flagged,
	};

	static const TCHAR* LexScope(EScope Scope)
	{
		switch (Scope)
		{
		case EScope::Values: return TEXT("values");
		case EScope::Stack: return TEXT("stack");
		case EScope::Emitter: return TEXT("emitter");
		case EScope::System: return TEXT("system");
		case EScope::Emitters: return TEXT("emitters");
		case EScope::Reordered: return TEXT("reordered");
		case EScope::Rejected: return TEXT("rejected");
		case EScope::Flagged: return TEXT("flagged");
		}
		return TEXT("?");
	}

	/** Whether the mutation table promises an incremental build for this scope. */
	static bool ExpectsIncremental(EScope Scope)
	{
		return Scope != EScope::Reordered && Scope != EScope::Rejected && Scope != EScope::Flagged;
	}

	/** One textual edit. `Find` has to occur exactly once, so an anchor that goes ambiguous fails loudly. */
	struct FEdit
	{
		FString Find;
		FString Replace;
	};

	enum EMutationFlags : uint8
	{
		None = 0,

		/**
		 * Drops both assets from memory before this step so the builds read them back from disk. This
		 * is how a command line build meets an asset, and it is the only way the digest an
		 * incremental build compares against can be shown to survive a save and a load.
		 */
		ReloadFirst = 1 << 0,

		/** Build A with bForce, so an unchanged text still reaches the incremental path. */
		ForceIncremental = 1 << 1,

		/**
		 * Both tracks build with `-Force`'s override of the safety gate, for an edit the gate refuses
		 * in the full rebuild too. It reads an edit as a loss whenever a stored constant stops being the
		 * value it held and the text does not name that input: a stage added or removed (the engine
		 * re-derives `SimStageExecutionLoops`), an argument inside a dynamic input chain, a gated input
		 * that goes away with its bool, a link or a literal that replaces the other. Without the
		 * override those steps stop the history, and everything after them with it. The two tracks'
		 * warnings are still compared, so the override does not hide a gate that judges them apart.
		 */
		AllowLoss = 1 << 2,
	};

	struct FMutation
	{
		const TCHAR* Name;
		EScope Scope;
		TArray<FEdit> Edits;
		uint8 Flags = None;
	};

	struct FFixture
	{
		const TCHAR* Name;
		/** The source, with `%ASSET%` where the asset's name goes. */
		const TCHAR* Source;
		TArray<FMutation> Mutations;
	};

	/** The anchor lines of a swap: A takes B's place and B takes A's, through a placeholder. */
	static TArray<FEdit> SwapLines(const FString& First, const FString& Second)
	{
		return {
			{ First, TEXT("@@SWAP@@") },
			{ Second, First },
			{ TEXT("@@SWAP@@"), Second },
		};
	}

	// -----------------------------------------------------------------------------------------
	// Running a sequence
	// -----------------------------------------------------------------------------------------

	/** What one step did, for the coverage table and the timing log. */
	struct FStepRecord
	{
		FString Fixture;
		FString Mutation;
		EScope Scope = EScope::Values;
		bool bRequested = false;
		bool bApplied = false;
		FString FallbackReason;
		FString Report;
		double SecondsIncremental = 0.0;
		double SecondsFull = 0.0;
	};

	/** The outcome of every run, produced once per process and read by both tests. */
	struct FSummary
	{
		TArray<FStepRecord> Steps;
		/** Differences between the tracks, build failures that should not be, and mutations that do not apply. */
		TArray<FString> EquivalenceProblems;
		double SecondsIncremental = 0.0;
		double SecondsFull = 0.0;
	};

	/**
	 * Collapses every 32-hex guid to `<guid>`, the way DescribeSystemFacts does for its own facts.
	 *
	 * A Set Parameters node is named after its guid, a data interface's JSON carries ids, and both are
	 * made up afresh by every build. They name an object and say nothing about what it holds, so two
	 * assets built from one text never agree on them. Only a maximal run of exactly 32 hex digits is
	 * taken for one; the trailing node numbers (`Foo001`) are left alone, because those are what the
	 * names channel exists to compare.
	 */
	static FString ScrubGuids(const FString& Text)
	{
		auto IsHex = [](TCHAR C) { return FChar::IsDigit(C) || (C >= 'A' && C <= 'F'); };
		FString Out;
		Out.Reserve(Text.Len());
		int32 Index = 0;
		while (Index < Text.Len())
		{
			if (IsHex(Text[Index]) && (Index == 0 || !IsHex(Text[Index - 1])))
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
			Out.AppendChar(Text[Index]);
			++Index;
		}
		return Out;
	}

	/** One of the two assets. Owns its package file for the length of a run. */
	struct FTrack
	{
		FString Label;
		FString ShortName;
		FString AssetName;
		FString PackageName;
		FString ObjectPath;
		FString PackageFile;
		/** The short name as the facts print it: they collapse the 32-hex guid inside it to `<guid>`. */
		FString ScrubbedShortName;
		TStrongObjectPtr<UNiagaraSystem> Held;

		FTrack(const TCHAR* InLabel, const TCHAR* InFixtureName, const FString& Unique)
			: Label(InLabel)
			, FixtureName(InFixtureName)
		{
			Name(Unique);
		}

		~FTrack()
		{
			Discard();
		}

		/** Gives the track a new asset to build into; the old one is deleted from disk. */
		void Renew(const FString& Unique)
		{
			Discard();
			Name(Unique);
		}

		UNiagaraSystem* Find() const
		{
			return FindObject<UNiagaraSystem>(nullptr, *ObjectPath);
		}

	private:
		FString FixtureName;

		void Name(const FString& Unique)
		{
			ShortName = FString::Printf(TEXT("NS_Incr%s%s_%s"), *FixtureName, *Label, *Unique);
			AssetName = TEXT("DreamFXAutomation/") + ShortName;
			PackageName = TEXT("/Game/") + AssetName;
			ObjectPath = PackageName + TEXT(".") + ShortName;
			PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			ScrubbedShortName = ShortName.Replace(*Unique, TEXT("<guid>"));
		}

		void Discard()
		{
			Held.Reset();
			IFileManager::Get().Delete(*PackageFile, false, true, true);
			const FString DumpBase = FPaths::ProjectSavedDir() / TEXT("DreamFX/BuildSafety") / ShortName;
			for (const TCHAR* Suffix : { TEXT(".before.facts"), TEXT(".after.facts"), TEXT(".lost.facts") })
			{
				IFileManager::Get().Delete(*(DumpBase + Suffix), false, true, true);
			}
		}

	public:

		FString SourceLabel() const
		{
			return FPaths::ProjectSavedDir() / TEXT("DreamFXAutomation/Incremental") / ShortName + TEXT(".dfs");
		}

		/**
		 * Replaces what is specific to this track.
		 *
		 * Both assets are named alike up to one letter (`NS_IncrSparksA_<guid>`, `NS_IncrSparksB_<guid>`),
		 * so the package path and the short name are the whole of the difference, and neither is a
		 * semantic one: they are where the asset lives. The facts already collapse the package path to
		 * `<self>` on their own, and collapse the guid in the short name to `<guid>` before this sees
		 * it, which is why the short name is looked for in both spellings. It turns up bare in object
		 * paths, in the gate's messages and in the export's `Name=`. Nothing else is replaced: a value,
		 * a module or node name, a renderer setting or a constant is never touched.
		 */
		FString Normalise(const FString& Text) const
		{
			FString Out = Text.Replace(*PackageName, TEXT("<self>"));
			Out = Out.Replace(*AssetName, TEXT("<asset-name>"));
			Out = Out.Replace(*ShortName, TEXT("<asset>"));
			return Out.Replace(*ScrubbedShortName, TEXT("<asset>"));
		}
	};

	/** One build, with what the comparison needs from it. */
	struct FOutcome
	{
		bool bParsed = false;
		FGenerateResult Result;
		TArray<FString> ErrorCodes;
		TArray<FString> WarningCodes;
		/** What the build safety gate said when it was overridden, which says what it judged a loss. */
		TArray<FString> GateWarnings;
		FString Diagnostics;
		/** The errors alone: a refusal to save reads better without the infos around it. */
		FString ErrorText;
		double Seconds = 0.0;
	};

	static FOutcome Build(const FString& Text, const FTrack& Track, const FGenerateOptions& Options)
	{
		FOutcome Outcome;
		FDocument Document;
		FDiagnosticSink Diagnostics;
		if (!FParser::ParseText(Text, Track.SourceLabel(), Document, Diagnostics))
		{
			Outcome.Diagnostics = Diagnostics.FormatAll();
			return Outcome;
		}
		Outcome.bParsed = true;
		const double Start = FPlatformTime::Seconds();
		Outcome.Result = FGenerator::Generate(Document, Options, Diagnostics);
		Outcome.Seconds = FPlatformTime::Seconds() - Start;
		for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
		{
			if (Diagnostic.Severity == EDiagnosticSeverity::Error)
			{
				Outcome.ErrorCodes.Add(Diagnostic.Code);
				Outcome.ErrorText += Diagnostic.Format() + TEXT("\n");
			}
			else if (Diagnostic.Severity == EDiagnosticSeverity::Warning)
			{
				Outcome.WarningCodes.Add(Diagnostic.Code);
				if (Diagnostic.Code == TEXT("DFX8018"))
				{
					Outcome.GateWarnings.Add(Track.Normalise(Diagnostic.Message));
				}
			}
		}
		Outcome.ErrorCodes.Sort();
		Outcome.WarningCodes.Sort();
		Outcome.Diagnostics = Diagnostics.FormatAll();
		return Outcome;
	}

	// -----------------------------------------------------------------------------------------
	// Reading an asset
	// -----------------------------------------------------------------------------------------

	/** The raw rapid-iteration names: the engine's node numbering, which the facts strip. */
	static void AppendRawNames(const FString& Prefix, UNiagaraScript* Script, TArray<FString>& OutNames)
	{
		if (Script == nullptr)
		{
			return;
		}
		for (const FNiagaraVariableWithOffset& Variable : Script->RapidIterationParameters.ReadParameterVariables())
		{
			OutNames.Add(FString::Printf(TEXT("%s ri %s (%s)"), *Prefix,
				*ScrubGuids(Variable.GetName().ToString()), *Variable.GetType().GetName()));
		}
	}

	static void AppendStackNames(const FString& Prefix, const FScriptStackInfo& Stack, TArray<FString>& OutNames)
	{
		int32 Position = 0;
		for (const FModuleInfo& Module : Stack.Modules)
		{
			OutNames.Add(FString::Printf(TEXT("%s #%d node %s enabled=%d setparameters=%d"), *Prefix, Position++,
				*ScrubGuids(Module.ModuleName.ToString()), Module.bEnabled ? 1 : 0, Module.bIsSetParameters ? 1 : 0));
		}
	}

	/** Module node names per stack and raw rapid-iteration names per script. */
	static void DescribeNames(UNiagaraSystem* System, TArray<FString>& OutNames)
	{
		AppendRawNames(TEXT("system-spawn"), System->GetSystemSpawnScript(), OutNames);
		AppendRawNames(TEXT("system-update"), System->GetSystemUpdateScript(), OutNames);

		TArray<FString> Errors;
		for (EStackKind Kind : { EStackKind::SystemSpawn, EStackKind::SystemUpdate })
		{
			FScriptStackInfo Stack;
			if (FNiagaraAdapter::GetScriptStackInfo(
				FStackAddress(System).WithScript(FNiagaraAdapter::ScriptNameForStack(Kind)), Stack, Errors))
			{
				AppendStackNames(FString::Printf(TEXT("system %s"), LexStackKind(Kind)), Stack, OutNames);
			}
		}

		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FString EmitterName = Handle.GetName().ToString();
			if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
			{
				TArray<UNiagaraScript*> Scripts;
				Data->GetScripts(Scripts, /*bCompilableOnly=*/true);
				for (UNiagaraScript* Script : Scripts)
				{
					if (Script != nullptr)
					{
						AppendRawNames(FString::Printf(TEXT("emitter %s %s"), *EmitterName,
							*StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script->GetUsage()))),
							Script, OutNames);
					}
				}
			}
			FEmitterInfo Info;
			if (FNiagaraAdapter::GetEmitterInfo(FStackAddress(System).WithEmitter(Handle.GetName()), Info, Errors))
			{
				for (const FScriptStackInfo& Stack : Info.Stacks)
				{
					AppendStackNames(FString::Printf(TEXT("emitter %s %s"), *EmitterName, *Stack.ScriptName.ToString()),
						Stack, OutNames);
				}
			}
		}
	}

	static FString DescribeValue(const FInputValue& Value)
	{
		switch (Value.Mode)
		{
		case EInputValueMode::Unset:
			return TEXT("unset");
		case EInputValueMode::Literal:
			return FString::Printf(TEXT("literal %s %s"),
				Value.LiteralStruct != nullptr ? *Value.LiteralStruct->GetName() : TEXT("?"),
				*BytesToHex(Value.LiteralBytes.GetData(), Value.LiteralBytes.Num()));
		case EInputValueMode::Enum:
			return FString::Printf(TEXT("enum %s::%s"),
				Value.EnumType != nullptr ? *Value.EnumType->GetName() : TEXT("?"), *Value.EnumEntryName.ToString());
		case EInputValueMode::Linked:
			return FString::Printf(TEXT("link %s (%s)"), *Value.LinkedVariable.GetName().ToString(),
				*Value.LinkedVariable.GetType().GetName());
		case EInputValueMode::Hlsl:
			return FString::Printf(TEXT("hlsl { %s }"), *Value.HlslExpression);
		case EInputValueMode::DynamicInput:
			return FString::Printf(TEXT("dynamic input %s"),
				Value.DynamicInputAsset != nullptr ? *Value.DynamicInputAsset->GetPathName() : TEXT("?"));
		case EInputValueMode::DataInterface:
			return FString::Printf(TEXT("data interface %s %s"),
				Value.DataInterfaceClass != nullptr ? *Value.DataInterfaceClass->GetName() : TEXT("?"),
				*ScrubGuids(Value.DataInterfaceJson));
		case EInputValueMode::ObjectAsset:
			return FString::Printf(TEXT("asset %s"),
				Value.ObjectAsset != nullptr ? *Value.ObjectAsset->GetPathName() : TEXT("none"));
		}
		return TEXT("?");
	}

	/** One input and, under a dynamic input, the inputs of its chain. */
	static void AppendInputValue(const FString& Label, const FStackAddress& InputAddress, const FInputValue& Value,
		int32 Depth, TArray<FString>& OutLines)
	{
		OutLines.Add(FString::Printf(TEXT("%s = %s"), *Label, *DescribeValue(Value)));
		if (Value.Mode != EInputValueMode::DynamicInput || Depth >= 6)
		{
			return;
		}
		TArray<FDynamicInputChild> Children;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetDynamicInputChildren(InputAddress, Children, Errors))
		{
			OutLines.Add(Label + TEXT(" children unreadable"));
			return;
		}
		for (const FDynamicInputChild& Child : Children)
		{
			const FStackAddress ChildAddress = InputAddress.WithInput(Child.Name);
			FInputValue ChildValue;
			Errors.Reset();
			if (FNiagaraAdapter::GetInput(ChildAddress, ChildValue, Errors))
			{
				AppendInputValue(Label + TEXT("/") + Child.Name.ToString(), ChildAddress, ChildValue, Depth + 1, OutLines);
			}
			else
			{
				OutLines.Add(FString::Printf(TEXT("%s/%s unreadable"), *Label, *Child.Name.ToString()));
			}
		}
	}

	/**
	 * Every input of every module, as the adapter reads it back -- the decompiler's own reader, minus
	 * the baseline probing that makes a full export cost a second a system.
	 *
	 * This is the channel for what the facts cannot see: a link target, an hlsl body, which dynamic
	 * input sits in a chain. None of those is a rapid-iteration constant.
	 */
	static void DescribeInputs(const FString& Prefix, const FStackAddress& StackAddress, const FScriptStackInfo& Stack,
		TArray<FString>& OutLines)
	{
		int32 Position = 0;
		for (const FModuleInfo& Module : Stack.Modules)
		{
			const FString ModuleLabel = FString::Printf(TEXT("%s #%d %s"), *Prefix, Position++, *ScrubGuids(Module.ModuleName.ToString()));
			const FStackAddress ModuleAddress = StackAddress.WithModule(Module.ModuleName);
			TArray<TTuple<FName, FInputValue>> Values;
			TArray<FString> Errors;
			if (!FNiagaraAdapter::GetModuleInputValues(ModuleAddress, Values, Errors))
			{
				OutLines.Add(ModuleLabel + TEXT(" inputs unreadable"));
				continue;
			}
			for (const TTuple<FName, FInputValue>& Entry : Values)
			{
				AppendInputValue(ModuleLabel + TEXT(" ") + Entry.Get<0>().ToString(), ModuleAddress.WithInput(Entry.Get<0>()),
					Entry.Get<1>(), 0, OutLines);
			}
		}
	}

	static void DescribeAllInputs(UNiagaraSystem* System, TArray<FString>& OutLines)
	{
		TArray<FString> Errors;
		for (EStackKind Kind : { EStackKind::SystemSpawn, EStackKind::SystemUpdate })
		{
			const FStackAddress Address = FStackAddress(System).WithScript(FNiagaraAdapter::ScriptNameForStack(Kind));
			FScriptStackInfo Stack;
			if (FNiagaraAdapter::GetScriptStackInfo(Address, Stack, Errors))
			{
				DescribeInputs(FString::Printf(TEXT("system %s"), LexStackKind(Kind)), Address, Stack, OutLines);
			}
		}
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FStackAddress EmitterAddress = FStackAddress(System).WithEmitter(Handle.GetName());
			FEmitterInfo Info;
			if (!FNiagaraAdapter::GetEmitterInfo(EmitterAddress, Info, Errors))
			{
				continue;
			}
			for (const FScriptStackInfo& Stack : Info.Stacks)
			{
				DescribeInputs(FString::Printf(TEXT("emitter %s %s"), *Handle.GetName().ToString(), *Stack.ScriptName.ToString()),
					EmitterAddress.WithScript(Stack.ScriptName), Stack, OutLines);
			}
		}
	}

	/** Everything compared about one track at one moment, normalised and sorted. */
	struct FSnapshot
	{
		TArray<FString> Facts;
		TArray<FString> Names;
		TArray<FString> Inputs;
		FString Export;
		bool bValid = false;
		bool bExported = false;
		FString ExportProblem;
	};

	/** @param bWithExport  also the decompiler's text, which costs more than all the rest together. */
	static FSnapshot Capture(const FTrack& Track, UNiagaraSystem* System, bool bWithExport = false)
	{
		FSnapshot Snapshot;
		if (System == nullptr)
		{
			return Snapshot;
		}
		Snapshot.bValid = true;

		UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR capture %s: facts"), *Track.Label);
		DescribeSystemFacts(System, Snapshot.Facts);
		for (FString& Fact : Snapshot.Facts)
		{
			Fact = Track.Normalise(Fact);
		}
		Snapshot.Facts.Sort();

		UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR capture %s: names"), *Track.Label);
		DescribeNames(System, Snapshot.Names);
		for (FString& Name : Snapshot.Names)
		{
			Name = Track.Normalise(Name);
		}
		Snapshot.Names.Sort();

		UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR capture %s: inputs"), *Track.Label);
		DescribeAllInputs(System, Snapshot.Inputs);
		for (FString& Line : Snapshot.Inputs)
		{
			Line = Track.Normalise(Line);
		}
		Snapshot.Inputs.Sort();

		if (bWithExport)
		{
			UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR capture %s: export"), *Track.Label);
			Snapshot.bExported = true;
			FDiagnosticSink Diagnostics;
			const FDecompileResult Export = FDecompiler::Decompile(System, TEXT("Game"), Diagnostics);
			if (Export.bSucceeded)
			{
				Snapshot.Export = Track.Normalise(Export.Source);
			}
			else
			{
				Snapshot.ExportProblem = Diagnostics.FormatAll();
			}
		}
		return Snapshot;
	}

	/** The members of two sorted lists that the other lacks, as a multiset difference. */
	static void DiffSorted(const TArray<FString>& A, const TArray<FString>& B,
		TArray<FString>& OutOnlyA, TArray<FString>& OutOnlyB)
	{
		int32 IndexA = 0;
		int32 IndexB = 0;
		while (IndexA < A.Num() || IndexB < B.Num())
		{
			if (IndexB >= B.Num() || (IndexA < A.Num() && A[IndexA] < B[IndexB]))
			{
				OutOnlyA.Add(A[IndexA++]);
			}
			else if (IndexA >= A.Num() || B[IndexB] < A[IndexA])
			{
				OutOnlyB.Add(B[IndexB++]);
			}
			else
			{
				++IndexA;
				++IndexB;
			}
		}
	}

	static void AppendDiff(const TCHAR* Channel, const TArray<FString>& A, const TArray<FString>& B, FString& OutReport)
	{
		TArray<FString> OnlyA, OnlyB;
		DiffSorted(A, B, OnlyA, OnlyB);
		if (OnlyA.IsEmpty() && OnlyB.IsEmpty())
		{
			return;
		}
		constexpr int32 MaxLines = 40;
		OutReport += FString::Printf(TEXT("\n  %s: %d only in the incremental track (A), %d only in the full rebuild (B)"),
			Channel, OnlyA.Num(), OnlyB.Num());
		for (int32 Index = 0; Index < OnlyA.Num() && Index < MaxLines; ++Index)
		{
			OutReport += TEXT("\n    A> ") + OnlyA[Index];
		}
		for (int32 Index = 0; Index < OnlyB.Num() && Index < MaxLines; ++Index)
		{
			OutReport += TEXT("\n    B> ") + OnlyB[Index];
		}
	}

	static FString DiffSnapshots(const FSnapshot& A, const FSnapshot& B)
	{
		FString Report;
		if (A.bValid != B.bValid)
		{
			return TEXT("\n  one track has no asset");
		}
		AppendDiff(TEXT("facts"), A.Facts, B.Facts, Report);
		AppendDiff(TEXT("names"), A.Names, B.Names, Report);
		AppendDiff(TEXT("inputs"), A.Inputs, B.Inputs, Report);
		if (!A.bExported || !B.bExported)
		{
			return Report;
		}
		if (A.ExportProblem.IsEmpty() != B.ExportProblem.IsEmpty())
		{
			Report += TEXT("\n  export: only one track exports");
		}
		else if (A.Export != B.Export)
		{
			TArray<FString> LinesA, LinesB;
			A.Export.ParseIntoArrayLines(LinesA, false);
			B.Export.ParseIntoArrayLines(LinesB, false);
			LinesA.Sort();
			LinesB.Sort();
			FString Lines;
			AppendDiff(TEXT("export lines"), LinesA, LinesB, Lines);
			Report += Lines.IsEmpty() ? FString(TEXT("\n  export: same lines in a different order")) : Lines;
		}
		return Report;
	}

	// -----------------------------------------------------------------------------------------
	// The runner
	// -----------------------------------------------------------------------------------------

	static bool ApplyEdits(FString& Text, const TArray<FEdit>& Edits, FString& OutProblem)
	{
		for (const FEdit& Edit : Edits)
		{
			const FString& Find = Edit.Find;
			int32 Count = 0;
			for (int32 From = Text.Find(Find, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
				From != INDEX_NONE;
				From = Text.Find(Find, ESearchCase::CaseSensitive, ESearchDir::FromStart, From + Find.Len()))
			{
				++Count;
			}
			if (Count != 1)
			{
				OutProblem = FString::Printf(TEXT("the anchor '%s' occurs %d times, not once"), *Find, Count);
				return false;
			}
			Text = Text.Replace(*Find, *Edit.Replace, ESearchCase::CaseSensitive);
		}
		return true;
	}

	static FString JoinCodes(const TArray<FString>& Codes)
	{
		return Codes.IsEmpty() ? FString(TEXT("none")) : FString::Join(Codes, TEXT(","));
	}

	/** Where `-DreamFXIncrementalDump=<dir>` writes each step's facts, names and export for both tracks. */
	static FString DumpDirectory()
	{
		FString Directory;
		FParse::Value(FCommandLine::Get(), TEXT("DreamFXIncrementalDump="), Directory);
		return Directory;
	}

	static void Dump(const FString& Directory, const FString& Stem, const TCHAR* Label, const FSnapshot& Snapshot)
	{
		if (Directory.IsEmpty() || !Snapshot.bValid)
		{
			return;
		}
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Base = Directory / (Stem + TEXT(".") + Label);
		FFileHelper::SaveStringToFile(FString::Join(Snapshot.Facts, TEXT("\n")), *(Base + TEXT(".facts")));
		FFileHelper::SaveStringToFile(FString::Join(Snapshot.Names, TEXT("\n")), *(Base + TEXT(".names")));
		FFileHelper::SaveStringToFile(FString::Join(Snapshot.Inputs, TEXT("\n")), *(Base + TEXT(".inputs")));
		if (Snapshot.bExported)
		{
			FFileHelper::SaveStringToFile(Snapshot.Export, *(Base + TEXT(".dfs")));
		}
	}

	class FRunner
	{
	public:
		FRunner(const FFixture& InFixture, FSummary& InSummary)
			: Fixture(InFixture)
			, Summary(InSummary)
			, Unique(FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, A(TEXT("A"), InFixture.Name, Unique)
			, B(TEXT("B"), InFixture.Name, Unique)
			, DumpDir(DumpDirectory())
		{
		}

		void Run()
		{
			// Raw string literals keep whatever line ending the source file has; the mutations'
			// multi-line anchors assume one.
			FString Text = FString(Fixture.Source).Replace(TEXT("\r\n"), TEXT("\n"));

			FGenerateOptions Initial;
			Initial.bSave = true;
			Initial.bForce = true;
			if (!BuildBoth(Text, Initial, Initial, TEXT("initial build"), nullptr))
			{
				return;
			}

			int32 StepIndex = 0;
			for (const FMutation& Mutation : Fixture.Mutations)
			{
				++StepIndex;
				FString Next = Text;
				FString Problem;
				if (!ApplyEdits(Next, Mutation.Edits, Problem))
				{
					Summary.EquivalenceProblems.Add(FString::Printf(TEXT("%s step %d '%s' does not apply to the text: %s"),
						Fixture.Name, StepIndex, Mutation.Name, *Problem));
					continue;
				}

				if ((Mutation.Flags & ReloadFirst) && !FParse::Param(FCommandLine::Get(), TEXT("DreamFXIncrementalNoReload")))
				{
					DropFromMemory();
				}

				FGenerateOptions Incremental;
				Incremental.bSave = true;
				Incremental.bIncremental = true;
				Incremental.bForce = (Mutation.Flags & ForceIncremental) != 0;
				Incremental.bForceLossyRebuild = (Mutation.Flags & AllowLoss) != 0;

				FGenerateOptions Full;
				Full.bSave = true;
				Full.bForce = true;
				Full.bIncremental = false;
				Full.bForceLossyRebuild = (Mutation.Flags & AllowLoss) != 0;

				FStepRecord Record;
				Record.Fixture = Fixture.Name;
				Record.Mutation = Mutation.Name;
				Record.Scope = Mutation.Scope;
				const FString Label = FString::Printf(TEXT("%s step %d '%s'"), Fixture.Name, StepIndex, Mutation.Name);

				bDiverged = false;
				const bool bAccepted = BuildBoth(Next, Incremental, Full, Label, &Record, Mutation.Scope);
				Summary.Steps.Add(Record);
				if (bAccepted)
				{
					Text = Next;
				}
				if (bDiverged)
				{
					Rebaseline(Text, Label);
				}
			}

			CompareExports();
		}

	private:
		const FFixture& Fixture;
		FSummary& Summary;
		FString Unique;
		FTrack A;
		FTrack B;
		FString DumpDir;
		int32 DumpIndex = 0;
		/** Set by BuildBoth when the two tracks ended a step unequal. */
		bool bDiverged = false;

		FString WithName(const FString& Text, const FTrack& Track) const
		{
			return Text.Replace(TEXT("%ASSET%"), *Track.ShortName);
		}

		/**
		 * The decompiler's text of both tracks, once the whole history has been applied. It reads the
		 * same things the input channel does, through a reader that also probes every module for its
		 * defaults, which is why it is not run after every step. What it adds is a second implementation
		 * of "what does this asset say", so a difference the input channel is blind to still shows.
		 */
		void CompareExports()
		{
			if (A.Find() == nullptr || B.Find() == nullptr)
			{
				return;
			}
			const FSnapshot SnapshotA = Capture(A, A.Find(), /*bWithExport=*/true);
			const FSnapshot SnapshotB = Capture(B, B.Find(), /*bWithExport=*/true);
			Dump(DumpDir, FString::Printf(TEXT("%s.final"), Fixture.Name), TEXT("A"), SnapshotA);
			Dump(DumpDir, FString::Printf(TEXT("%s.final"), Fixture.Name), TEXT("B"), SnapshotB);
			const FString Difference = DiffSnapshots(SnapshotA, SnapshotB);
			if (!Difference.IsEmpty())
			{
				Summary.EquivalenceProblems.Add(FString::Printf(
					TEXT("%s, after the whole history: the incremental track differs from the full rebuild.%s"),
					Fixture.Name, *Difference));
			}
		}

		/**
		 * Starts both tracks over, from new assets built in full from the accepted text.
		 *
		 * A difference that one step introduces does not go away by itself: the full rebuild can keep
		 * what the incremental one reset, or the other way round, and every later step would then
		 * report the same line again and bury whatever it adds. Reporting each difference once, at the
		 * step that made it, needs the tracks to agree again before the next one.
		 */
		void Rebaseline(const FString& Text, const FString& Label)
		{
			Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			A.Renew(Unique);
			B.Renew(Unique);
			FGenerateOptions Initial;
			Initial.bSave = true;
			Initial.bForce = true;
			UE_LOG(LogDreamFXIncremental, Display, TEXT("DFXINCR %s: the tracks differ; both are built again from new assets"), *Label);
			BuildBoth(Text, Initial, Initial, Label + TEXT(" (rebuilt in full after a difference)"), nullptr);
			bDiverged = false;
		}

		/**
		 * Unloads both packages, so the next build meets them the way a command line build does. A
		 * garbage collection alone would not: an asset is standalone, and stays.
		 */
		void DropFromMemory()
		{
			TArray<UPackage*> Packages;
			for (const FTrack* Track : { &A, &B })
			{
				if (UNiagaraSystem* System = Track->Find())
				{
					Packages.Add(System->GetOutermost());
				}
			}
			A.Held.Reset();
			B.Held.Reset();
			FText Error;
			UPackageTools::UnloadPackages(Packages, Error);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			if (A.Find() != nullptr || B.Find() != nullptr)
			{
				UE_LOG(LogDreamFXIncremental, Display,
					TEXT("DFXINCR %s: the assets stayed in memory after being unloaded (%s), so this step reads them from there"),
					Fixture.Name, *Error.ToString());
			}
		}

		/**
		 * Builds both tracks from one text and judges the pair.
		 *
		 * @return whether the text was accepted: both built it, so it becomes the history's new state.
		 */
		bool BuildBoth(const FString& Text, const FGenerateOptions& OptionsA, const FGenerateOptions& OptionsB,
			const FString& Label, FStepRecord* Record, EScope Scope = EScope::Values)
		{
			// What a rejected build must leave untouched, read before it can touch anything.
			FSnapshot BeforeA, BeforeB;
			if (Scope == EScope::Rejected)
			{
				BeforeA = Capture(A, A.Find());
				BeforeB = Capture(B, B.Find());
			}

			UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR %s: building A"), *Label);
			FOutcome OutcomeA = Build(WithName(Text, A), A, OptionsA);
			UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR %s: building B"), *Label);
			FOutcome OutcomeB = Build(WithName(Text, B), B, OptionsB);
			UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR %s: both built"), *Label);

			// The safety gate refused one track and not the other. That is a finding -- the two builds
			// were asked the same question and answered it differently -- and it is also where the two
			// assets stop agreeing about what the text says. So it is recorded, the refused track is
			// built again with the override so that both hold the text, and the comparison goes on.
			if (Scope != EScope::Rejected && Scope != EScope::Flagged && OutcomeA.bParsed && OutcomeB.bParsed
				&& OutcomeA.Result.bSucceeded != OutcomeB.Result.bSucceeded)
			{
				const bool bRefusedA = !OutcomeA.Result.bSucceeded;
				FOutcome& Refused = bRefusedA ? OutcomeA : OutcomeB;
				if (Refused.ErrorCodes.Num() == 1 && Refused.ErrorCodes[0] == TEXT("DFX8018"))
				{
					bDiverged = true;
					Summary.EquivalenceProblems.Add(FString::Printf(
						TEXT("%s: the build safety gate refused the %s track and accepted the %s one.\n%s track refused: %s"),
						*Label, bRefusedA ? TEXT("incremental") : TEXT("full"), bRefusedA ? TEXT("full") : TEXT("incremental"),
						bRefusedA ? TEXT("Incremental") : TEXT("Full"), *Refused.ErrorText));
					FGenerateOptions Forced = bRefusedA ? OptionsA : OptionsB;
					Forced.bForceLossyRebuild = true;
					UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR %s: building the refused track again with the override"), *Label);
					Refused = Build(WithName(Text, bRefusedA ? A : B), bRefusedA ? A : B, Forced);
				}
			}

			if (Record != nullptr)
			{
				Record->bRequested = OutcomeA.Result.Incremental.bRequested;
				Record->bApplied = OutcomeA.Result.Incremental.bApplied;
				Record->FallbackReason = OutcomeA.Result.Incremental.FallbackReason;
				Record->Report = OutcomeA.Result.Incremental.ToString();
				Record->SecondsIncremental = OutcomeA.Seconds;
				Record->SecondsFull = OutcomeB.Seconds;
			}
			Summary.SecondsIncremental += OutcomeA.Seconds;
			Summary.SecondsFull += OutcomeB.Seconds;
			UE_LOG(LogDreamFXIncremental, Display,
				TEXT("DFXINCR %s | A (incremental request): %s, %.2fs | B (full): %.2fs"), *Label,
				*OutcomeA.Result.Incremental.ToString(), OutcomeA.Seconds, OutcomeB.Seconds);

			if (!OutcomeA.bParsed || !OutcomeB.bParsed)
			{
				Summary.EquivalenceProblems.Add(FString::Printf(TEXT("%s: the mutated text does not parse:\n%s"),
					*Label, *(OutcomeA.bParsed ? OutcomeB : OutcomeA).Diagnostics));
				return false;
			}

			if (OutcomeA.ErrorCodes != OutcomeB.ErrorCodes
				|| OutcomeA.Result.bSucceeded != OutcomeB.Result.bSucceeded)
			{
				Summary.EquivalenceProblems.Add(FString::Printf(
					TEXT("%s: the tracks disagree about the build. A %s (errors: %s), B %s (errors: %s)\nA:\n%s\nB:\n%s"),
					*Label,
					OutcomeA.Result.bSucceeded ? TEXT("succeeded") : TEXT("failed"), *JoinCodes(OutcomeA.ErrorCodes),
					OutcomeB.Result.bSucceeded ? TEXT("succeeded") : TEXT("failed"), *JoinCodes(OutcomeB.ErrorCodes),
					*OutcomeA.ErrorText, *OutcomeB.ErrorText));
				return false;
			}

			if (OutcomeA.GateWarnings != OutcomeB.GateWarnings)
			{
				bDiverged = true;
				Summary.EquivalenceProblems.Add(FString::Printf(
					TEXT("%s: the build safety gate judged the two rebuilds differently.\n A: %s\n B: %s"), *Label,
					*FString::Join(OutcomeA.GateWarnings, TEXT(" | ")), *FString::Join(OutcomeB.GateWarnings, TEXT(" | "))));
			}

			// Warnings are compared for the log only: an incremental build is free to say less, or say it
			// differently, about things that are not errors -- but a difference is worth seeing.
			if (OutcomeA.WarningCodes != OutcomeB.WarningCodes)
			{
				UE_LOG(LogDreamFXIncremental, Warning, TEXT("DFXINCR %s: the tracks report different warnings. A: %s | B: %s"),
					*Label, *JoinCodes(OutcomeA.WarningCodes), *JoinCodes(OutcomeB.WarningCodes));
			}

			const bool bBuilt = OutcomeA.Result.bSucceeded;
			if (Scope == EScope::Rejected)
			{
				if (bBuilt)
				{
					Summary.EquivalenceProblems.Add(FString::Printf(TEXT("%s: the text should have been rejected and built"), *Label));
					return true;
				}
				// A failed build has to leave the asset as it was -- including the baseline an incremental
				// build would trust next.
				const FString DriftA = DiffSnapshots(BeforeA, Capture(A, A.Find()));
				const FString DriftB = DiffSnapshots(BeforeB, Capture(B, B.Find()));
				if (!DriftA.IsEmpty() || !DriftB.IsEmpty())
				{
					Summary.EquivalenceProblems.Add(FString::Printf(
						TEXT("%s: a rejected build changed the asset.%s%s"), *Label,
						DriftA.IsEmpty() ? TEXT("") : *(FString(TEXT("\n A:")) + DriftA),
						DriftB.IsEmpty() ? TEXT("") : *(FString(TEXT("\n B:")) + DriftB)));
				}
				return false;
			}
			if (Scope == EScope::Flagged ? bBuilt : !bBuilt)
			{
				Summary.EquivalenceProblems.Add(FString::Printf(
					TEXT("%s: the build %s in both tracks, which the mutation table does not expect:\n%s"), *Label,
					bBuilt ? TEXT("succeeded") : TEXT("failed"), *OutcomeA.Diagnostics));
				return false;
			}

			A.Held.Reset(A.Find());
			B.Held.Reset(B.Find());

			// The compiled state has to be current in both: the build says so itself, but it is the
			// claim an incremental build skips work on, so it is asked again here from the outside.
			for (const FTrack* Track : { &A, &B })
			{
				TArray<FString> Stale;
				if (!FNiagaraAdapter::VerifyCompiledStateCurrent(Track->Held.Get(), Stale))
				{
					bDiverged = true;
					Summary.EquivalenceProblems.Add(FString::Printf(TEXT("%s: track %s finished with stale scripts: %s"),
						*Label, *Track->Label, *FString::Join(Stale, TEXT(", "))));
				}
			}

			UE_LOG(LogDreamFXIncremental, Verbose, TEXT("DFXINCR %s: reading A and B"), *Label);
			const FSnapshot SnapshotA = Capture(A, A.Held.Get());
			const FSnapshot SnapshotB = Capture(B, B.Held.Get());
			const FString Stem = FString::Printf(TEXT("%s.%03d"), Fixture.Name, DumpIndex++);
			Dump(DumpDir, Stem, TEXT("A"), SnapshotA);
			Dump(DumpDir, Stem, TEXT("B"), SnapshotB);

			const FString Difference = DiffSnapshots(SnapshotA, SnapshotB);
			if (!Difference.IsEmpty())
			{
				bDiverged = true;
				Summary.EquivalenceProblems.Add(FString::Printf(
					TEXT("%s: the incremental track differs from the full rebuild.%s"), *Label, *Difference));
			}
			return Scope != EScope::Flagged;
		}
	};

	// -----------------------------------------------------------------------------------------
	// Fixtures
	// -----------------------------------------------------------------------------------------

	static TArray<FFixture> MakeFixtures();

	static FSummary& RunAll()
	{
		static FSummary Summary;
		static bool bRan = false;
		if (!bRan)
		{
			bRan = true;
			// For narrowing a failure down without paying for the whole history. A mutation that
			// depended on one dropped here no longer applies, and says so.
			FString OnlyFixture, OnlySteps;
			FParse::Value(FCommandLine::Get(), TEXT("DreamFXIncrementalFixture="), OnlyFixture);
			FParse::Value(FCommandLine::Get(), TEXT("DreamFXIncrementalSteps="), OnlySteps);
			TArray<FString> StepWords;
			OnlySteps.ParseIntoArray(StepWords, TEXT("|"));

			TArray<FFixture> Fixtures = MakeFixtures();
			for (FFixture& Fixture : Fixtures)
			{
				if (!OnlyFixture.IsEmpty() && OnlyFixture != Fixture.Name)
				{
					continue;
				}
				if (!StepWords.IsEmpty())
				{
					Fixture.Mutations.RemoveAll([&StepWords](const FMutation& Mutation)
					{
						return !StepWords.ContainsByPredicate([&Mutation](const FString& Word)
						{
							return FString(Mutation.Name).Contains(Word.Replace(TEXT("+"), TEXT(" ")));
						});
					});
				}
				FRunner Runner(Fixture, Summary);
				Runner.Run();
			}
			UE_LOG(LogDreamFXIncremental, Display, TEXT("DFXINCR total build time: incremental requests %.1fs, full rebuilds %.1fs, %d steps"),
				Summary.SecondsIncremental, Summary.SecondsFull, Summary.Steps.Num());
		}
		return Summary;
	}

}

namespace UE::DreamFX::Editor::IncrementalEquivalence
{
	// -----------------------------------------------------------------------------------------
	// Sparks: a CPU system of several emitters.
	//
	// It carries the module and value variety: static switches, a plain bool that gates other inputs,
	// dynamic input chains, a curve data interface, Set Parameters folding, linked inputs, an hlsl
	// expression, user parameters, renderers with properties and `Bind`s, system-scope stacks, and the
	// same module asset (GravityForce) in two main stacks of one emitter.
	//
	// Every anchor in the mutations below is a line that is unique in the text as it stands at that
	// step -- which is why the two emitters spell the same module differently.
	// -----------------------------------------------------------------------------------------

	static const TCHAR* SparksSource = TEXT(R"DFS(
System(Name="DreamFXAutomation/%ASSET%", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-200, -200, -50, 200, 200, 300);
    }

    Properties = {
        int    SparkCount = 12                     [ Group="Burst"; SortPriority=10 ];
        float  SparkSpeed = 300.0                  [ Group="Burst"; SortPriority=20 ];
        Color  TintA      = (1.0, 0.72, 0.25, 1.0) [ Group="Look" ];
        Color  TintB      = (1.0, 0.2, 0.05, 1.0)  [ Group="Look" ];
        Vector HitNormal  = (0, 0, 1)              [ Group="Hit" ];
        float  Wobble     = 0.3;
    }

    SystemUpdate = {
        SystemState(InactiveResponse = Complete, LoopBehavior = Infinite);
        float System.Moon.Gain = 1.0;
    }

    Emitter Sparks
    {
        Settings = {
            SimTarget          = CPU;
            LocalSpace         = false;
            Determinism        = true;
            RandomSeed         = 1337;
            AllocationMode     = Fixed;
            PreAllocationCount = 64;
        }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Once, InactiveResponse = Complete, LoopDuration = 0.15);
            SpawnBurst_Instantaneous(SpawnCount = User.SparkCount, SpawnTime = 0.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(
                LifetimeMode         = Random,
                LifetimeMin          = 0.25,
                LifetimeMax          = 0.55,
                ColorMode            = DirectSet,
                Color                = User.TintA,
                SpriteSizeMode       = RandomUniform,
                UniformSpriteSizeMin = 3.0,
                UniformSpriteSizeMax = 9.0
            );
            SystemLocation();
            AddVelocityInCone(
                ConeAxis                = User.HitNormal,
                ConeAxisCoordinateSpace = Simulation,
                ConeAngle               = 55.0,
                VelocityStrength        = Multiply_Float(A = RandomRangeFloat(Minimum = User.SparkSpeed * 0.6, Maximum = User.SparkSpeed), B = Add_Float(A = 0.5, B = 0.5))
            );
            GravityForce(Gravity = (0, 0, -120));
            Particles.Moon.SparkSeed = 0.5;
            Particles.Moon.SparkTint = User.TintA;
            ApplyInitialForces();
        }

        ParticleUpdate = {
            ParticleState();
            GravityForce(Gravity = (0, 0, -680));
            Drag(Drag = 1.4);
            SolveForcesAndVelocity();
            ScaleSpriteSize(
                ScaleSpriteSizeMode = Uniform,
                UniformScaleFactor  = FloatFromCurve(
                    FloatCurve = curve {
                        0.0 -> 1.0;
                        0.7 -> 0.85 [ Interp=Cubic; Arrive=-0.4; Leave=-1.2 ];
                        1.0 -> 0.0  [ Interp=Linear ];
                    },
                    CurveIndex = Particles.NormalizedAge
                )
            );
            Color Particles.Color = hlsl { float4(Particles.Color.rgb, saturate(1.0 - Particles.NormalizedAge)) };
        }

        SpriteRenderer Core
        {
            Alignment  = Unaligned;
            FacingMode = FaceCamera;
            SortMode   = ViewDepth;

            Bind SpriteSize -> Particles.SpriteSize;
            Bind Color      -> Particles.Color;
        }
    }

    Emitter Glow
    {
        Settings = { SimTarget = CPU; Determinism = true; RandomSeed = 7; AllocationMode = Fixed; PreAllocationCount = 48; }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = 20.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(LifetimeMode = DirectSet, Lifetime = 1.5, SpriteSizeMode = Uniform, UniformSpriteSize = 24.0);
            SystemLocation(Offset = (0, 0, 5));
        }

        ParticleUpdate = {
            ParticleState(KillParticlesWhenLifetimeHasElapsed = true);
            GravityForce(Gravity = (0, 0, -50));
            SolveForcesAndVelocity(ClampVelocity = true, SpeedLimit = 400.0);
        }

        SpriteRenderer Halo
        {
            SortMode = ViewDepth;
        }
    }

    // end of emitters
}
)DFS");

	/** An emitter to add, move and remove. Its module calls differ from Sparks' so no anchor goes ambiguous. */
	static const TCHAR* EmbersBlock = TEXT(R"DFS(    Emitter Embers
    {
        Settings = { SimTarget = CPU; Determinism = true; RandomSeed = 3; AllocationMode = Fixed; PreAllocationCount = 32; }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = 6.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(LifetimeMode = DirectSet, Lifetime = 2.0, SpriteSizeMode = Uniform, UniformSpriteSize = 6.0);
        }

        ParticleUpdate = {
            ParticleState(KillParticlesWhenLifetimeHasElapsed = true);
            GravityForce(Gravity = (0, 0, 30));
            SolveForcesAndVelocity(LimitAcceleration = false);
        }

        SpriteRenderer Spark
        {
        }
    }

)DFS");

	static const TCHAR* TrailBlock = TEXT(R"DFS(SpriteRenderer Trail
        {
            SortMode = None;
        }

        )DFS");

	static FFixture MakeSparks()
	{
		const FString EmbersFirst = FString(EmbersBlock) + TEXT("    Emitter Glow");
		const FString EmbersLast = FString(EmbersBlock) + TEXT("    // end of emitters");
		const FString TrailFirst = FString(TrailBlock) + TEXT("SpriteRenderer Halo");

		FFixture Fixture;
		Fixture.Name = TEXT("Sparks");
		Fixture.Source = SparksSource;
		Fixture.Mutations = {
			// ---- values -----------------------------------------------------------------------
			{ TEXT("float literal"), EScope::Values,
				{ { TEXT("Drag(Drag = 1.4);"), TEXT("Drag(Drag = 2.2);") } } },
			{ TEXT("enum input"), EScope::Values,
				{ { TEXT("ConeAxisCoordinateSpace = Simulation,"), TEXT("ConeAxisCoordinateSpace = World,") } } },
			{ TEXT("link target, module input"), EScope::Values,
				{ { TEXT("Color                = User.TintA,"), TEXT("Color                = User.TintB,") } } },
			{ TEXT("link target, Set Parameters entry"), EScope::Values,
				{ { TEXT("Particles.Moon.SparkTint = User.TintA;"), TEXT("Particles.Moon.SparkTint = User.TintB;") } } },
			{ TEXT("link target, curve index to a user parameter"), EScope::Values,
				{ { TEXT("CurveIndex = Particles.NormalizedAge"), TEXT("CurveIndex = User.Wobble") } } },
			{ TEXT("hlsl expression"), EScope::Values,
				{ { TEXT("saturate(1.0 - Particles.NormalizedAge)"), TEXT("saturate(0.5 - Particles.NormalizedAge)") } } },
			{ TEXT("argument added"), EScope::Values,
				{ { TEXT("SystemLocation();"), TEXT("SystemLocation(Offset = (0, 0, 10));") } } },
			{ TEXT("argument removed"), EScope::Values,
				{ { TEXT("ConeAngle               = 55.0,"), TEXT("") } } },

			// ---- dynamic input chains ---------------------------------------------------------
			{ TEXT("value inside a dynamic input chain"), EScope::Values,
				{ { TEXT("B = Add_Float(A = 0.5, B = 0.5)"), TEXT("B = Add_Float(A = 0.5, B = 0.75)") } }, AllowLoss },
			{ TEXT("link inside a dynamic input chain"), EScope::Values,
				{ { TEXT("Minimum = User.SparkSpeed * 0.6"), TEXT("Minimum = User.SparkSpeed * 0.4") } } },
			{ TEXT("nested dynamic input replaced by another"), EScope::Stack,
				{ { TEXT("B = Add_Float(A = 0.5, B = 0.75)"), TEXT("B = Subtract_Float(A = 1.5, B = 0.25)") } } },
			{ TEXT("dynamic input argument removed entirely"), EScope::Values,
				{ { TEXT("RandomRangeFloat(Minimum = User.SparkSpeed * 0.4, Maximum = User.SparkSpeed)"),
					TEXT("RandomRangeFloat(Maximum = User.SparkSpeed)") } } },
			{ TEXT("top-level dynamic input replaced by another"), EScope::Stack,
				{ { TEXT("Multiply_Float(A = RandomRangeFloat(Maximum = User.SparkSpeed), B = Subtract_Float(A = 1.5, B = 0.25))"),
					TEXT("RandomRangeFloat(Minimum = 100.0, Maximum = User.SparkSpeed)") } }, AllowLoss },
			{ TEXT("dynamic input replaced by a literal"), EScope::Stack,
				{ { TEXT("RandomRangeFloat(Minimum = 100.0, Maximum = User.SparkSpeed)"), TEXT("250.0") } } },
			{ TEXT("literal replaced by a dynamic input chain"), EScope::Stack,
				{ { TEXT("VelocityStrength        = 250.0"),
					TEXT("VelocityStrength        = Multiply_Float(A = 250.0, B = Add_Float(A = 0.5, B = 0.5))") } }, AllowLoss },

			// ---- data interface ---------------------------------------------------------------
			{ TEXT("curve key value"), EScope::Values,
				{ { TEXT("0.7 -> 0.85 [ Interp=Cubic; Arrive=-0.4; Leave=-1.2 ];"),
					TEXT("0.7 -> 0.55 [ Interp=Cubic; Arrive=-0.4; Leave=-1.2 ];") } }, ReloadFirst },
			{ TEXT("curve key added"), EScope::Values,
				{ { TEXT("1.0 -> 0.0  [ Interp=Linear ];"),
					TEXT("0.9 -> 0.2  [ Interp=Linear ];\n                        1.0 -> 0.0  [ Interp=Linear ];") } } },

			// ---- static switches and the bool that gates --------------------------------------
			{ TEXT("static switch flipped, gated inputs dropped"), EScope::Stack,
				{ { TEXT("EmitterState(LifeCycleMode = Self, LoopBehavior = Once, InactiveResponse = Complete, LoopDuration = 0.15);"),
					TEXT("EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);") } }, AllowLoss },
			{ TEXT("static switch flipped back, gated inputs restored"), EScope::Stack,
				{ { TEXT("EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);\n            SpawnBurst"),
					TEXT("EmitterState(LifeCycleMode = Self, LoopBehavior = Once, InactiveResponse = Complete, LoopDuration = 0.15);\n            SpawnBurst") } } },
			{ TEXT("static switch written on a module that had none"), EScope::Stack,
				{ { TEXT("GravityForce(Gravity = (0, 0, -680));"), TEXT("GravityForce(CoordinateSpace = World, Gravity = (0, 0, -680));") } } },
			{ TEXT("static switch value changed"), EScope::Stack,
				{ { TEXT("CoordinateSpace = World, Gravity"), TEXT("CoordinateSpace = Local, Gravity") } } },
			{ TEXT("gating bool turned off, gated input removed"), EScope::Values,
				{ { TEXT("SolveForcesAndVelocity(ClampVelocity = true, SpeedLimit = 400.0);"), TEXT("SolveForcesAndVelocity(ClampVelocity = false);") } }, AllowLoss },
			{ TEXT("gating bool turned on, gated input added"), EScope::Values,
				{ { TEXT("SolveForcesAndVelocity(ClampVelocity = false);"), TEXT("SolveForcesAndVelocity(ClampVelocity = true, SpeedLimit = 250.0);") } } },
			{ TEXT("gated input value changed under a true bool"), EScope::Values,
				{ { TEXT("SpeedLimit = 250.0"), TEXT("SpeedLimit = 320.0") } } },
			{ TEXT("gating bool and gated input added to a module that had neither"), EScope::Values,
				{ { TEXT("SpawnRate(SpawnRate = 20.0);"), TEXT("SpawnRate(SpawnRate = 20.0, UseSpawnProbability = true, SpawnProbability = 0.5);") } } },
			{ TEXT("gating bool and gated input removed together"), EScope::Values,
				{ { TEXT("SpawnRate(SpawnRate = 20.0, UseSpawnProbability = true, SpawnProbability = 0.5);"), TEXT("SpawnRate(SpawnRate = 20.0);") } }, AllowLoss },

			// ---- stack structure --------------------------------------------------------------
			{ TEXT("module added"), EScope::Stack,
				{ { TEXT("Drag(Drag = 2.2);"), TEXT("Drag(Drag = 2.2);\n            JitterPosition(JitterAmount = 2.0);") } } },
			{ TEXT("module removed"), EScope::Stack,
				{ { TEXT("JitterPosition(JitterAmount = 2.0);"), TEXT("") } } },
			{ TEXT("two modules swapped"), EScope::Stack,
				SwapLines(TEXT("GravityForce(CoordinateSpace = Local, Gravity = (0, 0, -680));"), TEXT("Drag(Drag = 2.2);")) },
			{ TEXT("node renamed with as"), EScope::Stack,
				{ { TEXT("Drag(Drag = 2.2);"), TEXT("Drag(Drag = 2.2) as BigDrag;") } } },
			{ TEXT("node rename dropped"), EScope::Stack,
				{ { TEXT("Drag(Drag = 2.2) as BigDrag;"), TEXT("Drag(Drag = 2.2);") } } },
			{ TEXT("module disabled"), EScope::Stack,
				{ { TEXT("Drag(Drag = 2.2);"), TEXT("disabled Drag(Drag = 2.2);") } } },
			{ TEXT("module enabled again"), EScope::Stack,
				{ { TEXT("disabled Drag(Drag = 2.2);"), TEXT("Drag(Drag = 2.2);") } } },

			// ---- Set Parameters ---------------------------------------------------------------
			{ TEXT("Set Parameters value"), EScope::Values,
				{ { TEXT("Particles.Moon.SparkSeed = 0.5;"), TEXT("Particles.Moon.SparkSeed = 0.9;") } } },
			{ TEXT("Set Parameters entry added"), EScope::Stack,
				{ { TEXT("Particles.Moon.SparkTint = User.TintB;"),
					TEXT("Particles.Moon.SparkTint = User.TintB;\n            Particles.Moon.SparkPhase = 0.25;") } } },
			{ TEXT("Set Parameters entry removed"), EScope::Stack,
				{ { TEXT("Particles.Moon.SparkSeed = 0.9;"), TEXT("") } } },

			// ---- renderers --------------------------------------------------------------------
			{ TEXT("renderer property changed"), EScope::Emitter,
				{ { TEXT("SortMode   = ViewDepth;"), TEXT("SortMode   = ViewDistance;") } }, ReloadFirst },
			{ TEXT("renderer property added"), EScope::Emitter,
				{ { TEXT("FacingMode = FaceCamera;"), TEXT("FacingMode = FaceCamera;\n            SubImageSize = (2, 2);") } } },
			{ TEXT("renderer property removed"), EScope::Emitter,
				{ { TEXT("SubImageSize = (2, 2);"), TEXT("") } }, AllowLoss },
			{ TEXT("Bind retargeted"), EScope::Emitter,
				{ { TEXT("Bind Color      -> Particles.Color;"), TEXT("Bind Color      -> Particles.Moon.SparkTint;") } } },
			{ TEXT("Bind added"), EScope::Emitter,
				{ { TEXT("Bind SpriteSize -> Particles.SpriteSize;"),
					TEXT("Bind SpriteSize -> Particles.SpriteSize;\n            Bind SpriteRotation -> Particles.SpriteRotation;") } } },
			{ TEXT("Bind removed"), EScope::Emitter,
				{ { TEXT("Bind SpriteRotation -> Particles.SpriteRotation;"), TEXT("") } } },
			{ TEXT("renderer added"), EScope::Emitter,
				{ { TEXT("SpriteRenderer Halo"), TrailFirst } } },
			{ TEXT("renderer removed"), EScope::Emitter,
				{ { TrailFirst, TEXT("SpriteRenderer Halo") } } },

			// ---- emitter settings -------------------------------------------------------------
			{ TEXT("emitter setting changed"), EScope::Emitter,
				{ { TEXT("RandomSeed         = 1337;"), TEXT("RandomSeed         = 99;") } } },
			{ TEXT("emitter bool setting flipped"), EScope::Emitter,
				{ { TEXT("LocalSpace         = false;"), TEXT("LocalSpace         = true;") } } },
			{ TEXT("emitter setting removed"), EScope::Emitter,
				{ { TEXT("PreAllocationCount = 64;"), TEXT("") } } },

			// ---- user parameters --------------------------------------------------------------
			{ TEXT("user parameter default changed"), EScope::System,
				{ { TEXT("SparkSpeed = 300.0"), TEXT("SparkSpeed = 350.0") } } },
			{ TEXT("user parameter group changed"), EScope::System,
				{ { TEXT("Color  TintB      = (1.0, 0.2, 0.05, 1.0)  [ Group=\"Look\" ];"),
					TEXT("Color  TintB      = (1.0, 0.2, 0.05, 1.0)  [ Group=\"Tint\"; SortPriority=5 ];") } } },
			{ TEXT("user parameter added"), EScope::System,
				{ { TEXT("float  Wobble     = 0.3;"), TEXT("float  Wobble     = 0.3;\n        float  Spread     = 5.0;") } } },
			{ TEXT("user parameter retyped"), EScope::System,
				{ { TEXT("float  Spread     = 5.0;"), TEXT("int    Spread     = 5;") } } },
			{ TEXT("user parameter removed"), EScope::System,
				{ { TEXT("int    Spread     = 5;"), TEXT("") } } },

			// ---- system-scope stacks ------------------------------------------------------------
			{ TEXT("system stack module input changed"), EScope::Stack,
				{ { TEXT("SystemState(InactiveResponse = Complete, LoopBehavior = Infinite);"),
					TEXT("SystemState(InactiveResponse = Complete, LoopBehavior = Multiple, LoopCount = 3, LoopDuration = 2.0);") } }, AllowLoss },
			{ TEXT("system Set Parameters value"), EScope::Values,
				{ { TEXT("float System.Moon.Gain = 1.0;"), TEXT("float System.Moon.Gain = 2.0;") } } },
			{ TEXT("system spawn stack declared"), EScope::Stack,
				{ { TEXT("SystemUpdate = {"), TEXT("SystemSpawn = { float System.Moon.Seed = 3.0; }\n\n    SystemUpdate = {") } } },
			{ TEXT("system spawn stack value changed"), EScope::Values,
				{ { TEXT("float System.Moon.Seed = 3.0;"), TEXT("float System.Moon.Seed = 4.0;") } } },
			// A stack the text does not declare is left as it is: the full rebuild keeps what the
			// previous build put there, and an incremental build has to keep it too.
			{ TEXT("system spawn stack declaration dropped, the stack is kept"), EScope::Stack,
				{ { TEXT("SystemSpawn = { float System.Moon.Seed = 4.0; }\n\n    "), TEXT("") } } },
			{ TEXT("system spawn stack declared empty"), EScope::Stack,
				{ { TEXT("SystemUpdate = {"), TEXT("SystemSpawn = { }\n\n    SystemUpdate = {") } } },

			// ---- emitters ---------------------------------------------------------------------
			{ TEXT("emitter added"), EScope::Emitters,
				{ { TEXT("    Emitter Glow"), EmbersFirst } } },
			{ TEXT("emitter moved after its sibling"), EScope::Reordered,
				{ { EmbersFirst, TEXT("    Emitter Glow") }, { TEXT("    // end of emitters"), EmbersLast } } },
			// Not reloaded first: this removes an emitter an earlier build of the same in-memory system
			// added, which is what used to corrupt the heap (see FNiagaraAdapter::RemoveEmitter).
			{ TEXT("emitter removed"), EScope::Emitters,
				{ { EmbersLast, TEXT("    // end of emitters") } } },

			// ---- text that changes nothing ----------------------------------------------------
			{ TEXT("comment added"), EScope::Values,
				{ { TEXT("Properties = {"), TEXT("// a note that changes nothing\n    Properties = {") } } },
			{ TEXT("rebuild forced with the text unchanged"), EScope::Values, {}, ForceIncremental },

			// ---- builds that report errors ----------------------------------------------------
			{ TEXT("unknown input, rejected before anything is written"), EScope::Rejected,
				{ { TEXT("Drag(Drag = 2.2);"), TEXT("Drag(Dragg = 2.2);") } } },
			{ TEXT("module dependency unmet, reported after the write"), EScope::Flagged,
				{ { TEXT("SolveForcesAndVelocity();"), TEXT("") } } },
			{ TEXT("float literal after a build that reported errors"), EScope::Values,
				{ { TEXT("Drag(Drag = 2.2);"), TEXT("Drag(Drag = 2.4);") } } },
		};
		return Fixture;
	}

	// -----------------------------------------------------------------------------------------
	// Relay: emitters that talk to each other.
	//
	// An event source and a receiver with an `OnEvent` handler, a disabled module, `Defaults`, a mesh
	// renderer that comes and goes, and a system-scope spawn stack. The handler's stack is addressed
	// through its own script rather than one of the four fixed ones, which is what makes it worth a
	// fixture of its own.
	// -----------------------------------------------------------------------------------------

	static const TCHAR* RelaySource = TEXT(R"DFS(
System(Name="DreamFXAutomation/%ASSET%", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-150, -150, -150, 150, 150, 150);
    }

    Properties = {
        float PulseRate = 8.0;
        int   Echoes    = 2;
    }

    SystemSpawn = { float System.Moon.Seed = 3.0; }

    Emitter Source
    {
        Settings = {
            RequiresPersistentIDs = true;
            Determinism           = true;
            RandomSeed            = 7;
        }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.5);
            SpawnBurst_Instantaneous(SpawnCount = 4);
        }

        ParticleSpawn = {
            V2/InitializeParticle(LifetimeMode = DirectSet, Lifetime = 0.4);
        }

        ParticleUpdate = {
            SolveForcesAndVelocity();
            GenerateLocationEvent();
        }
    }

    Emitter Receiver
    {
        Settings = {
            Determinism = true;
            RandomSeed  = 11;
        }

        Defaults = {
            float Particles.Moon.Spin = 0.0;
        }

        ParticleSpawn = {
            V2/InitializeParticle(LifetimeMode = DirectSet, Lifetime = 0.3);
        }

        // on event
        OnEvent(Source = Source, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 2) = { ReceiveLocationEvent(); }

        ParticleUpdate = {
            ParticleState();
            disabled Drag(Drag = 0.5);
            SolveForcesAndVelocity(LimitAcceleration = false);
        }

        SpriteRenderer Quad
        {
            SortMode = ViewDepth;
        }
    }

    // end of emitters
}
)DFS");

	static FFixture MakeRelay()
	{
		// Back-quoted because the module's switches are named for the attribute they sample, which no
		// identifier can hold.
		const FString Inputs = TEXT("ReceiveLocationEvent(InterpolateSpawnedPositions = true, `Velocity(Vector2)` = Apply, InheritedVelocityScale = ");
		const FString HandlerEvery = TEXT("OnEvent(Source = Source, Event = \"LocationEvent\", Mode = EveryParticle) = { ") + Inputs + TEXT("0.5); }");
		const FString HandlerBack = TEXT("OnEvent(Source = Source, Event = \"LocationEvent\", Mode = SpawnedParticles, SpawnNumber = 2) = { ReceiveLocationEvent(); }");
		const FString MeshFirst = TEXT("MeshRenderer Body\n        {\n            Meshes = [\"/Engine/BasicShapes/Cube\"];\n        }\n\n        SpriteRenderer Quad");

		FFixture Fixture;
		Fixture.Name = TEXT("Relay");
		Fixture.Source = RelaySource;
		Fixture.Mutations = {
			{ TEXT("event handler spawn count"), EScope::Emitter,
				{ { TEXT("SpawnNumber = 2)"), TEXT("SpawnNumber = 3)") } } },
			{ TEXT("event handler body gains inputs"), EScope::Stack,
				{ { TEXT("{ ReceiveLocationEvent(); }"), TEXT("{ ") + Inputs + TEXT("0.75); }") } } },
			{ TEXT("event handler body input value"), EScope::Values,
				{ { TEXT("InheritedVelocityScale = 0.75"), TEXT("InheritedVelocityScale = 0.5") } } },
			{ TEXT("event handler mode"), EScope::Emitter,
				{ { TEXT("Mode = SpawnedParticles, SpawnNumber = 3"), TEXT("Mode = EveryParticle") } } },
			{ TEXT("event handler removed"), EScope::Emitter,
				{ { HandlerEvery, TEXT("") } }, ReloadFirst },
			{ TEXT("event handler added back"), EScope::Emitter,
				{ { TEXT("// on event"), TEXT("// on event\n        ") + HandlerBack } } },
			{ TEXT("disabled module enabled"), EScope::Stack,
				{ { TEXT("disabled Drag(Drag = 0.5);"), TEXT("Drag(Drag = 0.5);") } } },
			{ TEXT("Defaults value changed"), EScope::Emitter,
				{ { TEXT("float Particles.Moon.Spin = 0.0;"), TEXT("float Particles.Moon.Spin = 1.5;") } } },
			{ TEXT("Defaults entry added"), EScope::Emitter,
				{ { TEXT("float Particles.Moon.Spin = 1.5;"), TEXT("float Particles.Moon.Spin = 1.5;\n            float Particles.Moon.Drift = 2.0;") } } },
			{ TEXT("Defaults entry removed"), EScope::Emitter,
				{ { TEXT("float Particles.Moon.Drift = 2.0;"), TEXT("") } } },
			{ TEXT("mesh renderer added"), EScope::Emitter,
				{ { TEXT("SpriteRenderer Quad"), MeshFirst } } },
			{ TEXT("mesh renderer asset changed"), EScope::Emitter,
				{ { TEXT("/Engine/BasicShapes/Cube"), TEXT("/Engine/BasicShapes/Sphere") } } },
			{ TEXT("mesh renderer removed"), EScope::Emitter,
				{ { MeshFirst.Replace(TEXT("Cube"), TEXT("Sphere")), TEXT("SpriteRenderer Quad") } } },
			{ TEXT("event source emitter setting"), EScope::Emitter,
				{ { TEXT("RandomSeed            = 7;"), TEXT("RandomSeed            = 9;") } } },
			{ TEXT("user parameter default changed"), EScope::System,
				{ { TEXT("PulseRate = 8.0"), TEXT("PulseRate = 9.5") } } },
			{ TEXT("event source emitter renamed"), EScope::Emitters,
				{ { TEXT("Emitter Source"), TEXT("Emitter Origin") }, { TEXT("Source = Source,"), TEXT("Source = Origin,") } } },
			{ TEXT("system spawn stack emptied"), EScope::Stack,
				{ { TEXT("SystemSpawn = { float System.Moon.Seed = 3.0; }"), TEXT("SystemSpawn = { }") } } },
		};
		return Fixture;
	}

	// -----------------------------------------------------------------------------------------
	// Gpu: a GPU emitter with simulation stages.
	//
	// Nothing compiles to a shader headlessly, so what this checks is the graph and the VM side: the
	// stage stacks, their bindings and the settings a GPU emitter needs. Each stage is on one line so
	// stages can be swapped, added and removed with single-line anchors.
	// -----------------------------------------------------------------------------------------

	static const TCHAR* GpuSource = TEXT(R"DFS(
System(Name="DreamFXAutomation/%ASSET%", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-100, -100, -100, 100, 100, 100);
    }

    Properties = {
        float Rate = 50.0;
    }

    Emitter Motes
    {
        Settings = {
            SimTarget          = GPU;
            LocalSpace         = false;
            Determinism        = true;
            RandomSeed         = 9;
            AllocationMode     = Fixed;
            PreAllocationCount = 128;
            FixedBounds        = box(-100, -100, -100, 100, 100, 100);
        }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = User.Rate);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(LifetimeMode = DirectSet, Lifetime = 1.0, SpriteSizeMode = Uniform, UniformSpriteSize = 4.0);
            SystemLocation();
        }

        ParticleUpdate = {
            ParticleState();
            GravityForce(Gravity = (0, 0, -200));
            SolveForcesAndVelocity();
        }

        Stage Settle = { ScaleColor(); }

        Stage Sharpen(NumIterations = 4, Enabled = false) = { ScaleColor(); Particles.Moon.StageMark = 1.0; }

        SpriteRenderer Core
        {
            Alignment  = Unaligned;
            FacingMode = FaceCamera;
            SortMode   = None;
        }
    }
}
)DFS");

	static FFixture MakeGpu()
	{
		const FString Polish = TEXT("Stage Polish(NumIterations = 2) = { ScaleColor(); }\n\n        SpriteRenderer Core");

		FFixture Fixture;
		Fixture.Name = TEXT("Gpu");
		Fixture.Source = GpuSource;
		Fixture.Mutations = {
			{ TEXT("stage iteration count"), EScope::Emitter,
				{ { TEXT("NumIterations = 4"), TEXT("NumIterations = 6") } } },
			{ TEXT("stage enabled flag dropped"), EScope::Emitter,
				{ { TEXT("Stage Sharpen(NumIterations = 6, Enabled = false)"), TEXT("Stage Sharpen(NumIterations = 6)") } }, AllowLoss },
			{ TEXT("stage Set Parameters value"), EScope::Values,
				{ { TEXT("Particles.Moon.StageMark = 1.0;"), TEXT("Particles.Moon.StageMark = 2.0;") } } },
			{ TEXT("stage module added"), EScope::Stack,
				{ { TEXT("Stage Settle = { ScaleColor(); }"), TEXT("Stage Settle = { ScaleColor(); Particles.Moon.SettleMark = 0.5; }") } } },
			{ TEXT("stage module removed"), EScope::Stack,
				{ { TEXT("ScaleColor(); Particles.Moon.SettleMark = 0.5;"), TEXT("ScaleColor();") } } },
			{ TEXT("stages swapped"), EScope::Emitter,
				SwapLines(TEXT("Stage Settle = { ScaleColor(); }"), TEXT("Stage Sharpen(NumIterations = 6) = { ScaleColor(); Particles.Moon.StageMark = 2.0; }")) },
			{ TEXT("stage added"), EScope::Emitter,
				{ { TEXT("SpriteRenderer Core"), Polish } }, ReloadFirst | AllowLoss },
			{ TEXT("stage removed"), EScope::Emitter,
				{ { Polish, TEXT("SpriteRenderer Core") } }, AllowLoss },
			{ TEXT("emitter setting changed"), EScope::Emitter,
				{ { TEXT("PreAllocationCount = 128;"), TEXT("PreAllocationCount = 256;") } } },
			{ TEXT("emitter bounds changed"), EScope::Emitter,
				{ { TEXT("FixedBounds        = box(-100, -100, -100, 100, 100, 100);"), TEXT("FixedBounds        = box(-120, -120, -120, 120, 120, 120);") } } },
			{ TEXT("link replaced by a literal"), EScope::Values,
				{ { TEXT("SpawnRate(SpawnRate = User.Rate);"), TEXT("SpawnRate(SpawnRate = 80.0);") } } },
			{ TEXT("literal replaced by a link"), EScope::Values,
				{ { TEXT("SpawnRate(SpawnRate = 80.0);"), TEXT("SpawnRate(SpawnRate = User.Rate);") } }, AllowLoss },
			{ TEXT("renderer property changed"), EScope::Emitter,
				{ { TEXT("SortMode   = None;"), TEXT("SortMode   = ViewDepth;") } }, AllowLoss },
			{ TEXT("system setting changed"), EScope::System,
				{ { TEXT("WarmupTime  = 0.0;"), TEXT("WarmupTime  = 0.5;") } } },
		};
		return Fixture;
	}

	static TArray<FFixture> MakeFixtures()
	{
		return { MakeSparks(), MakeRelay(), MakeGpu() };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXIncrementalEquivalenceTest,
	"DreamFX.Incremental.Equivalence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXIncrementalEquivalenceTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::IncrementalEquivalence;
	const FSummary& Summary = RunAll();
	for (const FString& Problem : Summary.EquivalenceProblems)
	{
		AddError(Problem);
	}
	TestTrue(TEXT("the sequences ran"), Summary.Steps.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXIncrementalCoverageTest,
	"DreamFX.Incremental.Coverage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXIncrementalCoverageTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::IncrementalEquivalence;
	const FSummary& Summary = RunAll();
	for (const FString& Problem : Summary.EquivalenceProblems)
	{
		AddError(Problem);
	}

	int32 Applied = 0;
	int32 Expected = 0;
	for (const FStepRecord& Step : Summary.Steps)
	{
		if (!ExpectsIncremental(Step.Scope))
		{
			continue;
		}
		++Expected;
		if (Step.bApplied)
		{
			++Applied;
		}
#if !DREAMFX_HAS_NIAGARA_FAST_EDIT
		else if (Step.FallbackReason.Contains(TEXT("which this engine rebuilds in full")))
		{
			// Without the fast-edit API the incremental path takes value writes and added or removed
			// emitters only; a structural change is the full rebuild on this engine, by design.
			--Expected;
		}
#endif
		else
		{
			AddError(FString::Printf(
				TEXT("%s / '%s' (%s): expected an incremental build, got a full rebuild. Reason: %s"),
				*Step.Fixture, *Step.Mutation, LexScope(Step.Scope),
				Step.FallbackReason.IsEmpty() ? TEXT("(none given)") : *Step.FallbackReason));
		}
	}
	UE_LOG(LogDreamFXIncremental, Display, TEXT("DFXINCR coverage: %d of %d steps expected to be incremental were, out of %d steps in all"),
		Applied, Expected, Summary.Steps.Num());
	return true;
}

#endif
