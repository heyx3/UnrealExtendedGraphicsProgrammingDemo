#pragma once

#include "CoreMinimal.h"

#include "SnkeCustomRenderPasses.h"

#include "GOL_RenderPass.generated.h"


//An instance of the Game of Life sim, running in one particular viewport.
struct GOL_DEMO_API FGameOfLifeView final : public FSnkeViewPersistentData
{
	static FRHITextureCreateDesc SimStateDesc(const FInt32Point& viewportSize);
	TRefCountPtr<FRHITexture2D> SimState, SimBuffer;
	
	FGameOfLifeView(FRDGBuilder& graph, const FViewInfo& view, const FIntRect& viewportSubset,
					float seed, float noiseScale);
	//Moves and destructor are handled automatically thanks to the ref-counted pointer.

	virtual void Resample(FRDGBuilder& graph, const FViewInfo& view,
						  const FInt32Point& oldResolution, const FInt32Point& newResolution,
						  const FInt32Point& offsetDelta) override;
};

USTRUCT(BlueprintType)
struct GOL_DEMO_API FGameOfLifeSettings
{
	GENERATED_BODY()
public:

	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta=(ClampMin=0))
	float OverallSpeed = 2.0f;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta=(ClampMin=0))
	float MinSpeed = 0.3f;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta=(ClampMin=0))
	float AccelerationExponent = 1.0f;
};


UCLASS(BlueprintType)
class GOL_DEMO_API U_GOL_RenderPass : public USnkeRenderPass
{
	GENERATED_BODY()
public:

	//Seed that's used when initializing the sim for a new viewport.
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float NewViewportSeed = 1.432;
	
	//The size of the details on-screen in a new initialized viewport.
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float NewViewportNoiseScale = 10.0f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	FGameOfLifeSettings SimSettings;

	TSnkePerViewData<FGameOfLifeView> PerViewData;

protected:

	virtual TSharedRef<FSnkeRenderPassSceneViewExtension> InitThisPass_GameThread(UWorld& thisWorld) override;
	virtual void Tick_RenderThread(const FSceneInterface& thisScene, float gameThreadDeltaSeconds) override;

private:

	TOptional<FLinearColor> color_renderThread;
};

struct GOL_DEMO_API F_GOL_PassSVE : public TSnkeRenderPassSceneViewExtension<U_GOL_RenderPass>
{
	//Re-use the parent constructor:
	using TSnkeRenderPassSceneViewExtension::TSnkeRenderPassSceneViewExtension;

	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& graph, const FSceneView& view,
												 const FPostProcessingInputs& inputs) override;
};