#include "GOL_RenderPass.h"

#include "MaterialCompiler.h"
#include "RenderGraphUtils.h"
#include "SnkePostProcessMaterialShaders.h"
#include "Runtime/Renderer/Private/PostProcess/PostProcessing.h"


FRHITextureCreateDesc FGameOfLifeView::SimStateDesc(const FInt32Point& viewportSize)
{
	//The sim looks pretty nice running at half-resolution;
	//    doing this also cuts the performance cost by 75%.
	auto texSize = viewportSize / 2;
	
	auto d = FRHITextureCreateDesc::Create2D(
		TEXT("GoL_State"),
		texSize,
		PF_R8G8 //Two-channel unorm, 8 bits per channel
	);
	d.AddFlags(TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

	return d;
}

#pragma region Initialize the sim state for new viewports

struct FGoLInitializePS : public Snke::FScreenSpaceShader
{
	DECLARE_EXPORTED_SHADER_TYPE(FGoLInitializePS, Material, )
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SNKE_SCREEN_SPACE_PASS_MATERIAL_DATA()
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
	SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FGoLInitializePS, Snke::FScreenSpaceShader)
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FGoLInitializePS, TEXT("/GameOfLife/Init.usf"), TEXT("Main"), SF_Pixel);

static void EnsureCompiledSimulateShader(ERHIFeatureLevel::Type featureLevel, const UMaterialInterface* uMaterial)
{
	FMaterialShaderTypes types;
	types.AddShaderType<Snke::FScreenSpaceRenderVS>();
	types.AddShaderType<FGoLInitializePS>();
	auto foundShaders = Snke::FindMaterialShaders_RenderThread(
		uMaterial, types,
		{ MD_PostProcess, featureLevel }
	);
	check(foundShaders);

	//Extract the shader and material proxy.
	auto* materialProxy = foundShaders->MaterialProxy;
	auto* materialF = foundShaders->Material;
	TShaderRef<Snke::FScreenSpaceRenderVS> shaderV;
	TShaderRef<FGoLInitializePS> shaderP;
	ensure(foundShaders->Shaders.TryGetVertexShader(shaderV));
	ensure(foundShaders->Shaders.TryGetPixelShader(shaderP));
}

static void InitGoLState(FRDGBuilder& graph, FRDGTextureRef simStateTex,
					     const FViewInfo& view, const FSceneTextureShaderParameters& sceneTextures,
					     const UMaterialInterface* uMaterial)
{
	auto* initShaderParams = graph.AllocParameters<FGoLInitializePS::FParameters>();
	initShaderParams->RenderTargets[0] = { simStateTex, ERenderTargetLoadAction::ENoAction };
	
	Snke::FScreenSpacePassMaterialInputs postProcessMaterialInputs;
	postProcessMaterialInputs.SceneTextures = sceneTextures;
	postProcessMaterialInputs.TargetView = &view;
	postProcessMaterialInputs.OutputViewportData = FScreenPassTextureViewport{ simStateTex };
	postProcessMaterialInputs.InputViewportData = FScreenPassTextureViewport{ view.ViewRect };

	Snke::AddScreenSpaceRenderPass<Snke::FScreenSpaceRenderVS, FGoLInitializePS>(
		graph, RDG_EVENT_NAME("GoL_Initialize"),
		postProcessMaterialInputs,
		Snke::FScreenSpacePassRenderState{ }, //Default to opaque blending and no depth/stencil usage
		initShaderParams, uMaterial,
		//Extract the specific param structs for each shader:
		&initShaderParams->ScreenSpacePassData, initShaderParams
	);
}

FGameOfLifeView::FGameOfLifeView(FRDGBuilder& graph, const FViewInfo& view, const FIntRect& viewportSubset,
							     const UMaterialInterface* initShaderMaterial,
							     const FSceneTextureShaderParameters& sceneTextures)
	: FSnkeViewPersistentData(graph, view, viewportSubset)
{
	auto desc = SimStateDesc(viewportSubset.Size());
	SimState = RHICreateTexture(desc);
	SimBuffer = RHICreateTexture(desc);

	auto simStateRDG = RegisterExternalTexture(graph, SimState, TEXT("GoL_InitialState"));
	InitGoLState(graph, simStateRDG, view, sceneTextures, initShaderMaterial);
}

#pragma endregion

#pragma region Resample the sim when viewport resizes

struct FGoLResamplePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGoLResamplePS);
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, InputTex)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
	SHADER_USE_PARAMETER_STRUCT(FGoLResamplePS, FGlobalShader)
};

IMPLEMENT_GLOBAL_SHADER(FGoLResamplePS, "/GameOfLife/Resample.usf", "Main", SF_Pixel);

void FGameOfLifeView::Resample(FRDGBuilder& graph, const FViewInfo& view,
							   const FInt32Point& oldResolution, const FInt32Point& newResolution,
							   const FInt32Point& offsetDelta)
{
	//The sim state texture doesn't share viewport space like viewport render-targets do,
	//    so we don't care about position changes -- only resolution changes.
	if (oldResolution == newResolution)
		return;

	auto oldState = SimState;
	auto newDesc = SimStateDesc({ newResolution.X, newResolution.Y });
	SimState = RHICreateTexture(newDesc);
	SimBuffer = RHICreateTexture(newDesc);

	auto oldStateRDG = RegisterExternalTexture(graph, oldState, TEXT("Previous_GoL_State")),
		 newStateRDG = RegisterExternalTexture(graph, SimState, TEXT("Next_GoL_State")); 

	auto* params = graph.AllocParameters<FGoLResamplePS::FParameters>();
	params->InputTex = graph.CreateSRV(FRDGTextureSRVDesc{ oldStateRDG });
	//If the resolution change is less than 0.5x we will lose data;
	//    fortunately that's not a big deal for demo purposes.
	params->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
	params->RenderTargets[0] = { newStateRDG, ERenderTargetLoadAction::ENoAction };

	//Unreal's "draw screen pass" will by default cover the whole screen using opaque blending
	//    and use their own trivial Vertex Shader.
	//This is perfect for our use-case.
	AddDrawScreenPass(
		graph, RDG_EVENT_NAME("GoL_Resample"), view,
		FScreenPassTextureViewport{ newStateRDG },
		FScreenPassTextureViewport{ oldStateRDG },
		TShaderMapRef<FGoLResamplePS>{ view.ShaderMap },
		params
	);
}

#pragma endregion

#pragma region Display the sim state as a post-process

struct FGoLDisplayPS : public Snke::FScreenSpaceShader
{
	DECLARE_EXPORTED_SHADER_TYPE(FGoLDisplayPS, Material, )

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SNKE_SCREEN_SPACE_PASS_MATERIAL_DATA()
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
	SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FGoLDisplayPS, Snke::FScreenSpaceShader)
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FGoLDisplayPS, TEXT("/GameOfLife/Display.usf"), TEXT("Main"), SF_Pixel);

static void RenderGoLState(FRDGBuilder& graph, const FViewInfo& view,
						   FRDGTextureRef simStateTex,
						   FRHIBlendState* blending,
						   const FRenderTargetBinding& output,
						   const UMaterialInterface* material,
						   const FSceneTextureShaderParameters& sceneTextures)
{
	auto* params = graph.AllocParameters<FGoLDisplayPS::FParameters>();
	params->RenderTargets[0] = output;

	//Configure the standard post-process Material inputs for this pass:
	Snke::FScreenSpacePassMaterialInputs inputs;
	inputs.Textures[0] = GetScreenPassTextureInput(
		FScreenPassTexture{ simStateTex },
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI()
	);
	inputs.SceneTextures = sceneTextures;
	inputs.TargetView = &view;
	inputs.InputViewportData = FScreenPassTextureViewport{ inputs.Textures[0].Texture };
	inputs.OutputViewportData = FScreenPassTextureViewport{ output.GetTexture(), view.ViewRect };
	
	Snke::AddScreenSpaceRenderPass<Snke::FScreenSpaceRenderVS, FGoLDisplayPS>(
		graph, RDG_EVENT_NAME("GoL_Display"), inputs,
		Snke::FScreenSpacePassRenderState{ blending },
		params, material,
		//Extract the specific param structs for each shader:
		&params->ScreenSpacePassData, params
	);
}

#pragma endregion

#pragma region Tick the sim state

struct FGoLSimulateCS : public Snke::FSimulationShader
{
	DECLARE_EXPORTED_SHADER_TYPE(FGoLSimulateCS, Material, );
	static FIntVector3 GroupSize() { return { 8, 8, 1 }; }

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, DeltaSeconds)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, NextSimStateTex)
		SNKE_SIMULATION_PASS_MATERIAL_DATA()
	END_SHADER_PARAMETER_STRUCT()
	SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FGoLSimulateCS, Snke::FSimulationShader)

	//Feed the group size to the shader so that it's only defined in one place.
	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& params,
											 FShaderCompilerEnvironment& env)
	{
		Snke::FSimulationShader::ModifyCompilationEnvironment(params, env);
		env.SetDefine(TEXT("SIM_GROUP_SIZE_X"), GroupSize().X);
		env.SetDefine(TEXT("SIM_GROUP_SIZE_Y"), GroupSize().Y);
		env.SetDefine(TEXT("SIM_GROUP_SIZE_Z"), GroupSize().Z);
	}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FGoLSimulateCS, TEXT("/GameOfLife/Simulate.usf"), TEXT("Main"), SF_Compute);

static void UpdateGoLState(FRDGBuilder& graph, const FViewInfo& view,
						   FRDGTextureRef currentSimState, FRDGTextureRef nextSimState,
						   float deltaSeconds,
						   const UMaterialInterface* uMaterial)
{
	check(currentSimState->Desc.Extent == nextSimState->Desc.Extent);
	
	//Provide the previous state to the material graph as Post-Process Texture 0.
	Snke::FSimulationPassMaterialInputs inputs;
	inputs.Textures[0] = GetScreenPassTextureInput(
		FScreenPassTexture{ currentSimState },
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI()
	);

	//Set up the other, non-Material shader params.
	auto* params = graph.AllocParameters<FGoLSimulateCS::FParameters>();
	params->DeltaSeconds = deltaSeconds;
	params->NextSimStateTex = graph.CreateUAV(nextSimState);

	//Compute the group count for this dispatch.
	Snke::FSimulationPassState state;
	state.GroupCount.Set<FIntVector3>(FComputeShaderUtils::GetGroupCount(
		FIntVector3{ currentSimState->Desc.Extent.X, currentSimState->Desc.Extent.Y, 1 },
		FGoLSimulateCS::GroupSize()
	));

	Snke::AddSimulationMaterialPass<FGoLSimulateCS>(graph, RDG_EVENT_NAME("GoL_Tick"),
												    inputs, state, view,
												    params, uMaterial);
}

#pragma endregion

TSharedRef<FSnkeRenderPassSceneViewExtension> U_GOL_RenderPass::InitThisPass_GameThread(UWorld& thisWorld)
{
	return FSceneViewExtensions::NewExtension<F_GOL_PassSVE>(this);
}
void U_GOL_RenderPass::ReInitializeAllViews()
{
	auto* viewData = &PerViewData;
	ENQUEUE_RENDER_COMMAND(QueueGoLReInitialization)([viewData](FRHICommandListImmediate& cmds)
	{
		viewData->ForEachView([&](int viewID, FGameOfLifeView& data, ERHIFeatureLevel::Type featureLevel)
		{
			data.ReinitializeViews = true;
		});
	});
}
void U_GOL_RenderPass::Tick_GameThread(UWorld& thisWorld, float deltaSeconds)
{
	Super::Tick_GameThread(thisWorld, deltaSeconds);

	//Update render-thread copies of our parameters.
	auto* matIn = EffectMaterial;
	auto* matOut = &effectMaterial_RenderThread;
	ENQUEUE_RENDER_COMMAND(UpdateGoLParams)([matIn, matOut](FRHICommandList& cmds)
	{
		*matOut = matIn;
	});
}
void U_GOL_RenderPass::Tick_RenderThread(const FSceneInterface& thisScene, float gameThreadDeltaSeconds)
{
	if (effectMaterial_RenderThread)
		EnsureCompiledSimulateShader(thisScene.GetFeatureLevel(), effectMaterial_RenderThread);
	
	Super::Tick_RenderThread(thisScene, gameThreadDeltaSeconds);
	PerViewData.Tick();

	//Update the delta-time for each viewport's next tick.
	PerViewData.ForEachView([&](int viewID, FGameOfLifeView& view, ERHIFeatureLevel::Type featureLevel) {
		view.NextTickTime += gameThreadDeltaSeconds;
	});
}

void F_GOL_PassSVE::PrePostProcessPass_RenderThread(FRDGBuilder& graph, const FSceneView& _view,
 											  		const FPostProcessingInputs& inputs)
{
	check(_view.bIsViewInfo);
	auto& view = reinterpret_cast<const FViewInfo&>(_view);
	auto* passMaterial = Pass->GetEffectMaterial_RenderThread();
	
	//Get or create the per-view data.
	auto& viewData = Pass->PerViewData.DataForView(
		graph, view,
		//For new views, the view data constructor arguments:
		passMaterial, GetSceneTextureShaderParameters(inputs.SceneTextures)
	);
	auto simStateRDG = RegisterExternalTexture(graph, viewData.SimState, TEXT("GoL_State"));

	//If re-initialization was requested, do that first.
	if (viewData.ReinitializeViews)
	{
		InitGoLState(graph, simStateRDG, view,
				     GetSceneTextureShaderParameters(inputs.SceneTextures),
				     passMaterial);
		viewData.ReinitializeViews = false;
	}
	//If some time has passed on the game thread, tick this viewport's sim.
	if (viewData.NextTickTime > 0)
	{
		auto nextSimStateRDG = RegisterExternalTexture(graph, viewData.SimBuffer, TEXT("GoL_NextState"));
		UpdateGoLState(
			graph, view,
			simStateRDG, nextSimStateRDG,
			viewData.NextTickTime, passMaterial
		);
		std::swap(viewData.SimBuffer, viewData.SimState);
		simStateRDG = nextSimStateRDG;
		viewData.NextTickTime = 0;
	}
	
	//Draw onto the scene color texture.
	RenderGoLState(
		graph, view, simStateRDG,
		//Use multiplicative blending.
		TStaticBlendState<CW_RGBA, BO_Add, BF_DestColor, BF_Zero>::GetRHI(),
		//Blend on top of the current scene color, so make sure its existing contents are Loaded when bound.
		{ inputs.SceneTextures->GetContents()->SceneColorTexture, ERenderTargetLoadAction::ELoad },
		passMaterial,
		GetSceneTextureShaderParameters(inputs.SceneTextures)
	);
}

#if WITH_EDITOR
int32 UMaterialExpressionGoLInitOutputs::Compile(FMaterialCompiler* compiler, int32 pinIdx)
{
	int32 codeID;
	if (pinIdx == 0)
	{
		codeID = InitialBinaryState.IsConnected() ?
				     InitialBinaryState.Compile(compiler) :
					 INDEX_NONE;
	}
	else if (pinIdx == 1)
	{
		codeID = InitialContinuousState.IsConnected() ?
				     InitialContinuousState.Compile(compiler) :
					 INDEX_NONE;
	}
	else
	{
		codeID = INDEX_NONE;
	}
	return compiler->CustomOutput(this, pinIdx, codeID);
}
int32 UMaterialExpressionGoLSimulate1Outputs::Compile(FMaterialCompiler* compiler, int32 pinIdx)
{
	int32 codeID;
	auto doPin = [&](int32 i, FExpressionInput& pin, float* fallback)
	{
		if (pinIdx != i)
			return false;
		
		if (pin.IsConnected())
			codeID = pin.Compile(compiler);
		else if (fallback)
			codeID = compiler->Constant(*fallback);
		else
			codeID = INDEX_NONE;
		return true;
	};

	if (!doPin(0, ThresholdTooFew, &ThresholdTooFewConst) &&
		!doPin(1, ThresholdResurrect, &ThresholdResurrectConst) &&
		!doPin(2, ThresholdTooMany, &ThresholdTooManyConst))
	{
		codeID = INDEX_NONE;
	}
	return compiler->CustomOutput(this, pinIdx, codeID);
}
int32 UMaterialExpressionGoLSimulate2Outputs::Compile(FMaterialCompiler* compiler, int32 pinIdx)
{
	int32 codeID;
	auto doPin = [&](int32 i, FExpressionInput& pin, float* fallback)
	{
		if (pinIdx != i)
			return false;
		
		if (pin.IsConnected())
			codeID = pin.Compile(compiler);
		else if (fallback)
			codeID = compiler->Constant(*fallback);
		else
			codeID = INDEX_NONE;
		return true;
	};

	if (!doPin(0, ContinuousValue, nullptr))
	{
		codeID = INDEX_NONE;
	}
	return compiler->CustomOutput(this, pinIdx, codeID);
}
#endif