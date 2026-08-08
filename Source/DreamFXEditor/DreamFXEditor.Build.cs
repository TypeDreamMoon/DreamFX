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

		if (bHasCustomHlslWrite)
		{
			PrivateIncludePaths.Add(Path.Combine(EngineDirectory, NiagaraEditorPrivate));

			// NiagaraNodeParameterMapGet.h includes SGraphPin.h, which is GraphEditor's. Only the
			// enabled configuration needs it, so a build without the exports gains no dependency.
			PrivateDependencyModuleNames.Add("GraphEditor");
		}
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
