#pragma once
#include "CoreMinimal.h"
#include "LinuxFileHandle.h"


struct FPhysicalExtent
{
	uint64 LogicalOffset = 0;
	uint64 PhysicalOffset = 0;
	uint64 Length = 0;
};

struct FRegisteredContainer
{
	TArray<FPhysicalExtent> PhysicalExtents;
	class FLinuxFileHandle* Handle = nullptr;
	
	void DebugOutput()
	{
		FString OutputString;
		for (int32 Index = 0; Index < PhysicalExtents.Num(); Index++)
		{
			OutputString += FString::Printf(TEXT("{Index: %d, LogicalOffset %llu, Length %llu, PhysicalOffset %llu}"), 
				Index, PhysicalExtents[Index].LogicalOffset, PhysicalExtents[Index].Length, PhysicalExtents[Index].PhysicalOffset);
		}
		UE_LOG(LogTemp, Warning, TEXT("File %s, Mappings: %s"), *Handle->GetFilename(), *OutputString);
	}
};


class FNvmeDevice
{
	struct FPrivateToken
	{
		explicit FPrivateToken() = default;
	};
public:
	FNvmeDevice(FPrivateToken);
	
	~FNvmeDevice();
	
	static TUniquePtr<FNvmeDevice> Create(const dev_t InDeviceId);
	
	bool RegisterContainer(class FLinuxFileHandle* Handle);
	
	void UnregisterContainer(const class FLinuxFileHandle* Handle);
	
	const FRegisteredContainer* FindContainer(const class FLinuxFileHandle* Handle);
	
	dev_t GetDeviceId() const
	{
		return DeviceId;
	}
	
	bool ShouldRemove() const
	{
		return RegisteredContainers.IsEmpty();
	}
	
	int32 GetFd() const
	{
		return Fd;
	}
	
	void SetFixedFd(const int32 InFixedFd)
	{
		FixedFd = InFixedFd;
	}
	
	int32 GetFixedFd() const
	{
		return FixedFd;
	}
	
	uint64 GetLogicalBlockSize() const
	{
		return LogicalBlockSize;
	}
	
	int32 GetNamespace() const
	{
		return NamespaceId;
	}
	
	uint64 GetStart() const
	{
		return Start;
	}
	
	uint64 GetSize() const
	{
		return Size;
	}
	
	uint64 GetPartitions() const
	{
		return Partitions;
	}
	
private:
	bool Initialize(const dev_t InDeviceId);
private:
	TArray<FRegisteredContainer> RegisteredContainers;
	
	FString CharacterDevice;
	FString ControllerDevice;
	dev_t DeviceId = 0;
	uint64 Start = 0;
	uint64 Size = 0;
	uint64 Partitions = 0;
	uint64 LogicalBlockSize = 0;
	int32 Fd = -1;
	int32 FixedFd = -1;
	int32 NamespaceId = -1;
};
