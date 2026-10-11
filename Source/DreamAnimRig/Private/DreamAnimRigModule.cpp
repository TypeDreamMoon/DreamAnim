// Copyright DreamMoon. All Rights Reserved.

#include "Driver/DabDocumentHandler.h"
#include "Modules/ModuleManager.h"
#include "Rig/DcrDocumentHandler.h"

namespace UE::DreamAnim::Rig
{
	/** Registers the ControlRig document kind with the driver, so `.dcr` sources compile and Control Rigs decompile. */
	class FDreamAnimRigModule final : public IModuleInterface
	{
	public:
		virtual void StartupModule() override
		{
			Compiler::RegisterDabDocumentHandler(Lang::EDabDocumentKind::ControlRig, MakeShared<FDcrDocumentHandler>());
		}

		virtual void ShutdownModule() override
		{
			Compiler::UnregisterDabDocumentHandler(Lang::EDabDocumentKind::ControlRig);
		}
	};
}

IMPLEMENT_MODULE(UE::DreamAnim::Rig::FDreamAnimRigModule, DreamAnimRig)
