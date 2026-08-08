#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "DreamFXEditorSettings.generated.h"

/**
 * The editor-side knobs: everything the headless pipeline does not need.
 *
 * Config=Editor rather than Config=Game on purpose -- none of this changes what a build produces, so
 * none of it belongs in a file the runtime reads. It lands in Project Settings > Plugins > DreamFX.
 */
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "DreamFX"))
class UDreamFXEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/**
	 * Whether *Open DreamFX Workspace* asks VSCode for a new window.
	 *
	 * Only the workspace launcher reads it. Opening a single file always reuses the window -- a new
	 * window per diagnostic jump would be unusable, and that is the same split DreamShader shipped.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Workspace",
		meta = (DisplayName = "Open Workspace In New Window"))
	bool bOpenWorkspaceInNewWindow = true;

	/**
	 * Where *Export .dfs* writes, relative to the project directory.
	 *
	 * Ordinary source, watched and built like the rest of `DFX/` (plan-v4 V1). What keeps an export
	 * from behaving like *Adopt* is not where it sits but what it names: its `Name=` points into the
	 * `Decompiled/` content namespace, so a build writes a mirror and never the asset it was read
	 * from. Files here that predate that rule are refused with DFX8013 rather than obeyed.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Decompiler",
		meta = (DisplayName = "Decompiled Output Directory"))
	FString DecompiledOutputDirectory = TEXT("DFX/Decompiled");

	/**
	 * How many files may change at once before the watcher stops rebuilding them automatically.
	 *
	 * Re-exporting a content pack, switching branch or running a scripted rewrite changes many
	 * sources in one go, and rebuilding all of them queues a Niagara compile per system. Measured
	 * with twelve files: 54 system compiles and eight compile-pool saturations, on a project where
	 * the same thing at 24 systems put 237 jobs in the queue and killed the editor. Past this many,
	 * the watcher offers the rebuild instead of starting it.
	 *
	 * Ordinary saves are unaffected -- one file, or any batch at or under this size, still rebuilds
	 * immediately, which is what makes the iteration loop work. An explicit *Rebuild DFX* is never
	 * gated either: it was asked for.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Workspace", meta = (ClampMin = "1",
		DisplayName = "Bulk Rebuild Threshold"))
	int32 BulkRebuildThreshold = 8;
};
