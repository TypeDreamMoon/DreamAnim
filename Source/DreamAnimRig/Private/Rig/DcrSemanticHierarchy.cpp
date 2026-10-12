// Copyright DreamMoon. All Rights Reserved.
//
// The sections that are data: Settings, Module, Hierarchy and Variables. Every value in them is a constant,
// read through the engine's reflection into the export text the model keeps.

#include "DcrSemanticInternal.h"

#include "Engine/SkeletalMesh.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrHierarchyImport.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigModuleDefines.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig::Sem
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	namespace
	{
		/** A leaf a declaration may hold: a literal, an enum value, an asset. No names of variables or lets exist here. */
		bool IsDeclConstant(const FDabExpr& Expr, const FProperty& Property)
		{
			if (IsDcrConstantLiteral(Expr))
			{
				return true;
			}
			const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
			const bool bEnum = CastField<FEnumProperty>(&Property) != nullptr || (Numeric && Numeric->GetIntPropertyEnum() != nullptr);
			const bool bObject = CastField<FObjectPropertyBase>(&Property) != nullptr;
			const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
			const bool bSoftPath = StructProperty && StructProperty->Struct->IsChildOf(TBaseStructure<FSoftObjectPath>::Get());
			if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
			{
				return bEnum || bObject || bSoftPath || (Name->Name == TEXT("None") && CastField<FNameProperty>(&Property) != nullptr);
			}
			if (Expr.Is<FDabMemberExpr>())
			{
				return bEnum;
			}
			if (const FDabCallExpr* Call = Expr.As<FDabCallExpr>())
			{
				const FDabNameExpr* Callee = Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
				return bObject && Callee && Callee->Name == TEXT("Path");
			}
			return false;
		}

		FString MakeElementKey(EDcrElementKind Kind, const FString& Name)
		{
			return FString::Printf(TEXT("%s|%s"), LexToString(Kind), *Name);
		}

		bool ElementKindFromWord(const FString& Word, EDcrElementKind& OutKind)
		{
			static const EDcrElementKind Kinds[] = {
				EDcrElementKind::Bone, EDcrElementKind::Null, EDcrElementKind::Control, EDcrElementKind::Curve, EDcrElementKind::Connector, EDcrElementKind::Socket,
			};
			for (const EDcrElementKind Kind : Kinds)
			{
				if (Word == LexToString(Kind))
				{
					OutKind = Kind;
					return true;
				}
			}
			return false;
		}

		/** Whether an element of this kind may have a parent of that kind. */
		bool CanBeParent(EDcrElementKind ParentKind)
		{
			return ParentKind == EDcrElementKind::Bone || ParentKind == EDcrElementKind::Null || ParentKind == EDcrElementKind::Control;
		}

		bool CanHaveParent(EDcrElementKind Kind)
		{
			return Kind == EDcrElementKind::Bone || Kind == EDcrElementKind::Null || Kind == EDcrElementKind::Control || Kind == EDcrElementKind::Socket;
		}

		/** The text of a name written as an identifier, a backtick name or a string. */
		bool GetNameText(const FDabExpr& Expr, FString& OutName)
		{
			if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
			{
				OutName = Name->Name;
				return true;
			}
			if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
			{
				OutName = Literal->Text;
				return true;
			}
			return false;
		}

		struct FHierarchyReader
		{
			FGlobals& G;
			TMap<FString, int32> Index;
			/** The elements the last `Import` gave that no refinement has changed yet. */
			TSet<FString> Refinable;

			explicit FHierarchyReader(FGlobals& InGlobals) : G(InGlobals) {}

			void AddKey(int32 ElementIndex)
			{
				Index.Add(MakeElementKey(G.Model.Hierarchy[ElementIndex].Kind, G.Model.Hierarchy[ElementIndex].Name), ElementIndex);
			}

			void ReadImport(const FDabAssignStmt& Stmt, const FString& PreviewMeshPath)
			{
				FString MeshPath;
				if (Stmt.Value)
				{
					FString Problem;
					bool bAmbiguous = false;
					MeshPath = G.ResolveAssetPath(*Stmt.Value, USkeletalMesh::StaticClass(), Problem, bAmbiguous);
					if (MeshPath.IsEmpty())
					{
						G.Sink.Error(bAmbiguous ? TEXT("DAB9002") : TEXT("DAB9001"), Stmt.Value->Span, Problem);
						return;
					}
				}
				else
				{
					MeshPath = PreviewMeshPath;
				}
				TArray<FDcrModelElement> Imported;
				if (!ImportDcrHierarchy(MeshPath, Imported))
				{
					G.Sink.Error(TEXT("DAB9001"), Stmt.Span, FString::Printf(TEXT("`%s` is not a skeletal mesh with a skeleton, so nothing can be imported from it."), *MeshPath));
					return;
				}
				for (FDcrModelElement& Element : Imported)
				{
					const FString Key = MakeElementKey(Element.Kind, Element.Name);
					if (Index.Contains(Key))
					{
						G.Sink.Error(TEXT("DAB9009"), Stmt.Span, FString::Printf(TEXT("The import gives %s `%s`, which the hierarchy already has."), LexToString(Element.Kind), *Element.Name));
						continue;
					}
					G.Model.Hierarchy.Add(MoveTemp(Element));
					AddKey(G.Model.Hierarchy.Num() - 1);
					Refinable.Add(Key);
				}
			}

			/** Reads `Parent = Bone(name)`. */
			bool ReadParent(const FDabAssignStmt& Stmt, EDcrElementKind& OutKind, FString& OutName)
			{
				const FDabCallExpr* Call = Stmt.Value ? Stmt.Value->As<FDabCallExpr>() : nullptr;
				const FDabNameExpr* Callee = Call && Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
				if (!Callee || !ElementKindFromWord(Callee->Name, OutKind) || Call->Args.Num() != 1 || !Call->Args[0] || Call->Args[0]->IsNamed()
					|| !Call->Args[0]->Value || !GetNameText(*Call->Args[0]->Value, OutName))
				{
					G.Sink.Error(TEXT("DAB9009"), Stmt.Span, TEXT("A parent is written as an element: `Parent = Bone(pelvis);`."));
					return false;
				}
				return true;
			}

			/** Reads one property of an element into its export text. Returns false, reported, when it is not one. */
			bool ReadProperty(const FDcrModelElement& Element, const FDabAssignStmt& Stmt, FString& OutText, bool& bOutRemove)
			{
				bOutRemove = false;
				const FString& Key = Stmt.Name;
				if (!Stmt.Value)
				{
					return false;
				}
				const FDabExpr& Expr = *Stmt.Value;
				const FProperty* Member = nullptr;
				FDcrType Type;
				FString DefaultText;
				bool bSettings = false;

				if (Element.Kind == EDcrElementKind::Bone && Key == TEXT("BoneType"))
				{
					FString ValueName;
					if (const FDabMemberExpr* Qualified = Expr.As<FDabMemberExpr>())
					{
						ValueName = Qualified->Member;
					}
					else if (!GetNameText(Expr, ValueName))
					{
						G.Sink.Error(TEXT("DAB9004"), Expr.Span, TEXT("`BoneType` takes User or Imported."));
						return false;
					}
					const UEnum* Enum = StaticEnum<ERigBoneType>();
					const int64 Value = Enum->GetValueByNameString(ValueName);
					if (Value == INDEX_NONE)
					{
						G.Sink.Error(TEXT("DAB9001"), Expr.Span, FString::Printf(TEXT("ERigBoneType has no value `%s`."), *ValueName));
						return false;
					}
					OutText = Enum->GetNameStringByValue(Value);
					return true;
				}
				if (Element.Kind == EDcrElementKind::Curve && Key == TEXT("Value"))
				{
					FDcrValueSlot Slot;
					const FDcrType FloatType = MakeDcrBuiltinType(TEXT("float"));
					if (!Slot.Init(FloatType) || !WriteConstantValue(G, *Slot.GetProperty(), Slot.GetMemory(), Expr))
					{
						return false;
					}
					const float Value = *static_cast<const float*>(Slot.GetMemory());
					OutText = FString::SanitizeFloat(static_cast<double>(Value));
					bOutRemove = Value == 0.f;
					return true;
				}

				if (Element.Kind == EDcrElementKind::Control && Key == TEXT("Initial"))
				{
					// The value is written as the control's type would: a number, a vector, a transform.
					ERigControlType ControlType = FRigControlSettings().ControlType;
					if (const FDcrValue* Setting = Element.Properties.FindByPredicate([](const FDcrValue& Value) { return Value.Name == TEXT("ControlType"); }))
					{
						const int64 Found = StaticEnum<ERigControlType>()->GetValueByNameString(Setting->Value);
						if (Found != INDEX_NONE)
						{
							ControlType = static_cast<ERigControlType>(Found);
						}
					}
					FRigControlValue ControlValue;
					FString Error;
					if (!WriteDcrControlValue(ControlType, Expr, ControlValue, Error))
					{
						G.Sink.Error(ClassifyValueError(Error), Expr.Span, Error);
						return false;
					}
					// Spelled from memory: a bool or an integer sits in the float store as its bits, which read
					// back from text as a float that is flushed to zero.
					FDcrValueSlot Spelled;
					if (!Spelled.Init(MakeDcrType(*FRigControlValue::StaticStruct())))
					{
						return false;
					}
					FRigControlValue::StaticStruct()->CopyScriptStruct(Spelled.GetMemory(), &ControlValue);
					OutText = Spelled.ExportText();
					bOutRemove = IsDcrControlValueIdentity(ControlType, ControlValue);
					return true;
				}

				const bool bTransformKey = Key == TEXT("Transform") || Key == TEXT("Offset") || Key == TEXT("ShapeTransform");
				if (bTransformKey)
				{
					const bool bAllowed = (Key == TEXT("Transform") && (Element.Kind == EDcrElementKind::Bone || Element.Kind == EDcrElementKind::Null || Element.Kind == EDcrElementKind::Socket))
						|| ((Key == TEXT("Offset") || Key == TEXT("ShapeTransform")) && Element.Kind == EDcrElementKind::Control);
					if (!bAllowed)
					{
						G.Sink.Error(TEXT("DAB9001"), Stmt.NameSpan, FString::Printf(TEXT("A %s has no property `%s`."), LexToString(Element.Kind), *Key));
						return false;
					}
					Type = MakeDcrType(*TBaseStructure<FTransform>::Get());
				}
				else if (Element.Kind == EDcrElementKind::Socket && Key == TEXT("Color"))
				{
					Type = MakeDcrType(*TBaseStructure<FLinearColor>::Get());
				}
				else if (Element.Kind == EDcrElementKind::Socket && Key == TEXT("Description"))
				{
					Type = MakeDcrBuiltinType(TEXT("FString"));
				}
				else if (Element.Kind == EDcrElementKind::Control)
				{
					Member = FindDcrStructMember(*FRigControlSettings::StaticStruct(), Key);
					bSettings = true;
				}
				else if (Element.Kind == EDcrElementKind::Connector)
				{
					Member = FindDcrStructMember(*FRigConnectorSettings::StaticStruct(), Key);
					bSettings = true;
				}
				if (!Member && !Type.IsSet())
				{
					G.Sink.Error(TEXT("DAB9001"), Stmt.NameSpan, FString::Printf(TEXT("A %s has no property `%s`."), LexToString(Element.Kind), *Key));
					return false;
				}

				FDcrValueSlot Slot;
				FDcrValueSlot Default;
				if (!(Member ? (Slot.Init(*Member) && Default.Init(*Member)) : (Slot.Init(Type) && Default.Init(Type))))
				{
					G.Sink.Error(TEXT("DAB9004"), Expr.Span, FString::Printf(TEXT("`%s` has a type the text cannot write."), *Key));
					return false;
				}
				if (Member && bSettings)
				{
					// Start from the settings' own defaults, so a tuple names only what differs.
					if (Element.Kind == EDcrElementKind::Control)
					{
						const FRigControlSettings Defaults;
						Slot.GetProperty()->CopyCompleteValue(Slot.GetMemory(), Member->ContainerPtrToValuePtr<void>(&Defaults));
						Default.GetProperty()->CopyCompleteValue(Default.GetMemory(), Member->ContainerPtrToValuePtr<void>(&Defaults));
					}
					else
					{
						const FRigConnectorSettings Defaults;
						Slot.GetProperty()->CopyCompleteValue(Slot.GetMemory(), Member->ContainerPtrToValuePtr<void>(&Defaults));
						Default.GetProperty()->CopyCompleteValue(Default.GetMemory(), Member->ContainerPtrToValuePtr<void>(&Defaults));
					}
				}
				if (Element.Kind == EDcrElementKind::Socket && Key == TEXT("Color"))
				{
					*static_cast<FLinearColor*>(Slot.GetMemory()) = FRigSocketElement::SocketDefaultColor;
					*static_cast<FLinearColor*>(Default.GetMemory()) = FRigSocketElement::SocketDefaultColor;
				}
				if (!WriteConstantValue(G, *Slot.GetProperty(), Slot.GetMemory(), Expr))
				{
					return false;
				}
				OutText = Slot.ExportText();
				// What the model leaves out is the default.
				bOutRemove = OutText == Default.ExportText();
				return true;
			}

			void SetProperty(FDcrModelElement& Element, const FString& Key, const FString& Text, bool bRemove)
			{
				const int32 Existing = Element.Properties.IndexOfByPredicate([&Key](const FDcrValue& Value) { return Value.Name == Key; });
				if (bRemove)
				{
					// The text says default, which the model holds as nothing; a bone's type is always held.
					if (Element.Kind == EDcrElementKind::Bone && Key == TEXT("BoneType"))
					{
						return;
					}
					if (Existing != INDEX_NONE)
					{
						Element.Properties.RemoveAt(Existing);
					}
					return;
				}
				if (Existing != INDEX_NONE)
				{
					Element.Properties[Existing].Value = Text;
					return;
				}
				FDcrValue& Value = Element.Properties.AddDefaulted_GetRef();
				Value.Name = Key;
				Value.Value = Text;
			}

			void ReadElement(const FDabNamedBlockStmt& Stmt, EDcrElementKind ParentKind, const FString& ParentName)
			{
				EDcrElementKind Kind = EDcrElementKind::Bone;
				if (!ElementKindFromWord(Stmt.Word, Kind))
				{
					G.Sink.Error(TEXT("DAB6011"), Stmt.WordSpan, FString::Printf(TEXT("`%s` is not a hierarchy element. Write Bone, Null, Control, Curve, Connector or Socket."), *Stmt.Word));
					return;
				}
				const FString Key = MakeElementKey(Kind, Stmt.Name);
				const bool bRefine = Refinable.Contains(Key);
				if (!bRefine && Index.Contains(Key))
				{
					G.Sink.Error(TEXT("DAB9009"), Stmt.NameSpan, FString::Printf(TEXT("%s `%s` is declared twice."), LexToString(Kind), *Stmt.Name));
					return;
				}

				// Gather the body first: the parent can be written anywhere in it.
				EDcrElementKind Parent = ParentKind;
				FString ParentOf = ParentName;
				bool bParentWritten = !ParentName.IsEmpty();
				TArray<const FDabAssignStmt*> Properties;
				TArray<const FDabNamedBlockStmt*> Children;
				if (Stmt.Body)
				{
					for (const FDabStmtPtr& Inner : Stmt.Body->Statements)
					{
						if (!Inner)
						{
							continue;
						}
						if (const FDabAssignStmt* Assign = Inner->As<FDabAssignStmt>())
						{
							if (Assign->Name == TEXT("Parent"))
							{
								bParentWritten = ReadParent(*Assign, Parent, ParentOf);
								if (!bParentWritten)
								{
									Parent = ParentKind;
									ParentOf = ParentName;
								}
							}
							else
							{
								Properties.Add(Assign);
							}
						}
						else if (const FDabNamedBlockStmt* Child = Inner->As<FDabNamedBlockStmt>())
						{
							Children.Add(Child);
						}
						else if (!Inner->Is<FDabRegionStmt>())
						{
							G.Sink.Error(TEXT("DAB6011"), Inner->Span, TEXT("An element holds its properties (`Key = Value;`) and the elements under it."));
						}
					}
				}

				int32 ElementIndex = INDEX_NONE;
				if (bRefine)
				{
					ElementIndex = Index.FindChecked(Key);
					Refinable.Remove(Key);
				}
				else
				{
					FDcrModelElement& Added = G.Model.Hierarchy.AddDefaulted_GetRef();
					Added.Kind = Kind;
					Added.Name = Stmt.Name;
					if (Kind == EDcrElementKind::Bone)
					{
						// A bone the text adds is a user bone unless it says otherwise.
						SetProperty(Added, TEXT("BoneType"), TEXT("User"), false);
					}
					ElementIndex = G.Model.Hierarchy.Num() - 1;
					AddKey(ElementIndex);
				}

				if (bParentWritten && !ParentOf.IsEmpty())
				{
					if (!CanHaveParent(Kind))
					{
						G.Sink.Error(TEXT("DAB9009"), Stmt.NameSpan, FString::Printf(TEXT("A %s has no parent."), LexToString(Kind)));
					}
					else if (!CanBeParent(Parent))
					{
						G.Sink.Error(TEXT("DAB9009"), Stmt.NameSpan, FString::Printf(TEXT("A %s cannot be the parent of a %s."), LexToString(Parent), LexToString(Kind)));
					}
					else if (!Index.Contains(MakeElementKey(Parent, ParentOf)))
					{
						G.Sink.Error(TEXT("DAB9009"), Stmt.NameSpan, FString::Printf(TEXT("%s `%s`, the parent of `%s`, is not declared before it."), LexToString(Parent), *ParentOf, *Stmt.Name));
					}
					else
					{
						G.Model.Hierarchy[ElementIndex].Parent = ParentOf;
						G.Model.Hierarchy[ElementIndex].ParentKind = Parent;
					}
				}

				// `Initial` is read last: its type is the control's, which the other properties may set.
				Properties.StableSort([](const FDabAssignStmt& A, const FDabAssignStmt& B) { return A.Name != TEXT("Initial") && B.Name == TEXT("Initial"); });
				for (const FDabAssignStmt* Assign : Properties)
				{
					FString Text;
					bool bRemove = false;
					// The element is copied: reading may add elements and move the array.
					const FDcrModelElement Snapshot = G.Model.Hierarchy[ElementIndex];
					if (ReadProperty(Snapshot, *Assign, Text, bRemove))
					{
						SetProperty(G.Model.Hierarchy[ElementIndex], Assign->Name, Text, bRemove);
					}
				}

				for (const FDabNamedBlockStmt* Child : Children)
				{
					ReadElement(*Child, Kind, Stmt.Name);
				}
			}
		};
	}

	bool ReadStringMeta(FGlobals& G, const FDabMetaEntry& Entry, FString& OutText)
	{
		const FDabLiteralExpr* Literal = Entry.Value ? Entry.Value->As<FDabLiteralExpr>() : nullptr;
		if (!Literal || Literal->LiteralKind != EDabLiteralKind::String)
		{
			G.Sink.Error(TEXT("DAB9004"), Entry.Span, FString::Printf(TEXT("`%s` takes a string."), *Entry.Key));
			return false;
		}
		OutText = Literal->Text;
		return true;
	}

	bool WriteConstantValue(FGlobals& G, const FProperty& Property, void* Memory, const FDabExpr& Expr)
	{
		TArray<FDcrValuePart> Parts;
		FString Error;
		const bool bWritten = WriteDcrValueExpr(Property, Memory, Expr,
			[](const FDabExpr& Leaf, const FProperty& Candidate) { return IsDeclConstant(Leaf, Candidate); },
			[&G](const FDabExpr& Leaf, const UClass* Expected) -> UObject*
			{
				FString Problem;
				bool bAmbiguous = false;
				const FString ObjectPath = G.ResolveAssetPath(Leaf, Expected, Problem, bAmbiguous);
				return ObjectPath.IsEmpty() ? nullptr : FSoftObjectPath(ObjectPath).TryLoad();
			},
			Parts, Error);
		if (!bWritten)
		{
			G.Sink.Error(ClassifyValueError(Error), Expr.Span, Error);
			return false;
		}
		if (!Parts.IsEmpty())
		{
			G.Sink.Error(TEXT("DAB9004"), Parts[0].Expr ? Parts[0].Expr->Span : Expr.Span, TEXT("This must be a constant: a number, a string, a value of an enum, an asset."));
			return false;
		}
		return true;
	}

	bool ReadConstantText(FGlobals& G, const FDcrType& Type, const FDabExpr& Expr, FString& OutText)
	{
		OutText.Reset();
		FDcrValueSlot Slot;
		FDcrValueSlot Default;
		if (!Slot.Init(Type) || !Default.Init(Type))
		{
			G.Sink.Error(TEXT("DAB9004"), Expr.Span, FString::Printf(TEXT("The type %s has no value the text can write."), *Type.CPPType));
			return false;
		}
		if (!WriteConstantValue(G, *Slot.GetProperty(), Slot.GetMemory(), Expr))
		{
			return false;
		}
		OutText = Slot.ExportText();
		if (OutText == Default.ExportText())
		{
			OutText.Reset();
		}
		return true;
	}

	bool ResolveTypeName(FGlobals& G, const FString& Spelling, const FDabSpan& Span, FDcrType& OutType)
	{
		FString Error;
		if (!FindDcrType(Spelling, OutType, Error))
		{
			G.Sink.Error(TEXT("DAB9001"), Span, Error);
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Settings
	// ------------------------------------------------------------------------------------------

	namespace
	{
		void ReadUses(FGlobals& G, const FDabAssignStmt& Stmt)
		{
			const FDabListExpr* List = Stmt.Value ? Stmt.Value->As<FDabListExpr>() : nullptr;
			if (!List)
			{
				G.Sink.Error(TEXT("DAB9004"), Stmt.Span, TEXT("`Uses` takes a list of the rigs whose public functions are called: [ Library, ... ]."));
				return;
			}
			for (int32 Index = 0; Index < List->Items.Num(); ++Index)
			{
				const FDabExprPtr& Item = List->Items[Index];
				if (!Item)
				{
					continue;
				}
				// `Alias = Rig`: the name calls go by (`Alias.Function`), for two rigs of one name.
				const FString& Alias = List->GetItemName(Index);
				FString Problem;
				bool bAmbiguous = false;
				const FString ObjectPath = G.ResolveAssetPath(*Item, nullptr, Problem, bAmbiguous);
				if (ObjectPath.IsEmpty())
				{
					G.Sink.Error(bAmbiguous ? TEXT("DAB9002") : TEXT("DAB9001"), Item->Span, Problem);
					continue;
				}
				if (G.Libraries.ContainsByPredicate([&ObjectPath](const TUniquePtr<FLibrary>& Library) { return Library->Host == ObjectPath; }))
				{
					continue;
				}
				const FDcrRig Rig = LoadDcrRig(ObjectPath);
				FDcrModel LibraryModel;
				if (!Rig.IsValid() || !ExtractDcrModel(Rig, LibraryModel).bSuccess)
				{
					G.Sink.Error(TEXT("DAB9012"), Item->Span, FString::Printf(TEXT("`%s` is not a Control Rig."), *ObjectPath),
						TEXT("`Uses` lists Control Rig assets, the rigs whose functions are called."));
					continue;
				}
				TUniquePtr<FLibrary> Library = MakeUnique<FLibrary>();
				Library->Host = ObjectPath;
				Library->Name = Alias.IsEmpty() ? FSoftObjectPath(ObjectPath).GetAssetName() : Alias;
				if (G.Libraries.ContainsByPredicate([&Library](const TUniquePtr<FLibrary>& Other) { return Other->Name == Library->Name; }))
				{
					G.Sink.Error(TEXT("DAB9002"), Item->Span, FString::Printf(TEXT("Two rigs in `Uses` are called `%s`."), *Library->Name),
						FString::Printf(TEXT("Name one for the calls into it: `Other%s = %s`."), *Library->Name, *ObjectPath));
					continue;
				}
				for (FDcrModelFunction& Function : LibraryModel.Functions)
				{
					if (!Function.bPublic)
					{
						Library->PrivateNames.Add(Function.Name);
						continue;
					}
					FExternalFunction& External = Library->Public.AddDefaulted_GetRef();
					for (const FDcrModelNode& Node : Function.Graph.Nodes)
					{
						if (Node.Kind == EDcrNodeKind::Entry && !Node.ExecuteOut.IsEmpty())
						{
							External.ExecuteName = Node.ExecuteOut[0];
						}
					}
					External.Function = MoveTemp(Function);
				}
				G.Libraries.Add(MoveTemp(Library));
			}
		}
	}

	void ReadSettings(FGlobals& G, const FDabBlockExpr& Block, FString& OutPreviewMeshPath)
	{
		for (const FDabStmtPtr& Item : Block.Statements)
		{
			const FDabAssignStmt* Stmt = Item ? Item->As<FDabAssignStmt>() : nullptr;
			if (!Stmt)
			{
				if (Item && !Item->Is<FDabRegionStmt>())
				{
					G.Sink.Error(TEXT("DAB6011"), Item->Span, TEXT("Settings holds `Key = Value;` entries."));
				}
				continue;
			}
			if (!Stmt->Value)
			{
				continue;
			}
			if (Stmt->Name == TEXT("PreviewMesh"))
			{
				if (const FDabNameExpr* None = Stmt->Value->As<FDabNameExpr>(); None && None->Name == TEXT("None"))
				{
					continue;
				}
				FString Problem;
				bool bAmbiguous = false;
				const FString MeshPath = G.ResolveAssetPath(*Stmt->Value, USkeletalMesh::StaticClass(), Problem, bAmbiguous);
				if (MeshPath.IsEmpty())
				{
					G.Sink.Error(bAmbiguous ? TEXT("DAB9002") : TEXT("DAB9001"), Stmt->Value->Span, Problem);
					continue;
				}
				OutPreviewMeshPath = MeshPath;
				FDcrValue& Setting = G.Model.Settings.AddDefaulted_GetRef();
				Setting.Name = TEXT("PreviewSkeletalMesh");
				Setting.Value = MeshPath;
			}
			else if (Stmt->Name == TEXT("ShapeLibraries"))
			{
				FDcrType Type;
				Type.CPPType = TEXT("TArray<TSoftObjectPtr<UControlRigShapeLibrary>>");
				Type.ObjectPath = TEXT("/Script/ControlRig.ControlRigShapeLibrary");
				FString Text;
				if (ReadConstantText(G, Type, *Stmt->Value, Text) && !Text.IsEmpty() && Text != TEXT("()"))
				{
					FDcrValue& Setting = G.Model.Settings.AddDefaulted_GetRef();
					Setting.Name = TEXT("ShapeLibraries");
					Setting.Value = Text;
				}
			}
			else if (Stmt->Name == TEXT("Uses"))
			{
				ReadUses(G, *Stmt);
			}
			else
			{
				G.Sink.Error(TEXT("DAB9001"), Stmt->NameSpan, FString::Printf(TEXT("There is no setting called `%s`. The settings are PreviewMesh, Uses and ShapeLibraries."), *Stmt->Name));
			}
		}
	}

	void ReadModuleSettings(FGlobals& G, const FDabBlockExpr& Block)
	{
		G.Model.bModule = true;
		const UScriptStruct* Struct = FRigModuleSettings::StaticStruct();
		const FRigModuleSettings Defaults;
		for (const FDabStmtPtr& Item : Block.Statements)
		{
			const FDabAssignStmt* Stmt = Item ? Item->As<FDabAssignStmt>() : nullptr;
			if (!Stmt || !Stmt->Value)
			{
				if (Item && !Item->Is<FDabRegionStmt>() && !Stmt)
				{
					G.Sink.Error(TEXT("DAB6011"), Item->Span, TEXT("Module holds `Key = Value;` entries."));
				}
				continue;
			}
			const FProperty* Property = FindDcrStructMember(*Struct, Stmt->Name);
			if (!Property)
			{
				G.Sink.Error(TEXT("DAB9001"), Stmt->NameSpan, FString::Printf(TEXT("A module has no setting called `%s`."), *Stmt->Name));
				continue;
			}
			FDcrValueSlot Slot;
			FDcrValueSlot Default;
			if (!Slot.Init(*Property) || !Default.Init(*Property))
			{
				G.Sink.Error(TEXT("DAB9004"), Stmt->Value->Span, FString::Printf(TEXT("The setting `%s` has a type the text cannot write."), *Stmt->Name));
				continue;
			}
			const void* DefaultValue = Property->ContainerPtrToValuePtr<void>(&Defaults);
			Slot.GetProperty()->CopyCompleteValue(Slot.GetMemory(), DefaultValue);
			Default.GetProperty()->CopyCompleteValue(Default.GetMemory(), DefaultValue);
			if (!WriteConstantValue(G, *Slot.GetProperty(), Slot.GetMemory(), *Stmt->Value))
			{
				continue;
			}
			if (!Slot.GetProperty()->Identical(Slot.GetMemory(), Default.GetMemory()))
			{
				FDcrValue& Setting = G.Model.ModuleSettings.AddDefaulted_GetRef();
				Setting.Name = Property->GetName();
				Setting.Value = Slot.ExportText();
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Hierarchy
	// ------------------------------------------------------------------------------------------

	void ReadHierarchy(FGlobals& G, const FDabBlockExpr& Block, const FString& PreviewMeshPath)
	{
		FHierarchyReader Reader(G);
		for (const FDabStmtPtr& Item : Block.Statements)
		{
			if (!Item)
			{
				continue;
			}
			if (const FDabAssignStmt* Assign = Item->As<FDabAssignStmt>())
			{
				if (Assign->Name == TEXT("Import"))
				{
					Reader.ReadImport(*Assign, PreviewMeshPath);
				}
				else
				{
					G.Sink.Error(TEXT("DAB6011"), Assign->Span, TEXT("The hierarchy holds elements (`Bone`, `Null`, `Control`, `Curve`, `Connector`, `Socket`) and `Import = Mesh;`."));
				}
			}
			else if (const FDabNamedBlockStmt* Element = Item->As<FDabNamedBlockStmt>())
			{
				Reader.ReadElement(*Element, EDcrElementKind::Bone, FString());
			}
			else if (!Item->Is<FDabRegionStmt>())
			{
				G.Sink.Error(TEXT("DAB6011"), Item->Span, TEXT("The hierarchy holds elements and `Import = Mesh;`."));
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Variables
	// ------------------------------------------------------------------------------------------

	void ReadVariablesBlock(FGlobals& G, const FDabBlockExpr& Block)
	{
		for (const FDabStmtPtr& Item : Block.Statements)
		{
			const FDabDeclStmt* Decl = Item ? Item->As<FDabDeclStmt>() : nullptr;
			if (!Decl)
			{
				if (Item && !Item->Is<FDabRegionStmt>())
				{
					G.Sink.Error(TEXT("DAB6011"), Item->Span, TEXT("Variables holds declarations: `Type Name = Default [ Public ];`."));
				}
				continue;
			}
			if (G.FindVariable(Decl->Name))
			{
				G.Sink.Error(TEXT("DAB6008"), Decl->NameSpan, FString::Printf(TEXT("The variable `%s` is declared twice."), *Decl->Name));
				continue;
			}
			FDcrModelVariable Variable;
			Variable.Name = Decl->Name;
			bool bTypeOk = ResolveTypeName(G, Decl->TypeName, Decl->TypeSpan, Variable.Type);
			if (bTypeOk && Decl->Value)
			{
				ReadConstantText(G, Variable.Type, *Decl->Value, Variable.Default);
			}
			if (Decl->Meta)
			{
				for (const TUniquePtr<FDabMetaEntry>& Entry : Decl->Meta->Entries)
				{
					if (!Entry)
					{
						continue;
					}
					if (Entry->Key == TEXT("Public"))
					{
						Variable.bPublic = true;
					}
					else if (Entry->Key == TEXT("ReadOnly"))
					{
						Variable.bReadOnly = true;
					}
					else if (Entry->Key == TEXT("Category"))
					{
						ReadStringMeta(G, *Entry, Variable.Category);
					}
					else if (Entry->Key == TEXT("Tooltip"))
					{
						ReadStringMeta(G, *Entry, Variable.Tooltip);
					}
					else
					{
						G.Sink.Error(TEXT("DAB9001"), Entry->KeySpan, FString::Printf(TEXT("A variable has no metadata called `%s`. The metadata are Public, ReadOnly, Category and Tooltip."), *Entry->Key));
					}
				}
			}
			if (bTypeOk)
			{
				G.Model.Variables.Add(MoveTemp(Variable));
			}
		}
	}
}
