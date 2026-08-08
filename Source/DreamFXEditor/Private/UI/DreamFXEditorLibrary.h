#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "DreamFXEditorLibrary.generated.h"

class UNiagaraEmitter;
class UNiagaraSystem;

/**
 * The DreamFX menu commands, callable from Python and Blueprint.
 *
 * Not a second implementation: every function here forwards to the same `FDreamFXCommands` body the
 * menus call. It exists because a Slate menu entry cannot be invoked from a script, and a command
 * that can only be triggered by a human click is a command that never gets regression tested --
 * plan-v3's acceptance criteria for E1-E3 are otherwise unverifiable except by hand.
 */
UCLASS()
class UDreamFXEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Tools > DreamFX > Rebuild DFX. Queues every source; the build runs on the next few ticks. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void RebuildAllSources();

	/** Tools > DreamFX > Verify DFX. Runs synchronously and writes nothing. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void VerifyAllSources();

	/** Tools > DreamFX > Open DreamFX Workspace. Rewrites the workspace file and launches it. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void OpenWorkspace();

	/** Writes `DFX/DreamFX.code-workspace` without launching anything. Returns the path, or empty. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static FString WriteWorkspaceFile();

	/** Content Browser > DreamFX > Export .dfs. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void ExportSystem(UNiagaraSystem* System);

	/** Content Browser > DreamFX > Export .dfe. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void ExportEmitter(UNiagaraEmitter* Emitter);

	/**
	 * Content Browser > DreamFX > Adopt.
	 *
	 * @param bSkipConfirmation  skips the modal dialog only. Both refusals -- unrepresentable
	 *                           features, and an existing source for the same asset -- still apply.
	 */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void AdoptSystem(UNiagaraSystem* System, bool bSkipConfirmation = false);

	/** Content Browser > DreamFX > Open Source. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void OpenSource(UObject* Asset);

	/** Content Browser > DreamFX > Rebuild from Source. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void RebuildFromSource(UObject* Asset);

	/** Content Browser > DreamFX > Verify. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static void VerifyAsset(UObject* Asset);

	/** Whether the asset carries a DreamFX provenance stamp -- what picks the two-state menu. */
	UFUNCTION(BlueprintCallable, Category = "DreamFX")
	static bool HasProvenance(UObject* Asset);
};
