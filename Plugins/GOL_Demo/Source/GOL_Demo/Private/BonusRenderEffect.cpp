#include "BonusRenderEffect.h"

#include "MaterialCompiler.h"
#include "SimpleMeshDrawCommandPass.h"
#include "MaterialDomain.h"
#include "Runtime/Renderer/Private/PostProcess/PostProcessing.h"
#include "Runtime/Renderer/Public/MeshPassProcessor.inl"

#include "EGP_GetMeshBatches.h"


class FBreMeshVS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FBreMeshVS, MeshMaterial);
	
    static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& params)
    {
        return params.MaterialParameters.MaterialDomain == MD_Surface &&
               FMeshMaterialShader::ShouldCompilePermutation(params);
    }

	FBreMeshVS() = default;
	FBreMeshVS(const ShaderMetaType::CompiledShaderInitializerType& initializer)
		: FMeshMaterialShader(initializer)
	{
	}
};
class FBreMeshPS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FBreMeshPS, MeshMaterial);
	
    static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& params)
    {
        return params.MaterialParameters.MaterialDomain == MD_Surface &&
               FMeshMaterialShader::ShouldCompilePermutation(params);
    }

	FBreMeshPS() = default;
	FBreMeshPS(const ShaderMetaType::CompiledShaderInitializerType& initializer)
		: FMeshMaterialShader(initializer)
	{
	}
};
IMPLEMENT_MATERIAL_SHADER_TYPE(, FBreMeshVS, TEXT("/GameOfLife/BonusRenderEffect.usf"), TEXT("MainVS"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(, FBreMeshPS, TEXT("/GameOfLife/BonusRenderEffect.usf"), TEXT("MainPS"), SF_Pixel);

#if WITH_EDITOR
int32 UMaterialExpressionBreSetupOutputs::Compile(FMaterialCompiler* compiler, int32 pinIdx)
{
	int32 codeID;
	auto doPin = [&]<typename N>(int32 i, FExpressionInput & pin, N * fallback)
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
	auto* nullFloat = static_cast<float*>(nullptr);

	if (pinIdx == 0)
		if (Start.IsConnected())
			codeID = Start.Compile(compiler);
		else if (RayStartsBackAtCamera)
			codeID = compiler->ViewProperty(EMaterialExposedViewProperty::MEVP_WorldSpaceCameraPosition);
		else
			codeID = compiler->WorldPosition(EWorldPositionIncludedOffsets::WPT_Default);
	else if (pinIdx == 1)
		if (Dir.IsConnected())
			codeID = Dir.Compile(compiler);
		else
			codeID = compiler->Sub(compiler->Constant(0.0f), compiler->CameraVector());
	else if (!doPin(0, Start, nullFloat) &&
			 !doPin(1, Dir, nullFloat) &&
			 !doPin(2, StepLength, &StepLengthConst) &&
			 !doPin(3, MaxLoops, &MaxLoopsConst) &&
			 !doPin(4, ThroughValue1, nullFloat) &&
			 !doPin(5, ThroughValue2, nullFloat) &&
			 !doPin(6, ThroughValue3, nullFloat))
		codeID = INDEX_NONE;

	return compiler->CustomOutput(this, pinIdx, codeID);
}
int32 UMaterialExpressionBreLoopOutputs::Compile(FMaterialCompiler* compiler, int32 pinIdx)
{
	int32 codeID;
	auto doPin = [&]<typename N>(int32 i, FExpressionInput& pin, N* fallback)
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

	auto* nullFloat = static_cast<float*>(nullptr);
	if (!doPin(0, ShouldExit, nullFloat) &&
		  !doPin(1, NextPosOffset, nullFloat) &&
		  !doPin(2, NewDirNormalized, nullFloat) &&
		  !doPin(3, NewStepLength, nullFloat) &&
		  !doPin(4, ThroughValue1, nullFloat) &&
		  !doPin(5, ThroughValue2, nullFloat) &&
		  !doPin(6, ThroughValue3, nullFloat))
	{
		codeID = INDEX_NONE;
	}
	return compiler->CustomOutput(this, pinIdx, codeID);
}
int32 UMaterialExpressionBreRenderOutputs::Compile(FMaterialCompiler* compiler, int32 pinIdx)
{
	int32 codeID;
	auto doPin = [&]<typename N>(int32 i, FExpressionInput & pin, N * fallback)
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
	auto* nullFloat = static_cast<float*>(nullptr);

	if (!doPin(0, EmissiveColor, nullFloat) &&
		!doPin(1, OpacityMask, nullFloat))
	{
		codeID = INDEX_NONE;
	}

	return compiler->CustomOutput(this, pinIdx, codeID);
}
#endif


class FBreMeshProcessor final : public FMeshPassProcessor
{
public:

    FMeshPassProcessorRenderState PassDrawState;

    FBreMeshProcessor(const FScene* scene, const FSceneView* view,
                      ERHIFeatureLevel::Type featureLevel,
                      FMeshPassDrawListContext* commandsOutput)
        : FMeshPassProcessor(
		      #if ENGINE_MINOR_VERSION > 3
        	      TEXT("BonusRenderEffect"),
        	  #endif
        	  scene, featureLevel, view, commandsOutput
          )
    {
        PassDrawState.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero>::GetRHI());
        PassDrawState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
    }

    virtual void AddMeshBatch(const FMeshBatch& batch, uint64 batchElementMask,
							  const FPrimitiveSceneProxy* proxy, int32 staticMeshID) override
    {
        //Get the first usable Material in the chain,
        //    starting at the batch's desired Material and ending at the Default Material.
        const FMaterialRenderProxy* fallbackMat = nullptr;
        const auto& resource = batch.MaterialRenderProxy->GetMaterialWithFallback(
            FeatureLevel, fallbackMat
        );
        const auto& materialProxy = fallbackMat ? *fallbackMat : *batch.MaterialRenderProxy;
        
        //Compile our shaders against the batch's Material and Vertex-Factory.
        TMeshProcessorShaders<FBreMeshVS, FBreMeshPS> shaderRefs;
        {
            FMaterialShaderTypes shaderTypes;
            shaderTypes.AddShaderType<FBreMeshVS>();
            shaderTypes.AddShaderType<FBreMeshPS>();

            FMaterialShaders materialShaders;
            verify(resource.TryGetShaders(shaderTypes, batch.VertexFactory->GetType(), materialShaders));

            materialShaders.TryGetVertexShader(shaderRefs.VertexShader);
            materialShaders.TryGetPixelShader(shaderRefs.PixelShader);
        }

        //Configure per-element settings.
        FMeshMaterialShaderElementData elementData;
        elementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, proxy, batch, staticMeshID, false);

        //Generate the draw calls for this batch.
        const FMeshDrawingPolicyOverrideSettings overrides = ComputeMeshOverrideSettings(batch);
		auto cullMode = ComputeMeshCullMode(resource, overrides);
		if (cullMode == ERasterizerCullMode::CM_None) //The outer shell can be two-sided, but not this pass
			cullMode = ERasterizerCullMode::CM_CW;
        BuildMeshDrawCommands(
            batch, batchElementMask, proxy,
            materialProxy, resource, PassDrawState,
            MoveTemp(shaderRefs),
            ComputeMeshFillMode(resource, overrides), cullMode,
            FMeshDrawCommandSortKey::Default, EMeshPassFeatures::Default,
            elementData
        );
    }
};

BEGIN_SHADER_PARAMETER_STRUCT(FBrePassParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()
struct FBrePassSVE : public T_EGP_RenderPassSceneViewExtension<UBreRenderPass,
								 							   UBreComponent, FBrePrimitiveSettings>
{
	using T_EGP_RenderPassSceneViewExtension::T_EGP_RenderPassSceneViewExtension;

	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& graph, const FSceneView& _view,
												 const FPostProcessingInputs& postInputs) override
	{
		check(_view.bIsViewInfo);
		auto& view = reinterpret_cast<const FViewInfo&>(_view);

		FScene* renderScene = nullptr;
		if (view.Family != nullptr && view.Family->Scene != nullptr)
			renderScene = view.Family->Scene->GetRenderScene();

		auto* passParams = graph.AllocParameters<FBrePassParameters>();
		passParams->View = view.ViewUniformBuffer;
		passParams->Scene = GetSceneUniformBufferRef(graph, view);
		passParams->RenderTargets[0] = {
			postInputs.SceneTextures->GetContents()->SceneColorTexture,
			ERenderTargetLoadAction::ELoad
		};
		passParams->RenderTargets.DepthStencil = {
			postInputs.SceneTextures->GetContents()->SceneDepthTexture,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilNop
		};

		FIntRect viewport{
			FIntPoint::ZeroValue,
			passParams->RenderTargets[0].GetTexture()->Desc.Extent
		};
		AddSimpleMeshPass(graph, passParams, renderScene, view, nullptr,
						  RDG_EVENT_NAME("SnkeBonusRenderEffect"),
						  viewport, ERDGPassFlags::Raster,
					      [&](FDynamicPassMeshDrawListContext* output)
		{
			FBreMeshProcessor meshProcessor{renderScene, &view, view.FeatureLevel, output };
			ForEachComponent_RenderThread([&](const UBreComponent& component,
											  const FBrePrimitiveSettings& settings,
											  const UPrimitiveComponent& primitive,
											  const FPrimitiveSceneProxy& primitiveProxy)
			{
				EGP::ForEachBatch(view, &primitiveProxy,
								   [&](const FMeshBatch& batch, uint64 mask, const auto* sceneProxy, int staticMeshID)
				{
					meshProcessor.AddMeshBatch(batch, mask, sceneProxy, staticMeshID);
				});
			});
		});
	}
};

TSubclassOf<U_EGP_RenderPass> UBreComponent::GetPassType() const
{
	return UBreRenderPass::StaticClass();
}
TSharedRef<F_EGP_RenderPassSceneViewExtension> UBreRenderPass::InitThisPass_GameThread(UWorld& thisWorld)
{
	return FSceneViewExtensions::NewExtension<FBrePassSVE>(this);
}
