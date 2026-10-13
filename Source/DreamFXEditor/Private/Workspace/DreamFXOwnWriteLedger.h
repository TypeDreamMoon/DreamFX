#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Source files the editor wrote itself a moment ago, and what it wrote into them.
	 *
	 * Write-back changes a .dfs from inside the process that is also watching the .dfs, so the watcher
	 * would read the edit as the author saving and rebuild the asset the values just came from. The
	 * ledger lets the writer say "this content, in this file, is mine" and the watcher ask about a
	 * change before it queues one.
	 *
	 * What decides a notification is mine is the file's content, not the fact that one arrived: the
	 * author can edit the file again a second later, and that edit must still rebuild. So an entry holds
	 * a hash, and a change counts as own only while the file on disk still hashes to it.
	 *
	 * Several notifications can follow one write (the OS reports a write more than once, and the
	 * watcher batches by tick), and none may follow it either. So a match does not consume the entry:
	 * it stands until it expires, which swallows every notification of that one write and leaves
	 * nothing behind when there were none. A different content ends it at once, since that is the
	 * author's edit and the entry has nothing more to say about it.
	 *
	 * Pure state, no clock and no disk of its own: the caller passes the time and a way to hash the
	 * file, which is what lets the rules be tested without either.
	 */
	class FOwnWriteLedger
	{
	public:
		/**
		 * Long enough to outlast an editor hitch between the write and the tick that sees the change,
		 * short enough that the entry cannot swallow a later save of the same text by someone else --
		 * and if one does, the asset already holds exactly that text's values.
		 */
		static constexpr double DefaultLifetimeSeconds = 10.0;

		/** The spelling of a content hash the ledger compares. */
		static FString HashBytes(const TArray<uint8>& Bytes);

		/** Hashes a file as it is now. False when it cannot be read. */
		static bool HashFile(const FString& File, FString& OutHash);

		/** Records that `File` now holds the content with this hash, replacing any earlier entry for it. */
		void Expect(const FString& File, const FString& ContentHash, double Now,
			double LifetimeSeconds = DefaultLifetimeSeconds);

		/**
		 * True when a change to `File` is the write recorded for it.
		 *
		 * `HashNow` is only called when a live entry exists, so a file nobody wrote costs nothing. It
		 * returns false when the file cannot be read, which ends the entry: a file that is gone is not
		 * the content that was written.
		 */
		bool IsOwnWrite(const FString& File, double Now, TFunctionRef<bool(FString&)> HashNow);

		/** Entries still alive at `Now`. */
		int32 Num(double Now);

		void Reset() { Entries.Reset(); }

	private:
		struct FEntry
		{
			FString ContentHash;
			double ExpiresAt = 0.0;
		};

		static FString KeyFor(const FString& File);
		void Purge(double Now);

		TMap<FString, FEntry> Entries;
	};
}
