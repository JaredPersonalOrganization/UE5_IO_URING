#include "NvmeDevice.h"

#include <sys/sysmacros.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>

#include "LinuxFileHandle.h"
#include "LinuxPlatformIoDispatcherModule.h"
#include "Algo/Partition.h"
#include "Misc/Paths.h"





// Helper Functions
bool GetFileValue(const FString& SysBlock, const FString& File, uint64& OutValue)
{
	FString Contents;
	if (!ReadFileContents(SysBlock / File, Contents))
	{
		return false;
	}
	
	OutValue = atol(TCHAR_TO_UTF8(*Contents));
	return true;
}

bool GetControllerDevice(const FString& SysBlock, FString& OutDeviceName)
{
	FString Content;
	if (!ReadFileContents(SysBlock / TEXT("uevent"), Content))
	{
		return false;
	}
	
	TArray<FString> Lines;
	Content.ParseIntoArray(Lines, TEXT("\n"), true);
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("DEVNAME=")))
		{
			OutDeviceName = Line.RightChop(8).TrimStartAndEnd();
			return !OutDeviceName.IsEmpty();
		}
	}
	
	UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find DEVNAME in file %s"), *(SysBlock / TEXT("uevent")));
	return false;
}

bool GetCharacterDevice(const FString& ControllerPath, FString& OutCharacterDevice)
{
	// Get the namespace.
	int32 NamespaceStart = 0;
	if (!ControllerPath.FindLastChar('n', NamespaceStart))
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find namespace start index in %s"), *ControllerPath);
		return false;
	}
	
	FString Namespace;
	int32 NamespaceEnd = NamespaceStart + 1;
	while (NamespaceEnd < ControllerPath.Len() && FChar::IsDigit(ControllerPath[NamespaceEnd]))
	{
		Namespace += ControllerPath[NamespaceEnd];
		NamespaceEnd++;
	}
	
	if (Namespace.IsEmpty())
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find namespace index in %s"), *ControllerPath);
		return false;
	}
	
	// Find the controller index
	const int32 Found = ControllerPath.Find(TEXT("nvme"));
	if (Found == INDEX_NONE)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find controller start index in %s"), *ControllerPath);
		return false;
	}
	
	FString ControllerIndex;
	int32 ControllerEnd = Found + 4;
	while (ControllerEnd < ControllerPath.Len() && FChar::IsDigit(ControllerPath[ControllerEnd]))
	{
		ControllerIndex += ControllerPath[ControllerEnd];
		ControllerEnd++;
	}
	
	if (ControllerIndex.IsEmpty())
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find controller index in %s"), *ControllerPath);
		return false; 
	}
	
	OutCharacterDevice = FString::Printf(TEXT("/dev/ng%sn%s"), *ControllerIndex, *Namespace);
	
	return true;
}

bool GetLogicalBlockSizeValue(const FString& ControllerPath, const FString& SysBlock, uint64& OutLogicalBlockSize)
{
	int32 PartitionIndex = -1;
	if (!ControllerPath.FindLastChar('p', PartitionIndex))
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find partition index in %s"), *ControllerPath);
		return false;
	}
	const int32 CharactersToRemove = ControllerPath.Len() - PartitionIndex;
	
	const FString& DeviceName = ControllerPath.LeftChop(CharactersToRemove);
	const FString Path = TEXT("subsystem") / DeviceName / TEXT("queue/logical_block_size");
	
	return GetFileValue(SysBlock, Path, OutLogicalBlockSize);
}


FNvmeDevice::FNvmeDevice(FPrivateToken)
{}
	
FNvmeDevice::~FNvmeDevice()
{
	if (Fd != -1)
	{
		close(Fd);
		Fd = -1;
	}
}
	
TUniquePtr<FNvmeDevice> FNvmeDevice::Create(const dev_t InDeviceId)
{
	TUniquePtr<FNvmeDevice> NewDevice = MakeUnique<FNvmeDevice>(FPrivateToken());
	if (NewDevice->Initialize(InDeviceId))
	{
		return MoveTemp(NewDevice);
	}
	return nullptr;
}

void FNvmeDevice::UnregisterContainer(const class FLinuxFileHandle* Handle)
{
	for (int32 Index = 0; Index < RegisteredContainers.Num(); Index++)
	{
		if (RegisteredContainers[Index].Handle == Handle)
		{
			RegisteredContainers.RemoveAt(Index);
			break;
		}
	}
}

const FRegisteredContainer* FNvmeDevice::FindContainer(const class FLinuxFileHandle* Handle)
{
	for (const FRegisteredContainer& Container : RegisteredContainers)
	{
		if (Container.Handle == Handle)
		{
			return &Container;
		}
	}
	return nullptr;
}

bool FNvmeDevice::RegisterContainer(class FLinuxFileHandle* Handle)
{
	if (LIKELY(FindContainer(Handle)))
	{
		return true;
	}
	
	if (Handle->GetState() != FLinuxFileHandle::Opened)
	{
		if (!Handle->Open())
		{
			return false;
		}
	}
	
	ON_SCOPE_EXIT
	{
		Handle->Close();
	};
	
	const int32 HandleFd = Handle->GetFd();

	constexpr uint32 MaxExtents = 1024;
	constexpr uint32 FiemapSize = sizeof(struct fiemap) + (MaxExtents * sizeof(struct fiemap_extent));
	fiemap* Map = static_cast<struct fiemap*>(FMemory::Malloc(FiemapSize));
	
	ON_SCOPE_EXIT
	{
		FMemory::Free(Map);
	};
	
	uint64 LogicalOffset = 0;
	bool bLastExtentFound = false;
	
	TArray<FPhysicalExtent> PhysicalExtents;
	
	while (!bLastExtentFound)
	{
		FMemory::Memzero(Map, FiemapSize);
		Map->fm_start = LogicalOffset;
		Map->fm_length = FIEMAP_MAX_OFFSET;
		Map->fm_extent_count = MaxExtents;
		
		if (ioctl(HandleFd, FS_IOC_FIEMAP, Map) < 0)
		{
			const int32 ErrNo = errno;
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("FIEMAP failed for container %s. Error %s"), *Handle->GetFilename(), UTF8_TO_TCHAR(strerror(ErrNo)));
			return false;
		}
		
		if (Map->fm_mapped_extents == 0)
		{
			break;
		}
		
		for (uint32 Index = 0; Index < Map->fm_mapped_extents; Index++)
		{
			fiemap_extent& Ext = Map->fm_extents[Index];
			
			FPhysicalExtent Entry;
			
			Entry.LogicalOffset = Ext.fe_logical;
			Entry.PhysicalOffset = Ext.fe_physical; 
			Entry.Length = Ext.fe_length;
			

			PhysicalExtents.Add(Entry);
			
			if (Ext.fe_flags & FIEMAP_EXTENT_LAST)
			{
				bLastExtentFound = true;
				break;
			}
		}
		
		if (!bLastExtentFound && Map->fm_mapped_extents > 0)
		{
			auto& Last = Map->fm_extents[Map->fm_mapped_extents - 1];
			LogicalOffset = Last.fe_logical + Last.fe_length;
		}
	}
	
	RegisteredContainers.Add(FRegisteredContainer{
		.PhysicalExtents = MoveTemp(PhysicalExtents),
		.Handle = Handle,
	});
	
	
	RegisteredContainers[RegisteredContainers.Num()-1].DebugOutput();
	
	return true;
}

bool FNvmeDevice::Initialize(const dev_t InDeviceId)
{
	DeviceId = InDeviceId;
	const FString SysBlock = FString::Printf(TEXT("/sys/dev/block/%u:%u"), major(DeviceId), minor(DeviceId));
	if (!FPaths::DirectoryExists(SysBlock))
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find block directory %s"), *SysBlock);
		return false;
	}
	
	// Size 
	if (!GetFileValue(SysBlock, TEXT("size"), Size))
	{
		return false;
	}
	
	// Partitions
	if (!GetFileValue(SysBlock, TEXT("partition"), Partitions))
	{
		return false;
	}
	
	// Start
	if (!GetFileValue(SysBlock, TEXT("start"), Start))
	{
		return false;
	}
	
	// Controller path
	if (!GetControllerDevice(SysBlock, ControllerDevice))
	{
		return false;
	}
	
	// Logical Block Size 
	if (!GetLogicalBlockSizeValue(ControllerDevice, SysBlock, LogicalBlockSize))
	{
		return false;
	}
	
	// Character path
	if (!GetCharacterDevice(ControllerDevice, CharacterDevice))
	{
		return false;
	}
	
	UE_LOG(LogLinuxPlatformIO, Display, TEXT("Successfully parsed NVME Device. SysBlock %s, Character Device %s, Controller Device %s, Start %llu, Paritions %llu, Size %llu, LogicalBlockSize %llu"), 
		*SysBlock, *CharacterDevice, *ControllerDevice, Start, Partitions, Size, LogicalBlockSize);
	
	Fd = open(TCHAR_TO_UTF8(*CharacterDevice), O_RDONLY);
	if (Fd < 0)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to open character device %s"), *CharacterDevice);
		return false;
	}
	
	NamespaceId = ioctl(Fd, NVME_IOCTL_ID);
	if (NamespaceId < 0)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to obtain namespace id. File=(%s) Error=(%s)"), *CharacterDevice, UTF8_TO_TCHAR(strerror(errno)));
		return false;
	}
	
	UE_LOG(LogLinuxPlatformIO, Display, TEXT("Successfully opened character device %s. Namespace %d"), *CharacterDevice, NamespaceId);
	
	return true;
}