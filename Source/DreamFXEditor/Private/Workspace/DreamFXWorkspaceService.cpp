#include "DreamFXWorkspaceService.h"

#include "Settings/DreamFXEditorSettings.h"
#include "SourceFiles/DreamFXPaths.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		FString QuoteProcessArgument(const FString& Argument)
		{
			FString Escaped = Argument;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("\"%s\""), *Escaped);
		}

		void AddExistingFileCandidate(TArray<FString>& OutCandidates, const FString& Candidate)
		{
			if (!Candidate.IsEmpty() && FPaths::FileExists(Candidate))
			{
				OutCandidates.AddUnique(FPaths::ConvertRelativePathToFull(Candidate));
			}
		}

		/**
		 * Where VSCode might be, most specific first.
		 *
		 * The nine-step table from DreamShader's workspace.md, kept in the same order: a per-user
		 * install shadows a machine-wide one, and the PATH entries come last because a `code` on PATH
		 * is as likely to be a shim for something else as it is the editor itself.
		 */
		TArray<FString> FindVSCodeExecutableCandidates()
		{
			TArray<FString> Candidates;

			auto AddFromEnvironmentDirectory = [&Candidates](const TCHAR* VariableName, const TCHAR* RelativePath)
			{
				const FString Directory = FPlatformMisc::GetEnvironmentVariable(VariableName);
				if (!Directory.IsEmpty())
				{
					AddExistingFileCandidate(Candidates, FPaths::Combine(Directory, RelativePath));
				}
			};

			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code/Code.exe"));
			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code/bin/code.cmd"));
			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code Insiders/Code - Insiders.exe"));
			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code Insiders/bin/code-insiders.cmd"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles"), TEXT("Microsoft VS Code/Code.exe"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles"), TEXT("Microsoft VS Code/bin/code.cmd"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles(x86)"), TEXT("Microsoft VS Code/Code.exe"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles(x86)"), TEXT("Microsoft VS Code/bin/code.cmd"));

			const FString PathEnvironment = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
			TArray<FString> PathEntries;
			PathEnvironment.ParseIntoArray(PathEntries, TEXT(";"), true);
			for (FString PathEntry : PathEntries)
			{
				PathEntry.TrimStartAndEndInline();
				if (PathEntry.IsEmpty())
				{
					continue;
				}

				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("code.cmd")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("code.exe")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("Code.exe")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("code-insiders.cmd")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("Code - Insiders.exe")));
			}

			return Candidates;
		}

		/**
		 * Runs one candidate with the given arguments.
		 *
		 * A `.cmd` shim has to go through cmd.exe -- CreateProc cannot execute a batch file directly --
		 * and that route needs a hidden console, or every launch flashes a window.
		 */
		bool LaunchCandidate(const FString& Candidate, const FString& Arguments)
		{
			FProcHandle ProcessHandle;
			if (Candidate.EndsWith(TEXT(".cmd"), ESearchCase::IgnoreCase)
				|| Candidate.EndsWith(TEXT(".bat"), ESearchCase::IgnoreCase))
			{
				FString CmdExe = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
				if (CmdExe.IsEmpty())
				{
					CmdExe = TEXT("C:/Windows/System32/cmd.exe");
				}

				const FString Parameters = FString::Printf(TEXT("/C \"\"%s\" %s\""), *Candidate, *Arguments);
				ProcessHandle = FPlatformProcess::CreateProc(
					*CmdExe, *Parameters, true, true, true, nullptr, 0, nullptr, nullptr);
			}
			else
			{
				ProcessHandle = FPlatformProcess::CreateProc(
					*Candidate, *Arguments, true, false, false, nullptr, 0, nullptr, nullptr);
			}

			if (ProcessHandle.IsValid())
			{
				FPlatformProcess::CloseProc(ProcessHandle);
				return true;
			}
			return false;
		}
	}

	bool FDreamFXLaunchUtils::LaunchVSCodeWorkspace(const FString& WorkspaceFilePath)
	{
		const UDreamFXEditorSettings* Settings = GetDefault<UDreamFXEditorSettings>();
		const TCHAR* WindowFlag = (Settings && !Settings->bOpenWorkspaceInNewWindow) ? TEXT("--reuse-window ") : TEXT("");
		const FString Arguments = FString::Printf(TEXT("%s%s"), WindowFlag, *QuoteProcessArgument(WorkspaceFilePath));

		for (const FString& Candidate : FindVSCodeExecutableCandidates())
		{
			if (LaunchCandidate(Candidate, Arguments))
			{
				return true;
			}
		}
		return false;
	}

	bool FDreamFXLaunchUtils::LaunchVSCodeFile(const FString& FilePath, const int32 Line, const int32 Column)
	{
		const FString GotoArgument = FString::Printf(
			TEXT("%s:%d:%d"), *FilePath, FMath::Max(1, Line), FMath::Max(1, Column));
		const FString Arguments = FString::Printf(
			TEXT("--reuse-window -g %s"), *QuoteProcessArgument(GotoArgument));

		for (const FString& Candidate : FindVSCodeExecutableCandidates())
		{
			if (LaunchCandidate(Candidate, Arguments))
			{
				return true;
			}
		}
		return false;
	}

	bool FDreamFXLaunchUtils::LaunchTextFileWithNotepad(const FString& FilePath)
	{
		TArray<FString> Candidates;
		const FString SystemRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
		AddExistingFileCandidate(Candidates, FPaths::Combine(SystemRoot, TEXT("System32/notepad.exe")));
		Candidates.Add(TEXT("notepad.exe"));

		for (const FString& Candidate : Candidates)
		{
			if (LaunchCandidate(Candidate, QuoteProcessArgument(FilePath)))
			{
				return true;
			}
		}
		return false;
	}

	bool FDreamFXLaunchUtils::LaunchTextFileInPreferredEditor(const FString& FilePath, const int32 Line, const int32 Column)
	{
		if (LaunchVSCodeFile(FilePath, Line, Column))
		{
			return true;
		}
		if (FPlatformProcess::LaunchFileInDefaultExternalApplication(*FilePath, nullptr, ELaunchVerb::Edit, false))
		{
			return true;
		}
		return LaunchTextFileWithNotepad(FilePath);
	}

	FString FDreamFXWorkspaceService::GetWorkspaceFilePath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("DFX/DreamFX.code-workspace")));
	}

	FString FDreamFXWorkspaceService::BuildWorkspaceJson(const FString& WorkspaceDirectory)
	{
		FString WorkspaceText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&WorkspaceText);
		Writer->WriteObjectStart();
		Writer->WriteArrayStart(TEXT("folders"));

		// The project root, always first and always ".": the workspace file lives inside it, and a
		// stable folder identity is what keeps VSCode's per-folder state attached across a rewrite.
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("name"), TEXT("DreamFX Source"));
		Writer->WriteValue(TEXT("path"), TEXT("."));
		Writer->WriteObjectEnd();

		for (const FSourceRoot& Root : FDreamFXPaths::GetSourceRoots())
		{
			if (Root.RootToken.IsEmpty())
			{
				continue; // the project root, already written as "."
			}

			FString FolderPath = Root.Directory;
			// A plugin on another drive -- an engine plugin with the engine on a different volume --
			// has no relative form on Windows. An absolute path is still a valid workspace folder.
			if (!FPaths::MakePathRelativeTo(FolderPath, *(WorkspaceDirectory + TEXT("/"))))
			{
				FolderPath = Root.Directory;
			}

			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"),
				FString::Printf(TEXT("Plugin: %s"), *Root.RootToken.RightChop(7 /*"Plugin."*/)));
			Writer->WriteValue(TEXT("path"), FolderPath);
			Writer->WriteObjectEnd();
		}

		Writer->WriteArrayEnd();
		Writer->WriteObjectStart(TEXT("settings"));
		Writer->WriteObjectStart(TEXT("files.associations"));
		// The id the DreamFXLang extension registers. The association is written whether or not the
		// extension is installed: without it VSCode falls back to plain text, which is harmless.
		Writer->WriteValue(TEXT("*.dfs"), TEXT("dreamfxlang"));
		Writer->WriteValue(TEXT("*.dfe"), TEXT("dreamfxlang"));
		Writer->WriteValue(TEXT("*.dfm"), TEXT("dreamfxlang"));
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();

		// A recommendation, not a requirement: VSCode shows it as a prompt the first time the
		// workspace opens and never again if it is dismissed. The association above is what makes the
		// files readable at all; this is what makes them readable *well*.
		Writer->WriteObjectStart(TEXT("extensions"));
		Writer->WriteArrayStart(TEXT("recommendations"));
		Writer->WriteValue(TEXT("typedreammoon.dreamfxlang-language-support"));
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();

		Writer->WriteObjectEnd();
		Writer->Close();

		return WorkspaceText;
	}

	bool FDreamFXWorkspaceService::WriteWorkspaceFile(FString& OutWorkspaceFilePath, FString& OutError)
	{
		const FString WorkspaceFilePath = GetWorkspaceFilePath();
		const FString WorkspaceDirectory = FPaths::GetPath(WorkspaceFilePath);

		if (!IFileManager::Get().MakeDirectory(*WorkspaceDirectory, /*Tree=*/true))
		{
			OutError = FString::Printf(TEXT("Failed to create DreamFX source directory '%s'."), *WorkspaceDirectory);
			return false;
		}

		// MakeDirectory may have just produced the project root that the cached scan missed, and a
		// plugin can be enabled after the first scan. Both would show up as a missing folder.
		FDreamFXPaths::InvalidateSourceRoots();

		const FString WorkspaceText = BuildWorkspaceJson(WorkspaceDirectory);

		if (!FFileHelper::SaveStringToFile(WorkspaceText, *WorkspaceFilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write DreamFX workspace file '%s'."), *WorkspaceFilePath);
			return false;
		}

		OutWorkspaceFilePath = WorkspaceFilePath;
		return true;
	}
}
