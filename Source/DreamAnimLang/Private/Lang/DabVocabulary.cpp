// Copyright DreamMoon. All Rights Reserved.
//
// The words of each kind of document, as Docs/Language.md and Docs/ControlRig.md describe them. Words whose
// meaning comes from the engine (node types, node fields, class settings, machine and transition properties,
// a Control Rig's units) are not here: `dab.ps1 vocab` reads those from the engine.

#include "Lang/DabVocabulary.h"

namespace UE::DreamAnim::Lang
{
	namespace VocabularyPrivate
	{
		using R = EDabWordRole;

		// The parts of the language every document shares.
		#define DAB_COMMON_WORDS \
			{ TEXT("Name"), R::HeaderArgument, TEXT("header"), TEXT("The asset's path under the root, without an extension.") }, \
			{ TEXT("Root"), R::HeaderArgument, TEXT("header"), TEXT("Where the path starts: \"Game\", \"Engine\" or \"Plugin.<Name>\".") }, \
			{ TEXT("#include"), R::Directive, TEXT("file"), TEXT("Includes a template library (.dat) before the document.") }, \
			{ TEXT("#Region"), R::Directive, TEXT("block"), TEXT("Starts a region: a comment box around what it holds.") }, \
			{ TEXT("#EndRegion"), R::Directive, TEXT("block"), TEXT("Ends a region.") }, \
			{ TEXT("Path"), R::Function, TEXT("value"), TEXT("An asset by its root and path: Path(Root, \"Folder/Asset\").") }, \
			{ TEXT("Unsupported"), R::Function, TEXT("value"), TEXT("The decompiler's placeholder for what the text cannot say; it does not build.") }, \
			{ TEXT("true"), R::Value, TEXT("value"), TEXT("The bool true.") }, \
			{ TEXT("false"), R::Value, TEXT("value"), TEXT("The bool false.") }, \
			{ TEXT("None"), R::Value, TEXT("value"), TEXT("No object, no name, no pose.") }

		const FDabWord AnimBlueprintWords[] =
		{
			{ TEXT("AnimBlueprint"), R::Document, TEXT("file"), TEXT("The document of an Animation Blueprint (.dab).") },
			DAB_COMMON_WORDS,
			{ TEXT("Template"), R::Block, TEXT("document"), TEXT("A named expression, expanded where it is used.") },
			{ TEXT("Settings"), R::Section, TEXT("document"), TEXT("Parent class, skeleton, interfaces, default animation set, class settings.") },
			{ TEXT("Defaults"), R::Section, TEXT("document"), TEXT("Class defaults of the parent class's properties.") },
			{ TEXT("Aliases"), R::Section, TEXT("document"), TEXT("Short names for paths from Self.") },
			{ TEXT("Properties"), R::Section, TEXT("document"), TEXT("Tuning values: Blueprint variables the graph reads.") },
			{ TEXT("Assets"), R::Section, TEXT("document"), TEXT("Animation slots an animation set fills.") },
			{ TEXT("Notifies"), R::Section, TEXT("document"), TEXT("Anim notify handlers.") },
			{ TEXT("Graph"), R::Section, TEXT("document"), TEXT("The AnimGraph.") },
			{ TEXT("Layer"), R::Block, TEXT("document"), TEXT("An animation layer: Layer Name = Pose;, a body, or a declaration.") },
			{ TEXT("StateMachine"), R::Block, TEXT("document"), TEXT("A state machine; it runs where its name is used as a pose.") },
			{ TEXT("State"), R::Block, TEXT("StateMachine"), TEXT("A state of the machine.") },
			{ TEXT("Conduit"), R::Block, TEXT("StateMachine"), TEXT("A conduit, with an optional condition.") },
			{ TEXT("Alias"), R::Block, TEXT("StateMachine"), TEXT("A state alias: Alias Name = [ States ]; or = All;.") },
			{ TEXT("Parent"), R::Setting, TEXT("Settings"), TEXT("The class the Blueprint derives from.") },
			{ TEXT("Skeleton"), R::Setting, TEXT("Settings"), TEXT("The target skeleton.") },
			{ TEXT("Implements"), R::Setting, TEXT("Settings"), TEXT("Animation Layer Interfaces the Blueprint implements.") },
			{ TEXT("DefaultAnimSet"), R::Setting, TEXT("Settings"), TEXT("The animation set the Blueprint starts with.") },
			{ TEXT("Output"), R::Key, TEXT("Graph, Layer"), TEXT("The graph's result pose.") },
			{ TEXT("Inputs"), R::Key, TEXT("Graph, Layer, LinkedAnimGraph, LinkedAnimLayer"), TEXT("The order of a graph's input poses; on a linked node, what feeds its target's inputs.") },
			{ TEXT("Entry"), R::Key, TEXT("StateMachine"), TEXT("The state or conduit the machine starts in.") },
			{ TEXT("Rules"), R::Key, TEXT("StateMachine"), TEXT("Named conditions for the machine's transitions and conduits.") },
			{ TEXT("Transitions"), R::Key, TEXT("StateMachine"), TEXT("The machine's transitions: From -> To : Condition [ options ];.") },
			{ TEXT("Pose"), R::Key, TEXT("State"), TEXT("The state's result.") },
			{ TEXT("OnEnter"), R::Key, TEXT("State"), TEXT("A call when the state is entered.") },
			{ TEXT("OnExit"), R::Key, TEXT("State"), TEXT("A call when the state is left.") },
			{ TEXT("OnBlended"), R::Key, TEXT("State"), TEXT("A call when the state has fully blended in.") },
			{ TEXT("Default"), R::Key, TEXT("keyed blend map"), TEXT("The pose a keyed BlendPose map falls back to; also the default in a keyed BlendTime map.") },
			{ TEXT("Cached"), R::Metadata, TEXT("named pose"), TEXT("Caches the pose so that several places read it.") },
			{ TEXT("OnInitialUpdate"), R::Metadata, TEXT("named pose, Output"), TEXT("The pose's root node calls this on its initial update.") },
			{ TEXT("OnBecomeRelevant"), R::Metadata, TEXT("named pose, Output"), TEXT("The pose's root node calls this when it becomes relevant.") },
			{ TEXT("OnUpdate"), R::Metadata, TEXT("named pose, Output"), TEXT("The pose's root node calls this on every update.") },
			{ TEXT("LayerGroup"), R::Metadata, TEXT("Layer"), TEXT("The layer's group.") },
			{ TEXT("BlendOptions"), R::Metadata, TEXT("Layer"), TEXT("The layer's blend in and out options.") },
			{ TEXT("Blend"), R::Metadata, TEXT("transition"), TEXT("Crossfade duration in seconds.") },
			{ TEXT("Curve"), R::Metadata, TEXT("transition"), TEXT("The blend curve, an EAlphaBlendOption name.") },
			{ TEXT("Inertialize"), R::Metadata, TEXT("transition"), TEXT("Blend by inertialization; with a value, also its duration.") },
			{ TEXT("Priority"), R::Metadata, TEXT("transition"), TEXT("Priority order; the lowest number wins.") },
			{ TEXT("BlendProfile"), R::Metadata, TEXT("transition"), TEXT("The transition's blend profile.") },
			{ TEXT("Self"), R::Value, TEXT("expression"), TEXT("The anim instance: paths into C++ start here.") },
			{ TEXT("Props"), R::Value, TEXT("expression"), TEXT("The tuning values of the Properties section.") },
			{ TEXT("Assets"), R::Value, TEXT("expression"), TEXT("The asset slots of the Assets section.") },
			{ TEXT("Auto"), R::Value, TEXT("transition condition"), TEXT("The engine's automatic rule: when the source state's animation is about to finish.") },
			{ TEXT("All"), R::Value, TEXT("Alias"), TEXT("Every state of the machine.") },
			{ TEXT("Input"), R::Builtin, TEXT("pose"), TEXT("An input pose of the graph: a Linked Input Pose node. Input(Name, settings...).") },
			{ TEXT("Slot"), R::Builtin, TEXT("pose"), TEXT("A Slot node: Slot(\"Name\", settings...).") },
			{ TEXT("Call"), R::Builtin, TEXT("pose"), TEXT("A Call Function node: Call(Target.Function, On = Site).") },
			{ TEXT("bones"), R::Function, TEXT("value"), TEXT("A branch filter list of a layered blend: bones(a, b, Depth = 2).") },
			{ TEXT("Abs"), R::Function, TEXT("expression"), TEXT("Absolute value.") },
			{ TEXT("Min"), R::Function, TEXT("expression"), TEXT("The smaller of two values.") },
			{ TEXT("Max"), R::Function, TEXT("expression"), TEXT("The larger of two values.") },
			{ TEXT("Clamp"), R::Function, TEXT("expression"), TEXT("A value limited to lo..hi: Clamp(x, lo, hi).") },
			{ TEXT("ByteToInt"), R::Function, TEXT("expression"), TEXT("Widens a byte to an int.") },
			{ TEXT("ByteToInt64"), R::Function, TEXT("expression"), TEXT("Widens a byte to an int64.") },
			{ TEXT("IntToInt64"), R::Function, TEXT("expression"), TEXT("Widens an int to an int64.") },
			{ TEXT("ByteToReal"), R::Function, TEXT("expression"), TEXT("A byte as a double.") },
			{ TEXT("IntToReal"), R::Function, TEXT("expression"), TEXT("An int as a double.") },
			{ TEXT("Int64ToReal"), R::Function, TEXT("expression"), TEXT("An int64 as a double.") },
			{ TEXT("IsValid"), R::Function, TEXT("expression"), TEXT("For a gameplay tag, whether it is valid; for an object or class, whether it is not None.") },
			{ TEXT("Matches"), R::Function, TEXT("expression"), TEXT("Whether a tag matches a tag, or a container has it: Matches(t, u, Exact = true).") },
			{ TEXT("StateWeight"), R::Function, TEXT("expression"), TEXT("The blend weight of a state.") },
			{ TEXT("StateTime"), R::Function, TEXT("expression"), TEXT("The time spent in the machine's current state.") },
			{ TEXT("MachineWeight"), R::Function, TEXT("expression"), TEXT("The blend weight of a machine.") },
			{ TEXT("TimeRemaining"), R::Function, TEXT("expression"), TEXT("The time left on the most relevant asset player of a state.") },
			{ TEXT("TimeRemainingRatio"), R::Function, TEXT("expression"), TEXT("The same, as a fraction of its length.") },
		};

		const FDabWord AnimLayerInterfaceWords[] =
		{
			{ TEXT("AnimLayerInterface"), R::Document, TEXT("file"), TEXT("The document of an Animation Layer Interface (.dai).") },
			DAB_COMMON_WORDS,
			{ TEXT("Template"), R::Block, TEXT("document"), TEXT("A named expression, expanded where it is used.") },
			{ TEXT("Settings"), R::Section, TEXT("document"), TEXT("Parent = AnimLayerInterface;.") },
			{ TEXT("Layer"), R::Block, TEXT("document"), TEXT("A layer declaration: Layer Name [ meta ];.") },
			{ TEXT("Parent"), R::Setting, TEXT("Settings"), TEXT("AnimLayerInterface.") },
			{ TEXT("LayerGroup"), R::Metadata, TEXT("Layer"), TEXT("The layer's group.") },
		};

		const FDabWord AnimTemplatesWords[] =
		{
			{ TEXT("AnimTemplates"), R::Document, TEXT("file"), TEXT("A library of templates (.dat); it makes no asset.") },
			DAB_COMMON_WORDS,
			{ TEXT("Template"), R::Block, TEXT("document"), TEXT("A named expression, expanded where it is used.") },
		};

		const FDabWord ControlRigWords[] =
		{
			{ TEXT("ControlRig"), R::Document, TEXT("file"), TEXT("The document of a Control Rig (.dcr).") },
			DAB_COMMON_WORDS,
			{ TEXT("Settings"), R::Section, TEXT("document"), TEXT("Preview mesh, shape libraries, the rigs whose functions are called.") },
			{ TEXT("Module"), R::Section, TEXT("document"), TEXT("Module settings; makes the rig a module.") },
			{ TEXT("Hierarchy"), R::Section, TEXT("document"), TEXT("The static hierarchy.") },
			{ TEXT("Variables"), R::Section, TEXT("document"), TEXT("Member variables.") },
			{ TEXT("Event"), R::Block, TEXT("document"), TEXT("An event and its code: Event Name [ meta ] { ... }.") },
			{ TEXT("Function"), R::Block, TEXT("document"), TEXT("A function of the rig's library.") },
			{ TEXT("Detached"), R::Block, TEXT("document, code"), TEXT("Code that nothing runs.") },
			{ TEXT("Bone"), R::Block, TEXT("Hierarchy"), TEXT("A bone; also an element key, Bone(name).") },
			{ TEXT("Null"), R::Block, TEXT("Hierarchy"), TEXT("A null; also an element key, Null(name).") },
			{ TEXT("Control"), R::Block, TEXT("Hierarchy"), TEXT("A control; also an element key, Control(name).") },
			{ TEXT("Curve"), R::Block, TEXT("Hierarchy"), TEXT("A curve; also an element key, Curve(name).") },
			{ TEXT("Connector"), R::Block, TEXT("Hierarchy"), TEXT("A connector; also an element key, Connector(name).") },
			{ TEXT("Socket"), R::Block, TEXT("Hierarchy"), TEXT("A socket; also an element key, Socket(name).") },
			{ TEXT("Import"), R::Key, TEXT("Hierarchy"), TEXT("Fills the hierarchy with the bones and curves of a mesh.") },
			{ TEXT("Parent"), R::Key, TEXT("hierarchy element"), TEXT("The element's parent, an element key.") },
			{ TEXT("PreviewMesh"), R::Setting, TEXT("Settings"), TEXT("The skeletal mesh the editor previews the rig with.") },
			{ TEXT("ShapeLibraries"), R::Setting, TEXT("Settings"), TEXT("The shape libraries of the controls; left out, the default one; [] for none.") },
			{ TEXT("Uses"), R::Setting, TEXT("Settings"), TEXT("The rigs whose public functions this rig calls: [ Rig, Alias = Rig ].") },
			{ TEXT("Forward"), R::Key, TEXT("Event"), TEXT("Begin Execution: the forwards solve.") },
			{ TEXT("Backward"), R::Key, TEXT("Event"), TEXT("Inverse Execution: the backwards solve.") },
			{ TEXT("Construction"), R::Key, TEXT("Event"), TEXT("Prepare For Execution.") },
			{ TEXT("PostConstruction"), R::Key, TEXT("Event"), TEXT("Post Prepare For Execution.") },
			{ TEXT("PreForward"), R::Key, TEXT("Event"), TEXT("Pre Begin Execution.") },
			{ TEXT("PostForward"), R::Key, TEXT("Event"), TEXT("Post Begin Execution.") },
			{ TEXT("Interaction"), R::Key, TEXT("Event"), TEXT("Interaction Execution.") },
			{ TEXT("Connector"), R::Key, TEXT("Event"), TEXT("Connector Execution, which resolves a module's connectors.") },
			{ TEXT("Graph"), R::Metadata, TEXT("Event, Detached"), TEXT("The graph of the rig the code is in.") },
			{ TEXT("Public"), R::Metadata, TEXT("variable, Function"), TEXT("A public variable (a module's setting), or a function other rigs can call.") },
			{ TEXT("ReadOnly"), R::Metadata, TEXT("variable"), TEXT("The code cannot assign the variable.") },
			{ TEXT("Category"), R::Metadata, TEXT("variable, Function"), TEXT("The category in the editor.") },
			{ TEXT("Tooltip"), R::Metadata, TEXT("variable"), TEXT("The variable's tooltip.") },
			{ TEXT("Keywords"), R::Metadata, TEXT("Function"), TEXT("The function's keywords.") },
			{ TEXT("Description"), R::Metadata, TEXT("Function"), TEXT("The function's description.") },
			{ TEXT("Pure"), R::Metadata, TEXT("Function"), TEXT("A function written as code that callers see as a value.") },
			{ TEXT("Inline"), R::Metadata, TEXT("Function"), TEXT("The body of one collapse node, made where it is called.") },
			{ TEXT("let"), R::Statement, TEXT("code"), TEXT("Names a value: let Name = value;.") },
			{ TEXT("local"), R::Statement, TEXT("code"), TEXT("A local variable of the function: local Type Name [= constant];.") },
			{ TEXT("if"), R::Statement, TEXT("code"), TEXT("A Branch node: if (condition) { ... } else { ... }.") },
			{ TEXT("else"), R::Statement, TEXT("code"), TEXT("The False side of an if.") },
			{ TEXT("for"), R::Statement, TEXT("code"), TEXT("A loop: for (Element, Index in Array) { ... }, for (Index in Range(n)) { ... }.") },
			{ TEXT("in"), R::Statement, TEXT("for"), TEXT("Separates a loop's names from what it runs over.") },
			{ TEXT("return"), R::Statement, TEXT("code"), TEXT("A function's outputs: return (Output = value);.") },
			{ TEXT("Detached"), R::Statement, TEXT("code"), TEXT("Code that nothing runs, in a block of code.") },
			{ TEXT("Injected"), R::Builtin, TEXT("expression"), TEXT("A node injected on a pin: Pin = Injected(Node(...)).") },
			{ TEXT("Range"), R::Builtin, TEXT("for"), TEXT("A For Loop Count node: for (Index in Range(5)).") },
			{ TEXT("RunEvent"), R::Builtin, TEXT("code"), TEXT("Runs another event of the rig: RunEvent(Event);.") },
		};

		#undef DAB_COMMON_WORDS

		const FDabDocumentWords Documents[] =
		{
			{ EDabDocumentKind::AnimBlueprint, TEXT(".dab"), AnimBlueprintWords },
			{ EDabDocumentKind::AnimLayerInterface, TEXT(".dai"), AnimLayerInterfaceWords },
			{ EDabDocumentKind::AnimTemplates, TEXT(".dat"), AnimTemplatesWords },
			{ EDabDocumentKind::ControlRig, TEXT(".dcr"), ControlRigWords },
		};
	}

	const TCHAR* LexToString(EDabWordRole Role)
	{
		switch (Role)
		{
		case EDabWordRole::Document: return TEXT("document");
		case EDabWordRole::HeaderArgument: return TEXT("headerArgument");
		case EDabWordRole::Section: return TEXT("section");
		case EDabWordRole::Block: return TEXT("block");
		case EDabWordRole::Key: return TEXT("key");
		case EDabWordRole::Statement: return TEXT("statement");
		case EDabWordRole::Value: return TEXT("value");
		case EDabWordRole::Function: return TEXT("function");
		case EDabWordRole::Builtin: return TEXT("builtin");
		case EDabWordRole::Setting: return TEXT("setting");
		case EDabWordRole::Metadata: return TEXT("metadata");
		case EDabWordRole::Directive: return TEXT("directive");
		}
		return TEXT("");
	}

	TConstArrayView<FDabDocumentWords> GetDabDocumentWords()
	{
		return VocabularyPrivate::Documents;
	}

	TConstArrayView<FDabWord> GetDabWords(EDabDocumentKind Kind)
	{
		for (const FDabDocumentWords& Document : VocabularyPrivate::Documents)
		{
			if (Document.Kind == Kind)
			{
				return Document.Words;
			}
		}
		return {};
	}

	bool IsDabBlockWord(EDabDocumentKind Kind, FStringView Text)
	{
		for (const FDabWord& Word : GetDabWords(Kind))
		{
			if (Word.Role == EDabWordRole::Block && Text.Equals(Word.Text, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}
