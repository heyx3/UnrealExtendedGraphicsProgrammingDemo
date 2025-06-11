#include "GOL_RenderPass.h"

#include "RenderGraphUtils.h"

void U_GOL_RenderPass::Tick_GameThread(UWorld& thisWorld, float deltaSeconds)
{
	//Pick the new clear color (or null to not clear this frame).
	TOptional<FLinearColor> newClearColor;
	if (ForceColor || FMath::FRand() < 0.1f)
	{
		newClearColor = (ForceColor ?
							ForcedColor :
							FLinearColor{ FMath::FRand(), FMath::FRand(), FMath::FRand() });
	}
	
	//Copy the color and field pointer into a lambda and send that lambda to the render thread.
	//This prevents any race conditions between us and the SVE.
	auto* outputPtr = &color_renderThread;
	auto updateClearColor = [outputPtr, newClearColor](FRHICommandListImmediate&) {
		*outputPtr = newClearColor;
	};
	ENQUEUE_RENDER_COMMAND(UpdateClearColor)(MoveTemp(updateClearColor));
}
TSharedRef<F_EGP_RenderPassSceneViewExtension> U_GOL_RenderPass::InitThisPass_GameThread(UWorld& thisWorld)
{
	//By default, only apply this pass to the first player-controller's view.
	ViewFilter->FilterByPlayerIdx(0);
	
	return FSceneViewExtensions::NewExtension<F_GOL_PassSVE>(this);
}

void F_GOL_PassSVE::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& graph, FSceneView& view,
	const FRenderTargetBindingSlots& renderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> sceneTextures
) {
	auto tryClearColor = Pass->GetClearColor_RenderThread();
	auto sceneColor = renderTargets.Output[0].GetTexture();

	if (tryClearColor.IsSet() && sceneColor != nullptr)
		AddClearRenderTargetPass(graph, sceneColor, *tryClearColor);
}
