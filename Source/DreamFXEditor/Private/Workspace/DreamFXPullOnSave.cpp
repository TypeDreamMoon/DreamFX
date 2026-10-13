#include "Workspace/DreamFXPullOnSave.h"

#include "DreamFXModule.h"
#include "Generation/DreamFXProvenance.h"
#include "Settings/DreamFXEditorSettings.h"
#include "UI/DreamFXAssetCommands.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Depth of FSelfSaveScope. Saves run on the game thread, so no atomics. */
		int32 GPullOnSaveSelfDepth = 0;

		FDelegateHandle GPullOnSaveHandle;
		FTSTicker::FDelegateHandle GPullOnSaveTicker;

		/** Source file -> the system saved for it. A set, so a Save All that names one system twice pulls once. */
		TMap<FString, TWeakObjectPtr<UNiagaraSystem>> GPullOnSavePending;

		TFunction<void(UNiagaraSystem*, const FString&)> GPullOnSaveObserver;
		bool GPullOnSaveDrivenByTest = false;

		bool RunPending(float /*DeltaTime*/)
		{
			GPullOnSaveTicker.Reset();

			TMap<FString, TWeakObjectPtr<UNiagaraSystem>> Work = MoveTemp(GPullOnSavePending);
			GPullOnSavePending.Reset();

			for (const TPair<FString, TWeakObjectPtr<UNiagaraSystem>>& Entry : Work)
			{
				if (UNiagaraSystem* System = Entry.Value.Get())
				{
					// Quiet when there was nothing to write: this runs on every save of every
					// generated system, and most of those saves tuned nothing.
					FDreamFXCommands::PullIntoSource(System, /*bQuietWhenUnchanged=*/true);
				}
			}

			return false; // one-shot
		}

		void Schedule(UNiagaraSystem* System, const FString& SourceFile)
		{
			if (GPullOnSaveObserver)
			{
				GPullOnSaveObserver(System, SourceFile);
				return;
			}

			GPullOnSavePending.Add(SourceFile, System);
			if (!GPullOnSaveTicker.IsValid())
			{
				GPullOnSaveTicker = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateStatic(&RunPending), /*InDelay=*/0.0f);
			}
		}

		void OnPackageSaved(const FString& /*FileName*/, UPackage* Package, FObjectPostSaveContext SaveContext)
		{
			if (Package == nullptr || !IsInGameThread())
			{
				return;
			}

			const UDreamFXEditorSettings* Settings = GetDefault<UDreamFXEditorSettings>();

			FPullOnSaveFacts Facts;
			Facts.bSelfSave = FPullOnSave::IsSelfSaving();
			Facts.bSettingOn = Settings != nullptr && Settings->bPullOnAssetSave;
			Facts.bCommandlet = IsRunningCommandlet();
			Facts.bAutomation = GIsAutomationTesting;
			Facts.bProceduralSave = SaveContext.IsProceduralSave() || SaveContext.IsCooking();
			Facts.bAutoSave = SaveContext.IsFromAutoSave();
			Facts.bPlaySession = GEditor != nullptr && GEditor->PlayWorld != nullptr;
			Facts.bDrivenByTest = GPullOnSaveDrivenByTest;

			const EPullOnSaveSkip Skip = FPullOnSave::Decide(Facts);
			if (Skip != EPullOnSaveSkip::None)
			{
				if (Skip == EPullOnSaveSkip::SelfSave)
				{
					UE_LOG(LogDreamFX, VeryVerbose, TEXT("Write back on save: %s (%s)"),
						FPullOnSave::Describe(Skip), *Package->GetName());
				}
				return;
			}

			// A package holds its asset at the top level; nested objects are never the system.
			TArray<UNiagaraSystem*, TInlineAllocator<1>> Systems;
			ForEachObjectWithPackage(Package, [&Systems](UObject* Object)
			{
				if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Object))
				{
					Systems.Add(System);
				}
				return true;
			}, EGetObjectsFlags::None);

			for (UNiagaraSystem* System : Systems)
			{
				// The same resolution the menu commands use: the stamp, then the file it names.
				FProvenanceStamp Stamp;
				FString SourceFile;
				if (!FProvenance::Read(System, Stamp) || Stamp.SourceFullPath.IsEmpty()
					|| !FProvenance::ResolveSourceFile(Stamp, SourceFile)
					|| !FPaths::GetExtension(SourceFile).Equals(TEXT("dfs"), ESearchCase::IgnoreCase))
				{
					continue;
				}

				Schedule(System, SourceFile);
			}
		}
	}

	void FPullOnSave::Register()
	{
		if (!GPullOnSaveHandle.IsValid())
		{
			GPullOnSaveHandle = UPackage::PackageSavedWithContextEvent.AddStatic(&OnPackageSaved);
		}
	}

	void FPullOnSave::Unregister()
	{
		if (GPullOnSaveHandle.IsValid())
		{
			UPackage::PackageSavedWithContextEvent.Remove(GPullOnSaveHandle);
			GPullOnSaveHandle.Reset();
		}
		if (GPullOnSaveTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GPullOnSaveTicker);
			GPullOnSaveTicker.Reset();
		}
		GPullOnSavePending.Reset();
	}

	EPullOnSaveSkip FPullOnSave::Decide(const FPullOnSaveFacts& Facts)
	{
		// First, and for everyone: this is the rule that keeps a build and a pull from feeding each other.
		if (Facts.bSelfSave)
		{
			return EPullOnSaveSkip::SelfSave;
		}
		if (!Facts.bDrivenByTest)
		{
			if (!Facts.bSettingOn)
			{
				return EPullOnSaveSkip::SettingOff;
			}
			if (Facts.bAutomation)
			{
				return EPullOnSaveSkip::Automation;
			}
		}
		if (Facts.bCommandlet)
		{
			return EPullOnSaveSkip::Commandlet;
		}
		if (Facts.bProceduralSave)
		{
			return EPullOnSaveSkip::ProceduralSave;
		}
		if (Facts.bAutoSave)
		{
			return EPullOnSaveSkip::AutoSave;
		}
		if (Facts.bPlaySession)
		{
			return EPullOnSaveSkip::PlaySession;
		}
		return EPullOnSaveSkip::None;
	}

	const TCHAR* FPullOnSave::Describe(const EPullOnSaveSkip Skip)
	{
		switch (Skip)
		{
		case EPullOnSaveSkip::SelfSave:       return TEXT("DreamFX saved this itself");
		case EPullOnSaveSkip::SettingOff:     return TEXT("write-back on save is off");
		case EPullOnSaveSkip::Commandlet:     return TEXT("running a commandlet");
		case EPullOnSaveSkip::Automation:     return TEXT("an automation test is running");
		case EPullOnSaveSkip::ProceduralSave: return TEXT("not a save by the author (cook or procedural)");
		case EPullOnSaveSkip::AutoSave:       return TEXT("an autosave");
		case EPullOnSaveSkip::PlaySession:    return TEXT("the editor is playing");
		default:                              return TEXT("none");
		}
	}

	FPullOnSave::FSelfSaveScope::FSelfSaveScope()
	{
		++GPullOnSaveSelfDepth;
	}

	FPullOnSave::FSelfSaveScope::~FSelfSaveScope()
	{
		--GPullOnSaveSelfDepth;
	}

	bool FPullOnSave::IsSelfSaving()
	{
		return GPullOnSaveSelfDepth > 0;
	}

	FPullOnSave::FTestScope::FTestScope(TFunction<void(UNiagaraSystem*, const FString&)> InObserver)
	{
		// The module registers the hook in an interactive editor; a test must not depend on that.
		if (!GPullOnSaveHandle.IsValid())
		{
			Register();
			bRegisteredHere = true;
		}
		GPullOnSaveObserver = MoveTemp(InObserver);
		GPullOnSaveDrivenByTest = true;
	}

	FPullOnSave::FTestScope::~FTestScope()
	{
		GPullOnSaveObserver = nullptr;
		GPullOnSaveDrivenByTest = false;
		if (bRegisteredHere)
		{
			Unregister();
		}
	}
}
