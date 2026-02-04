// Copyright Epic Games, Inc. All Rights Reserved.

using System.Collections.Generic;
using EpicGames.Core;
using UnrealBuildTool;
using UnrealBuildBase;
using System.IO;

public class LinuxPlatformIoDispatcher : ModuleRules
{
	public LinuxPlatformIoDispatcher(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
				DirectoryReference.Combine(Unreal.EngineSourceDirectory, "Runtime", "Core", "Internal").FullName
			}
		);

		// Target.PreBuildSteps.Add("");
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
				
			}
		);
		
		
		PrivateDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.Add("CoreUObject");
		PublicDependencyModuleNames.Add("PakFile");
	}
}
