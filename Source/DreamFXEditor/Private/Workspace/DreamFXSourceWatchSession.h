#pragma once

#include "CoreMinimal.h"
#include "IDirectoryWatcher.h"
#include "Workspace/DreamFXSourceDependencies.h"

class IDirectoryWatcher;
struct FFileChangeData;

namespace UE::DreamFX::Editor
{
	/** Directory subscriptions and dependency invalidation, independent of the UI/build queue. */
	class FSourceWatchSession
	{
	public:
		FSourceWatchSession(IDirectoryWatcher& InWatcher, TFunction<void(const TSet<FString>&)> InQueueSources);
		~FSourceWatchSession();
		/** Module shutdown can destroy the borrowed watcher before this session. */
		void Stop(bool bWatcherAvailable = true);
		void Refresh(const TArray<FString>& SourceDirectories, bool bQueueNewSources = true);
		void HandleChanges(const TArray<FFileChangeData>& Changes);
		/** Drain after IDirectoryWatcher::Tick; never edit subscriptions inside their own callback. */
		void ProcessPendingChanges();
		void InvalidateFile(const FString& File);

		/**
		 * Current subscriptions. Recursive watches cover the DFX roots only; everything else -- the
		 * directory holding each root, external dependency directories and their parents -- is a
		 * shallow watch that reports direct children. A recursive watch above a root would deliver
		 * every write under the project (Saved, Intermediate, DerivedDataCache) to this session.
		 */
		void GetWatches(TArray<FString>& OutRecursive, TArray<FString>& OutShallow) const;

	private:
		struct FWatchSpec
		{
			FString Directory;
			bool bRecursive = false;
		};

		void Reindex();
		void ProcessChanges(const TArray<FFileChangeData>& Changes);
		void ReconcileWatches();
		void Invalidate(TArray<FString> ChangedFiles, const TArray<FString>& ChangedDirectories);
		void GetKnownFiles(TArray<FString>& Out) const;
		TArray<FWatchSpec> DesiredWatches() const;
		bool IsRootSource(const FString& File) const;
		TMap<FString, FDelegateHandle>& HandlesFor(bool bRecursive) { return bRecursive ? RecursiveHandles : ShallowHandles; }

		IDirectoryWatcher& Watcher;
		TFunction<void(const TSet<FString>&)> QueueSources;
		TMap<FString, FDelegateHandle> RecursiveHandles;
		TMap<FString, FDelegateHandle> ShallowHandles;
		/** Watched directories that were themselves added, removed or renamed and must be re-subscribed. */
		TSet<FString> StaleWatches;
		TArray<FString> Roots;
		TArray<FString> SourceFiles;
		TArray<FFileChangeData> PendingChanges;
		FSourceDependencyIndex Dependencies;
		bool bInitialized = false;
	};
}
