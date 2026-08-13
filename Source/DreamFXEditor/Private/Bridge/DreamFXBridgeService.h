#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * A drop-folder RPC channel between an external editor and this Unreal Editor.
	 *
	 * It exists to remove a footgun, not to add convenience. `dfx build` writes packages, and so does
	 * a running editor; when both save the same package the later save silently wins and the earlier
	 * work is gone with no error anywhere. Before this, "the editor is open" -- the single most common
	 * state to be in -- meant there was *no* safe way to build from outside. Handing the build to the
	 * editor process itself removes the second writer entirely.
	 *
	 * The convenience is real too: the engine is already loaded, so a request costs what the work
	 * costs rather than the work plus a 13-second boot.
	 *
	 * Files, not a socket: no port to choose, no firewall prompt, no handshake, and a request written
	 * while the editor is closed simply waits on disk instead of failing to connect. Everything lives
	 * under `<Project>/Saved/DreamFX/Bridge/`, which is already outside version control.
	 */
	class FBridgeService
	{
	public:
		/** Starts polling for requests and publishing the heartbeat. */
		static void Register();

		/** Stops polling and marks the channel closed, so a client does not wait on a dead editor. */
		static void Unregister();

		/** `<Project>/Saved/DreamFX/Bridge`. */
		static FString GetBridgeDirectory();
	};
}
