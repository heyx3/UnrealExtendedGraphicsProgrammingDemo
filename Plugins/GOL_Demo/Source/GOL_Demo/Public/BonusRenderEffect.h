#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpressionCustomOutput.h"

#include "EGP_CustomRenderPasses.h"

#include "BonusRenderEffect.generated.h"


//Things defined for this effect will be abbreviated 'BRE' for Bonus Render Effect,
//    as opposed to 'GoL' for Game of Life.

USTRUCT(BlueprintType)
struct GOL_DEMO_API FBrePrimitiveSettings
{
	GENERATED_BODY()
public:

	//No settings for now.
};

UCLASS(meta=(BlueprintSpawnableComponent))
class GOL_DEMO_API UBreComponent : public U_EGP_RenderPassComponent
{
	GENERATED_BODY()
public:

	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta=(ShowOnlyInnerProperties))
	FBrePrimitiveSettings RenderSettings;

	virtual TSubclassOf<U_EGP_RenderPass> GetPassType() const override;
	EGP_PASS_COMPONENT_SIMPLE_PROXY_IMPL(FBrePrimitiveSettings, RenderSettings)
};


UCLASS(BlueprintType)
class GOL_DEMO_API UBreRenderPass : public U_EGP_RenderPass
{
	GENERATED_BODY()
public:

protected:

	virtual TSharedRef<F_EGP_RenderPassSceneViewExtension> InitThisPass_GameThread(UWorld& thisWorld) override;
};


UCLASS(CollapseCategories, HideCategories=Object, DisplayName="BRE Setup")
class GOL_DEMO_API UMaterialExpressionBreSetupOutputs : public UMaterialExpressionCustomOutput
{
	GENERATED_BODY()
public:

	//Defaults to the fragment's or camera's world position (see Details panel).
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput Start;
	//Defaults to the camera's forward vector.
	//Doesn't have to be normalized.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput Dir;
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput StepLength;
	
	//A cutoff to prevent bad performance or infinite loops.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput MaxLoops;
	
	//RGBA float values that get plugged into the first loop iteration.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput ThroughValue1;
	//RGBA float values that get plugged into the next first iteration.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput ThroughValue2;
	//RGBA float values that get plugged into the next first iteration.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput ThroughValue3;

	//If true, the default ray start position is the camera rather than the mesh surface.
	UPROPERTY(EditAnywhere)
	bool RayStartsBackAtCamera = false;
	
	UPROPERTY(EditAnywhere, meta=(OverridingInputProperty=MaxLoops))
	int MaxLoopsConst = 100;
	UPROPERTY(EditAnywhere, meta=(OverridingInputProperty=StepLength))
	float StepLengthConst = 1.0f;


	virtual FString GetFunctionName() const override { return TEXT("BRE_Outputs_Setup_"); }
	virtual FString GetDisplayName() const override { return TEXT("BRE Setup (pixel shader)"); }

	#if WITH_EDITOR
	virtual void GetCaption(TArray<FString>& output) const override { output.Add(TEXT("Bonus Render Effect Outputs: Setup (pixel shader)")); }
	virtual int32 GetNumOutputs() const override { return 7; }
	virtual EShaderFrequency GetShaderFrequency() override { return SF_Pixel; }
	virtual int32 Compile(class FMaterialCompiler*, int32 pinIdx) override;
	#endif
};
UCLASS(CollapseCategories, HideCategories=Object, DisplayName="BRE Loop")
class GOL_DEMO_API UMaterialExpressionBreLoopOutputs : public UMaterialExpressionCustomOutput
{
	GENERATED_BODY()
public:

	//A bool (number <0.5 for false, >0.5 for true) indicating whether to exit the loop here.
	UPROPERTY(meta=(RequiredInput=true))
	FExpressionInput ShouldExit;
	
	//An extra change to the position for the next loop; defaults to {0,0,0}.
	//Only applies if 'ShouldExit' is false.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput NextPosOffset;
	//Defaults to leaving the direction unchanged.
	//Only applies if 'ShouldExit' is false.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput NewDirNormalized;
	//Defaults to leaving the step length unchanged.
	//Only applies if 'ShouldExit' is false.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput NewStepLength;

	//RGBA float values that get plugged into the next loop iteration,
	//    and to the output after the loop exits.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput ThroughValue1;
	//RGBA float values that get plugged into the next loop iteration,
	//    and to the output after the loop exits.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput ThroughValue2;
	//RGBA float values that get plugged into the next loop iteration,
	//    and to the output after the loop exits.
	UPROPERTY(meta=(RequiredInput=false))
	FExpressionInput ThroughValue3;

	virtual FString GetFunctionName() const override { return TEXT("BRE_Outputs_Loop_"); }
	virtual FString GetDisplayName() const override { return TEXT("BRE Loop (pixel shader)"); }

	#if WITH_EDITOR
	virtual void GetCaption(TArray<FString>& output) const override { output.Add(TEXT("Bonus Render Effect Outputs: Loop (pixel shader)")); }
	virtual int32 GetNumOutputs() const override { return 7; }
	virtual EShaderFrequency GetShaderFrequency() override { return SF_Pixel; }
	virtual int32 Compile(class FMaterialCompiler*, int32 pinIdx) override;
	#endif
};
UCLASS(CollapseCategories, HideCategories=Object, DisplayName="BRE Render")
class GOL_DEMO_API UMaterialExpressionBreRenderOutputs : public UMaterialExpressionCustomOutput
{
	GENERATED_BODY()
public:

	UPROPERTY(meta=(RequiredInput=true))
	FExpressionInput EmissiveColor;
	//Any value < 0.5 means "invisible"; any value >= 0.5 means "visible".
	UPROPERTY(meta = (RequiredInput = true))
	FExpressionInput OpacityMask;

	virtual FString GetFunctionName() const override { return TEXT("BRE_Outputs_Render_"); }
	virtual FString GetDisplayName() const override { return TEXT("BRE Render (pixel shader)"); }

	#if WITH_EDITOR
	virtual void GetCaption(TArray<FString>& output) const override { output.Add(TEXT("Bonus Render Effect Outputs: Render (pixel shader)")); }
	virtual int32 GetNumOutputs() const override { return 2; }
	virtual EShaderFrequency GetShaderFrequency() override { return SF_Pixel; }
	virtual int32 Compile(class FMaterialCompiler*, int32 pinIdx) override;
	#endif
};