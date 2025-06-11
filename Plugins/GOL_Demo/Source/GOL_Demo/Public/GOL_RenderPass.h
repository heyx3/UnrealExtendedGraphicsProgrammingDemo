#pragma once

#include "CoreMinimal.h"

#include "EGP_CustomRenderPasses.h"

#include "GOL_RenderPass.generated.h"


UCLASS(BlueprintType)
class GOL_DEMO_API U_GOL_RenderPass : public U_EGP_RenderPass
{
	GENERATED_BODY()
public:

	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta=(InlineEditConditionToggle))
	bool ForceColor = false;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta=(EditCondition="ForceColor"))
	FLinearColor ForcedColor = { 1, 0, 1, 1 };

	TOptional<FLinearColor> GetClearColor_RenderThread() const
	{
		check(IsInRenderingThread());
		return color_renderThread;
	}

protected:

	virtual TSharedRef<F_EGP_RenderPassSceneViewExtension> InitThisPass_GameThread(UWorld& thisWorld) override;
	virtual void Tick_GameThread(UWorld& thisWorld, float deltaSeconds) override;

private:

	TOptional<FLinearColor> color_renderThread;
};

struct GOL_DEMO_API F_GOL_PassSVE : public T_EGP_RenderPassSceneViewExtension<U_GOL_RenderPass>
{
	//Re-use the parent constructor:
	using T_EGP_RenderPassSceneViewExtension::T_EGP_RenderPassSceneViewExtension;

	//Apply our effect immediately after the deferred pass
	//    and before lighting is computed.
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& graph, FSceneView& view,
		const FRenderTargetBindingSlots& renderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> sceneTextures
	) override;
};