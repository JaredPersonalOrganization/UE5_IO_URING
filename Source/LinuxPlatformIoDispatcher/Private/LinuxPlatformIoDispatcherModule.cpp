// Copyright Epic Games, Inc. All Rights Reserved.

#include "LinuxPlatformIoDispatcherModule.h"	

#include "LinuxPlatformIoDispatcher.h"


#define LOCTEXT_NAMESPACE "FLinuxPlatformIoDispatcherModule"

DEFINE_LOG_CATEGORY(LogLinuxPlatformIO);



bool FLinuxPlatformIoDispatcherModule::bIsInitialized = false;

uint64 GetLoadingCycles()
{
	return FLinuxPlatformIoDispatcher::GetCycleCounter();
}

void ResetLoadingCycles()
{
	return FLinuxPlatformIoDispatcher::ResetCyclesCounter();
}

void SetLoadingPriority(const EDispatcherPriority InPriority)
{
	FLinuxPlatformIoDispatcher::UpdatePriority(InPriority);
}

EDispatcherPriority GetLoadingPriority()
{
	return FLinuxPlatformIoDispatcher::GetLoadingPriority();
}

bool FLinuxPlatformIoDispatcherModule::IsInitialized()
{
	return bIsInitialized;
}

void FLinuxPlatformIoDispatcherModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FLinuxPlatformIoDispatcherModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}


bool GUseLinuxPlatformIO = true;
static FAutoConsoleVariableRef CVarUseLinuxPlatformIO(
	TEXT("r.Linux.Streaming.UsePlatformIO"),
	GUseLinuxPlatformIO,
	TEXT("Determines if we are going to use Linux iouring or a generic implementation.\n"),
	ECVF_ReadOnly
);


TUniquePtr<IPlatformFileIoStore> FLinuxPlatformIoDispatcherModule::CreatePlatformFileIoStore()
{
	if (GUseLinuxPlatformIO)
	{
		bIsInitialized = true;
		return FLinuxPlatformIoDispatcher::Create();
	}
	return nullptr;
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FLinuxPlatformIoDispatcherModule, LinuxPlatformIoDispatcher)