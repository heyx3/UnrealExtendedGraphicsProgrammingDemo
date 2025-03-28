#pragma once

#include "CoreMinimal.h"

#include "SnkeCustomRenderPasses.h"

#include "GOL_RenderPass.generated.h"


//An instance of the Game of Life sim, running in one particular viewport.
struct GOL_DEMO_API FGameOfLifeView final : public FSnkeViewPersistentData
{
	static FRHITextureCreateDesc SimStateDesc(const FInt32Point& viewportSize);
	TRefCountPtr<FRHITexture2D> SimState, SimBuffer;
 
	float NextTickTime = 0;
	
	FGameOfLifeView(FRDGBuilder& graph, const FViewInfo& view, const FIntRect& viewportSubset,
					const UMaterialInterface* initShaderMaterial,
					const FSceneTextureShaderParameters& sceneTextures);
	//Moves and destructor are handled automatically thanks to the ref-counted pointer.

	virtual void Resample(FRDGBuilder& graph, const FViewInfo& view,
						  const FInt32Point& oldResolution, const FInt32Point& newResolution,
						  const FInt32Point& offsetDelta) override;
};


UCLASS(BlueprintType)
class GOL_DEMO_API U_GOL_RenderPass : public USnkeRenderPass
{
	GENERATED_BODY()
public:

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	UMaterialInterface* EffectMaterial = nullptr;
	UMaterialInterface* GetEffectMaterial_RenderThread() const { check(IsInRenderingThread()); return effectMaterial_RenderThread; }

	TSnkePerViewData<FGameOfLifeView> PerViewData;

protected:

	virtual TSharedRef<FSnkeRenderPassSceneViewExtension> InitThisPass_GameThread(UWorld& thisWorld) override;
	virtual void Tick_GameThread(UWorld& thisWorld, float deltaSeconds) override;
	virtual void Tick_RenderThread(const FSceneInterface& thisScene, float gameThreadDeltaSeconds) override;

private:

	// ReSharper disable once CppUE4ProbableMemoryIssuesWithUObject
	UMaterialInterface* effectMaterial_RenderThread = nullptr;
};

struct GOL_DEMO_API F_GOL_PassSVE : public TSnkeRenderPassSceneViewExtension<U_GOL_RenderPass>
{
	//Re-use the parent constructor:
	using TSnkeRenderPassSceneViewExtension::TSnkeRenderPassSceneViewExtension;

	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& graph, const FSceneView& view,
												 const FPostProcessingInputs& inputs) override;
};