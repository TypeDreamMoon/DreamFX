#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;

namespace UE::DreamFX::Editor
{
	/** Why a package save does not start a write-back. `None` means it does. */
	enum class EPullOnSaveSkip : uint8
	{
		None,
		/** DreamFX made this save itself, after a build. */
		SelfSave,
		/** `bPullOnAssetSave` is off. */
		SettingOff,
		Commandlet,
		/** An automation test is running; only a test that drives the hook itself asks for a pull. */
		Automation,
		/** Cook or another save nobody edited anything for. */
		ProceduralSave,
		AutoSave,
		/** The editor is playing: the asset in memory may be mid-simulation state, and nothing was saved by hand. */
		PlaySession,
	};

	/** Everything the decision depends on, so the rules can be checked without an editor around them. */
	struct FPullOnSaveFacts
	{
		bool bSelfSave = false;
		bool bSettingOn = false;
		bool bCommandlet = false;
		bool bAutomation = false;
		bool bProceduralSave = false;
		bool bAutoSave = false;
		bool bPlaySession = false;
		/** A test calls the hook on purpose: the setting and the automation skip are not what it is about. */
		bool bDrivenByTest = false;
	};

	/**
	 * Writes an asset's values back into its .dfs when the author saves the asset (`bPullOnAssetSave`).
	 *
	 * The package-saved event is the trigger and nothing else is done inside it: saving is in the
	 * middle of the engine's own bookkeeping, and a pull loads packages and writes a file. The event
	 * only notes which source files to visit, and the work happens on the next core tick.
	 *
	 * The hard part is not starting a pull from DreamFX's own saves. A build ends by saving the system
	 * it generated; if that save started a pull, every rebuild would be followed by a write-back of
	 * the values it had just written, into the file it had just been built from. So every DreamFX
	 * save goes through FSelfSaveScope, and the hook declines while one is open.
	 */
	class FPullOnSave
	{
	public:
		static void Register();
		static void Unregister();

		/** The rules, in the order they apply. Pure. */
		static EPullOnSaveSkip Decide(const FPullOnSaveFacts& Facts);

		/** A sentence for the log. */
		static const TCHAR* Describe(EPullOnSaveSkip Skip);

		/**
		 * Open around a save DreamFX makes itself. Nests, and only the game thread saves, so a plain
		 * counter is enough.
		 */
		struct FSelfSaveScope
		{
			FSelfSaveScope();
			~FSelfSaveScope();
			FSelfSaveScope(const FSelfSaveScope&) = delete;
			FSelfSaveScope& operator=(const FSelfSaveScope&) = delete;
		};

		static bool IsSelfSaving();

		/**
		 * Replaces the pull with a callback for the lifetime of the object, and treats the hook as driven
		 * by a test: the setting is not consulted and an automation run does not suppress it. The self-save
		 * rule stays exactly as it is, which is the one the tests are about.
		 *
		 * The callback sees what the hook would have scheduled -- the system and its source file.
		 */
		class FTestScope
		{
		public:
			explicit FTestScope(TFunction<void(UNiagaraSystem*, const FString&)> InObserver);
			~FTestScope();
			FTestScope(const FTestScope&) = delete;
			FTestScope& operator=(const FTestScope&) = delete;

		private:
			bool bRegisteredHere = false;
		};
	};
}
