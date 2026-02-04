#include "LinuxFileHandle.h"
#include "LinuxFileHandle.h"

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>

#include "LinuxPlatformIoDispatcherModule.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"


static FString NormalizeFilename(const TCHAR* Filename)
{
	FString Result(Filename);
	
	// If we are already absolute return
	if (!FPaths::IsRelative(Result))
	{
		return Result;
	}
	
	FPaths::NormalizeFilename(Result);
	return FPaths::ConvertRelativePathToFull(Result);
}

static int32 CountPathComponents(const FString & Filename)
{
	if (Filename.Len() == 0)
	{
		return 0;
	}

	// if the first character is not a separator, it's part of a distinct component
	int NumComponents = (Filename[0] != TEXT('/')) ? 1 : 0;
	for (const auto & Char : Filename)
	{
		if (Char == TEXT('/'))
		{
			++NumComponents;
		}
	}

	// cannot be 0 components if path is non-empty
	return FMath::Max(NumComponents, 1);
}

static FString GetPathComponent(const FString & Filename, int NumPathComponent)
{
	// skip over empty part
	int StartPosition = (Filename[0] == TEXT('/')) ? 1 : 0;
		
	for (int ComponentIdx = 0; ComponentIdx < NumPathComponent; ++ComponentIdx)
	{
		int FoundAtIndex = Filename.Find(TEXT("/"), ESearchCase::CaseSensitive,
										 ESearchDir::FromStart, StartPosition);
			
		if (FoundAtIndex == INDEX_NONE)
		{
			checkf(false, TEXT("Asked to get %d-th path component, but filename '%s' doesn't have that many!"), 
					 NumPathComponent, *Filename);
			break;
		}
			
		StartPosition = FoundAtIndex + 1;	// skip the '/' itself
	}

	// now return the 
	int NextSlash = Filename.Find(TEXT("/"), ESearchCase::CaseSensitive,
									ESearchDir::FromStart, StartPosition);
	if (NextSlash == INDEX_NONE)
	{
		// just return the rest of the string
		return Filename.RightChop(StartPosition);
	}
	else if (NextSlash == StartPosition)
	{
		return TEXT("");	// encountered an invalid path like /foo/bar//baz
	}
		
	return Filename.Mid(StartPosition, NextSlash - StartPosition);
}

static bool MapFileRecursively(const FString & Filename, int PathComponentToLookFor, int MaxPathComponents, FString & ConstructedPath)
{
	// get the directory without the last path component
	FString BaseDir = ConstructedPath;

	// get the path component to compare
	FString PathComponent = GetPathComponent(Filename, PathComponentToLookFor);
	FString PathComponentLower = PathComponent.ToLower();

	bool bFound = false;

	// see if we can open this (we should)
	DIR* DirHandle = opendir(TCHAR_TO_UTF8(*BaseDir));
	if (DirHandle)
	{
		struct dirent *Entry;
		while ((Entry = readdir(DirHandle)) != nullptr)
		{
			FString DirEntry = UTF8_TO_TCHAR(Entry->d_name);
			if (DirEntry.ToLower() == PathComponentLower)
			{
				if (PathComponentToLookFor < MaxPathComponents - 1)
				{
					// make sure this is a directory
					bool bIsDirectory = Entry->d_type == DT_DIR;
					if(Entry->d_type == DT_UNKNOWN || Entry->d_type == DT_LNK)
					{
						struct stat StatInfo;
						if(stat(TCHAR_TO_UTF8(*(BaseDir / Entry->d_name)), &StatInfo) == 0)
						{
							bIsDirectory = S_ISDIR(StatInfo.st_mode);
						}
					}

					if (bIsDirectory)
					{
						// recurse with the new filename
						FString NewConstructedPath = ConstructedPath;
						NewConstructedPath /= DirEntry;

						bFound = MapFileRecursively(Filename, PathComponentToLookFor + 1, MaxPathComponents, NewConstructedPath);
						if (bFound)
						{
							ConstructedPath = NewConstructedPath;
							break;
						}
					}
				}
				else
				{
					// last level, try opening directly
					FString ConstructedFilename = ConstructedPath;
					ConstructedFilename /= DirEntry;

					struct stat StatInfo;
					bFound = (stat(TCHAR_TO_UTF8(*ConstructedFilename), &StatInfo) == 0);
					if (bFound)
					{
						ConstructedPath = ConstructedFilename;
						break;
					}
				}
			}
		}
		closedir(DirHandle);
	}
		
	return bFound;
}


FLinuxFileHandle::FLinuxFileHandle(const FString& InFilename, const int32 InHandle, const uint64 InSize, const int32 InOpenFlags, const uint64 InBlockSize)
	: Filename(InFilename), Size(InSize), BlockSize(InBlockSize), Handle(InHandle), OpenFlags(InOpenFlags), State(Opened)
{}

FLinuxFileHandle::~FLinuxFileHandle()
{
	Close();
}
	
FLinuxFileHandle* FLinuxFileHandle::CreateFileHandle(const TCHAR* FilePath, const bool bUseDirect)
{
	// Try to use the filename normally first
	int32 OpenFlags = O_RDONLY | O_CLOEXEC | __O_NOATIME;
	if (bUseDirect)
	{
		OpenFlags |= __O_DIRECT;
	}
	FString Filename = NormalizeFilename(FilePath);
	int32 Handle = open(TCHAR_TO_UTF8(*Filename), OpenFlags);
	if (Handle == -1 && errno != ENOENT && bUseDirect)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT( "Trying to open file %s without O_DIRECT Flag" ), *Filename);
		OpenFlags &= ~__O_DIRECT; // Try without the flag
		Handle = open(TCHAR_TO_UTF8(*Filename), OpenFlags);
	}
	
	if (Handle == -1)
	{
		if (ENOENT != errno)
		{
			int32 ErrNo = errno;
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT( "open('%s', O_RDONLY | O_CLOEXEC) failed: errno=%d (%s)" ), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
			return nullptr;
		}
		
#if (UE_GAME || UE_SERVER)
		// According to Unreal we have no business transversing the filesystem during Game and Server builds. The filepath should be correct.
		static bool bReadingFromPakFiles = FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile")) != nullptr;
		if (LIKELY(bReadingFromPakFiles))
		{
			return -1;
		}
#endif
		
		const int MaxPathComponents = CountPathComponents(Filename);
		if (MaxPathComponents > 0)
		{
			FString FoundFilename(TEXT("/"));	// start with root
			if (MapFileRecursively(Filename, 0, MaxPathComponents, FoundFilename))
			{
				Handle = open(TCHAR_TO_UTF8(*FoundFilename), OpenFlags);
				if (Handle == -1 && errno != ENOENT && bUseDirect)
				{
					UE_LOG(LogLinuxPlatformIO, Warning, TEXT( "Trying to open file %s without O_DIRECT Flag" ), *Filename);
					OpenFlags &= ~__O_DIRECT; // Try without the flag
					Handle = open(TCHAR_TO_UTF8(*Filename), OpenFlags);
				}
				if (Handle != -1)
				{
					if (Filename != FoundFilename)
					{
						Filename = FoundFilename;
						UE_LOG(LogLinuxPlatformIO, Log, TEXT("Mapped '%s' to '%s'"), *Filename, *FoundFilename);
					}
				}
			}
		}
	}
	
	if (Handle == -1)
	{
		return nullptr;
	}
	
	struct stat FileInfo;
	if (fstat(Handle, &FileInfo) == -1)
	{
		int32 ErrNo = errno;
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT( "fstat(Handle, &FileInfo) failed: File %s, errno=%d, Error (%s)" ), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
		return nullptr;
	}
	
	uint64 RequiredAlignment = 0;
	if (OpenFlags & __O_DIRECT)
	{
		if (S_ISBLK(FileInfo.st_mode))
		{
			uint32 LogicalSectorSize = 0;
			if (ioctl(Handle, BLKSSZGET, &LogicalSectorSize) == -1)
			{
				int32 ErrNo = errno;
				UE_LOG(LogLinuxPlatformIO, Warning, TEXT("BLKSSZGET failed: File %s, errno=%d (%s)"), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
				return nullptr;
			}

			uint32 PhysicalSectorSize = 0;
			if (ioctl(Handle, BLKPBSZGET, &PhysicalSectorSize) == -1)
			{
				int32 ErrNo = errno;
				UE_LOG(LogLinuxPlatformIO, Warning, TEXT("BLKPBSZGET failed: File %s, errno=%d (%s)"), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
				return nullptr;
			}

			RequiredAlignment = FMath::Max<uint64>(LogicalSectorSize, PhysicalSectorSize);
		}
		else if (S_ISREG(FileInfo.st_mode))
		{
			struct statfs FsInfo;
			if (fstatfs(Handle, &FsInfo) == -1)
			{
				int32 ErrNo = errno;
				UE_LOG(LogLinuxPlatformIO, Warning, TEXT("fstatfs failed: File %s, errno=%d (%s)"), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
				return nullptr;
			}
			RequiredAlignment = FsInfo.f_bsize;
		}
		else
		{
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Unsupported file type for DIRECT IO: %s"), *Filename);
			return nullptr;
		}
	}
	
	
	return new FLinuxFileHandle(Filename, Handle, FileInfo.st_size, OpenFlags, RequiredAlignment);
}
	
void FLinuxFileHandle::Close()
{
	if (State == Opened)
	{
		close(Handle);
		State = Closed;
		Handle = -1;
	}
}

bool FLinuxFileHandle::Open()
{
	if (State != Opened)
	{
		Handle = open(TCHAR_TO_UTF8(*Filename), OpenFlags);
		if (Handle == -1)
		{
			int32 ErrNo = errno;
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT( "open('%s', O_RDONLY | O_CLOEXEC) failed: errno=%d (%s)" ), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
			return false;
		}
		State = Opened;
	}
	return true;
}


