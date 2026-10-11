// Copyright DreamMoon. All Rights Reserved.
//
// The hierarchy of a generated rig: what is there goes, then the model's elements are added in order
// through the hierarchy controller, undo off. Element settings are the engine's own properties read back
// from export text by name.

#include "DcrGenerateContext.h"

#include "Misc/StringOutputDevice.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyElements.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig::Gen
{
	using namespace UE::DreamAnim::Model;

	namespace
	{
		ERigElementType ToElementType(EDcrElementKind Kind)
		{
			switch (Kind)
			{
			case EDcrElementKind::Bone: return ERigElementType::Bone;
			case EDcrElementKind::Null: return ERigElementType::Null;
			case EDcrElementKind::Control: return ERigElementType::Control;
			case EDcrElementKind::Curve: return ERigElementType::Curve;
			case EDcrElementKind::Connector: return ERigElementType::Connector;
			case EDcrElementKind::Socket: return ERigElementType::Socket;
			default: return ERigElementType::None;
			}
		}

		bool ImportTransform(FContext& Ctx, const FString& Where, const FString& What, const FString& Text, FTransform& OutTransform)
		{
			FString Error;
			FTransform Value = FTransform::Identity;
			if (!ImportStructText(TBaseStructure<FTransform>::Get(), Text, &Value, Error))
			{
				Ctx.Error(Where, FString::Printf(TEXT("%s '%s' is not a transform: %s"), *What, *Text, *Error));
				return false;
			}
			OutTransform = Value;
			return true;
		}

		/** Sets one property of a settings struct from export text; the name must be one of the struct's own. */
		void ImportSettingsProperty(FContext& Ctx, const FString& Where, const UScriptStruct* Struct, void* Settings, const FDcrValue& Property)
		{
			const FProperty* Target = Struct->FindPropertyByName(FName(*Property.Name));
			if (!Target)
			{
				Ctx.Warning(Where, FString::Printf(TEXT("'%s' is not a property of %s; ignored"), *Property.Name, *Struct->GetName()));
				return;
			}
			FStringOutputDevice Errors;
			const TCHAR* End = Target->ImportText_Direct(*Property.Value, Target->ContainerPtrToValuePtr<void>(Settings), nullptr, PPF_None, &Errors);
			if (!End || !Errors.IsEmpty())
			{
				Ctx.Warning(Where, FString::Printf(TEXT("'%s' could not be read from '%s'%s"), *Property.Name, *Property.Value, Errors.IsEmpty() ? TEXT("") : *(TEXT(": ") + Errors.TrimStartAndEnd())));
			}
		}

		FRigElementKey AddElement(FContext& Ctx, URigHierarchyController& Controller, const FDcrModelElement& Element, const FString& Where)
		{
			const FName Name(*Element.Name);
			FRigElementKey Parent;
			if (!Element.Parent.IsEmpty())
			{
				Parent = FRigElementKey(FName(*Element.Parent), ToElementType(Element.ParentKind));
			}

			switch (Element.Kind)
			{
			case EDcrElementKind::Bone:
			{
				FTransform Transform = FTransform::Identity;
				ERigBoneType BoneType = ERigBoneType::User;
				for (const FDcrValue& Property : Element.Properties)
				{
					if (Property.Name == TEXT("Transform"))
					{
						ImportTransform(Ctx, Where, Property.Name, Property.Value, Transform);
					}
					else if (Property.Name == TEXT("BoneType"))
					{
						const int64 Value = StaticEnum<ERigBoneType>()->GetValueByNameString(Property.Value);
						if (Value == INDEX_NONE)
						{
							Ctx.Warning(Where, FString::Printf(TEXT("'%s' is not a bone type; ignored"), *Property.Value));
						}
						else
						{
							BoneType = ERigBoneType(Value);
						}
					}
					else
					{
						Ctx.Warning(Where, FString::Printf(TEXT("a bone has no property '%s'; ignored"), *Property.Name));
					}
				}
				return Controller.AddBone(Name, Parent, Transform, false, BoneType, false, false);
			}
			case EDcrElementKind::Null:
			{
				FTransform Transform = FTransform::Identity;
				for (const FDcrValue& Property : Element.Properties)
				{
					if (Property.Name == TEXT("Transform"))
					{
						ImportTransform(Ctx, Where, Property.Name, Property.Value, Transform);
					}
					else
					{
						Ctx.Warning(Where, FString::Printf(TEXT("a null has no property '%s'; ignored"), *Property.Name));
					}
				}
				return Controller.AddNull(Name, Parent, Transform, false, false, false);
			}
			case EDcrElementKind::Control:
			{
				FRigControlSettings Settings;
				FTransform Offset = FTransform::Identity;
				FTransform ShapeTransform = FTransform::Identity;
				const FString* InitialText = nullptr;
				for (const FDcrValue& Property : Element.Properties)
				{
					if (Property.Name == TEXT("Offset"))
					{
						ImportTransform(Ctx, Where, Property.Name, Property.Value, Offset);
					}
					else if (Property.Name == TEXT("ShapeTransform"))
					{
						ImportTransform(Ctx, Where, Property.Name, Property.Value, ShapeTransform);
					}
					else if (Property.Name == TEXT("Initial"))
					{
						InitialText = &Property.Value;
					}
					else
					{
						ImportSettingsProperty(Ctx, Where, FRigControlSettings::StaticStruct(), &Settings, Property);
					}
				}

				// The value is typed by the control, so it is read after the settings are in.
				FRigControlValue Initial = Settings.GetIdentityValue();
				if (InitialText)
				{
					FString Error;
					FRigControlValue Value = Initial;
					if (ImportStructText(FRigControlValue::StaticStruct(), *InitialText, &Value, Error))
					{
						Initial = Value;
					}
					else
					{
						Ctx.Warning(Where, FString::Printf(TEXT("the initial value '%s' could not be read: %s"), **InitialText, *Error));
					}
				}
				const FRigElementKey Added = Controller.AddControl(Name, Parent, Settings, Initial, Offset, ShapeTransform, false, false);
				// The engine gives a control a display name no sibling has; older rigs hold two the same, kept as written.
				FRigControlElement* Control = Controller.GetHierarchy()->Find<FRigControlElement>(Added);
				if (Control && !Settings.DisplayName.IsNone())
				{
					Control->Settings.DisplayName = Settings.DisplayName;
				}
				return Added;
			}
			case EDcrElementKind::Curve:
			{
				float Value = 0.f;
				for (const FDcrValue& Property : Element.Properties)
				{
					if (Property.Name == TEXT("Value"))
					{
						Value = float(FCString::Atod(*Property.Value));
					}
					else
					{
						Ctx.Warning(Where, FString::Printf(TEXT("a curve has no property '%s'; ignored"), *Property.Name));
					}
				}
				return Controller.AddCurve(Name, Value, false, false);
			}
			case EDcrElementKind::Connector:
			{
				FRigConnectorSettings Settings;
				for (const FDcrValue& Property : Element.Properties)
				{
					ImportSettingsProperty(Ctx, Where, FRigConnectorSettings::StaticStruct(), &Settings, Property);
				}
				return Controller.AddConnector(Name, Settings, false, false);
			}
			case EDcrElementKind::Socket:
			{
				FTransform Transform = FTransform::Identity;
				FLinearColor Color = FRigSocketElement::SocketDefaultColor;
				FString Description;
				for (const FDcrValue& Property : Element.Properties)
				{
					if (Property.Name == TEXT("Transform"))
					{
						ImportTransform(Ctx, Where, Property.Name, Property.Value, Transform);
					}
					else if (Property.Name == TEXT("Color"))
					{
						FString Error;
						FLinearColor Value = Color;
						if (ImportStructText(TBaseStructure<FLinearColor>::Get(), Property.Value, &Value, Error))
						{
							Color = Value;
						}
						else
						{
							Ctx.Warning(Where, FString::Printf(TEXT("the color '%s' could not be read: %s"), *Property.Value, *Error));
						}
					}
					else if (Property.Name == TEXT("Description"))
					{
						Description = Property.Value;
					}
					else
					{
						Ctx.Warning(Where, FString::Printf(TEXT("a socket has no property '%s'; ignored"), *Property.Name));
					}
				}
				return Controller.AddSocket(Name, Parent, Transform, false, Color, Description, false, false);
			}
			default:
				break;
			}
			Ctx.Warning(Where, TEXT("an element of unknown kind is skipped"));
			return FRigElementKey();
		}
	}

	void BuildHierarchy(FContext& Ctx, const FDcrModel& Model)
	{
		URigHierarchyController* Controller = Ctx.Rig.ControlRig->GetHierarchyController();
		URigHierarchy* Hierarchy = Ctx.Rig.ControlRig->GetHierarchy();
		if (!Controller || !Hierarchy)
		{
			if (!Model.Hierarchy.IsEmpty())
			{
				Ctx.Error(TEXT("Hierarchy"), TEXT("the rig has no hierarchy controller"));
			}
			return;
		}

		// What is there goes, last created first, so that parents outlive their children until their turn.
		const TArray<FRigElementKey> Existing = Hierarchy->GetAllKeys(false);
		for (int32 Index = Existing.Num() - 1; Index >= 0; --Index)
		{
			if (Hierarchy->Contains(Existing[Index]))
			{
				Controller->RemoveElement(Existing[Index], false, false);
			}
		}

		for (const FDcrModelElement& Element : Model.Hierarchy)
		{
			const FString Where = FString::Printf(TEXT("Hierarchy/%s(%s)"), LexToString(Element.Kind), *Element.Name);
			if (Element.Kind == EDcrElementKind::Unknown)
			{
				Ctx.Warning(Where, TEXT("an element of unknown kind is skipped"));
				continue;
			}
			const FRigElementKey Added = AddElement(Ctx, *Controller, Element, Where);
			if (!Added.IsValid())
			{
				Ctx.Error(Where, TEXT("the element could not be added"));
			}
			else if (Added.Name != FName(*Element.Name))
			{
				Ctx.Error(Where, FString::Printf(TEXT("the element was added as '%s'"), *Added.Name.ToString()));
			}
		}
	}
}
