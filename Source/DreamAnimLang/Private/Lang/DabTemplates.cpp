// Copyright DreamMoon. All Rights Reserved.

#include "Lang/DabTemplates.h"

#include "HAL/FileManager.h"
#include "Lang/DabParser.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamAnim::Lang
{
	namespace
	{
		// A template that (directly or not) calls itself would otherwise expand forever.
		constexpr int32 MaxExpansionDepth = 16;

		const TCHAR* TemplateWord = TEXT("Template");

		struct FTemplateDef
		{
			const FDabNamedBlockStmt* Stmt = nullptr;
			FString File;
		};

		// -- includes ---------------------------------------------------------------------------

		struct FIncludeWalker
		{
			FDabCompilationUnit& Unit;
			IDabIncludeLoader& Loader;
			FDabDiagnosticSink& Sink;
			TSet<FString> Parsed;
			TArray<FString> Stack;

			void Walk(const FDabFile& File)
			{
				for (const TUniquePtr<FDabInclude>& Include : File.Includes)
				{
					FString Resolved;
					FString Text;
					if (!Loader.Load(File.Path, Include->Path, Resolved, Text))
					{
						Sink.ErrorIn(TEXT("DAB3201"), File.Path, Include->PathSpan,
							FString::Printf(TEXT("Cannot find the included file '%s'."), *Include->Path),
							TEXT("Paths are relative to the including file, then to each DAnim/ root."));
						continue;
					}
					if (Stack.Contains(Resolved))
					{
						Sink.ErrorIn(TEXT("DAB3202"), File.Path, Include->PathSpan,
							FString::Printf(TEXT("'%s' includes itself through a cycle: %s -> %s."),
								*Include->Path, *FString::Join(Stack, TEXT(" -> ")), *Resolved));
						continue;
					}
					if (Parsed.Contains(Resolved))
					{
						continue;
					}
					Parsed.Add(Resolved);

					FDabSourceText Source(Resolved, MoveTemp(Text));
					FDabDiagnosticSink LibrarySink(Resolved);
					TUniquePtr<FDabFile> Library = ParseDab(Source, LibrarySink);
					Sink.Append(MoveTemp(LibrarySink));

					if (!Library->Document || Library->Document->DocumentKind != EDabDocumentKind::AnimTemplates)
					{
						Sink.ErrorIn(TEXT("DAB3203"), File.Path, Include->PathSpan,
							FString::Printf(TEXT("'%s' is not a template library: an included file must hold an AnimTemplates(...) document."), *Include->Path));
						continue;
					}

					const FDabFile& Added = *Library;
					Unit.LibrarySources.Add(MoveTemp(Source));
					Unit.Libraries.Add(MoveTemp(Library));
					Stack.Push(Resolved);
					Walk(Added);
					Stack.Pop();
				}
			}
		};

		// -- expansion --------------------------------------------------------------------------

		// Every span a node carries, set to Span. Applied to everything cloned from a template body,
		// so diagnostics about expanded code point at the call the author wrote.
		void RetargetSpans(FDabNode& Node, const FDabSpan& Span)
		{
			Node.Span = Span;
			switch (Node.Kind)
			{
			case EDabNodeKind::MemberExpr: static_cast<FDabMemberExpr&>(Node).MemberSpan = Span; break;
			case EDabNodeKind::CallExpr: static_cast<FDabCallExpr&>(Node).ArgsSpan = Span; break;
			case EDabNodeKind::BinaryExpr: static_cast<FDabBinaryExpr&>(Node).OpSpan = Span; break;
			case EDabNodeKind::Argument: static_cast<FDabArgument&>(Node).NameSpan = Span; break;
			case EDabNodeKind::MetaEntry: static_cast<FDabMetaEntry&>(Node).KeySpan = Span; break;
			case EDabNodeKind::AssignStmt: static_cast<FDabAssignStmt&>(Node).NameSpan = Span; break;
			case EDabNodeKind::DeclStmt:
				static_cast<FDabDeclStmt&>(Node).TypeSpan = Span;
				static_cast<FDabDeclStmt&>(Node).NameSpan = Span;
				break;
			case EDabNodeKind::TransitionStmt:
				static_cast<FDabTransitionStmt&>(Node).FromSpan = Span;
				static_cast<FDabTransitionStmt&>(Node).ToSpan = Span;
				break;
			case EDabNodeKind::NamedBlockStmt:
				static_cast<FDabNamedBlockStmt&>(Node).WordSpan = Span;
				static_cast<FDabNamedBlockStmt&>(Node).NameSpan = Span;
				break;
			case EDabNodeKind::Param:
				static_cast<FDabParam&>(Node).TypeSpan = Span;
				static_cast<FDabParam&>(Node).NameSpan = Span;
				break;
			case EDabNodeKind::LetStmt:
				static_cast<FDabLetStmt&>(Node).TypeSpan = Span;
				static_cast<FDabLetStmt&>(Node).NameSpan = Span;
				for (FDabFlowBlock& Block : static_cast<FDabLetStmt&>(Node).Blocks)
				{
					Block.PinSpan = Span;
				}
				break;
			case EDabNodeKind::ExprStmt:
				for (FDabFlowBlock& Block : static_cast<FDabExprStmt&>(Node).Blocks)
				{
					Block.PinSpan = Span;
				}
				break;
			case EDabNodeKind::ForStmt:
				for (FDabSpan& NameSpan : static_cast<FDabForStmt&>(Node).NameSpans)
				{
					NameSpan = Span;
				}
				break;
			case EDabNodeKind::IfStmt: static_cast<FDabIfStmt&>(Node).ElseSpan = Span; break;
			default: break;
			}
			ForEachChild(Node, [&Span](const FDabNode& Child)
			{
				// ForEachChild hands out const references to nodes this function's caller owns and is
				// allowed to change; the expansion works on a private clone.
				RetargetSpans(const_cast<FDabNode&>(Child), Span);
			});
		}

		// Calls Visit on every expression slot a node owns directly, so a slot can be replaced.
		void ForEachExprSlot(FDabNode& Node, TFunctionRef<void(FDabExprPtr&)> Visit);

		void ForEachExprSlotInStmt(FDabStmt& Stmt, TFunctionRef<void(FDabExprPtr&)> Visit)
		{
			ForEachExprSlot(Stmt, Visit);
		}

		void ForEachExprSlotInMeta(FDabMeta* Meta, TFunctionRef<void(FDabExprPtr&)> Visit)
		{
			if (!Meta)
			{
				return;
			}
			for (TUniquePtr<FDabMetaEntry>& Entry : Meta->Entries)
			{
				if (Entry->Value)
				{
					Visit(Entry->Value);
				}
			}
		}

		void ForEachExprSlot(FDabNode& Node, TFunctionRef<void(FDabExprPtr&)> Visit)
		{
			switch (Node.Kind)
			{
			case EDabNodeKind::MemberExpr: Visit(static_cast<FDabMemberExpr&>(Node).Object); break;
			case EDabNodeKind::CallExpr:
			{
				FDabCallExpr& Call = static_cast<FDabCallExpr&>(Node);
				Visit(Call.Callee);
				for (FDabArgumentPtr& Arg : Call.Args)
				{
					Visit(Arg->Value);
				}
				break;
			}
			case EDabNodeKind::ListExpr:
				for (FDabExprPtr& Item : static_cast<FDabListExpr&>(Node).Items)
				{
					Visit(Item);
				}
				break;
			case EDabNodeKind::TupleExpr:
				for (FDabArgumentPtr& Item : static_cast<FDabTupleExpr&>(Node).Items)
				{
					Visit(Item->Value);
				}
				break;
			case EDabNodeKind::BlockExpr:
				for (FDabStmtPtr& Stmt : static_cast<FDabBlockExpr&>(Node).Statements)
				{
					ForEachExprSlotInStmt(*Stmt, Visit);
				}
				break;
			case EDabNodeKind::StackExpr:
				for (FDabExprPtr& Step : static_cast<FDabStackExpr&>(Node).Steps)
				{
					Visit(Step);
				}
				break;
			case EDabNodeKind::UnaryExpr: Visit(static_cast<FDabUnaryExpr&>(Node).Operand); break;
			case EDabNodeKind::BinaryExpr:
				Visit(static_cast<FDabBinaryExpr&>(Node).Lhs);
				Visit(static_cast<FDabBinaryExpr&>(Node).Rhs);
				break;
			case EDabNodeKind::ConditionalExpr:
				Visit(static_cast<FDabConditionalExpr&>(Node).Condition);
				Visit(static_cast<FDabConditionalExpr&>(Node).Then);
				Visit(static_cast<FDabConditionalExpr&>(Node).Else);
				break;
			case EDabNodeKind::ParenExpr: Visit(static_cast<FDabParenExpr&>(Node).Inner); break;
			case EDabNodeKind::IndexExpr:
				Visit(static_cast<FDabIndexExpr&>(Node).Object);
				Visit(static_cast<FDabIndexExpr&>(Node).Index);
				break;
			case EDabNodeKind::AssignStmt:
				Visit(static_cast<FDabAssignStmt&>(Node).Value);
				ForEachExprSlotInMeta(static_cast<FDabAssignStmt&>(Node).Meta.Get(), Visit);
				break;
			case EDabNodeKind::DeclStmt:
				if (static_cast<FDabDeclStmt&>(Node).Value)
				{
					Visit(static_cast<FDabDeclStmt&>(Node).Value);
				}
				ForEachExprSlotInMeta(static_cast<FDabDeclStmt&>(Node).Meta.Get(), Visit);
				break;
			case EDabNodeKind::TransitionStmt:
				Visit(static_cast<FDabTransitionStmt&>(Node).Condition);
				ForEachExprSlotInMeta(static_cast<FDabTransitionStmt&>(Node).Meta.Get(), Visit);
				break;
			case EDabNodeKind::NamedBlockStmt:
			{
				FDabNamedBlockStmt& Block = static_cast<FDabNamedBlockStmt&>(Node);
				ForEachExprSlotInMeta(Block.Meta.Get(), Visit);
				if (Block.Body)
				{
					ForEachExprSlot(*Block.Body, Visit);
				}
				if (Block.Code)
				{
					ForEachExprSlot(*Block.Code, Visit);
				}
				if (Block.Value)
				{
					Visit(Block.Value);
				}
				break;
			}
			case EDabNodeKind::CodeBlock:
				for (FDabStmtPtr& Stmt : static_cast<FDabCodeBlock&>(Node).Statements)
				{
					ForEachExprSlotInStmt(*Stmt, Visit);
				}
				break;
			case EDabNodeKind::LetStmt:
				if (static_cast<FDabLetStmt&>(Node).Value)
				{
					Visit(static_cast<FDabLetStmt&>(Node).Value);
				}
				for (FDabFlowBlock& Block : static_cast<FDabLetStmt&>(Node).Blocks)
				{
					if (Block.Body)
					{
						ForEachExprSlot(*Block.Body, Visit);
					}
				}
				break;
			case EDabNodeKind::SetStmt:
				Visit(static_cast<FDabSetStmt&>(Node).Target);
				Visit(static_cast<FDabSetStmt&>(Node).Value);
				break;
			case EDabNodeKind::ExprStmt:
				Visit(static_cast<FDabExprStmt&>(Node).Expr);
				for (FDabFlowBlock& Block : static_cast<FDabExprStmt&>(Node).Blocks)
				{
					if (Block.Body)
					{
						ForEachExprSlot(*Block.Body, Visit);
					}
				}
				break;
			case EDabNodeKind::IfStmt:
			{
				FDabIfStmt& If = static_cast<FDabIfStmt&>(Node);
				Visit(If.Condition);
				if (If.Then)
				{
					ForEachExprSlot(*If.Then, Visit);
				}
				if (If.ElseIf)
				{
					ForEachExprSlot(*If.ElseIf, Visit);
				}
				if (If.Else)
				{
					ForEachExprSlot(*If.Else, Visit);
				}
				break;
			}
			case EDabNodeKind::ForStmt:
			{
				FDabForStmt& For = static_cast<FDabForStmt&>(Node);
				Visit(For.Iterable);
				if (For.Body)
				{
					ForEachExprSlot(*For.Body, Visit);
				}
				break;
			}
			case EDabNodeKind::ReturnStmt:
				if (static_cast<FDabReturnStmt&>(Node).Value)
				{
					Visit(static_cast<FDabReturnStmt&>(Node).Value);
				}
				break;
			case EDabNodeKind::DetachedStmt:
				if (static_cast<FDabDetachedStmt&>(Node).Body)
				{
					ForEachExprSlot(*static_cast<FDabDetachedStmt&>(Node).Body, Visit);
				}
				break;
			default:
				break;
			}
		}

		struct FExpander
		{
			FDabFile& Main;
			FDabDiagnosticSink& Sink;
			TMap<FString, FTemplateDef> Templates;

			const FTemplateDef* FindTemplateCall(const FDabExpr& Expr, const FDabCallExpr*& OutCall) const
			{
				const FDabCallExpr* Call = Expr.As<FDabCallExpr>();
				if (!Call || !Call->Callee)
				{
					return nullptr;
				}
				const FDabNameExpr* Callee = Call->Callee->As<FDabNameExpr>();
				if (!Callee)
				{
					return nullptr;
				}
				OutCall = Call;
				return Templates.Find(Callee->Name);
			}

			// Replaces a slot's node, dropping side-table entries that would otherwise point at freed memory.
			void ReplaceSlot(FDabExprPtr& Slot, FDabExprPtr&& NewExpr)
			{
				Main.Origins.Remove(Slot.Get());
				Main.Trivia.Remove(Slot.Get());
				Slot = MoveTemp(NewExpr);
			}

			// Binds the call's arguments to the template's parameters. False (with a diagnostic) when they do not fit.
			bool Bind(const FDabCallExpr& Call, const FDabNamedBlockStmt& Def, TArray<const FDabExpr*>& OutValues)
			{
				const int32 NumParams = Def.Params.Num();
				OutValues.Init(nullptr, NumParams);
				bool bOk = true;

				int32 Positional = 0;
				for (const FDabArgumentPtr& Arg : Call.Args)
				{
					if (!Arg->IsNamed())
					{
						if (Positional >= NumParams)
						{
							Sink.Error(TEXT("DAB3103"), Arg->Span,
								FString::Printf(TEXT("Template '%s' takes %d argument(s); this is one too many."), *Def.Name, NumParams));
							bOk = false;
						}
						else
						{
							OutValues[Positional] = Arg->Value.Get();
						}
						++Positional;
						continue;
					}

					const int32 Index = Def.Params.IndexOfByPredicate([&Arg](const TUniquePtr<FDabParam>& Param)
					{
						return Param->Name.Equals(Arg->Name, ESearchCase::CaseSensitive);
					});
					if (Index == INDEX_NONE)
					{
						Sink.Error(TEXT("DAB3102"), Arg->NameSpan,
							FString::Printf(TEXT("Template '%s' has no parameter '%s'."), *Def.Name, *Arg->Name));
						bOk = false;
					}
					else if (OutValues[Index])
					{
						Sink.Error(TEXT("DAB3102"), Arg->NameSpan,
							FString::Printf(TEXT("Parameter '%s' of template '%s' is given twice."), *Arg->Name, *Def.Name));
						bOk = false;
					}
					else
					{
						OutValues[Index] = Arg->Value.Get();
					}
				}

				for (int32 Index = 0; Index < NumParams; ++Index)
				{
					if (!OutValues[Index])
					{
						if (Def.Params[Index]->Default)
						{
							OutValues[Index] = Def.Params[Index]->Default.Get();
						}
						else
						{
							Sink.Error(TEXT("DAB3101"), Call.Span,
								FString::Printf(TEXT("Template '%s' needs an argument for '%s'."), *Def.Name, *Def.Params[Index]->Name));
							bOk = false;
						}
					}
				}
				return bOk;
			}

			// Clones the template body with every parameter name replaced by a clone of its value.
			FDabExprPtr Instantiate(const FDabNamedBlockStmt& Def, const TArray<const FDabExpr*>& Values, const FDabSpan& CallSpan)
			{
				FDabExprPtr Result = CloneExpr(*Def.Value);
				RetargetSpans(*Result, CallSpan);

				struct FBinding
				{
					const FDabExpr* Value = nullptr;
					bool bFromDefault = false;
				};
				TMap<FString, FBinding> Bindings;
				for (int32 Index = 0; Index < Def.Params.Num(); ++Index)
				{
					const bool bFromDefault = Values[Index] == Def.Params[Index]->Default.Get();
					Bindings.Add(Def.Params[Index]->Name, FBinding{ Values[Index], bFromDefault });
				}

				TFunction<void(FDabExprPtr&)> Substitute = [&Bindings, &Substitute, &CallSpan](FDabExprPtr& Slot)
				{
					if (const FDabNameExpr* Name = Slot->As<FDabNameExpr>())
					{
						if (const FBinding* Binding = Bindings.Find(Name->Name))
						{
							FDabExprPtr Copy = CloneExpr(*Binding->Value);
							// A default comes from the definition, not from the call: give it the call's position too.
							if (Binding->bFromDefault)
							{
								RetargetSpans(*Copy, CallSpan);
							}
							Slot = MoveTemp(Copy);
							return;
						}
					}
					ForEachExprSlot(*Slot, Substitute);
				};
				Substitute(Result);
				return Result;
			}

			void ExpandSlot(FDabExprPtr& Slot, int32 Depth)
			{
				if (!Slot)
				{
					return;
				}

				const FDabCallExpr* Call = nullptr;
				const FTemplateDef* Def = FindTemplateCall(*Slot, Call);
				if (!Def)
				{
					ForEachExprSlot(*Slot, [this, Depth](FDabExprPtr& Child) { ExpandSlot(Child, Depth); });
					return;
				}

				if (Depth >= MaxExpansionDepth)
				{
					Sink.Error(TEXT("DAB3104"), Slot->Span,
						FString::Printf(TEXT("Template '%s' expands more than %d levels deep; does it call itself?"), *Def->Stmt->Name, MaxExpansionDepth));
					return;
				}

				// Arguments first, so each is expanded once rather than once per use in the body.
				ForEachExprSlot(*Slot, [this, Depth](FDabExprPtr& Child) { ExpandSlot(Child, Depth); });

				TArray<const FDabExpr*> Values;
				if (!Bind(*Call, *Def->Stmt, Values))
				{
					return;
				}

				FDabExpansionOrigin Origin;
				Origin.TemplateName = Def->Stmt->Name;
				Origin.CallSpan = Slot->Span;
				Origin.DefinitionSpan = Def->Stmt->Span;
				Origin.DefinitionFile = Def->File;

				FDabExprPtr Expanded = Instantiate(*Def->Stmt, Values, Slot->Span);
				ReplaceSlot(Slot, MoveTemp(Expanded));
				ExpandSlot(Slot, Depth + 1);

				// When the body was itself a single template call, the inner expansion already replaced the
				// root; the author wrote the outer call, so that is the origin to keep.
				Main.Origins.Add(Slot.Get(), MoveTemp(Origin));
			}

			void ExpandBlock(FDabBlockExpr& Block)
			{
				for (FDabStmtPtr& Stmt : Block.Statements)
				{
					if (FDabNamedBlockStmt* Named = Stmt->As<FDabNamedBlockStmt>())
					{
						if (Named->Word.Equals(TemplateWord, ESearchCase::CaseSensitive))
						{
							continue;
						}
						ForEachExprSlotInMeta(Named->Meta.Get(), [this](FDabExprPtr& Slot) { ExpandSlot(Slot, 0); });
						for (TUniquePtr<FDabParam>& Param : Named->Params)
						{
							ExpandSlot(Param->Default, 0);
						}
						if (Named->Body)
						{
							ExpandBlock(*Named->Body);
						}
						if (Named->Code)
						{
							// The statements of code are expanded where they stand, like the entries of a block.
							ForEachExprSlot(*Named->Code, [this](FDabExprPtr& Slot) { ExpandSlot(Slot, 0); });
						}
						if (Named->Value)
						{
							ExpandSlot(Named->Value, 0);
						}
						continue;
					}
					ForEachExprSlot(*Stmt, [this](FDabExprPtr& Slot) { ExpandSlot(Slot, 0); });
				}
			}
		};

		void CollectTemplates(const FDabFile& File, TMap<FString, FTemplateDef>& Templates, FDabDiagnosticSink& Sink)
		{
			if (!File.Document || !File.Document->Body)
			{
				return;
			}
			for (const FDabStmtPtr& Stmt : File.Document->Body->Statements)
			{
				const FDabNamedBlockStmt* Named = Stmt->As<FDabNamedBlockStmt>();
				if (!Named || !Named->Word.Equals(TemplateWord, ESearchCase::CaseSensitive))
				{
					continue;
				}
				if (!Named->Value)
				{
					Sink.ErrorIn(TEXT("DAB3106"), File.Path, Named->NameSpan,
						FString::Printf(TEXT("Template '%s' has no body; write it as 'Template %s(...) = expression;'."), *Named->Name, *Named->Name));
					continue;
				}
				if (const FTemplateDef* Existing = Templates.Find(Named->Name))
				{
					Sink.ErrorIn(TEXT("DAB3105"), File.Path, Named->NameSpan,
						FString::Printf(TEXT("Template '%s' is already defined in %s (line %d)."),
							*Named->Name, *Existing->File, Existing->Stmt->NameSpan.Line));
					continue;
				}
				Templates.Add(Named->Name, FTemplateDef{ Named, File.Path });
			}
		}
	}

	bool FDabFileSystemIncludeLoader::Load(const FString& FromFile, const FString& IncludePath, FString& OutResolvedPath, FString& OutText)
	{
		TArray<FString> Candidates;
		if (!FPaths::IsRelative(IncludePath))
		{
			Candidates.Add(IncludePath);
		}
		else
		{
			Candidates.Add(FPaths::Combine(FPaths::GetPath(FromFile), IncludePath));
			for (const FString& Root : Roots)
			{
				Candidates.Add(FPaths::Combine(Root, IncludePath));
			}
		}

		for (FString& Candidate : Candidates)
		{
			Candidate = FPaths::ConvertRelativePathToFull(Candidate);
			FPaths::NormalizeFilename(Candidate);
			FPaths::CollapseRelativeDirectories(Candidate);
			if (IFileManager::Get().FileExists(*Candidate) && FFileHelper::LoadFileToString(OutText, *Candidate))
			{
				OutResolvedPath = Candidate;
				return true;
			}
		}
		return false;
	}

	bool ResolveDabIncludes(FDabCompilationUnit& Unit, IDabIncludeLoader& Loader, FDabDiagnosticSink& Sink)
	{
		if (!Unit.Main)
		{
			return true;
		}
		const int32 ErrorsBefore = Sink.NumErrors();
		FIncludeWalker Walker{ Unit, Loader, Sink };
		Walker.Parsed.Add(Unit.Main->Path);
		Walker.Stack.Add(Unit.Main->Path);
		Walker.Walk(*Unit.Main);
		return Sink.NumErrors() == ErrorsBefore;
	}

	bool ExpandDabTemplates(FDabCompilationUnit& Unit, FDabDiagnosticSink& Sink)
	{
		if (!Unit.Main || !Unit.Main->Document || !Unit.Main->Document->Body)
		{
			return true;
		}
		const int32 ErrorsBefore = Sink.NumErrors();

		FExpander Expander{ *Unit.Main, Sink };
		CollectTemplates(*Unit.Main, Expander.Templates, Sink);
		for (const TUniquePtr<FDabFile>& Library : Unit.Libraries)
		{
			CollectTemplates(*Library, Expander.Templates, Sink);
		}

		if (!Expander.Templates.IsEmpty())
		{
			Expander.ExpandBlock(*Unit.Main->Document->Body);
		}
		return Sink.NumErrors() == ErrorsBefore;
	}
}
