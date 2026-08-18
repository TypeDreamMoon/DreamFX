#include "DreamFXPhase0Commandlet.h"

#include "DreamFXModule.h"

#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraVariant.h"

#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::DreamFX::Phase0
{
	/**
	 * One probe outcome. `bRequired` probes gate the commandlet's exit code; optional probes are
	 * reported but never fail the run, because a Phase 0 result of "core chain works, feature X
	 * doesn't" is a useful answer, not a failure.
	 */
	struct FProbeResult
	{
		FString Name;
		bool bPassed = false;
		bool bRequired = true;
		FString Detail;
	};

	/**
	 * The API addresses script stacks by the *enum name* of ENiagaraScriptUsage -- see
	 * ScriptUsageFromName in NiagaraExternalSystemEditorUtilities.cpp:328, which resolves the FName
	 * through StaticEnum<ENiagaraScriptUsage>()->GetValueByName. Deriving the string from the same
	 * reflection data the engine uses avoids hardcoding a guess at the qualified spelling.
	 */
	FName ScriptUsageName(ENiagaraScriptUsage Usage)
	{
		const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
		return UsageEnum ? UsageEnum->GetNameByValue(static_cast<int64>(Usage)) : NAME_None;
	}

	/** Joins a context's accumulated errors into one log-friendly line. */
	FString DescribeErrors(const FNiagaraExternalEditContext& Context)
	{
		TArray<FString> Lines;
		for (const FText& Error : Context.Errors)
		{
			Lines.Add(Error.ToString());
		}
		return FString::Join(Lines, TEXT(" | "));
	}
}

UDreamFXPhase0Commandlet::UDreamFXPhase0Commandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UDreamFXPhase0Commandlet::Main(const FString& Params)
{
	using namespace UE::DreamFX::Phase0;

	FString PackagePath = TEXT("/Game/DreamFX_Phase0");
	FString AssetName = TEXT("NS_DreamFXPhase0");
	FParse::Value(*Params, TEXT("Path="), PackagePath);
	FParse::Value(*Params, TEXT("Name="), AssetName);
	const bool bSave = FParse::Param(*Params, TEXT("Save"));

	TArray<FProbeResult> Results;
	auto Report = [&Results](const FString& Name, bool bPassed, bool bRequired, const FString& Detail)
	{
		Results.Add(FProbeResult{Name, bPassed, bRequired, Detail});
		UE_LOG(LogDreamFX, Display, TEXT("[%s] %-34s %s"),
			bPassed ? TEXT("PASS") : (bRequired ? TEXT("FAIL") : TEXT("SKIP")),
			*Name, *Detail);
	};

	UE_LOG(LogDreamFX, Display, TEXT("=== DreamFX Phase 0 -- headless Niagara authoring probe ==="));
	UE_LOG(LogDreamFX, Display, TEXT("Target asset: %s/%s"), *PackagePath, *AssetName);

	// ---------------------------------------------------------------------------------------
	// P0  Pre-existing asset at the target path.
	//
	//     CreateNiagaraSystem does no collision handling whatsoever -- it hands the path straight to
	//     FNiagaraSystemAssetBuilder (NiagaraExternalSystemEditorUtilities.cpp:984). When a .uasset
	//     already sits there, the resulting package is only partially loaded, and SavePackage does not
	//     fail gracefully: ValidatePackage raises appError ("asset is only partially loaded",
	//     SavePackage2.cpp:249) and takes the whole process down.
	//
	//     Regenerating over an existing asset is DreamFX's normal case, so the generator must always
	//     fully load first. Probed here because a crash-on-rerun would be a miserable thing to
	//     discover from a CI gate.
	// ---------------------------------------------------------------------------------------
	const FString TargetPackageName = PackagePath / AssetName;
	{
		const bool bExists = FPackageName::DoesPackageExist(TargetPackageName);
		bool bLoaded = false;
		if (bExists)
		{
			if (UPackage* ExistingPackage = LoadPackage(nullptr, *TargetPackageName, LOAD_None))
			{
				ExistingPackage->FullyLoad();
				bLoaded = ExistingPackage->IsFullyLoaded();
			}
		}
		Report(TEXT("P0 Pre-existing asset"), !bExists || bLoaded, true,
			bExists ? FString::Printf(TEXT("existed, fullyLoaded=%d"), bLoaded ? 1 : 0)
			        : TEXT("clean path, nothing to load"));
	}

	// ---------------------------------------------------------------------------------------
	// P1  CreateNiagaraSystem. Also the first real construction of FNiagaraExternalEditContext,
	//     which is what R1 is actually about -- it builds an FNiagaraSystemViewModel internally.
	// ---------------------------------------------------------------------------------------
	UNiagaraSystem* System = nullptr;
	{
		FNiagaraExternalEditContext Context;
		System = UNiagaraExternalEditUtilities::CreateNiagaraSystem(AssetName, PackagePath, nullptr, Context);
		Report(TEXT("P1 CreateNiagaraSystem"), System != nullptr && !Context.HasErrors(), true,
			System ? FString::Printf(TEXT("-> %s"), *System->GetPathName()) : DescribeErrors(Context));
	}

	if (System == nullptr)
	{
		UE_LOG(LogDreamFX, Error, TEXT("P1 failed; nothing downstream can run. Aborting."));
		return 1;
	}

	// ---------------------------------------------------------------------------------------
	// P2  User variable round-trip. Plan doc 3.1 maps the `Properties = {}` block onto these.
	// ---------------------------------------------------------------------------------------
	{
		FNiagaraExternalEditContext Context(System);

		FNiagaraExt_UserVariable Variable;
		Variable.Name = TEXT("SparkCount");
		Variable.Type = FNiagaraTypeDefinition::GetFloatDef();
		Variable.Description = FText::FromString(TEXT("DreamFX Phase 0 probe variable"));

		const float DefaultValue = 24.0f;
		Variable.DefaultValue.Set(FNiagaraTypeDefinition::GetFloatDef(), FNiagaraVariant(&DefaultValue, sizeof(float)));

		UNiagaraExternalEditUtilities::AddUserVariable(System, Variable, Context);
		const bool bAdded = !Context.HasErrors();

		FNiagaraExternalEditContext ReadContext(System);
		FNiagaraExt_UserVariables ReadBack;
		UNiagaraExternalEditUtilities::GetUserVariables(System, ReadBack, ReadContext);

		// Probe run 1 showed the write succeeds but an exact-name match fails, so the stored name is
		// not the bare one we passed in. Match on suffix and log what actually came back.
		TArray<FString> ReadNames;
		for (const FNiagaraExt_UserVariable& ReadVariable : ReadBack.UserVariables)
		{
			ReadNames.Add(ReadVariable.Name.ToString());
		}
		const bool bFound = ReadNames.ContainsByPredicate(
			[](const FString& Name) { return Name.EndsWith(TEXT("SparkCount")); });

		Report(TEXT("P2 AddUserVariable+readback"), bAdded && bFound, false,
			bAdded ? FString::Printf(TEXT("%d read back: [%s]"),
					ReadBack.UserVariables.Num(), *FString::Join(ReadNames, TEXT(", ")))
			       : DescribeErrors(Context));
	}

	// ---------------------------------------------------------------------------------------
	// P3  AddEmitter. Probe run 1 established that a null template is rejected outright
	//     ("template emitter is null"), so there is no create-from-nothing path. The engine's own
	//     answer is UNiagaraEmitterFactoryNew::InitializeEmitter, which builds a fresh emitter with
	//     its four script stacks wired up; we synthesize a transient one and hand it in as template.
	//     bAddDefaultModulesAndRenderers=false because DreamFX declares every module itself.
	// ---------------------------------------------------------------------------------------
	FName EmitterName = NAME_None;
	{
		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_EmitterTopology Topology;

		UNiagaraEmitter* TemplateEmitter = NewObject<UNiagaraEmitter>(
			GetTransientPackage(), TEXT("DreamFXPhase0TemplateEmitter"), RF_Transactional);
		UNiagaraEmitterFactoryNew::InitializeEmitter(TemplateEmitter, /*bAddDefaultModulesAndRenderers=*/false);

		UNiagaraExternalEditUtilities::AddEmitter(TemplateEmitter, TEXT("Sparks"), Topology, Context);

		EmitterName = Topology.EmitterName;
		const bool bOk = !Context.HasErrors() && EmitterName != NAME_None;
		Report(TEXT("P3 AddEmitter(null template)"), bOk, true,
			bOk ? FString::Printf(TEXT("name=%s simTarget=%d spawnModules=%d updateModules=%d"),
					*EmitterName.ToString(), static_cast<int32>(Topology.SimTarget),
					Topology.ParticleSpawnScript.Modules.Num(),
					Topology.ParticleUpdateScript.Modules.Num())
			    : DescribeErrors(Context));
	}

	// ---------------------------------------------------------------------------------------
	// P4  AddModule into the Particle Update stack -- the single most important write path,
	//     since every DreamFX stack statement lowers to this call.
	// ---------------------------------------------------------------------------------------
	const FName ParticleUpdateName = ScriptUsageName(ENiagaraScriptUsage::ParticleUpdateScript);
	{
		UNiagaraScript* ModuleAsset = LoadObject<UNiagaraScript>(
			nullptr, TEXT("/Niagara/Modules/Update/Forces/GravityForce.GravityForce"));

		if (ModuleAsset == nullptr)
		{
			Report(TEXT("P4 AddModule(GravityForce)"), false, true, TEXT("module asset failed to load"));
		}
		else if (EmitterName == NAME_None)
		{
			Report(TEXT("P4 AddModule(GravityForce)"), false, true, TEXT("no emitter from P3"));
		}
		else
		{
			FNiagaraExternalEditContext Context(System);
			FNiagaraExt_StackItemReference StackRef(System, EmitterName, ParticleUpdateName);
			FNiagaraExt_ModuleTopology Topology;
			UNiagaraExternalEditUtilities::AddModule(StackRef, ModuleAsset, Topology, Context);

			const bool bOk = !Context.HasErrors() && Topology.ModuleName != NAME_None;
			Report(TEXT("P4 AddModule(GravityForce)"), bOk, true,
				bOk ? FString::Printf(TEXT("module=%s inputs=%d (scriptName=%s)"),
						*Topology.ModuleName.ToString(), Topology.Inputs.Num(), *ParticleUpdateName.ToString())
				    : DescribeErrors(Context));
		}
	}

	// ---------------------------------------------------------------------------------------
	// P5  Topology read-back. This is the decompiler's whole backend -- if it works headless,
	//     Phase 5 is mostly a pretty-printer.
	// ---------------------------------------------------------------------------------------
	{
		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_StackItemReference EmitterRef(System, EmitterName);
		FNiagaraExt_EmitterTopology Topology;
		UNiagaraExternalEditUtilities::GetEmitterTopology(EmitterRef, Topology, Context);

		const int32 TotalModules =
			Topology.EmitterSpawnScript.Modules.Num() + Topology.EmitterUpdateScript.Modules.Num() +
			Topology.ParticleSpawnScript.Modules.Num() + Topology.ParticleUpdateScript.Modules.Num();

		Report(TEXT("P5 GetEmitterTopology"), !Context.HasErrors(), true,
			Context.HasErrors() ? DescribeErrors(Context)
			                    : FString::Printf(TEXT("%d module(s) across 4 stacks, %d renderer(s)"),
			                          TotalModules, Topology.Renderers.Num()));

		for (const FNiagaraExt_ScriptStackTopology* Stack :
			{&Topology.EmitterSpawnScript, &Topology.EmitterUpdateScript,
			 &Topology.ParticleSpawnScript, &Topology.ParticleUpdateScript})
		{
			for (const FNiagaraExt_ModuleTopology& Module : Stack->Modules)
			{
				UE_LOG(LogDreamFX, Display, TEXT("       %s / %s (setParams=%d, inputs=%d)"),
					*Stack->ScriptName.ToString(), *Module.ModuleName.ToString(),
					Module.bIsSetParametersModule ? 1 : 0, Module.Inputs.Num());
			}
		}
	}

	// ---------------------------------------------------------------------------------------
	// P6  Compile + read diagnostics back. This is the CI gate in miniature.
	// ---------------------------------------------------------------------------------------
	{
		// PollForCompilationComplete is a non-blocking poll -- it forwards to QueryCompileComplete(false)
		// and returns false for as long as a compile is in flight, which is what probe run 2 hit.
		// A build gate needs the blocking form. bShowProgress=false keeps a headless run quiet.
		System->RequestCompile(/*bForce=*/false);
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/false, /*bShowProgress=*/false);

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_SystemCompileState CompileState;
		UNiagaraExternalEditUtilities::GetSystemCompileState(System, CompileState, Context);

		// Do NOT gate on PollForCompilationComplete. It forwards to QueryCompileComplete, which
		// returns false whenever ActiveCompilations is empty (NiagaraSystem.cpp:3536) -- i.e. it reads
		// false both while a compile is in flight AND once it has finished. The only sound success
		// signal is the reported status plus the error flag.
		const ENiagaraExt_ScriptCompileStatus Status = CompileState.AggregateStatus;
		const bool bStatusOk =
			Status == ENiagaraExt_ScriptCompileStatus::UpToDate ||
			Status == ENiagaraExt_ScriptCompileStatus::UpToDateWithWarnings ||
			Status == ENiagaraExt_ScriptCompileStatus::ComputeUpToDateWithWarnings;

		const UEnum* StatusEnum = StaticEnum<ENiagaraExt_ScriptCompileStatus>();
		const FString StatusName = StatusEnum
			? StatusEnum->GetNameStringByValue(static_cast<int64>(Status))
			: FString::FromInt(static_cast<int32>(Status));

		Report(TEXT("P6 Compile+GetSystemCompileState"),
			bStatusOk && !CompileState.bHasErrors && !Context.HasErrors(), true,
			FString::Printf(TEXT("status=%s errors=%d warnings=%d scripts=%d stale=%d"),
				*StatusName, CompileState.bHasErrors ? 1 : 0, CompileState.bHasWarnings ? 1 : 0,
				CompileState.Scripts.Num(), CompileState.bIsStale ? 1 : 0));

		for (const FNiagaraExt_ScriptCompileInfo& Script : CompileState.Scripts)
		{
			for (const FNiagaraExt_CompileEvent& Event : Script.CompileEvents)
			{
				UE_LOG(LogDreamFX, Display, TEXT("       [%s/%s] sev=%d %s"),
					*Script.EmitterName.ToString(), *Script.ScriptName.ToString(),
					static_cast<int32>(Event.Severity), *Event.Message);
			}
		}
	}

	// ---------------------------------------------------------------------------------------
	// P7  Persist. Plan doc Phase 0 item 6 wants save -> reopen -> play; this is the save half.
	// ---------------------------------------------------------------------------------------
	if (bSave)
	{
		UPackage* Package = System->GetOutermost();
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GWarn; // default GError makes a failed save fatal

		const bool bSaved = UPackage::SavePackage(Package, System, *FileName, SaveArgs);
		Report(TEXT("P7 SavePackage"), bSaved, false, bSaved ? FileName : TEXT("save returned false"));
	}
	else
	{
		Report(TEXT("P7 SavePackage"), false, false, TEXT("skipped (pass -Save to enable)"));
	}

	// ---------------------------------------------------------------------------------------

	int32 RequiredFailures = 0;
	for (const FProbeResult& Result : Results)
	{
		if (Result.bRequired && !Result.bPassed)
		{
			++RequiredFailures;
		}
	}

	UE_LOG(LogDreamFX, Display, TEXT("=== Phase 0 done: %d probe(s), %d required failure(s) ==="),
		Results.Num(), RequiredFailures);

	return RequiredFailures == 0 ? 0 : 1;
}
