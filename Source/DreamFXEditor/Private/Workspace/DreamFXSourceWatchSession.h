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

	private:
		void Reindex();
		void ProcessChanges(const TArray<FFileChangeData>& Changes);
		void ReconcileWatches();
		void Invalidate(TArray<FString> ChangedFiles, const TArray<FString>& ChangedDirectories);
		void GetKnownFiles(TArray<FString>& Out) const;
		TArray<FString> DesiredWatchDirectories() const;
		bool IsRootSource(const FString& File) const;

		IDirectoryWatcher& Watcher;
		TFunction<void(const TSet<FString>&)> QueueSources;
		TMap<FString, FDelegateHandle> Handles;
		TArray<FString> Roots;
		TArray<FString> SourceFiles;
		TArray<FFileChangeData> PendingChanges;
		FSourceDependencyIndex Dependencies;
		bool bInitialized = false;
	};
}
