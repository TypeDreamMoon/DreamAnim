// Copyright DreamMoon. All Rights Reserved.
//
// What a freshly added node holds, and so which pin values are worth keeping.
//
// The engine answers this itself: a pin of a unit, a dispatch, a function call or an aggregate knows the
// value its node would have started with (URigVMPin::HasOriginalDefaultValue), computed the same way the
// controller computes it when it adds the node and when it resets a pin. Asking the pin is exact for
// every node kind and costs no probe node in a scratch graph. Nodes that have no such source (a collapse
// node, a return) are compared with the default of the pin's type instead.

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"

class UObject;
class URigVMNode;
class URigVMPin;

namespace UE::DreamAnim::Rig
{
	class FDcrNodeDefaults
	{
	public:
		/** The value a new pin of the type starts with, in export text: `0.000000`, `False`, `()`, the first enumerator, a struct's defaults. */
		static FString GetTypeDefault(const FString& CPPType, const UObject* TypeObject);

		/** True when Value is empty or the type's default. */
		static bool IsTypeDefault(const FString& CPPType, const UObject* TypeObject, const FString& Value);

		/** True when the root pin holds what a freshly added node of the same kind holds. */
		static bool IsFreshValue(URigVMNode* Node, URigVMPin* RootPin);

		/**
		 * Adds the node's root input (and IO) pin values that differ from a fresh node's to Out.Values, by
		 * root pin name. Hidden and execute pins never appear; the pins in AlwaysPins appear whatever they hold.
		 */
		static void CollectValues(URigVMNode* Node, const TSet<FName>& AlwaysPins, Model::FDcrModelNode& Out);
	};
}
