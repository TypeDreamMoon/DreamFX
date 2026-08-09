#include "Generation/DreamFXGraphSurgeon.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_Niagara.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "NiagaraCommon.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if DREAMFX_HAS_CUSTOMHLSL_WRITE
#include "NiagaraNodeParameterMapGet.h"
#include "NiagaraNodeParameterMapSet.h"
#endif

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** The two node classes that only exist in NiagaraEditor's private headers. */
		const TCHAR* const ParameterMapGetClassName = TEXT("NiagaraNodeParameterMapGet");
		const TCHAR* const ParameterMapSetClassName = TEXT("NiagaraNodeParameterMapSet");

		/**
		 * The sub-category Niagara stamps on a dynamic-pin node's "Add" pin.
		 *
		 * UNiagaraNodeWithDynamicPins::AddPinSubCategory holds this value and is public, but it is a
		 * static data member with no export macro, so naming it would not link off a stock engine. The
		 * literal is duplicated here instead and then checked against a pin the engine actually built,
		 * in the self-check -- a copied constant that is verified against its source is a different
		 * thing from a copied constant that is assumed.
		 */
		const TCHAR* const AddPinSubCategoryLiteral = TEXT("DynamicAddPin");

		FEdGraphPinType MakeAddPinType()
		{
			return FEdGraphPinType(UEdGraphSchema_Niagara::PinCategoryMisc, AddPinSubCategoryLiteral,
				nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
		}

		bool IsAddPin(const UEdGraphPin* Pin)
		{
			return Pin != nullptr
				&& Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc
				&& Pin->PinType.PinSubCategory == AddPinSubCategoryLiteral;
		}

		UEdGraphPin* FindAddPin(UNiagaraNode& Node, EEdGraphPinDirection Direction)
		{
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin != nullptr && Pin->Direction == Direction && IsAddPin(Pin))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		UClass* FindNiagaraEditorClass(const TCHAR* ClassName)
		{
			return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/NiagaraEditor.%s"), ClassName));
		}

		/**
		 * What FGraphNodeCreator does, for a class that is only known at runtime.
		 *
		 * The template is unusable here because it needs NodeType::StaticClass(), which a class without
		 * MinimalAPI does not export. Its two steps are reproduced instead: UEdGraph::CreateNode is
		 * protected and reachable only by the template, which is its friend, but the three lines that
		 * function contains are all public -- and everything the template's Finalize calls is a public
		 * virtual, so the construction sequence survives intact.
		 */
		UNiagaraNode* CreateAndFinalizeNode(UNiagaraGraph& Graph, UClass* NodeClass)
		{
			if (NodeClass == nullptr || !NodeClass->IsChildOf(UNiagaraNode::StaticClass()))
			{
				return nullptr;
			}

			UNiagaraNode* Node = NewObject<UNiagaraNode>(&Graph, NodeClass, NAME_None, RF_Transactional);
			if (Node == nullptr)
			{
				return nullptr;
			}
			if (Graph.HasAnyFlags(RF_Transient))
			{
				Node->SetFlags(RF_Transient);
			}
			Graph.AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/true);

			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			if (Node->Pins.Num() == 0)
			{
				Node->AllocateDefaultPins();
			}
			return Node;
		}
	}

	// -------------------------------------------------------------------------------------------
	// State 2 -- rebuilt from the public surface
	// -------------------------------------------------------------------------------------------

	/**
	 * Every operation here is the engine's own implementation, re-expressed in terms of things a
	 * stock engine does export. The bodies were read out of NiagaraEditor's sources rather than
	 * guessed, so each one carries a note saying what it mirrors and where it deliberately differs.
	 */
	class FReflectedGraphSurgeon final : public FGraphSurgeon
	{
	public:
		/** Resolves everything the backend depends on, or explains what is missing. */
		bool Initialize(FString& OutWhyNot)
		{
			ParameterMapGetClass = FindNiagaraEditorClass(ParameterMapGetClassName);
			ParameterMapSetClass = FindNiagaraEditorClass(ParameterMapSetClassName);
			if (ParameterMapGetClass == nullptr || ParameterMapSetClass == nullptr)
			{
				OutWhyNot = TEXT("the parameter map get/set node classes are not registered under /Script/NiagaraEditor");
				return false;
			}

			CustomHlslProperty = CastField<FStrProperty>(
				UNiagaraNodeCustomHlsl::StaticClass()->FindPropertyByName(TEXT("CustomHlsl")));
			if (CustomHlslProperty == nullptr)
			{
				OutWhyNot = TEXT("UNiagaraNodeCustomHlsl has no string property named 'CustomHlsl'");
				return false;
			}

			VariableToScriptVariableProperty = CastField<FMapProperty>(
				UNiagaraGraph::StaticClass()->FindPropertyByName(TEXT("VariableToScriptVariable")));
			ParameterToReferencesProperty = CastField<FMapProperty>(
				UNiagaraGraph::StaticClass()->FindPropertyByName(TEXT("ParameterToReferencesMap")));
			if (VariableToScriptVariableProperty == nullptr || ParameterToReferencesProperty == nullptr)
			{
				OutWhyNot = TEXT("UNiagaraGraph no longer carries both parameter maps as UPROPERTYs");
				return false;
			}

			CreatedByUserProperty = CastField<FBoolProperty>(
				FNiagaraGraphParameterReferenceCollection::StaticStruct()->FindPropertyByName(TEXT("bCreatedByUser")));
			if (CreatedByUserProperty == nullptr)
			{
				OutWhyNot = TEXT("FNiagaraGraphParameterReferenceCollection has no 'bCreatedByUser' property");
				return false;
			}

			return CheckAddPinSubCategory(OutWhyNot);
		}

		virtual UNiagaraNode* CreateParameterMapGet(UNiagaraGraph& Graph) override
		{
			return CreateAndFinalizeNode(Graph, ParameterMapGetClass);
		}

		virtual UNiagaraNode* CreateParameterMapSet(UNiagaraGraph& Graph) override
		{
			return CreateAndFinalizeNode(Graph, ParameterMapSetClass);
		}

		/**
		 * Mirrors UNiagaraNodeWithDynamicPins::RequestNewTypedPin.
		 *
		 * The engine's version turns the existing Add pin into the requested pin and then builds a new
		 * Add pin behind it, which is what puts the Add pin last; appending instead would reorder the
		 * node's pins and change the graph. CreateAddPin and OnNewTypedPinAdded are both protected, so
		 * the replacement Add pin is created directly and the one lasting effect of OnNewTypedPinAdded
		 * -- the custom HLSL node's function signature -- is rebuilt explicitly below.
		 *
		 * The engine also uniques and sanitises the incoming pin name. That is skipped: a .dfm's input
		 * and attribute names are validated identifiers by the time they reach here, and two pins on one
		 * node cannot collide because the declarations they come from cannot.
		 */
		virtual UEdGraphPin* AddTypedPin(UNiagaraNode& Node, EEdGraphPinDirection Direction,
			const FNiagaraTypeDefinition& Type, FName Name) override
		{
			const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
			if (Schema == nullptr)
			{
				return nullptr;
			}

			Node.Modify();

			UEdGraphPin* NewPin = FindAddPin(Node, Direction);
			if (NewPin != nullptr)
			{
				NewPin->Modify();
				NewPin->PinType = Schema->TypeDefinitionToPinType(Type);
				NewPin->PinName = Name;
				Node.CreatePin(Direction, MakeAddPinType(), TEXT("Add"));
			}
			else
			{
				NewPin = Node.CreatePin(Direction, Schema->TypeDefinitionToPinType(Type), Name);
			}

			if (UNiagaraNodeCustomHlsl* HlslNode = Cast<UNiagaraNodeCustomHlsl>(&Node))
			{
				RebuildSignatureFromPins(*HlslNode, *Schema);
			}

			Node.MarkNodeRequiresSynchronization(__FUNCTION__, /*bRaiseGraphNeedsRecompile=*/true);
			return NewPin;
		}

		/** Mirrors UNiagaraNodeCustomHlsl::SetCustomHlsl; only the field write needs reflection. */
		virtual void SetCustomHlsl(UNiagaraNodeCustomHlsl& Node, const FString& Hlsl) override
		{
			Node.Modify();
			CustomHlslProperty->SetPropertyValue_InContainer(&Node, Hlsl);
			Node.RefreshFromExternalChanges();

			// The engine guards this call the same way: before the node is in a graph there is nothing
			// to notify, and notifying anyway crashes.
			if (Node.GetOuter() != nullptr && Node.GetOuter()->IsA<UNiagaraGraph>())
			{
				Node.MarkNodeRequiresSynchronization(__FUNCTION__, /*bRaiseGraphNeedsRecompile=*/true);
			}
		}

		/**
		 * Mirrors UNiagaraNodeCustomHlsl::InitAsCustomHlslDynamicInput.
		 *
		 * The engine opens with ReallocatePins(), which is protected. On a node this new it reduces to
		 * re-running AllocateDefaultPins over an empty pin array, and the node creator has already done
		 * exactly that, so dropping it changes nothing. The two pins and the usage flag are the rest.
		 */
		virtual void InitAsDynamicInput(UNiagaraNodeCustomHlsl& Node, const FNiagaraTypeDefinition& OutputType) override
		{
			Node.Modify();
			AddTypedPin(Node, EGPD_Input, FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map"));
			AddTypedPin(Node, EGPD_Output, OutputType, TEXT("CustomHLSLOutput"));
			Node.ScriptUsage = ENiagaraScriptUsage::DynamicInput;
		}

		/**
		 * Mirrors UNiagaraGraph::AddParameter(Variable, MetaData, false, false) and, through it,
		 * CreateScriptVariableInternal.
		 *
		 * Both graph maps are private UPROPERTYs, so they are reached with FScriptMapHelper. The script
		 * variable itself needs no reflection: its class is MinimalAPI and Init is exported. The engine
		 * decides DefaultMode from the variable's namespace here; that is left alone because the module
		 * generator overwrites it immediately afterwards for every parameter it creates.
		 */
		virtual UNiagaraScriptVariable* AddParameter(UNiagaraGraph& Graph, const FNiagaraVariable& Variable,
			const FNiagaraVariableMetaData& MetaData) override
		{
			FScriptMapHelper References(ParameterToReferencesProperty,
				ParameterToReferencesProperty->ContainerPtrToValuePtr<void>(&Graph));
			if (References.FindValueFromHash(&Variable) == nullptr)
			{
				// The exported constructor for this struct takes the flag, but it is not inline and the
				// struct carries no export macro, so it is default-constructed and the flag is written
				// through its own UPROPERTY.
				FNiagaraGraphParameterReferenceCollection Collection;
				CreatedByUserProperty->SetPropertyValue_InContainer(&Collection, true);
				References.AddPair(&Variable, &Collection);
			}

			FScriptMapHelper ScriptVariables(VariableToScriptVariableProperty,
				VariableToScriptVariableProperty->ContainerPtrToValuePtr<void>(&Graph));
			if (uint8* ExistingValue = ScriptVariables.FindValueFromHash(&Variable))
			{
				return *reinterpret_cast<UNiagaraScriptVariable**>(ExistingValue);
			}

			Graph.Modify();

			UNiagaraScriptVariable* NewScriptVariable =
				NewObject<UNiagaraScriptVariable>(&Graph, FName(), RF_Transactional);
			NewScriptVariable->Init(Variable, MetaData);
			NewScriptVariable->SetIsStaticSwitch(false);

			TObjectPtr<UNiagaraScriptVariable> Value = NewScriptVariable;
			ScriptVariables.AddPair(&Variable, &Value);

			return NewScriptVariable;
		}

		virtual const TCHAR* Describe() const override
		{
			return TEXT("reflection");
		}

	private:
		/** Mirrors UNiagaraNodeCustomHlsl::RebuildSignatureFromPins. Signature is a public member. */
		void RebuildSignatureFromPins(UNiagaraNodeCustomHlsl& Node, const UEdGraphSchema_Niagara& Schema) const
		{
			Node.Modify();

			FNiagaraFunctionSignature Sig = Node.Signature;
			Sig.Inputs.Empty();
			Sig.Outputs.Empty();

			TArray<UEdGraphPin*> InputPins;
			TArray<UEdGraphPin*> OutputPins;
			Node.GetInputPins(InputPins);
			Node.GetOutputPins(OutputPins);

			for (UEdGraphPin* Pin : InputPins)
			{
				if (!IsAddPin(Pin))
				{
					Sig.Inputs.Add(Schema.PinToNiagaraVariable(Pin, true));
				}
			}
			for (UEdGraphPin* Pin : OutputPins)
			{
				if (!IsAddPin(Pin))
				{
					Sig.Outputs.Add(Schema.PinToNiagaraVariable(Pin, false));
				}
			}

			Node.Signature = Sig;
		}

		/**
		 * Builds a throwaway node and reads the sub-category off the Add pin the engine gave it.
		 *
		 * This is the one constant that had to be copied rather than referenced, so it is the one worth
		 * confirming against the engine instead of trusting. A mismatch would not fail loudly -- pins
		 * would simply be appended instead of replacing the Add pin, and the graph would come out with
		 * its pins in the wrong order.
		 */
		bool CheckAddPinSubCategory(FString& OutWhyNot) const
		{
			UNiagaraGraph* ProbeGraph = NewObject<UNiagaraGraph>(GetTransientPackage(), NAME_None, RF_Transient);
			UNiagaraNode* ProbeNode = CreateAndFinalizeNode(*ProbeGraph, ParameterMapGetClass);
			if (ProbeNode == nullptr)
			{
				OutWhyNot = TEXT("a parameter map get node could not be built from its class");
				return false;
			}

			for (const UEdGraphPin* Pin : ProbeNode->Pins)
			{
				if (Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc)
				{
					if (Pin->PinType.PinSubCategory == AddPinSubCategoryLiteral)
					{
						return true;
					}
					OutWhyNot = FString::Printf(
						TEXT("this engine names the dynamic add pin '%s', not '%s'"),
						*Pin->PinType.PinSubCategory.ToString(), AddPinSubCategoryLiteral);
					return false;
				}
			}

			OutWhyNot = TEXT("a new parameter map get node has no add pin to identify");
			return false;
		}

		UClass* ParameterMapGetClass = nullptr;
		UClass* ParameterMapSetClass = nullptr;
		FStrProperty* CustomHlslProperty = nullptr;
		FMapProperty* VariableToScriptVariableProperty = nullptr;
		FMapProperty* ParameterToReferencesProperty = nullptr;
		FBoolProperty* CreatedByUserProperty = nullptr;
	};

#if DREAMFX_HAS_CUSTOMHLSL_WRITE

	// -------------------------------------------------------------------------------------------
	// State 1 -- MoonEngine exports the declarations, so this is a pass-through
	// -------------------------------------------------------------------------------------------

	class FDirectGraphSurgeon final : public FGraphSurgeon
	{
	public:
		virtual UNiagaraNode* CreateParameterMapGet(UNiagaraGraph& Graph) override
		{
			FGraphNodeCreator<UNiagaraNodeParameterMapGet> Creator(Graph);
			UNiagaraNodeParameterMapGet* Node = Creator.CreateNode();
			Creator.Finalize();
			return Node;
		}

		virtual UNiagaraNode* CreateParameterMapSet(UNiagaraGraph& Graph) override
		{
			FGraphNodeCreator<UNiagaraNodeParameterMapSet> Creator(Graph);
			UNiagaraNodeParameterMapSet* Node = Creator.CreateNode();
			Creator.Finalize();
			return Node;
		}

		virtual UEdGraphPin* AddTypedPin(UNiagaraNode& Node, EEdGraphPinDirection Direction,
			const FNiagaraTypeDefinition& Type, FName Name) override
		{
			UNiagaraNodeWithDynamicPins* DynamicNode = Cast<UNiagaraNodeWithDynamicPins>(&Node);
			return DynamicNode != nullptr ? DynamicNode->RequestNewTypedPin(Direction, Type, Name) : nullptr;
		}

		virtual void SetCustomHlsl(UNiagaraNodeCustomHlsl& Node, const FString& Hlsl) override
		{
			Node.SetCustomHlsl(Hlsl);
		}

		virtual void InitAsDynamicInput(UNiagaraNodeCustomHlsl& Node, const FNiagaraTypeDefinition& OutputType) override
		{
			Node.InitAsCustomHlslDynamicInput(OutputType);
		}

		virtual UNiagaraScriptVariable* AddParameter(UNiagaraGraph& Graph, const FNiagaraVariable& Variable,
			const FNiagaraVariableMetaData& MetaData) override
		{
			return Graph.AddParameter(Variable, MetaData, /*bIsStaticSwitch=*/false, /*bNotifyChanged=*/false);
		}

		virtual const TCHAR* Describe() const override
		{
			return TEXT("direct");
		}
	};

#endif // DREAMFX_HAS_CUSTOMHLSL_WRITE

	TUniquePtr<FGraphSurgeon> FGraphSurgeon::Create(FString& OutUnavailableReason)
	{
#if DREAMFX_HAS_CUSTOMHLSL_WRITE
		if (!FParse::Param(FCommandLine::Get(), TEXT("DreamFXForceReflectionBackend")))
		{
			return MakeUnique<FDirectGraphSurgeon>();
		}

		// -DreamFXForceReflectionBackend selects state 2 on an engine that does not need it, which is
		// the only way to diff the two implementations' output against each other. It is also how the
		// reflected path gets exercised by CI on the machine that has MoonEngine.
#endif

		TUniquePtr<FReflectedGraphSurgeon> Reflected = MakeUnique<FReflectedGraphSurgeon>();
		FString WhyNot;
		if (Reflected->Initialize(WhyNot))
		{
			return Reflected;
		}

		OutUnavailableReason = WhyNot;
		return nullptr;
	}
}
