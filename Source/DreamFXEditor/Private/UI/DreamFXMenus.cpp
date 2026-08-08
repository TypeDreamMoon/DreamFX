#include "DreamFXMenus.h"

#include "DreamFXAssetCommands.h"

#include "ContentBrowserMenuContexts.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"

#define LOCTEXT_NAMESPACE "DreamFXMenus"

namespace UE::DreamFX::Editor
{
	namespace
	{
		const FName DreamFXMenuOwner(TEXT("DreamFXEditor"));

		FDelegateHandle GStartupCallbackHandle;
		bool bGMenusRegistered = false;

		FSlateIcon Icon(const TCHAR* Name)
		{
			return FSlateIcon(FAppStyle::GetAppStyleSetName(), Name);
		}

		/**
		 * The DreamFX submenu for one system, in whichever of its two states applies.
		 *
		 * A generated asset and an imported one need opposite things: the first wants a way back to its
		 * text, the second wants a way *into* text. Showing both sets at once would offer *Adopt* on an
		 * asset that already has a source, which is the one command that must never run twice.
		 */
		void PopulateSystemMenu(UToolMenu* Menu, TWeakObjectPtr<UNiagaraSystem> WeakSystem)
		{
			UNiagaraSystem* System = WeakSystem.Get();
			if (System == nullptr)
			{
				return;
			}

			if (FDreamFXCommands::HasProvenance(System))
			{
				FToolMenuSection& Section = Menu->FindOrAddSection(
					TEXT("DreamFX.SourceActions"), LOCTEXT("SourceSection", "Source"));

				Section.AddMenuEntry(
					TEXT("DreamFX.OpenSource"),
					LOCTEXT("OpenSourceLabel", "Open Source"),
					LOCTEXT("OpenSourceTooltip", "Open the .dfs this asset was generated from, in VSCode."),
					Icon(TEXT("Icons.OpenInExternalEditor")),
					FUIAction(FExecuteAction::CreateLambda([WeakSystem]()
					{
						FDreamFXCommands::OpenSource(WeakSystem.Get());
					})));

				Section.AddMenuEntry(
					TEXT("DreamFX.RebuildFromSource"),
					LOCTEXT("RebuildFromSourceLabel", "Rebuild from Source"),
					LOCTEXT("RebuildFromSourceTooltip", "Rebuild this asset from its .dfs, as if the file had just been saved."),
					Icon(TEXT("Icons.Refresh")),
					FUIAction(FExecuteAction::CreateLambda([WeakSystem]()
					{
						FDreamFXCommands::RebuildFromSource(WeakSystem.Get());
					})));

				Section.AddMenuEntry(
					TEXT("DreamFX.VerifyAsset"),
					LOCTEXT("VerifyAssetLabel", "Verify"),
					LOCTEXT("VerifyAssetTooltip", "Check this asset against its .dfs without writing anything."),
					Icon(TEXT("Icons.Adjust")),
					FUIAction(FExecuteAction::CreateLambda([WeakSystem]()
					{
						FDreamFXCommands::VerifyAsset(WeakSystem.Get());
					})));
				return;
			}

			FToolMenuSection& Section = Menu->FindOrAddSection(
				TEXT("DreamFX.DecompileActions"), LOCTEXT("DecompileSection", "Decompiler"));

			Section.AddMenuEntry(
				TEXT("DreamFX.ExportDfs"),
				LOCTEXT("ExportDfsLabel", "Export .dfs"),
				LOCTEXT("ExportDfsTooltip", "Export this Niagara System to a DreamFXLang .dfs source file and open it. The asset is not modified; anything the language cannot express is listed at the top of the file."),
				Icon(TEXT("Icons.Save")),
				FUIAction(FExecuteAction::CreateLambda([WeakSystem]()
				{
					FDreamFXCommands::ExportSystem(WeakSystem.Get());
				})));

			Section.AddMenuEntry(
				TEXT("DreamFX.Adopt"),
				LOCTEXT("AdoptLabel", "Adopt (take over as a DFX source)"),
				LOCTEXT("AdoptTooltip", "Write this asset's source into the real DFX root, rebuild the asset from it, and verify the round trip. From then on the text is the source of truth. Refused when anything about the asset cannot be expressed, or when another .dfs already generates it."),
				Icon(TEXT("Icons.Import")),
				FUIAction(FExecuteAction::CreateLambda([WeakSystem]()
				{
					FDreamFXCommands::AdoptSystem(WeakSystem.Get());
				})));
		}

		void AddSystemSubMenu(FToolMenuSection& Section, UNiagaraSystem* System)
		{
			Section.AddSubMenu(
				TEXT("DreamFX.SystemActions"),
				LOCTEXT("SystemActionsLabel", "DreamFX"),
				LOCTEXT("SystemActionsTooltip", "DreamFX actions for this Niagara System. Requires exactly one selected asset."),
				FNewToolMenuDelegate::CreateStatic(&PopulateSystemMenu, TWeakObjectPtr<UNiagaraSystem>(System)),
				false,
				Icon(TEXT("Icons.Settings")));
		}

		/**
		 * The emitter submenu: one entry, because a standalone emitter has no round trip.
		 *
		 * A .dfe describes something that is copied into a system rather than something that generates
		 * an asset, so there is nothing to stamp with provenance, nothing to rebuild, and nothing to
		 * verify against -- Export is the whole surface.
		 */
		void PopulateEmitterMenu(UToolMenu* Menu, TWeakObjectPtr<UNiagaraEmitter> WeakEmitter)
		{
			if (!WeakEmitter.IsValid())
			{
				return;
			}

			FToolMenuSection& Section = Menu->FindOrAddSection(
				TEXT("DreamFX.DecompileActions"), LOCTEXT("DecompileSection", "Decompiler"));

			Section.AddMenuEntry(
				TEXT("DreamFX.ExportDfe"),
				LOCTEXT("ExportDfeLabel", "Export .dfe"),
				LOCTEXT("ExportDfeTooltip", "Export this emitter to a DreamFXLang .dfe source file and open it. The emitter is read through a throwaway host system, so what comes back is what it contributes when a system uses it."),
				Icon(TEXT("Icons.Save")),
				FUIAction(FExecuteAction::CreateLambda([WeakEmitter]()
				{
					FDreamFXCommands::ExportEmitter(WeakEmitter.Get());
				})));
		}

		void PopulateEmitterAssetMenu(FToolMenuSection& InSection)
		{
			const UContentBrowserAssetContextMenuContext* Context =
				UContentBrowserAssetContextMenuContext::FindContextWithAssets(InSection);
			if (Context == nullptr || Context->SelectedAssets.Num() != 1)
			{
				return;
			}

			UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context->SelectedAssets[0].GetAsset());
			if (Emitter == nullptr)
			{
				return;
			}

			InSection.AddSubMenu(
				TEXT("DreamFX.EmitterActions"),
				LOCTEXT("EmitterActionsLabel", "DreamFX"),
				LOCTEXT("EmitterActionsTooltip", "DreamFX actions for this Niagara Emitter. Requires exactly one selected asset."),
				FNewToolMenuDelegate::CreateStatic(&PopulateEmitterMenu, TWeakObjectPtr<UNiagaraEmitter>(Emitter)),
				false,
				Icon(TEXT("Icons.Settings")));
		}

		/** Content Browser right-click. Single selection only, same limit DreamShader ships. */
		void PopulateSystemAssetMenu(FToolMenuSection& InSection)
		{
			const UContentBrowserAssetContextMenuContext* Context =
				UContentBrowserAssetContextMenuContext::FindContextWithAssets(InSection);
			if (Context == nullptr || Context->SelectedAssets.Num() != 1)
			{
				return;
			}

			// GetAsset() loads the package. It is the only way to read provenance and decide which of
			// the two states applies, and by right-click time the asset is usually resident anyway.
			UNiagaraSystem* System = Cast<UNiagaraSystem>(Context->SelectedAssets[0].GetAsset());
			if (System == nullptr)
			{
				return;
			}

			AddSystemSubMenu(InSection, System);
		}

		/**
		 * Niagara system editor toolbar.
		 *
		 * `AssetEditor.Niagara.ToolBar` is shared: FNiagaraSystemToolkit, FNiagaraSimCacheToolkit,
		 * FNiagaraParameterDefinitionsToolkit, FNiagaraParameterCollectionToolkit and
		 * FNiagaraStatelessEmitterTemplateToolkit all return the toolkit name "Niagara". So the first
		 * step is to find a UNiagaraSystem among the edited objects and add nothing when there is none
		 * -- otherwise a DreamFX button appears on a sim cache, where every entry would misfire.
		 */
		void PopulateSystemEditorToolbar(FToolMenuSection& InSection)
		{
			const UAssetEditorToolkitMenuContext* Context = InSection.FindContext<UAssetEditorToolkitMenuContext>();
			if (Context == nullptr)
			{
				return;
			}

			UNiagaraSystem* System = nullptr;
			for (UObject* Object : Context->GetEditingObjects())
			{
				if (UNiagaraSystem* Candidate = Cast<UNiagaraSystem>(Object))
				{
					System = Candidate;
					break;
				}
			}

			if (System == nullptr)
			{
				return;
			}

			InSection.AddEntry(FToolMenuEntry::InitComboButton(
				TEXT("DreamFX.SystemToolbarMenu"),
				FUIAction(),
				FNewToolMenuChoice(FNewToolMenuDelegate::CreateStatic(
					&PopulateSystemMenu, TWeakObjectPtr<UNiagaraSystem>(System))),
				LOCTEXT("SystemToolbarLabel", "DreamFX"),
				LOCTEXT("SystemToolbarTooltip", "DreamFX actions for this Niagara System."),
				Icon(TEXT("Icons.Settings"))));
		}

		void RegisterMenusInternal()
		{
			if (bGMenusRegistered || IsEngineExitRequested() || GExitPurge || UToolMenus::Get() == nullptr)
			{
				return;
			}
			bGMenusRegistered = true;

			FToolMenuOwnerScoped MenuOwner(DreamFXMenuOwner);

			// --- Tools menu -------------------------------------------------------------------
			if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
			{
				FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("DreamFX"));
				Section.AddMenuEntry(
					TEXT("DreamFX.RebuildAll"),
					LOCTEXT("RebuildAllLabel", "Rebuild DFX"),
					LOCTEXT("RebuildAllTooltip", "Rebuild every DreamFX .dfs, .dfe and .dfm source file, as if each had just been saved."),
					Icon(TEXT("Icons.Refresh")),
					FUIAction(FExecuteAction::CreateStatic(&FDreamFXCommands::RebuildAll)));
				Section.AddMenuEntry(
					TEXT("DreamFX.VerifyAll"),
					LOCTEXT("VerifyAllLabel", "Verify DFX"),
					LOCTEXT("VerifyAllTooltip", "Check every generated asset against its source without writing anything."),
					Icon(TEXT("Icons.Adjust")),
					FUIAction(FExecuteAction::CreateStatic(&FDreamFXCommands::VerifyAll)));
				Section.AddMenuEntry(
					TEXT("DreamFX.OpenWorkspace"),
					LOCTEXT("OpenWorkspaceLabel", "Open DreamFX Workspace (VSCode)"),
					LOCTEXT("OpenWorkspaceTooltip", "Rewrite DFX/DreamFX.code-workspace from the current source roots and open it in VSCode, or Notepad if VSCode is unavailable."),
					Icon(TEXT("Icons.OpenInExternalEditor")),
					FUIAction(FExecuteAction::CreateStatic(&FDreamFXCommands::OpenWorkspace)));
			}

			// --- Level Editor toolbar ---------------------------------------------------------
			if (UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(
				TEXT("LevelEditor.LevelEditorToolBar.AssetsToolBar")))
			{
				FToolMenuSection& Section = ToolbarMenu->FindOrAddSection(TEXT("DreamFX"));
				Section.AddEntry(FToolMenuEntry::InitToolBarButton(
					TEXT("DreamFX.RebuildAllToolbar"),
					FUIAction(FExecuteAction::CreateStatic(&FDreamFXCommands::RebuildAll)),
					LOCTEXT("RebuildAllToolbarLabel", "DFX"),
					LOCTEXT("RebuildAllToolbarTooltip", "Rebuild every DreamFX source file."),
					Icon(TEXT("Icons.Refresh"))));
				Section.AddEntry(FToolMenuEntry::InitToolBarButton(
					TEXT("DreamFX.OpenWorkspaceToolbar"),
					FUIAction(FExecuteAction::CreateStatic(&FDreamFXCommands::OpenWorkspace)),
					LOCTEXT("OpenWorkspaceToolbarLabel", "Open DreamFX Workspace (VSCode)"),
					LOCTEXT("OpenWorkspaceToolbarTooltip", "Open the DreamFX source workspace in VSCode, or Notepad if VSCode is unavailable."),
					Icon(TEXT("Icons.OpenInExternalEditor"))));
			}

			// --- Content Browser right-click --------------------------------------------------
			if (UToolMenu* SystemAssetMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(
				UNiagaraSystem::StaticClass()))
			{
				FToolMenuSection& Section = SystemAssetMenu->FindOrAddSection(TEXT("GetAssetActions"));
				Section.AddDynamicEntry(
					TEXT("DreamFX.SystemAssetActions"),
					FNewToolMenuSectionDelegate::CreateStatic(&PopulateSystemAssetMenu));
			}

			if (UToolMenu* EmitterAssetMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(
				UNiagaraEmitter::StaticClass()))
			{
				FToolMenuSection& Section = EmitterAssetMenu->FindOrAddSection(TEXT("GetAssetActions"));
				Section.AddDynamicEntry(
					TEXT("DreamFX.EmitterAssetActions"),
					FNewToolMenuSectionDelegate::CreateStatic(&PopulateEmitterAssetMenu));
			}

			// --- Niagara system editor toolbar ------------------------------------------------
			if (UToolMenu* SystemEditorToolbar = UToolMenus::Get()->ExtendMenu(TEXT("AssetEditor.Niagara.ToolBar")))
			{
				FToolMenuSection& Section = SystemEditorToolbar->FindOrAddSection(TEXT("DreamFX"));
				Section.AddDynamicEntry(
					TEXT("DreamFX.SystemEditorToolbarActions"),
					FNewToolMenuSectionDelegate::CreateStatic(&PopulateSystemEditorToolbar));
			}
		}
	}

	void FDreamFXMenus::Register()
	{
		// UToolMenus may not exist yet at module startup, and the menus it extends certainly do not.
		// The startup callback is the engine's own answer to that ordering.
		GStartupCallbackHandle = UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenusInternal));
	}

	void FDreamFXMenus::Unregister()
	{
		if (GStartupCallbackHandle.IsValid())
		{
			UToolMenus::UnRegisterStartupCallback(GStartupCallbackHandle);
			GStartupCallbackHandle.Reset();
		}

		if (!IsEngineExitRequested() && !GExitPurge && UToolMenus::Get() != nullptr)
		{
			UToolMenus::Get()->UnregisterOwnerByName(DreamFXMenuOwner);
		}

		bGMenusRegistered = false;
	}
}

#undef LOCTEXT_NAMESPACE
