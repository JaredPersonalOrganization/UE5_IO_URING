#pragma once
#include "CoreMinimal.h"







class FLinuxFileHandle
{
	FLinuxFileHandle(const FString& InFilename, const int32 InHandle, const uint64 InSize, const int32 InOpenFlags, const uint64 InBlockSize);
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
	
	void UpdateHandle(const int32 InHandle, const EHandleState InState)
	{
		Handle = InHandle;
		State = InState;
	}
	
	EHandleState GetState() const
	{
		return State;
	}
	
	int32 GetHandle() const
	{
		return Handle;
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
	

private:
	FString Filename;
	uint64 Size = 0;
	uint64 BlockSize = 0;
	int32  Handle = 0;
	int32  OpenFlags = 0;
	EHandleState State = Closed;
};