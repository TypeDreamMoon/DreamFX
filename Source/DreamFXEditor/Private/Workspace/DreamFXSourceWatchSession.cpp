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
		/** The directory itself when it exists, otherwise its nearest existing ancestor. */
		FString NearestExistingDirectory(FString Directory)
		{
			while (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
			{
				const FString Parent = ParentDirectory(Directory);
				if (Parent == Directory) { return FString(); }
				Directory = Parent;
			}
			return Directory;
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
			for (const auto& Entry : RecursiveHandles) { Watcher.UnregisterDirectoryChangedCallback_Handle(Entry.Key, Entry.Value); }
			for (const auto& Entry : ShallowHandles) { Watcher.UnregisterDirectoryChangedCallback_Handle(Entry.Key, Entry.Value); }
		}
		RecursiveHandles.Reset();
		ShallowHandles.Reset();
		StaleWatches.Reset();
		PendingChanges.Reset();
	}

	void FSourceWatchSession::GetWatches(TArray<FString>& OutRecursive, TArray<FString>& OutShallow) const
	{
		RecursiveHandles.GetKeys(OutRecursive);
		ShallowHandles.GetKeys(OutShallow);
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

	TArray<FSourceWatchSession::FWatchSpec> FSourceWatchSession::DesiredWatches() const
	{
		// Roots are the only recursive subscriptions. A nested root is already covered by its parent.
		TArray<FString> Recursive;
		for (const FString& Root : Roots)
		{
			if (IFileManager::Get().DirectoryExists(*Root)) { Recursive.AddUnique(Root); }
		}
		Recursive.Sort([](const FString& A, const FString& B) { return A.Len() < B.Len(); });
		TArray<FString> RecursiveResult;
		for (const FString& Directory : Recursive)
		{
			if (!RecursiveResult.ContainsByPredicate([&](const FString& Existing) { return InDirectory(Directory, Existing); }))
			{
				RecursiveResult.Add(Directory);
			}
		}

		// Everything else needs only direct children: a file inside a dependency directory, or a
		// watched directory (a root, a dependency directory) being created, removed, renamed or
		// replaced inside its parent. A missing directory is watched through its nearest existing
		// ancestor, whose direct child is the next directory to appear; the subscription then moves
		// one level down on the following reconcile.
		TArray<FString> Shallow;
		auto AddShallow = [&](const FString& Directory)
		{
			const FString Existing = NearestExistingDirectory(Directory);
			if (!Existing.IsEmpty()
				&& !RecursiveResult.ContainsByPredicate([&](const FString& Covered) { return InDirectory(Existing, Covered); }))
			{
				Shallow.AddUnique(Existing);
			}
		};
		for (const FString& Root : Roots) { AddShallow(ParentDirectory(Root)); }
		TArray<FString> Referenced;
		Dependencies.GetReferencedFiles(Referenced);
		for (const FString& File : Referenced)
		{
			if (IsRootSource(File)) { continue; }
			const FString Directory = ParentDirectory(File);
			AddShallow(Directory);
			if (IFileManager::Get().DirectoryExists(*Directory)) { AddShallow(ParentDirectory(Directory)); }
		}

		TArray<FWatchSpec> Result;
		for (const FString& Directory : RecursiveResult) { Result.Add({ Directory, true }); }
		for (const FString& Directory : Shallow) { Result.Add({ Directory, false }); }
		return Result;
	}

	void FSourceWatchSession::ReconcileWatches()
	{
		// A watched directory that was replaced (moved away and another moved in, or deleted and
		// recreated) leaves its OS handle on the old directory object. Subscribe it afresh.
		for (const FString& Stale : StaleWatches)
		{
			for (const bool bRecursive : { true, false })
			{
				TMap<FString, FDelegateHandle>& Handles = HandlesFor(bRecursive);
				if (const FDelegateHandle* Handle = Handles.Find(Stale))
				{
					Watcher.UnregisterDirectoryChangedCallback_Handle(Stale, *Handle);
					Handles.Remove(Stale);
				}
			}
		}
		StaleWatches.Reset();

		const TArray<FWatchSpec> Desired = DesiredWatches();
		// Register replacements first so expanding/collapsing watch coverage does not leave a gap.
		bool bAllRegistered = true;
		for (const FWatchSpec& Spec : Desired)
		{
			TMap<FString, FDelegateHandle>& Handles = HandlesFor(Spec.bRecursive);
			if (Handles.Contains(Spec.Directory)) { continue; }
			const uint32 Flags = IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges
				| (Spec.bRecursive ? 0 : IDirectoryWatcher::WatchOptions::IgnoreChangesInSubtree);
			FDelegateHandle Handle;
			if (Watcher.RegisterDirectoryChangedCallback_Handle(Spec.Directory,
				IDirectoryWatcher::FDirectoryChanged::CreateRaw(this, &FSourceWatchSession::HandleChanges), Handle, Flags))
			{
				Handles.Add(Spec.Directory, Handle);
			}
			else { bAllRegistered = false; }
		}
		if (!bAllRegistered) { return; } // Keep existing coverage when a replacement cannot be registered.
		for (const bool bRecursive : { true, false })
		{
			for (auto It = HandlesFor(bRecursive).CreateIterator(); It; ++It)
			{
				const bool bWanted = Desired.ContainsByPredicate([&](const FWatchSpec& Spec)
				{
					return Spec.bRecursive == bRecursive && FPaths::IsSamePath(Spec.Directory, It.Key());
				});
				if (!bWanted)
				{
					Watcher.UnregisterDirectoryChangedCallback_Handle(It.Key(), It.Value());
					It.RemoveCurrent();
				}
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
		for (const bool bRecursive : { true, false })
		{
			for (const auto& Entry : HandlesFor(bRecursive))
			{
				if (!IFileManager::Get().DirectoryExists(*Entry.Key)) { ChangedDirectories.AddUnique(Entry.Key); }
			}
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

			if (bRescan || Change.Action != FFileChangeData::FCA_Modified)
			{
				for (const bool bRecursive : { true, false })
				{
					for (const auto& Entry : HandlesFor(bRecursive))
					{
						if (InDirectory(Entry.Key, File)) { StaleWatches.Add(Entry.Key); }
					}
				}
			}

			// Decide relevance from paths alone before touching the disk: a shallow watch still
			// reports unrelated siblings, and none of them may cost a filesystem query.
			const bool bUnderRoot = IsRootSource(File)
				|| Roots.ContainsByPredicate([&](const FString& Root) { return InDirectory(Root, File); });
			const bool bKnownOrAncestor = Known.ContainsByPredicate([&](const FString& Entry) { return InDirectory(Entry, File); });
			if (!bRescan && !bUnderRoot && !bKnownOrAncestor) { continue; }

			const bool bDirectory = IFileManager::Get().DirectoryExists(*File)
				|| Known.ContainsByPredicate([&](const FString& Entry)
				{
					// IsUnderDirectory(X, X) is true. A known source itself is not a directory;
					// only a strict descendant proves that a removed path used to be one.
					return !FPaths::IsSamePath(Entry, File) && FPaths::IsUnderDirectory(Entry, File);
				});
			if (bRescan || bDirectory)
			{
				if ((bRescan || Change.Action != FFileChangeData::FCA_Modified) && (bUnderRoot || bKnownOrAncestor))
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
		else if (!StaleWatches.IsEmpty()) { ReconcileWatches(); }
	}

	void FSourceWatchSession::InvalidateFile(const FString& File)
	{
		Invalidate({ FPaths::ConvertRelativePathToFull(File) }, {});
	}
}
