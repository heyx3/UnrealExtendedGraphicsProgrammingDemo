using UnrealBuildTool;

public class GOL_Demo : ModuleRules
{
	public GOL_Demo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"ExtendedGraphicsProgramming"
		});
			
		
		PrivateDependencyModuleNames.AddRange(new string[] {
			"CoreUObject",
			"Projects",
			"Engine",
			"Slate",
			"SlateCore",
			"RenderCore", "Renderer", "RHICore", "RHI"
		});
	}
}
