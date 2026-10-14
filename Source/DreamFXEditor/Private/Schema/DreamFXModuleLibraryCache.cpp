#include "Schema/DreamFXModuleLibraryCache.h"

#include "DreamFXModule.h"
#include "Schema/DreamFXModuleLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"
#include "NiagaraScript.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/PackageReload.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

#include <atomic>

namespace UE::DreamFX::Editor
{
	namespace
	{
		/**
		 * More than one because a source can name its own `ModulePaths`, and a project that does
		 * alternates between a few of them. Not many, because each keeps a rooted probe system alive.
		 */
		constexpr int32 MaxLibraries = 4;

		struct FEntry
		{
			FString Key;
			TSharedRef<FModuleLibrary> Library;
		};

		struct FState
		{
			/** Most recently used first. */
			TArray<FEntry> Entries;

			/**
			 * Set by a notification that cannot drop the entries itself because it is not on the game
			 * thread, where the drop would release a GC root. The next Get pays it.
			 */
			std::atomic<bool> bDropOwed{ false };

			bool bHooked = false;
			FTopLevelAssetPath ScriptClassPath;

			FDelegateHandle PackageSaved;
			FDelegateHandle PackageMarkedDirty;
			FDelegateHandle PackageReloaded;
			FDelegateHandle ObjectsReplaced;
			FDelegateHandle AssetAdded;
			FDelegateHandle AssetRemoved;
			FDelegateHandle AssetRenamed;
			FDelegateHandle AssetUpdated;
		};

		FState& GetState()
		{
			static FState State;
			return State;
		}

		/**
		 * Whether the package is a Niagara module, dynamic input or function asset.
		 *
		 * Top-level only. A system or emitter package holds a dozen scripts of its own -- the spawn and
		 * update scripts, scratch pad modules -- but those are not addressable by name, so no library
		 * ever resolved one. Counting them would drop every library on each build's own save of the
		 * system it just made, which is the one write this cache has to survive.
		 */
		bool HoldsScript(const UPackage* Package)
		{
			if (Package == nullptr)
			{
				return false;
			}

			bool bFound = false;
			ForEachObjectWithPackage(Package, [&bFound](UObject* Object)
			{
				if (Object->IsA<UNiagaraScript>())
				{
					bFound = true;
					return false;
				}
				return true;
			}, EGetObjectsFlags::None);
			return bFound;
		}

		void Drop(const TCHAR* Reason, const FString& Subject)
		{
			FState& State = GetState();
			if (!IsInGameThread())
			{
				State.bDropOwed = true;
				return;
			}

			State.bDropOwed = false;
			if (State.Entries.Num() == 0)
			{
				return;
			}

			UE_LOG(LogDreamFX, Verbose, TEXT("Dropping %d shared module librar%s: %s%s%s"),
				State.Entries.Num(), State.Entries.Num() == 1 ? TEXT("y") : TEXT("ies"), Reason,
				Subject.IsEmpty() ? TEXT("") : TEXT(" "), *Subject);

			// Anyone mid-build holds a reference of their own, so this releases the cache's share only.
			State.Entries.Reset();
		}

		void OnPackageSaved(const FString& FileName, UPackage* Package, FObjectPostSaveContext SaveContext)
		{
			if (HoldsScript(Package))
			{
				Drop(TEXT("a Niagara script was saved:"), Package->GetName());
			}
		}

		void OnPackageMarkedDirty(UPackage* Package, bool bWasDirty)
		{
			// A script edited in the editor and not yet saved is what the next build would probe, so
			// waiting for the save would leave a window in which the cache answers for the old module.
			if (HoldsScript(Package))
			{
				Drop(TEXT("a Niagara script was modified:"), Package->GetName());
			}
		}

		void OnPackageReloaded(EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
		{
			if (Event != nullptr && (HoldsScript(Event->GetOldPackage()) || HoldsScript(Event->GetNewPackage())))
			{
				Drop(TEXT("a Niagara script was reloaded:"),
					Event->GetOldPackage() != nullptr ? Event->GetOldPackage()->GetName() : FString());
			}
		}

		void OnObjectsReplaced(const FCoreUObjectDelegates::FReplacementObjectMap& Replacements)
		{
			for (const TPair<UObject*, UObject*>& Pair : Replacements)
			{
				if ((Pair.Key != nullptr && Pair.Key->IsA<UNiagaraScript>())
					|| (Pair.Value != nullptr && Pair.Value->IsA<UNiagaraScript>()))
				{
					Drop(TEXT("a Niagara script was replaced."), FString());
					return;
				}
			}
		}

		void OnAssetChanged(const FAssetData& Asset, const TCHAR* What)
		{
			// Ignored for any other class: the registry reports every asset in the project, and a
			// scan at startup reports all of them.
			if (Asset.AssetClassPath == GetState().ScriptClassPath)
			{
				Drop(What, Asset.PackageName.ToString());
			}
		}

		void EnsureHooked()
		{
			FState& State = GetState();
			if (State.bHooked)
			{
				return;
			}
			State.bHooked = true;

			State.ScriptClassPath = UNiagaraScript::StaticClass()->GetClassPathName();

			State.PackageSaved = UPackage::PackageSavedWithContextEvent.AddStatic(&OnPackageSaved);
			State.PackageMarkedDirty = UPackage::PackageMarkedDirtyEvent.AddStatic(&OnPackageMarkedDirty);
			State.PackageReloaded = FCoreUObjectDelegates::OnPackageReloaded.AddStatic(&OnPackageReloaded);
			State.ObjectsReplaced = FCoreUObjectDelegates::OnObjectsReplaced.AddStatic(&OnObjectsReplaced);

			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			State.AssetAdded = Registry.OnAssetAdded().AddLambda(
				[](const FAssetData& Asset) { OnAssetChanged(Asset, TEXT("a Niagara script asset was added:")); });
			State.AssetRemoved = Registry.OnAssetRemoved().AddLambda(
				[](const FAssetData& Asset) { OnAssetChanged(Asset, TEXT("a Niagara script asset was removed:")); });
			State.AssetRenamed = Registry.OnAssetRenamed().AddLambda(
				[](const FAssetData& Asset, const FString&) { OnAssetChanged(Asset, TEXT("a Niagara script asset was renamed:")); });
			State.AssetUpdated = Registry.OnAssetUpdated().AddLambda(
				[](const FAssetData& Asset) { OnAssetChanged(Asset, TEXT("a Niagara script asset was updated:")); });
		}

		void Unhook()
		{
			FState& State = GetState();
			if (!State.bHooked)
			{
				return;
			}
			State.bHooked = false;

			UPackage::PackageSavedWithContextEvent.Remove(State.PackageSaved);
			UPackage::PackageMarkedDirtyEvent.Remove(State.PackageMarkedDirty);
			FCoreUObjectDelegates::OnPackageReloaded.Remove(State.PackageReloaded);
			FCoreUObjectDelegates::OnObjectsReplaced.Remove(State.ObjectsReplaced);

			// The registry module may be gone already when the engine is tearing down, and asking for
			// it then would load it again just to unsubscribe from something that no longer exists.
			if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
			{
				IAssetRegistry& Registry = Module->Get();
				Registry.OnAssetAdded().Remove(State.AssetAdded);
				Registry.OnAssetRemoved().Remove(State.AssetRemoved);
				Registry.OnAssetRenamed().Remove(State.AssetRenamed);
				Registry.OnAssetUpdated().Remove(State.AssetUpdated);
			}

			State.PackageSaved.Reset();
			State.PackageMarkedDirty.Reset();
			State.PackageReloaded.Reset();
			State.ObjectsReplaced.Reset();
			State.AssetAdded.Reset();
			State.AssetRemoved.Reset();
			State.AssetRenamed.Reset();
			State.AssetUpdated.Reset();
		}

		/**
		 * The search paths a library will actually use, spelled so that equal sets give equal keys.
		 *
		 * The defaults are part of the set: a source that lists `/Niagara/Modules` itself asks for the
		 * same library as one that lists nothing.
		 */
		FString MakeKey(const TArray<FString>& SearchPaths)
		{
			TArray<FString> Normalized;
			auto AddPath = [&Normalized](FString Path)
			{
				Path.TrimStartAndEndInline();
				while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
				{
					Path.LeftChopInline(1);
				}
				if (!Path.IsEmpty())
				{
					Normalized.AddUnique(MoveTemp(Path));
				}
			};

			for (const FString& Path : FModuleLibrary::GetDefaultSearchPaths())
			{
				AddPath(Path);
			}
			for (const FString& Path : SearchPaths)
			{
				AddPath(Path);
			}

			Normalized.Sort();
			return FString::Join(Normalized, TEXT("\n"));
		}
	}

	TSharedRef<FModuleLibrary> FModuleLibraryCache::Get(const TArray<FString>& SearchPaths)
	{
		FState& State = GetState();
		EnsureHooked();

		if (State.bDropOwed.exchange(false))
		{
			State.Entries.Reset();
		}

		// A library whose cached answers point at objects that have since been destroyed cannot be
		// trusted to hand out the rest of them; see FModuleLibrary::HasStaleReferences.
		State.Entries.RemoveAll([](const FEntry& Entry) { return Entry.Library->HasStaleReferences(); });

		const FString Key = MakeKey(SearchPaths);
		const int32 Existing = State.Entries.IndexOfByPredicate([&Key](const FEntry& Entry) { return Entry.Key == Key; });
		if (Existing != INDEX_NONE)
		{
			if (Existing != 0)
			{
				State.Entries.Swap(0, Existing);
			}
			return State.Entries[0].Library;
		}

		TSharedRef<FModuleLibrary> Library = MakeShared<FModuleLibrary>();
		if (SearchPaths.Num() > 0)
		{
			Library->SetSearchPaths(SearchPaths);
		}

		State.Entries.Insert(FEntry{ Key, Library }, 0);
		if (State.Entries.Num() > MaxLibraries)
		{
			State.Entries.SetNum(MaxLibraries);
		}
		return Library;
	}

	void FModuleLibraryCache::Invalidate(const TCHAR* Reason)
	{
		Drop(Reason, FString());
	}

	void FModuleLibraryCache::Shutdown()
	{
		Unhook();

		FState& State = GetState();
		State.bDropOwed = false;
		State.Entries.Reset();
	}
}
