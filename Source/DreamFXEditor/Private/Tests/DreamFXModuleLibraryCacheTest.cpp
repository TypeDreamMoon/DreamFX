#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "Schema/DreamFXModuleLibraryCache.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSharedModuleLibraryTest,
	"DreamFX.Regression.Schema.SharedModuleLibrary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSharedModuleLibraryTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;

	FModuleLibraryCache::Invalidate(TEXT("the test starts from nothing"));

	const TArray<FString> Extra = { TEXT("/Game/DreamFXAutomation/ModulesA"), TEXT("/Game/DreamFXAutomation/ModulesB") };

	const TSharedRef<FModuleLibrary> Library = FModuleLibraryCache::Get(Extra);
	const uint32 Serial = Library->GetSerial();

	// Same set, same library -- including when the set is spelled in another order, with a trailing
	// slash, or with the built-in defaults written out, because none of those change what resolves.
	TestEqual(TEXT("the same paths reuse the library"), FModuleLibraryCache::Get(Extra)->GetSerial(), Serial);
	TestEqual(TEXT("order does not matter"),
		FModuleLibraryCache::Get({ Extra[1], Extra[0] })->GetSerial(), Serial);
	TestEqual(TEXT("a trailing slash does not matter"),
		FModuleLibraryCache::Get({ Extra[0] + TEXT("/"), Extra[1] })->GetSerial(), Serial);
	TestEqual(TEXT("spelling out the defaults does not matter"),
		FModuleLibraryCache::Get({ Extra[0], Extra[1], TEXT("/Niagara/Modules") })->GetSerial(), Serial);

	// A different set resolves names differently and must not share.
	const uint32 OtherSerial = FModuleLibraryCache::Get({ Extra[0] })->GetSerial();
	TestNotEqual(TEXT("a different path set gets its own library"), OtherSerial, Serial);
	TestEqual(TEXT("and keeps it"), FModuleLibraryCache::Get({ Extra[0] })->GetSerial(), OtherSerial);
	TestEqual(TEXT("no extra paths is the default set"),
		FModuleLibraryCache::Get({})->GetSerial(), FModuleLibraryCache::Get(FModuleLibrary::GetDefaultSearchPaths())->GetSerial());
	TestEqual(TEXT("the first set is still there after the others"), FModuleLibraryCache::Get(Extra)->GetSerial(), Serial);

	// What the library learned survives to the next build: the probe is the expensive part, and the
	// schema it returns lives at one address for as long as the library does.
	{
		FModuleLibraryCache::Invalidate(TEXT("measuring the probe on a clean library"));
		const TSharedRef<FModuleLibrary> Probing = FModuleLibraryCache::Get({});

		FString Error;
		UNiagaraScript* Module = Probing->FindModule(TEXT("ScaleColor"), Error);
		if (TestNotNull(TEXT("ScaleColor module"), Module))
		{
			const FModuleSchema* First = Probing->GetStackSchema(Module, EStackKind::ParticleUpdate, Error);
			if (TestNotNull(TEXT("probed schema"), First))
			{
				const TSharedRef<FModuleLibrary> Again = FModuleLibraryCache::Get({});
				TestEqual(TEXT("the next build gets the same library"), Again->GetSerial(), Probing->GetSerial());
				TestTrue(TEXT("and the schema it already probed"),
					Again->GetStackSchema(Module, EStackKind::ParticleUpdate, Error) == First);
			}
		}
	}

	// A script saved, edited or replaced makes every cached answer suspect.
	UPackage* ScriptPackage = CreatePackage(TEXT("/Temp/DreamFXAutomation/SharedLibraryScript"));
	UNiagaraScript* Script = NewObject<UNiagaraScript>(ScriptPackage, TEXT("SharedLibraryScript"), RF_Public | RF_Standalone);
	UNiagaraScript* Replacement = NewObject<UNiagaraScript>(ScriptPackage, TEXT("SharedLibraryScriptReplacement"), RF_Public | RF_Standalone);

	uint32 Previous = FModuleLibraryCache::Get(Extra)->GetSerial();
	FModuleLibraryCache::Invalidate(TEXT("the test asks for it"));
	TestNotEqual(TEXT("an explicit invalidation gives a fresh library"), FModuleLibraryCache::Get(Extra)->GetSerial(), Previous);

	Previous = FModuleLibraryCache::Get(Extra)->GetSerial();
	ScriptPackage->MarkPackageDirty();
	TestNotEqual(TEXT("modifying a script drops the library"), FModuleLibraryCache::Get(Extra)->GetSerial(), Previous);

	Previous = FModuleLibraryCache::Get(Extra)->GetSerial();
	{
		FCoreUObjectDelegates::FReplacementObjectMap Replaced;
		Replaced.Add(Script, Replacement);
		FCoreUObjectDelegates::OnObjectsReplaced.Broadcast(Replaced);
	}
	TestNotEqual(TEXT("replacing a script drops the library"), FModuleLibraryCache::Get(Extra)->GetSerial(), Previous);

	// A system package holds scripts of its own, but not addressable ones: a build saves one on every
	// run and has to keep what it learned.
	Previous = FModuleLibraryCache::Get(Extra)->GetSerial();
	{
		TArray<FString> Errors;
		bool bCreated = false;
		UNiagaraSystem* System = FNiagaraAdapter::AcquireSystem(
			TEXT("/Temp/DreamFXAutomation"), TEXT("SharedLibrarySystem"), bCreated, Errors);
		if (TestNotNull(TEXT("system"), System))
		{
			System->GetOutermost()->MarkPackageDirty();
		}
	}
	TestEqual(TEXT("modifying a system leaves the library alone"), FModuleLibraryCache::Get(Extra)->GetSerial(), Previous);

	FModuleLibraryCache::Shutdown();
	TestNotEqual(TEXT("shutdown releases the libraries"), FModuleLibraryCache::Get(Extra)->GetSerial(), Previous);

	// The first library is still usable by whoever held it across the drops.
	TestTrue(TEXT("a held library outlives its eviction"), Library->GetSearchPaths().Contains(Extra[0]));

	FModuleLibraryCache::Invalidate(TEXT("leaving nothing rooted behind"));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
