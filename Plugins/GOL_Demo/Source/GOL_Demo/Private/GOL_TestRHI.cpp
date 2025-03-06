#include "GOL_TestRHI.h"

#include "ClearQuad.h"
#include "Kismet/KismetRenderingLibrary.h"


void A_GOL_TestRHI::BeginPlay()
{
	Super::BeginPlay();

	const int Resolution = 16;
	TestOutput = NewObject<UTextureRenderTarget2D>(this);
	TestOutput->ClearColor = FLinearColor::Transparent;
	TestOutput->InitCustomFormat(Resolution, Resolution, PF_R8G8B8A8, true);
}
void A_GOL_TestRHI::Tick(float deltaSeconds)
{
	Super::Tick(deltaSeconds);

	auto* resource = TestOutput->GetResource();
	auto* rhi = (resource == nullptr) ? nullptr : resource->GetTexture2DRHI();
	
	auto randF = []{ return FMath::FRandRange(0.0f, 1.0f); };
	if (rhi && randF() < 0.09f)	
	{
		FLinearColor clearColor{ randF(), randF(), randF(), 1 };

		ENQUEUE_RENDER_COMMAND(RandomizedClear)([rhi, clearColor](FRHICommandListImmediate& cmds) {
			//NOTE: Clearing textures is a lot easier to use with RDG; this is just an example of how to do it at the RHI level.
			
			//Start using the texture as a render target.
			cmds.Transition(FRHITransitionInfo{ rhi, ERHIAccess::Unknown, ERHIAccess::RTV });
			cmds.BeginRenderPass(FRHIRenderPassInfo{ rhi, ERenderTargetActions::DontLoad_Store },
							     TEXT("RandomizedClear"));

			//Clear it to a random color.
			DrawClearQuad(
				cmds, true, clearColor,
				//No depth/stencil.
				false, 0, false, 0
			);

			//Clean up.
			cmds.EndRenderPass();
			cmds.Transition(FRHITransitionInfo{ rhi, ERHIAccess::RTV, ERHIAccess::SRVMask });
		});
	}
}
