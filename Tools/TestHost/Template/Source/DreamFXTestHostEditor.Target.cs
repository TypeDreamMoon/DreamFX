// Copyright (c) 2026 TypeDreamMoon. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

/*
 * The editor target of the DreamFX test host -- the one every gate step runs in.
 *
 * SHARED BUILD ENVIRONMENT is the point of this file. An editor target is modular, and a modular target
 * shares the engine's build products: the engine modules already compiled into Engine/Binaries/Win64 are
 * linked against as they are, and only this project's module and its project plugin (DreamFX, whose
 * Binaries and Intermediate live in its own worktree) are compiled. That is already the default; it is
 * written out so that nobody "fixes" it to Unique, which would compile the entire engine a second time
 * into this project.
 *
 * The other settings are DevTest's (DevTestEditor.Target.cs) line for line, so that the two projects agree
 * on every engine module being up to date and neither rebuilds the other's.
 */
public class DreamFXTestHostEditorTarget : TargetRules
{
	public DreamFXTestHostEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		BuildEnvironment = TargetBuildEnvironment.Shared;
		ExtraModuleNames.Add("DreamFXTestHost");
	}
}
