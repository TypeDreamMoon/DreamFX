#include "Workspace/DreamFXSourceWatchSession.h"

#include "HAL/FileManager.h"
#include "IDirectoryWatcher.h"
#include "Misc/Paths.h"
#include "SourceFiles/DreamFXPaths.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		bool ContainsPath(const TArray<FString>& Paths, const FString& File)
		{
			return Paths.ContainsByPredicate([&](const FString& Path) { return FPaths::IsSamePath(Path, File); });
		}
		bool InDirectory(const FString& File, const FString& Directory)
		{
			return FPaths::IsSamePath(File, Directory) || FPaths::IsUnderDirectory(File, Directory);
		}
		FString ParentDirectory(const FString& Path)
		{
			FString Parent = FPaths::GetPath(Path);
			// Keep a volume root absolute ("I:/", never the drive-relative spelling "I:").
			if (Parent.EndsWith(TEXT(":"))) { Parent += TEXT("/"); }
			return Parent.IsEmpty() ? Path : Parent;
		}
	}

	FSourceWatchSession::FSourceWatchSession(IDirectoryWatcher& InWatcher, TFunction<void(const TSet<FString>&)> InQueueSources)
		: Watcher(InWatcher), QueueSources(MoveTemp(InQueueSources)) {}

	FSourceWatchSession::~FSourceWatchSession()
	{
		Stop();
	}

	void FSourceWatchSession::Stop(bool bWatcherAvailable)
	{
		if (bWatcherAvailable)
		{
			for (const auto& Entry : Handles) { Watcher.UnregisterDirectoryChangedCallback_Handle(Entry.Key, Entry.Value); }
		}
		Handles.Reset();
		PendingChanges.Reset();
	}

	bool FSourceWatchSession::IsRootSource(const FString& File) const
	{
		return Roots.ContainsByPredicate([&](const FString& Root) { return FPaths::IsUnderDirectory(File, Root); });
	}

	void FSourceWatchSession::GetKnownFiles(TArray<FString>& Out) const
	{
		Out = SourceFiles;
		Dependencies.GetReferencedFiles(Out);
	}

	void FSourceWatchSession::Reindex()
	{
		SourceFiles.Reset();
		for (const FString& Root : Roots)
		{
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Root, TEXT("*"), true, false);
			for (const FString& File : Found)
			{
				if (FDreamFXPaths::IsSourceFile(File)) { SourceFiles.AddUnique(FPaths::ConvertRelativePathToFull(File)); }
			}
		}
		Dependencies.Refresh(SourceFiles);
	}

	TArray<FString> FSourceWatchSession::DesiredWatchDirectories() const
	{
		TArray<FString> Directories;
		for (const FString& Root : Roots) { Directories.AddUnique(ParentDirectory(Root)); }
		TArray<FString> Referenced;
		Dependencies.GetReferencedFiles(Referenced);
		for (const FString& File : Referenced)
		{
			if (IsRootSource(File)) { continue; }
			// Watch a surviving ancestor while a dependency's directory is absent. Creation of any
			// missing intermediate directory then invalidates the unresolved reference. Start above
			// the containing directory: watching that directory itself misses its rename/removal.
			FString Directory = ParentDirectory(ParentDirectory(File));
			while (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
			{
				const FString Parent = ParentDirectory(Directory);
				if (Parent == Directory) { break; }
				Directory = Parent;
			}
			if (!Directory.IsEmpty() && IFileManager::Get().DirectoryExists(*Directory)) { Directories.AddUnique(Directory); }
		}
		// Keep surviving ancestor coverage after a missing directory returns to avoid unnecessary
		// subscription changes while preserving notifications for the containing directory itself.
		for (const auto& Entry : Handles)
		{
			if (IFileManager::Get().DirectoryExists(*Entry.Key)
				&& Directories.ContainsByPredicate([&](const FString& Directory) { return InDirectory(Directory, Entry.Key); }))
			{
				Directories.AddUnique(Entry.Key);
			}
		}
		Directories.Sort([](const FString& A, const FString& B) { return A.Len() < B.Len(); });
		TArray<FString> Result;
		for (const FString& Directory : Directories)
		{
			if (IFileManager::Get().DirectoryExists(*Directory)
				&& !Result.ContainsByPredicate([&](const FString& Existing) { return InDirectory(Directory, Existing); }))
			{
				Result.Add(Directory);
			}
		}
		return Result;
	}

	void FSourceWatchSession::ReconcileWatches()
	{
		const TArray<FString> Desired = DesiredWatchDirectories();
		// Register replacements first so expanding/collapsing watch coverage does not leave a gap.
		bool bAllRegistered = true;
		for (const FString& Directory : Desired)
		{
			if (!Handles.Contains(Directory))
			{
				FDelegateHandle Handle;
				if (Watcher.RegisterDirectoryChangedCallback_Handle(Directory,
					IDirectoryWatcher::FDirectoryChanged::CreateRaw(this, &FSourceWatchSession::HandleChanges), Handle,
					IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges)) { Handles.Add(Directory, Handle); }
				else { bAllRegistered = false; }
			}
		}
		if (!bAllRegistered) { return; } // Keep existing coverage when a replacement cannot be registered.
		for (auto It = Handles.CreateIterator(); It; ++It)
		{
			if (!ContainsPath(Desired, It.Key()))
			{
				Watcher.UnregisterDirectoryChangedCallback_Handle(It.Key(), It.Value());
				It.RemoveCurrent();
			}
		}
	}

	void FSourceWatchSession::Refresh(const TArray<FString>& SourceDirectories, bool bQueueNewSources)
	{
		TArray<FString> NewRoots;
		for (const FString& Root : SourceDirectories) { NewRoots.AddUnique(FPaths::ConvertRelativePathToFull(Root)); }
		TArray<FString> ChangedDirectories;
		for (const FString& Root : Roots) { if (!ContainsPath(NewRoots, Root)) { ChangedDirectories.AddUnique(Root); } }
		for (const FString& Root : NewRoots) { if (!ContainsPath(Roots, Root)) { ChangedDirectories.AddUnique(Root); } }
		// A watch on the removed directory itself need not receive its removal/rename event.
		for (const auto& Entry : Handles)
		{
			if (!IFileManager::Get().DirectoryExists(*Entry.Key)) { ChangedDirectories.AddUnique(Entry.Key); }
		}
		Roots = MoveTemp(NewRoots);
		if (!bInitialized || !bQueueNewSources)
		{
			Reindex();
			bInitialized = true;
			ReconcileWatches();
		}
		else if (!ChangedDirectories.IsEmpty()) { Invalidate({}, ChangedDirectories); }
		else { ReconcileWatches(); }
	}

	void FSourceWatchSession::Invalidate(TArray<FString> ChangedFiles, const TArray<FString>& ChangedDirectories)
	{
		auto AddAffectedFiles = [&](const TArray<FString>& Known)
		{
			for (const FString& File : Known)
			{
				if (ChangedDirectories.ContainsByPredicate([&](const FString& Directory) { return InDirectory(File, Directory); }))
				{
					ChangedFiles.AddUnique(File);
				}
			}
		};
		TArray<FString> Known;
		GetKnownFiles(Known);
		AddAffectedFiles(Known);
		TSet<FString> Pending;
		Dependencies.FindDependents(ChangedFiles, Pending);
		Reindex();
		GetKnownFiles(Known);
		AddAffectedFiles(Known);
		Dependencies.FindDependents(ChangedFiles, Pending);
		for (const FString& File : ChangedFiles)
		{
			if (IsRootSource(File) && FPaths::FileExists(File)) { Pending.Add(File); }
		}
		for (auto It = Pending.CreateIterator(); It; ++It)
		{
			if (!IsRootSource(*It) || !FPaths::FileExists(*It)) { It.RemoveCurrent(); }
		}
		ReconcileWatches();
		if (!Pending.IsEmpty()) { QueueSources(Pending); }
	}

	void FSourceWatchSession::HandleChanges(const TArray<FFileChangeData>& Changes)
	{
		PendingChanges.Append(Changes);
	}

	void FSourceWatchSession::ProcessPendingChanges()
	{
		if (PendingChanges.IsEmpty()) { return; }
		const TArray<FFileChangeData> Changes = MoveTemp(PendingChanges);
		PendingChanges.Reset();
		ProcessChanges(Changes);
	}

	void FSourceWatchSession::ProcessChanges(const TArray<FFileChangeData>& Changes)
	{
		TArray<FString> Known;
		GetKnownFiles(Known);
		TArray<FString> ChangedFiles, ChangedDirectories;
		for (const FFileChangeData& Change : Changes)
		{
			const FString File = FPaths::ConvertRelativePathToFull(Change.Filename);
			// Unknown/rescan actions are directory invalidations too. This also supports engine
			// versions predating the named FCA_RescanRequired enumerator.
			const bool bRescan = Change.Action != FFileChangeData::FCA_Added
				&& Change.Action != FFileChangeData::FCA_Removed && Change.Action != FFileChangeData::FCA_Modified;
			const bool bDirectory = IFileManager::Get().DirectoryExists(*File)
				|| Known.ContainsByPredicate([&](const FString& Entry)
				{
					// IsUnderDirectory(X, X) is true. A known source itself is not a directory;
					// only a strict descendant proves that a removed path used to be one.
					return !FPaths::IsSamePath(Entry, File) && FPaths::IsUnderDirectory(Entry, File);
				});
			if (bRescan || bDirectory)
			{
				if ((bRescan || Change.Action != FFileChangeData::FCA_Modified)
					&& (IsRootSource(File) || Roots.ContainsByPredicate([&](const FString& Root) { return InDirectory(Root, File); })
						|| Known.ContainsByPredicate([&](const FString& Entry) { return InDirectory(Entry, File); })))
				{
					ChangedDirectories.AddUnique(File);
				}
			}
			else if (FDreamFXPaths::IsSourceFile(File) && (IsRootSource(File) || ContainsPath(Known, File)))
			{
				ChangedFiles.AddUnique(File);
			}
		}
		if (!ChangedFiles.IsEmpty() || !ChangedDirectories.IsEmpty()) { Invalidate(MoveTemp(ChangedFiles), ChangedDirectories); }
	}

	void FSourceWatchSession::InvalidateFile(const FString& File)
	{
		Invalidate({ FPaths::ConvertRelativePathToFull(File) }, {});
	}
}
