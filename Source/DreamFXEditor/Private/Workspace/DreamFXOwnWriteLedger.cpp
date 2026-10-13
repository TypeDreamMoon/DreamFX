#include "Workspace/DreamFXOwnWriteLedger.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace UE::DreamFX::Editor
{
	FString FOwnWriteLedger::HashBytes(const TArray<uint8>& Bytes)
	{
		return FMD5::HashBytes(Bytes.GetData(), Bytes.Num());
	}

	bool FOwnWriteLedger::HashFile(const FString& File, FString& OutHash)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent))
		{
			return false;
		}
		OutHash = HashBytes(Bytes);
		return true;
	}

	FString FOwnWriteLedger::KeyFor(const FString& File)
	{
		// The directory watcher and the writer spell the same file differently (relative or full,
		// back or forward slashes, drive letter case), and Windows does not tell them apart.
		FString Key = FPaths::ConvertRelativePathToFull(File);
		FPaths::NormalizeFilename(Key);
		return Key.ToLower();
	}

	void FOwnWriteLedger::Purge(const double Now)
	{
		for (auto It = Entries.CreateIterator(); It; ++It)
		{
			if (It.Value().ExpiresAt <= Now)
			{
				It.RemoveCurrent();
			}
		}
	}

	void FOwnWriteLedger::Expect(const FString& File, const FString& ContentHash, const double Now,
		const double LifetimeSeconds)
	{
		Purge(Now);

		FEntry& Entry = Entries.FindOrAdd(KeyFor(File));
		Entry.ContentHash = ContentHash;
		Entry.ExpiresAt = Now + LifetimeSeconds;
	}

	bool FOwnWriteLedger::IsOwnWrite(const FString& File, const double Now, TFunctionRef<bool(FString&)> HashNow)
	{
		Purge(Now);

		const FString Key = KeyFor(File);
		const FEntry* Entry = Entries.Find(Key);
		if (Entry == nullptr)
		{
			return false;
		}

		FString Current;
		if (!HashNow(Current) || Current != Entry->ContentHash)
		{
			Entries.Remove(Key);
			return false;
		}

		return true;
	}

	int32 FOwnWriteLedger::Num(const double Now)
	{
		Purge(Now);
		return Entries.Num();
	}
}
