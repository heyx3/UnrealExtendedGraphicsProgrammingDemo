#include "GOL_RenderPass.h"

#include "Landscape.h"
#include "MaterialCompiler.h"
#include "RenderGraphUtils.h"
#include "SimpleMeshDrawCommandPass.h"
#include "Runtime/Renderer/Private/PostProcess/PostProcessing.h"
#include "Runtime/Renderer/Public/MeshPassProcessor.inl"

#include "EGP_GetMeshBatches.h"
#include "EGP_PostProcessMaterialShaders.h"
#include "EGP_DownsampleDepthPass.h"


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

struct FGoLInitializePS : public EGP::FScreenSpaceShader
{
    DECLARE_EXPORTED_SHADER_TYPE(FGoLInitializePS, Material, )
    
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        EGP_SCREEN_SPACE_PASS_MATERIAL_DATA()
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
    SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FGoLInitializePS, EGP::FScreenSpaceShader)
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FGoLInitializePS, TEXT("/GameOfLife/Init.usf"), TEXT("Main"), SF_Pixel);

static void EnsureCompiledSimulateShader(ERHIFeatureLevel::Type featureLevel, const UMaterialInterface* uMaterial)
{
    FMaterialShaderTypes types;
    types.AddShaderType<EGP::FScreenSpaceRenderVS>();
    types.AddShaderType<FGoLInitializePS>();
    auto foundShaders = EGP::FindMaterialShaders_RenderThread(
        uMaterial, types,
        { MD_PostProcess, featureLevel }
    );
    check(foundShaders);

    //Extract the shader and material proxy.
    auto* materialProxy = foundShaders->MaterialProxy;
    auto* materialF = foundShaders->Material;
    TShaderRef<EGP::FScreenSpaceRenderVS> shaderV;
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
    
    EGP::FScreenSpacePassMaterialInputs postProcessMaterialInputs;
    postProcessMaterialInputs.SceneTextures = sceneTextures;
    postProcessMaterialInputs.TargetView = &view;
    postProcessMaterialInputs.OutputViewportData = FScreenPassTextureViewport{ simStateTex };
    postProcessMaterialInputs.InputViewportData = FScreenPassTextureViewport{ view.ViewRect };

    EGP::AddScreenSpaceRenderPass<EGP::FScreenSpaceRenderVS, FGoLInitializePS>(
        graph, RDG_EVENT_NAME("GoL_Initialize"),
        postProcessMaterialInputs,
        EGP::FScreenSpacePassRenderState{ }, //Default to opaque blending and no depth/stencil usage
        initShaderParams, uMaterial,
        //Extract the specific param structs for each shader:
        &initShaderParams->ScreenSpacePassData, initShaderParams
    );
}

FGameOfLifeView::FGameOfLifeView(FRDGBuilder& graph, const FViewInfo& view, const FIntRect& viewportSubset,
                                 const UMaterialInterface* initShaderMaterial,
                                 const FSceneTextureShaderParameters& sceneTextures)
    : F_EGP_ViewPersistentData(graph, view, viewportSubset)
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

struct FGoLDisplayPS : public EGP::FScreenSpaceShader
{
    DECLARE_EXPORTED_SHADER_TYPE(FGoLDisplayPS, Material, )

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        EGP_SCREEN_SPACE_PASS_MATERIAL_DATA()
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
    SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FGoLDisplayPS, EGP::FScreenSpaceShader)
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
    EGP::FScreenSpacePassMaterialInputs inputs;
    inputs.Textures[0] = GetScreenPassTextureInput(
        FScreenPassTexture{ simStateTex },
        TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI()
    );
    inputs.SceneTextures = sceneTextures;
    inputs.TargetView = &view;
    inputs.InputViewportData = FScreenPassTextureViewport{ inputs.Textures[0].Texture };
    inputs.OutputViewportData = FScreenPassTextureViewport{ output.GetTexture(), view.ViewRect };
    
    EGP::AddScreenSpaceRenderPass<EGP::FScreenSpaceRenderVS, FGoLDisplayPS>(
        graph, RDG_EVENT_NAME("GoL_Display"), inputs,
        EGP::FScreenSpacePassRenderState{ blending },
        params, material,
        //Extract the specific param structs for each shader:
        &params->ScreenSpacePassData, params
    );
}

#pragma endregion

#pragma region Tick the sim state

struct FGoLSimulateCS : public EGP::FSimulationShader
{
    DECLARE_EXPORTED_SHADER_TYPE(FGoLSimulateCS, Material, );
    static FIntVector3 GroupSize() { return { 8, 8, 1 }; }

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(float, DeltaSeconds)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, NextSimStateTex)
        EGP_SIMULATION_PASS_MATERIAL_DATA()
    END_SHADER_PARAMETER_STRUCT()
    SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FGoLSimulateCS, EGP::FSimulationShader)

    //Feed the group size to the shader so that it's only defined in one place.
    static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& params,
                                             FShaderCompilerEnvironment& env)
    {
        EGP::FSimulationShader::ModifyCompilationEnvironment(params, env);
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
    EGP::FSimulationPassMaterialInputs inputs;
    inputs.Textures[0] = GetScreenPassTextureInput(
        FScreenPassTexture{ currentSimState },
        TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI()
    );

    //Set up the other, non-Material shader params.
    auto* params = graph.AllocParameters<FGoLSimulateCS::FParameters>();
    params->DeltaSeconds = deltaSeconds;
    params->NextSimStateTex = graph.CreateUAV(nextSimState);

    //Compute the group count for this dispatch.
    EGP::FSimulationPassState state;
    state.GroupCount.Set<FIntVector3>(FComputeShaderUtils::GetGroupCount(
        FIntVector3{ currentSimState->Desc.Extent.X, currentSimState->Desc.Extent.Y, 1 },
        FGoLSimulateCS::GroupSize()
    ));

    EGP::AddSimulationMaterialPass<FGoLSimulateCS>(graph, RDG_EVENT_NAME("GoL_Tick"),
                                                    inputs, state, view,
                                                    params, uMaterial);
}

#pragma endregion

#pragma region Primitive Component draw passes

TSubclassOf<U_EGP_RenderPass> UGoLComponent::GetPassType() const
{
    return U_GOL_RenderPass::StaticClass();
}

//Custom render-pass settings for each individual mesh batch within each component we want to render.
//This is actually where we need to keep all shader parameters even if they're not per-batch.
struct FGoLMeshShaderElementData : public FMeshMaterialShaderElementData
{
    FRHITexture* PreviousStateTex;
    FRHISamplerState* PreviousStateSampler;
};

class FGoLMeshVS : public FMeshMaterialShader
{
public:
    DECLARE_SHADER_TYPE(FGoLMeshVS, MeshMaterial);

    static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& params)
    {
        return params.MaterialParameters.MaterialDomain == MD_Surface &&
               FMeshMaterialShader::ShouldCompilePermutation(params);
    }

    FGoLMeshVS() = default;
    FGoLMeshVS(const ShaderMetaType::CompiledShaderInitializerType& initializer)
        : FMeshMaterialShader(initializer)
    {
    }
};
class FGoLMeshPS : public FMeshMaterialShader
{
public:
    DECLARE_SHADER_TYPE(FGoLMeshPS, MeshMaterial);

    static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& params)
    {
        return params.MaterialParameters.MaterialDomain == MD_Surface &&
               FMeshMaterialShader::ShouldCompilePermutation(params);
    }

    FGoLMeshPS() = default;
    FGoLMeshPS(const ShaderMetaType::CompiledShaderInitializerType& initializer)
        : FMeshMaterialShader(initializer)
    {
        //The user's Material may or may not make use of the previous state texture,
        //    so the uniforms must be marked Optional.
        PreviousStateTex.Bind(
            initializer.ParameterMap,
            TEXT("PreviousStateTex"),
            SPF_Optional
        );
        PreviousStateSampler.Bind(
            initializer.ParameterMap,
            TEXT("PreviousStateSampler"),
            SPF_Optional
        );
    }

    //Binds parameters to the shader to render the given "element".
    void GetShaderBindings(
        const FScene* scene,
        ERHIFeatureLevel::Type featureLevel,
        const FPrimitiveSceneProxy* primitiveSceneProxy,
        const FMaterialRenderProxy& materialRenderProxy,
        const FMaterial& material,
        #if ENGINE_MINOR_VERSION < 4
            const FMeshPassProcessorRenderState& drawRenderState,
        #endif
        const FGoLMeshShaderElementData& element,
        FMeshDrawSingleShaderBindings& shaderBindings) const
    {
        FMeshMaterialShader::GetShaderBindings(
            scene, featureLevel,
            primitiveSceneProxy, materialRenderProxy, material,
            #if ENGINE_MINOR_VERSION < 4
                drawRenderState,
            #endif
            element, shaderBindings
        );

        shaderBindings.AddTexture(PreviousStateTex, PreviousStateSampler,
                                  element.PreviousStateSampler, element.PreviousStateTex);
    }

private:

    LAYOUT_FIELD(FShaderResourceParameter, PreviousStateTex);
    LAYOUT_FIELD(FShaderResourceParameter, PreviousStateSampler);
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FGoLMeshVS, TEXT("/GameOfLife/Mesh.usf"), TEXT("MainVS"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(, FGoLMeshPS, TEXT("/GameOfLife/Mesh.usf"), TEXT("MainPS"), SF_Pixel);

//This struct doesn't feed into any shader, but tells the RDG about our mesh pass.
BEGIN_SHADER_PARAMETER_STRUCT(FGoLMeshPassParameters, )
    SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, PrevState)
    SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
    SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

//Generates actual draw calls for various kinds of 3D primitives.
class FGoLMeshProcessor final : public FMeshPassProcessor
{
public:

    FMeshPassProcessorRenderState PassDrawState;

    FGoLMeshProcessor(const FScene* scene, const FSceneView* view,
                      ERHIFeatureLevel::Type featureLevel,
                      FMeshPassDrawListContext* commandsOutput,
                      FBlendStateRHIRef blendState)
        : FMeshPassProcessor(
              #if ENGINE_MINOR_VERSION > 3
        	      TEXT("GameOfLife"),
        	  #endif
              scene, featureLevel, view, commandsOutput
          )
    {
        PassDrawState.SetBlendState(blendState);
        PassDrawState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
    }

    void AddMeshBatch(const FMeshBatch& batch, uint64 batchElementMask,
                      const FPrimitiveSceneProxy* proxy, int32 staticMeshID,
                      FRHITexture* prevState, FRHISamplerState* prevStateSampler)
    {
        //Get the first usable Material in the chain,
        //    starting at the batch's desired Material and ending at the Default Material.
        const FMaterialRenderProxy* fallbackMat = nullptr;
        const auto& resource = batch.MaterialRenderProxy->GetMaterialWithFallback(
            FeatureLevel, fallbackMat
        );
        const auto& materialProxy = fallbackMat ? *fallbackMat : *batch.MaterialRenderProxy;
        
        //Compile our shaders against the batch's Material and Vertex-Factory.
        TMeshProcessorShaders<FGoLMeshVS, FGoLMeshPS> shaderRefs;
        {
            FMaterialShaderTypes shaderTypes;
            shaderTypes.AddShaderType<FGoLMeshVS>();
            shaderTypes.AddShaderType<FGoLMeshPS>();

            FMaterialShaders materialShaders;
            verify(resource.TryGetShaders(shaderTypes, batch.VertexFactory->GetType(), materialShaders));

            materialShaders.TryGetVertexShader(shaderRefs.VertexShader);
            materialShaders.TryGetPixelShader(shaderRefs.PixelShader);
        }

        //Configure per-element settings.
        FGoLMeshShaderElementData elementData;
        elementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, proxy, batch, staticMeshID, false);
        //Set per-element shader parameters.
        elementData.PreviousStateTex = prevState;
        elementData.PreviousStateSampler = prevStateSampler;

        //Generate the draw calls for this batch.
        const FMeshDrawingPolicyOverrideSettings overrides = ComputeMeshOverrideSettings(batch);
        BuildMeshDrawCommands(
            batch, batchElementMask, proxy,
            materialProxy, resource, PassDrawState,
            MoveTemp(shaderRefs),
            ComputeMeshFillMode(resource, overrides),
            ComputeMeshCullMode(resource, overrides),
            FMeshDrawCommandSortKey::Default, EMeshPassFeatures::Default,
            elementData
        );
    }
    
    //The usual 'AddMeshBatch()' will not be used; instead we will use an alternative with more parameters.
    virtual void AddMeshBatch(const FMeshBatch& batch, uint64 batchElementMask,
                              const FPrimitiveSceneProxy* proxy, int32 staticMeshID) override { check(false); }
};

#pragma endregion

TSharedRef<F_EGP_RenderPassSceneViewExtension> U_GOL_RenderPass::InitThisPass_GameThread(UWorld& thisWorld)
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
	if (!Pass->ViewFilter->ShouldRenderFor(view))
		return;

    auto* passMaterial = Pass->GetEffectMaterial_RenderThread();
    
    RDG_EVENT_SCOPE(graph, "Game of Life, viewport %ix%i",
                    view.ViewRect.Width(), view.ViewRect.Height());
    
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
        RDG_EVENT_SCOPE(graph, "GoL: Re-initialize");
        InitGoLState(graph, simStateRDG, view,
                     GetSceneTextureShaderParameters(inputs.SceneTextures),
                     passMaterial);
        viewData.ReinitializeViews = false;
    }
    //If some time has passed on the game thread, tick this viewport's sim.
    if (viewData.NextTickTime > 0)
    {
        RDG_EVENT_SCOPE(graph, "GoL: Tick %f seconds", viewData.NextTickTime);
        
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

    //Draw our mesh pass into the sim state.
    {
        RDG_EVENT_SCOPE(graph, "GoL: Mesh passes (%i primitives)",
                        Pass->GetComponentData_RenderThread().Num());

        FScene* renderScene = nullptr;
        if (view.Family != nullptr && view.Family->Scene != nullptr)
            renderScene = view.Family->Scene->GetRenderScene();

        //We want to use the scene's depth-texture for our mesh pass,
        //    however the Game of Life state texture exists on its own
        //    rather than being a subset of a larger render target,
        //    and also uses a lower-resolution than the viewport.
        //To fix this, we need to resample the depth buffer.
        FRDGTextureRef depthBuffer = inputs.SceneTextures->GetContents()->SceneDepthTexture;
        if (view.ViewRect.Min != FIntPoint::ZeroValue || depthBuffer->Desc.Extent != viewData.SimState->GetSizeXY())
        {
            auto resampledDepthBufferDesc = depthBuffer->Desc;
            resampledDepthBufferDesc.Extent = viewData.SimState->GetSizeXY();
            FRDGTextureRef resampledDepthBuffer = graph.CreateTexture(
                resampledDepthBufferDesc, TEXT("GoL_ResampledSceneDepth")
            );
            
            EGP::AddDownsampleDepthPass(
                graph, view,
                FScreenPassTexture{ depthBuffer, view.ViewRect },
                FScreenPassRenderTarget{ resampledDepthBuffer, ERenderTargetLoadAction::ENoAction },
                EDownsampleDepthFilter::Checkerboard
            );
            depthBuffer = resampledDepthBuffer;
        }

        //Note that it doesn't matter if the texture has already been registered in this graph previously --
        //    in that case its previous RDG handle will be returned here.
        auto nextSimStateRDG = RegisterExternalTexture(
            graph, viewData.SimBuffer,
            TEXT("GoL_NextState")
        );

        //Set up the RDG configuration of the pass.
        auto* passParams = graph.AllocParameters<FGoLMeshPassParameters>();
        passParams->View = view.ViewUniformBuffer;
        passParams->Scene = GetSceneUniformBufferRef(graph, view);
        passParams->PrevState = graph.CreateSRV(FRDGTextureSRVDesc{ simStateRDG });
        passParams->RenderTargets[0] = FRenderTargetBinding{ nextSimStateRDG, ERenderTargetLoadAction::ELoad };
        passParams->RenderTargets.DepthStencil = {
            depthBuffer,
            ERenderTargetLoadAction::ELoad,
            FExclusiveDepthStencil::DepthRead_StencilNop
        };

        //Dispatch the draw calls.
        AddCopyTexturePass(graph, simStateRDG, nextSimStateRDG);
        AddSimpleMeshPass(graph, passParams, renderScene, view, nullptr,
                          RDG_EVENT_NAME("GoLMeshes"),
                          FIntRect{ FIntPoint::ZeroValue, simStateRDG->Desc.Extent },
                          [&](FDynamicPassMeshDrawListContext* output)
        {
            //Define one mesh processor for each blend mode.
            FGoLMeshProcessor meshProcessorAlpha{
                renderScene, &view, view.FeatureLevel, output,
                TStaticBlendState<CW_RGBA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI()
            };
            FGoLMeshProcessor meshProcessorAdditive{
                renderScene, &view, view.FeatureLevel, output,
                TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One>::GetRHI()
            };
            FGoLMeshProcessor meshProcessorMultiply{
                renderScene, &view, view.FeatureLevel, output,
                TStaticBlendState<CW_RGBA, BO_Add, BF_DestColor, BF_Zero>::GetRHI()
            };

            //Get every scene primitive that's tagged with our custom component.
            ForEachComponent_RenderThread([&](const UGoLComponent& component,
                                                     const FGoLPrimitiveRenderSettings& componentSettings,
                                                     const UPrimitiveComponent& primitive,
                                                     const FPrimitiveSceneProxy& primitiveProxy)
            {
                //Pick the blend mode.
                FGoLMeshProcessor* componentProcessor;
                switch (componentSettings.BlendMode)
                {
                    case EGoLMeshBlendModes::Alpha: componentProcessor = &meshProcessorAlpha; break;
                    case EGoLMeshBlendModes::Additive: componentProcessor = &meshProcessorAdditive; break;
                    case EGoLMeshBlendModes::Multiply: componentProcessor = &meshProcessorMultiply; break;
                    default: check(false); return;
                }
                
                //Draw every renderable mesh-batch in that component.
                EGP::ForEachBatch(view, &primitiveProxy,
                                   [&](const FMeshBatch& batch, uint64 mask, const auto* sceneProxy, int staticMeshID)
                {
                    componentProcessor->AddMeshBatch(batch, mask, sceneProxy, staticMeshID,
                                                     viewData.SimState,
                                                     TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI());
                });
            });
        });

        //Swap 'previous' and 'next' textures.
        std::swap(viewData.SimBuffer, viewData.SimState);
        simStateRDG = nextSimStateRDG;
    }
    
    //Finally, draw the sim state onto the scene color texture.
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
int32 UMaterialExpressionGoLMeshOutputs::Compile(FMaterialCompiler* compiler, int32 pinIdx)
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

    if (!doPin(0, DiscreteOutput, &DiscreteOutputConst) &&
          !doPin(1, ContinuousOutput, &ContinuousOutputConst) &&
          !doPin(2, OutputAlpha, &OutputAlphaConst))
    {
        codeID = INDEX_NONE;
    }
    return compiler->CustomOutput(this, pinIdx, codeID);
}
#endif

void UGoLUtilities::GetLandscapeComponents(ALandscape* landscape, TArray<ULandscapeComponent*>& output)
{
    output.Empty();
    for (TObjectIterator<ULandscapeComponent> it; it; ++it)
        output.Add(*it);
}
