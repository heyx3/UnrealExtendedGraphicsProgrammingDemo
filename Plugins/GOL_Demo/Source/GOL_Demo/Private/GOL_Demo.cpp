#include "GOL_Demo.h"

#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FGOL_DemoModule"

void FGOL_DemoModule::StartupModule()
{
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