#include "GOL_RenderPass.h"

#include "RenderGraphUtils.h"


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

#pragma endregion

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

#pragma region Resampling the sim when viewport resizes

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


TSharedRef<FSnkeRenderPassSceneViewExtension> U_GOL_RenderPass::InitThisPass(UWorld& thisWorld)
{
	//By default, only apply this pass to the first player-controller's view.
	ViewFilter->FilterByPlayerIdx(0);
	
	return FSceneViewExtensions::NewExtension<F_GOL_PassSVE>(this);
}

void U_GOL_RenderPass::Tick_GameThread(UWorld& thisWorld, float deltaSeconds)
{

}
void U_GOL_RenderPass::Tick_RenderThread(const FSceneInterface& thisScene, float gameThreadDeltaSeconds)
{
	PerViewData.Tick();
}


void F_GOL_PassSVE::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& graph, FSceneView& _view,
	const FRenderTargetBindingSlots& renderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> sceneTextures
) {
	check(_view.bIsViewInfo);
	auto& view = reinterpret_cast<FViewInfo&>(_view);
	
	auto sceneColor = renderTargets.Output[0].GetTexture();

	//Get or create the per-view data.
	auto& viewData = Pass->PerViewData.DataForView(
		graph, view,
		//If this is a new view and we are creating new data, these are its constructor arguments.
		//Note that there's minimal threat of race conditions here because floats are simple objects.
		Pass->NewViewportSeed, Pass->NewViewportNoiseScale
	);

	//DEBUG: Ping-pong the sim data between the two buffers so that the shaders are used every frame.
	{
		auto& dest = viewData.SimBuffer;
		auto newStateRDG = RegisterExternalTexture(graph, dest, TEXT("GoL_State_Debugg"));

		auto* params = graph.AllocParameters<FGoLInitializePS::FParameters>();
		params->Seed = 1.3344f;
		params->NoiseScale = 10;
		params->RenderTargets[0] = { newStateRDG, ERenderTargetLoadAction::ENoAction };;

		AddDrawScreenPass(
			graph, RDG_EVENT_NAME("GoL_Initialize"), view,
			FScreenPassTextureViewport{ newStateRDG },
			FScreenPassTextureViewport{ newStateRDG->Desc.Extent },
			TShaderMapRef<FGoLInitializePS>{ view.ShaderMap },
			params
		);
	}
	{
		auto& src = viewData.SimBuffer;
		auto& dest = viewData.SimState;
		
		auto oldStateRDG = RegisterExternalTexture(graph, src, TEXT("GoL_State_A")),
			 newStateRDG = RegisterExternalTexture(graph, dest, TEXT("GoL_State_B"));
		
		auto* params = graph.AllocParameters<FGoLResamplePS::FParameters>();
		params->InputTex = graph.CreateSRV(FRDGTextureSRVDesc{ oldStateRDG });
		params->InputSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
		params->RenderTargets[0] = { newStateRDG, ERenderTargetLoadAction::ENoAction };
		AddDrawScreenPass(
			graph, RDG_EVENT_NAME("GoL_Resample"), view,
			FScreenPassTextureViewport{ newStateRDG },
			FScreenPassTextureViewport{ oldStateRDG },
			TShaderMapRef<FGoLResamplePS>{ view.ShaderMap },
			params
		);
	}
}
