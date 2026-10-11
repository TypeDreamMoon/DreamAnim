// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrLower.h"

#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
#include "Lang/DabPrinter.h"
#include "Misc/ScopeExit.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrHierarchyImport.h"
#include "Rig/DcrNames.h"
#include "Rig/DcrSyntax.h"
#include "Rig/DcrValues.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMGraphFunctionDefinition.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMTypeUtils.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigModuleDefines.h"
#include "Units/Modules/RigUnit_ConnectionCandidates.h"
#include "Units/Modules/RigUnit_ConnectorExecution.h"
#include "UObject/StructOnScope.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	namespace LowerPrivate
	{
		// ------------------------------------------------------------------------------------------
		// Small pieces
		// ------------------------------------------------------------------------------------------

		/** `Node.Root.Sub.Path` split at the first two dots. */
		struct FPinPath
		{
			FString Node;
			FString Root;
			FString Sub;
		};

		FPinPath SplitPinPath(const FString& Path)
		{
			FPinPath Result;
			FString Rest;
			if (!Path.Split(TEXT("."), &Result.Node, &Rest))
			{
				Result.Node = Path;
				return Result;
			}
			if (!Rest.Split(TEXT("."), &Result.Root, &Result.Sub))
			{
				Result.Root = Rest;
			}
			return Result;
		}

		FString JoinPath(const FString& A, const FString& B)
		{
			return A.IsEmpty() ? B : B.IsEmpty() ? A : A + TEXT(".") + B;
		}

		/** Follows Sub (`Translation.X`, `0.Item`) from a value of Property; the property found, or null. */
		const FProperty* WalkSubPath(const FProperty* Property, const FString& Sub)
		{
			if (Sub.IsEmpty())
			{
				return Property;
			}
			TArray<FString> Segments;
			Sub.ParseIntoArray(Segments, TEXT("."));
			for (const FString& Segment : Segments)
			{
				if (!Property)
				{
					return nullptr;
				}
				if (const FArrayProperty* Array = CastField<FArrayProperty>(Property); Array && Segment.IsNumeric())
				{
					Property = Array->Inner;
				}
				else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
				{
					Property = Struct->Struct->FindPropertyByName(*Segment);
				}
				else
				{
					return nullptr;
				}
			}
			return Property;
		}

		/** `.Translation.X` or `[0].Item` after Base; user struct members by display name when the root property is known. */
		FDabExprPtr AppendSubPath(FDabExprPtr Base, const FProperty* Property, const FString& Sub)
		{
			if (Sub.IsEmpty())
			{
				return Base;
			}
			TArray<FString> Segments;
			Sub.ParseIntoArray(Segments, TEXT("."));
			for (const FString& Segment : Segments)
			{
				const FArrayProperty* Array = CastField<FArrayProperty>(Property);
				if (Segment.IsNumeric())
				{
					Base = MakeDcrIndex(MoveTemp(Base), MakeDcrInteger(FCString::Atoi64(*Segment)));
					Property = Array ? Array->Inner : nullptr;
					continue;
				}
				const FStructProperty* Struct = CastField<FStructProperty>(Property);
				const FProperty* Member = Struct ? Struct->Struct->FindPropertyByName(*Segment) : nullptr;
				Base = MakeDcrMember(MoveTemp(Base), Member ? GetDcrMemberName(*Member) : Segment);
				Property = Member;
			}
			return Base;
		}

		/** `DISPATCH_RigVMDispatch_SelectInt32_1` -> `SelectInt32_1`, `Get Transform 2` -> `GetTransform2`: a node name as a let name. */
		FString MakeLetName(const FString& NodeName)
		{
			FString Name = NodeName;
			Name.RemoveFromStart(TEXT("DISPATCH_"));
			Name = GetDcrShortName(Name);
			FString Result;
			for (const TCHAR Char : Name)
			{
				if (FChar::IsAlnum(Char) || Char == TEXT('_'))
				{
					Result.AppendChar(Char);
				}
			}
			if (Result.IsEmpty() || FChar::IsDigit(Result[0]))
			{
				Result = TEXT("Node") + Result;
			}
			return Result;
		}

		/** Names the text reads as something else wherever a let or a loop name could stand. */
		bool IsReservedName(const FString& Name)
		{
			static const TCHAR* const Reserved[] = {
				TEXT("Bone"), TEXT("Null"), TEXT("Control"), TEXT("Curve"), TEXT("Connector"), TEXT("Socket"), TEXT("Path"), TEXT("Injected"),
				TEXT("Unsupported"), TEXT("Range"), TEXT("true"), TEXT("false"), TEXT("None"), TEXT("Self"), TEXT("Root"), TEXT("Plugin"),
				TEXT("Game"), TEXT("Engine"),
			};
			for (const TCHAR* Word : Reserved)
			{
				if (Name.Equals(Word, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return IsDabCodeWord(Name);
		}

		// One comment box as the comment above a statement: `//` lines; a second box on the same statement as a block
		// comment of its own, so the two read back as two.
		void AddDcrCommentTrivia(FDabTrivia& Trivia, const FString& Text)
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			if (!Trivia.Leading.IsEmpty())
			{
				FDabComment Block;
				Block.bBlock = true;
				Block.Text = TEXT("/* ") + FString::Join(Lines, TEXT("\n")).TrimEnd() + TEXT(" */");
				Trivia.Leading.Add(Block);
				return;
			}
			for (const FString& Line : Lines)
			{
				FDabComment Comment;
				Comment.Text = (TEXT("// ") + Line).TrimEnd();
				Trivia.Leading.Add(Comment);
			}
		}

		TUniquePtr<FDabMetaEntry> MakeMetaEntry(const FString& Key, FDabExprPtr Value)
		{
			TUniquePtr<FDabMetaEntry> Entry = MakeUnique<FDabMetaEntry>();
			Entry->Key = Key;
			Entry->Value = MoveTemp(Value);
			return Entry;
		}

		TUniquePtr<FDabAssignStmt> MakeAssign(const FString& Name, FDabExprPtr Value)
		{
			TUniquePtr<FDabAssignStmt> Assign = MakeUnique<FDabAssignStmt>();
			Assign->Name = Name;
			Assign->bNameIsString = !IsDabPlainIdentifier(Name) && Name.Contains(TEXT("\""));
			Assign->Value = MoveTemp(Value);
			return Assign;
		}

		FDabStmtPtr MakeExprStmt(FDabExprPtr Expr)
		{
			TUniquePtr<FDabExprStmt> Stmt = MakeUnique<FDabExprStmt>();
			Stmt->Expr = MoveTemp(Expr);
			return Stmt;
		}

		FDabStmtPtr MakeRegion(const FString& Label, bool bEnd)
		{
			TUniquePtr<FDabRegionStmt> Region = MakeUnique<FDabRegionStmt>();
			Region->bEnd = bEnd;
			Region->Label = Label;
			return Region;
		}

		// ------------------------------------------------------------------------------------------
		// A graph's links, indexed
		// ------------------------------------------------------------------------------------------

		struct FGraphIndex
		{
			const FDcrModelGraph* Graph = nullptr;
			TMap<FString, const FDcrModelNode*> Nodes;
			/** Data links by target pin path -> source pin path. */
			TMap<FString, FString> DataSource;
			/** Data links leaving each node, as target pin paths. */
			TMap<FString, TArray<FString>> DataTargets;
			/** Data links into each node, as (target pin path, source pin path). */
			TMap<FString, TArray<TPair<FString, FString>>> DataInputs;
			/** Links into a root pin's sub-pins, by root pin path (`Node.Pin`) -> target pin paths below it. */
			TMap<FString, TArray<FString>> SubTargets;
			/** Execute links by source pin path (`Node.Pin`) -> target node. */
			TMap<FString, FString> ExecNext;
			TMap<FString, int32> ExecInCount;
			/** Injected nodes by the pin path they sit on. */
			TMap<FString, const FDcrModelNode*> InjectedInputs;
			TMap<FString, const FDcrModelNode*> InjectedOutputs;

			void Build(const FDcrModelGraph& InGraph)
			{
				Graph = &InGraph;
				for (const FDcrModelNode& Node : InGraph.Nodes)
				{
					Nodes.Add(Node.Name, &Node);
					if (!Node.InjectedOn.IsEmpty())
					{
						(Node.bInjectedAsInput ? InjectedInputs : InjectedOutputs).Add(Node.InjectedOn, &Node);
					}
				}
				for (const FDcrModelLink& Link : InGraph.Links)
				{
					const FPinPath Source = SplitPinPath(Link.Source);
					const FPinPath Target = SplitPinPath(Link.Target);
					const FDcrModelNode* const* SourceNode = Nodes.Find(Source.Node);
					if (SourceNode && Source.Sub.IsEmpty() && (*SourceNode)->ExecuteOut.Contains(Source.Root))
					{
						ExecNext.Add(Source.Node + TEXT(".") + Source.Root, Target.Node);
						ExecInCount.FindOrAdd(Target.Node)++;
						continue;
					}
					DataSource.Add(Link.Target, Link.Source);
					DataTargets.FindOrAdd(Source.Node).Add(Link.Target);
					DataInputs.FindOrAdd(Target.Node).Emplace(Link.Target, Link.Source);
					if (!Target.Sub.IsEmpty())
					{
						SubTargets.FindOrAdd(Target.Node + TEXT(".") + Target.Root).Add(Link.Target);
					}
				}
			}

			/** The data links into a node, as (target pin path, source pin path). */
			TConstArrayView<TPair<FString, FString>> InputsOf(const FString& NodeName) const
			{
				const TArray<TPair<FString, FString>>* Inputs = DataInputs.Find(NodeName);
				return Inputs ? TConstArrayView<TPair<FString, FString>>(*Inputs) : TConstArrayView<TPair<FString, FString>>();
			}

			const FDcrModelNode* Find(const FString& Name) const
			{
				const FDcrModelNode* const* Node = Nodes.Find(Name);
				return Node ? *Node : nullptr;
			}

			/** The node an execute pin leads to, or null. */
			const FDcrModelNode* Next(const FDcrModelNode& Node, const FString& Pin) const
			{
				const FString* Target = ExecNext.Find(Node.Name + TEXT(".") + Pin);
				return Target ? Find(*Target) : nullptr;
			}
		};

		// ------------------------------------------------------------------------------------------
		// What a node is
		// ------------------------------------------------------------------------------------------

		enum class ERole : uint8 { Event, Entry, Return, Statement, Data, Comment, Injected, Other };

		bool IsEventNode(const FDcrModelNode& Node)
		{
			return Node.Kind == EDcrNodeKind::Unit && (FindDcrEventByStruct(Node.StructPath) || Node.StructPath == DcrUserEventStructPath);
		}

		ERole GetRole(const FDcrModelNode& Node)
		{
			// A node injected on an input belongs to that input. One injected on an output is read like any data
			// node, and written `Injected(Node(...), Output)`.
			if (!Node.InjectedOn.IsEmpty() && Node.bInjectedAsInput)
			{
				return ERole::Injected;
			}
			switch (Node.Kind)
			{
			case EDcrNodeKind::Comment: return ERole::Comment;
			case EDcrNodeKind::Entry: return ERole::Entry;
			case EDcrNodeKind::Return: return ERole::Return;
			case EDcrNodeKind::Reroute:
			case EDcrNodeKind::Unknown: return ERole::Other;
			default: break;
			}
			if (IsEventNode(Node))
			{
				return ERole::Event;
			}
			return Node.ExecuteIn.IsEmpty() ? ERole::Data : ERole::Statement;
		}

		/** The execute pin a statement goes on by: Completed when there is one, else the pin it both takes and gives. */
		FString GetContinuationPin(const FDcrModelNode& Node)
		{
			if (Node.ExecuteOut.Contains(TEXT("Completed")))
			{
				return TEXT("Completed");
			}
			for (const FString& Pin : Node.ExecuteOut)
			{
				if (Node.ExecuteIn.Contains(Pin))
				{
					return Pin;
				}
			}
			return Node.ExecuteOut.Num() == 1 ? Node.ExecuteOut[0] : FString();
		}

		bool IsBranch(const FDcrModelNode& Node)
		{
			return Node.Kind == EDcrNodeKind::Unit && Node.StructPath.EndsWith(TEXT(".RigVMFunction_ControlFlowBranch"));
		}

		bool IsForLoopCount(const FDcrModelNode& Node)
		{
			return Node.Kind == EDcrNodeKind::Unit && Node.StructPath.EndsWith(TEXT(".RigVMFunction_ForLoopCount"));
		}

		bool IsArrayIterator(const FDcrModelNode& Node, const UScriptStruct* Struct)
		{
			return Node.Kind == EDcrNodeKind::Dispatch && Struct && Struct->GetName() == TEXT("RigVMDispatch_ArrayIterator");
		}

		const FDcrValue* FindValue(const FDcrModelNode& Node, const FString& Pin)
		{
			return Node.Values.FindByPredicate([&Pin](const FDcrValue& Value) { return Value.Name == Pin; });
		}

		/** A call's pins with their types and defaults. */
		struct FSignature
		{
			TArray<FDcrPin> Pins;
			/** Export text of each input's default, by pin name; missing means the type's default. */
			TMap<FString, FString> Defaults;
			bool bKnown = false;

			const FDcrPin* Find(const FString& Name) const
			{
				return Pins.FindByPredicate([&Name](const FDcrPin& Pin) { return Pin.Name == Name; });
			}
			int32 NumOutputs() const
			{
				int32 Count = 0;
				for (const FDcrPin& Pin : Pins)
				{
					Count += Pin.bOutput ? 1 : 0;
				}
				return Count;
			}
		};

		// ------------------------------------------------------------------------------------------
		// The file
		// ------------------------------------------------------------------------------------------

		class FFileLowering;

		/** One code body: an event, a function, a detached chain. */
		class FBodyLowering
		{
		public:
			FBodyLowering(FFileLowering& InFile, const FGraphIndex& InGraph, const FDcrModelFunction* InFunction, const FString& InLocation)
				: File(InFile), Graph(InGraph), Function(InFunction), Location(InLocation)
			{
			}

			/** The statements of the chain an execute pin starts (an event's, an entry's). */
			TUniquePtr<FDabCodeBlock> LowerChain(const FDcrModelNode& Start, const FString& Pin);
			/**
			 * The same, with chains nothing runs (from their heads) and data nothing reads written in a `Detached`
			 * block of this body, so that they can read its lets.
			 */
			TUniquePtr<FDabCodeBlock> LowerChain(const FDcrModelNode& Start, const FString& Pin, TConstArrayView<const FDcrModelNode*> DetachedHeads,
				TConstArrayView<const FDcrModelNode*> Dangling);
			/** The statements of chains no event runs, each from its head. */
			TUniquePtr<FDabCodeBlock> LowerDetached(TConstArrayView<const FDcrModelNode*> Heads, TConstArrayView<const FDcrModelNode*> Dangling);
			/** A pure function's value: its single output, or a tuple of them. */
			FDabExprPtr LowerPureReturn(const FDcrModelNode& Return);
			/** A pure function's code when its outputs share a node (a let names it) or it has locals; null otherwise. */
			TUniquePtr<FDabCodeBlock> LowerPureCode(const FDcrModelNode& Return);

			/**
			 * Places the graph's comment boxes that enclose statements of this code: a region around the
			 * statements a box encloses when they are consecutive in one block, a line comment on the first of
			 * them otherwise. Placed comments are taken out of Comments.
			 */
			void PlaceComments(FDabCodeBlock& Code, TArray<const FDcrModelNode*>& Comments);

			/** Nodes this body wrote, for the detached search. */
			TSet<FString> Visited;

		private:
			struct FSkelBlock;

			/** One statement of the skeleton: a node on the chain and the blocks under it. */
			struct FSkel
			{
				const FDcrModelNode* Node = nullptr;
				enum class EKind : uint8 { Call, If, For, Return, Detached } Kind = EKind::Call;
				TArray<TUniquePtr<FSkelBlock>> Blocks;
				/** Whether the next statement in the same block was already folded into this one (else-if). */
				int32 Ordinal = 0;
			};

			struct FSkelBlock
			{
				TArray<FSkel> Statements;
				FSkelBlock* Parent = nullptr;
				int32 IndexInParent = INDEX_NONE;
				int32 Depth = 0;
			};

			/** Where a let goes: before statement Index of Block. */
			struct FPlace
			{
				FSkelBlock* Block = nullptr;
				int32 Index = 0;
			};

			FFileLowering& File;
			const FGraphIndex& Graph;
			const FDcrModelFunction* Function;
			FString Location;

			TUniquePtr<FSkelBlock> Root;
			int32 NextOrdinal = 0;
			/** Uses of each data node within this body, by consumer pin path. */
			TMap<FString, int32> UseCounts;
			/** Data nodes and statement nodes this body writes as lets, with their names. */
			TMap<FString, FString> LetNames;
			/** Statement nodes written inside the statement that uses them (`FootTrace(...).Z`). */
			TSet<FString> InlinedStatements;
			/** Where each pure let goes. */
			TMap<FString, FPlace> LetPlaces;
			/** The skeleton statement of each statement node. */
			TMap<FString, FSkel*> SkelOf;
			TMap<FString, FSkelBlock*> BlockOf;
			/** Loop nodes' outputs by name: `Node.Pin` -> the loop name the text uses. */
			TMap<FString, FString> LoopNames;
			TSet<FString> UsedNames;
			/** Pure lets already written (each once). */
			TSet<FString> WrittenLets;
			/** Statement bodies in progress, to keep a node from recursing into itself. */
			TSet<FString> Building;
			/** The nodes each written statement stands for, for placing comment boxes. */
			TMap<const FDabStmt*, TArray<FString>> StmtNodes;
			/** Where nodes written right now are recorded. */
			TArray<FString>* Collecting = nullptr;
			/** Typed lets the expressions written right now need, written before the statement that holds them. */
			TArray<FDabStmtPtr> Hoisted;
			/** Whether the body is code (it can hold lets), not a pure function's value. */
			bool bCode = false;

			// Skeleton
			void BuildSkeleton(FSkelBlock& Block, const FDcrModelNode* Node);
			// Analysis
			void CountUses();
			void CountNodeInputs(const FDcrModelNode& Node, TSet<FString>& Seen);
			bool IsLetData(const FDcrModelNode& Node) const;
			void ChooseInlinedStatements(FSkelBlock& Block);
			void CollectStatementSources(const FDcrModelNode& Node, TArray<const FDcrModelNode*>& OutMutables, TSet<FString>& Seen, bool bTop) const;
			void CollectLetRefs(const FDcrModelNode& Node, TArray<FString>& OutLets, TSet<FString>& Seen, bool bTop) const;
			void PlaceLets(TConstArrayView<const FDcrModelNode*> Dangling, FSkelBlock* Fallback);
			bool PlaceDangling(const FDcrModelNode& Node, FPlace& OutPlace) const;
			FPlace PlaceOf(const FSkel& Skel) const;
			/** A Detached statement at At of Block, and the block it holds. */
			FSkelBlock& AddDetachedSkel(FSkelBlock& Block, int32 At, const FDcrModelNode& Node);
			/** The innermost block in which everything a detached chain reads is in scope; null when nothing it reads is in a block. */
			FSkelBlock* FindDetachedPlace(const FDcrModelNode& Head) const;
			FPlace Common(const FPlace& A, const FPlace& B) const;
			FString UniqueName(const FString& Wanted);
			/**
			 * The name a node's let starts from: the node's own, or for a collapse node its function's, which is what
			 * the text names the node after (the node's own name would not come back).
			 */
			FString LetNameFor(const FDcrModelNode& Node) const;
			// Writing
			void WriteBlock(const FSkelBlock& Block, TArray<FDabStmtPtr>& Out);
			void WriteLetsAt(const FSkelBlock& Block, int32 Index, TArray<FDabStmtPtr>& Out);
			void WriteLet(const FString& NodeName, TArray<FDabStmtPtr>& Out);
			FDabStmtPtr WriteStatement(const FSkel& Skel);
			FDabStmtPtr WriteIf(const FSkel& Skel);
			FDabStmtPtr WriteFor(const FSkel& Skel);
			FDabStmtPtr WriteReturn(const FDcrModelNode& Node);
			// Expressions
			FDabExprPtr NodeExpr(const FDcrModelNode& Node);
			FDabExprPtr SourceExpr(const FString& SourcePath);
			/** A variable node's name, or what the text cannot say when the rig has no such variable. */
			FDabExprPtr VariableExpr(const FDcrModelNode& Node);
			bool HasVariable(const FString& Name) const;
			/** A pin's value as the text writes it: what differs from the pin's default, or with bForType from its type's (for a let). */
			FDabExprPtr InputExpr(const FDcrModelNode& Node, const FDcrPin& Pin, const FSignature& Signature, bool bForce, bool bForType = false);
			const FDcrPin* FindTypingPin(const FDcrModelNode& Node, const FSignature& Signature);
			bool IsTypedByUses(const FDcrModelNode& Node, const FSignature& Signature);
			const FDcrPin* FindUntypedOutput(const FDcrModelNode& Node);
			FDabExprPtr TypedLetFor(const FDcrModelNode& Node, const FDcrPin& Pin, const FSignature& Signature);
			FDabExprPtr OperatorExpr(const FDcrModelNode& Node, const UScriptStruct& Struct, const FSignature& Signature);
			Model::FDcrType SourceType(const FString& SourcePath);
		};

		class FFileLowering
		{
			friend class FBodyLowering;

		public:
			FFileLowering(const FDcrModel& InModel, const FDcrLowerOptions& InOptions, FDabFile& InFile, TArray<FDcrModelUnsupported>& InUnsupported)
				: Model(InModel), Options(InOptions), File(InFile), Unsupported(InUnsupported)
			{
				for (const FDcrModelVariable& Variable : Model.Variables)
				{
					GlobalNames.Add(Variable.Name);
				}
				for (const FDcrModelFunction& Function : Model.Functions)
				{
					GlobalNames.Add(Function.Name);
				}
				for (const FDcrModelVariable& Variable : Model.Variables)
				{
					VariableNames.Add(Variable.Name);
				}
				for (const FDcrModelFunction& Function : Model.Functions)
				{
					for (const FDcrModelVariable& Local : Function.Locals)
					{
						VariableNames.Add(Local.Name);
					}
					for (const FDcrModelFunctionPin& Input : Function.Inputs)
					{
						VariableNames.Add(Input.Name);
					}
				}
			}

			const FDcrModel& Model;
			const FDcrLowerOptions& Options;
			FDabFile& File;
			TArray<FDcrModelUnsupported>& Unsupported;
			TSet<FString> GlobalNames;
			TMap<FString, int32> LibraryFunctionCounts;
			/** The rigs this one calls into, by asset name: two of one name cannot be told apart in `Library.Name`. */
			TMap<FString, int32> LibraryNameCounts;
			bool bLibrariesRead = false;
			/** Every variable, local and parameter of the rig: a variable node of any other name reads a variable that is gone. */
			TSet<FString> VariableNames;
			/** Function hosts of calls to other rigs' functions, by asset path, in first-use order. */
			TArray<FString> UsedLibraries;

			void AddUnsupported(const FString& Where, const FString& What)
			{
				Unsupported.Add({ Where, What });
			}

			/** How many of the rigs this one calls into have a public function of this name (read on first use). */
			int32 CountLibraryFunctions(const FString& Name)
			{
				if (!bLibrariesRead)
				{
					bLibrariesRead = true;
					TSet<FString> Hosts;
					TFunction<void(const FDcrModelGraph&)> Collect = [&Hosts, &Collect](const FDcrModelGraph& Graph)
					{
						for (const FDcrModelNode& Node : Graph.Nodes)
						{
							if (Node.Kind == EDcrNodeKind::ExternalCall)
							{
								Hosts.Add(Node.FunctionHost);
							}
							if (Node.Contained.IsValid())
							{
								Collect(*Node.Contained);
							}
						}
					};
					for (const FDcrModelGraph& Graph : Model.Graphs)
					{
						Collect(Graph);
					}
					for (const FDcrModelFunction& Function : Model.Functions)
					{
						Collect(Function.Graph);
					}
					for (FString Host : Hosts)
					{
						LibraryNameCounts.FindOrAdd(GetLibraryName(Host))++;
						Host.RemoveFromEnd(TEXT("_C"));
						const FDcrRig Rig = LoadDcrRig(Host);
						URigVMFunctionLibrary* Library = Rig.IsValid() ? Rig.RigVM->GetLocalFunctionLibrary() : nullptr;
						for (URigVMLibraryNode* Function : Library ? Library->GetFunctions() : TArray<URigVMLibraryNode*>())
						{
							if (Function && Library->IsFunctionPublic(Function->GetFName()))
							{
								LibraryFunctionCounts.FindOrAdd(Function->GetName())++;
							}
						}
					}
				}
				return LibraryFunctionCounts.FindRef(Name);
			}

			const UScriptStruct* FindStruct(const FString& Path)
			{
				if (const UScriptStruct* const* Found = Structs.Find(Path))
				{
					return *Found;
				}
				const UScriptStruct* Struct = Path.IsEmpty() ? nullptr : FindObject<UScriptStruct>(nullptr, *Path);
				if (!Struct && !Path.IsEmpty())
				{
					Struct = LoadObject<UScriptStruct>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
				}
				Structs.Add(Path, Struct);
				return Struct;
			}

			/** The unit or dispatch struct of a node; null for other kinds. */
			const UScriptStruct* GetNodeStruct(const FDcrModelNode& Node)
			{
				if (Node.Kind == EDcrNodeKind::Unit)
				{
					return FindStruct(Node.StructPath);
				}
				if (Node.Kind == EDcrNodeKind::Dispatch)
				{
					return FindDcrDispatchStruct(Node.Notation);
				}
				return nullptr;
			}

			const FDcrModelFunction* FindFunction(const FString& Name) const
			{
				return Model.Functions.FindByPredicate([&Name](const FDcrModelFunction& Function) { return Function.Name == Name; });
			}

			const FSignature& GetSignature(const FDcrModelNode& Node, const FDcrModelFunction* Enclosing);

			/** The export text of a fresh unit's member. */
			FString GetUnitDefault(const UScriptStruct& Struct, const FProperty& Property)
			{
				TSharedPtr<FStructOnScope>& Defaults = UnitDefaults.FindOrAdd(&Struct);
				if (!Defaults.IsValid())
				{
					Defaults = MakeShared<FStructOnScope>(&Struct);
				}
				FString Text;
				Property.ExportTextItem_Direct(Text, Property.ContainerPtrToValuePtr<void>(Defaults->GetStructMemory()), nullptr, nullptr, PPF_None);
				return Text;
			}

			FString GetLibraryName(const FString& HostPath) const;

			/** The inline function a collapse node is written as (made on first use); null when its graph is missing. */
			const FDcrModelFunction* GetInline(const FDcrModelNode& Collapse);
			/** The whole file. */
			TUniquePtr<FDabDocument> LowerDocument();

		private:
			TMap<const FDcrModelNode*, TSharedPtr<FDcrModelFunction>> Inlines;
			TArray<const FDcrModelNode*> InlineOrder;

			TUniquePtr<FDabNamedBlockStmt> LowerFunction(const FDcrModelFunction& Function, const FDcrModelGraph& Graph, bool bInline);
			void LowerGraphEvents(const FDcrModelGraph& Graph, bool bDefaultGraph, TArray<FDabStmtPtr>& Out);
			FDabExprPtr LowerSettings();
			FDabExprPtr LowerModule();
			FDabExprPtr LowerHierarchy();
			FDabExprPtr LowerVariables();
			FDabExprPtr ElementValue(const FDcrModelElement& Element, const FDcrValue& Property);
			static TArray<const FDcrModelNode*> CommentsOf(const FDcrModelGraph& Graph);
			/** The statements execution reaches from a node, along every execute output. */
			static TSet<FString> ReachableStatements(const FGraphIndex& Index, const FDcrModelNode& Start);
			/** Heads of the chains Reached leaves out, and data nodes nothing reads. */
			static void FindDetached(const FGraphIndex& Index, const TSet<FString>& Reached, TArray<const FDcrModelNode*>& OutHeads,
				TArray<const FDcrModelNode*>& OutDangling);
			/** The statements a node reads from, through the data nodes between. */
			static void CollectStatementDeps(const FGraphIndex& Index, const FDcrModelNode& Node, TSet<FString>& OutStatements, TSet<FString>& Seen);
			/** Chains that read what another chain makes after it (its lets come first); otherwise the graph's order. */
			static void OrderDetachedHeads(const FGraphIndex& Index, TArray<const FDcrModelNode*>& Heads);
			void AddLeadingComments(const FDabStmt& Stmt, TConstArrayView<const FDcrModelNode*> Comments);

			TMap<FString, const UScriptStruct*> Structs;
			TMap<const UScriptStruct*, TSharedPtr<FStructOnScope>> UnitDefaults;
			/** Signatures that depend only on what is called, and those that depend on the node itself; held so references stay valid. */
			TMap<FString, TUniquePtr<FSignature>> Signatures;
			TMap<const FDcrModelNode*, TUniquePtr<FSignature>> NodeSignatures;
		};

		const FSignature& FFileLowering::GetSignature(const FDcrModelNode& Node, const FDcrModelFunction* Enclosing)
		{
			const FString Key = FString::Printf(TEXT("%s|%s|%s|%s|%s"), LexToString(Node.Kind), *Node.StructPath, *Node.Notation, *Node.Function,
				*Node.FunctionHost);
			if (Node.Kind == EDcrNodeKind::Dispatch || Node.Kind == EDcrNodeKind::VariableGet || Node.Kind == EDcrNodeKind::VariableSet
				|| Node.Kind == EDcrNodeKind::Entry || Node.Kind == EDcrNodeKind::Return)
			{
				// These depend on the node itself (resolved types, the variable, the enclosing function): never cached.
				if (const TUniquePtr<FSignature>* Found = NodeSignatures.Find(&Node))
				{
					return **Found;
				}
				FSignature& Scratch = *NodeSignatures.Add(&Node, MakeUnique<FSignature>());
				if (Node.Kind == EDcrNodeKind::Dispatch)
				{
					if (const UScriptStruct* Struct = GetNodeStruct(Node))
					{
						Scratch.Pins = GetDcrNodePins(*Struct);
						Scratch.bKnown = true;
						const FRigVMDispatchFactory* Factory = FRigVMRegistry::Get().FindDispatchFactory(FRigVMDispatchFactory::GetFactoryName(Struct));
						for (FDcrPin& Pin : Scratch.Pins)
						{
							for (const TPair<FString, FDcrType>& Resolved : Node.ResolvedTypes)
							{
								if (Resolved.Key == Pin.Name)
								{
									Pin.Type = Resolved.Value;
								}
							}
							if (Factory && Pin.bInput && Pin.Type.IsSet())
							{
								UObject* TypeObject = Pin.Type.ObjectPath.IsEmpty() ? nullptr : RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Pin.Type.ObjectPath);
								const TRigVMTypeIndex TypeIndex = FRigVMRegistry::Get().GetTypeIndex(FName(*Pin.Type.CPPType), TypeObject);
								if (TypeIndex != INDEX_NONE)
								{
									Scratch.Defaults.Add(Pin.Name, Factory->GetArgumentDefaultValue(FName(*Pin.Name), TypeIndex));
								}
							}
						}
					}
				}
				else if (Node.Kind == EDcrNodeKind::VariableGet || Node.Kind == EDcrNodeKind::VariableSet)
				{
					FDcrPin& Pin = Scratch.Pins.AddDefaulted_GetRef();
					Pin.Name = TEXT("Value");
					Pin.bInput = Node.Kind == EDcrNodeKind::VariableSet;
					Pin.bOutput = Node.Kind == EDcrNodeKind::VariableGet;
					Pin.Type = Node.VariableType;
					Scratch.bKnown = true;
				}
				else if (Enclosing)
				{
					const TArray<FDcrModelFunctionPin>& Source = Node.Kind == EDcrNodeKind::Entry ? Enclosing->Inputs : Enclosing->Outputs;
					for (const FDcrModelFunctionPin& FunctionPin : Source)
					{
						FDcrPin& Pin = Scratch.Pins.AddDefaulted_GetRef();
						Pin.Name = FunctionPin.Name;
						Pin.bOutput = Node.Kind == EDcrNodeKind::Entry;
						Pin.bInput = Node.Kind == EDcrNodeKind::Return;
						Pin.Type = FunctionPin.Type;
						if (!FunctionPin.Default.IsEmpty())
						{
							Scratch.Defaults.Add(FunctionPin.Name, FunctionPin.Default);
						}
					}
					Scratch.bKnown = true;
				}
				return Scratch;
			}

			if (Node.Kind == EDcrNodeKind::Collapse)
			{
				if (const TUniquePtr<FSignature>* Found = NodeSignatures.Find(&Node))
				{
					return **Found;
				}
				FSignature& Scratch = *NodeSignatures.Add(&Node, MakeUnique<FSignature>());
				if (const FDcrModelFunction* Inline = GetInline(Node))
				{
					for (const FDcrModelFunctionPin& FunctionPin : Inline->Inputs)
					{
						FDcrPin& Pin = Scratch.Pins.AddDefaulted_GetRef();
						Pin.Name = FunctionPin.Name;
						Pin.bInput = true;
						Pin.Type = FunctionPin.Type;
						if (!FunctionPin.Default.IsEmpty())
						{
							Scratch.Defaults.Add(FunctionPin.Name, FunctionPin.Default);
						}
					}
					for (const FDcrModelFunctionPin& FunctionPin : Inline->Outputs)
					{
						FDcrPin& Pin = Scratch.Pins.AddDefaulted_GetRef();
						Pin.Name = FunctionPin.Name;
						Pin.bOutput = true;
						Pin.Type = FunctionPin.Type;
					}
					Scratch.bKnown = true;
				}
				return Scratch;
			}

			if (const TUniquePtr<FSignature>* Cached = Signatures.Find(Key))
			{
				return **Cached;
			}
			FSignature& Signature = *Signatures.Add(Key, MakeUnique<FSignature>());
			if (Node.Kind == EDcrNodeKind::Unit)
			{
				if (const UScriptStruct* Struct = GetNodeStruct(Node))
				{
					Signature.Pins = GetDcrNodePins(*Struct);
					Signature.bKnown = true;
					for (const FDcrPin& Pin : Signature.Pins)
					{
						if (Pin.bInput && Pin.Property)
						{
							Signature.Defaults.Add(Pin.Name, GetUnitDefault(*Struct, *Pin.Property));
						}
					}
				}
			}
			else if (Node.Kind == EDcrNodeKind::LocalCall)
			{
				if (const FDcrModelFunction* Called = FindFunction(Node.Function))
				{
					for (const FDcrModelFunctionPin& FunctionPin : Called->Inputs)
					{
						FDcrPin& Pin = Signature.Pins.AddDefaulted_GetRef();
						Pin.Name = FunctionPin.Name;
						Pin.bInput = true;
						Pin.Type = FunctionPin.Type;
						if (!FunctionPin.Default.IsEmpty())
						{
							Signature.Defaults.Add(FunctionPin.Name, FunctionPin.Default);
						}
					}
					for (const FDcrModelFunctionPin& FunctionPin : Called->Outputs)
					{
						FDcrPin& Pin = Signature.Pins.AddDefaulted_GetRef();
						Pin.Name = FunctionPin.Name;
						Pin.bOutput = true;
						Pin.Type = FunctionPin.Type;
					}
					Signature.bKnown = true;
				}
			}
			else if (Node.Kind == EDcrNodeKind::ExternalCall)
			{
				FString Error;
				const FRigVMGraphFunctionData* Data = FRigVMGraphFunctionData::FindFunctionData(FSoftObjectPath(Node.FunctionHost), FName(*Node.Function), nullptr, &Error);
				if (Data)
				{
					for (const FRigVMGraphFunctionArgument& Argument : Data->Header.Arguments)
					{
						const UObject* TypeObject = Argument.CPPTypeObject.LoadSynchronous();
						if (const UScriptStruct* TypeStruct = Cast<UScriptStruct>(TypeObject); TypeStruct && TypeStruct->IsChildOf(FRigVMExecutePin::StaticStruct()))
						{
							continue;
						}
						if (Argument.Direction == ERigVMPinDirection::Hidden)
						{
							continue;
						}
						FDcrPin& Pin = Signature.Pins.AddDefaulted_GetRef();
						Pin.Name = Argument.Name.ToString();
						Pin.bInput = Argument.Direction == ERigVMPinDirection::Input || Argument.Direction == ERigVMPinDirection::IO
							|| Argument.Direction == ERigVMPinDirection::Visible;
						Pin.bOutput = Argument.Direction == ERigVMPinDirection::Output || Argument.Direction == ERigVMPinDirection::IO;
						Pin.Type.CPPType = Argument.CPPType.ToString();
						Pin.Type.ObjectPath = TypeObject ? TypeObject->GetPathName() : FString();
						if (Pin.bInput && !Argument.DefaultValue.IsEmpty())
						{
							Signature.Defaults.Add(Pin.Name, Argument.DefaultValue);
						}
					}
					Signature.bKnown = true;
				}
			}
			return Signature;
		}

		FString FFileLowering::GetLibraryName(const FString& HostPath) const
		{
			FString Path = HostPath;
			Path.RemoveFromEnd(TEXT("_C"));
			return FSoftObjectPath(Path).GetAssetName();
		}

		// ------------------------------------------------------------------------------------------
		// Skeleton: the statements in order
		// ------------------------------------------------------------------------------------------

		void FBodyLowering::BuildSkeleton(FSkelBlock& Block, const FDcrModelNode* Node)
		{
			while (Node)
			{
				if (Visited.Contains(Node->Name))
				{
					File.AddUnsupported(Location + TEXT("/") + Node->Name, TEXT("two execute paths lead into this node; it is written once, where the first one reaches it"));
					return;
				}
				Visited.Add(Node->Name);
				FSkel& Skel = Block.Statements.AddDefaulted_GetRef();
				Skel.Node = Node;
				Skel.Ordinal = NextOrdinal++;
				SkelOf.Add(Node->Name, nullptr);
				BlockOf.Add(Node->Name, &Block);

				const UScriptStruct* Struct = File.GetNodeStruct(*Node);
				auto AddBlock = [this, &Block](FSkel& Owner, const FDcrModelNode* First)
				{
					TUniquePtr<FSkelBlock> Inner = MakeUnique<FSkelBlock>();
					Inner->Parent = &Block;
					Inner->IndexInParent = Block.Statements.Num() - 1;
					Inner->Depth = Block.Depth + 1;
					BuildSkeleton(*Inner, First);
					Owner.Blocks.Add(MoveTemp(Inner));
				};

				if (Node->Kind == EDcrNodeKind::Return)
				{
					Skel.Kind = FSkel::EKind::Return;
					return;
				}
				if (IsBranch(*Node))
				{
					Skel.Kind = FSkel::EKind::If;
					// Blocks are built after the statement is in place: the array may move while they are.
					const FDcrModelNode* TrueNode = Graph.Next(*Node, TEXT("True"));
					const FDcrModelNode* FalseNode = Graph.Next(*Node, TEXT("False"));
					const int32 Index = Block.Statements.Num() - 1;
					AddBlock(Block.Statements[Index], TrueNode);
					AddBlock(Block.Statements[Index], FalseNode);
					Node = Graph.Next(*Node, TEXT("Completed"));
					continue;
				}
				if (IsForLoopCount(*Node) || IsArrayIterator(*Node, Struct))
				{
					Skel.Kind = FSkel::EKind::For;
					// The body hangs off the execute pin the loop both takes and gives: `ExecutePin` on RigVM's own
					// units, `ExecuteContext` on Control Rig's and on dispatches.
					const FString* BodyPin = Node->ExecuteOut.FindByPredicate([Node](const FString& Pin) { return Pin != TEXT("Completed") && Node->ExecuteIn.Contains(Pin); });
					const FDcrModelNode* BodyNode = BodyPin ? Graph.Next(*Node, *BodyPin) : nullptr;
					const int32 Index = Block.Statements.Num() - 1;
					AddBlock(Block.Statements[Index], BodyNode);
					Node = Graph.Next(*Node, TEXT("Completed"));
					continue;
				}

				const FString Continue = GetContinuationPin(*Node);
				for (const FString& Pin : Node->ExecuteOut)
				{
					if (Pin != Continue && !Node->ExecuteIn.Contains(Pin) && Graph.Next(*Node, Pin))
					{
						File.AddUnsupported(Location + TEXT("/") + Node->Name + TEXT(".") + Pin,
							TEXT("an execute output other than the one execution continues by; what it runs is left out"));
					}
				}
				Node = Continue.IsEmpty() ? nullptr : Graph.Next(*Node, Continue);
			}
		}

		// ------------------------------------------------------------------------------------------
		// Analysis: which data nodes are lets, which statements are written inline, where lets go
		// ------------------------------------------------------------------------------------------

		bool FBodyLowering::IsLetData(const FDcrModelNode& Node) const
		{
			if (Node.Kind == EDcrNodeKind::VariableGet || Node.Kind == EDcrNodeKind::Entry || GetRole(Node) != ERole::Data)
			{
				return false;
			}
			return UseCounts.FindRef(Node.Name) > 1;
		}

		void FBodyLowering::CountNodeInputs(const FDcrModelNode& Node, TSet<FString>& Seen)
		{
			if (Seen.Contains(Node.Name))
			{
				return;
			}
			Seen.Add(Node.Name);
			const FString Prefix = Node.Name + TEXT(".");
			for (const TPair<FString, FString>& Link : Graph.InputsOf(Node.Name))
			{
				if (!Link.Key.StartsWith(Prefix))
				{
					continue;
				}
				const FPinPath Source = SplitPinPath(Link.Value);
				const FDcrModelNode* SourceNode = Graph.Find(Source.Node);
				if (!SourceNode)
				{
					continue;
				}
				UseCounts.FindOrAdd(SourceNode->Name)++;
				const ERole Role = GetRole(*SourceNode);
				if (Role == ERole::Data || Role == ERole::Injected)
				{
					CountNodeInputs(*SourceNode, Seen);
				}
			}
			// An injected node sits on one of this node's pins: its inputs are used here too.
			for (const TPair<FString, const FDcrModelNode*>& Injected : Graph.InjectedInputs)
			{
				if (Injected.Key.StartsWith(Prefix))
				{
					CountNodeInputs(*Injected.Value, Seen);
				}
			}
		}

		void FBodyLowering::CountUses()
		{
			TSet<FString> Seen;
			TFunction<void(const FSkelBlock&)> Walk = [&](const FSkelBlock& Block)
			{
				for (const FSkel& Skel : Block.Statements)
				{
					if (Skel.Kind != FSkel::EKind::Detached)
					{
						CountNodeInputs(*Skel.Node, Seen);
					}
					for (const TUniquePtr<FSkelBlock>& Inner : Skel.Blocks)
					{
						Walk(*Inner);
					}
				}
			};
			Walk(*Root);
		}

		void FBodyLowering::CollectStatementSources(const FDcrModelNode& Node, TArray<const FDcrModelNode*>& OutMutables, TSet<FString>& Seen, bool bTop) const
		{
			if (!bTop && Seen.Contains(Node.Name))
			{
				return;
			}
			Seen.Add(Node.Name);
			const FString Prefix = Node.Name + TEXT(".");
			// Pins in the order their arguments are written, so the post-order matches how semantic runs nested calls.
			TArray<TPair<FString, FString>> Inputs;
			for (const TPair<FString, FString>& Link : Graph.InputsOf(Node.Name))
			{
				if (Link.Key.StartsWith(Prefix))
				{
					Inputs.Add(Link);
				}
			}
			const FSignature& Signature = const_cast<FFileLowering&>(File).GetSignature(Node, Function);
			auto PinOrder = [&Signature](const FString& TargetPath)
			{
				const FPinPath Target = SplitPinPath(TargetPath);
				const int32 Index = Signature.Pins.IndexOfByPredicate([&Target](const FDcrPin& Pin) { return Pin.Name == Target.Root; });
				return Index == INDEX_NONE ? MAX_int32 : Index;
			};
			Inputs.StableSort([&PinOrder](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
			{
				const int32 OrderA = PinOrder(A.Key);
				const int32 OrderB = PinOrder(B.Key);
				return OrderA != OrderB ? OrderA < OrderB : A.Key < B.Key;
			});
			for (const TPair<FString, FString>& Link : Inputs)
			{
				const FDcrModelNode* SourceNode = Graph.Find(SplitPinPath(Link.Value).Node);
				if (!SourceNode)
				{
					continue;
				}
				const ERole Role = GetRole(*SourceNode);
				if (Role == ERole::Statement)
				{
					if (UseCounts.FindRef(SourceNode->Name) == 1)
					{
						CollectStatementSources(*SourceNode, OutMutables, Seen, false);
						OutMutables.Add(SourceNode);
					}
				}
				else if (Role == ERole::Data && !IsLetData(*SourceNode))
				{
					CollectStatementSources(*SourceNode, OutMutables, Seen, false);
				}
			}
		}

		void FBodyLowering::ChooseInlinedStatements(FSkelBlock& Block)
		{
			for (int32 Index = 0; Index < Block.Statements.Num(); ++Index)
			{
				FSkel& Skel = Block.Statements[Index];
				for (TUniquePtr<FSkelBlock>& Inner : Skel.Blocks)
				{
					ChooseInlinedStatements(*Inner);
				}
				if (Skel.Kind == FSkel::EKind::Detached)
				{
					continue;
				}
				// Statements whose single use is in this one, in the order semantic would run them: they must be
				// exactly the plain statements right before it.
				TArray<const FDcrModelNode*> Mutables;
				TSet<FString> Seen;
				CollectStatementSources(*Skel.Node, Mutables, Seen, true);
				if (Mutables.IsEmpty() || Mutables.Num() > Index)
				{
					continue;
				}
				bool bMatches = true;
				for (int32 Offset = 0; Offset < Mutables.Num() && bMatches; ++Offset)
				{
					const FSkel& Before = Block.Statements[Index - Mutables.Num() + Offset];
					bMatches = Before.Node == Mutables[Offset] && Before.Kind == FSkel::EKind::Call && !InlinedStatements.Contains(Before.Node->Name);
				}
				if (bMatches)
				{
					for (const FDcrModelNode* Mutable : Mutables)
					{
						InlinedStatements.Add(Mutable->Name);
					}
				}
			}
		}

		void FBodyLowering::CollectLetRefs(const FDcrModelNode& Node, TArray<FString>& OutLets, TSet<FString>& Seen, bool bTop) const
		{
			if (!bTop && Seen.Contains(Node.Name))
			{
				return;
			}
			Seen.Add(Node.Name);
			const FString Prefix = Node.Name + TEXT(".");
			auto Visit = [&](const FDcrModelNode& SourceNode)
			{
				const ERole Role = GetRole(SourceNode);
				if (Role == ERole::Data && IsLetData(SourceNode))
				{
					OutLets.AddUnique(SourceNode.Name);
				}
				else if (Role == ERole::Data || Role == ERole::Injected || (Role == ERole::Statement && InlinedStatements.Contains(SourceNode.Name)))
				{
					CollectLetRefs(SourceNode, OutLets, Seen, false);
				}
			};
			for (const TPair<FString, FString>& Link : Graph.InputsOf(Node.Name))
			{
				if (Link.Key.StartsWith(Prefix))
				{
					if (const FDcrModelNode* SourceNode = Graph.Find(SplitPinPath(Link.Value).Node))
					{
						Visit(*SourceNode);
					}
				}
			}
			for (const TPair<FString, const FDcrModelNode*>& Injected : Graph.InjectedInputs)
			{
				if (Injected.Key.StartsWith(Prefix))
				{
					Visit(*Injected.Value);
				}
			}
		}

		FBodyLowering::FPlace FBodyLowering::PlaceOf(const FSkel& Skel) const
		{
			const FSkelBlock* Block = BlockOf.FindRef(Skel.Node->Name);
			FPlace Place;
			Place.Block = const_cast<FSkelBlock*>(Block);
			Place.Index = Block ? Block->Statements.IndexOfByPredicate([&Skel](const FSkel& Other) { return Other.Node == Skel.Node; }) : 0;
			return Place;
		}

		FBodyLowering::FPlace FBodyLowering::Common(const FPlace& InA, const FPlace& InB) const
		{
			FPlace A = InA;
			FPlace B = InB;
			if (!A.Block)
			{
				return B;
			}
			if (!B.Block)
			{
				return A;
			}
			while (A.Block->Depth > B.Block->Depth)
			{
				A.Index = A.Block->IndexInParent;
				A.Block = A.Block->Parent;
			}
			while (B.Block->Depth > A.Block->Depth)
			{
				B.Index = B.Block->IndexInParent;
				B.Block = B.Block->Parent;
			}
			while (A.Block != B.Block)
			{
				A.Index = A.Block->IndexInParent;
				A.Block = A.Block->Parent;
				B.Index = B.Block->IndexInParent;
				B.Block = B.Block->Parent;
			}
			A.Index = FMath::Min(A.Index, B.Index);
			return A;
		}

		FString FBodyLowering::LetNameFor(const FDcrModelNode& Node) const
		{
			const FDcrModelFunction* Inline = Node.Kind == EDcrNodeKind::Collapse ? File.GetInline(Node) : nullptr;
			return MakeLetName(Inline ? Inline->Name : Node.Name);
		}

		bool FBodyLowering::PlaceDangling(const FDcrModelNode& Node, FPlace& OutPlace) const
		{
			// Where everything the node reads is in scope: after the statements it reads, inside the loops whose items it reads.
			bool bAny = false;
			FPlace Latest;
			TSet<FString> Seen;
			TArray<const FDcrModelNode*> Stack;
			Stack.Add(&Node);
			while (!Stack.IsEmpty())
			{
				const FDcrModelNode* Current = Stack.Pop();
				if (Seen.Contains(Current->Name))
				{
					continue;
				}
				Seen.Add(Current->Name);
				for (const TPair<FString, FString>& Link : Graph.InputsOf(Current->Name))
				{
					const FDcrModelNode* Source = Graph.Find(SplitPinPath(Link.Value).Node);
					if (!Source)
					{
						continue;
					}
					FSkelBlock* const* Found = BlockOf.Find(Source->Name);
					if (!Found || !*Found)
					{
						if (GetRole(*Source) != ERole::Statement)
						{
							Stack.Add(Source);
						}
						continue;
					}
					FSkelBlock* Block = *Found;
					const int32 Index = Block->Statements.IndexOfByPredicate([Source](const FSkel& Skel) { return Skel.Node == Source; });
					if (Index == INDEX_NONE)
					{
						continue;
					}
					const FSkel& Skel = Block->Statements[Index];
					FPlace Place;
					if (Skel.Kind == FSkel::EKind::For && !Skel.Blocks.IsEmpty())
					{
						Place.Block = Skel.Blocks[0].Get();
						Place.Index = Place.Block->Statements.Num();
					}
					else
					{
						Place.Block = Block;
						Place.Index = Index + 1;
					}
					if (!bAny)
					{
						Latest = Place;
						bAny = true;
						continue;
					}
					// The later of two places, when one is in scope at the other.
					auto Encloses = [](const FSkelBlock* Outer, const FSkelBlock* Inner)
					{
						for (const FSkelBlock* Block = Inner; Block; Block = Block->Parent)
						{
							if (Block == Outer)
							{
								return true;
							}
						}
						return false;
					};
					if (Place.Block == Latest.Block)
					{
						Latest.Index = FMath::Max(Latest.Index, Place.Index);
					}
					else if (Encloses(Latest.Block, Place.Block))
					{
						Latest = Place;
					}
					else if (!Encloses(Place.Block, Latest.Block))
					{
						return false;
					}
				}
			}
			OutPlace = Latest;
			return bAny;
		}

		void FBodyLowering::PlaceLets(TConstArrayView<const FDcrModelNode*> Dangling, FSkelBlock* Fallback)
		{
			// Uses of each pure let: statements, then other lets.
			TMap<FString, TArray<FString>> LetUsers;
			TMap<FString, FPlace> Places;
			TFunction<void(FSkelBlock&)> Walk = [&](FSkelBlock& Block)
			{
				for (FSkel& Skel : Block.Statements)
				{
					if (Skel.Kind == FSkel::EKind::Detached)
					{
						Walk(*Skel.Blocks[0]);
						continue;
					}
					if (InlinedStatements.Contains(Skel.Node->Name))
					{
						continue;
					}
					TArray<FString> Lets;
					TSet<FString> Seen;
					CollectLetRefs(*Skel.Node, Lets, Seen, true);
					const FPlace Here = PlaceOf(Skel);
					for (const FString& Let : Lets)
					{
						FPlace* Existing = Places.Find(Let);
						Places.Add(Let, Existing ? Common(*Existing, Here) : Here);
					}
					for (TUniquePtr<FSkelBlock>& Inner : Skel.Blocks)
					{
						Walk(*Inner);
					}
				}
			};
			Walk(*Root);

			// Data nodes nothing reads: where what they read is in scope; the rest at the end of the fallback block.
			// (Before the lets they read are placed, which go no later than they.)
			for (const FDcrModelNode* Node : Dangling)
			{
				FPlace Place;
				if (Places.Contains(Node->Name))
				{
					continue;
				}
				if (PlaceDangling(*Node, Place))
				{
					Places.Add(Node->Name, Place);
				}
				else if (Fallback)
				{
					Places.Add(Node->Name, FPlace{ Fallback, Fallback->Statements.Num() });
				}
			}

			// Lets used by lets: a let goes no later than every let that reads it. Repeat until nothing moves.
			TMap<FString, TArray<FString>> Reads;
			for (const TPair<FString, FDcrModelNode const*>& Pair : Graph.Nodes)
			{
				if (Places.Contains(Pair.Key) || IsLetData(*Pair.Value))
				{
					TArray<FString> Lets;
					TSet<FString> Seen;
					CollectLetRefs(*Pair.Value, Lets, Seen, true);
					Reads.Add(Pair.Key, MoveTemp(Lets));
				}
			}
			bool bChanged = true;
			for (int32 Pass = 0; bChanged && Pass < 64; ++Pass)
			{
				bChanged = false;
				for (const TPair<FString, TArray<FString>>& Reader : Reads)
				{
					const FPlace* ReaderFound = Places.Find(Reader.Key);
					if (!ReaderFound)
					{
						continue;
					}
					// A copy: adding to Places below may move what Find pointed at.
					const FPlace ReaderPlace = *ReaderFound;
					for (const FString& Read : Reader.Value)
					{
						FPlace* Existing = Places.Find(Read);
						const FPlace Merged = Existing ? Common(*Existing, ReaderPlace) : ReaderPlace;
						if (!Existing || Merged.Block != Existing->Block || Merged.Index != Existing->Index)
						{
							Places.Add(Read, Merged);
							bChanged = true;
						}
					}
				}
			}
			LetPlaces = MoveTemp(Places);
		}

		FString FBodyLowering::UniqueName(const FString& Wanted)
		{
			FString Name = Wanted;
			for (int32 Suffix = 1; UsedNames.Contains(Name) || File.GlobalNames.Contains(Name) || IsReservedName(Name); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s_%d"), *Wanted, Suffix);
			}
			UsedNames.Add(Name);
			return Name;
		}

		// ------------------------------------------------------------------------------------------
		// Writing statements
		// ------------------------------------------------------------------------------------------

		TUniquePtr<FDabCodeBlock> FBodyLowering::LowerChain(const FDcrModelNode& Start, const FString& Pin)
		{
			return LowerChain(Start, Pin, {}, {});
		}

		TUniquePtr<FDabCodeBlock> FBodyLowering::LowerChain(const FDcrModelNode& Start, const FString& Pin, TConstArrayView<const FDcrModelNode*> DetachedHeads,
			TConstArrayView<const FDcrModelNode*> Dangling)
		{
			Root = MakeUnique<FSkelBlock>();
			bCode = true;
			if (Function)
			{
				for (const FDcrModelFunctionPin& Input : Function->Inputs)
				{
					UsedNames.Add(Input.Name);
				}
				for (const FDcrModelVariable& Local : Function->Locals)
				{
					UsedNames.Add(Local.Name);
				}
			}
			Visited.Add(Start.Name);
			BuildSkeleton(*Root, Graph.Next(Start, Pin));

			// A function's return node that execution never reaches still gives the outputs: write it last.
			if (Function)
			{
				for (const FDcrModelNode& Node : Graph.Graph->Nodes)
				{
					if (Node.Kind == EDcrNodeKind::Return && !Visited.Contains(Node.Name))
					{
						FSkel& Skel = Root->Statements.AddDefaulted_GetRef();
						Skel.Node = &Node;
						Skel.Kind = FSkel::EKind::Return;
						Skel.Ordinal = NextOrdinal++;
						BlockOf.Add(Node.Name, Root.Get());
						Visited.Add(Node.Name);
					}
				}
			}

			// What nothing runs goes in Detached blocks, one per chain (code in one block runs in sequence): at the end
			// of the innermost block whose statements it reads, else at the end of the body, before a closing return
			// (which must stay last). Data nodes nothing reads go in the first at the end of the body.
			FSkelBlock* DetachedBlock = nullptr;
			const bool bBeforeReturn = !Root->Statements.IsEmpty() && Root->Statements.Last().Kind == FSkel::EKind::Return;
			int32 At = Root->Statements.Num() - (bBeforeReturn ? 1 : 0);
			for (const FDcrModelNode* Head : DetachedHeads)
			{
				FSkelBlock* Target = FindDetachedPlace(*Head);
				if (!Target || Target == Root.Get())
				{
					FSkelBlock& Inner = AddDetachedSkel(*Root, At++, Start);
					DetachedBlock = DetachedBlock ? DetachedBlock : &Inner;
					BuildSkeleton(Inner, Head);
				}
				else
				{
					BuildSkeleton(AddDetachedSkel(*Target, Target->Statements.Num(), Start), Head);
				}
			}
			if (!DetachedBlock && !Dangling.IsEmpty())
			{
				DetachedBlock = &AddDetachedSkel(*Root, At, Start);
			}

			CountUses();
			// Data nodes nothing reads: each is a let of its own.
			for (const FDcrModelNode* Node : Dangling)
			{
				TSet<FString> Seen;
				CountNodeInputs(*Node, Seen);
				UseCounts.FindOrAdd(Node->Name) = 2;
			}
			ChooseInlinedStatements(*Root);
			PlaceLets(Dangling, DetachedBlock);

			TUniquePtr<FDabCodeBlock> Code = MakeUnique<FDabCodeBlock>();
			WriteBlock(*Root, Code->Statements);
			WriteLetsAt(*Root, Root->Statements.Num(), Code->Statements);
			return Code;
		}

		FBodyLowering::FSkelBlock* FBodyLowering::FindDetachedPlace(const FDcrModelNode& Head) const
		{
			TSet<FString> Reads;
			TSet<FString> Seen;
			const TSet<FString> Chain = FFileLowering::ReachableStatements(Graph, Head);
			for (const FString& Name : Chain)
			{
				if (const FDcrModelNode* Node = Graph.Find(Name))
				{
					FFileLowering::CollectStatementDeps(Graph, *Node, Reads, Seen);
				}
			}
			auto Encloses = [](const FSkelBlock* Outer, const FSkelBlock* Inner)
			{
				for (const FSkelBlock* Block = Inner; Block; Block = Block->Parent)
				{
					if (Block == Outer)
					{
						return true;
					}
				}
				return false;
			};
			FSkelBlock* Deepest = nullptr;
			for (const FString& Read : Reads)
			{
				FSkelBlock* const* Found = Chain.Contains(Read) ? nullptr : BlockOf.Find(Read);
				if (!Found || !*Found)
				{
					continue;
				}
				// A loop's values are in scope in its body only.
				FSkelBlock* Block = *Found;
				const int32 Index = Block->Statements.IndexOfByPredicate([&Read](const FSkel& Skel) { return Skel.Node && Skel.Node->Name == Read; });
				if (Index != INDEX_NONE && Block->Statements[Index].Kind == FSkel::EKind::For && !Block->Statements[Index].Blocks.IsEmpty())
				{
					Block = Block->Statements[Index].Blocks[0].Get();
				}
				if (!Deepest || Encloses(Deepest, Block))
				{
					Deepest = Block;
				}
				else if (!Encloses(Block, Deepest))
				{
					return nullptr;
				}
			}
			return Deepest;
		}

		FBodyLowering::FSkelBlock& FBodyLowering::AddDetachedSkel(FSkelBlock& Block, int32 At, const FDcrModelNode& Node)
		{
			FSkel Detached;
			Detached.Node = &Node;
			Detached.Kind = FSkel::EKind::Detached;
			Detached.Ordinal = NextOrdinal++;
			Block.Statements.Insert(MoveTemp(Detached), At);
			// The blocks after it moved on by one.
			for (int32 Index = At + 1; Index < Block.Statements.Num(); ++Index)
			{
				for (TUniquePtr<FSkelBlock>& Inner : Block.Statements[Index].Blocks)
				{
					Inner->IndexInParent = Index;
				}
			}
			TUniquePtr<FSkelBlock> Inner = MakeUnique<FSkelBlock>();
			Inner->Parent = &Block;
			Inner->IndexInParent = At;
			Inner->Depth = Block.Depth + 1;
			FSkelBlock& Result = *Inner;
			Block.Statements[At].Blocks.Add(MoveTemp(Inner));
			return Result;
		}

		TUniquePtr<FDabCodeBlock> FBodyLowering::LowerDetached(TConstArrayView<const FDcrModelNode*> Heads, TConstArrayView<const FDcrModelNode*> Dangling)
		{
			Root = MakeUnique<FSkelBlock>();
			bCode = true;
			// The first chain is the block's own code; each other one is a Detached block of its own, so it does not run after it.
			for (int32 Index = 0; Index < Heads.Num(); ++Index)
			{
				if (Index == 0)
				{
					BuildSkeleton(*Root, Heads[Index]);
				}
				else
				{
					BuildSkeleton(AddDetachedSkel(*Root, Root->Statements.Num(), *Heads[Index]), Heads[Index]);
				}
			}
			CountUses();
			// Data nodes nothing uses: each is a let of its own.
			for (const FDcrModelNode* Node : Dangling)
			{
				TSet<FString> Seen;
				CountNodeInputs(*Node, Seen);
				UseCounts.FindOrAdd(Node->Name) = 2;
			}
			ChooseInlinedStatements(*Root);
			PlaceLets(Dangling, Root.Get());

			TUniquePtr<FDabCodeBlock> Code = MakeUnique<FDabCodeBlock>();
			WriteBlock(*Root, Code->Statements);
			WriteLetsAt(*Root, Root->Statements.Num(), Code->Statements);
			return Code;
		}

		void FBodyLowering::WriteLetsAt(const FSkelBlock& Block, int32 Index, TArray<FDabStmtPtr>& Out)
		{
			TArray<FString> Here;
			for (const TPair<FString, FPlace>& Place : LetPlaces)
			{
				if (Place.Value.Block == &Block && Place.Value.Index == Index && !WrittenLets.Contains(Place.Key))
				{
					Here.Add(Place.Key);
				}
			}
			// Stable order: by the node's position in the graph's node list.
			Here.Sort([this](const FString& A, const FString& B)
			{
				const int32 IndexA = Graph.Graph->Nodes.IndexOfByPredicate([&A](const FDcrModelNode& Node) { return Node.Name == A; });
				const int32 IndexB = Graph.Graph->Nodes.IndexOfByPredicate([&B](const FDcrModelNode& Node) { return Node.Name == B; });
				return IndexA < IndexB;
			});
			for (const FString& Let : Here)
			{
				WriteLet(Let, Out);
			}
		}

		void FBodyLowering::WriteLet(const FString& NodeName, TArray<FDabStmtPtr>& Out)
		{
			if (WrittenLets.Contains(NodeName))
			{
				return;
			}
			WrittenLets.Add(NodeName);
			const FDcrModelNode* Node = Graph.Find(NodeName);
			if (!Node)
			{
				return;
			}
			// What this let reads is written before it.
			TArray<FString> Reads;
			TSet<FString> Seen;
			CollectLetRefs(*Node, Reads, Seen, true);
			for (const FString& Read : Reads)
			{
				if (!WrittenLets.Contains(Read) && LetPlaces.Contains(Read))
				{
					WriteLet(Read, Out);
				}
			}
			const FString Name = UniqueName(LetNameFor(*Node));
			TArray<FString> Nodes;
			TArray<FString>* Previous = Collecting;
			Collecting = &Nodes;
			// Typed lets of the statement being written stay with it; this let's own go before it.
			TArray<FDabStmtPtr> Outer = MoveTemp(Hoisted);
			Hoisted.Reset();
			FDabExprPtr Value = NodeExpr(*Node);
			Collecting = Previous;
			LetNames.Add(NodeName, Name);
			TUniquePtr<FDabLetStmt> Let = MakeUnique<FDabLetStmt>();
			Let->Name = Name;
			Let->Value = MoveTemp(Value);
			if (const FDcrPin* Untyped = FindUntypedOutput(*Node))
			{
				Let->LetKind = EDabLetKind::Typed;
				Let->TypeName = GetDcrTypeName(Untyped->Type);
			}
			StmtNodes.Add(Let.Get(), MoveTemp(Nodes));
			Out.Append(MoveTemp(Hoisted));
			Hoisted = MoveTemp(Outer);
			Out.Add(MoveTemp(Let));
		}

		void FBodyLowering::WriteBlock(const FSkelBlock& Block, TArray<FDabStmtPtr>& Out)
		{
			// Typed lets of the statement that holds this block (an `if` condition) go before that statement.
			TArray<FDabStmtPtr> Outer = MoveTemp(Hoisted);
			Hoisted.Reset();
			for (int32 Index = 0; Index < Block.Statements.Num(); ++Index)
			{
				const FSkel& Skel = Block.Statements[Index];
				WriteLetsAt(Block, Index, Out);
				if (Skel.Kind == FSkel::EKind::Detached)
				{
					TUniquePtr<FDabDetachedStmt> Detached = MakeUnique<FDabDetachedStmt>();
					Detached->Body = MakeUnique<FDabCodeBlock>();
					WriteBlock(*Skel.Blocks[0], Detached->Body->Statements);
					WriteLetsAt(*Skel.Blocks[0], Skel.Blocks[0]->Statements.Num(), Detached->Body->Statements);
					// Everything meant for it may have found a place in scope instead.
					if (!Detached->Body->Statements.IsEmpty())
					{
						Out.Add(MoveTemp(Detached));
					}
					continue;
				}
				if (InlinedStatements.Contains(Skel.Node->Name))
				{
					continue;
				}
				TArray<FString> Nodes;
				Nodes.Add(Skel.Node->Name);
				TArray<FString>* Previous = Collecting;
				Collecting = &Nodes;
				FDabStmtPtr Stmt = WriteStatement(Skel);
				Collecting = Previous;
				Out.Append(MoveTemp(Hoisted));
				Hoisted.Reset();
				if (Stmt)
				{
					StmtNodes.Add(Stmt.Get(), MoveTemp(Nodes));
					Out.Add(MoveTemp(Stmt));
				}
			}
			Hoisted = MoveTemp(Outer);
		}

		FDabStmtPtr FBodyLowering::WriteStatement(const FSkel& Skel)
		{
			const FDcrModelNode& Node = *Skel.Node;
			switch (Skel.Kind)
			{
			case FSkel::EKind::If: return WriteIf(Skel);
			case FSkel::EKind::For: return WriteFor(Skel);
			case FSkel::EKind::Return: return WriteReturn(Node);
			default: break;
			}

			const FSignature& Signature = File.GetSignature(Node, Function);
			if (Node.Kind == EDcrNodeKind::VariableSet)
			{
				if (!HasVariable(Node.Variable))
				{
					return MakeExprStmt(VariableExpr(Node));
				}
				TUniquePtr<FDabSetStmt> Set = MakeUnique<FDabSetStmt>();
				Set->Target = MakeDcrName(Node.Variable);
				const FDcrPin* Pin = Signature.Find(TEXT("Value"));
				Set->Value = Pin ? InputExpr(Node, *Pin, Signature, true) : nullptr;
				if (!Set->Value)
				{
					Set->Value = MakeDcrUnsupported(TEXT("the value of this setter"));
				}
				return Set;
			}

			// `Array[i] = x;` for a setter of an element of a variable.
			const UScriptStruct* Struct = File.GetNodeStruct(Node);
			if (Struct && Struct->GetName() == TEXT("RigVMDispatch_ArraySetAtIndex"))
			{
				const FString* ArraySource = Graph.DataSource.Find(Node.Name + TEXT(".Array"));
				const FDcrModelNode* ArrayNode = ArraySource ? Graph.Find(SplitPinPath(*ArraySource).Node) : nullptr;
				if (ArrayNode && ArrayNode->Kind == EDcrNodeKind::VariableGet && SplitPinPath(*ArraySource).Sub.IsEmpty()
					&& Graph.DataTargets.FindRef(ArrayNode->Name).Num() == 1)
				{
					const FDcrPin* IndexPin = Signature.Find(TEXT("Index"));
					const FDcrPin* ElementPin = Signature.Find(TEXT("Element"));
					if (IndexPin && ElementPin)
					{
						TUniquePtr<FDabSetStmt> Set = MakeUnique<FDabSetStmt>();
						Set->Target = MakeDcrIndex(MakeDcrName(ArrayNode->Variable), InputExpr(Node, *IndexPin, Signature, true));
						Set->Value = InputExpr(Node, *ElementPin, Signature, true);
						Visited.Add(ArrayNode->Name);
						if (Collecting)
						{
							Collecting->AddUnique(ArrayNode->Name);
						}
						return Set;
					}
				}
			}

			const bool bUsed = Graph.DataTargets.Contains(Node.Name);
			FDabExprPtr Call = NodeExpr(Node);
			if (bUsed && !InlinedStatements.Contains(Node.Name))
			{
				const FString Name = UniqueName(LetNameFor(Node));
				LetNames.Add(Node.Name, Name);
				TUniquePtr<FDabLetStmt> Let = MakeUnique<FDabLetStmt>();
				Let->Name = Name;
				Let->Value = MoveTemp(Call);
				return Let;
			}
			return MakeExprStmt(MoveTemp(Call));
		}

		FDabStmtPtr FBodyLowering::WriteIf(const FSkel& Skel)
		{
			const FDcrModelNode& Node = *Skel.Node;
			if (Collecting)
			{
				Collecting->AddUnique(Node.Name);
			}
			const FSignature& Signature = File.GetSignature(Node, Function);
			TUniquePtr<FDabIfStmt> If = MakeUnique<FDabIfStmt>();
			const FDcrPin* Condition = Signature.Find(TEXT("Condition"));
			If->Condition = Condition ? InputExpr(Node, *Condition, Signature, true) : MakeDcrBool(false);
			If->Then = MakeUnique<FDabCodeBlock>();
			WriteBlock(*Skel.Blocks[0], If->Then->Statements);
			WriteLetsAt(*Skel.Blocks[0], Skel.Blocks[0]->Statements.Num(), If->Then->Statements);

			const FSkelBlock& False = *Skel.Blocks[1];
			// `else if`: the false side is one branch and nothing else, and nothing in it needs a let placed before it.
			const bool bElseIf = False.Statements.Num() == 1 && False.Statements[0].Kind == FSkel::EKind::If
				&& !Algo::AnyOf(LetPlaces, [&False](const TPair<FString, FPlace>& Place) { return Place.Value.Block == &False; });
			if (bElseIf)
			{
				FDabStmtPtr Inner = WriteIf(False.Statements[0]);
				If->ElseIf.Reset(static_cast<FDabIfStmt*>(Inner.Release()));
			}
			else if (!False.Statements.IsEmpty() || Algo::AnyOf(LetPlaces, [&False](const TPair<FString, FPlace>& Place) { return Place.Value.Block == &False; }))
			{
				If->Else = MakeUnique<FDabCodeBlock>();
				WriteBlock(False, If->Else->Statements);
				WriteLetsAt(False, False.Statements.Num(), If->Else->Statements);
			}
			return If;
		}

		FDabStmtPtr FBodyLowering::WriteFor(const FSkel& Skel)
		{
			const FDcrModelNode& Node = *Skel.Node;
			const FSignature& Signature = File.GetSignature(Node, Function);
			TUniquePtr<FDabForStmt> For = MakeUnique<FDabForStmt>();
			const bool bCount = IsForLoopCount(Node);
			const TArray<FString> Outputs = bCount ? TArray<FString>{ TEXT("Index"), TEXT("Ratio") }
				: TArray<FString>{ TEXT("Element"), TEXT("Index"), TEXT("Count"), TEXT("Ratio") };
			// Names up to the last output the body uses; the first always.
			int32 Last = 0;
			for (int32 Index = 0; Index < Outputs.Num(); ++Index)
			{
				const FString Prefix = Node.Name + TEXT(".") + Outputs[Index];
				for (const TPair<FString, FString>& Link : Graph.DataSource)
				{
					if (Link.Value == Prefix || Link.Value.StartsWith(Prefix + TEXT(".")))
					{
						Last = FMath::Max(Last, Index);
					}
				}
			}
			for (int32 Index = 0; Index <= Last; ++Index)
			{
				const FString Name = UniqueName(Outputs[Index]);
				For->Names.Add(Name);
				LoopNames.Add(Node.Name + TEXT(".") + Outputs[Index], Name);
			}
			if (bCount)
			{
				const FDcrPin* Count = Signature.Find(TEXT("Count"));
				TArray<FDabExprPtr> Args;
				FDabExprPtr CountExpr = Count ? InputExpr(Node, *Count, Signature, true) : nullptr;
				Args.Add(CountExpr ? MoveTemp(CountExpr) : MakeDcrInteger(1));
				For->Iterable = MakeDcrCall(TEXT("Range"), MoveTemp(Args));
			}
			else
			{
				const FDcrPin* Array = Signature.Find(TEXT("Array"));
				// A literal array tells no element type: a typed let names it.
				For->Iterable = !Array ? nullptr
					: FindTypingPin(Node, Signature) == Array ? TypedLetFor(Node, *Array, Signature) : InputExpr(Node, *Array, Signature, true);
				if (!For->Iterable)
				{
					For->Iterable = MakeDcrList({});
				}
			}
			For->Body = MakeUnique<FDabCodeBlock>();
			WriteBlock(*Skel.Blocks[0], For->Body->Statements);
			WriteLetsAt(*Skel.Blocks[0], Skel.Blocks[0]->Statements.Num(), For->Body->Statements);
			return For;
		}

		FDabStmtPtr FBodyLowering::WriteReturn(const FDcrModelNode& Node)
		{
			const FSignature& Signature = File.GetSignature(Node, Function);
			TArray<TPair<FString, FDabExprPtr>> Items;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				if (FDabExprPtr Value = InputExpr(Node, Pin, Signature, false))
				{
					Items.Emplace(Pin.Name, MoveTemp(Value));
				}
			}
			if (Items.IsEmpty())
			{
				return nullptr;
			}
			TUniquePtr<FDabReturnStmt> Return = MakeUnique<FDabReturnStmt>();
			Return->Value = MakeDcrTuple(MoveTemp(Items));
			return Return;
		}

		TUniquePtr<FDabCodeBlock> FBodyLowering::LowerPureCode(const FDcrModelNode& Return)
		{
			Root = MakeUnique<FSkelBlock>();
			bCode = true;
			for (const FDcrModelFunctionPin& Input : Function->Inputs)
			{
				UsedNames.Add(Input.Name);
			}
			FSkel& Skel = Root->Statements.AddDefaulted_GetRef();
			Skel.Node = &Return;
			Skel.Kind = FSkel::EKind::Return;
			Skel.Ordinal = NextOrdinal++;
			BlockOf.Add(Return.Name, Root.Get());
			Visited.Add(Return.Name);

			// Data nodes nothing reads: lets of their own in a Detached block before the return.
			TArray<const FDcrModelNode*> Heads;
			TArray<const FDcrModelNode*> Dangling;
			TSet<FString> Reached;
			Reached.Add(Return.Name);
			FFileLowering::FindDetached(Graph, Reached, Heads, Dangling);
			FSkelBlock* DetachedBlock = Dangling.IsEmpty() ? nullptr : &AddDetachedSkel(*Root, 0, *Dangling[0]);

			CountUses();
			for (const FDcrModelNode* Node : Dangling)
			{
				TSet<FString> Seen;
				CountNodeInputs(*Node, Seen);
				UseCounts.FindOrAdd(Node->Name) = 2;
			}
			bool bShared = false;
			for (const TPair<FString, int32>& Use : UseCounts)
			{
				const FDcrModelNode* Node = Graph.Find(Use.Key);
				bShared |= Node && IsLetData(*Node);
			}
			if (!bShared && Function->Locals.IsEmpty())
			{
				return nullptr;
			}
			ChooseInlinedStatements(*Root);
			PlaceLets(Dangling, DetachedBlock);
			TUniquePtr<FDabCodeBlock> Code = MakeUnique<FDabCodeBlock>();
			WriteBlock(*Root, Code->Statements);
			return Code;
		}

		FDabExprPtr FBodyLowering::LowerPureReturn(const FDcrModelNode& Return)
		{
			Root = MakeUnique<FSkelBlock>();
			const FSignature& Signature = File.GetSignature(Return, Function);
			// Nothing is shared in a pure function's text: each use is written out (pure nodes give the same value).
			TArray<TPair<FString, FDabExprPtr>> Items;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				FDabExprPtr Value = InputExpr(Return, Pin, Signature, Signature.Pins.Num() == 1);
				if (Value)
				{
					Items.Emplace(Pin.Name, MoveTemp(Value));
				}
			}
			if (Signature.Pins.Num() == 1 && Items.Num() == 1)
			{
				return MoveTemp(Items[0].Value);
			}
			return MakeDcrTuple(MoveTemp(Items));
		}

		// ------------------------------------------------------------------------------------------
		// Expressions
		// ------------------------------------------------------------------------------------------

		Model::FDcrType FBodyLowering::SourceType(const FString& SourcePath)
		{
			const FPinPath Source = SplitPinPath(SourcePath);
			const FDcrModelNode* Node = Graph.Find(Source.Node);
			if (!Node)
			{
				return FDcrType();
			}
			FDcrType RootType;
			const FProperty* RootProperty = nullptr;
			if (IsForLoopCount(*Node))
			{
				RootType = MakeDcrBuiltinType(Source.Root == TEXT("Ratio") ? TEXT("float") : TEXT("int32"));
			}
			else
			{
				const FSignature& Signature = File.GetSignature(*Node, Function);
				const FDcrPin* Pin = Signature.Find(Source.Root);
				if (!Pin)
				{
					return FDcrType();
				}
				RootType = Pin->Type;
				RootProperty = Pin->Property;
			}
			if (Source.Sub.IsEmpty())
			{
				return RootType;
			}
			FDcrValueSlot Slot;
			if (!RootProperty && Slot.Init(RootType))
			{
				RootProperty = Slot.GetProperty();
			}
			const FProperty* Leaf = WalkSubPath(RootProperty, Source.Sub);
			return Leaf ? GetDcrType(*Leaf) : FDcrType();
		}

		bool FBodyLowering::HasVariable(const FString& Name) const
		{
			// The rig's, any function's, or (for an Inline body) the inline function's own inputs.
			return File.VariableNames.Contains(Name)
				|| (Function && (Function->Inputs.ContainsByPredicate([&Name](const FDcrModelFunctionPin& Input) { return Input.Name == Name; })
					|| Function->Locals.ContainsByPredicate([&Name](const FDcrModelVariable& Local) { return Local.Name == Name; })));
		}

		FDabExprPtr FBodyLowering::VariableExpr(const FDcrModelNode& Node)
		{
			if (HasVariable(Node.Variable))
			{
				return MakeDcrName(Node.Variable);
			}
			File.AddUnsupported(Location + TEXT("/") + Node.Name, FString::Printf(TEXT("a node of the variable `%s`, which the rig does not have"), *Node.Variable));
			return MakeDcrUnsupported(FString::Printf(TEXT("the missing variable %s"), *Node.Variable));
		}

		FDabExprPtr FBodyLowering::SourceExpr(const FString& SourcePath)
		{
			const FPinPath Source = SplitPinPath(SourcePath);
			const FDcrModelNode* Node = Graph.Find(Source.Node);
			if (!Node)
			{
				return MakeDcrUnsupported(FString::Printf(TEXT("a link from %s"), *SourcePath));
			}

			// The property the sub-path walks, for user struct member names.
			auto RootPropertyOf = [this, Node, &Source](FDcrValueSlot& Slot) -> const FProperty*
			{
				const FSignature& Signature = File.GetSignature(*Node, Function);
				const FDcrPin* Pin = Signature.Find(Source.Root);
				if (!Pin)
				{
					return nullptr;
				}
				if (Pin->Property)
				{
					return Pin->Property;
				}
				return Pin->Type.IsSet() && Slot.Init(Pin->Type) ? Slot.GetProperty() : nullptr;
			};
			FDcrValueSlot Slot;

			if (const FString* LoopName = LoopNames.Find(Source.Node + TEXT(".") + Source.Root))
			{
				FDcrValueSlot LoopSlot;
				const FDcrType Type = SourceType(Source.Node + TEXT(".") + Source.Root);
				return AppendSubPath(MakeDcrName(*LoopName), Type.IsSet() && LoopSlot.Init(Type) ? LoopSlot.GetProperty() : nullptr, Source.Sub);
			}
			if (Node->Kind == EDcrNodeKind::Entry)
			{
				return AppendSubPath(MakeDcrName(Source.Root), RootPropertyOf(Slot), Source.Sub);
			}
			if (Node->Kind == EDcrNodeKind::VariableGet)
			{
				Visited.Add(Node->Name);
				if (Collecting)
				{
					Collecting->AddUnique(Node->Name);
				}
				return AppendSubPath(VariableExpr(*Node), RootPropertyOf(Slot), Source.Sub);
			}

			const FSignature& Signature = File.GetSignature(*Node, Function);
			const bool bSingleOutput = Signature.bKnown && Signature.NumOutputs() == 1;
			FDabExprPtr Base;
			if (const FString* LetName = LetNames.Find(Node->Name))
			{
				Base = MakeDcrName(*LetName);
			}
			else if (LetPlaces.Contains(Node->Name) && !Building.Contains(Node->Name))
			{
				// A let whose place was not reached (a use outside the block it was placed in): write it here.
				File.AddUnsupported(Location + TEXT("/") + Node->Name, TEXT("its value is used outside the block it belongs to"));
				Base = NodeExpr(*Node);
			}
			else
			{
				const ERole Role = GetRole(*Node);
				if (Role == ERole::Statement && !InlinedStatements.Contains(Node->Name))
				{
					File.AddUnsupported(Location + TEXT("/") + Node->Name, TEXT("its output is used before it runs, or from another code block"));
					return MakeDcrUnsupported(FString::Printf(TEXT("the output of %s"), *Node->Name));
				}
				Base = NodeExpr(*Node);
			}
			if (bSingleOutput)
			{
				return AppendSubPath(MoveTemp(Base), RootPropertyOf(Slot), Source.Sub);
			}
			return AppendSubPath(MakeDcrMember(MoveTemp(Base), Source.Root), RootPropertyOf(Slot), Source.Sub);
		}

		FDabExprPtr FBodyLowering::InputExpr(const FDcrModelNode& Node, const FDcrPin& Pin, const FSignature& Signature, bool bForce, bool bForType)
		{
			const FString PinPath = Node.Name + TEXT(".") + Pin.Name;

			// An injected node on this pin: `Injected(Node(...))`.
			if (const FDcrModelNode* const* Injected = Graph.InjectedInputs.Find(PinPath))
			{
				TArray<FDabExprPtr> Args;
				Args.Add(NodeExpr(**Injected));
				return MakeDcrCall(TEXT("Injected"), MoveTemp(Args));
			}

			if (const FString* Source = Graph.DataSource.Find(PinPath))
			{
				return SourceExpr(*Source);
			}

			const FDcrValue* Value = FindValue(Node, Pin.Name);
			const FString* DefaultText = Signature.Defaults.Find(Pin.Name);
			const TArray<FString>* SubTargets = Graph.SubTargets.Find(PinPath);
			if (!Value && !SubTargets && !bForce)
			{
				return nullptr;
			}

			// The pin's value in memory, to write a literal from.
			FDcrValueSlot ValueSlot;
			FDcrValueSlot DefaultSlot;
			const bool bSlots = Pin.Property ? (ValueSlot.Init(*Pin.Property) && DefaultSlot.Init(*Pin.Property))
				: (Pin.Type.IsSet() && ValueSlot.Init(Pin.Type) && DefaultSlot.Init(Pin.Type));
			if (!bSlots)
			{
				return Value || SubTargets ? MakeDcrUnsupported(FString::Printf(TEXT("a value of %s"), *PinPath)) : nullptr;
			}
			if (!bForType)
			{
				DefaultSlot.ImportText(DefaultText ? *DefaultText : FString());
			}
			if (!ValueSlot.ImportText(Value ? Value->Value : (DefaultText ? *DefaultText : FString())))
			{
				File.AddUnsupported(Location + TEXT("/") + PinPath, FString::Printf(TEXT("the value `%s` does not read back"), Value ? *Value->Value : TEXT("")));
			}

			if (SubTargets)
			{
				TMap<FString, FDabExprPtr> Parts;
				for (const FString& Target : *SubTargets)
				{
					const FPinPath TargetPath = SplitPinPath(Target);
					Parts.Add(TargetPath.Sub, SourceExpr(Graph.DataSource[Target]));
				}
				TArray<FString> Unplaced;
				FDabExprPtr Literal = MakeDcrValueExprWithParts(*ValueSlot.GetProperty(), ValueSlot.GetMemory(), DefaultSlot.GetMemory(), Parts, Unplaced);
				for (const FString& Path : Unplaced)
				{
					File.AddUnsupported(Location + TEXT("/") + PinPath + TEXT(".") + Path, TEXT("a link into a part of the value that does not exist"));
				}
				return Literal;
			}
			// A value the engine kept although it is what a new node holds (its text was spelled differently): nothing to say.
			if (!bForce && AreDcrValuesSame(*ValueSlot.GetProperty(), ValueSlot.GetMemory(), DefaultSlot.GetMemory()))
			{
				return nullptr;
			}
			return MakeDcrValueExpr(*ValueSlot.GetProperty(), ValueSlot.GetMemory(), DefaultSlot.GetMemory());
		}

		bool FBodyLowering::IsTypedByUses(const FDcrModelNode& Node, const FSignature& Signature)
		{
			// Its one wildcard output goes only into pins of exactly its type: semantic settles the node at its first use.
			const FDcrPin* Output = nullptr;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				const bool bWildcard = Node.ResolvedTypes.ContainsByPredicate([&Pin](const TPair<FString, FDcrType>& Resolved) { return Resolved.Key == Pin.Name; });
				if (bWildcard && Pin.bOutput && !Pin.bInput)
				{
					if (Output)
					{
						return false;
					}
					Output = &Pin;
				}
			}
			const TArray<FString>* Targets = Graph.DataTargets.Find(Node.Name);
			if (!Output || !Output->Type.IsSet() || !Targets)
			{
				return false;
			}
			bool bUsed = false;
			for (const FString& Target : *Targets)
			{
				const FString* Source = Graph.DataSource.Find(Target);
				if (!Source)
				{
					return false;
				}
				const FPinPath SourcePath = SplitPinPath(*Source);
				if (SourcePath.Root != Output->Name)
				{
					continue;
				}
				const FPinPath TargetPath = SplitPinPath(Target);
				const FDcrModelNode* Consumer = Graph.Find(TargetPath.Node);
				if (!SourcePath.Sub.IsEmpty() || !Consumer || Graph.InjectedInputs.Contains(TargetPath.Node + TEXT(".") + TargetPath.Root))
				{
					return false;
				}
				const bool bIntoWildcard = Consumer->Kind == EDcrNodeKind::Dispatch
					&& Consumer->ResolvedTypes.ContainsByPredicate([&TargetPath](const TPair<FString, FDcrType>& Resolved) { return Resolved.Key == TargetPath.Root; });
				if (bIntoWildcard || SourceType(Target) != Output->Type)
				{
					return false;
				}
				// An operand of an operator: semantic needs the operands' types to choose the operator's node.
				const UScriptStruct* ConsumerStruct = File.GetNodeStruct(*Consumer);
				const FDcrOperator* Operator = ConsumerStruct ? FindDcrOperatorForStruct(*ConsumerStruct) : nullptr;
				if (Operator && Operator->Operands.ContainsByPredicate([&TargetPath](const TCHAR* Operand) { return TargetPath.Root == Operand; }))
				{
					return false;
				}
				bUsed = true;
			}
			return bUsed;
		}

		const FDcrPin* FBodyLowering::FindUntypedOutput(const FDcrModelNode& Node)
		{
			// A dispatch with no wildcard input that tells its types (`FromString(...)`) and no use that does: the let names the type.
			if (Node.Kind != EDcrNodeKind::Dispatch || Node.ResolvedTypes.IsEmpty() || !Node.ExecuteIn.IsEmpty())
			{
				return nullptr;
			}
			const FSignature& Signature = File.GetSignature(Node, Function);
			const FDcrPin* Output = nullptr;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				const bool bWildcard = Node.ResolvedTypes.ContainsByPredicate([&Pin](const TPair<FString, FDcrType>& Resolved) { return Resolved.Key == Pin.Name; });
				if (!bWildcard)
				{
					continue;
				}
				if (Pin.bInput)
				{
					return nullptr;
				}
				if (Output)
				{
					return nullptr;
				}
				Output = &Pin;
			}
			if (!Output || !Output->Type.IsSet() || GetDcrTypeName(Output->Type).IsEmpty() || IsTypedByUses(Node, Signature))
			{
				return nullptr;
			}
			return Output;
		}

		const FDcrPin* FBodyLowering::FindTypingPin(const FDcrModelNode& Node, const FSignature& Signature)
		{
			// A dispatch none of whose wildcard inputs is linked: nothing tells its types but a typed let of one of its
			// values, or (for a value) the pins it goes into.
			if (Node.Kind != EDcrNodeKind::Dispatch || Node.ResolvedTypes.IsEmpty())
			{
				return nullptr;
			}
			if (Node.ExecuteIn.IsEmpty() && IsTypedByUses(Node, Signature))
			{
				return nullptr;
			}
			const FDcrPin* First = nullptr;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				const bool bWildcard = Node.ResolvedTypes.ContainsByPredicate([&Pin](const TPair<FString, FDcrType>& Resolved) { return Resolved.Key == Pin.Name; });
				if (!Pin.bInput || !bWildcard)
				{
					continue;
				}
				const FString PinPath = Node.Name + TEXT(".") + Pin.Name;
				if (Graph.DataSource.Contains(PinPath) || Graph.InjectedInputs.Contains(PinPath))
				{
					return nullptr;
				}
				if (!First && Pin.Type.IsSet() && !GetDcrTypeName(Pin.Type).IsEmpty())
				{
					First = &Pin;
				}
			}
			return First;
		}

		FDabExprPtr FBodyLowering::TypedLetFor(const FDcrModelNode& Node, const FDcrPin& Pin, const FSignature& Signature)
		{
			// `true`, `false` and `Bone(name)` tell their type themselves; a pure function's value has no place for a let.
			const bool bBool = Pin.Type.CPPType == TEXT("bool");
			FDabExprPtr Value = InputExpr(Node, Pin, Signature, true, bCode && !bBool);
			const bool bKey = Value && Value->Is<FDabCallExpr>() && Pin.Type.ObjectPath == FRigElementKey::StaticStruct()->GetPathName();
			if (!Value || bBool || bKey || !bCode)
			{
				return Value;
			}
			const FString Name = UniqueName(Pin.Name);
			TUniquePtr<FDabLetStmt> Let = MakeUnique<FDabLetStmt>();
			Let->LetKind = EDabLetKind::Typed;
			Let->TypeName = GetDcrTypeName(Pin.Type);
			Let->Name = Name;
			Let->Value = MoveTemp(Value);
			Hoisted.Add(MoveTemp(Let));
			return MakeDcrName(Name);
		}

		FDabExprPtr FBodyLowering::OperatorExpr(const FDcrModelNode& Node, const UScriptStruct& Struct, const FSignature& Signature)
		{
			const FDcrOperator* Operator = FindDcrOperatorForStruct(Struct);
			if (!Operator || !Node.AggregatePins.IsEmpty())
			{
				return nullptr;
			}
			// Every input is an operand, and at least one operand is linked: the operator's node is then chosen by type.
			bool bAnyLinked = false;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				if (!Pin.bInput)
				{
					continue;
				}
				const FString PinPath = Node.Name + TEXT(".") + Pin.Name;
				const bool bOperand = Operator->Operands.ContainsByPredicate([&Pin](const TCHAR* Operand) { return Pin.Name == Operand; });
				const bool bLinked = Graph.DataSource.Contains(PinPath);
				if (!bOperand)
				{
					if (bLinked || FindValue(Node, Pin.Name) || Graph.SubTargets.Contains(PinPath))
					{
						return nullptr;
					}
					continue;
				}
				if (Graph.SubTargets.Contains(PinPath) || Graph.InjectedInputs.Contains(PinPath))
				{
					return nullptr;
				}
				bAnyLinked |= bLinked;
			}
			if (!bAnyLinked)
			{
				return nullptr;
			}
			if (Operator->bDispatch && Operator->Kind == EDcrOperatorKind::Index)
			{
				// `a[i]` reads the array pin; it must be linked for its type.
				if (!Graph.DataSource.Contains(Node.Name + TEXT(".Array")))
				{
					return nullptr;
				}
			}
			if (Operator->Kind == EDcrOperatorKind::Conditional)
			{
				if (!Graph.DataSource.Contains(Node.Name + TEXT(".True")) && !Graph.DataSource.Contains(Node.Name + TEXT(".False")))
				{
					return nullptr;
				}
			}

			TArray<FDabExprPtr> Operands;
			TArray<FDcrOperand> Known;
			for (const TCHAR* OperandName : Operator->Operands)
			{
				const FDcrPin* Pin = Signature.Find(OperandName);
				if (!Pin)
				{
					return nullptr;
				}
				const FString PinPath = Node.Name + TEXT(".") + Pin->Name;
				FDabExprPtr Expr = InputExpr(Node, *Pin, Signature, true);
				if (!Expr)
				{
					return nullptr;
				}
				FDcrOperand& Operand = Known.AddDefaulted_GetRef();
				if (const FString* Source = Graph.DataSource.Find(PinPath))
				{
					Operand.Type = SourceType(*Source);
				}
				Operands.Add(MoveTemp(Expr));
			}
			for (int32 Index = 0; Index < Known.Num(); ++Index)
			{
				if (!Known[Index].Type.IsSet())
				{
					Known[Index].Literal = Operands[Index].Get();
				}
			}

			if (!Operator->bDispatch)
			{
				// Semantic must pick this very unit back (the double twin of a float one, as it is written) for these operands.
				TArray<const FDcrOperator*> Candidates = Operator->Kind == EDcrOperatorKind::Binary
					? GetDcrBinaryOperators(Operator->Binary)
					: TArray<const FDcrOperator*>{ GetDcrUnaryOperator(Operator->Unary) };
				const FDcrOperatorChoice Choice = ResolveDcrOperatorUnit(Candidates, Known);
				const UScriptStruct* Written = GetDcrDoubleTwin(Struct) ? GetDcrDoubleTwin(Struct) : &Struct;
				if (!Choice.Unit || Choice.Unit != Written)
				{
					return nullptr;
				}
			}

			switch (Operator->Kind)
			{
			case EDcrOperatorKind::Binary: return MakeDcrBinary(Operator->Binary, MoveTemp(Operands[0]), MoveTemp(Operands[1]));
			case EDcrOperatorKind::Unary: return MakeDcrUnary(Operator->Unary, MoveTemp(Operands[0]));
			case EDcrOperatorKind::Conditional: return MakeDcrConditional(MoveTemp(Operands[0]), MoveTemp(Operands[1]), MoveTemp(Operands[2]));
			case EDcrOperatorKind::Index: return MakeDcrIndex(MoveTemp(Operands[0]), MoveTemp(Operands[1]));
			}
			return nullptr;
		}

		FDabExprPtr FBodyLowering::NodeExpr(const FDcrModelNode& Node)
		{
			Visited.Add(Node.Name);
			if (Collecting)
			{
				Collecting->AddUnique(Node.Name);
			}
			if (Building.Contains(Node.Name))
			{
				File.AddUnsupported(Location + TEXT("/") + Node.Name, TEXT("its inputs depend on its own outputs"));
				return MakeDcrUnsupported(FString::Printf(TEXT("a cycle through %s"), *Node.Name));
			}
			Building.Add(Node.Name);
			ON_SCOPE_EXIT { Building.Remove(Node.Name); };

			const FSignature& Signature = File.GetSignature(Node, Function);
			if (Node.Kind == EDcrNodeKind::VariableGet)
			{
				return VariableExpr(Node);
			}
			if (Node.Kind == EDcrNodeKind::InvokeEntry)
			{
				// `RunEvent(Backward)`: the event as `Event` writes it.
				const FDcrEventWord* Event = FindDcrEventByEventName(Node.Function);
				TArray<FDabExprPtr> Args;
				Args.Add(MakeDcrName(Event ? FString(Event->Word) : Node.Function));
				return MakeDcrCall(DcrRunEventCallee, MoveTemp(Args));
			}
			if (!Signature.bKnown)
			{
				File.AddUnsupported(Location + TEXT("/") + Node.Name, FString::Printf(TEXT("a %s node whose pins could not be read"), LexToString(Node.Kind)));
				return MakeDcrUnsupported(FString::Printf(TEXT("node %s"), *Node.Name));
			}

			const UScriptStruct* Struct = File.GetNodeStruct(Node);
			if (Struct)
			{
				if (FDabExprPtr Operator = OperatorExpr(Node, *Struct, Signature))
				{
					return Operator;
				}
			}

			FDabExprPtr Callee;
			FString Placeholder;
			if (Struct)
			{
				// Float math is written as its double twin, as the text builds it (an operator picks the twin too).
				const UScriptStruct* Written = GetDcrDoubleTwin(*Struct) ? GetDcrDoubleTwin(*Struct) : Struct;
				const FString TypeName = GetDcrNodeTypeName(*Written);
				// A rig function of the same name would be found first: then the struct's full name.
				Callee = MakeDcrName(File.FindFunction(TypeName) ? Written->GetName() : TypeName);
				// The engine takes these into a module rig only; older rigs and libraries hold them all the same.
				const bool bModuleOnly = Struct == FRigUnit_ConnectorExecution::StaticStruct() || Struct == FRigUnit_GetCandidates::StaticStruct()
					|| Struct == FRigUnit_DiscardMatches::StaticStruct();
				if (bModuleOnly && !File.Model.bModule)
				{
					File.AddUnsupported(Location + TEXT("/") + Node.Name, FString::Printf(TEXT("a %s node, which only a module rig can hold"), *TypeName));
					Placeholder = FString::Printf(TEXT("a %s node"), *TypeName);
					Callee = MakeDcrName(TEXT("Unsupported"));
				}
			}
			else if (Node.Kind == EDcrNodeKind::LocalCall)
			{
				Callee = MakeDcrName(Node.Function);
			}
			else if (Node.Kind == EDcrNodeKind::Collapse)
			{
				const FDcrModelFunction* Inline = File.GetInline(Node);
				Callee = MakeDcrName(Inline ? Inline->Name : Node.Name);
			}
			else if (Node.Kind == EDcrNodeKind::ExternalCall)
			{
				FString Host = Node.FunctionHost;
				Host.RemoveFromEnd(TEXT("_C"));
				File.UsedLibraries.AddUnique(Host);
				// `Library.Name` where the bare name would find something else first, or as well.
				const bool bQualify = File.FindFunction(Node.Function) || !FindDcrUnits(Node.Function).IsEmpty()
					|| !FindDcrDispatches(Node.Function).IsEmpty() || !FindDcrTemplateNotations(Node.Function).IsEmpty()
					|| File.CountLibraryFunctions(Node.Function) > 1;
				if (bQualify && File.LibraryNameCounts.FindRef(File.GetLibraryName(Host)) > 1)
				{
					// Its arguments are still written, inside the placeholder: what they call is still called.
					File.AddUnsupported(Location + TEXT("/") + Node.Name, FString::Printf(TEXT("a call of `%s` in one of several rigs in Uses named %s"),
						*Node.Function, *File.GetLibraryName(Host)));
					Placeholder = FString::Printf(TEXT("a call of %s in %s"), *Node.Function, *Host);
					Callee = MakeDcrName(TEXT("Unsupported"));
				}
				else
				{
					Callee = bQualify ? MakeDcrMember(MakeDcrName(File.GetLibraryName(Host)), Node.Function) : MakeDcrName(Node.Function);
				}
			}
			else
			{
				File.AddUnsupported(Location + TEXT("/") + Node.Name, FString::Printf(TEXT("a %s node"), LexToString(Node.Kind)));
				return MakeDcrUnsupported(FString::Printf(TEXT("node %s"), *Node.Name));
			}

			// Aggregate nodes take every input by position: `Add(a, b, c)`. A data node gives its first input by
			// position (`GetTransform(Bone(root))`), and both when it has two and both are written
			// (`NameConcat(A, "_ctrl")`). Statements name every argument.
			const bool bAggregate = !Node.AggregatePins.IsEmpty();
			const bool bPure = Node.ExecuteIn.IsEmpty();
			int32 NumInputs = 0;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				NumInputs += Pin.bInput ? 1 : 0;
			}
			const FDcrPin* TypingPin = FindTypingPin(Node, Signature);
			TArray<FDabExprPtr> Positional;
			TArray<TPair<FString, FDabExprPtr>> Named;
			bool bStillPositional = bPure || bAggregate;
			int32 InputIndex = 0;
			for (const FDcrPin& Pin : Signature.Pins)
			{
				if (!Pin.bInput)
				{
					continue;
				}
				FDabExprPtr Arg = &Pin == TypingPin ? TypedLetFor(Node, Pin, Signature) : InputExpr(Node, Pin, Signature, bAggregate);
				const bool bWithinLimit = bAggregate || InputIndex == 0 || (NumInputs == 2 && InputIndex == 1);
				++InputIndex;
				if (!Arg)
				{
					bStillPositional = false;
					continue;
				}
				if (bStillPositional && bWithinLimit)
				{
					Positional.Add(MoveTemp(Arg));
				}
				else
				{
					bStillPositional = false;
					Named.Emplace(Pin.Name, MoveTemp(Arg));
				}
			}
			for (const FString& Extra : Node.AggregatePins)
			{
				if (!Signature.Find(Extra))
				{
					// Pins added past the struct's own: their values are links or values by the same name.
					FDcrPin Added;
					Added.Name = Extra;
					Added.bInput = true;
					if (const FDcrPin* First = Signature.Pins.FindByPredicate([](const FDcrPin& Pin) { return Pin.bInput; }))
					{
						Added.Property = First->Property;
						Added.Type = First->Type;
					}
					FDabExprPtr Arg = InputExpr(Node, Added, Signature, true);
					Positional.Add(Arg ? MoveTemp(Arg) : MakeDcrUnsupported(Extra));
				}
			}
			if (Struct && Struct->GetPathName() == DcrUserEventStructPath)
			{
				return MakeDcrUnsupported(TEXT("a user event used as a value"));
			}
			if (!Placeholder.IsEmpty())
			{
				Positional.Insert(MakeDcrString(Placeholder), 0);
			}
			FDabExprPtr Call = MakeDcrCall(MoveTemp(Callee), MoveTemp(Positional), MoveTemp(Named));
			if (!Node.InjectedOn.IsEmpty() && !Node.bInjectedAsInput)
			{
				TArray<FDabExprPtr> Args;
				Args.Add(MoveTemp(Call));
				Args.Add(MakeDcrName(TEXT("Output")));
				return MakeDcrCall(TEXT("Injected"), MoveTemp(Args));
			}
			return Call;
		}

		// ------------------------------------------------------------------------------------------
		// Comment boxes
		// ------------------------------------------------------------------------------------------

		void FBodyLowering::PlaceComments(FDabCodeBlock& Code, TArray<const FDcrModelNode*>& Comments)
		{
			struct FStmtRef
			{
				TArray<FDabStmtPtr>* Block = nullptr;
				const FDabStmt* Stmt = nullptr;
			};
			TArray<FStmtRef> Order;
			TFunction<void(TArray<FDabStmtPtr>&)> Gather = [&Order, &Gather](TArray<FDabStmtPtr>& Block)
			{
				for (FDabStmtPtr& Stmt : Block)
				{
					Order.Add({ &Block, Stmt.Get() });
					if (FDabIfStmt* If = Stmt->As<FDabIfStmt>())
					{
						for (FDabIfStmt* Current = If; Current; Current = Current->ElseIf.Get())
						{
							if (Current->Then)
							{
								Gather(Current->Then->Statements);
							}
							if (Current->Else)
							{
								Gather(Current->Else->Statements);
							}
						}
					}
					else if (FDabForStmt* For = Stmt->As<FDabForStmt>(); For && For->Body)
					{
						Gather(For->Body->Statements);
					}
					else if (FDabDetachedStmt* Detached = Stmt->As<FDabDetachedStmt>(); Detached && Detached->Body)
					{
						Gather(Detached->Body->Statements);
					}
				}
			};
			Gather(Code.Statements);

			auto Inside = [this](const FDcrModelNode& Comment, const FString& NodeName)
			{
				const FDcrModelNode* Node = Graph.Find(NodeName);
				if (!Node)
				{
					return false;
				}
				const FVector2D Min = Comment.Position;
				const FVector2D Max = Comment.Position + Comment.CommentSize;
				return Node->Position.X >= Min.X && Node->Position.Y >= Min.Y && Node->Position.X <= Max.X && Node->Position.Y <= Max.Y;
			};
			auto IndexIn = [](const TArray<FDabStmtPtr>& Block, const FDabStmt* Stmt)
			{
				return Block.IndexOfByPredicate([Stmt](const FDabStmtPtr& Other) { return Other.Get() == Stmt; });
			};

			// Larger boxes first, so a box inside another becomes a region inside its region.
			TArray<const FDcrModelNode*> Sorted = Comments;
			Sorted.StableSort([](const FDcrModelNode& A, const FDcrModelNode& B)
			{
				return A.CommentSize.X * A.CommentSize.Y > B.CommentSize.X * B.CommentSize.Y;
			});

			struct FRegion
			{
				const FDabStmt* First = nullptr;
				const FDabStmt* Last = nullptr;
				FString Label;
			};
			TMap<TArray<FDabStmtPtr>*, TArray<FRegion>> Regions;
			for (const FDcrModelNode* Comment : Sorted)
			{
				TArray<const FStmtRef*> Touching;
				for (const FStmtRef& Ref : Order)
				{
					const TArray<FString>* Nodes = StmtNodes.Find(Ref.Stmt);
					if (Nodes && Algo::AnyOf(*Nodes, [&Inside, Comment](const FString& Name) { return Inside(*Comment, Name); }))
					{
						Touching.Add(&Ref);
					}
				}
				if (Touching.IsEmpty())
				{
					continue;
				}
				Comments.Remove(Comment);

				// A region: one block, consecutive statements (lets placed between them may join in).
				TArray<FDabStmtPtr>* Block = Touching[0]->Block;
				bool bRegion = Algo::AllOf(Touching, [Block](const FStmtRef* Ref) { return Ref->Block == Block; });
				int32 First = INDEX_NONE;
				int32 Last = INDEX_NONE;
				if (bRegion)
				{
					First = IndexIn(*Block, Touching[0]->Stmt);
					Last = IndexIn(*Block, Touching.Last()->Stmt);
					for (int32 Index = First; Index <= Last && bRegion; ++Index)
					{
						const FDabStmt* Stmt = (*Block)[Index].Get();
						bRegion = Stmt->Is<FDabLetStmt>() || Touching.ContainsByPredicate([Stmt](const FStmtRef* Ref) { return Ref->Stmt == Stmt; });
					}
					// A box around one statement is the comment above it; a region is two or more.
					bRegion &= Last > First;
				}
				if (bRegion)
				{
					for (const FRegion& Other : Regions.FindOrAdd(Block))
					{
						const int32 OtherFirst = IndexIn(*Block, Other.First);
						const int32 OtherLast = IndexIn(*Block, Other.Last);
						const bool bDisjoint = Last < OtherFirst || First > OtherLast;
						const bool bWithin = First >= OtherFirst && Last <= OtherLast;
						const bool bAround = First <= OtherFirst && Last >= OtherLast;
						bRegion &= bDisjoint || bWithin || bAround;
					}
				}
				if (bRegion)
				{
					Regions.FindOrAdd(Block).Add({ (*Block)[First].Get(), (*Block)[Last].Get(), Comment->CommentText });
					continue;
				}
				AddDcrCommentTrivia(File.File.Trivia.FindOrAdd(Touching[0]->Stmt), Comment->CommentText);
			}

			for (TPair<TArray<FDabStmtPtr>*, TArray<FRegion>>& Pair : Regions)
			{
				TArray<FDabStmtPtr>& Block = *Pair.Key;
				// Outer regions first: an inner region's markers then land inside the outer one's.
				Pair.Value.StableSort([&Block, &IndexIn](const FRegion& A, const FRegion& B)
				{
					return IndexIn(Block, A.Last) - IndexIn(Block, A.First) > IndexIn(Block, B.Last) - IndexIn(Block, B.First);
				});
				for (const FRegion& Region : Pair.Value)
				{
					Block.Insert(MakeRegion(Region.Label, false), IndexIn(Block, Region.First));
					Block.Insert(MakeRegion(FString(), true), IndexIn(Block, Region.Last) + 1);
				}
			}
		}

		// ------------------------------------------------------------------------------------------
		// The file: sections, functions, events
		// ------------------------------------------------------------------------------------------

		const FDcrModelFunction* FFileLowering::GetInline(const FDcrModelNode& Collapse)
		{
			if (const TSharedPtr<FDcrModelFunction>* Found = Inlines.Find(&Collapse))
			{
				return Found->Get();
			}
			if (!Collapse.Contained)
			{
				Inlines.Add(&Collapse, nullptr);
				return nullptr;
			}
			TSharedPtr<FDcrModelFunction> Inline = MakeShared<FDcrModelFunction>();
			FString Name = Collapse.Name;
			for (int32 Suffix = 1; GlobalNames.Contains(Name); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s_%d"), *Collapse.Name, Suffix);
			}
			GlobalNames.Add(Name);
			Inline->Name = Name;
			Inline->bMutable = !Collapse.ExecuteIn.IsEmpty() || !Collapse.ExecuteOut.IsEmpty();
			for (const FDcrModelNode& Node : Collapse.Contained->Nodes)
			{
				if (Node.Kind != EDcrNodeKind::Entry && Node.Kind != EDcrNodeKind::Return)
				{
					continue;
				}
				TArray<FDcrModelFunctionPin>& Pins = Node.Kind == EDcrNodeKind::Entry ? Inline->Inputs : Inline->Outputs;
				for (const TPair<FString, FDcrType>& Resolved : Node.ResolvedTypes)
				{
					FDcrModelFunctionPin& Pin = Pins.AddDefaulted_GetRef();
					Pin.Name = Resolved.Key;
					Pin.Type = Resolved.Value;
					if (const FDcrValue* Value = FindValue(Node, Resolved.Key))
					{
						Pin.Default = Value->Value;
					}
				}
			}
			Inlines.Add(&Collapse, Inline);
			InlineOrder.Add(&Collapse);
			return Inline.Get();
		}

		TUniquePtr<FDabNamedBlockStmt> FFileLowering::LowerFunction(const FDcrModelFunction& Function, const FDcrModelGraph& Graph, bool bInline)
		{
			TUniquePtr<FDabNamedBlockStmt> Stmt = MakeUnique<FDabNamedBlockStmt>();
			Stmt->Word = TEXT("Function");
			Stmt->Name = Function.Name;
			Stmt->bHasParams = true;
			const FString Location = (bInline ? TEXT("Inline/") : TEXT("FunctionLibrary/")) + Function.Name;

			auto MakeParam = [this, &Location](const FDcrModelFunctionPin& Pin, bool bWithDefault)
			{
				TUniquePtr<FDabParam> Param = MakeUnique<FDabParam>();
				Param->Name = Pin.Name;
				Param->TypeName = GetDcrTypeName(Pin.Type);
				if (Param->TypeName.IsEmpty())
				{
					AddUnsupported(Location + TEXT(".") + Pin.Name, FString::Printf(TEXT("the type %s (%s)"), *Pin.Type.CPPType, *Pin.Type.ObjectPath));
					Param->TypeName = Pin.Type.CPPType;
				}
				if (bWithDefault && !Pin.Default.IsEmpty())
				{
					Param->Default = MakeDcrValueExprFromText(Pin.Type, Pin.Default);
				}
				return Param;
			};
			for (const FDcrModelFunctionPin& Input : Function.Inputs)
			{
				Stmt->Params.Add(MakeParam(Input, true));
			}
			Stmt->bHasOutputs = !Function.Outputs.IsEmpty();
			for (const FDcrModelFunctionPin& Output : Function.Outputs)
			{
				Stmt->Outputs.Add(MakeParam(Output, false));
			}

			TUniquePtr<FDabMeta> Meta = MakeUnique<FDabMeta>();
			if (bInline)
			{
				Meta->Entries.Add(MakeMetaEntry(TEXT("Inline"), nullptr));
			}
			if (Function.bPublic)
			{
				Meta->Entries.Add(MakeMetaEntry(TEXT("Public"), nullptr));
			}
			if (!Function.Category.IsEmpty())
			{
				Meta->Entries.Add(MakeMetaEntry(TEXT("Category"), MakeDcrString(Function.Category)));
			}
			if (!Function.Keywords.IsEmpty())
			{
				Meta->Entries.Add(MakeMetaEntry(TEXT("Keywords"), MakeDcrString(Function.Keywords)));
			}
			if (!Function.Description.IsEmpty())
			{
				Meta->Entries.Add(MakeMetaEntry(TEXT("Description"), MakeDcrString(Function.Description)));
			}
			if (!Meta->Entries.IsEmpty())
			{
				Stmt->Meta = MoveTemp(Meta);
			}

			FGraphIndex Index;
			Index.Build(Graph);
			const FDcrModelNode* Entry = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Entry; });
			const FDcrModelNode* Return = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Return; });

			// `local Type Name = value;` for each of the function's locals, first in its code.
			auto WriteLocals = [this, &Function, &Location](TArray<FDabStmtPtr>& Out)
			{
				for (const FDcrModelVariable& Local : Function.Locals)
				{
					TUniquePtr<FDabLetStmt> Let = MakeUnique<FDabLetStmt>();
					Let->LetKind = EDabLetKind::Local;
					Let->Name = Local.Name;
					Let->TypeName = GetDcrTypeName(Local.Type);
					if (Let->TypeName.IsEmpty())
					{
						AddUnsupported(Location + TEXT("/") + Local.Name, FString::Printf(TEXT("the type %s (%s)"), *Local.Type.CPPType, *Local.Type.ObjectPath));
						Let->TypeName = Local.Type.CPPType;
					}
					if (!Local.Default.IsEmpty())
					{
						Let->Value = MakeDcrValueExprFromText(Local.Type, Local.Default);
					}
					Out.Add(MoveTemp(Let));
				}
			};

			// A pure function whose graph runs code from its entry is written as code too, marked pure.
			const FDcrModelNode* FirstRun = !Function.bMutable && Entry ? Index.Next(*Entry, GetContinuationPin(*Entry)) : nullptr;
			if (FirstRun && FirstRun->Kind != EDcrNodeKind::Return)
			{
				if (!Stmt->Meta)
				{
					Stmt->Meta = MakeUnique<FDabMeta>();
				}
				Stmt->Meta->Entries.Insert(MakeMetaEntry(TEXT("Pure"), nullptr), bInline ? 1 : 0);
			}
			else if (!Function.bMutable)
			{
				// One expression, or code when the outputs share a node or there are locals: `[ Pure ] { let ...; return (...); }`.
				if (Return)
				{
					FBodyLowering CodeBody(*this, Index, &Function, Location);
					if (TUniquePtr<FDabCodeBlock> Code = CodeBody.LowerPureCode(*Return))
					{
						TArray<const FDcrModelNode*> Comments = CommentsOf(Graph);
						CodeBody.PlaceComments(*Code, Comments);
						if (!Stmt->Meta)
						{
							Stmt->Meta = MakeUnique<FDabMeta>();
						}
						Stmt->Meta->Entries.Insert(MakeMetaEntry(TEXT("Pure"), nullptr), bInline ? 1 : 0);
						Stmt->Code = MakeUnique<FDabCodeBlock>();
						WriteLocals(Stmt->Code->Statements);
						Stmt->Code->Statements.Append(MoveTemp(Code->Statements));
						AddLeadingComments(*Stmt, Comments);
						return Stmt;
					}
				}
				FBodyLowering Body(*this, Index, &Function, Location);
				Stmt->Value = Return ? Body.LowerPureReturn(*Return) : MakeDcrUnsupported(TEXT("a pure function without a return node"));
				return Stmt;
			}

			Stmt->Code = MakeUnique<FDabCodeBlock>();
			WriteLocals(Stmt->Code->Statements);

			TArray<const FDcrModelNode*> Comments = CommentsOf(Graph);
			if (Entry)
			{
				// What no execute path from the entry reaches goes in the body's Detached block, where it can read the body's lets.
				const TSet<FString> Reached = ReachableStatements(Index, *Entry);
				TArray<const FDcrModelNode*> Heads;
				TArray<const FDcrModelNode*> Dangling;
				FindDetached(Index, Reached, Heads, Dangling);
				FBodyLowering Body(*this, Index, &Function, Location);
				TUniquePtr<FDabCodeBlock> Code = Body.LowerChain(*Entry, GetContinuationPin(*Entry), Heads, Dangling);
				Body.PlaceComments(*Code, Comments);
				Stmt->Code->Statements.Append(MoveTemp(Code->Statements));
			}
			AddLeadingComments(*Stmt, Comments);
			return Stmt;
		}

		TSet<FString> FFileLowering::ReachableStatements(const FGraphIndex& Index, const FDcrModelNode& Start)
		{
			TSet<FString> Reached;
			TArray<const FDcrModelNode*> Stack;
			Stack.Add(&Start);
			while (!Stack.IsEmpty())
			{
				const FDcrModelNode* Node = Stack.Pop();
				for (const FString& Pin : Node->ExecuteOut)
				{
					const FDcrModelNode* Next = Index.Next(*Node, Pin);
					if (Next && !Reached.Contains(Next->Name))
					{
						Reached.Add(Next->Name);
						Stack.Add(Next);
					}
				}
			}
			return Reached;
		}

		void FFileLowering::FindDetached(const FGraphIndex& Index, const TSet<FString>& Reached, TArray<const FDcrModelNode*>& OutHeads,
			TArray<const FDcrModelNode*>& OutDangling)
		{
			for (const FDcrModelNode& Node : Index.Graph->Nodes)
			{
				if (Reached.Contains(Node.Name))
				{
					continue;
				}
				const ERole Role = GetRole(Node);
				if (Role == ERole::Statement && Index.ExecInCount.FindRef(Node.Name) == 0)
				{
					OutHeads.Add(&Node);
				}
				else if (Role == ERole::Data && !Index.DataTargets.Contains(Node.Name) && Node.Kind != EDcrNodeKind::VariableGet)
				{
					OutDangling.Add(&Node);
				}
			}
			OrderDetachedHeads(Index, OutHeads);
		}

		void FFileLowering::OrderDetachedHeads(const FGraphIndex& Index, TArray<const FDcrModelNode*>& Heads)
		{
			const int32 Num = Heads.Num();
			if (Num < 2)
			{
				return;
			}
			TArray<TSet<FString>> Chains;
			TArray<TSet<FString>> Reads;
			for (const FDcrModelNode* Head : Heads)
			{
				Chains.Add(ReachableStatements(Index, *Head));
				TSet<FString> Read;
				TSet<FString> Seen;
				for (const FString& Name : Chains.Last())
				{
					if (const FDcrModelNode* Node = Index.Find(Name))
					{
						CollectStatementDeps(Index, *Node, Read, Seen);
					}
				}
				Reads.Add(MoveTemp(Read));
			}
			auto ReadsFrom = [&Chains, &Reads](int32 Reader, int32 Maker)
			{
				for (const FString& Name : Reads[Reader])
				{
					if (Chains[Maker].Contains(Name))
					{
						return true;
					}
				}
				return false;
			};
			TArray<const FDcrModelNode*> Ordered;
			TArray<bool> Placed;
			Placed.Init(false, Num);
			while (Ordered.Num() < Num)
			{
				int32 Next = INDEX_NONE;
				for (int32 Candidate = 0; Candidate < Num && Next == INDEX_NONE; ++Candidate)
				{
					bool bReady = !Placed[Candidate];
					for (int32 Other = 0; Other < Num && bReady; ++Other)
					{
						bReady = Other == Candidate || Placed[Other] || !ReadsFrom(Candidate, Other);
					}
					Next = bReady ? Candidate : INDEX_NONE;
				}
				// Chains that read each other: the graph's order.
				for (int32 Candidate = 0; Candidate < Num && Next == INDEX_NONE; ++Candidate)
				{
					Next = Placed[Candidate] ? INDEX_NONE : Candidate;
				}
				Placed[Next] = true;
				Ordered.Add(Heads[Next]);
			}
			Heads = MoveTemp(Ordered);
		}

		void FFileLowering::CollectStatementDeps(const FGraphIndex& Index, const FDcrModelNode& Node, TSet<FString>& OutStatements, TSet<FString>& Seen)
		{
			auto Visit = [&](const FDcrModelNode& Source)
			{
				if (Seen.Contains(Source.Name))
				{
					return;
				}
				Seen.Add(Source.Name);
				const ERole Role = GetRole(Source);
				if (Role == ERole::Statement)
				{
					OutStatements.Add(Source.Name);
				}
				else if (Role == ERole::Data || Role == ERole::Injected)
				{
					CollectStatementDeps(Index, Source, OutStatements, Seen);
				}
			};
			for (const TPair<FString, FString>& Link : Index.InputsOf(Node.Name))
			{
				if (const FDcrModelNode* Source = Index.Find(SplitPinPath(Link.Value).Node))
				{
					Visit(*Source);
				}
			}
			const FString Prefix = Node.Name + TEXT(".");
			for (const TPair<FString, const FDcrModelNode*>& Injected : Index.InjectedInputs)
			{
				if (Injected.Key.StartsWith(Prefix))
				{
					Visit(*Injected.Value);
				}
			}
		}

		void FFileLowering::LowerGraphEvents(const FDcrModelGraph& Graph, bool bDefaultGraph, TArray<FDabStmtPtr>& Out)
		{
			FGraphIndex Index;
			Index.Build(Graph);
			TSet<FString> Visited;
			TSet<FString> Words;
			TArray<const FDcrModelNode*> Comments = CommentsOf(Graph);
			const int32 FirstOut = Out.Num();

			auto GraphMeta = [&Graph, bDefaultGraph]() -> TUniquePtr<FDabMeta>
			{
				if (bDefaultGraph)
				{
					return nullptr;
				}
				TUniquePtr<FDabMeta> Meta = MakeUnique<FDabMeta>();
				Meta->Entries.Add(MakeMetaEntry(TEXT("Graph"), MakeDcrString(Graph.Name)));
				return Meta;
			};

			// Which statements each event runs; a detached chain or loose data that reads one of them is written
			// inside that event, where its lets are in scope.
			TArray<const FDcrModelNode*> Events;
			TArray<TSet<FString>> EventReach;
			TSet<FString> AllReached;
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				if (GetRole(Node) == ERole::Event)
				{
					Events.Add(&Node);
					EventReach.Add(ReachableStatements(Index, Node));
					AllReached.Append(EventReach.Last());
					AllReached.Add(Node.Name);
				}
			}
			TArray<const FDcrModelNode*> AllHeads;
			TArray<const FDcrModelNode*> AllDangling;
			FindDetached(Index, AllReached, AllHeads, AllDangling);
			TArray<TArray<const FDcrModelNode*>> EventHeads;
			TArray<TArray<const FDcrModelNode*>> EventDangling;
			EventHeads.SetNum(Events.Num());
			EventDangling.SetNum(Events.Num());
			TArray<const FDcrModelNode*> TopHeads;
			TArray<const FDcrModelNode*> TopDangling;
			// The data nodes each event's statements read (getters aside: each use of a variable is a getter of its own).
			auto CollectData = [&Index](const FDcrModelNode& From, TSet<FString>& Out)
			{
				TArray<const FDcrModelNode*> Stack;
				Stack.Add(&From);
				while (!Stack.IsEmpty())
				{
					const FDcrModelNode* Current = Stack.Pop();
					for (const TPair<FString, FString>& Link : Index.InputsOf(Current->Name))
					{
						const FDcrModelNode* Source = Index.Find(SplitPinPath(Link.Value).Node);
						if (Source && GetRole(*Source) == ERole::Data && Source->Kind != EDcrNodeKind::VariableGet && !Out.Contains(Source->Name))
						{
							Out.Add(Source->Name);
							Stack.Add(Source);
						}
					}
				}
			};
			TArray<TSet<FString>> EventData;
			for (int32 Event = 0; Event < Events.Num(); ++Event)
			{
				TSet<FString>& Data = EventData.AddDefaulted_GetRef();
				for (const FString& Name : EventReach[Event])
				{
					if (const FDcrModelNode* Node = Index.Find(Name))
					{
						CollectData(*Node, Data);
					}
				}
			}
			// The event whose statements it reads, or whose data nodes it shares (written once, where both see it).
			auto HomeOf = [&](const TArray<const FDcrModelNode*>& Nodes) -> int32
			{
				TSet<FString> Deps;
				TSet<FString> Seen;
				TSet<FString> Data;
				for (const FDcrModelNode* Node : Nodes)
				{
					CollectStatementDeps(Index, *Node, Deps, Seen);
					CollectData(*Node, Data);
					if (GetRole(*Node) == ERole::Data)
					{
						Data.Add(Node->Name);
					}
				}
				for (int32 Event = 0; Event < Events.Num(); ++Event)
				{
					for (const FString& Dep : Deps)
					{
						if (EventReach[Event].Contains(Dep))
						{
							return Event;
						}
					}
				}
				for (int32 Event = 0; Event < Events.Num(); ++Event)
				{
					for (const FString& Name : Data)
					{
						if (EventData[Event].Contains(Name))
						{
							return Event;
						}
					}
				}
				return INDEX_NONE;
			};
			for (const FDcrModelNode* Head : AllHeads)
			{
				TArray<const FDcrModelNode*> Chain;
				Chain.Add(Head);
				for (const FString& Name : ReachableStatements(Index, *Head))
				{
					if (const FDcrModelNode* Node = Index.Find(Name))
					{
						Chain.Add(Node);
					}
				}
				const int32 Home = HomeOf(Chain);
				(Home == INDEX_NONE ? TopHeads : EventHeads[Home]).Add(Head);
			}
			for (const FDcrModelNode* Node : AllDangling)
			{
				const int32 Home = HomeOf({ Node });
				(Home == INDEX_NONE ? TopDangling : EventDangling[Home]).Add(Node);
			}

			for (int32 EventIndex = 0; EventIndex < Events.Num(); ++EventIndex)
			{
				const FDcrModelNode& Node = *Events[EventIndex];
				FString Word;
				if (const FDcrEventWord* Event = FindDcrEventByStruct(Node.StructPath))
				{
					Word = Event->Word;
				}
				else
				{
					FDcrValueSlot Slot;
					Slot.Init(MakeDcrBuiltinType(TEXT("FName")));
					const FDcrValue* EventName = FindValue(Node, DcrUserEventNamePin);
					Word = TEXT("My Event");
					if (EventName && Slot.ImportText(EventName->Value))
					{
						Word = static_cast<const FName*>(Slot.GetMemory())->ToString();
					}
					if (FindDcrEventByWord(Word))
					{
						AddUnsupported(Graph.Name + TEXT("/") + Node.Name, FString::Printf(TEXT("a user event named `%s`, like a built-in event"), *Word));
					}
				}
				if (Words.Contains(Word))
				{
					AddUnsupported(Graph.Name + TEXT("/") + Node.Name, FString::Printf(TEXT("a second `%s` event in one graph"), *Word));
				}
				Words.Add(Word);

				TUniquePtr<FDabNamedBlockStmt> Stmt = MakeUnique<FDabNamedBlockStmt>();
				Stmt->Word = TEXT("Event");
				Stmt->Name = Word;
				Stmt->Meta = GraphMeta();
				FBodyLowering Body(*this, Index, nullptr, Graph.Name + TEXT("/") + Word);
				Stmt->Code = Body.LowerChain(Node, GetContinuationPin(Node), EventHeads[EventIndex], EventDangling[EventIndex]);
				Body.PlaceComments(*Stmt->Code, Comments);
				Visited.Append(Body.Visited);
				Out.Add(MoveTemp(Stmt));
			}

			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				if (!Visited.Contains(Node.Name) && GetRole(Node) == ERole::Other)
				{
					AddUnsupported(Graph.Name + TEXT("/") + Node.Name, FString::Printf(TEXT("a %s node"), LexToString(Node.Kind)));
				}
			}
			const TArray<const FDcrModelNode*>& Heads = TopHeads;
			const TArray<const FDcrModelNode*>& Dangling = TopDangling;
			if (!Heads.IsEmpty() || !Dangling.IsEmpty())
			{
				TUniquePtr<FDabNamedBlockStmt> Stmt = MakeUnique<FDabNamedBlockStmt>();
				Stmt->Word = TEXT("Detached");
				Stmt->Meta = GraphMeta();
				FBodyLowering Body(*this, Index, nullptr, Graph.Name + TEXT("/Detached"));
				Stmt->Code = Body.LowerDetached(Heads, Dangling);
				Body.PlaceComments(*Stmt->Code, Comments);
				Visited.Append(Body.Visited);
				Out.Add(MoveTemp(Stmt));
			}

			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				const ERole Role = GetRole(Node);
				if (!Visited.Contains(Node.Name) && (Role == ERole::Statement || Role == ERole::Data))
				{
					AddUnsupported(Graph.Name + TEXT("/") + Node.Name, TEXT("no event and no detached chain reaches this node"));
				}
			}
			// Boxes around no statement: above the graph's first block, so their words are kept.
			if (Out.IsValidIndex(FirstOut))
			{
				AddLeadingComments(*Out[FirstOut], Comments);
			}
			else
			{
				for (const FDcrModelNode* Comment : Comments)
				{
					AddUnsupported(Graph.Name + TEXT("/") + Comment->Name, TEXT("a comment in a graph with nothing else to write"));
				}
			}
		}

		TArray<const FDcrModelNode*> FFileLowering::CommentsOf(const FDcrModelGraph& Graph)
		{
			TArray<const FDcrModelNode*> Comments;
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Kind == EDcrNodeKind::Comment)
				{
					Comments.Add(&Node);
				}
			}
			return Comments;
		}

		void FFileLowering::AddLeadingComments(const FDabStmt& Stmt, TConstArrayView<const FDcrModelNode*> Comments)
		{
			FDabTrivia& Trivia = File.Trivia.FindOrAdd(&Stmt);
			for (const FDcrModelNode* Comment : Comments)
			{
				AddDcrCommentTrivia(Trivia, Comment->CommentText);
			}
		}

		FDabExprPtr FFileLowering::LowerVariables()
		{
			if (Model.Variables.IsEmpty())
			{
				return nullptr;
			}
			TUniquePtr<FDabBlockExpr> Block = MakeUnique<FDabBlockExpr>();
			for (const FDcrModelVariable& Variable : Model.Variables)
			{
				TUniquePtr<FDabDeclStmt> Decl = MakeUnique<FDabDeclStmt>();
				Decl->Name = Variable.Name;
				Decl->TypeName = GetDcrTypeName(Variable.Type);
				if (Decl->TypeName.IsEmpty())
				{
					AddUnsupported(TEXT("Variables/") + Variable.Name, FString::Printf(TEXT("the type %s (%s)"), *Variable.Type.CPPType, *Variable.Type.ObjectPath));
					Decl->TypeName = Variable.Type.CPPType;
				}
				if (!Variable.Default.IsEmpty())
				{
					Decl->Value = MakeDcrValueExprFromText(Variable.Type, Variable.Default);
					if (!Decl->Value)
					{
						AddUnsupported(TEXT("Variables/") + Variable.Name, FString::Printf(TEXT("the default `%s` does not read back"), *Variable.Default));
					}
				}
				TUniquePtr<FDabMeta> Meta = MakeUnique<FDabMeta>();
				if (Variable.bPublic)
				{
					Meta->Entries.Add(MakeMetaEntry(TEXT("Public"), nullptr));
				}
				if (Variable.bReadOnly)
				{
					Meta->Entries.Add(MakeMetaEntry(TEXT("ReadOnly"), nullptr));
				}
				if (!Variable.Category.IsEmpty())
				{
					Meta->Entries.Add(MakeMetaEntry(TEXT("Category"), MakeDcrString(Variable.Category)));
				}
				if (!Variable.Tooltip.IsEmpty())
				{
					Meta->Entries.Add(MakeMetaEntry(TEXT("Tooltip"), MakeDcrString(Variable.Tooltip)));
				}
				if (!Meta->Entries.IsEmpty())
				{
					Decl->Meta = MoveTemp(Meta);
				}
				Block->Statements.Add(MoveTemp(Decl));
			}
			return Block;
		}

		FDabExprPtr FFileLowering::LowerSettings()
		{
			TUniquePtr<FDabBlockExpr> Block = MakeUnique<FDabBlockExpr>();
			for (const FDcrValue& Setting : Model.Settings)
			{
				if (Setting.Name == TEXT("PreviewSkeletalMesh"))
				{
					Block->Statements.Add(MakeAssign(TEXT("PreviewMesh"), MakeDcrAssetExpr(FSoftObjectPath(Setting.Value.TrimQuotes()))));
					continue;
				}
				if (Setting.Name == TEXT("ShapeLibraries"))
				{
					FDcrValueSlot Slot;
					const FDcrType Type = MakeDcrType(*TBaseStructure<FSoftObjectPath>::Get(), true);
					TArray<FDabExprPtr> Items;
					if (Slot.Init(MakeDcrBuiltinType(TEXT("FString"), true)) && Slot.ImportText(Setting.Value))
					{
						for (const FString& Path : *static_cast<const TArray<FString>*>(Slot.GetMemory()))
						{
							Items.Add(MakeDcrAssetExpr(FSoftObjectPath(Path)));
						}
					}
					else
					{
						AddUnsupported(TEXT("Settings/ShapeLibraries"), FString::Printf(TEXT("the value `%s`"), *Setting.Value));
					}
					Block->Statements.Add(MakeAssign(TEXT("ShapeLibraries"), MakeDcrList(MoveTemp(Items))));
					continue;
				}
				AddUnsupported(TEXT("Settings/") + Setting.Name, FString::Printf(TEXT("the setting `%s`"), *Setting.Value));
			}
			if (!UsedLibraries.IsEmpty())
			{
				TArray<FDabExprPtr> Items;
				for (const FString& Library : UsedLibraries)
				{
					Items.Add(MakeDcrAssetExpr(FSoftObjectPath(Library)));
				}
				Block->Statements.Add(MakeAssign(TEXT("Uses"), MakeDcrList(MoveTemp(Items))));
			}
			return Block->Statements.IsEmpty() ? nullptr : MoveTemp(Block);
		}

		FDabExprPtr FFileLowering::LowerModule()
		{
			if (!Model.bModule)
			{
				return nullptr;
			}
			TUniquePtr<FDabBlockExpr> Block = MakeUnique<FDabBlockExpr>();
			for (const FDcrValue& Setting : Model.ModuleSettings)
			{
				const FProperty* Property = FindDcrStructMember(*FRigModuleSettings::StaticStruct(), Setting.Name);
				FDabExprPtr Value = Property ? MakeDcrValueExprFromText(*Property, Setting.Value) : nullptr;
				if (!Value)
				{
					AddUnsupported(TEXT("Module/") + Setting.Name, FString::Printf(TEXT("the module setting `%s`"), *Setting.Value));
					continue;
				}
				Block->Statements.Add(MakeAssign(GetDcrMemberName(*Property), MoveTemp(Value)));
			}
			return Block;
		}

		FDabExprPtr FFileLowering::ElementValue(const FDcrModelElement& Element, const FDcrValue& Property)
		{
			// The type each key's export text is in.
			const FProperty* Member = nullptr;
			FDcrType Type;
			const FString& Key = Property.Name;
			if (Key == TEXT("Transform") || Key == TEXT("Offset") || Key == TEXT("ShapeTransform"))
			{
				Type = MakeDcrType(*TBaseStructure<FTransform>::Get());
			}
			else if (Element.Kind == EDcrElementKind::Bone && Key == TEXT("BoneType"))
			{
				Type = MakeDcrType(*StaticEnum<ERigBoneType>());
			}
			else if (Element.Kind == EDcrElementKind::Control && Key == TEXT("Initial"))
			{
				// Written as a value of the control's type, not as the float store it is kept in.
				ERigControlType ControlType = FRigControlSettings().ControlType;
				if (const FDcrValue* TypeProperty = Element.Properties.FindByPredicate([](const FDcrValue& Value) { return Value.Name == TEXT("ControlType"); }))
				{
					const int64 Found = StaticEnum<ERigControlType>()->GetValueByNameString(TypeProperty->Value);
					ControlType = Found == INDEX_NONE ? ControlType : ERigControlType(Found);
				}
				FDcrValueSlot Stored;
				if (Stored.Init(MakeDcrType(*FRigControlValue::StaticStruct())) && Stored.ImportText(Property.Value))
				{
					if (FDabExprPtr Value = MakeDcrControlValueExpr(ControlType, *static_cast<const FRigControlValue*>(Stored.GetMemory())))
					{
						return Value;
					}
				}
				Type = MakeDcrType(*FRigControlValue::StaticStruct());
			}
			else if (Element.Kind == EDcrElementKind::Control)
			{
				Member = FindDcrStructMember(*FRigControlSettings::StaticStruct(), Key);
			}
			else if (Element.Kind == EDcrElementKind::Connector)
			{
				Member = FindDcrStructMember(*FRigConnectorSettings::StaticStruct(), Key);
			}
			else if (Element.Kind == EDcrElementKind::Curve && Key == TEXT("Value"))
			{
				Type = MakeDcrBuiltinType(TEXT("float"));
			}
			else if (Element.Kind == EDcrElementKind::Socket && Key == TEXT("Color"))
			{
				Type = MakeDcrType(*TBaseStructure<FLinearColor>::Get());
			}
			else if (Element.Kind == EDcrElementKind::Socket && Key == TEXT("Description"))
			{
				Type = MakeDcrBuiltinType(TEXT("FString"));
			}
			FDabExprPtr Value = Member ? MakeDcrValueExprFromText(*Member, Property.Value)
				: Type.IsSet() ? MakeDcrValueExprFromText(Type, Property.Value) : nullptr;
			if (!Value)
			{
				AddUnsupported(FString::Printf(TEXT("Hierarchy/%s(%s).%s"), LexToString(Element.Kind), *Element.Name, *Key),
					FString::Printf(TEXT("the value `%s`"), *Property.Value));
				return MakeDcrUnsupported(Property.Value);
			}
			return Value;
		}

		FDabExprPtr FFileLowering::LowerHierarchy()
		{
			if (Model.Hierarchy.IsEmpty())
			{
				return nullptr;
			}
			TUniquePtr<FDabBlockExpr> Block = MakeUnique<FDabBlockExpr>();

			// `Import = Mesh;` when the rig holds what importing the preview mesh gives: exactly, or with the
			// properties of some elements refined (written after it). Not when most of them would need refining.
			TSet<int32> Imported;
			TMap<int32, const TArray<FDcrValue>*> Refined;
			FDcrHierarchyImportMatch Match;
			if (Options.bDetectImport)
			{
				const FDcrValue* Preview = Model.Settings.FindByPredicate([](const FDcrValue& Setting) { return Setting.Name == TEXT("PreviewSkeletalMesh"); });
				const FString MeshPath = Preview ? Preview->Value.TrimQuotes() : FString();
				if (!MeshPath.IsEmpty())
				{
					Match = MatchDcrHierarchyImport(MeshPath, Model.Hierarchy);
					if (!Match.IsEmpty() && Match.Refined.Num() <= Match.Exact.Num())
					{
						Block->Statements.Add(MakeAssign(TEXT("Import"), MakeDcrAssetExpr(FSoftObjectPath(MeshPath))));
						Imported.Append(Match.Exact);
						for (int32 Index = 0; Index < Match.Refined.Num(); ++Index)
						{
							Refined.Add(Match.Refined[Index], &Match.RefinedProperties[Index]);
						}
					}
				}
			}

			for (int32 ElementIndex = 0; ElementIndex < Model.Hierarchy.Num(); ++ElementIndex)
			{
				if (Imported.Contains(ElementIndex))
				{
					continue;
				}
				const FDcrModelElement& Element = Model.Hierarchy[ElementIndex];
				if (const TArray<FDcrValue>* const* Properties = Refined.Find(ElementIndex))
				{
					// An imported element whose properties the rig holds otherwise: only those, no parent.
					TUniquePtr<FDabNamedBlockStmt> Stmt = MakeUnique<FDabNamedBlockStmt>();
					Stmt->Word = LexToString(Element.Kind);
					Stmt->Name = Element.Name;
					Stmt->Body = MakeUnique<FDabBlockExpr>();
					for (const FDcrValue& Property : **Properties)
					{
						Stmt->Body->Statements.Add(MakeAssign(Property.Name, ElementValue(Element, Property)));
					}
					Block->Statements.Add(MoveTemp(Stmt));
					continue;
				}
				TUniquePtr<FDabNamedBlockStmt> Stmt = MakeUnique<FDabNamedBlockStmt>();
				Stmt->Word = LexToString(Element.Kind);
				Stmt->Name = Element.Name;
				TUniquePtr<FDabBlockExpr> Body = MakeUnique<FDabBlockExpr>();
				if (!Element.Parent.IsEmpty())
				{
					TArray<FDabExprPtr> Args;
					Args.Add(MakeDcrName(Element.Parent));
					Body->Statements.Add(MakeAssign(TEXT("Parent"), MakeDcrCall(LexToString(Element.ParentKind), MoveTemp(Args))));
				}
				for (const FDcrValue& Property : Element.Properties)
				{
					// A bone the text adds is a user bone unless it says otherwise.
					if (Element.Kind == EDcrElementKind::Bone && Property.Name == TEXT("BoneType") && Property.Value == TEXT("User"))
					{
						continue;
					}
					Body->Statements.Add(MakeAssign(Property.Name, ElementValue(Element, Property)));
				}
				if (!Body->Statements.IsEmpty())
				{
					Stmt->Body = MoveTemp(Body);
				}
				Block->Statements.Add(MoveTemp(Stmt));
			}
			return Block;
		}

		TUniquePtr<FDabDocument> FFileLowering::LowerDocument()
		{
			TUniquePtr<FDabDocument> Document = MakeUnique<FDabDocument>();
			Document->DocumentKind = EDabDocumentKind::ControlRig;
			Document->KindName = TEXT("ControlRig");
			FString Rel;
			const FString RootName = SplitDcrObjectPath(Model.AssetPath, Rel);
			for (const TPair<const TCHAR*, FString>& Header : { TPair<const TCHAR*, FString>(TEXT("Name"), Rel), TPair<const TCHAR*, FString>(TEXT("Root"), RootName) })
			{
				FDabArgumentPtr Argument = MakeUnique<FDabArgument>();
				Argument->Name = Header.Key;
				Argument->Value = MakeDcrString(Header.Value);
				Document->HeaderArgs.Add(MoveTemp(Argument));
			}
			Document->Body = MakeUnique<FDabBlockExpr>();

			// Code first: lowering it finds the libraries the settings list.
			TArray<FDabStmtPtr> Code;
			for (const FDcrModelFunction& Function : Model.Functions)
			{
				Code.Add(LowerFunction(Function, Function.Graph, false));
			}
			for (int32 Index = 0; Index < Model.Graphs.Num(); ++Index)
			{
				LowerGraphEvents(Model.Graphs[Index], Model.Graphs[Index].Name == TEXT("RigVMModel"), Code);
			}
			// Collapse nodes found along the way, each an inline function; lowering one can find more.
			TArray<FDabStmtPtr> InlineFunctions;
			for (int32 Index = 0; Index < InlineOrder.Num(); ++Index)
			{
				const FDcrModelNode& Collapse = *InlineOrder[Index];
				if (const FDcrModelFunction* Inline = GetInline(Collapse))
				{
					InlineFunctions.Add(LowerFunction(*Inline, *Collapse.Contained, true));
				}
			}

			TArray<FDabStmtPtr>& Body = Document->Body->Statements;
			if (FDabExprPtr Settings = LowerSettings())
			{
				Body.Add(MakeAssign(TEXT("Settings"), MoveTemp(Settings)));
			}
			if (FDabExprPtr Module = LowerModule())
			{
				Body.Add(MakeAssign(TEXT("Module"), MoveTemp(Module)));
			}
			if (FDabExprPtr Hierarchy = LowerHierarchy())
			{
				Body.Add(MakeAssign(TEXT("Hierarchy"), MoveTemp(Hierarchy)));
			}
			if (FDabExprPtr Variables = LowerVariables())
			{
				Body.Add(MakeAssign(TEXT("Variables"), MoveTemp(Variables)));
			}
			Body.Append(MoveTemp(Code));
			Body.Append(MoveTemp(InlineFunctions));
			for (int32 Index = 1; Index < Body.Num(); ++Index)
			{
				File.Trivia.FindOrAdd(Body[Index].Get()).BlankLinesBefore = 1;
			}
			return Document;
		}
	}

	FDcrLowerResult LowerDcrModel(const FDcrModel& Model, const FDcrLowerOptions& Options)
	{
		FDcrLowerResult Result;
		Result.File = MakeUnique<FDabFile>();
		Result.Unsupported = Model.Unsupported;
		LowerPrivate::FFileLowering File(Model, Options, *Result.File, Result.Unsupported);
		Result.File->Document = File.LowerDocument();

		// What the text cannot say is listed at the top, so it is seen where it was lost.
		if (!Result.Unsupported.IsEmpty())
		{
			FDabTrivia& Trivia = Result.File->Trivia.FindOrAdd(Result.File->Document.Get());
			FDabComment Header;
			Header.Text = TEXT("// Not expressible in the text; kept out of it:");
			Trivia.Leading.Add(Header);
			for (const FDcrModelUnsupported& Unsupported : Result.Unsupported)
			{
				FDabComment Line;
				Line.Text = FString::Printf(TEXT("//   %s: %s"), *Unsupported.Location, *Unsupported.Description.Replace(TEXT("\n"), TEXT(" ")));
				Trivia.Leading.Add(Line);
			}
		}
		return Result;
	}
}
