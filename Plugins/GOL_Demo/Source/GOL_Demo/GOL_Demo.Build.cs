// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GOL_Demo : ModuleRules
{
	public GOL_Demo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		//To use post-processing structs we need some "private" engine headers.
		PrivateIncludePaths.AddRange(new string[] {
			System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Private"),
		});
		
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
			"RenderCore", "Renderer", "RHICore", "RHI",
			"Landscape"
		});
	}
}
