#include "GOL_Demo.h"

#include "RenderGraphUtils.h"
#include "ScreenRendering.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FGOL_DemoModule"

void FGOL_DemoModule::StartupModule()
{
	//Make sure the graphics programming library is already initialized
	//    so that we have its shaders and shader headers.
	auto dependency = IPluginManager::Get().FindPlugin(TEXT("ExtendedGraphicsProgramming"));
	check(dependency && dependency->IsEnabled());
	
	//Register our shader folder with the engine.
	auto thisPlugin = IPluginManager::Get().FindPlugin(TEXT("GOL_Demo"));
	auto thisShadersDir = FPaths::Combine(thisPlugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/GameOfLife"), thisShadersDir);
}

void FGOL_DemoModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FGOL_DemoModule, GOL_Demo)