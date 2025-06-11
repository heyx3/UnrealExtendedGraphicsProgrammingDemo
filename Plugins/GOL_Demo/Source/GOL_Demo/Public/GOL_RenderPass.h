#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpressionCustomOutput.h"

#include "EGP_CustomRenderPasses.h"

#include "GOL_RenderPass.generated.h"


//An instance of the Game of Life sim, running in one particular viewport.
struct GOL_DEMO_API FGameOfLifeView final : public F_EGP_ViewPersistentData
{
	static FRHITextureCreateDesc SimStateDesc(const FInt32Point& viewportSize);
	TRefCountPtr<FRHITexture2D> SimState, SimBuffer;
 
	float NextTickTime = 0;
	bool ReinitializeViews = false;
	
	FGameOfLifeView(FRDGBuilder& graph, const FViewInfo& view, const FIntRect& viewportSubset,
					const UMaterialInterface* initShaderMaterial,
					const FSceneTextureShaderParameters& sceneTextures);
	//Moves and destructor are handled automatically thanks to the ref-counted pointer.

	virtual void Resample(FRDGBuilder& graph, const FViewInfo& view,
						  const FInt32Point& oldResolution, const FInt32Point& newResolution,
						  const FInt32Point& offsetDelta) override;
};


UCLASS(BlueprintType)
class GOL_DEMO_API U_GOL_RenderPass : public U_EGP_RenderPass
{
	GENERATED_BODY()
public:

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	UMaterialInterface* EffectMaterial = nullptr;
	UMaterialInterface* GetEffectMaterial_RenderThread() const { check(IsInRenderingThread()); return effectMaterial_RenderThread; }

	T_EGP_PerViewData<FGameOfLifeView> PerViewData;

	UFUNCTION(BlueprintCallable)
	void ReInitializeAllViews();

protected:

	virtual TSharedRef<F_EGP_RenderPassSceneViewExtension> InitThisPass_GameThread(UWorld& thisWorld) override;
	virtual void Tick_GameThread(UWorld& thisWorld, float deltaSeconds) override;
	virtual void Tick_RenderThread(const FSceneInterface& thisScene, float gameThreadDeltaSeconds) override;

private:

	// ReSharper disable once CppUE4ProbableMemoryIssuesWithUObject
	UMaterialInterface* effectMaterial_RenderThread = nullptr;
};

struct GOL_DEMO_API F_GOL_PassSVE : public T_EGP_RenderPassSceneViewExtension<U_GOL_RenderPass>
{
	//Re-use the parent constructor:
	using T_EGP_RenderPassSceneViewExtension::T_EGP_RenderPassSceneViewExtension;

	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& graph, const FSceneView& view,
												 const FPostProcessingInputs& inputs) override;
};


#pragma region Custom Material Outputs

UCLASS(CollapseCategories, HideCategories=Object, DisplayName="GoL Outputs: Init")
class GOL_DEMO_API UMaterialExpressionGoLInitOutputs : public UMaterialExpressionCustomOutput
{
	GENERATED_BODY()
public:

	//Automatically snapped to 0 or 1.
	UPROPERTY(meta=(RequiredInput=true))
	FExpressionInput InitialBinaryState;

	//If not set, defaults to the binary state value (before it's snapped to 0 or 1).
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput InitialContinuousState;
	
	virtual FString GetFunctionName() const override { return TEXT("GoL_Outputs_Init_"); }
	virtual FString GetDisplayName() const override { return TEXT("GoL Outputs: Init"); }

	#if WITH_EDITOR
		virtual void GetCaption(TArray<FString>& output) const override { output.Add(TEXT("Game of Life Outputs: Init pass")); }
		virtual int32 GetNumOutputs() const override { return 2; }
		virtual EShaderFrequency GetShaderFrequency() override { return SF_Pixel; }
		virtual int32 Compile(class FMaterialCompiler*, int32 pinIdx) override;
	#endif
};

UCLASS(CollapseCategories, HideCategories=Object, DisplayName="GoL Outputs: Simulate (pt 1)")
class GOL_DEMO_API UMaterialExpressionGoLSimulate1Outputs : public UMaterialExpressionCustomOutput
{
	GENERATED_BODY()
public:

	UPROPERTY(meta=(RequiredInput=false), DisplayName="Threshold: Underpopulated")
	FExpressionInput ThresholdTooFew;
	UPROPERTY(meta=(OverridingInputProperty=ThresholdTooFew))
	float ThresholdTooFewConst = 2.0f;
	
	UPROPERTY(meta=(RequiredInput=false), DisplayName="Threshold: Ideal")
	FExpressionInput ThresholdResurrect;
	UPROPERTY(meta=(OverridingInputProperty=ThresholdResurrect))
	float ThresholdResurrectConst = 2.5f;
	
	UPROPERTY(meta=(RequiredInput=false), DisplayName="Threshold: Overpopulation")
	FExpressionInput ThresholdTooMany;
	UPROPERTY(meta=(OverridingInputProperty=ThresholdTooMany))
	float ThresholdTooManyConst = 3.0f;
	
	virtual FString GetFunctionName() const override { return TEXT("GoL_Outputs_Simulate_Pt1_"); }
	virtual FString GetDisplayName() const override { return TEXT("GoL Outputs: Simulate (part 1)"); }

	#if WITH_EDITOR
		virtual void GetCaption(TArray<FString>& output) const override { output.Add(TEXT("Game of Life Outputs: Simulate pass (part 1)")); }
		virtual int32 GetNumOutputs() const override { return 3; }
		virtual EShaderFrequency GetShaderFrequency() override { return SF_Compute; }
		virtual int32 Compile(class FMaterialCompiler*, int32 pinIdx) override;
	#endif
};

UCLASS(CollapseCategories, HideCategories=Object, DisplayName="GoL Outputs: Simulate (pt 2)")
class GOL_DEMO_API UMaterialExpressionGoLSimulate2Outputs : public UMaterialExpressionCustomOutput
{
	GENERATED_BODY()
public:

	//If not provided, the continuous state is set to match the discrete one.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput ContinuousValue;
	
	virtual FString GetFunctionName() const override { return TEXT("GoL_Outputs_Simulate_Pt2_"); }
	virtual FString GetDisplayName() const override { return TEXT("GoL Outputs: Simulate (part 2)"); }

#if WITH_EDITOR
	virtual void GetCaption(TArray<FString>& output) const override { output.Add(TEXT("Game of Life Outputs: Simulate pass (part 2)")); }
	virtual int32 GetNumOutputs() const override { return 1; }
	virtual EShaderFrequency GetShaderFrequency() override { return SF_Compute; }
	virtual int32 Compile(class FMaterialCompiler*, int32 pinIdx) override;
#endif
};

#pragma endregion