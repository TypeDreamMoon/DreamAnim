// Copyright DreamMoon. All Rights Reserved.
//
// Generate: the model into a Control Rig asset of either kind.
//
// The order is the hierarchy, the member variables, the asset's settings, the function library, the
// top-level graphs, and one compile at the end. The calls go through the RigVM controllers and the
// hierarchy controller with undo off. Graph notifications stay on: the function headers other nodes are
// made from are kept up to date by them, so a build with notifications suspended would stale them.
// What is switched off is the automatic recompile, and the engine's own error reports and toasts.

#include "Rig/DcrGenerate.h"

#include "DcrGenerateContext.h"
#include "DcrValues.h"

#include "ControlRigEditorAsset.h"
#include "ControlRigGizmoLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "RigVMBlueprintLegacy.h"
#include "RigVMEditorAsset.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMTypeUtils.h"
#include "Rigs/RigModuleDefines.h"
#include "Settings/ControlRigSettings.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig
{
	namespace Gen
	{
		using namespace UE::DreamAnim::Model;

		namespace
		{
			TAutoConsoleVariable<bool> CVarEngineReports(
				TEXT("DreamAnim.Rig.GenerateEngineReports"),
				false,
				TEXT("Leave the RigVM controllers' own error reports and toasts on while a Control Rig is generated. Off by default: the generator reports failures itself."),
				ECVF_Default);
		}

		FContext::FContext(const FDcrRig& InRig, const FDcrGenerateOptions& InOptions, FDcrGenerateResult& InResult)
			: Rig(InRig)
			, Options(InOptions)
			, Result(InResult)
			, bEngineReports(CVarEngineReports.GetValueOnAnyThread())
		{
		}

		FContext::~FContext()
		{
			RestoreReporting();
		}

		URigVMController* FContext::Controller(URigVMGraph* Graph)
		{
			URigVMController* Controller = Graph ? GetDcrController(Rig, Graph) : nullptr;
			const bool bKnown = Controllers.ContainsByPredicate([Controller](const TWeakObjectPtr<URigVMController>& Known) { return Known.Get() == Controller; });
			if (Controller && !bEngineReports && !bKnown)
			{
				Controllers.Add(TWeakObjectPtr<URigVMController>(Controller));
				ReportingBefore.Add(Controller->IsReportingEnabled());
				Controller->EnableReporting(false);
			}
			return Controller;
		}

		URigVMFunctionLibrary* FContext::Library() const
		{
			return Rig.RigVM.GetInterface() ? Rig.RigVM->GetLocalFunctionLibrary() : nullptr;
		}

		URigVMController* FContext::LibraryController()
		{
			return Controller(Library());
		}

		void FContext::RestoreReporting()
		{
			for (int32 Index = 0; Index < Controllers.Num(); ++Index)
			{
				if (URigVMController* Controller = Controllers[Index].Get())
				{
					Controller->EnableReporting(ReportingBefore[Index]);
				}
			}
			Controllers.Reset();
			ReportingBefore.Reset();
		}

		void FContext::Error(const FString& Where, const FString& Message)
		{
			Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Where, *Message));
		}

		void FContext::Warning(const FString& Where, const FString& Message)
		{
			Result.Warnings.Add(FString::Printf(TEXT("%s: %s"), *Where, *Message));
		}

		bool PinHasType(const URigVMPin* Pin, const FDcrType& Type)
		{
			if (!Pin)
			{
				return false;
			}
			const UObject* TypeObject = Pin->GetCPPTypeObject();
			return Pin->GetCPPType() == Type.CPPType && (TypeObject ? TypeObject->GetPathName() : FString()) == Type.ObjectPath;
		}

		bool LooksLikeTypeDefault(const FString& Text)
		{
			if (Text.IsEmpty() || Text == TEXT("()") || Text == TEXT("False") || Text == TEXT("None"))
			{
				return true;
			}
			// Zeros and the names of members: no digit but 0, no quote, no bare word.
			const int32 Length = Text.Len();
			int32 Index = 0;
			while (Index < Length)
			{
				const TCHAR Char = Text[Index];
				if (FChar::IsAlpha(Char) || Char == TEXT('_'))
				{
					int32 End = Index;
					while (End < Length && (FChar::IsAlnum(Text[End]) || Text[End] == TEXT('_')))
					{
						++End;
					}
					if (End < Length && Text[End] == TEXT('='))
					{
						Index = End + 1;
						continue;
					}
					return false;
				}
				if ((Char >= TEXT('1') && Char <= TEXT('9')) || Char == TEXT('"') || Char == TEXT('\''))
				{
					return false;
				}
				++Index;
			}
			return true;
		}

		bool ImportStructText(const UScriptStruct* Struct, const FString& Text, void* Data, FString& OutError)
		{
			FStringOutputDevice Errors;
			const FString StructName = Struct->GetName();
			const TCHAR* End = Struct->ImportText(*Text, Data, nullptr, PPF_None, &Errors, StructName);
			if (End && Errors.IsEmpty())
			{
				return true;
			}
			if (Errors.IsEmpty())
			{
				OutError = TEXT("the text does not read as the value");
			}
			else
			{
				OutError = Errors.TrimStartAndEnd();
			}
			return false;
		}

		namespace
		{
			// ---------------------------------------------------------------- Variables

			bool IsBuiltInBaseType(const FString& Type)
			{
				static const TCHAR* const Names[] = {
					TEXT("bool"), TEXT("int32"), TEXT("int64"), TEXT("uint8"), TEXT("uint32"), TEXT("uint64"),
					TEXT("float"), TEXT("double"), TEXT("FName"), TEXT("FString"), TEXT("FText"),
				};
				for (const TCHAR* Name : Names)
				{
					if (Type.Equals(Name, ESearchCase::CaseSensitive))
					{
						return true;
					}
				}
				return false;
			}

			/**
			 * The type string a member variable is added with: the C++ name for a built-in type, the type object's
			 * path for anything else (wrapped in TArray<> for an array). The engine checks that a struct or an
			 * enum has its object and ends the process when it does not, so nothing it would refuse goes to it.
			 */
			bool MakeMemberVariableType(const FDcrType& Type, FString& OutType, FString& OutError)
			{
				const bool bArray = Type.CPPType.StartsWith(TEXT("TArray<")) && Type.CPPType.EndsWith(TEXT(">"));
				if (Type.ObjectPath.IsEmpty())
				{
					const FString Base = bArray ? Type.CPPType.Mid(7, Type.CPPType.Len() - 8) : Type.CPPType;
					if (!IsBuiltInBaseType(Base))
					{
						OutError = FString::Printf(TEXT("the type '%s' has no object path and is not a built-in type"), *Type.CPPType);
						return false;
					}
					OutType = Type.CPPType;
					return true;
				}
				UObject* TypeObject = RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Type.ObjectPath);
				if (!TypeObject)
				{
					TypeObject = LoadObject<UObject>(nullptr, *Type.ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
				}
				if (!TypeObject)
				{
					OutError = FString::Printf(TEXT("the type object '%s' cannot be found"), *Type.ObjectPath);
					return false;
				}
				OutType = bArray ? FString::Printf(TEXT("TArray<%s>"), *Type.ObjectPath) : Type.ObjectPath;
				return true;
			}

			bool VariableMatches(const FRigVMGraphVariableDescription& Existing, const FDcrModelVariable& Wanted)
			{
				FDcrType ExistingType;
				ExistingType.CPPType = Existing.CPPType;
				if (Existing.CPPTypeObject)
				{
					ExistingType.ObjectPath = Existing.CPPTypeObject->GetPathName();
				}
				if (ExistingType != Wanted.Type || Existing.bPublic != Wanted.bPublic)
				{
					return false;
				}
				if (Wanted.Default.IsEmpty())
				{
					return LooksLikeTypeDefault(Existing.DefaultValue);
				}
				return Existing.DefaultValue == Wanted.Default;
			}

			/**
			 * A member variable's default as the asset takes it. A Blueprint-based rig parses it as a Blueprint pin
			 * default, which spells a vector, a rotator and a transform its own way (`0,0,0|0,0,0|8,8,8`).
			 */
			FString MakeMemberDefault(const FContext& Ctx, const FDcrModelVariable& Variable)
			{
				FString Default = MakeDcrEngineDefault(Variable.Type, Variable.Default);
				FDcrValueSlot Slot;
				FString PinForm;
				if (!Default.IsEmpty() && Cast<URigVMBlueprint>(Ctx.Rig.Editor) && Slot.Init(Variable.Type) && Slot.ImportText(Default)
					&& FBlueprintEditorUtils::PropertyValueToString_Direct(Slot.GetProperty(), static_cast<const uint8*>(Slot.GetMemory()), PinForm))
				{
					Default = PinForm;
				}
				return Default;
			}

			/** Makes the asset's member variables the model's: extra ones go, missing ones come, different ones are added again. */
			void SyncVariables(FContext& Ctx, const FDcrModel& Model)
			{
				IRigVMEditorAssetInterface* RigVM = Ctx.Rig.RigVM.GetInterface();
				const TArray<FRigVMGraphVariableDescription> Existing = RigVM->GetAssetVariables();

				for (const FRigVMGraphVariableDescription& Variable : Existing)
				{
					const bool bWanted = Model.Variables.ContainsByPredicate([&Variable](const FDcrModelVariable& Wanted) { return FName(*Wanted.Name) == Variable.Name; });
					if (!bWanted)
					{
						RigVM->RemoveMemberVariable(Variable.Name);
					}
				}

				for (const FDcrModelVariable& Variable : Model.Variables)
				{
					const FString Here = TEXT("Variables/") + Variable.Name;
					FString TypeText, Error;
					if (!MakeMemberVariableType(Variable.Type, TypeText, Error))
					{
						Ctx.Error(Here, Error);
						continue;
					}

					const FName Wanted(*Variable.Name);
					const FRigVMGraphVariableDescription* Current = Existing.FindByPredicate([&Wanted](const FRigVMGraphVariableDescription& Candidate) { return Candidate.Name == Wanted; });
					FName Actual = Wanted;
					if (!Current || !VariableMatches(*Current, Variable))
					{
						if (Current)
						{
							RigVM->RemoveMemberVariable(Wanted);
						}
						Actual = RigVM->AddMemberVariable(Wanted, TypeText, Variable.bPublic, Variable.bReadOnly, MakeMemberDefault(Ctx, Variable));
						if (Actual.IsNone())
						{
							Ctx.Error(Here, FString::Printf(TEXT("could not be added as '%s'"), *TypeText));
							continue;
						}
						if (Actual != Wanted)
						{
							Ctx.Warning(Here, FString::Printf(TEXT("was added as '%s'"), *Actual.ToString()));
						}
					}
					Ctx.MemberVariables.Add(Variable.Name, Actual);

					if (!Variable.Category.IsEmpty())
					{
						RigVM->SetVariableCategory(Actual, Variable.Category);
					}
					if (!Variable.Tooltip.IsEmpty())
					{
						RigVM->SetVariableTooltip(Actual, FText::FromString(Variable.Tooltip));
					}
				}

				// The model's order.
				TArray<FName> Order;
				for (const FDcrModelVariable& Variable : Model.Variables)
				{
					if (const FName* Actual = Ctx.MemberVariables.Find(Variable.Name))
					{
						Order.Add(*Actual);
					}
				}
				TArray<FName> Now;
				for (const FRigVMGraphVariableDescription& Variable : RigVM->GetAssetVariables())
				{
					if (Order.Contains(Variable.Name))
					{
						Now.Add(Variable.Name);
					}
				}
				if (Now != Order)
				{
					for (int32 Index = 0; Index < Order.Num(); ++Index)
					{
						RigVM->SetVariableIndex(Order[Index], Index);
					}
				}
			}

			// ---------------------------------------------------------------- Settings

			/** An object path out of export text: bare, quoted, or with its class in front. */
			bool ParseObjectPath(const FString& Text, FSoftObjectPath& OutPath)
			{
				const FString Trimmed = Text.TrimStartAndEnd();
				if (Trimmed.IsEmpty())
				{
					return false;
				}
				FSoftObjectPath Path;
				const TCHAR* Buffer = *Trimmed;
				if (!Path.ImportTextItem(Buffer, PPF_None, nullptr, nullptr) || Path.IsNull())
				{
					return false;
				}
				OutPath = Path;
				return true;
			}

			/** The paths of an array in export text: `("/A.A","/B.B")`, or one bare path. */
			TArray<FSoftObjectPath> ParseObjectPathList(const FString& Text)
			{
				FString Inner = Text.TrimStartAndEnd();
				if (Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")")))
				{
					Inner = Inner.Mid(1, Inner.Len() - 2);
				}
				TArray<FString> Items;
				FString Item;
				TCHAR Quote = 0;
				for (const TCHAR Char : Inner)
				{
					if (Quote)
					{
						Quote = Char == Quote ? TCHAR(0) : Quote;
					}
					else if (Char == TEXT('"') || Char == TEXT('\''))
					{
						Quote = Char;
					}
					else if (Char == TEXT(','))
					{
						Items.Add(Item);
						Item.Reset();
						continue;
					}
					Item.AppendChar(Char);
				}
				Items.Add(Item);

				TArray<FSoftObjectPath> Paths;
				for (const FString& Entry : Items)
				{
					FSoftObjectPath Path;
					if (ParseObjectPath(Entry, Path))
					{
						Paths.Add(Path);
					}
				}
				return Paths;
			}

			void ApplySettings(FContext& Ctx, const FDcrModel& Model)
			{
#if WITH_EDITORONLY_DATA
				// What a new rig starts with, unless the model names its libraries (an existing rig is rebuilt the same).
				if (!Model.Settings.ContainsByPredicate([](const FDcrValue& Setting) { return Setting.Name == TEXT("ShapeLibraries"); }))
				{
					TArray<TSoftObjectPtr<UControlRigShapeLibrary>>& Libraries = Ctx.Rig.ControlRig->GetShapeLibraries();
					Libraries.Reset();
					Libraries.Add(UControlRigSettings::Get()->DefaultShapeLibrary);
				}
#endif
				for (const FDcrValue& Setting : Model.Settings)
				{
					const FString Here = TEXT("Settings/") + Setting.Name;
					if (Setting.Name == TEXT("PreviewSkeletalMesh") || Setting.Name == TEXT("PreviewMesh"))
					{
						FSoftObjectPath Path;
						USkeletalMesh* Mesh = nullptr;
						if (ParseObjectPath(Setting.Value, Path))
						{
							Mesh = Cast<USkeletalMesh>(Path.TryLoad());
							if (!Mesh)
							{
								// A mesh this project does not have: the reference is kept, as the asset kept it.
								bool bKept = false;
								for (UObject* Target : { Ctx.Rig.Editor, Ctx.Rig.Asset })
								{
									const FProperty* Property = Target ? Target->GetClass()->FindPropertyByName(TEXT("PreviewSkeletalMesh")) : nullptr;
									if (Property && Property->ImportText_InContainer(*Path.ToString(), Target, Target, PPF_None))
									{
										bKept = true;
										break;
									}
								}
								if (!bKept)
								{
									Ctx.Warning(Here, FString::Printf(TEXT("the mesh '%s' cannot be loaded"), *Path.ToString()));
								}
								continue;
							}
						}
						Ctx.Rig.ControlRig->SetPreviewMesh(Mesh, false);
					}
					else if (Setting.Name == TEXT("ShapeLibraries"))
					{
#if WITH_EDITORONLY_DATA
						TArray<TSoftObjectPtr<UControlRigShapeLibrary>>& Libraries = Ctx.Rig.ControlRig->GetShapeLibraries();
						Libraries.Reset();
						for (const FSoftObjectPath& Path : ParseObjectPathList(Setting.Value))
						{
							Libraries.Add(TSoftObjectPtr<UControlRigShapeLibrary>(Path));
						}
#endif
					}
					else
					{
						// Any other key is a property of the asset class, by its name.
						bool bApplied = false;
						for (UObject* Target : { Ctx.Rig.Editor, Ctx.Rig.Asset })
						{
							const FProperty* Property = Target ? Target->GetClass()->FindPropertyByName(FName(*Setting.Name)) : nullptr;
							if (!Property)
							{
								continue;
							}
							FStringOutputDevice Errors;
							const TCHAR* End = Property->ImportText_InContainer(*Setting.Value, Target, Target, PPF_None, &Errors);
							if (!End || !Errors.IsEmpty())
							{
								Ctx.Warning(Here, FString::Printf(TEXT("'%s' could not be read as a value of %s"), *Setting.Value, *Property->GetClass()->GetName()));
							}
							bApplied = true;
							break;
						}
						if (!bApplied)
						{
							Ctx.Warning(Here, TEXT("the asset has no such property; ignored"));
						}
					}
				}

			}

			/**
			 * Makes the asset a rig module or a standalone rig as the model says, before anything else is built:
			 * turning a rig into a module rewrites its hierarchy. Then writes the module settings.
			 */
			void ApplyModule(FContext& Ctx, const FDcrModel& Model)
			{
				IControlRigEditorAssetInterface* Editor = Ctx.Rig.ControlRig.GetInterface();
				if (!Editor)
				{
					return;
				}
				FString Error;
				if (Model.bModule && !Editor->IsControlRigModule())
				{
					if (!Editor->TurnIntoControlRigModule(false, &Error))
					{
						Ctx.Error(TEXT("Module"), FString::Printf(TEXT("the asset could not be made a rig module: %s"), *Error));
						return;
					}
				}
				else if (!Model.bModule && Editor->IsControlRigModule())
				{
					if (!Editor->TurnIntoStandaloneRig(&Error))
					{
						Ctx.Error(TEXT("Module"), FString::Printf(TEXT("the asset could not be made a standalone rig: %s"), *Error));
					}
					return;
				}
				if (!Model.bModule)
				{
					return;
				}

				FRigModuleSettings& Settings = Editor->GetRigModuleSettings();
				Settings = FRigModuleSettings();
				// Turning a rig into a module names it after the asset; the model says the name.
				Settings.Identifier.Name = Ctx.Rig.Asset->GetName();
				for (const FDcrValue& Setting : Model.ModuleSettings)
				{
					const FString Here = TEXT("Module/") + Setting.Name;
					FProperty* Property = FRigModuleSettings::StaticStruct()->FindPropertyByName(*Setting.Name);
					if (!Property)
					{
						Ctx.Warning(Here, TEXT("FRigModuleSettings has no such property; ignored"));
						continue;
					}
					FStringOutputDevice Errors;
					if (!Property->ImportText_Direct(*Setting.Value, Property->ContainerPtrToValuePtr<void>(&Settings), nullptr, PPF_None, &Errors))
					{
						Ctx.Warning(Here, FString::Printf(TEXT("'%s' could not be read: %s"), *Setting.Value, *Errors));
					}
				}
			}

			// ---------------------------------------------------------------- Function library

			/** Lists the loaded rigs that call a function, for when the function goes: the engine deletes their call nodes. */
			void ListCallers(FContext& Ctx, URigVMFunctionLibrary* Library, const FName& FunctionName, const FString& Here)
			{
				const FString OwnPackage = Ctx.Rig.Asset->GetPackage()->GetName();
				for (const TSoftObjectPtr<URigVMFunctionReferenceNode>& Reference : Library->GetReferencesForFunction(FunctionName))
				{
					const FString Package = Reference.ToSoftObjectPath().GetLongPackageName();
					if (Package.IsEmpty() || Package == OwnPackage)
					{
						continue;
					}
					if (Reference.IsValid())
					{
						Ctx.Result.BrokenCallers.AddUnique(FString::Printf(TEXT("%s (called '%s')"), *Package, *FunctionName.ToString()));
					}
					else
					{
						Ctx.Warning(Here, FString::Printf(TEXT("'%s' calls it and is not loaded; its call node will not find the function"), *Package));
					}
				}
			}

			void RemoveFunction(FContext& Ctx, URigVMFunctionLibrary* Library, URigVMController* LibraryController, const FName& FunctionName, const FString& Here)
			{
				ListCallers(Ctx, Library, FunctionName, Here);
				if (!LibraryController->RemoveFunctionFromLibrary(FunctionName, false, false))
				{
					Ctx.Error(Here, TEXT("the function could not be removed"));
				}
			}

			/** The exposed pins of a function made to match the model's: extra ones go, missing ones come, different types change, and the order is the model's. */
			void SyncSignature(FContext& Ctx, URigVMLibraryNode* Node, const FDcrModelFunction& Function, const FString& Here)
			{
				URigVMGraph* FunctionGraph = Node->GetContainedGraph();
				URigVMController* FunctionController = Ctx.Controller(FunctionGraph);
				URigVMController* LibraryController = Ctx.LibraryController();
				if (!FunctionController || !LibraryController)
				{
					Ctx.Error(Here, TEXT("the function has no controller"));
					return;
				}
				const FName FunctionName = Node->GetFName();

				struct FWantedPin
				{
					FName Name;
					ERigVMPinDirection Direction;
					const FDcrModelFunctionPin* Pin;
				};
				TArray<FWantedPin> Wanted;
				for (const FDcrModelFunctionPin& Pin : Function.Inputs)
				{
					Wanted.Add({ FName(*Pin.Name), ERigVMPinDirection::Input, &Pin });
				}
				for (const FDcrModelFunctionPin& Pin : Function.Outputs)
				{
					Wanted.Add({ FName(*Pin.Name), ERigVMPinDirection::Output, &Pin });
				}

				// What the model does not have goes.
				const TArray<URigVMPin*> Current = Node->GetPins();
				for (URigVMPin* Pin : Current)
				{
					if (!Pin || Pin->IsExecuteContext())
					{
						continue;
					}
					const bool bKept = Wanted.ContainsByPredicate([Pin](const FWantedPin& Candidate) { return Candidate.Name == Pin->GetFName() && Candidate.Direction == Pin->GetDirection(); });
					if (!bKept)
					{
						FunctionController->RemoveExposedPin(Pin->GetFName(), false, false);
					}
				}

				for (const FWantedPin& Want : Wanted)
				{
					const FString PinWhere = Here + TEXT(".") + Want.Name.ToString();
					const FDcrType& Type = Want.Pin->Type;
					URigVMPin* Pin = Node->FindPin(Want.Name.ToString());
					if (!Pin)
					{
						const FName Added = FunctionController->AddExposedPin(Want.Name, Want.Direction, Type.CPPType, TypeObjectPath(Type), MakeDcrEngineDefault(Type, Want.Pin->Default), false, false, false);
						if (Added != Want.Name)
						{
							Ctx.Error(PinWhere, Added.IsNone() ? FString::Printf(TEXT("the pin (%s) could not be exposed"), *Type.CPPType) : FString::Printf(TEXT("the pin was exposed as '%s'"), *Added.ToString()));
						}
						continue;
					}
					if (!PinHasType(Pin, Type))
					{
						bool bSetupUndoRedo = false;
						if (!FunctionController->ChangeExposedPinType(Want.Name, Type.CPPType, TypeObjectPath(Type), bSetupUndoRedo, true, false))
						{
							Ctx.Error(PinWhere, FString::Printf(TEXT("the type could not be changed to %s"), *Type.CPPType));
							continue;
						}
					}
					// The default lives on the function's node in the library.
					Pin = Node->FindPin(Want.Name.ToString());
					const FString PinPath = FunctionName.ToString() + TEXT(".") + Want.Name.ToString();
					const FString Now = Pin ? Pin->GetDefaultValue() : FString();
					const FString Default = MakeDcrEngineDefault(Type, Want.Pin->Default);
					if (!Default.IsEmpty())
					{
						if (Now != Default && !LibraryController->SetPinDefaultValue(PinPath, Default, true, false, false, false, true))
						{
							Ctx.Error(PinWhere, FString::Printf(TEXT("the default '%s' was refused"), *Default));
						}
					}
					else if (!LooksLikeTypeDefault(Now))
					{
						LibraryController->ResetPinDefaultValue(PinPath, false, false);
					}
				}

				// The execute pin first, then the inputs, then the outputs.
				int32 Index = 0;
				if (URigVMPin* Execute = Node->FindExecutePin())
				{
					if (Execute->GetPinIndex() != 0)
					{
						FunctionController->SetExposedPinIndex(Execute->GetFName(), 0, false, false);
					}
					Index = 1;
				}
				for (const FWantedPin& Want : Wanted)
				{
					if (Node->FindPin(Want.Name.ToString()))
					{
						FunctionController->SetExposedPinIndex(Want.Name, Index++, false, false);
					}
				}
			}

			/** The local variables of a function made to match the model's. */
			void SyncLocals(FContext& Ctx, URigVMController* FunctionController, URigVMGraph* FunctionGraph, const FDcrModelFunction& Function, const FString& Here)
			{
				const TArray<FRigVMGraphVariableDescription> Existing = FunctionGraph->GetLocalVariables(false);
				bool bSame = Existing.Num() == Function.Locals.Num();
				for (int32 Index = 0; bSame && Index < Existing.Num(); ++Index)
				{
					const FDcrModelVariable& Wanted = Function.Locals[Index];
					FDcrType ExistingType;
					ExistingType.CPPType = Existing[Index].CPPType;
					if (Existing[Index].CPPTypeObject)
					{
						ExistingType.ObjectPath = Existing[Index].CPPTypeObject->GetPathName();
					}
					bSame = Existing[Index].Name == FName(*Wanted.Name) && ExistingType == Wanted.Type
						&& (Wanted.Default.IsEmpty() ? LooksLikeTypeDefault(Existing[Index].DefaultValue) : Existing[Index].DefaultValue == Wanted.Default);
				}
				if (bSame)
				{
					return;
				}
				for (const FRigVMGraphVariableDescription& Variable : Existing)
				{
					FunctionController->RemoveLocalVariable(Variable.Name, false, false);
				}
				for (const FDcrModelVariable& Local : Function.Locals)
				{
					const FRigVMGraphVariableDescription Added = FunctionController->AddLocalVariableFromObjectPath(FName(*Local.Name), Local.Type.CPPType, Local.Type.ObjectPath, MakeDcrEngineDefault(Local.Type, Local.Default), false);
					if (Added.Name != FName(*Local.Name))
					{
						Ctx.Error(Here + TEXT("/") + Local.Name, Added.Name.IsNone() ? TEXT("the local variable could not be added") : TEXT("the local variable was added under another name"));
					}
				}
			}

			/** The function's node in the library, ready for its body: it exists, with the model's signature, locals and metadata, and its graph holds only Entry and Return. */
			URigVMLibraryNode* PrepareFunction(FContext& Ctx, const FDcrModelFunction& Function, int32 Index)
			{
				URigVMFunctionLibrary* Library = Ctx.Library();
				URigVMController* LibraryController = Ctx.LibraryController();
				const FString Here = TEXT("FunctionLibrary/") + Function.Name;
				const FName Name(*Function.Name);

				URigVMLibraryNode* Node = Library->FindFunction(Name);
				if (Node && (Node->FindExecutePin() != nullptr) != Function.bMutable)
				{
					// The pure and mutable kinds differ by the execute pin, which the library can add and take away in place.
					if (!LibraryController->SetFunctionIsPure(Name, !Function.bMutable, false, false))
					{
						Ctx.Warning(Here, TEXT("it could not change between pure and mutable in place; it is removed and added again"));
						RemoveFunction(Ctx, Library, LibraryController, Name, Here);
						Node = nullptr;
					}
				}
				if (!Node)
				{
					Node = LibraryController->AddFunctionToLibrary(Name, Function.bMutable, FVector2D(0.0, 300.0 * Index), false, false);
					if (!Node)
					{
						Ctx.Error(Here, TEXT("the function could not be added to the library"));
						return nullptr;
					}
					if (Node->GetFName() != Name)
					{
						Ctx.Error(Here, FString::Printf(TEXT("the function was added as '%s'"), *Node->GetName()));
					}
				}

				SyncSignature(Ctx, Node, Function, Here);

				URigVMGraph* FunctionGraph = Node->GetContainedGraph();
				URigVMController* FunctionController = Ctx.Controller(FunctionGraph);
				if (!FunctionController)
				{
					return nullptr;
				}
				ApplyExecutePinName(Ctx, FunctionController, FunctionGraph, Function.Graph, Here);
				// The body is rebuilt, so nothing of the old one stays; the locals are settled while no node uses them.
				ClearGraph(Ctx, FunctionGraph, Here);
				SyncLocals(Ctx, FunctionController, FunctionGraph, Function, Here);

				LibraryController->MarkFunctionAsPublic(Name, Function.bPublic, false, false);
				LibraryController->SetNodeCategoryByName(Name, Function.Category, false, false);
				LibraryController->SetNodeKeywordsByName(Name, Function.Keywords, false, false);
				LibraryController->SetNodeDescriptionByName(Name, Function.Description, false, false);
				return Node;
			}

			/** Brings the function library to the model's: functions that are gone are removed, the others are updated in place, then every body is built. */
			void SyncFunctions(FContext& Ctx, const FDcrModel& Model)
			{
				URigVMFunctionLibrary* Library = Ctx.Library();
				URigVMController* LibraryController = Ctx.LibraryController();
				if (!Library || !LibraryController)
				{
					if (!Model.Functions.IsEmpty())
					{
						Ctx.Error(TEXT("FunctionLibrary"), TEXT("the rig has no function library"));
					}
					return;
				}

				TSet<FName> Wanted;
				for (const FDcrModelFunction& Function : Model.Functions)
				{
					Wanted.Add(FName(*Function.Name));
				}
				for (URigVMLibraryNode* Node : Library->GetFunctions())
				{
					if (Node && !Wanted.Contains(Node->GetFName()))
					{
						RemoveFunction(Ctx, Library, LibraryController, Node->GetFName(), TEXT("FunctionLibrary/") + Node->GetName());
					}
				}

				// Every signature first: a body may call any function of the library.
				int32 Index = 0;
				for (const FDcrModelFunction& Function : Model.Functions)
				{
					PrepareFunction(Ctx, Function, Index++);
				}
				for (const FDcrModelFunction& Function : Model.Functions)
				{
					if (URigVMLibraryNode* Node = Library->FindFunction(FName(*Function.Name)))
					{
						BuildGraph(Ctx, Node->GetContainedGraph(), Function.Graph, TEXT("FunctionLibrary/") + Function.Name, EGraphRole::Function);
					}
				}
			}

			// ---------------------------------------------------------------- Top-level graphs

			/** Removing a top-level graph is protected on the interface; both asset classes expose it. */
			bool RemoveTopLevelGraph(const FDcrRig& Rig, const FString& Name)
			{
				if (URigVMBlueprint* Blueprint = Cast<URigVMBlueprint>(Rig.Editor))
				{
					return Blueprint->RemoveModel(Name, false, false);
				}
				if (URigVMEditorAsset* EditorAsset = Cast<URigVMEditorAsset>(Rig.Editor))
				{
					return EditorAsset->RemoveModel(Name, false, false);
				}
				return false;
			}

			void BuildTopLevelGraphs(FContext& Ctx, const FDcrModel& Model)
			{
				URigVMGraph* DefaultGraph = Ctx.Rig.RigVM->GetDefaultModel();

				// What is there goes first: an event may be in a rig once, so one left in an old graph would keep the
				// model's from being added. Graphs the model does not have go; the default graph stays, emptied.
				TSet<FString> Wanted;
				for (const FDcrModelGraph& Source : Model.Graphs)
				{
					Wanted.Add(Source.Name);
				}
				TArray<URigVMGraph*> Existing;
				if (FRigVMClient* Client = Ctx.Rig.RigVM->GetRigVMClient())
				{
					for (URigVMGraph* Graph : Client->GetModels())
					{
						Existing.Add(Graph);
					}
				}
				for (URigVMGraph* Graph : Existing)
				{
					if (!Graph)
					{
						continue;
					}
					const FString Name = Graph->GetGraphName();
					if (Graph == DefaultGraph || Wanted.Contains(Name))
					{
						ClearGraph(Ctx, Graph, Name);
					}
					else if (!RemoveTopLevelGraph(Ctx.Rig, Name))
					{
						Ctx.Error(Name, TEXT("the graph could not be removed"));
					}
				}

				for (int32 Index = 0; Index < Model.Graphs.Num(); ++Index)
				{
					const FDcrModelGraph& Source = Model.Graphs[Index];
					URigVMGraph* Target = FindDcrModelGraph(Ctx.Rig, Source.Name);
					if (!Target && Index == 0 && DefaultGraph)
					{
						// The first graph is the default graph, under the name the model gives it.
						Target = DefaultGraph;
						if (Ctx.Rig.RigVM->GetRigVMClient()->RenameModel(DefaultGraph->GetNodePath(), FName(*Source.Name), false).IsNone())
						{
							Ctx.Warning(Source.Name, FString::Printf(TEXT("the first graph is built into the default graph '%s'"), *DefaultGraph->GetGraphName()));
						}
					}
					if (!Target)
					{
						Target = AddDcrModelGraph(Ctx.Rig, Source.Name);
						if (!Target)
						{
							Ctx.Error(Source.Name, TEXT("the graph could not be added"));
							continue;
						}
						if (Target->GetGraphName() != Source.Name)
						{
							Ctx.Error(Source.Name, FString::Printf(TEXT("the graph was added as '%s'"), *Target->GetGraphName()));
						}
					}
					BuildGraph(Ctx, Target, Source, Source.Name, EGraphRole::TopLevel);
				}
			}

			EDcrAssetKind ToAssetKind(EDcrModelAssetKind Kind)
			{
				return Kind == EDcrModelAssetKind::Legacy ? EDcrAssetKind::Legacy : EDcrAssetKind::Independent;
			}
		}
	}

	FDcrGenerateResult GenerateDcrRig(const Model::FDcrModel& Model, const FDcrGenerateOptions& Options)
	{
		using namespace Gen;

		FDcrGenerateResult Result;
		if (Model.AssetPath.IsEmpty())
		{
			Result.Errors.Add(TEXT("the model has no asset path"));
			return Result;
		}

		const FString ObjectPath = Model.AssetPath.Contains(TEXT(".")) ? Model.AssetPath
			: Model.AssetPath + TEXT(".") + FPackageName::GetShortName(Model.AssetPath);

		// An existing asset is rebuilt in place and keeps its kind; a missing one is created in the model's.
		FDcrRig Rig;
		UObject* Existing = nullptr;
		{
			// A Blueprint compiles as it loads; what it says of the old graphs does not matter, they are replaced.
			FDcrQuietCompileScope Quiet;
			Existing = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (Existing)
		{
			Rig = WrapDcrRig(Existing);
			if (!Rig.IsValid())
			{
				Result.Errors.Add(FString::Printf(TEXT("%s: the asset exists and is not a Control Rig"), *ObjectPath));
				return Result;
			}
			if (Rig.GetKind() != ToAssetKind(Model.AssetKind))
			{
				Result.Warnings.Add(FString::Printf(TEXT("%s: the asset is of the %s kind and keeps it"), *ObjectPath, LexToString(Rig.GetKind())));
			}
		}
		else
		{
			Rig = CreateDcrRig(FPackageName::ObjectPathToPackageName(ObjectPath), ToAssetKind(Model.AssetKind));
			if (!Rig.IsValid())
			{
				Result.Errors.Add(FString::Printf(TEXT("%s: the asset could not be created"), *ObjectPath));
				return Result;
			}
			if (Rig.Asset->GetPathName() != ObjectPath)
			{
				Result.Warnings.Add(FString::Printf(TEXT("%s: the asset was created as '%s'"), *ObjectPath, *Rig.Asset->GetPathName()));
			}
		}
		Result.Rig = Rig;

		// What the extractor could not place was never in the model's graphs; say so, so that a rig that looks short is not a surprise.
		for (int32 Index = 0; Index < Model.Unsupported.Num() && Index < 50; ++Index)
		{
			Result.Warnings.Add(FString::Printf(TEXT("unsupported at %s: %s"), *Model.Unsupported[Index].Location, *Model.Unsupported[Index].Description));
		}

		{
			// Building compiles too, where the engine asks for it on its own (turning the automatic recompile back on).
			FDcrQuietCompileScope Quiet;
			FContext Ctx(Rig, Options, Result);
			const bool bAutoRecompile = Rig.RigVM->GetAutoVMRecompile();
			Rig.RigVM->SetAutoVMRecompile(false);

			ApplyModule(Ctx, Model);
			BuildHierarchy(Ctx, Model);
			SyncVariables(Ctx, Model);
			ApplySettings(Ctx, Model);
			SyncFunctions(Ctx, Model);
			BuildTopLevelGraphs(Ctx, Model);

			Rig.RigVM->SetAutoVMRecompile(bAutoRecompile);
			Ctx.RestoreReporting();
		}

		int32 CompileWarnings = 0;
		for (const FString& Error : CompileDcrRig(Rig, &CompileWarnings))
		{
			Result.Errors.Add(TEXT("compile: ") + Error);
		}
		if (CompileWarnings > 0)
		{
			Result.Warnings.Add(FString::Printf(TEXT("compile: %d warnings"), CompileWarnings));
		}

		Rig.Asset->MarkPackageDirty();
		if (Options.bSave && Result.Errors.IsEmpty() && !SaveDcrRig(Rig))
		{
			Result.Errors.Add(FString::Printf(TEXT("%s: the package could not be saved"), *ObjectPath));
		}
		return Result;
	}
}
