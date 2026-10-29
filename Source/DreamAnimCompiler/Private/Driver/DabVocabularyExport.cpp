// Copyright DreamMoon. All Rights Reserved.
//
// The vocabulary `dab.ps1 vocab` writes. Node types and their pins are read with the checker's own rules
// (which classes are node types and how the text names them, which fields are arguments), and defaults are
// printed the way the decompiler writes values, so what the vocabulary lists is what a .dab can say.

#include "Driver/DabVocabularyExport.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomProperty.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
#include "AnimGraphNode_MeshRefPose.h"
#include "AnimGraphNode_SkeletalControlBase.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimNodeBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Driver/DabDocumentHandler.h"
#include "EdGraphSchema_K2.h"
#include "Extract/DabExtract.h"
#include "Extract/DabExtractPrivate.h"
#include "Interfaces/IPluginManager.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabVocabulary.h"
#include "Misc/EngineVersion.h"
#include "Model/DabModel.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Semantic/DabSemanticInternal.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler
{
	namespace VocabularyPrivate
	{
		using FJsonValues = TArray<TSharedPtr<FJsonValue>>;

		TSharedPtr<FJsonValue> MakeString(const FString& Text)
		{
			return MakeShared<FJsonValueString>(Text);
		}

		/** The type as the text spells it: `float`, `Vector`, `AnimSequence`, `TArray<float>` ... */
		FString GetTypeSpelling(const FProperty& Property)
		{
			FEdGraphPinType PinType;
			if (GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(&Property, PinType))
			{
				return ExtractPrivate::GetPinTypeSpelling(PinType);
			}
			return Property.GetCPPType();
		}

		const UEnum* GetEnum(const FProperty& Property)
		{
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
			{
				return EnumProperty->GetEnum();
			}
			if (const FByteProperty* Byte = CastField<FByteProperty>(&Property))
			{
				return Byte->Enum;
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				return GetEnum(*Array->Inner);
			}
			return nullptr;
		}

		/** The bare names the text writes an enum's values by (`Linear`, `TLT_StandardBlend`), hidden ones and the MAX left out. */
		FJsonValues GetEnumEntries(const UEnum& Enum)
		{
			FJsonValues Entries;
			const int32 Count = Enum.ContainsExistingMax() ? Enum.NumEnums() - 1 : Enum.NumEnums();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Enum.HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
				FString Name = Enum.GetNameStringByIndex(Index);
				int32 Scope = INDEX_NONE;
				if (Name.FindLastChar(TEXT(':'), Scope))
				{
					Name.RightChopInline(Scope + 1);
				}
				Entries.Add(MakeString(Name));
			}
			return Entries;
		}

		/** How a field can be shown on the node: `always`, `shown`, `hidden` (the editor's pin options), `never` or `none`. */
		const TCHAR* GetPinExposure(const FProperty& Property)
		{
			if (Property.HasMetaData(TEXT("NeverAsPin")))
			{
				return TEXT("never");
			}
			if (Property.HasMetaData(TEXT("AlwaysAsPin")))
			{
				return TEXT("always");
			}
			if (Property.HasMetaData(TEXT("PinShownByDefault")))
			{
				return TEXT("shown");
			}
			if (Property.HasMetaData(TEXT("PinHiddenByDefault")))
			{
				return TEXT("hidden");
			}
			return TEXT("none");
		}

		const TCHAR* GetPoseSpace(const FProperty& Property)
		{
			const FProperty* Element = &Property;
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				Element = Array->Inner;
			}
			const FStructProperty* Struct = CastField<FStructProperty>(Element);
			return Struct && Struct->Struct && Struct->Struct->IsChildOf(FComponentSpacePoseLink::StaticStruct()) ? TEXT("component") : TEXT("local");
		}

		/** How a few node classes are written: not by their type name. */
		const TCHAR* GetWrittenAs(const UClass& Class)
		{
			static const TPair<const TCHAR*, const TCHAR*> Special[] =
			{
				{ TEXT("AnimGraphNode_Root"), TEXT("Output = Pose;") },
				{ TEXT("AnimGraphNode_LinkedInputPose"), TEXT("Input(Name, settings...)") },
				{ TEXT("AnimGraphNode_CallFunction"), TEXT("Call(Target.Function, On = Site)") },
				{ TEXT("AnimGraphNode_Slot"), TEXT("Slot(\"Name\", settings...)") },
				{ TEXT("AnimGraphNode_StateMachine"), TEXT("the state machine's name, where it runs") },
				{ TEXT("AnimGraphNode_SaveCachedPose"), TEXT("Name = Pose [ Cached ];") },
				{ TEXT("AnimGraphNode_UseCachedPose"), TEXT("the cached pose's name") },
				{ TEXT("AnimGraphNode_StateResult"), TEXT("a State's Pose = ...;") },
				{ TEXT("AnimGraphNode_TransitionResult"), TEXT("a transition's condition") },
			};
			const FString Name = Class.GetName();
			for (const TPair<const TCHAR*, const TCHAR*>& Entry : Special)
			{
				if (Name == Entry.Key)
				{
					return Entry.Value;
				}
			}
			return nullptr;
		}

		/** Converts values the way the decompiler writes them; what it cannot write it leaves out. */
		class FValuePrinter
		{
		public:
			FValuePrinter()
				: Blueprint(NewObject<UAnimBlueprint>(GetTransientPackage(), NAME_None, RF_Transient))
				, Context(*Blueprint, Model, Options)
			{
			}

			~FValuePrinter()
			{
				Blueprint->MarkAsGarbage();
			}

			/** The text of Value, or empty when the language cannot write it. */
			FString Print(const FProperty& Property, const void* Value)
			{
				const int32 Before = Model.Unsupported.Num();
				const Lang::FDabExprPtr Expr = ExtractPrivate::ConvertPropertyValue(Context, Property, Value, nullptr, Property.GetName());
				if (!Expr || Model.Unsupported.Num() != Before)
				{
					return FString();
				}
				return Lang::PrintDabExpr(*Expr);
			}

		private:
			Model::FDabModel Model;
			FDabExtractOptions Options;
			UAnimBlueprint* Blueprint;
			ExtractPrivate::FDabExtractContext Context;
		};

		/** One value pin: a field of the runtime node, or a setting of the editor node (`Tag`, `BoundEnum`). */
		TSharedRef<FJsonObject> MakeValuePin(const FProperty& Property, const void* Value, const TCHAR* Kind, bool bPerPose, FValuePrinter& Printer)
		{
			TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
			Pin->SetStringField(TEXT("name"), Property.GetName());
			Pin->SetStringField(TEXT("kind"), Kind);
			Pin->SetStringField(TEXT("type"), GetTypeSpelling(Property));
			Pin->SetStringField(TEXT("cppType"), Property.GetCPPType());
			Pin->SetBoolField(TEXT("array"), Property.IsA<FArrayProperty>());
			if (bPerPose)
			{
				// One entry per pose of the node's pose array; a single value stands for every pose.
				Pin->SetBoolField(TEXT("perPose"), true);
			}
			const TCHAR* Exposure = GetPinExposure(Property);
			Pin->SetStringField(TEXT("pin"), Exposure);
			// A pin the node can show can be bound to an expression; any other field takes a constant only.
			Pin->SetBoolField(TEXT("bindable"), FCString::Strcmp(Exposure, TEXT("always")) == 0 || FCString::Strcmp(Exposure, TEXT("shown")) == 0
				|| FCString::Strcmp(Exposure, TEXT("hidden")) == 0);
			Pin->SetBoolField(TEXT("editorOnly"), Property.IsEditorOnlyProperty());
			const FString Category = Property.GetMetaData(TEXT("Category"));
			if (!Category.IsEmpty())
			{
				Pin->SetStringField(TEXT("category"), Category);
			}
			if (const UStruct* Owner = Property.GetOwnerStruct())
			{
				// The struct or class that declares the field: a base node's fields are shared by its children.
				Pin->SetStringField(TEXT("owner"), Owner->GetName());
			}
			const FString Default = Printer.Print(Property, Value);
			if (!Default.IsEmpty())
			{
				Pin->SetStringField(TEXT("default"), Default);
			}
			if (const UEnum* Enum = GetEnum(Property))
			{
				Pin->SetStringField(TEXT("enum"), Enum->GetName());
				Pin->SetArrayField(TEXT("values"), GetEnumEntries(*Enum));
			}
			const FString Tooltip = Property.GetToolTipText(true).ToString();
			if (!Tooltip.IsEmpty())
			{
				Pin->SetStringField(TEXT("tooltip"), Tooltip);
			}
			return Pin;
		}

		TSharedPtr<FJsonObject> MakeAnimNode(UClass& Class, TConstArrayView<UClass*> Classes, FValuePrinter& Printer)
		{
			using namespace SemanticPrivate;
			const UAnimGraphNode_Base* Defaults = Cast<UAnimGraphNode_Base>(Class.GetDefaultObject());
			const FStructProperty* NodeProperty = Defaults ? Defaults->GetFNodeProperty() : nullptr;
			if (!Defaults || !NodeProperty || !NodeProperty->Struct)
			{
				return nullptr;
			}
			const UScriptStruct* Struct = NodeProperty->Struct;
			const void* NodeDefaults = NodeProperty->ContainerPtrToValuePtr<void>(Defaults);

			TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("name"), GetSemanticNodeTypeName(Class, Classes));
			Node->SetStringField(TEXT("class"), Class.GetPathName());
			Node->SetStringField(TEXT("struct"), Struct->GetPathName());
			if (const TCHAR* WrittenAs = GetWrittenAs(Class))
			{
				Node->SetStringField(TEXT("writtenAs"), WrittenAs);
			}
			Node->SetBoolField(TEXT("deprecated"), Class.HasAnyClassFlags(CLASS_Deprecated));
			const FString Tooltip = Class.GetToolTipText(true).ToString();
			if (!Tooltip.IsEmpty())
			{
				Node->SetStringField(TEXT("tooltip"), Tooltip);
			}
			// The output pose: none on a sink; in component space after a skeletal control or a change of space to it.
			const bool bSink = Defaults->IsSinkNode();
			Node->SetBoolField(TEXT("sink"), bSink);
			if (!bSink)
			{
				const bool bComponent = Class.IsChildOf(UAnimGraphNode_SkeletalControlBase::StaticClass())
					|| Class.IsChildOf(UAnimGraphNode_LocalToComponentSpace::StaticClass()) || Class.IsChildOf(UAnimGraphNode_MeshRefPose::StaticClass());
				Node->SetStringField(TEXT("outputSpace"), bComponent ? TEXT("component") : TEXT("local"));
			}
			const bool bLinkedGraph = IsStructNamedOrChildOf(Struct, FName(TEXT("AnimNode_LinkedAnimGraph")));
			if (bLinkedGraph)
			{
				// Pose inputs named after the target's input poses: Inputs = { Name = Pose; }.
				Node->SetBoolField(TEXT("inputsByName"), true);
			}
			if (Class.IsChildOf(UAnimGraphNode_CustomProperty::StaticClass()))
			{
				// Pins of another class (a linked instance's variables, a rig's inputs) taken as named arguments.
				Node->SetBoolField(TEXT("customPins"), true);
			}
			if (Class.GetName() == TEXT("AnimGraphNode_BlendListByEnum"))
			{
				Node->SetStringField(TEXT("keyedBy"), TEXT("enum"));
			}
			else if (Class.GetName().EndsWith(TEXT("AnimGraphNode_GameplayTagsBlend"), ESearchCase::CaseSensitive))
			{
				Node->SetStringField(TEXT("keyedBy"), TEXT("tag"));
			}

			FJsonValues Pins;
			int32 NumPoseArrays = 0;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				NumPoseArrays += IsPoseLinkProperty(**It) && It->IsA<FArrayProperty>() && !bLinkedGraph ? 1 : 0;
			}
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				const FProperty& Field = **It;
				if (IsPoseLinkProperty(Field))
				{
					if (bLinkedGraph && Field.IsA<FArrayProperty>())
					{
						continue;
					}
					TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
					Pin->SetStringField(TEXT("name"), Field.GetName());
					const bool bArray = Field.IsA<FArrayProperty>();
					Pin->SetStringField(TEXT("kind"), bArray ? TEXT("poseArray") : TEXT("pose"));
					Pin->SetStringField(TEXT("space"), GetPoseSpace(Field));
					if (bArray)
					{
						// Written as a list, [ P0, P1 ]; the editor shows one pin per pose.
						Pin->SetStringField(TEXT("editorPins"), Field.GetName() + TEXT("_{index}"));
					}
					Pins.Add(MakeShared<FJsonValueObject>(Pin));
					continue;
				}
				if (!IsRecordedNodeField(Field))
				{
					continue;
				}
				const bool bPerPose = NumPoseArrays == 1 && Field.IsA<FArrayProperty>();
				Pins.Add(MakeShared<FJsonValueObject>(MakeValuePin(Field, Field.ContainerPtrToValuePtr<void>(NodeDefaults), TEXT("value"), bPerPose, Printer)));
			}
			for (TFieldIterator<FProperty> It(&Class); It; ++It)
			{
				if (IsEditorNodeArgument(**It, NodeProperty))
				{
					Pins.Add(MakeShared<FJsonValueObject>(MakeValuePin(**It, It->ContainerPtrToValuePtr<void>(Defaults), TEXT("setting"), false, Printer)));
				}
			}
			Node->SetArrayField(TEXT("pins"), Pins);
			return Node;
		}

		FJsonValues MakeDocuments()
		{
			FJsonValues Documents;
			for (const Lang::FDabDocumentWords& Document : Lang::GetDabDocumentWords())
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("kind"), Lang::LexToString(Document.Kind));
				Entry->SetStringField(TEXT("extension"), Document.Extension);
				FJsonValues Words;
				for (const Lang::FDabWord& Word : Document.Words)
				{
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("text"), Word.Text);
					Item->SetStringField(TEXT("role"), Lang::LexToString(Word.Role));
					Item->SetStringField(TEXT("context"), Word.Context);
					Item->SetStringField(TEXT("summary"), Word.Summary);
					Words.Add(MakeShared<FJsonValueObject>(Item));
				}
				Entry->SetArrayField(TEXT("words"), Words);
				Documents.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return Documents;
		}
	}

	TSharedRef<FJsonObject> MakeDabVocabulary(const FDabVocabularyOptions& Options)
	{
		using namespace VocabularyPrivate;
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("format"), 1);
		Root->SetNumberField(TEXT("grammarVersion"), Lang::DabGrammarVersion);
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamAnim"));
		Root->SetStringField(TEXT("pluginVersion"), Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString());
		Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
		Root->SetArrayField(TEXT("documents"), MakeDocuments());

		// Every node type, in the order of the names the text writes.
		TArray<UClass*> Classes = SemanticPrivate::GetSemanticNodeClasses();
		Classes.RemoveAll([](const UClass* Class) { return Class->HasAnyClassFlags(CLASS_Abstract); });
		TArray<TPair<FString, UClass*>> Named;
		for (UClass* Class : Classes)
		{
			Named.Emplace(SemanticPrivate::GetSemanticNodeTypeName(*Class, Classes), Class);
		}
		Named.Sort([](const TPair<FString, UClass*>& A, const TPair<FString, UClass*>& B) { return A.Key < B.Key; });
		FValuePrinter Printer;
		FJsonValues Nodes;
		for (const TPair<FString, UClass*>& Entry : Named)
		{
			if (const TSharedPtr<FJsonObject> Node = MakeAnimNode(*Entry.Value, Classes, Printer))
			{
				Nodes.Add(MakeShared<FJsonValueObject>(Node));
			}
		}
		Root->SetArrayField(TEXT("animNodes"), Nodes);

		for (IDabDocumentHandler* Handler : GetDabDocumentHandlers())
		{
			Handler->AddVocabulary(*Root, Options);
		}
		return Root;
	}

	FString MakeDabVocabularyJson(const FDabVocabularyOptions& Options)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(MakeDabVocabulary(Options), Writer);
		return Text + TEXT("\n");
	}
}
