#pragma once
#include <sys/sysmacros.h>
#include "CoreMinimal.h"




bool ReadFileContents(const FString& Filename, FString& OutContents);

class FLinuxFileHandle
{
	FLinuxFileHandle(const FString& InFilename, const int32 InFd, const uint64 InSize, const int32 InOpenFlags, const uint64 InBlockSize, const dev_t InDevice);
public:
	~FLinuxFileHandle();
	
	static FLinuxFileHandle* CreateFileHandle(const TCHAR* FilePath, const bool bUseDirect);
	
	void Close();
	
	bool Open();
	
	enum EHandleState : uint8
	{
		Closed, 
		Opened, 
		Fixed
	};
	
	void UpdateFd(const int32 InHandle, const EHandleState InState)
	{
		Fd = InHandle;
		State = InState;
	}
	
	EHandleState GetState() const
	{
		return State;
	}
	
	int32 GetFd() const
	{
		return Fd;
	}
	
	uint64 GetSize() const
	{
		return Size;
	}
	
	const FString& GetFilename() const
	{
		return Filename;
	}
	
	bool IsDirect() const
	{
		return OpenFlags & __O_DIRECT;
	}
	
	uint32 GetBlockSize() const
	{
		return BlockSize;
	}
	
	uint32 GetDeviceMajor() const
	{
		return major(Device);
	}
	
	uint32 GetDeviceMinor() const
	{
		return minor(Device);
	}
	
	dev_t GetDevice() const
	{
		return Device;
	}

private:
	FString Filename;
	uint64 Size = 0;
	uint64 BlockSize = 0;
	int32  Fd = 0;
	int32  OpenFlags = 0;
	dev_t Device;
	EHandleState State = Closed;
};