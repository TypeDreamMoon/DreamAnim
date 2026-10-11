// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrNames.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/UserDefinedEnum.h"
#include "Lang/DabPrinter.h"
#include "Rig/DcrSyntax.h"
#include "Rig/DcrValues.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMTypeUtils.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Lang;

	const TCHAR* const DcrUserEventStructPath = TEXT("/Script/RigVM.RigVMFunction_UserDefinedEvent");
	const TCHAR* const DcrUserEventNamePin = TEXT("EventName");

	// ------------------------------------------------------------------------------------------
	// Events
	// ------------------------------------------------------------------------------------------

	TConstArrayView<FDcrEventWord> GetDcrEventWords()
	{
		static const FDcrEventWord Words[] = {
			{ TEXT("Forward"), TEXT("/Script/ControlRig.RigUnit_BeginExecution"), TEXT("Forwards Solve") },
			{ TEXT("Backward"), TEXT("/Script/ControlRig.RigUnit_InverseExecution"), TEXT("Backwards Solve") },
			{ TEXT("Construction"), TEXT("/Script/ControlRig.RigUnit_PrepareForExecution"), TEXT("Construction") },
			{ TEXT("PostConstruction"), TEXT("/Script/ControlRig.RigUnit_PostPrepareForExecution"), TEXT("PostConstruction") },
			{ TEXT("PreForward"), TEXT("/Script/ControlRig.RigUnit_PreBeginExecution"), TEXT("Pre Forwards Solve") },
			{ TEXT("PostForward"), TEXT("/Script/ControlRig.RigUnit_PostBeginExecution"), TEXT("Post Forwards Solve") },
			{ TEXT("Interaction"), TEXT("/Script/ControlRig.RigUnit_InteractionExecution"), TEXT("Interaction") },
			{ TEXT("Connector"), TEXT("/Script/ControlRig.RigUnit_ConnectorExecution"), TEXT("Connector") },
		};
		return Words;
	}

	const FDcrEventWord* FindDcrEventByWord(const FString& Word)
	{
		for (const FDcrEventWord& Event : GetDcrEventWords())
		{
			if (Word.Equals(Event.Word, ESearchCase::CaseSensitive))
			{
				return &Event;
			}
		}
		return nullptr;
	}

	const FDcrEventWord* FindDcrEventByStruct(const FString& StructPath)
	{
		for (const FDcrEventWord& Event : GetDcrEventWords())
		{
			if (StructPath.Equals(Event.StructPath, ESearchCase::CaseSensitive))
			{
				return &Event;
			}
		}
		return nullptr;
	}

	const FDcrEventWord* FindDcrEventByEventName(const FString& EventName)
	{
		for (const FDcrEventWord& Event : GetDcrEventWords())
		{
			if (EventName.Equals(Event.EventName, ESearchCase::CaseSensitive))
			{
				return &Event;
			}
		}
		return nullptr;
	}

	const TCHAR* const DcrRunEventCallee = TEXT("RunEvent");

	// ------------------------------------------------------------------------------------------
	// Units and dispatches
	// ------------------------------------------------------------------------------------------

	namespace NamesPrivate
	{
		const TCHAR* const Prefixes[] = { TEXT("RigUnit_"), TEXT("RigVMFunction_"), TEXT("RigVMDispatch_"), TEXT("RigDispatch_") };

		struct FNodeTypeIndex
		{
			TMap<FString, TArray<FDcrNodeType>> UnitsByShort;
			TMap<FString, FDcrNodeType> UnitsByFull;
			TMap<FString, TArray<FDcrNodeType>> DispatchesByShort;
			TMap<FString, FDcrNodeType> DispatchesByFull;
			TMap<FString, TArray<FName>> TemplatesByName;
			TMap<FName, const UScriptStruct*> DispatchByNotation;
			TMap<const UScriptStruct*, FName> NotationByDispatch;
			int32 FunctionCount = INDEX_NONE;
			int32 FactoryCount = INDEX_NONE;

			static void AddByShort(TMap<FString, TArray<FDcrNodeType>>& Map, const FDcrNodeType& Type)
			{
				TArray<FDcrNodeType>& Entries = Map.FindOrAdd(GetDcrShortName(Type.Struct->GetName()));
				if (!Entries.ContainsByPredicate([&Type](const FDcrNodeType& Entry) { return Entry.Struct == Type.Struct; }))
				{
					Entries.Add(Type);
				}
			}

			void Build()
			{
				const FRigVMRegistry& Registry = FRigVMRegistry::Get();
				const TChunkedArray<FRigVMFunction>& Functions = Registry.GetFunctions();
				const TArray<FRigVMDispatchFactory*>& Factories = Registry.GetFactories();
				if (Functions.Num() == FunctionCount && Factories.Num() == FactoryCount)
				{
					return;
				}
				*this = FNodeTypeIndex();
				FunctionCount = Functions.Num();
				FactoryCount = Factories.Num();

				for (int32 Index = 0; Index < Functions.Num(); ++Index)
				{
					const FRigVMFunction& Function = Functions[Index];
					if (!Function.Struct || Function.Factory)
					{
						continue;
					}
					FDcrNodeType Type;
					Type.Struct = Function.Struct;
					Type.bDeprecated = Function.Struct->HasMetaData(FRigVMStruct::DeprecatedMetaName);
					UnitsByFull.Add(Function.Struct->GetName(), Type);
					AddByShort(UnitsByShort, Type);
				}
				for (const FRigVMDispatchFactory* Factory : Factories)
				{
					const UScriptStruct* Struct = Factory ? Factory->GetScriptStruct() : nullptr;
					if (!Struct)
					{
						continue;
					}
					FDcrNodeType Type;
					Type.Struct = Struct;
					Type.bDispatch = true;
					Type.bDeprecated = Struct->HasMetaData(FRigVMStruct::DeprecatedMetaName);
					DispatchesByFull.Add(Struct->GetName(), Type);
					AddByShort(DispatchesByShort, Type);
					const FName Notation = Factory->GetTemplateNotation();
					DispatchByNotation.Add(Notation, Struct);
					NotationByDispatch.Add(Struct, Notation);
				}

				const TChunkedArray<FRigVMTemplate>& Templates = Registry.GetTemplates();
				for (int32 Index = 0; Index < Templates.Num(); ++Index)
				{
					const FRigVMTemplate& Template = Templates[Index];
					if (Template.UsesDispatch())
					{
						continue;
					}
					FString Name = Template.GetName().ToString();
					Name.ReplaceInline(TEXT(" "), TEXT(""));
					Name.ReplaceInline(TEXT("\t"), TEXT(""));
					TemplatesByName.FindOrAdd(Name).AddUnique(Template.GetNotation());
				}
			}
		};

		FNodeTypeIndex& GetNodeTypeIndex()
		{
			check(IsInGameThread());
			static FNodeTypeIndex Index;
			Index.Build();
			return Index;
		}

		TArray<FDcrNodeType> FindIn(const TMap<FString, FDcrNodeType>& ByFull, const TMap<FString, TArray<FDcrNodeType>>& ByShort, const FString& Name)
		{
			if (const FDcrNodeType* Full = ByFull.Find(Name))
			{
				return { *Full };
			}
			const TArray<FDcrNodeType>* Entries = ByShort.Find(Name);
			if (!Entries)
			{
				return {};
			}
			TArray<FDcrNodeType> Current = Entries->FilterByPredicate([](const FDcrNodeType& Entry) { return !Entry.bDeprecated; });
			return Current.IsEmpty() ? *Entries : Current;
		}
	}

	FString GetDcrShortName(const FString& StructName)
	{
		for (const TCHAR* Prefix : NamesPrivate::Prefixes)
		{
			if (StructName.StartsWith(Prefix, ESearchCase::CaseSensitive) && StructName.Len() > FCString::Strlen(Prefix))
			{
				return StructName.Mid(FCString::Strlen(Prefix));
			}
		}
		return StructName;
	}

	TArray<FDcrNodeType> FindDcrUnits(const FString& Name)
	{
		const NamesPrivate::FNodeTypeIndex& Index = NamesPrivate::GetNodeTypeIndex();
		return NamesPrivate::FindIn(Index.UnitsByFull, Index.UnitsByShort, Name);
	}

	TArray<FDcrNodeType> FindDcrDispatches(const FString& Name)
	{
		const NamesPrivate::FNodeTypeIndex& Index = NamesPrivate::GetNodeTypeIndex();
		return NamesPrivate::FindIn(Index.DispatchesByFull, Index.DispatchesByShort, Name);
	}

	TArray<FName> FindDcrTemplateNotations(const FString& Name)
	{
		const NamesPrivate::FNodeTypeIndex& Index = NamesPrivate::GetNodeTypeIndex();
		const TArray<FName>* Notations = Index.TemplatesByName.Find(Name);
		return Notations ? *Notations : TArray<FName>();
	}

	FString GetDcrNodeTypeName(const UScriptStruct& Struct)
	{
		const FString Short = GetDcrShortName(Struct.GetName());
		const TArray<FDcrNodeType> Units = FindDcrUnits(Short);
		const TArray<FDcrNodeType>& Found = Units.IsEmpty() ? FindDcrDispatches(Short) : Units;
		if (Found.Num() == 1 && Found[0].Struct == &Struct)
		{
			return Short;
		}
		return Struct.GetName();
	}

	const UScriptStruct* FindDcrDispatchStruct(const FString& Notation)
	{
		const NamesPrivate::FNodeTypeIndex& Index = NamesPrivate::GetNodeTypeIndex();
		const UScriptStruct* const* Struct = Index.DispatchByNotation.Find(FName(*Notation));
		return Struct ? *Struct : nullptr;
	}

	FString GetDcrDispatchNotation(const UScriptStruct& Struct)
	{
		const NamesPrivate::FNodeTypeIndex& Index = NamesPrivate::GetNodeTypeIndex();
		const FName* Notation = Index.NotationByDispatch.Find(&Struct);
		return Notation ? Notation->ToString() : FString();
	}

	// ------------------------------------------------------------------------------------------
	// Operators
	// ------------------------------------------------------------------------------------------

	namespace NamesPrivate
	{
		struct FOperatorTable
		{
			TArray<FDcrOperator> Binary;
			TArray<FDcrOperator> Unary;
			FDcrOperator Conditional;
			FDcrOperator Index;

			FOperatorTable()
			{
				auto AddBinary = [this](EDabBinaryOp Op, const TCHAR* Node, bool bDispatch, const TCHAR* A = TEXT("A"), const TCHAR* B = TEXT("B"))
				{
					FDcrOperator& Entry = Binary.AddDefaulted_GetRef();
					Entry.Kind = EDcrOperatorKind::Binary;
					Entry.Binary = Op;
					Entry.NodeName = Node;
					Entry.bDispatch = bDispatch;
					Entry.Operands = { A, B };
					Entry.Result = TEXT("Result");
				};
				AddBinary(EDabBinaryOp::Or, TEXT("Or"), false);
				AddBinary(EDabBinaryOp::And, TEXT("And"), false);
				AddBinary(EDabBinaryOp::Equal, TEXT("RigVMDispatch_CoreEquals"), true);
				AddBinary(EDabBinaryOp::NotEqual, TEXT("RigVMDispatch_CoreNotEquals"), true);
				AddBinary(EDabBinaryOp::Less, TEXT("Less"), false);
				AddBinary(EDabBinaryOp::LessEqual, TEXT("LessEqual"), false);
				AddBinary(EDabBinaryOp::Greater, TEXT("Greater"), false);
				AddBinary(EDabBinaryOp::GreaterEqual, TEXT("GreaterEqual"), false);
				AddBinary(EDabBinaryOp::Add, TEXT("Add"), false);
				AddBinary(EDabBinaryOp::Subtract, TEXT("Subtract"), false);
				AddBinary(EDabBinaryOp::Multiply, TEXT("Multiply"), false);
				AddBinary(EDabBinaryOp::Multiply, TEXT("Scale"), false, TEXT("Value"), TEXT("Factor"));
				AddBinary(EDabBinaryOp::Divide, TEXT("Divide"), false);
				AddBinary(EDabBinaryOp::Modulo, TEXT("Modulo"), false);

				auto AddUnary = [this](EDabUnaryOp Op, const TCHAR* Node)
				{
					FDcrOperator& Entry = Unary.AddDefaulted_GetRef();
					Entry.Kind = EDcrOperatorKind::Unary;
					Entry.Unary = Op;
					Entry.NodeName = Node;
					Entry.Operands = { TEXT("Value") };
					Entry.Result = TEXT("Result");
				};
				AddUnary(EDabUnaryOp::Not, TEXT("Not"));
				AddUnary(EDabUnaryOp::Negate, TEXT("Negate"));

				Conditional.Kind = EDcrOperatorKind::Conditional;
				Conditional.NodeName = TEXT("RigVMDispatch_If");
				Conditional.bDispatch = true;
				Conditional.Operands = { TEXT("Condition"), TEXT("True"), TEXT("False") };
				Conditional.Result = TEXT("Result");

				Index.Kind = EDcrOperatorKind::Index;
				Index.NodeName = TEXT("RigVMDispatch_ArrayGetAtIndex");
				Index.bDispatch = true;
				Index.Operands = { TEXT("Array"), TEXT("Index") };
				Index.Result = TEXT("Element");
			}
		};

		const FOperatorTable& GetOperatorTable()
		{
			static const FOperatorTable Table;
			return Table;
		}
	}

	TArray<const FDcrOperator*> GetDcrBinaryOperators(EDabBinaryOp Op)
	{
		TArray<const FDcrOperator*> Result;
		for (const FDcrOperator& Entry : NamesPrivate::GetOperatorTable().Binary)
		{
			if (Entry.Binary == Op)
			{
				Result.Add(&Entry);
			}
		}
		return Result;
	}

	const FDcrOperator* GetDcrUnaryOperator(EDabUnaryOp Op)
	{
		for (const FDcrOperator& Entry : NamesPrivate::GetOperatorTable().Unary)
		{
			if (Entry.Unary == Op)
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	const FDcrOperator& GetDcrConditionalOperator()
	{
		return NamesPrivate::GetOperatorTable().Conditional;
	}

	const FDcrOperator& GetDcrIndexOperator()
	{
		return NamesPrivate::GetOperatorTable().Index;
	}

	const FDcrOperator* FindDcrOperatorForStruct(const UScriptStruct& Struct)
	{
		const NamesPrivate::FOperatorTable& Table = NamesPrivate::GetOperatorTable();
		const FString StructName = Struct.GetName();
		for (const FDcrOperator* Dispatch : { &Table.Conditional, &Table.Index })
		{
			if (StructName == Dispatch->NodeName)
			{
				return Dispatch;
			}
		}
		const FString TemplateName = Struct.GetMetaData(FRigVMRegistry::TemplateNameMetaName);
		auto HasOperands = [&Struct](const FDcrOperator& Entry)
		{
			for (const TCHAR* Pin : Entry.Operands)
			{
				if (!Struct.FindPropertyByName(Pin))
				{
					return false;
				}
			}
			return Struct.FindPropertyByName(Entry.Result) != nullptr;
		};
		for (const TArray<FDcrOperator>* Group : { &Table.Binary, &Table.Unary })
		{
			for (const FDcrOperator& Entry : *Group)
			{
				const bool bNameMatches = Entry.bDispatch ? StructName == Entry.NodeName : (!TemplateName.IsEmpty() && TemplateName == Entry.NodeName);
				if (bNameMatches && (Entry.bDispatch || HasOperands(Entry)))
				{
					return &Entry;
				}
			}
		}
		return nullptr;
	}

	const UScriptStruct* GetDcrDoubleTwin(const UScriptStruct& Struct)
	{
		static const FString FloatPrefix = TEXT("RigVMFunction_MathFloat");
		const FString Name = Struct.GetName();
		if (!Name.StartsWith(FloatPrefix, ESearchCase::CaseSensitive))
		{
			return nullptr;
		}
		const FString TwinName = TEXT("RigVMFunction_MathDouble") + Name.Mid(FloatPrefix.Len());
		return FindObject<UScriptStruct>(Struct.GetOuter(), *TwinName);
	}

	// ------------------------------------------------------------------------------------------
	// Assets
	// ------------------------------------------------------------------------------------------

	namespace NamesPrivate
	{
		struct FAssetEntry
		{
			FString ObjectPath;
			FTopLevelAssetPath ClassPath;
		};

		/** Registered assets by name, kept current through the registry's events. */
		class FAssetIndex
		{
		public:
			const TArray<FAssetEntry>* Find(const FString& Name)
			{
				check(IsInGameThread());
				Build();
				return ByName.Find(Name);
			}

		private:
			TMap<FString, TArray<FAssetEntry>> ByName;
			bool bBuilt = false;
			bool bSubscribed = false;

			void Build()
			{
				IAssetRegistry* Registry = IAssetRegistry::Get();
				if (!Registry)
				{
					return;
				}
				if (!bSubscribed)
				{
					bSubscribed = true;
					Registry->OnAssetAdded().AddLambda([this](const FAssetData&) { bBuilt = false; });
					Registry->OnAssetRemoved().AddLambda([this](const FAssetData&) { bBuilt = false; });
					Registry->OnAssetRenamed().AddLambda([this](const FAssetData&, const FString&) { bBuilt = false; });
				}
				if (bBuilt)
				{
					return;
				}
				if (Registry->IsLoadingAssets())
				{
					Registry->WaitForCompletion();
				}
				ByName.Reset();
				const FTopLevelAssetPath RedirectorPath = UObjectRedirector::StaticClass()->GetClassPathName();
				Registry->EnumerateAllAssets([this, &RedirectorPath](const FAssetData& Asset)
				{
					if (Asset.AssetClassPath != RedirectorPath)
					{
						ByName.FindOrAdd(Asset.AssetName.ToString()).Add({ Asset.GetObjectPathString(), Asset.AssetClassPath });
					}
					return true;
				});
				bBuilt = true;
			}
		};

		FAssetIndex& GetAssetIndex()
		{
			static FAssetIndex Index;
			return Index;
		}
	}

	FString SplitDcrObjectPath(const FString& ObjectPath, FString& OutRel)
	{
		FString Package = FPackageName::ObjectPathToPackageName(ObjectPath);
		Package.RemoveFromStart(TEXT("/"));
		FString Mount;
		if (!Package.Split(TEXT("/"), &Mount, &OutRel))
		{
			OutRel = Package;
			return TEXT("Game");
		}
		if (Mount == TEXT("Game") || Mount == TEXT("Engine"))
		{
			return Mount;
		}
		return TEXT("Plugin.") + Mount;
	}

	TArray<FString> FindDcrAssetsByName(const FString& Name, const UClass* Class)
	{
		TArray<FString> Result;
		const TArray<NamesPrivate::FAssetEntry>* Entries = NamesPrivate::GetAssetIndex().Find(Name);
		if (!Entries)
		{
			return Result;
		}
		for (const NamesPrivate::FAssetEntry& Entry : *Entries)
		{
			if (Class)
			{
				const UClass* AssetClass = FindObject<UClass>(Entry.ClassPath);
				if (!AssetClass || !AssetClass->IsChildOf(Class))
				{
					continue;
				}
			}
			Result.AddUnique(Entry.ObjectPath);
		}
		return Result;
	}

	FDabExprPtr MakeDcrAssetExpr(const FSoftObjectPath& Path)
	{
		if (Path.IsNull())
		{
			return MakeDcrName(TEXT("None"));
		}
		const FString ObjectPath = Path.GetAssetPathString();
		const FString AssetName = Path.GetAssetName();
		if (AssetName != TEXT("None"))
		{
			const TArray<FString> Same = FindDcrAssetsByName(AssetName);
			if (Same.Num() == 1 && Same[0] == ObjectPath)
			{
				return MakeDcrName(AssetName);
			}
		}
		FString Rel;
		const FString Root = SplitDcrObjectPath(ObjectPath, Rel);
		FString Left;
		FString Right;
		FDabExprPtr RootExpr = Root.Split(TEXT("."), &Left, &Right) ? MakeDcrMember(MakeDcrName(Left), Right) : MakeDcrName(Root);
		TArray<FDabExprPtr> Args;
		Args.Add(MoveTemp(RootExpr));
		Args.Add(MakeDcrString(Rel));
		return MakeDcrCall(TEXT("Path"), MoveTemp(Args));
	}

	FString GetDcrPathExprObjectPath(const FDabExpr& Expr)
	{
		const FDabCallExpr* Call = Expr.As<FDabCallExpr>();
		const FDabNameExpr* Callee = Call && Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
		if (!Callee || Callee->Name != TEXT("Path") || Call->Args.Num() != 2 || Call->Args[0]->IsNamed() || Call->Args[1]->IsNamed())
		{
			return FString();
		}
		const FDabLiteralExpr* Rel = Call->Args[1]->Value ? Call->Args[1]->Value->As<FDabLiteralExpr>() : nullptr;
		TArray<FString> RootSegments;
		if (!Rel || Rel->LiteralKind != EDabLiteralKind::String || !Call->Args[0]->Value || !TryGetDabPath(*Call->Args[0]->Value, RootSegments))
		{
			return FString();
		}
		FString Mount;
		if (RootSegments.Num() == 1 && (RootSegments[0] == TEXT("Game") || RootSegments[0] == TEXT("Engine")))
		{
			Mount = RootSegments[0];
		}
		else if (RootSegments.Num() == 2 && RootSegments[0] == TEXT("Plugin"))
		{
			Mount = RootSegments[1];
		}
		else
		{
			return FString();
		}
		FString RelPath = Rel->Text;
		RelPath.RemoveFromStart(TEXT("/"));
		int32 Slash = INDEX_NONE;
		const FString Leaf = RelPath.FindLastChar(TEXT('/'), Slash) ? RelPath.Mid(Slash + 1) : RelPath;
		return FString::Printf(TEXT("/%s/%s.%s"), *Mount, *RelPath, *Leaf);
	}

	// ------------------------------------------------------------------------------------------
	// Types
	// ------------------------------------------------------------------------------------------

	namespace NamesPrivate
	{
		const TCHAR* const Builtins[] = {
			TEXT("bool"), TEXT("int32"), TEXT("int64"), TEXT("uint8"), TEXT("float"), TEXT("double"), TEXT("FName"), TEXT("FString"), TEXT("FText"),
		};

		bool IsBuiltin(const FString& CPPType)
		{
			for (const TCHAR* Builtin : Builtins)
			{
				if (CPPType.Equals(Builtin, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		bool IsNative(const UObject& Object)
		{
			return Object.GetOutermost()->GetName().StartsWith(TEXT("/Script/"));
		}

		/** Native objects of class T named Name (no prefix games). */
		template <typename T>
		TArray<T*> FindNative(const FString& Name)
		{
			TArray<UObject*> Found;
			StaticFindAllObjects(Found, T::StaticClass(), *Name, EFindObjectFlags::ExactClass);
			TArray<T*> Result;
			for (UObject* Object : Found)
			{
				if (T* Typed = Cast<T>(Object); Typed && IsNative(*Typed) && !Cast<UUserDefinedStruct>(Object) && !Cast<UUserDefinedEnum>(Object))
				{
					Result.AddUnique(Typed);
				}
			}
			return Result;
		}

		/** The native name of an enum as the text writes it: `ERigControlAxis` -> `RigControlAxis`. */
		FString StripEnumPrefix(const FString& Name)
		{
			return Name.Len() > 1 && Name[0] == TEXT('E') && FChar::IsUpper(Name[1]) ? Name.Mid(1) : Name;
		}
	}

	Model::FDcrType MakeDcrType(const UObject& TypeObject, bool bArray)
	{
		Model::FDcrType Type;
		Type.CPPType = RigVMTypeUtils::CPPTypeFromObject(&TypeObject, RigVMTypeUtils::EClassArgType::AsObject);
		if (bArray)
		{
			Type.CPPType = RigVMTypeUtils::ArrayTypeFromBaseType(Type.CPPType);
		}
		Type.ObjectPath = TypeObject.GetPathName();
		return Type;
	}

	Model::FDcrType MakeDcrBuiltinType(const TCHAR* CPPType, bool bArray)
	{
		Model::FDcrType Type;
		Type.CPPType = bArray ? RigVMTypeUtils::ArrayTypeFromBaseType(CPPType) : FString(CPPType);
		return Type;
	}

	bool FindDcrType(const FString& InSpelling, Model::FDcrType& OutType, FString& OutError)
	{
		using namespace NamesPrivate;
		const FString Spelling = InSpelling.TrimStartAndEnd();
		for (const TCHAR* ArrayPrefix : { TEXT("Array<"), TEXT("TArray<") })
		{
			if (Spelling.StartsWith(ArrayPrefix, ESearchCase::CaseSensitive) && Spelling.EndsWith(TEXT(">")))
			{
				const int32 PrefixLen = FCString::Strlen(ArrayPrefix);
				Model::FDcrType Element;
				if (!FindDcrType(Spelling.Mid(PrefixLen, Spelling.Len() - PrefixLen - 1), Element, OutError))
				{
					return false;
				}
				if (RigVMTypeUtils::IsArrayType(Element.CPPType))
				{
					OutError = FString::Printf(TEXT("`%s`: arrays of arrays are not supported"), *Spelling);
					return false;
				}
				OutType = Element;
				OutType.CPPType = RigVMTypeUtils::ArrayTypeFromBaseType(Element.CPPType);
				return true;
			}
		}

		if (IsBuiltin(Spelling))
		{
			OutType = MakeDcrBuiltinType(*Spelling);
			return true;
		}

		TArray<UObject*> Candidates;
		auto AddNative = [&Candidates](const FString& Name)
		{
			for (UScriptStruct* Struct : FindNative<UScriptStruct>(Name))
			{
				Candidates.AddUnique(Struct);
			}
		};
		AddNative(Spelling);
		if (Spelling.Len() > 1 && Spelling[0] == TEXT('F'))
		{
			AddNative(Spelling.Mid(1));
		}
		if (Candidates.IsEmpty())
		{
			for (UEnum* Enum : FindNative<UEnum>(TEXT("E") + Spelling))
			{
				Candidates.AddUnique(Enum);
			}
			for (UEnum* Enum : FindNative<UEnum>(Spelling))
			{
				Candidates.AddUnique(Enum);
			}
		}
		if (Candidates.IsEmpty())
		{
			for (const UClass* AssetClass : { UUserDefinedStruct::StaticClass(), static_cast<UClass*>(UUserDefinedEnum::StaticClass()) })
			{
				for (const FString& ObjectPath : FindDcrAssetsByName(Spelling, AssetClass))
				{
					if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad())
					{
						Candidates.AddUnique(Asset);
					}
				}
			}
		}
		if (Candidates.IsEmpty())
		{
			for (const FString& Name : { Spelling, Spelling.Len() > 1 && Spelling[0] == TEXT('U') ? Spelling.Mid(1) : FString() })
			{
				if (!Name.IsEmpty())
				{
					for (UClass* Class : FindNative<UClass>(Name))
					{
						Candidates.AddUnique(Class);
					}
				}
			}
		}

		if (Candidates.Num() == 1)
		{
			OutType = MakeDcrType(*Candidates[0]);
			return true;
		}
		OutError = Candidates.IsEmpty()
			? FString::Printf(TEXT("no type is called `%s`"), *Spelling)
			: FString::Printf(TEXT("`%s` names %d types; write its full name"), *Spelling, Candidates.Num());
		return false;
	}

	FString GetDcrTypeName(const Model::FDcrType& Type)
	{
		using namespace NamesPrivate;
		const bool bArray = RigVMTypeUtils::IsArrayType(Type.CPPType);
		const FString Base = bArray ? RigVMTypeUtils::BaseTypeFromArrayType(Type.CPPType) : Type.CPPType;

		TArray<FString, TInlineAllocator<3>> Spellings;
		if (Type.ObjectPath.IsEmpty())
		{
			if (IsBuiltin(Base))
			{
				Spellings.Add(Base);
			}
		}
		else if (const UObject* Object = RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Type.ObjectPath))
		{
			if (Cast<UUserDefinedStruct>(Object) || Cast<UUserDefinedEnum>(Object))
			{
				Spellings.Add(Object->GetName());
			}
			else if (Cast<UScriptStruct>(Object))
			{
				Spellings.Add(Object->GetName());
				Spellings.Add(TEXT("F") + Object->GetName());
			}
			else if (Cast<UEnum>(Object))
			{
				Spellings.Add(StripEnumPrefix(Object->GetName()));
				Spellings.Add(Object->GetName());
			}
			else if (const UClass* Class = Cast<UClass>(Object); Class && Base.StartsWith(TEXT("TObjectPtr<")))
			{
				Spellings.Add(Class->GetName());
				Spellings.Add(FString(Class->GetPrefixCPP()) + Class->GetName());
			}
		}

		for (const FString& Spelling : Spellings)
		{
			const FString Written = bArray ? FString::Printf(TEXT("Array<%s>"), *Spelling) : Spelling;
			Model::FDcrType Back;
			FString Error;
			if (FindDcrType(Written, Back, Error) && Back == Type)
			{
				return Written;
			}
		}
		return FString();
	}

	// ------------------------------------------------------------------------------------------
	// Pins
	// ------------------------------------------------------------------------------------------

	bool IsDcrExecuteProperty(const FProperty& Property)
	{
		const FStructProperty* Struct = CastField<FStructProperty>(&Property);
		return Struct && Struct->Struct && Struct->Struct->IsChildOf(FRigVMExecutePin::StaticStruct());
	}

	namespace NamesPrivate
	{
		Model::FDcrType TypeFromIndex(TRigVMTypeIndex TypeIndex)
		{
			const FRigVMTemplateArgumentType& ArgumentType = FRigVMRegistry::Get().GetType(TypeIndex);
			Model::FDcrType Type;
			Type.CPPType = ArgumentType.CPPType.ToString();
			if (ArgumentType.CPPTypeObject)
			{
				Type.ObjectPath = ArgumentType.CPPTypeObject->GetPathName();
			}
			return Type;
		}
	}

	TArray<FDcrPin> GetDcrNodePins(const UScriptStruct& Struct)
	{
		TArray<FDcrPin> Pins;
		if (Struct.IsChildOf(FRigVMDispatchFactory::StaticStruct()))
		{
			const FRigVMDispatchFactory* Factory = FRigVMRegistry::Get().FindDispatchFactory(FRigVMDispatchFactory::GetFactoryName(&Struct));
			const FRigVMTemplate* Template = Factory ? Factory->GetTemplate() : nullptr;
			if (!Template)
			{
				return Pins;
			}
			for (int32 Index = 0; Index < Template->NumArguments(); ++Index)
			{
				const FRigVMTemplateArgument* Argument = Template->GetArgument(Index);
				const ERigVMPinDirection Direction = Argument->GetDirection();
				if (Direction == ERigVMPinDirection::Hidden || Argument->IsExecute())
				{
					continue;
				}
				FDcrPin& Pin = Pins.AddDefaulted_GetRef();
				Pin.Name = Argument->GetName().ToString();
				Pin.bInput = Direction == ERigVMPinDirection::Input || Direction == ERigVMPinDirection::IO || Direction == ERigVMPinDirection::Visible;
				Pin.bOutput = Direction == ERigVMPinDirection::Output || Direction == ERigVMPinDirection::IO;
				if (Argument->IsSingleton())
				{
					Pin.Type = NamesPrivate::TypeFromIndex(Argument->GetTypeIndex(0));
				}
			}
			return Pins;
		}

		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			FProperty* Property = *It;
			const ERigVMPinDirection Direction = FRigVMStruct::GetPinDirectionFromProperty(Property);
			if (Direction == ERigVMPinDirection::Hidden || IsDcrExecuteProperty(*Property))
			{
				continue;
			}
			FDcrPin& Pin = Pins.AddDefaulted_GetRef();
			Pin.Name = Property->GetName();
			Pin.bInput = Direction == ERigVMPinDirection::Input || Direction == ERigVMPinDirection::IO || Direction == ERigVMPinDirection::Visible;
			Pin.bOutput = Direction == ERigVMPinDirection::Output || Direction == ERigVMPinDirection::IO;
			Pin.Property = Property;
			Pin.Type = GetDcrType(*Property);
		}
		return Pins;
	}

	// ------------------------------------------------------------------------------------------
	// Operator choice
	// ------------------------------------------------------------------------------------------

	bool CanDcrLiteralBe(const FDabExpr& Literal, const Model::FDcrType& Type)
	{
		FDcrValueSlot Slot;
		if (!Slot.Init(Type))
		{
			return false;
		}
		TArray<FDcrValuePart> Parts;
		FString Error;
		const bool bWritten = WriteDcrValueExpr(*Slot.GetProperty(), Slot.GetMemory(), Literal,
			[](const FDabExpr& Expr, const FProperty&) { return IsDcrConstantLiteral(Expr) || Expr.Is<FDabNameExpr>() || Expr.Is<FDabMemberExpr>(); },
			[](const FDabExpr&, const UClass*) -> UObject* { return nullptr; },
			Parts, Error);
		return bWritten && Parts.IsEmpty();
	}

	static FDcrOperatorChoice ChooseDcrOperatorUnit(TConstArrayView<const FDcrOperator*> Candidates, TConstArrayView<FDcrOperand> Operands,
		const Model::FDcrType* Result)
	{
		static const FString Double(TEXT("double"));
		static const FString Float(TEXT("float"));
		auto IsSwap = [](const FString& A, const FString& B)
		{
			const FString BaseA = RigVMTypeUtils::IsArrayType(A) ? RigVMTypeUtils::BaseTypeFromArrayType(A) : A;
			const FString BaseB = RigVMTypeUtils::IsArrayType(B) ? RigVMTypeUtils::BaseTypeFromArrayType(B) : B;
			return RigVMTypeUtils::IsArrayType(A) == RigVMTypeUtils::IsArrayType(B)
				&& ((BaseA == Double && BaseB == Float) || (BaseA == Float && BaseB == Double));
		};

		FDcrOperatorChoice Best;
		int32 BestSwaps = MAX_int32;
		int32 BestDoubles = -1;
		for (const FDcrOperator* Candidate : Candidates)
		{
			if (!Candidate || Candidate->bDispatch || Candidate->Operands.Num() != Operands.Num())
			{
				continue;
			}
			for (const FName& Notation : FindDcrTemplateNotations(Candidate->NodeName))
			{
				const FRigVMTemplate* Template = FRigVMRegistry::Get().FindTemplate(Notation);
				if (!Template)
				{
					continue;
				}
				for (int32 Index = 0; Index < Template->NumPermutations(); ++Index)
				{
					const FRigVMFunction* Function = Template->GetPermutation(Index);
					const UScriptStruct* Unit = Function ? Function->Struct : nullptr;
					if (!Unit || Unit->HasMetaData(FRigVMStruct::DeprecatedMetaName))
					{
						continue;
					}
					if (Result && Candidate->Result)
					{
						const FProperty* ResultPin = Unit->FindPropertyByName(Candidate->Result);
						const Model::FDcrType ResultType = ResultPin ? GetDcrType(*ResultPin) : Model::FDcrType();
						if (!ResultPin || (ResultType != *Result && !IsSwap(ResultType.CPPType, Result->CPPType)))
						{
							continue;
						}
					}
					int32 Swaps = 0;
					int32 Doubles = 0;
					bool bFits = true;
					for (int32 Operand = 0; Operand < Operands.Num() && bFits; ++Operand)
					{
						const FProperty* Pin = Unit->FindPropertyByName(Candidate->Operands[Operand]);
						if (!Pin)
						{
							bFits = false;
							break;
						}
						const Model::FDcrType PinType = GetDcrType(*Pin);
						Doubles += PinType.CPPType.Contains(Double) ? 1 : 0;
						const FDcrOperand& Given = Operands[Operand];
						if (Given.Type.IsSet())
						{
							if (Given.Type == PinType)
							{
								continue;
							}
							if (IsSwap(Given.Type.CPPType, PinType.CPPType))
							{
								++Swaps;
								continue;
							}
							bFits = false;
						}
						else if (Given.Fits)
						{
							bFits = Given.Fits(PinType);
						}
						else
						{
							bFits = Given.Literal && CanDcrLiteralBe(*Given.Literal, PinType);
						}
					}
					if (bFits && (Swaps < BestSwaps || (Swaps == BestSwaps && Doubles > BestDoubles)))
					{
						Best.Operator = Candidate;
						Best.Unit = Unit;
						BestSwaps = Swaps;
						BestDoubles = Doubles;
					}
				}
			}
		}
		return Best;
	}

	FDcrOperatorChoice ResolveDcrOperatorUnit(TConstArrayView<const FDcrOperator*> Candidates, TConstArrayView<FDcrOperand> Operands,
		const Model::FDcrType* Result)
	{
		FDcrOperatorChoice Choice;
		if (Result && Result->IsSet())
		{
			Choice = ChooseDcrOperatorUnit(Candidates, Operands, Result);
		}
		if (!Choice.Unit)
		{
			Choice = ChooseDcrOperatorUnit(Candidates, Operands, nullptr);
		}
		// Float math is built as its double twin, as a call by name is: the text holds no float math of its own.
		if (const UScriptStruct* Twin = Choice.Unit ? GetDcrDoubleTwin(*Choice.Unit) : nullptr)
		{
			Choice.Unit = Twin;
		}
		return Choice;
	}
}
