using UnrealBuildTool;

public class GameOfLife : ModuleRules
{
	public GameOfLife(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "ExtendedGraphicsProgramming" });
		PrivateDependencyModuleNames.AddRange(new string[] {  });
	}
}
