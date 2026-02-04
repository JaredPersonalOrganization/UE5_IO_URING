// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "Runtime/PakFile/Internal/IoDispatcherFileBackendTypes.h"

DECLARE_LOG_CATEGORY_EXTERN(LogLinuxPlatformIO, Log, All);


enum class EDispatcherPriority : uint8
{
	Normal,
	High,
};

LINUXPLATFORMIODISPATCHER_API uint64 GetLoadingCycles();

LINUXPLATFORMIODISPATCHER_API void ResetLoadingCycles(); 

LINUXPLATFORMIODISPATCHER_API void SetLoadingPriority(const EDispatcherPriority InPriority);

LINUXPLATFORMIODISPATCHER_API EDispatcherPriority GetLoadingPriority();


// DO NOT INITIALIZE THIS CLASS. IT WILL BE CREATED AUTOMATICALLY BY THE ENGINE.
class LINUXPLATFORMIODISPATCHER_API FLinuxPlatformIoDispatcherModule : public IPlatformFileIoStoreModule
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	
	/**IPlatformFileIoStoreModule implementation */
	virtual TUniquePtr<IPlatformFileIoStore> CreatePlatformFileIoStore() override;
	
	static bool IsInitialized();
private:
	static bool bIsInitialized;
};
