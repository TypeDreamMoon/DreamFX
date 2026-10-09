#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamFX::Editor::WorkspaceFollowups
{
	/** Removes only the exact files and empty directories created by this fixture. */
	struct FFiles
	{
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".dreamfx-test-tmp")
			/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TArray<FString> Files, Directories;
		void Track(const FString& File)
		{
			Files.AddUnique(File);
			for (FString Parent = FPaths::GetPath(File); FPaths::IsUnderDirectory(Parent, Directory) || FPaths::IsSamePath(Parent, Directory);
				Parent = FPaths::GetPath(Parent))
			{
				Directories.AddUnique(Parent);
				if (FPaths::IsSamePath(Parent, Directory)) { break; }
			}
		}
		FString Write(const FString& Relative, const FString& Text)
		{
			const FString File = Directory / Relative;
			Track(File);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
			return FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ? File : FString();
		}
		bool MoveDirectory(const FString& From, const FString& To)
		{
			if (IFileManager::Get().DirectoryExists(*To) || !IFileManager::Get().Move(*To, *From, false, false, false, true)) { return false; }
			for (FString& File : Files) { if (FPaths::IsUnderDirectory(File, From)) { File = To + File.RightChop(From.Len()); } }
			for (FString& Dir : Directories)
			{
				if (FPaths::IsSamePath(Dir, From) || FPaths::IsUnderDirectory(Dir, From)) { Dir = To + Dir.RightChop(From.Len()); }
			}
			return true;
		}
		~FFiles()
		{
			for (const FString& File : Files) { IFileManager::Get().Delete(*File, false, false, true); }
			Directories.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
			for (const FString& Dir : Directories) { IFileManager::Get().DeleteDirectory(*Dir, false, false); }
		}
	};
}
