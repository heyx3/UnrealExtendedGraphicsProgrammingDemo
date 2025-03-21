#include "GOL_RenderPass.h"

#include "RenderGraphUtils.h"
#include "Runtime/Renderer/Private/PostProcess/PostProcessing.h"


FRHITextureCreateDesc FGameOfLifeView::SimStateDesc(const FInt32Point& size)
{
	auto d = FRHITextureCreateDesc::Create2D(
		TEXT("GoL_State"),
		size,
		PF_R8
	);
	d.AddFlags(TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

	return d;
}

#pragma region Initialize the sim state for new viewports

struct FGoLInitializePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGoLInitializePS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, Seed)
		SHADER_PARAMETER(float, NoiseScale)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
	SHADER_USE_PARAMETER_STRUCT(FGoLInitializePS, FGlobalShader)
};

IMPLEMENT_GLOBAL_SHADER(FGoLInitializePS, "/GameOfLife/Init.usf", "Main", SF_Pixel);

FGameOfLifeView::FGameOfLifeView(FRDGBuilder& graph, const FViewInfo& view, const FIntRect& viewportSubset,
								 float seed, float noiseScale)
	: FSnkeViewPersistentData(graph, view, viewportSubset)
{
	auto desc = SimStateDesc(viewportSubset.Size());
	SimState = RHICreateTexture(desc);
	SimBuffer = RHICreateTexture(desc);

	//Initialize the sim state using our pixel shader.
	auto simStateRDG = RegisterExternalTexture(graph, SimState, TEXT("GoL_InitialState"));
	auto* initShaderParams = graph.AllocParameters<FGoLInitializePS::FParameters>();
	initShaderParams->Seed = seed;
	initShaderParams->NoiseScale = noiseScale;
	initShaderParams->RenderTargets[0] = { simStateRDG, ERenderTargetLoadAction::ENoAction };
	AddDrawScreenPass(
		graph, RDG_EVENT_NAME("GoL_Initialize"), view,
		FScreenPassTextureViewport{ simStateRDG },
		FScreenPassTextureViewport{ simStateRDG->Desc.Extent },
		TShaderMapRef<FGoLInitializePS>{ view.ShaderMap },
		initShaderParams
	);
}

#pragma endregion

#pragma region Resample the sim when viewport resizes

struct FGoLResamplePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGoLResamplePS);
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, InputTex)
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
	SimState = RHICreateTexture(SimStateDesc({ newResolution.X, newResolution.Y }));

	auto oldStateRDG = RegisterExternalTexture(graph, oldState, TEXT("Previous_GoL_State")),
		 newStateRDG = RegisterExternalTexture(graph, SimState, TEXT("Next_GoL_State")); 

	auto* params = graph.AllocParameters<FGoLResamplePS::FParameters>();
	params->InputTex = graph.CreateSRV(FRDGTextureSRVDesc{ oldStateRDG });
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

struct FGoLDisplayPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGoLDisplayPS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimState)
		SHADER_PARAMETER_SAMPLER(SamplerState, SimSampler)
		//To calculate UV's in 0-1 space rather than the viewport's sub-set of 0-1 space:
		SHADER_PARAMETER(FUint32Vector4, ViewportMinAndSize)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
	SHADER_USE_PARAMETER_STRUCT(FGoLDisplayPS, FGlobalShader)
};

IMPLEMENT_GLOBAL_SHADER(FGoLDisplayPS, "/GameOfLife/Display.usf", "Main", SF_Pixel);

static void RenderGoLState(FRDGBuilder& graph, const FViewInfo& view,
						   FRDGTextureRef simStateTex,
						   FRHIBlendState* blending, const FRenderTargetBinding& output)
{
	auto* params = graph.AllocParameters<FGoLDisplayPS::FParameters>();
	params->SimState = graph.CreateSRV(FRDGTextureSRVDesc{ simStateTex });
	params->SimSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
	params->ViewportMinAndSize = FUint32Vector4(
		view.ViewRect.Min.X, view.ViewRect.Min.Y,
		view.ViewRect.Width(), view.ViewRect.Height()
	);
	
	params->RenderTargets[0] = output;
	
	AddDrawScreenPass(
		graph, RDG_EVENT_NAME("GoL_Display"), view,
		FScreenPassTextureViewport{ output.GetTexture() },
		FScreenPassTextureViewport{ simStateTex },
		TShaderMapRef<FScreenPassVS>{ view.ShaderMap }, //The usual screen-pass VS
		TShaderMapRef<FGoLDisplayPS>{ view.ShaderMap },
		blending, //Custom blend mode
		params
	);
}

#pragma endregion


TSharedRef<FSnkeRenderPassSceneViewExtension> U_GOL_RenderPass::InitThisPass_GameThread(UWorld& thisWorld)
{
	//By default, only apply this pass to the first player-controller's view.
	//ViewFilter->FilterByPlayerIdx(0);
	
	return FSceneViewExtensions::NewExtension<F_GOL_PassSVE>(this);
}

void U_GOL_RenderPass::Tick_GameThread(UWorld& thisWorld, float deltaSeconds)
{

}
void U_GOL_RenderPass::Tick_RenderThread(const FSceneInterface& thisScene, float gameThreadDeltaSeconds)
{
	PerViewData.Tick();
}

void F_GOL_PassSVE::PrePostProcessPass_RenderThread(FRDGBuilder& graph, const FSceneView& _view,
 											  		const FPostProcessingInputs& inputs)
{
	check(_view.bIsViewInfo);
	auto& view = reinterpret_cast<const FViewInfo&>(_view);
	
	//Get or create the per-view data.
	auto& viewData = Pass->PerViewData.DataForView(
		graph, view,
		//If this is a new view and we are creating new data, these are its constructor arguments.
		//Note that there's minimal threat of race conditions here because floats are simple objects.
		Pass->NewViewportSeed, Pass->NewViewportNoiseScale
	);
	auto simStateRDG = RegisterExternalTexture(graph, viewData.SimState, TEXT("GoL_State"));

	//Draw onto the scene color texture.
	RenderGoLState(
		graph, view, simStateRDG,
		//Use multiplicative blending.
		TStaticBlendState<CW_RGBA, BO_Add, BF_DestColor, BF_Zero>::GetRHI(),
		//Blend on top of the current scene color, so make sure its existing contents are Loaded when bound.
		{ inputs.SceneTextures->GetContents()->SceneColorTexture, ERenderTargetLoadAction::ELoad }
	);
}
