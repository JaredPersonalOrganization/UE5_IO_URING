#include "LinuxFileHandle.h"
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>

#include "LinuxPlatformIoDispatcherModule.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"


bool ReadFileContents(const FString& Filename, FString& OutContents)
{
	FILE* File = fopen(TCHAR_TO_UTF8(*Filename), "r");
	if (!File)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to open file %s. Error %d %s"), *Filename, errno, UTF8_TO_TCHAR(strerror(errno)));
		return false;
	}
	
	ON_SCOPE_EXIT
	{
		fclose(File);
	};
	
	TArray<char> Buf;
	Buf.SetNum(1024);
	
	size_t TotalRead = 0;
	
	for (;;)
	{
		const size_t BytesRead = fread(Buf.GetData() + TotalRead, 1, Buf.Num() - TotalRead, File);
		
		TotalRead += BytesRead;
		
		if (BytesRead == 0)
		{
			if (ferror(File))
			{
				UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to read file %s. Error %d %s"), *Filename, errno, UTF8_TO_TCHAR(strerror(errno)));
				return false;
			}
			break;
		}

		if (TotalRead == Buf.Num())
		{
			Buf.SetNum(Buf.Num() * 2); 
		}
	}
	
	
	if (TotalRead == Buf.Num())
	{
		Buf.Add('\0');
	}
	else
	{
		Buf[TotalRead] = '\0';
	}
	
	OutContents = Buf.GetData();
	
	return true;
}

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


FLinuxFileHandle::FLinuxFileHandle(const FString& InFilename, const int32 InFd, const uint64 InSize, const int32 InOpenFlags, const uint64 InBlockSize, const dev_t InDevice)
	: Filename(InFilename), Size(InSize), BlockSize(InBlockSize), Fd(InFd), OpenFlags(InOpenFlags), Device(InDevice), State(Opened)
{}

FLinuxFileHandle::~FLinuxFileHandle()
{
	Close();
}

FLinuxFileHandle* FLinuxFileHandle::CreateFileHandle(const TCHAR* FilePath, const bool bUseDirect)
{
	int32 OpenFlags = O_RDONLY | O_CLOEXEC | __O_NOATIME; // Should we use NOATIME? 
	if (bUseDirect)
	{
		OpenFlags |= __O_DIRECT;
	}
	
	const FString Filename = NormalizeFilename(FilePath);
	const char* CFilename = TCHAR_TO_UTF8(*Filename);
	int32 Handle = open(CFilename, OpenFlags);
	if (Handle == -1)
	{
		if (errno != ENOENT && bUseDirect)
		{
			OpenFlags &= ~__O_DIRECT; // Try again without O_DIRECT
			Handle = open(CFilename, OpenFlags);
		}
	}
	
	if (Handle == -1)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("open(Filename, OpenFlags) failed: Filename=(%s). Flags=(%d), error=%d, error=%s"), *Filename, OpenFlags, errno, UTF8_TO_TCHAR(strerror(errno)));
		return nullptr;
	}
	
	struct stat FileInfo;
	if (fstat(Handle, &FileInfo) == -1)
	{
		int32 ErrNo = errno;
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("fstat(Handle, &FileInfo) failed: Filename=(%s), errno=(%d), error=(%s)" ), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
		return nullptr;
	}
	
	uint64 RequiredAlignment = 0;
	if (OpenFlags & __O_DIRECT)
	{
		if (!S_ISREG(FileInfo.st_mode))
		{
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Unsupported file type for DIRECT IO: filename=(%s), type=(%u)"), *Filename, FileInfo.st_mode);
			return nullptr;
		}
		
		struct statfs FsInfo;
		if (fstatfs(Handle, &FsInfo) == -1)
		{
			int32 ErrNo = errno;
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("fstatfs(Handle, &FsInfo) fstatfs failed: File=(%s), errno=(%d) error=(%s)"), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
			return nullptr;
		}
		RequiredAlignment = FsInfo.f_bsize;
	}
	
	return new FLinuxFileHandle(Filename, Handle, FileInfo.st_size, OpenFlags, RequiredAlignment, FileInfo.st_dev);
}
	
void FLinuxFileHandle::Close()
{
	if (State == Opened)
	{
		close(Fd);
		State = Closed;
		Fd = -1;
	}
}

bool FLinuxFileHandle::Open()
{
	if (State != Opened)
	{
		Fd = open(TCHAR_TO_UTF8(*Filename), OpenFlags);
		if (Fd == -1)
		{
			const int32 ErrNo = errno;
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT( "open('%s', O_RDONLY | O_CLOEXEC) failed: errno=%d (%s)" ), *Filename, ErrNo, UTF8_TO_TCHAR(strerror(ErrNo)));
			return false;
		}
		State = Opened;
	}
	return true;
}


