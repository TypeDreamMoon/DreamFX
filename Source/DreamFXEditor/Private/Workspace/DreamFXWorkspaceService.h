#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Getting a text file in front of the author.
	 *
	 * A mirror of `FDreamShaderEditorLaunchUtils`, down to the discovery order and the fallback chain,
	 * because a team that has learned where DreamShader opens its files should not have to learn a
	 * second answer for DreamFX. Windows-only, like the rest of the discovery table.
	 */
	struct FDreamFXLaunchUtils
	{
		/** Opens a `.code-workspace`. Honours `bOpenWorkspaceInNewWindow`. */
		static bool LaunchVSCodeWorkspace(const FString& WorkspaceFilePath);

		/** Opens one file at a position. Always reuses the window; line and column are clamped to >= 1. */
		static bool LaunchVSCodeFile(const FString& FilePath, int32 Line = 1, int32 Column = 1);

		static bool LaunchTextFileWithNotepad(const FString& FilePath);

		/** VSCode, then the OS default editor, then Notepad. Returns false only if all three failed. */
		static bool LaunchTextFileInPreferredEditor(const FString& FilePath, int32 Line = 1, int32 Column = 1);
	};

	/** The `.code-workspace` file: where it goes, and what is in it. */
	struct FDreamFXWorkspaceService
	{
		/** `<project>/DFX/DreamFX.code-workspace`, whether or not it exists yet. */
		static FString GetWorkspaceFilePath();

		/**
		 * Rewrites the workspace file from the current source roots.
		 *
		 * Full rewrite, never a merge -- same as DreamShader. Hand-added `launch` or `tasks` blocks in
		 * this file are lost on the next call; per-user configuration belongs in `DFX/.vscode/`.
		 */
		static bool WriteWorkspaceFile(FString& OutWorkspaceFilePath, FString& OutError);
	};
}
