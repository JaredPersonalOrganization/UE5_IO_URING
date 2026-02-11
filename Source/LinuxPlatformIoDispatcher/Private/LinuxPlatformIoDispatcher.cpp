#include "LinuxPlatformIoDispatcher.h"

#include <netinet/ip.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include "IPlatformFilePak.h"
#include "LinuxFileHandle.h"
#include "LinuxPlatformIoDispatcherModule.h"
#include "NvmeDevice.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "uring/int_flags.h"
#include "uring/liburing.h"


bool GRegisterBuffers = true;
static FAutoConsoleVariableRef CVarRegisterBuffers(
	TEXT("r.Linux.Streaming.RegisterBuffers"),
	GRegisterBuffers,
	TEXT("Whether to register buffers. May have a performance improvement"),
	ECVF_ReadOnly
);

bool GUseSQPollThread = false;
static FAutoConsoleVariableRef CVarSQPollThread(
	TEXT("r.Linux.Streaming.UseSQPollThread"),
	GUseSQPollThread,
	TEXT("Whether to use a SQPoll thread. This isn't a free performance switch. Currently makes the performance worse."),
	ECVF_ReadOnly
);

int32 GSQPollIdleTimeMS = 50;
static FAutoConsoleVariableRef CVarSQPollIdleTimeMS(
	TEXT("r.Linux.Streaming.SqPollIdleTimeMS"),
	GSQPollIdleTimeMS,
	TEXT("SQPoll sleep timer"),
	ECVF_ReadOnly
);

bool GUseDirectIO = false;
static FAutoConsoleVariableRef CVarUseDirectIO(
	TEXT("r.Linux.Streaming.UseDirectIO"),
	GUseDirectIO, 
	TEXT("Tries to use O_DIRECT. Experimental. Does not increase performance."),
	ECVF_ReadOnly
);

bool GUseIOPoll = false;
static FAutoConsoleVariableRef CVarUseIOPoll(
	TEXT("r.Linux.Streaming.UseIOPoll"),
	GUseIOPoll,
	TEXT("Tries to use IOPoll. Checks /sys/module/nvme/parameters/poll_queues to see if it's available. Do not use this. Most of the time it will not work."),
	ECVF_ReadOnly
);

bool GUseNvmeDirect= false;
static FAutoConsoleVariableRef CVarUseNvmeDirect(
	TEXT("r.Linux.Streaming.NVMEPassthrough"),
	GUseNvmeDirect,
	TEXT("Whether use NVME Passthrough directly with io_uring. Experimental."),
	ECVF_ReadOnly
);

int32 GIODefaultBufferAlignment = 4096;
static FAutoConsoleVariableRef CVarDirectIODefaultBufferAlignment(
	TEXT("r.Linux.Streaming.DefaultBufferAlignment"),
	GIODefaultBufferAlignment,
	TEXT("This value can different meanings. When used with DirectIO it represents optimal read size, when used with NvmeDirect it represents the logical block size. For both of these, it will increase if necessary."),
	ECVF_ReadOnly
);

int32 GMaxNumOpenFiles = 1024;
static FAutoConsoleVariableRef CVarMaxOpenFiles(
	TEXT("r.Linux.Streaming.MaxFixedFiles"),
	GMaxNumOpenFiles,
	TEXT("Maximum number of fixed Files. Does not change the process max. If you need more files, increase the process max too. Default 1024"),
	ECVF_ReadOnly
);

int32 GQueueDepth = -1;
static FAutoConsoleVariableRef CVarQueueDepth(
	TEXT("r.Linux.Streaming.QueueDepth"),
	GQueueDepth,
	TEXT("Sets the queue depth of the submission queue entries. Default matches the number of read buffers."),
	ECVF_ReadOnly
);

int32 GBatchSubmitSize = 4;
static FAutoConsoleVariableRef CVarBatchSubmitSize(
	TEXT("r.Linux.Streaming.BatchSize"),
	GBatchSubmitSize, 
	TEXT("Batch size before we submit. Should be small to prevent starving decompression workers."),
	ECVF_ReadOnly
);

int32 GIoWqMaxBoundedWorkers = 0;
static FAutoConsoleVariableRef CVarIoWqMaxBoundedWorkers(
	TEXT("r.Linux.Streaming.IoWqMaxBoundedWorkers"),
	GIoWqMaxBoundedWorkers,
	TEXT("Maximum number of bounded workers created io_uring. Softly enforced. Supported since version 5.15")
		 TEXT("By default io_uring limits the number of bounded workers by the SQ Ring size and the number of CPUs on the system."),
		 ECVF_ReadOnly
);

int32 GIoWqMaxUnboundedWorkers = 0;
static FAutoConsoleVariableRef CVarIoWqMaxUnboundedWorkers(
	TEXT("r.Linux.Streaming.IoWqMaxUnboundedWorkers"),
	GIoWqMaxUnboundedWorkers,
	TEXT("Maximum number of Unbounded workers created io_uring. Softly enforced. Supported since version 5.15")
		 TEXT("By defualt io_uring limits the number of unbounded workers by the RLIMIT_NPROC limit of the system."),
		 ECVF_ReadOnly
);

// Testing Macros
#define TEST_WORST_CASE 0
#define TEST_DIRECTIO_REALLOCATE 0
#define TEST_LARGE_BUFFER_SIZES 0

// Logging 
#define VERIFY_URING(Function) \
{ \
	const int32 UringFunctionResult = Function; \
	if(UNLIKELY(UringFunctionResult < 0)) \
	{  \
		UE_LOG(LogLinuxPlatformIO, Fatal, TEXT("IoUring Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(-UringFunctionResult)), -UringFunctionResult, UTF8_TO_TCHAR(#Function), UTF8_TO_TCHAR(__FILE__), __LINE__); \
	} \
}

#define VERIFY_URING_VALUE(Value) \
{ \
	if(UNLIKELY(Value < 0)) \
	{ \
		UE_LOG(LogLinuxPlatformIO, Fatal, TEXT("IoUring Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(-Value)), -Value, UTF8_TO_TCHAR(__FUNCTION__), UTF8_TO_TCHAR(__FILE__), __LINE__); \
	} \
}

#define VERIFY_URING_SAFE(Function) \
{ \
	const int32 UringFunctionResult = Function; \
	if(UNLIKELY(UringFunctionResult < 0)) \
	{  \
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("IoUring Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(-UringFunctionResult)), -UringFunctionResult, UTF8_TO_TCHAR(#Function), UTF8_TO_TCHAR(__FILE__), __LINE__); \
		return false; \
	} \
}

#define VERIFY_URING_SAFE_NO_RET(Function) \
{ \
	const int32 UringFunctionResult = Function; \
	if(UNLIKELY(UringFunctionResult < 0)) \
	{  \
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("IoUring Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(-UringFunctionResult)), -UringFunctionResult, UTF8_TO_TCHAR(#Function), UTF8_TO_TCHAR(__FILE__), __LINE__); \
	} \
}

// Calls Linux function. On -1 logs error and returns false
#define VERIFY_LINUX_SAFE(Function) \
{ \
	const int32 LinuxFunctionResult = Function; \
	if(UNLIKELY(LinuxFunctionResult == -1)) \
	{ \
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Linux Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(errno)), errno, UTF8_TO_TCHAR(#Function), UTF8_TO_TCHAR(__FILE__), __LINE__); \
		return false; \
	} \
}

#define VERIFY_LINUX(Function) \
{ \
	const int32 LinuxFunctionResult = Function; \
	if(UNLIKELY(LinuxFunctionResult == -1)) \
	{ \
		const int32 ErrNo = errno; \
		UE_LOG(LogLinuxPlatformIO, Fatal, TEXT("Linux Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(ErrNo)), ErrNo, UTF8_TO_TCHAR(#Function), UTF8_TO_TCHAR(__FILE__), __LINE__); \
	} \
}

std::atomic<EDispatcherPriority> FLinuxPlatformIoDispatcher::Priority = EDispatcherPriority::Normal;
std::atomic<uint64> FLinuxPlatformIoDispatcher::CyclesCounter = 0;


FLinuxPlatformIoDispatcher::FLinuxPlatformIoDispatcher(FPrivateToken)
{}

FLinuxPlatformIoDispatcher::~FLinuxPlatformIoDispatcher()
{
	// We can't free DirectIOBuffer because we don't know the lifetime
	io_uring_unregister_buffers(&Ring);
	io_uring_queue_exit(&Ring);
	
	for (const FNvmeRequest* Request : NvmeRequestPool)
	{
		delete Request;
	}
}
	
void FLinuxPlatformIoDispatcher::Initialize(const FInitializePlatformFileIoStoreParams& Params)
{
	WakeUpDispatcherThreadDelegate = Params.WakeUpDispatcherThreadDelegate;
	BufferAllocator = Params.BufferAllocator;
	BlockCache = Params.BlockCache;
	Stats = Params.Stats;
	Finalize();
}

void FLinuxPlatformIoDispatcher::RegisterFile(class FLinuxFileHandle* File)
{
	checkf(!FreeRegisteredFiles.IsEmpty(), TEXT("Ran out of registered files!"));
	check(File->GetState() == FLinuxFileHandle::Opened);
			
	const int32 FixedFd = FreeRegisteredFiles.Pop(EAllowShrinking::No);
	const int32 Fd = File->GetFd();
	
	VERIFY_URING(io_uring_register_files_update(&Ring, FixedFd, &Fd, 1));
	
	File->Close(); 
	File->UpdateFd(FixedFd, FLinuxFileHandle::Fixed);
}

void FLinuxPlatformIoDispatcher::UnregisterFile(class FLinuxFileHandle* File)
{
	if (EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect))
	{
		UnregisterNvmeFile(File);
	}
	else if (File->GetState() == FLinuxFileHandle::Fixed)
	{
		constexpr int32 NewFd = -1;
		const int32 FixedFd = File->GetFd();
		VERIFY_URING(io_uring_register_files_update(&Ring, FixedFd, &NewFd, 1));
		FreeRegisteredFiles.Add(FixedFd);
	}
	delete File;
}

void FLinuxPlatformIoDispatcher::UnregisterNvmeFile(class FLinuxFileHandle* File)
{
	const int32 FoundDevice = RegisteredNvmeDevices.IndexOfByPredicate([Pid = File->GetDevice()](const TUniquePtr<FNvmeDevice>& NvmeDevice) { return NvmeDevice->GetDeviceId() == Pid; });
	if (FoundDevice != INDEX_NONE)
	{
		RegisteredNvmeDevices[FoundDevice]->UnregisterContainer(File);
		
		if (RegisteredNvmeDevices[FoundDevice]->ShouldRemove())
		{
			const int32 FixedFd = RegisteredNvmeDevices[FoundDevice]->GetFixedFd();
			if (FixedFd != INDEX_NONE)
			{
				constexpr int32 NewFd = -1;
				VERIFY_URING(io_uring_register_files_update(&Ring, FixedFd, &NewFd, 1));
				FreeRegisteredFiles.Add(FixedFd);
			}
			RegisteredNvmeDevices.RemoveAt(FoundDevice);
		}
	}
}

int32 FLinuxPlatformIoDispatcher::RegisterNvmeFile(class FLinuxFileHandle* File)
{
	const int32 FoundDevice = RegisteredNvmeDevices.IndexOfByPredicate([Pid = File->GetDevice()](const TUniquePtr<FNvmeDevice>& NvmeDevice) { return NvmeDevice->GetDeviceId() == Pid; });
	if (FoundDevice != INDEX_NONE)
	{
		if (!RegisteredNvmeDevices[FoundDevice]->RegisterContainer(File))
		{
			return INDEX_NONE;
		}
		return FoundDevice;
	}
	
	if (TUniquePtr<FNvmeDevice> NvmeDevice = FNvmeDevice::Create(File->GetDevice()))
	{
		if (NvmeDevice->RegisterContainer(File))
		{
			checkf(!FreeRegisteredFiles.IsEmpty(), TEXT("Ran out of registered files!"));
			
			const int32 FixedFd = FreeRegisteredFiles.Pop(EAllowShrinking::No);
			const int32 Fd = NvmeDevice->GetFd();
			
			VERIFY_URING(io_uring_register_files_update(&Ring, FixedFd, &Fd, 1));
			
			NvmeDevice->SetFixedFd(FixedFd);
			
			return RegisteredNvmeDevices.Add(MoveTemp(NvmeDevice));
		}
	}
	
	return INDEX_NONE;
}

bool FLinuxPlatformIoDispatcher::OpenContainer(const TCHAR* ContainerFilePath, uint64& ContainerFileHandle, uint64& ContainerFileSize)
{
	FLinuxFileHandle* Handle = FLinuxFileHandle::CreateFileHandle(ContainerFilePath, EnumHasAnyFlags(Flags, EUringFlags::DirectIO));
	if (!Handle)
	{
		return false;
	}
	
	// We let the dedicated thread register the files, because we don't control the process limits, and fixed files count towards the user limits.
	Handle->Close(); 
	ContainerFileSize = Handle->GetSize();
	ContainerFileHandle = reinterpret_cast<UPTRINT>(Handle);
	
	return true;
}
	
void FLinuxPlatformIoDispatcher::CloseContainer(uint64 ContainerFileHandle)
{
	{
		check(ContainerFileHandle);
		FLinuxFileHandle* Handle = reinterpret_cast<FLinuxFileHandle*>(ContainerFileHandle);
		FScopeLock _(&UnregisterCritical);
		FilesToUnregister.Add(Handle);
	}
	ServiceNotify();
}

uint8 FLinuxPlatformIoDispatcher::GetPriorityFlags() const
{
	// Try draining the queue with IOSQE_IO_DRAIN to see if completions get posted
	// Try disabling defer_taskrun too.
	return Priority == EDispatcherPriority::High ? IOSQE_ASYNC : 0;
}

int32 FLinuxPlatformIoDispatcher::OpenFile(FFileIoStoreReadRequest* Request)
{
	check(Request->ContainerFilePartition->FileHandle);
	FLinuxFileHandle* FileHandle = reinterpret_cast<FLinuxFileHandle*>(Request->ContainerFilePartition->FileHandle);
	
	if (EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect))
	{
		return RegisterNvmeFile(FileHandle);
	}
	else if (FileHandle->GetState() != FLinuxFileHandle::Fixed)
	{
		if (!FileHandle->Open())
		{
			return INDEX_NONE;
		}
		RegisterFile(FileHandle);
	}
	return 0;
}
	
bool FLinuxPlatformIoDispatcher::CreateCustomRequests(FFileIoStoreResolvedRequest& ResolvedRequest, FFileIoStoreReadRequestList& OutRequests)
{
	return false;
}

void FLinuxPlatformIoDispatcher::UpdateRegisteredBuffers(uint8* Start)
{
	const bool bNeedsReset = !RegisteredBuffers.IsEmpty();
	RegisteredBuffers.Reset();
	constexpr uint64 MaxBufferSize =  1ull << 30; // Max 1GiB
	const uint64 TotalSize = ActualBufferSize * NumAllocatedBuffers;
	
	if (TotalSize > MaxBufferSize) 
	{
		// Break it up into smaller chunks
		const uint64 RegisteredBuffersPerAlloc = MaxBufferSize / ActualBufferSize;
		const uint64 SizePerAlloc = RegisteredBuffersPerAlloc * ActualBufferSize;
					
		uint64 BufferIndex = 0;
		while (BufferIndex + RegisteredBuffersPerAlloc <= NumAllocatedBuffers) 
		{
			uint8* Current = Start + BufferIndex * ActualBufferSize;
			RegisteredBuffers.Add(iovec{.iov_base = Current, .iov_len = SizePerAlloc});
			BufferIndex += RegisteredBuffersPerAlloc;
		}
					
		if (BufferIndex < NumAllocatedBuffers) 
		{
			uint32 Remaining = NumAllocatedBuffers - BufferIndex;
			uint8* Current = Start + BufferIndex * ActualBufferSize;
			RegisteredBuffers.Add(iovec{.iov_base = Current, .iov_len = ActualBufferSize * Remaining});
		}
	}
	else
	{
		RegisteredBuffers.Add(iovec{.iov_base = Start, .iov_len = TotalSize});
	}
	
	if (bNeedsReset)
	{
		SubmitRequest(nullptr);
		WaitAndProcessCompletionRequests(true);
		VERIFY_URING(io_uring_unregister_buffers(&Ring));
	}
	
	VERIFY_URING(io_uring_register_buffers(&Ring, RegisteredBuffers.GetData(), RegisteredBuffers.Num()));
	
	UpdateRegisteredBuffersOffset();
	
	UE_LOG(LogLinuxPlatformIO, Display, TEXT("Successfully registered %d buffers. TotalSize %llu, ActualBufferSize %llu, "), RegisteredBuffers.Num(), TotalSize, ActualBufferSize);
}

void FLinuxPlatformIoDispatcher::UpdateRegisteredBuffersOffset()
{
	for (auto& Pair : AcquiredBufferProperties)
	{
		const uint8* Memory = Pair.Value.NewBuffer ? Pair.Value.NewBuffer : Pair.Key->Memory;
		Pair.Value.BufferIndex = -1;
		for (int32 RegisteredBufferIndex = 0; RegisteredBufferIndex < RegisteredBuffers.Num(); ++RegisteredBufferIndex)
		{
			uint8* Base = static_cast<uint8*>(RegisteredBuffers[RegisteredBufferIndex].iov_base);
			const uint64 Length  = RegisteredBuffers[RegisteredBufferIndex].iov_len;

			if (Memory >= Base && Memory < Base + Length)
			{
				Pair.Value.BufferIndex = RegisteredBufferIndex;
				break;
			}
		}
		check(Pair.Value.BufferIndex != -1);
	}
}

TUniquePtr<FLinuxPlatformIoDispatcher> FLinuxPlatformIoDispatcher::Create()
{
	TUniquePtr<FLinuxPlatformIoDispatcher> Impl = MakeUnique<FLinuxPlatformIoDispatcher>(FPrivateToken());
	if (!Impl->Initialize())
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to create io_uring dispatcher. Falling back to default"));
		return nullptr;
	}
	return MoveTemp(Impl);
}

int32 FLinuxPlatformIoDispatcher::CreateRing()
{
	const bool bIsMultiThreaded = FGenericPlatformProcess::SupportsMultithreading();
	if (!bIsMultiThreaded)
	{
		EnumRemoveFlags(Flags, EUringFlags::SQPoll);
	}
	
	int32 MaxPendingRequests = GQueueDepth;
	if (MaxPendingRequests == -1)
	{
		auto DispatcherBufferMemory = IConsoleManager::Get().FindConsoleVariable(TEXT("s.IoDispatcherBufferMemoryMB"));
		check(DispatcherBufferMemory);
		
		auto DispatcherBufferSizeKB = IConsoleManager::Get().FindConsoleVariable(TEXT("s.IoDispatcherBufferSizeKB"));
		check(DispatcherBufferSizeKB);
		
		const uint64 BufferSize = static_cast<uint64>(DispatcherBufferSizeKB->GetInt()) << 10ull; 
		const uint64 MemorySize = static_cast<uint64>(DispatcherBufferMemory->GetInt()) << 20ull;
		
		MaxPendingRequests = MemorySize / BufferSize;
	}
	
	io_uring_params Params;
	FMemory::Memset(&Params, 0, sizeof(Params));
	FMemory::Memset(&Ring, 0, sizeof(Ring));
	
	if (EnumHasAnyFlags(Flags, EUringFlags::SQPoll))
	{
		check(GSQPollIdleTimeMS > 0);
#if TEST_WORST_CASE
		bSubmitAll = false;
		bMustEnableRing = false;
		Params.flags = IORING_SETUP_SQPOLL;
		Params.sq_thread_idle = GSQPollIdleTimeMS;
		return io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params);
#endif
		Params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SUBMIT_ALL;
		Params.sq_thread_idle = GSQPollIdleTimeMS;
		
		EnumRemoveFlags(Flags, EUringFlags::RegisterRing);
		EnumRemoveFlags(Flags, EUringFlags::EnableRing);
		
		if (EnumHasAnyFlags(Flags, EUringFlags::IOPoll))
		{
			Params.flags |= IORING_SETUP_IOPOLL;
		}
		
		if (EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect))
		{
			Params.flags |= IORING_SETUP_SQE128 | IORING_SETUP_CQE32; // Required flags. Will not drop them.
		}
		
		if (io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params) == 0)
		{
			return 0;
		}
		
		EnumRemoveFlags(Flags, EUringFlags::SubmitAll);
		Params.flags &= ~IORING_SETUP_SUBMIT_ALL;
		return io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params);
	}
	else
	{
#if TEST_WORST_CASE
		bSubmitAll = false;
		bMustEnableRing = false;
		Params.flags = 0;
		return io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params);
#endif
		
		Params.flags = IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_R_DISABLED;
		
		if (EnumHasAnyFlags(Flags, EUringFlags::IOPoll))
		{
			Params.flags |= IORING_SETUP_IOPOLL;
		}
		
		if (EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect))
		{
			Params.flags |= IORING_SETUP_SQE128 | IORING_SETUP_CQE32; // Required flags. Will not drop them.
		}
		
		if (io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params) == 0)
		{
			return 0;
		}
		
		Params.flags &= ~IORING_SETUP_DEFER_TASKRUN;
		if (io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params) == 0)
		{
			return 0;
		}
		
		Params.flags &= ~IORING_SETUP_SINGLE_ISSUER;
		if (io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params) == 0)
		{
			return 0;
		}
		
		Params.flags &= ~IORING_SETUP_COOP_TASKRUN;
		if (io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params) == 0)
		{
			return 0;
		}
		
		// TODO: Is it even worth creating a ring at this point? We need to check the performance.
		EnumRemoveFlags(Flags, EUringFlags::SubmitAll);
		Params.flags &= ~IORING_SETUP_SUBMIT_ALL;
		return io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params);
	}
}

void FLinuxPlatformIoDispatcher::Finalize()
{
#if TEST_WORST_CASE
	bUseRegisteredBuffers = false;
#endif
	
	TArray<FFileIoStoreBuffer*> FreeBuffers;
	while (FFileIoStoreBuffer* Buffer = BufferAllocator->AllocBuffer())
	{
		FreeBuffers.Add(Buffer);
	}
	check(!FreeBuffers.IsEmpty());
	
	FreeBuffers.Sort([](const FFileIoStoreBuffer& A, const FFileIoStoreBuffer& B)
	{
		return A.Memory < B.Memory;
	});
	
	NumAllocatedBuffers = FreeBuffers.Num();
	ActualBufferSize = BufferAllocator->GetBufferSize();
	AcquiredBufferProperties.Reserve(NumAllocatedBuffers);
	
	for (int32 BufferIndex = 0; BufferIndex < NumAllocatedBuffers; BufferIndex++)
	{
		AcquiredBufferProperties.Add(FreeBuffers[BufferIndex]);
	}
	
	if (EnumHasAnyFlags(Flags, EUringFlags::DirectIO | EUringFlags::NvmeDirect))
	{
		FMemory::Free(FreeBuffers[0]->Memory);
			
		// Reallocate the buffers with the correct size
		BufferAlignment = GIODefaultBufferAlignment;
		ActualBufferSize = Align(ActualBufferSize + 2 * BufferAlignment, BufferAlignment);
		DirectIOBuffer = static_cast<uint8*>(FMemory::Malloc(ActualBufferSize * NumAllocatedBuffers, BufferAlignment));
		
		FMemory::Memzero(DirectIOBuffer, ActualBufferSize * NumAllocatedBuffers);
			
		for (uint64 Index = 0; Index < NumAllocatedBuffers; Index++)
		{
			FreeBuffers[Index]->Memory = DirectIOBuffer + Index * ActualBufferSize;
		}
	}
		
	if (EnumHasAnyFlags(Flags, EUringFlags::RegisterBuffers))
	{
		UpdateRegisteredBuffers(FreeBuffers[0]->Memory);
	}
		
	for (int32 Index = 0; Index < FreeBuffers.Num(); Index++)
	{
		BufferAllocator->FreeBuffer(FreeBuffers[Index]); 
	}
}


void AddFlags(EUringFlags& Flags)
{
	uint8 FlagsToAdd = 0;
	if (GUseIOPoll)
	{
		FlagsToAdd |=  static_cast<uint8>(EUringFlags::IOPoll);
	}
	if (GUseNvmeDirect) // NVME commands removes this flag
	{
		FlagsToAdd |=  static_cast<uint8>(EUringFlags::NvmeDirect);
	}
	if (GUseDirectIO)
	{
		FlagsToAdd |=  static_cast<uint8>(EUringFlags::DirectIO);
	}
	if (GRegisterBuffers)
	{
		FlagsToAdd |= static_cast<uint8>(EUringFlags::RegisterBuffers);
	}
	if (GUseSQPollThread)
	{
		FlagsToAdd |= static_cast<uint8>(EUringFlags::SQPoll);
	}
	
	EnumAddFlags(Flags, static_cast<EUringFlags>(FlagsToAdd));
}


bool FLinuxPlatformIoDispatcher::Initialize()
{
	UE_LOG(LogLinuxPlatformIO, Display, TEXT("Initializing Linux Platform IO"));
	// Add the flags
	AddFlags(Flags);
	
	if (EnumHasAnyFlags(Flags, EUringFlags::IOPoll)) 
	{
		FString Contents;
		if (!ReadFileContents(TEXT("/sys/module/nvme/parameters/poll_queues"), Contents))
		{
			EnumRemoveFlags(Flags, EUringFlags::IOPoll);
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Dropping IOPoll Feature"));
		}
		else if ((NumPollQueues = atoi(TCHAR_TO_UTF8(*Contents))) <= 0)
		{
			EnumRemoveFlags(Flags, EUringFlags::IOPoll);
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Invalid number of poll queues %d. Dropping IOPoll."), NumPollQueues);
		}
		else
		{
			UE_LOG(LogLinuxPlatformIO, Display, TEXT("IO-Poll Enabled with %d Poll Queues"), NumPollQueues);
			EnumAddFlags(Flags, EUringFlags::DirectIO); // Direct IO is required. Will be ignored if NvmeDirect is enabled.
		}
	}
	
	if (const int32 ErrNo = CreateRing(); ErrNo != 0)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to initialize io_uring. Error %s"), UTF8_TO_TCHAR(strerror(ErrNo)));
		return false;
	}
	
	// Check for the features we must have
	if (!(Ring.features & IORING_FEAT_NODROP)) // 5.5
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to initalize io_uring. Missing feature IORING_FEAT_NODROP"));
		return false;
	}
	
	if (!(Ring.features & IORING_FEAT_RSRC_TAGS)) // Fixed Buffers/Files 5.13
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to initalize io_uring. Missing feature IORING_FEAT_RSRC_TAGS"));
		return false;
	}
	
	CompletedCqesBuffer.SetNum(Ring.cq.ring_entries);
	
	// Set up registered files.
	{
		rlimit RLimit;
		VERIFY_LINUX_SAFE(getrlimit(RLIMIT_NOFILE, &RLimit));
		
		TArray<int32> SparseHandles;
		SparseHandles.SetNum(FMath::Min<int32>(RLimit.rlim_cur, GMaxNumOpenFiles));
		for (int32 Index = 0; Index < SparseHandles.Num(); Index++)
		{
			SparseHandles[Index] = -1;
			FreeRegisteredFiles.Add(Index);
		}
		
		VERIFY_URING_SAFE(io_uring_register_files(&Ring, SparseHandles.GetData(), SparseHandles.Num()))
		
		Algo::Reverse(FreeRegisteredFiles);
	}
	
	// Setup unbound/bounded workers
	{
		uint32 Workers[2] = {0, 0};
		const int32 Result = io_uring_register_iowq_max_workers(&Ring, Workers);
		if (Result == 0)
		{
			UE_LOG(LogLinuxPlatformIO, Display, TEXT("io_uring by default has %u bounded workers, and %u unbounded workers"), Workers[0], Workers[1]);
			if (GIoWqMaxBoundedWorkers > 0 || GIoWqMaxUnboundedWorkers > 0)
			{
				Workers[0] = GIoWqMaxBoundedWorkers > 0 ? GIoWqMaxBoundedWorkers : 0;
				Workers[1] = GIoWqMaxUnboundedWorkers > 0 ? GIoWqMaxUnboundedWorkers : 0;
				VERIFY_URING_SAFE_NO_RET(io_uring_register_iowq_max_workers(&Ring, Workers)); 
			}
		}
	}
	
	if (EnumHasAnyFlags(Flags, EUringFlags::RegisterBuffers))
	{
		rlimit RLimit;
		VERIFY_LINUX_SAFE(getrlimit(RLIMIT_MEMLOCK, &RLimit));
		
		// Check if we have enough space to fit registered buffers
		auto DispatcherBufferMemory = IConsoleManager::Get().FindConsoleVariable(TEXT("s.IoDispatcherBufferMemoryMB"));
		check(DispatcherBufferMemory);
		
#if TEST_LARGE_BUFFER_SIZES
		DispatcherBufferMemory->Set(2152); // Set the dispatcher buffer memory greater than 1GB to ensure registeration is working
#endif 
		
		const uint64 TotalBufferSize = static_cast<uint64>(DispatcherBufferMemory->GetInt()) << 20ull;
		
		if (TotalBufferSize > RLimit.rlim_cur)
		{
			EnumRemoveFlags(Flags, EUringFlags::RegisterBuffers);
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Dropping registered buffers for io_uring. Not enough space within locked memory. Has %llu, Needs %llu"), RLimit.rlim_cur, TotalBufferSize);
		}
		else if (EnumHasAnyFlags(Flags, EUringFlags::DirectIO) || EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect)) // DirectIO/DirectNVME requires more locked memory
		{
			auto DispatcherBufferSizeKB = IConsoleManager::Get().FindConsoleVariable(TEXT("s.IoDispatcherBufferSizeKB"));
			check(DispatcherBufferSizeKB);
				
			const uint64 ReadBufferSize = static_cast<uint64>(DispatcherBufferSizeKB->GetInt()) << 10ull;
			const uint64 BufferCount = TotalBufferSize / ReadBufferSize;
			constexpr uint64 MaximumBlockSize = 65536; // Test for the largest size.
			const uint64 AlignedSize = Align(ReadBufferSize + 2 * MaximumBlockSize, MaximumBlockSize);
				
			if (AlignedSize * BufferCount > RLimit.rlim_cur)
			{
				UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Dropping registered buffers io_uring. Not enough space within locked memory. Has %llu, Needs %llu"), RLimit.rlim_cur, AlignedSize * BufferCount);
				EnumRemoveFlags(Flags, EUringFlags::RegisterBuffers);
			}
		}
	}
	
	return true;
}

void FLinuxPlatformIoDispatcher::OnRequestComplete(FFileIoStoreReadRequest* Request)
{
	{
		FScopeLock _(&CompletedRequestsCritical);
		CompletedRequests.Add(Request);
	}
	WakeUpDispatcherThreadDelegate->Execute();
}

void FLinuxPlatformIoDispatcher::ProcessCompletionEvent(io_uring_cqe* CompletionEvent)
{
	// Assumes that we have the lock
	check(CompletionEvent->user_data);
	
	--NumPendingCompletions;
	
	FFileIoStoreReadRequest* Request = nullptr;
	if (EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect))
	{
		FNvmeRequest* NvmeRequest = reinterpret_cast<FNvmeRequest*>(CompletionEvent->user_data);
		if (--NvmeRequest->RemainingRequests != 0)
		{
			return;
		}
		
		// Get the request and add it back to the pool
		Request = NvmeRequest->Request;
		NvmeRequestPool.Add(NvmeRequest);
		
		if (CompletionEvent->res < 0)
		{
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to complete read request. Code %d, Error %s, Request %p"), -CompletionEvent->res, UTF8_TO_TCHAR(strerror(-CompletionEvent->res)), Request);
			Request->bFailed = true;
		}
		
		const uint64 NvmeResult = CompletionEvent->big_cqe[0];
		const uint16 Status = (uint16)((NvmeResult >> 16) & 0x7FF);
		
		if (Status != 0)
		{
			Request->bFailed = true;
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to complete read request. Nvme Error Code %u"), (uint32)Status);
		}
	}
	else
	{
		Request = reinterpret_cast<FFileIoStoreReadRequest*>(CompletionEvent->user_data);
		
		if (Request->Size != CompletionEvent->res)
		{
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to complete read request. Code %d, Error %s, Request %p"), -CompletionEvent->res, UTF8_TO_TCHAR(strerror(-CompletionEvent->res)), Request);
			Request->bFailed = true;
		}
	}
	
	Stats->OnFilesystemReadCompleted(Request);
	
	FFileIOStoreBufferProperties& Properties = AcquiredBufferProperties.FindChecked(Request->Buffer);
	
	Request->Buffer->Memory = Request->Buffer->Memory + Properties.FixupOffset;
	
	CompletedRequests.Add(Request);
	
	++NumCompletionEventsAhead;
}

void FLinuxPlatformIoDispatcher::ProcessCompletedRequests()
{
	if (NumPendingCompletions > 0)
	{
		if (EnumHasAnyFlags(Flags, EUringFlags::IOPoll))
		{
			if (EnumHasAnyFlags(Flags, EUringFlags::SQPoll))
			{
				// If we have a SQPoll thread it will handle polling for us
				io_uring_sqpoll_wake(&Ring);
			}
			else if (io_uring_cq_ready(&Ring) == 0) // If we have any events available during io_uring_enter it will return immediately. So do this to avoid entering the kernel.
			{
				// The problem IOPoll is that the kernel will only poll for single bio requests.
				// And there's a high chance that we don't have any single bio I/Os.
				// And we still have to enter the kernel to check it.
				// All of this doesn't apply for nvme commands.
				VERIFY_URING(io_uring_enter(Ring.enter_ring_fd, 0, 0,  ring_enter_flags(&Ring) | IORING_ENTER_GETEVENTS, nullptr));
			}
		}
		
		const uint32 Ready = io_uring_peek_batch_cqe(&Ring, CompletedCqesBuffer.GetData(), CompletedCqesBuffer.Num()); // May enter the kernel if we are deferring task run or are overflowing.
		if (Ready > 0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::ProcessCompletedRequests);
			{
				FScopeLock Lock(&CompletedRequestsCritical);
				for (uint32 Index = 0; Index < Ready; ++Index)
				{
					ProcessCompletionEvent(CompletedCqesBuffer[Index]);
				}
			}
			WakeUpDispatcherThreadDelegate->Execute();	
			io_uring_cq_advance(&Ring, Ready);
		}
		else
		{
			VERIFY_URING_VALUE(Ready);
		}
	}
}

void FLinuxPlatformIoDispatcher::WaitAndProcessCompletionRequests(const bool bDrain)
{
	if (NumPendingCompletions > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::WaitAndProcessCompletionRequests);
		do
		{
			io_uring_cqe* CompletionEvent = nullptr;
			VERIFY_URING(io_uring_wait_cqe(&Ring, &CompletionEvent));
			{
				FScopeLock Lock(&CompletedRequestsCritical);
				ProcessCompletionEvent(CompletionEvent);
				io_uring_cqe_seen(&Ring, CompletionEvent);
			}
			WakeUpDispatcherThreadDelegate->Execute();	
			ProcessCompletedRequests();
			
		} while (bDrain && NumPendingCompletions > 0);
	}
}

void FLinuxPlatformIoDispatcher::UpdateMemory(const uint64 BlockSize)
{
#if TEST_DIRECTIO_REALLOCATE // TODO: Fix me. Const...
	static uint64 Counter = 0;
	static uint64 UpdateCounter = FMath::RandRange(1, 50);
	const uint64 BlockSize =  FMath::Min<uint64>(++Counter % UpdateCounter == 0 ? DirectIOBufferAlignment * 2 : DirectIOBufferAlignment, 65536); // clamp to 64K
#endif
	
	if (UNLIKELY(BlockSize > BufferAlignment)) // Reallocate. Should be very rare event
	{
		TArray<FFileIoStoreBuffer*> BufferReferences;
		AcquiredBufferProperties.GenerateKeyArray(BufferReferences);
		
		if (BufferReferences.Num() == 1)
		{
			FMemory::Free(DirectIOBuffer);
		}
		else
		{
			// Sort by address
			BufferReferences.Sort([](const FFileIoStoreBuffer& A, const FFileIoStoreBuffer& B)
			{
				return A.Memory < B.Memory;
			});
			
			// Release old buffer
			TArray<FFileIoStoreBuffer*> ActiveReferences = BufferReferences;
			ActiveReferences.Remove(AcquiredBuffer);
			
			// Remove free buffers from active references
			TArray<FFileIoStoreBuffer*> FreeBuffers;
			while (FFileIoStoreBuffer* Buffer = BufferAllocator->AllocBuffer())
			{
				FreeBuffers.Add(Buffer);
				ActiveReferences.Remove(Buffer);
			}
			
			for (FFileIoStoreBuffer* Buffer : FreeBuffers)
			{
				BufferAllocator->FreeBuffer(Buffer);
			}
			
			if (ActiveReferences.IsEmpty())
			{
				FMemory::Free(DirectIOBuffer);
			}
			else
			{
#if TEST_DIRECTIO_REALLOCATE
				UE_LOG(LogLinuxPlatformIO, Display, TEXT("Adding pending release %p, References %d"), DirectIOBuffer, ActiveReferences.Num());
#endif 
				DirectIOPendingReleases.Add(FPendingMemoryRelease{.References = MoveTemp(ActiveReferences), .Memory = DirectIOBuffer});	
			}
		}
		
		BufferAlignment = BlockSize;
		ActualBufferSize = Align(BufferAllocator->GetBufferSize() + 2 * BufferAlignment, BufferAlignment);
		DirectIOBuffer = static_cast<uint8*>(FMemory::Malloc(ActualBufferSize * NumAllocatedBuffers, BufferAlignment));
		
		for (int32 Index = 0; Index < NumAllocatedBuffers; Index++)
		{
			uint8* Ptr = DirectIOBuffer + Index * ActualBufferSize;
			FFileIoStoreBuffer* Buffer = BufferReferences[Index];
			FFileIOStoreBufferProperties& Properties = AcquiredBufferProperties.FindChecked(Buffer);
			
			if (BufferReferences[Index] == AcquiredBuffer)
			{
				BufferReferences[Index]->Memory = Ptr;
				Properties.NewBuffer = nullptr;
				Properties.FixupOffset = 0;
			}
			else
			{
				Properties.NewBuffer = Ptr;
			}
		}
		
		if (EnumHasAnyFlags(Flags, EUringFlags::RegisterBuffers))
		{
			UpdateRegisteredBuffers(DirectIOBuffer);	
		}
	}
	else
	{
		FFileIOStoreBufferProperties& Properties = AcquiredBufferProperties.FindChecked(AcquiredBuffer);
		if (UNLIKELY(Properties.NewBuffer))
		{
			AcquiredBuffer->Memory = Properties.NewBuffer;
			Properties.NewBuffer = nullptr;
			Properties.FixupOffset = 0;
		}
	}
	
	if (UNLIKELY(!DirectIOPendingReleases.IsEmpty()))
	{
		for (auto Iterator(DirectIOPendingReleases.CreateIterator()); Iterator; ++Iterator)
		{
			FPendingMemoryRelease& Release = *(Iterator);
			Release.References.Remove(AcquiredBuffer);
			if (Release.References.IsEmpty())
			{
#if TEST_DIRECTIO_REALLOCATE
				UE_LOG(LogLinuxPlatformIO, Display, TEXT("Releasing memory %p. Num Pending %d"), Release.Memory, DirectIOPendingReleases.Num() - 1);
#endif 
				FMemory::Free(Release.Memory);
				Iterator.RemoveCurrent();
			}
		}
	}
}

void FLinuxPlatformIoDispatcher::AllocateMemory(FFileIoStoreReadRequest* Request, const int32 NvmeDeviceIndex)
{
	if (EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect))
	{
		const uint64 LogicalBlockSize = RegisteredNvmeDevices[NvmeDeviceIndex]->GetLogicalBlockSize();
		UpdateMemory(LogicalBlockSize);
	}
	else if (EnumHasAnyFlags(Flags, EUringFlags::DirectIO))
	{
		check(Request->ContainerFilePartition->FileHandle);
		FLinuxFileHandle* FileHandle = reinterpret_cast<FLinuxFileHandle*>(Request->ContainerFilePartition->FileHandle);
		const uint64 BlockSize = FileHandle->IsDirect() ? FileHandle->GetBlockSize() : 0;
		UpdateMemory(BlockSize);
	}
	Request->Buffer = AcquiredBuffer;
	AcquiredBuffer = nullptr;
}

io_uring_sqe* FLinuxPlatformIoDispatcher::GetSubmissionQueueEvent()
{
	io_uring_sqe* NextSqe = io_uring_get_sqe(&Ring);
	if (LIKELY(NextSqe))
	{
		return NextSqe;
	}
	
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::GetSubmissionEvent);
	do
	{
		if (EnumHasAnyFlags(Flags, EUringFlags::SQPoll))
		{
			VERIFY_URING(io_uring_sqring_wait(&Ring));
		}
		else
		{
			SubmitRequest(nullptr);
		}
		NextSqe = io_uring_get_sqe(&Ring);
	}
	while (!NextSqe);
	
	return NextSqe;
}


void FLinuxPlatformIoDispatcher::PrepareRequestNvme(FFileIoStoreReadRequest* Request, const int32 NvmeDeviceIndex)
{
	check(NvmeDeviceIndex != -1); // Shouldn't happen. SUBMIT_ALL should be supported.
	check(Request->ContainerFilePartition->FileHandle);
	
	FLinuxFileHandle* FileHandle = reinterpret_cast<FLinuxFileHandle*>(Request->ContainerFilePartition->FileHandle);
	TUniquePtr<FNvmeDevice>& NvmeDevice = RegisteredNvmeDevices[NvmeDeviceIndex];
	
	// Make sure the memory location is back to the original start location.
	FFileIOStoreBufferProperties& Properties = AcquiredBufferProperties.FindChecked(Request->Buffer);
	Request->Buffer->Memory -= Properties.FixupOffset;
	Properties.FixupOffset = 0;
	
	const uint64 BlockSize = NvmeDevice->GetLogicalBlockSize();
	const uint64 DataOffset = Request->Offset;
	const uint64 DataSize = Request->Size;
	const uint64 PartitionStartLBA = (NvmeDevice->GetStart() * 512) / BlockSize;
	
	const uint64 DataEnd = DataOffset + DataSize;
	
	const FRegisteredContainer* Container = NvmeDevice->FindContainer(FileHandle);
	if (!Container)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to registered container for file %s"), *FileHandle->GetFilename());
		Request->bFailed = true;
		OnRequestComplete(Request);
		return;
	}
	
	FNvmeRequest* Tracker = NvmeRequestPool.IsEmpty() ? new FNvmeRequest : NvmeRequestPool.Pop();
	Tracker->RemainingRequests = 0;
	Tracker->Request = Request;
	
	// Calculate the N Blocks, 
	// Find the starting extent, Update N Blocks
	// If N blocks is zero break, otherwise continue until N blocks is zero.
	
	// Calculate the aligned size, and fixup offset.
	const uint64 AlignedOffset = AlignDown(DataOffset, BlockSize);
	const uint64 AlignedEnd = Align(DataOffset + DataSize, BlockSize);
	const uint64 AlignedSize = AlignedEnd - AlignedOffset;
	Properties.FixupOffset = DataOffset - AlignedOffset;
	
	uint8* MemoryPtr = Request->Buffer->Memory;
	uint64 RemainingBlocks = AlignedSize / BlockSize;
	bool bFoundStart = false;
	
	const int32 StartExtentIndex = Container->FindStartExtent(AlignedOffset);
	if (UNLIKELY(StartExtentIndex == INDEX_NONE || StartExtentIndex == Container->PhysicalExtents.Num()))
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to find starting extent for file %s at offset %llu"), *FileHandle->GetFilename(), AlignedOffset);
		Request->bFailed = true;
		OnRequestComplete(Request);
		return;
	}
	
	for (int32 CurrentExtentIndex = StartExtentIndex; CurrentExtentIndex < Container->PhysicalExtents.Num(); ++CurrentExtentIndex)
	{
		io_uring_sqe* SubmissionEvent = GetSubmissionQueueEvent();
		SubmissionEvent->opcode = IORING_OP_URING_CMD;
		SubmissionEvent->fd = NvmeDevice->GetFixedFd();
		SubmissionEvent->flags = IOSQE_FIXED_FILE;
		SubmissionEvent->cmd_op = NVME_URING_CMD_IO;
		SubmissionEvent->uring_cmd_flags = IORING_URING_CMD_FIXED;
		SubmissionEvent->buf_index = Properties.BufferIndex;
		SubmissionEvent->user_data = reinterpret_cast<UPTRINT>(Tracker);
		
		const FPhysicalExtent& Extent = Container->PhysicalExtents[CurrentExtentIndex];
		uint64 StartLb, NumLbs;
		
		if (CurrentExtentIndex == StartExtentIndex)
		{
			const uint64 PhysicalByteOffset = (AlignedOffset - Extent.LogicalOffset) + Extent.PhysicalOffset;
			const uint64 BytesAvailableInExtent = (Extent.LogicalOffset + Extent.Length) - AlignedOffset;
			const uint64 BlocksAvailableInExtent = BytesAvailableInExtent / BlockSize;
			if (BlocksAvailableInExtent < RemainingBlocks)
			{
				NumLbs = BlocksAvailableInExtent; // Multiple extents. Slower.
			}
			else
			{
				NumLbs = RemainingBlocks; // Single extent.
			}
			StartLb = (PhysicalByteOffset / BlockSize) + PartitionStartLBA;
		}
		else
		{
			check(AlignedOffset < Extent.LogicalOffset && AlignedOffset + AlignedSize > Extent.LogicalOffset); // Sanity check. Should be continuous.
			
			const uint64 NumAvailableBlocks = Extent.Length / BlockSize;
			if (NumAvailableBlocks > RemainingBlocks)
			{
				NumLbs = RemainingBlocks; // Done
			}
			else
			{
				NumLbs = NumAvailableBlocks; 
			}
			StartLb = (Extent.PhysicalOffset / BlockSize) + PartitionStartLBA;
		}
		
		struct nvme_uring_cmd* Cmd = reinterpret_cast<struct nvme_uring_cmd*>(SubmissionEvent->cmd);
		FMemory::Memzero(Cmd, sizeof(struct nvme_uring_cmd));
			
		const uint64 PhysicalSize = BlockSize * NumLbs;
			
		Cmd->opcode = NVME_CMD_READ; 
		Cmd->nsid = NvmeDevice->GetNamespace();
		Cmd->addr = reinterpret_cast<uint64>(MemoryPtr);
		Cmd->data_len = PhysicalSize;
		Cmd->cdw10 = static_cast<uint32>(StartLb & 0xFFFFFFFF); // Starting Logical Block
		Cmd->cdw11 = static_cast<uint32>(StartLb >> 32);		// Starting Logical Block
		Cmd->cdw12 = static_cast<uint32>(NumLbs - 1) & 0xFFFF;	// Num Logical Blocks. 16 bits. Could add an assert in the setup if the read size is going to be larger than this.
		Cmd->cdw13 |= NVME_RW_DSM_LATENCY_LOW;					// Lowest latency. No idea what this actually does.
			
		++NumPendingCompletions;
			
		++Tracker->RemainingRequests;
			
		RemainingBlocks -= NumLbs;
			
		if (RemainingBlocks == 0)
		{
			break;
		}
			
		MemoryPtr += PhysicalSize;
	}
	
	check(RemainingBlocks == 0);
}

void FLinuxPlatformIoDispatcher::PrepareRequestNormal(FFileIoStoreReadRequest* Request)
{
	io_uring_sqe* SubmissionEvent = GetSubmissionQueueEvent();
	FLinuxFileHandle* FileHandle = reinterpret_cast<FLinuxFileHandle*>(Request->ContainerFilePartition->FileHandle);
	
	uint64 Offset = Request->Offset;
	uint64 Size = Request->Size;
	
	FFileIOStoreBufferProperties& Properties = AcquiredBufferProperties.FindChecked(Request->Buffer);
	
	Request->Buffer->Memory -= Properties.FixupOffset;
	Properties.FixupOffset = 0;
	
	if (FileHandle->IsDirect())
	{
		const uint64 BlockSize = FileHandle->GetBlockSize();
		const uint64 DataOffset = Request->Offset;
		const uint64 DataSize = Request->Size;
		const uint64 AlignedOffset = AlignDown(DataOffset, BlockSize);
		const uint64 AlignedEnd = Align(DataOffset + DataSize, BlockSize);
		
		Size = AlignedEnd - AlignedOffset;
		Properties.FixupOffset = DataOffset - AlignedOffset;
	}
	
	SubmissionEvent->opcode = EnumHasAnyFlags(Flags, EUringFlags::RegisterBuffers) ? IORING_OP_READ_FIXED : IORING_OP_READ;
	SubmissionEvent->buf_index = Properties.BufferIndex;
	SubmissionEvent->flags = IOSQE_FIXED_FILE | GetPriorityFlags();
	SubmissionEvent->fd = FileHandle->GetFd();
	SubmissionEvent->off = Offset;
	SubmissionEvent->len = Size;
	SubmissionEvent->addr = reinterpret_cast<UPTRINT>(Request->Buffer->Memory + Properties.FixupOffset); // Why are we doing this? 
	SubmissionEvent->user_data = reinterpret_cast<UPTRINT>(Request);	
	
	++NumPendingCompletions;
}


void FLinuxPlatformIoDispatcher::IssueRequest(FFileIoStoreReadRequest* Request, const int32 NvmeDeviceIndex)
{
	if (EnumHasAnyFlags(Flags, EUringFlags::NvmeDirect))
	{
		PrepareRequestNvme(Request, NvmeDeviceIndex);
	}
	else
	{
		PrepareRequestNormal(Request);
	}
}

void FLinuxPlatformIoDispatcher::SubmitRequest(FFileIoStoreReadRequest* Request)
{
	if (EnumHasAnyFlags(Flags, EUringFlags::SQPoll))
	{
		VERIFY_URING(io_uring_submit(&Ring));
	}
	else
	{
		if (Request)
		{
			PendingSubmits.Add(Request);
			if (PendingSubmits.Num() < GBatchSubmitSize)
			{
				return;
			}
		}
		
		TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::SubmitRequests);
		do
		{
			const int32 Expected = PendingSubmits.Num();
			const int32 NumSubmitted = io_uring_submit(&Ring);
				
			if (LIKELY(Expected == NumSubmitted || EnumHasAnyFlags(Flags, EUringFlags::SubmitAll)))
			{
				break;
			}
				
			if (NumSubmitted == -EBUSY)
			{
				// Overflow in the completion queue while leaving io_uring_enter. We still successfully submitted all the requests and have to clear the completion queue.
				ProcessCompletedRequests();	
				break;
			}
			VERIFY_URING_VALUE(NumSubmitted);
				
			// CQE should've posted for the failed event. Go past it.
			PendingSubmits.RemoveAt(0, NumSubmitted);
			for (int32 Index = 0; Index < PendingSubmits.Num(); Index++)
			{
				IssueRequest(PendingSubmits[Index], -1);
			}
				
		} while (true);
			
		PendingSubmits.Reset();
	}
}

FFileIoStoreReadRequest* FLinuxPlatformIoDispatcher::GetNextRequest(FFileIoStoreRequestQueue& RequestQueue)
{
	if (!AcquiredBuffer)
	{
		AcquiredBuffer = BufferAllocator->AllocBuffer();
		if (!AcquiredBuffer)
		{
			return nullptr;
		}
	}
	return RequestQueue.Pop();
}

bool FLinuxPlatformIoDispatcher::StartRequests(FFileIoStoreRequestQueue& RequestQueue)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::StartRequests);
	
	const uint64 CyclesStart = FPlatformTime::Cycles64();
	
	if (UNLIKELY(EnumHasAnyFlags(Flags, EUringFlags::EnableRing)))
	{
		EnumRemoveFlags(Flags, EUringFlags::EnableRing);
		VERIFY_URING(io_uring_enable_rings(&Ring));
	}
	
	if (UNLIKELY(EnumHasAnyFlags(Flags, EUringFlags::RegisterRing)))
	{
		EnumRemoveFlags(Flags, EUringFlags::RegisterRing);
		if (const int32 ErrNo = io_uring_register_ring_fd(&Ring); ErrNo != 1)
		{
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to register ring fd, ErrorCode %d, Error %s"), -ErrNo, UTF8_TO_TCHAR(strerror(-ErrNo))); // Not required.
		}
	}
	
	{
		FScopeLock _(&UnregisterCritical);
		if (UNLIKELY(!FilesToUnregister.IsEmpty()))
		{
			for (FLinuxFileHandle* File : FilesToUnregister)
			{
				UnregisterFile(File);
			}
			FilesToUnregister.Reset();
		}
	}
	
	FFileIoStoreReadRequest* NextRequest;
	int32 ResultOrIndex;
	do
	{
		while (true)
		{
			if ((NextRequest = GetNextRequest(RequestQueue)) == nullptr)
			{
				break;
			}
		
			if (NextRequest->bCancelled || NextRequest->bFailed)
			{
				OnRequestComplete(NextRequest);
				continue;
			}
			
			if ((ResultOrIndex = OpenFile(NextRequest)) == INDEX_NONE)
			{
				NextRequest->bFailed = true;
				OnRequestComplete(NextRequest);
				continue;
			}
			
			AllocateMemory(NextRequest, ResultOrIndex);
		
			if (BlockCache->Read(NextRequest))
			{
				OnRequestComplete(NextRequest);
				continue;
			}
			
			IssueRequest(NextRequest, ResultOrIndex);
			
			SubmitRequest(NextRequest);
			
			ProcessCompletedRequests();	
		}
		
		if (!PendingSubmits.IsEmpty())
		{
			SubmitRequest(nullptr);
			ProcessCompletedRequests();	
		}
		
		WaitAndProcessCompletionRequests(false);
		
	} while (NumPendingCompletions > 0);
	
	
	CyclesCounter += FPlatformTime::Cycles64() - CyclesStart;
	
	return false;
}
	
void FLinuxPlatformIoDispatcher::GetCompletedRequests(FFileIoStoreReadRequestList& OutRequests)
{
	FScopeLock _(&CompletedRequestsCritical);
	NumCompletionEventsAhead = 0;
	OutRequests.AppendSteal(CompletedRequests);
}

void FLinuxPlatformIoDispatcher::ServiceNotify()
{
	Event->Trigger();
}
	
void FLinuxPlatformIoDispatcher::ServiceWait()
{
	Event->Wait();
}