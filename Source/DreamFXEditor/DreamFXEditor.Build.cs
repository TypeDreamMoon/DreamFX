using System;
using System.IO;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Logging;
using UnrealBuildTool;

public class DreamFXEditor : ModuleRules
{
	/// <summary>
	/// The NiagaraEditor declarations a .dfm needs in order to become a UNiagaraScript.
	/// Each is probed for its export macro; every one must be present or the feature stays off,
	/// because a partial patch would turn a graceful degrade into a link error.
	///
	/// Pattern notes: the macro has to sit on the same declaration, so each regex pins the return
	/// type and the symbol name together. \s* between them lets the engine reformat freely, and
	/// matching the parameter list's opening token keeps `AddParameter` from being satisfied by one
	/// of its two sibling overloads, which take different arguments and are not what DreamFX calls.
	/// </summary>
	private static readonly (string Header, string Pattern, string Symbol)[] RequiredExports =
	{
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraNodeCustomHlsl.h",
			@"NIAGARAEDITOR_API\s+void\s+SetCustomHlsl\s*\(",
			"UNiagaraNodeCustomHlsl::SetCustomHlsl"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraNodeCustomHlsl.h",
			@"NIAGARAEDITOR_API\s+void\s+InitAsCustomHlslDynamicInput\s*\(",
			"UNiagaraNodeCustomHlsl::InitAsCustomHlslDynamicInput"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraNodeWithDynamicPins.h",
			@"NIAGARAEDITOR_API\s+UEdGraphPin\*\s+RequestNewTypedPin\s*\([^)]*FName",
			"UNiagaraNodeWithDynamicPins::RequestNewTypedPin"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraGraph.h",
			@"NIAGARAEDITOR_API\s+UNiagaraScriptVariable\*\s+AddParameter\s*\([^)]*FNiagaraVariableMetaData",
			"UNiagaraGraph::AddParameter"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Private/NiagaraNodeParameterMapGet.h",
			@"UCLASS\(MinimalAPI\)\s*class\s+UNiagaraNodeParameterMapGet",
			"UNiagaraNodeParameterMapGet"
		)
	};

	/// <summary>
	/// The MoonEngine additions to Niagara's external-edit API.
	///
	/// Stock UE 5.8 already ships most of that API -- AddEmitter, AddModule, AddSetParametersModule,
	/// GetModuleTopology, SetStackInputData and forty-odd others. What MoonEngine adds is a batch
	/// story: clear a stack in one call rather than n, defer the per-add refresh to one at the end,
	/// and stop the system relaunching a compile after every edit. Plus the parameter-default pair,
	/// which is the only functional item in the list.
	///
	/// Probed together, for the same reason as RequiredExports: a partial match would compile and
	/// then fail to link. The two optional *parameters* are probed as well -- AddModule without
	/// bDeferStackRefresh still links, but silently refreshes per add, which is the cost this whole
	/// define exists to avoid.
	/// </summary>
	private static readonly (string Header, string Pattern, string Symbol)[] FastEditExports =
	{
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h",
			@"static\s+void\s+ClearScriptStack\s*\(",
			"UNiagaraExternalEditUtilities::ClearScriptStack"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h",
			@"static\s+void\s+RefreshScriptStack\s*\(",
			"UNiagaraExternalEditUtilities::RefreshScriptStack"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h",
			@"static\s+void\s+GetEmitterParameterDefaults\s*\(",
			"UNiagaraExternalEditUtilities::GetEmitterParameterDefaults"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h",
			@"static\s+void\s+SetEmitterParameterDefault\s*\(",
			"UNiagaraExternalEditUtilities::SetEmitterParameterDefault"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h",
			@"static\s+void\s+CleanUpStaleEmitterParameters\s*\(",
			"UNiagaraExternalEditUtilities::CleanUpStaleEmitterParameters"
		),
		(
			"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h",
			@"AddModule\s*\([^)]*bDeferStackRefresh",
			"UNiagaraExternalEditUtilities::AddModule(..., bDeferStackRefresh)"
		),
		(
			"Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraSystem.h",
			@"void\s+SetSuppressCompileRequests\s*\(",
			"UNiagaraSystem::SetSuppressCompileRequests"
		)
	};

	/// <summary>
	/// The header that carries Niagara's whole external-edit API.
	///
	/// Epic introduced it in 5.8. Measured across every engine on this machine, 5.3 through 5.7 do not
	/// mention `ExternalEdit` anywhere in the Niagara plugin -- the API is not renamed or moved there,
	/// it does not exist. Of the 93 engine headers DreamFX includes, this is the only one missing on
	/// 5.6 and 5.7, which is why one probe is enough to describe the whole gap.
	/// </summary>
	private const string ExternalEditHeader =
		"Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h";

	/// <summary>
	/// NiagaraEditor's private include directory, relative to the engine root.
	///
	/// UNiagaraNodeParameterMapGet lives there, and a module's inputs have to come through one: a
	/// `Module.X` token read inside a custom HLSL node resolves against *that node's* scope, not the
	/// script's, so the input has to arrive as a pin fed from a map get at script scope. Reaching a
	/// private header is only defensible because this whole path is probe-gated -- on any engine
	/// without the exports the directory is never added and the include is never compiled.
	/// </summary>
	private const string NiagaraEditorPrivate = "Plugins/FX/Niagara/Source/NiagaraEditor/Private";

	public DreamFXEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Private/ is on the include path so the layered folders (Adapter, Schema, Generation, ...)
		// can include each other by their folder-qualified path.
		PrivateIncludePaths.Add(ModuleDirectory + "/Private");

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AssetRegistry",
				"AssetTools",
				"ContentBrowser",
				"Core",
				"CoreUObject",
				"DeveloperSettings",
				"DirectoryWatcher",
				"DreamFX",
				"Engine",
				"InputCore",
				"Json",
				"JsonUtilities",
				"Niagara",
				// GetMergeId (the durable stage usage id) is NIAGARACORE_API, not NIAGARA_API.
				"NiagaraCore",
				"NiagaraEditor",
				"Projects",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd"
			});

		// plan-v2 W1, option A: .dfm generation is an optional enhancement that turns itself on only
		// when the engine being built against exports the symbols it needs. On MoonEngine it does; on
		// a stock or prebuilt engine the probe misses and .dfm keeps the Phase 4 degraded behaviour
		// (parse + validate + DFX5100). Same plugin source either way -- the difference is one define.
		//
		// Probing the header text rather than trying to link is deliberate: a link failure is a build
		// break, and the whole point of the exception clause in plan 2.5 is that the engine patch must
		// never become a hard dependency.
		bool bHasCustomHlslWrite = ProbeCustomHlslWriteSupport();
		PublicDefinitions.Add("DREAMFX_HAS_CUSTOMHLSL_WRITE=" + (bHasCustomHlslWrite ? "1" : "0"));

		// The same bargain for the batch-edit API. Without it the adapter still does every operation,
		// just the slow way the stock engine offers: clear a stack by removing its modules one at a
		// time, let each add refresh the group, and let every edit relaunch the system's compile.
		// Measured on this tree, that is the difference between 9.2 and roughly 17 minutes for all 55
		// systems -- and a few seconds either way for the single-asset rebuild that is the normal edit.
		bool bHasFastEdit = ProbeFastEditSupport();
		PublicDefinitions.Add("DREAMFX_HAS_NIAGARA_FAST_EDIT=" + (bHasFastEdit ? "1" : "0"));

		// Engines older than 5.8 have no external-edit API at all. The adapter is written against it and
		// nothing else in DreamFX is, so rather than a second adapter there is a compatibility layer that
		// re-implements the API's surface on the same view models the engine's own implementation uses --
		// FNiagaraSystemViewModel, UNiagaraStackModuleItem, UNiagaraStackFunctionInput -- every one of
		// which carries NIAGARAEDITOR_API on 5.6 and 5.7. The adapter includes whichever exists.
		//
		// Compat/ only reaches the include path on an engine that needs it, so on 5.8 and MoonEngine the
		// header cannot be included even by accident and the shipped path is bit-for-bit what it was.
		bool bHasExternalEdit = File.Exists(
			Path.Combine(EngineDirectory, ExternalEditHeader.Replace('/', Path.DirectorySeparatorChar)));
		PublicDefinitions.Add("DREAMFX_HAS_NIAGARA_EXTERNAL_EDIT=" + (bHasExternalEdit ? "1" : "0"));

		if (!bHasExternalEdit)
		{
			Logger.LogInformation(
				"DreamFX: this engine has no external-edit API; building the compatibility layer instead.");
			PrivateIncludePaths.Add(ModuleDirectory + "/Private/Compat");
		}

		if (bHasCustomHlslWrite)
		{
			PrivateIncludePaths.Add(Path.Combine(EngineDirectory, NiagaraEditorPrivate));

			// NiagaraNodeParameterMapGet.h includes SGraphPin.h, which is GraphEditor's. Only the
			// enabled configuration needs it, so a build without the exports gains no dependency.
			PrivateDependencyModuleNames.Add("GraphEditor");
		}
	}

	private bool ProbeFastEditSupport()
	{
		if (Environment.GetEnvironmentVariable("DREAMFX_FORCE_NO_FAST_EDIT") == "1")
		{
			// Same escape hatch as the .dfm probe: prove the stock-engine path builds and passes on a
			// machine that does have the patched engine, without needing the other engine to hand.
			//
			// TOUCH THIS FILE WHEN YOU CHANGE THE VARIABLE. UBT tracks Build.cs by timestamp and does
			// not know an environment variable fed a PublicDefinition, so flipping the variable alone
			// leaves the cached makefile -- and the previous define -- in place. The tell is a build
			// that finishes in a fraction of a second having compiled nothing, and the cost of missing
			// it is an experiment that measured the binary it meant to replace. The log line below is
			// the only proof that the flip took: no line, no flip.
			Logger.LogInformation("DreamFX: Niagara fast-edit path disabled by DREAMFX_FORCE_NO_FAST_EDIT.");
			return false;
		}

		foreach ((string Header, string Pattern, string Symbol) Required in FastEditExports)
		{
			string FullPath = Path.Combine(EngineDirectory, Required.Header.Replace('/', Path.DirectorySeparatorChar));
			if (!File.Exists(FullPath))
			{
				Logger.LogInformation(
					"DreamFX: Niagara fast-edit path disabled -- '{Header}' not found.", Required.Header);
				return false;
			}

			if (!Regex.IsMatch(File.ReadAllText(FullPath), Required.Pattern))
			{
				Logger.LogInformation(
					"DreamFX: Niagara fast-edit path disabled -- this engine has no {Symbol}.", Required.Symbol);
				return false;
			}
		}

		Logger.LogInformation("DreamFX: Niagara fast-edit path enabled (MoonEngine batch-edit API present).");
		return true;
	}

	private bool ProbeCustomHlslWriteSupport()
	{
		if (Environment.GetEnvironmentVariable("DREAMFX_FORCE_NO_CUSTOMHLSL_WRITE") == "1")
		{
			// Escape hatch for the plan-v2 W1 acceptance test: prove the degraded path still builds
			// and still passes CI on a machine that does have the patched engine.
			Logger.LogInformation("DreamFX: .dfm generation disabled by DREAMFX_FORCE_NO_CUSTOMHLSL_WRITE.");
			return false;
		}

		foreach ((string Header, string Pattern, string Symbol) Required in RequiredExports)
		{
			string FullPath = Path.Combine(EngineDirectory, Required.Header.Replace('/', Path.DirectorySeparatorChar));
			if (!File.Exists(FullPath))
			{
				Logger.LogInformation(
					"DreamFX: .dfm generation disabled -- '{Header}' not found (prebuilt engine has no NiagaraEditor sources).",
					Required.Header);
				return false;
			}

			if (!Regex.IsMatch(File.ReadAllText(FullPath), Required.Pattern))
			{
				Logger.LogInformation(
					"DreamFX: .dfm generation disabled -- {Symbol} carries no NIAGARAEDITOR_API in this engine.",
					Required.Symbol);
				return false;
			}
		}

		Logger.LogInformation("DreamFX: .dfm generation enabled (every required NiagaraEditor export is present).");
		return true;
	}
}
