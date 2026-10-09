#include "WriteBack/DreamFXSourceValue.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Generation/DreamFXValueLowering.h"

#include "Dom/JsonObject.h"
#include "UObject/StructOnScope.h"
#include "NiagaraCommon.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** A number as the language spells it: integral values as integers, the rest losslessly. */
		FString NumberToSource(double Number)
		{
			// JSON has one number type, so an int32 property comes back as 1337.0 and would re-import as
			// a float -- which L7 then rejects. Integral values print as integers.
			return FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
				? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)))
				: FormatFloatLossless(static_cast<float>(Number));
		}
	}

	FString ReferenceToPackagePath(const FString& ReferencePath)
	{
		FString PackagePath = ReferencePath;
		int32 Dot = INDEX_NONE;
		if (PackagePath.FindLastChar(TEXT('.'), Dot))
		{
			PackagePath.LeftInline(Dot);
		}
		return PackagePath;
	}

	TSharedPtr<FJsonValue> FindJsonPropertyByPath(const TSharedPtr<FJsonObject>& Object, const FString& Path)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() <= 1)
		{
			return Object->TryGetField(Path);
		}

		TSharedPtr<FJsonObject> Current = Object;
		for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
		{
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (!Current->TryGetObjectField(Segments[Index], Child) || Child == nullptr || !Child->IsValid())
			{
				return nullptr;
			}
			Current = *Child;
		}
		return Current->TryGetField(Segments.Last());
	}

	bool TryReadReferenceObject(const TSharedPtr<FJsonValue>& Value, FString& OutPackagePath)
	{
		FString ReferencePath;
		if (Value.IsValid() && Value->Type == EJson::Object && Value->AsObject().IsValid()
			&& Value->AsObject()->TryGetStringField(TEXT("refPath"), ReferencePath)
			&& ReferencePath.StartsWith(TEXT("/")))
		{
			OutPackagePath = ReferenceToPackagePath(ReferencePath);
			return true;
		}
		return false;
	}

	FString QuoteSourceString(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
		Value.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
		Value.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
		Value.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
		return FString::Printf(TEXT("\"%s\""), *Value);
	}

	bool JsonTextToSourceString(const FString& JsonText, FString& OutLiteral)
	{
		// Re-serialised rather than passed through, so the same value always produces the same bytes
		// however the engine happened to format it -- which is what keeps a re-export of the mirror
		// identical to the export it came from.
		TSharedPtr<FJsonValue> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			return false;
		}

		FString Compact;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Compact);

		if (Parsed->Type == EJson::Object)
		{
			if (!FJsonSerializer::Serialize(Parsed->AsObject().ToSharedRef(), Writer))
			{
				return false;
			}
		}
		else if (Parsed->Type == EJson::Array)
		{
			if (!FJsonSerializer::Serialize(Parsed->AsArray(), Writer))
			{
				return false;
			}
		}
		else
		{
			return false;
		}

		if (Compact.IsEmpty() || Compact.Contains(TEXT("\n")) || Compact.Contains(TEXT("\r")))
		{
			return false;
		}

		OutLiteral = QuoteSourceString(Compact);
		return true;
	}

	bool TryWriteJsonBlob(const TSharedPtr<FJsonValue>& Value, FString& OutLiteral)
	{
		if (!Value.IsValid() || (Value->Type != EJson::Object && Value->Type != EJson::Array))
		{
			return false;
		}

		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);

		const bool bSerialized = Value->Type == EJson::Object
			? FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer)
			: FJsonSerializer::Serialize(Value->AsArray(), Writer);

		return bSerialized && JsonTextToSourceString(Json, OutLiteral);
	}

	bool TryWriteNumberTuple(const FString& PropertyName, const TSharedPtr<FJsonObject>& Object,
		FString& OutLiteral)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		static const TCHAR* const Xy[]   = { TEXT("X"), TEXT("Y") };
		static const TCHAR* const Xyz[]  = { TEXT("X"), TEXT("Y"), TEXT("Z") };
		static const TCHAR* const Xyzw[] = { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") };
		static const TCHAR* const Rgba[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

		const bool bGeneratorWouldUseRgba = PropertyName.Contains(TEXT("Color"));

		const TCHAR* const* Names = nullptr;
		switch (Object->Values.Num())
		{
		case 2: Names = Xy; break;
		case 3: Names = Xyz; break;
		case 4:
			Names = Object->HasField(TEXT("R")) ? Rgba : Xyzw;
			if ((Names == Rgba) != bGeneratorWouldUseRgba)
			{
				return false;
			}
			break;
		default: return false;
		}

		const int32 Count = Object->Values.Num();
		TArray<FString> Parts;
		Parts.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const TSharedPtr<FJsonValue> Field = Object->TryGetField(Names[Index]);
			if (!Field.IsValid() || Field->Type != EJson::Number)
			{
				return false;
			}
			Parts.Add(NumberToSource(Field->AsNumber()));
		}

		OutLiteral = FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(", ")));
		return true;
	}

	/**
	 * Each element's remaining fields are compared against a default-constructed element first, so an
	 * entry with a custom pivot or scale is refused instead of being flattened away.
	 */
	bool TryWriteReferenceArray(const UClass* RendererClass, const FString& Key,
		const TArray<TSharedPtr<FJsonValue>>& Elements, FString& OutSource)
	{
		if (Elements.Num() == 0)
		{
			return false;
		}

		FString ReferenceField;
		FString ElementDefaultsJson;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetArrayElementReferenceField(
			RendererClass, Key, ReferenceField, ElementDefaultsJson, Errors))
		{
			return false;
		}

		TSharedPtr<FJsonObject> ElementDefaults;
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ElementDefaultsJson);
			FJsonSerializer::Deserialize(Reader, ElementDefaults);
		}

		TArray<FString> Paths;
		bool bDroppedField = false;

		for (const TSharedPtr<FJsonValue>& Element : Elements)
		{
			if (!Element.IsValid() || Element->Type != EJson::Object || !Element->AsObject().IsValid())
			{
				return false;
			}

			const TSharedPtr<FJsonObject> Object = Element->AsObject();

			FString PackagePath;
			if (!TryReadReferenceObject(Object->TryGetField(ReferenceField), PackagePath))
			{
				// An element whose reference is unset is not representable as a path, and writing an
				// empty string would import as "no asset" on a different element index.
				return false;
			}
			Paths.Add(PackagePath);

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
			{
				if (Field.Key == ReferenceField || !Field.Value.IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonValue> Default = ElementDefaults.IsValid()
					? ElementDefaults->TryGetField(Field.Key) : nullptr;
				if (!Default.IsValid() || !FJsonValue::CompareEqual(*Field.Value, *Default))
				{
					bDroppedField = true;
				}
			}
		}

		if (bDroppedField)
		{
			return false;
		}

		TArray<FString> Quoted;
		Quoted.Reserve(Paths.Num());
		for (const FString& Path : Paths)
		{
			Quoted.Add(FString::Printf(TEXT("\"%s\""), *Path));
		}
		OutSource = FString::Printf(TEXT("[%s]"), *FString::Join(Quoted, TEXT(", ")));
		return true;
	}

	namespace
	{
		bool NormalizeUserParameterBinding(TSharedPtr<FJsonValue>& Value,
			const TSharedPtr<FJsonValue>& Default)
		{
			const TSharedPtr<FJsonObject>* Parameter = nullptr;
			const TSharedPtr<FJsonObject>* DefaultParameter = nullptr;
			if (!Value.IsValid() || Value->Type != EJson::Object
				|| !Default.IsValid() || Default->Type != EJson::Object
				|| !Value->AsObject()->TryGetObjectField(TEXT("parameter"), Parameter)
				|| !Default->AsObject()->TryGetObjectField(TEXT("parameter"), DefaultParameter))
			{
				return false;
			}
			FString Name;
			const TSharedPtr<FJsonValue> Type = (*Parameter)->TryGetField(TEXT("typeDefHandle"));
			const TSharedPtr<FJsonValue> DefaultType = (*DefaultParameter)->TryGetField(TEXT("typeDefHandle"));
			if (!(*Parameter)->TryGetStringField(TEXT("name"), Name) || !Type.IsValid() || !DefaultType.IsValid()
				|| !FJsonValue::CompareEqual(*Type, *DefaultType))
			{
				return false;
			}
			// Niagara serializes TypeDefHandle as a process-local registry index. These bindings
			// have a fixed renderer-defined type (MaterialInterface for Sprite/Ribbon); import only
			// the name so the fresh renderer keeps that type in every editor session.
			const TSharedRef<FJsonObject> StableParameter = MakeShared<FJsonObject>();
			StableParameter->SetStringField(TEXT("name"), Name);
			const TSharedRef<FJsonObject> StableBinding = MakeShared<FJsonObject>();
			StableBinding->SetObjectField(TEXT("parameter"), StableParameter);
			Value = MakeShared<FJsonValueObject>(StableBinding);
			return true;
		}

		/** Normalize bindings at every struct/array depth, including mesh OverrideMaterials. */
		bool NormalizeRendererBindings(const FProperty* Property, const void* DefaultData,
			TSharedPtr<FJsonValue>& Value)
		{
			if (Property == nullptr || !Value.IsValid()) { return true; }
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				if (Struct->Struct == FNiagaraUserParameterBinding::StaticStruct())
				{
					return DefaultData != nullptr && NormalizeUserParameterBinding(Value,
						FJsonObjectConverter::UPropertyToJsonValue(const_cast<FProperty*>(Property), DefaultData));
				}
				if (Value->Type != EJson::Object) { return true; }
				for (auto& Field : Value->AsObject()->Values)
				{
					const FProperty* Member = FindFProperty<FProperty>(Struct->Struct, FName(*Field.Key));
					if (!NormalizeRendererBindings(Member,
						Member != nullptr && DefaultData != nullptr ? Member->ContainerPtrToValuePtr<void>(DefaultData) : nullptr,
						Field.Value)) { return false; }
				}
			}
			else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
				if (Inner != nullptr && Value->Type == EJson::Array)
				{
					// Import constructs fresh elements, even when the renderer CDO array is empty.
					FStructOnScope ElementDefaults(Inner->Struct);
					TArray<TSharedPtr<FJsonValue>> Elements = Value->AsArray();
					for (TSharedPtr<FJsonValue>& Element : Elements)
					{
						if (!NormalizeRendererBindings(Inner, ElementDefaults.GetStructMemory(), Element)) { return false; }
					}
					Value = MakeShared<FJsonValueArray>(MoveTemp(Elements));
				}
			}
			return true;
		}
	}

	bool NormalizeRendererPropertyBindings(const UClass* RendererClass, const FString& Key, TSharedPtr<FJsonValue>& Value)
	{
		const FProperty* Property = RendererClass != nullptr ? FindFProperty<FProperty>(RendererClass, FName(*Key)) : nullptr;
		return NormalizeRendererBindings(Property,
			Property != nullptr ? Property->ContainerPtrToValuePtr<void>(RendererClass->GetDefaultObject()) : nullptr,
			Value);
	}

	bool RenderJsonPropertyAsSource(const UClass* PropertyClass, const FString& Key,
		const TSharedPtr<FJsonValue>& Value, FString& OutSource, FString& OutWhy)
	{
		OutSource.Reset();
		OutWhy.Reset();

		if (!Value.IsValid() || Value->IsNull())
		{
			OutWhy = TEXT("the asset holds null there");
			return false;
		}

		switch (Value->Type)
		{
		case EJson::Boolean:
			OutSource = Value->AsBool() ? TEXT("true") : TEXT("false");
			return true;

		case EJson::Number:
			OutSource = NumberToSource(Value->AsNumber());
			return true;

		case EJson::String:
		{
			const FString Raw = Value->AsString();
			// A path is quoted; an enumerator name or a bare word is not. Both spellings exist in the
			// DSL, and which one a value needs is decided by the value itself.
			OutSource = Raw.StartsWith(TEXT("/")) ? FString::Printf(TEXT("\"%s\""), *Raw) : Raw;
			return true;
		}

		case EJson::Object:
		{
			// An asset reference first: it is the shape that carries an object, and a tuple or a blob
			// could also in principle match a `{"X":..,"Y":..}`.
			FString PackagePath;
			if (TryReadReferenceObject(Value, PackagePath))
			{
				OutSource = FString::Printf(TEXT("\"%s\""), *PackagePath);
				return true;
			}

			if (TryWriteNumberTuple(Key, Value->AsObject(), OutSource))
			{
				return true;
			}

			if (TryWriteJsonBlob(Value, OutSource))
			{
				return true;
			}

			OutWhy = TEXT("it is a structured value with no settled spelling of its own");
			return false;
		}

		case EJson::Array:
		{
			if (PropertyClass != nullptr
				&& TryWriteReferenceArray(PropertyClass, Key, Value->AsArray(), OutSource))
			{
				return true;
			}

			if (TryWriteJsonBlob(Value, OutSource))
			{
				return true;
			}

			OutWhy = TEXT("it is an array with no settled spelling of its own");
			return false;
		}

		default:
			OutWhy = TEXT("it is a value type this language does not spell");
			return false;
		}
	}
}
