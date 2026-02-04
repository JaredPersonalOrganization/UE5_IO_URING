#include "LinuxPlatformIoDispatcher.h"

#include <netinet/ip.h>
#include <sys/eventfd.h>
#include <sys/resource.h>

#include "IPlatformFilePak.h"
#include "LinuxFileHandle.h"
#include "LinuxPlatformIoDispatcherModule.h"
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
	TEXT("Whether to use a SQPoll thread. This isn't a free performance switch. Currently it makes the performance worse.\n"),
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

int32 GDirectIODefaultBufferAlignment = 4096;
static FAutoConsoleVariableRef CVarDirectIODefaultBufferAlignment(
	TEXT("r.Linux.Streaming.DirectIOBufferAlignment"),
	GDirectIODefaultBufferAlignment,
	TEXT("Default alignment to for DirectIO buffers in bytes. Increases automatically if a larger alignment is required."),
	ECVF_ReadOnly
);

int32 GMaxNumOpenFiles = 1024;
static FAutoConsoleVariableRef CVarMaxOpenFiles(
	TEXT("r.Linux.Streaming.MaxFixedFiles"),
	GMaxNumOpenFiles,
	TEXT("Maximum number of fixed Files. Does not change the process max. If you need more files, increase the process max too. Default 1024\n"),
	ECVF_ReadOnly
);

int32 GMaxPendingRequests = -1;
static FAutoConsoleVariableRef CVarMaxPendingRequests(
	TEXT("r.Linux.Streaming.MaxPendingRequests"),
	GMaxNumOpenFiles,
	TEXT("Sets the maximum number of in submission queue entries. Default matches the number of read buffers.\n"),
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
	TEXT("Maximum number of bounded workers created io_uring. Loosely enforced. Supported since version 5.15")
		 TEXT("By default io_uring limits the number of bounded workers by the SQ Ring size and the number of CPUs on the system."),
		 ECVF_ReadOnly
);

int32 GIoWqMaxUnboundedWorkers = 0;
static FAutoConsoleVariableRef CVarIoWqMaxUnboundedWorkers(
	TEXT("r.Linux.Streaming.IoWqMaxUnboundedWorkers"),
	GIoWqMaxUnboundedWorkers,
	TEXT("Maximum number of Unbounded workers created io_uring. Loosely enforced. Supported since version 5.15")
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

#define VERIFY_URING_SAFE(Function, StoredResult) \
{ \
	StoredResult = true; \
	const int32 UringFunctionResult = Function; \
	if(UNLIKELY(UringFunctionResult < 0)) \
	{  \
		StoredResult = false; \
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("IoUring Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(-UringFunctionResult)), -UringFunctionResult, UTF8_TO_TCHAR(#Function), UTF8_TO_TCHAR(__FILE__), __LINE__); \
	} \
}

#define VERIFY_LINUX_SAFE(Function, StoredResult) \
{ \
	StoredResult = true; \
	const int32 LinuxFunctionResult = Function; \
	if(UNLIKELY(LinuxFunctionResult == -1)) \
	{ \
		StoredResult = false; \
		const int32 ErrNo = errno; \
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Linux Error %s, Code %d, Function %s, File %s, Line %u"), UTF8_TO_TCHAR(strerror(ErrNo)), ErrNo, UTF8_TO_TCHAR(#Function), UTF8_TO_TCHAR(__FILE__), __LINE__); \
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
}
	
void FLinuxPlatformIoDispatcher::Initialize(const FInitializePlatformFileIoStoreParams& Params)
{
	WakeUpDispatcherThreadDelegate = Params.WakeUpDispatcherThreadDelegate;
	BufferAllocator = Params.BufferAllocator;
	BlockCache = Params.BlockCache;
	Stats = Params.Stats;
	FinalizeUring();
}

void FLinuxPlatformIoDispatcher::RegisterFile(class FLinuxFileHandle* File)
{
	checkf(!FreeRegisteredFiles.IsEmpty(), TEXT("Ran out of registered files!"));
	check(File->GetState() == FLinuxFileHandle::Opened);
			
	const int32 FixedHandle = FreeRegisteredFiles.Pop(EAllowShrinking::No);
	const int32 Fd = File->GetHandle();
	
	VERIFY_URING(io_uring_register_files_update(&Ring, FixedHandle, &Fd, 1));
	
	File->Close(); 
	File->UpdateHandle(FixedHandle, FLinuxFileHandle::Fixed);
}

void FLinuxPlatformIoDispatcher::UnregisterFile(class FLinuxFileHandle* File)
{
	if (File->GetState() == FLinuxFileHandle::Fixed)
	{
		constexpr int32 NewFd = -1;
		const int32 FixedHandle = File->GetHandle();
		VERIFY_URING(io_uring_register_files_update(&Ring, FixedHandle, &NewFd, 1));
		FreeRegisteredFiles.Add(FixedHandle);
	}
	delete File;
}
	
bool FLinuxPlatformIoDispatcher::OpenContainer(const TCHAR* ContainerFilePath, uint64& ContainerFileHandle, uint64& ContainerFileSize)
{
	if (FLinuxFileHandle* Handle = FLinuxFileHandle::CreateFileHandle(ContainerFilePath, GUseDirectIO))
	{
		Handle->Close(); 
		ContainerFileSize = Handle->GetSize();
		ContainerFileHandle = reinterpret_cast<UPTRINT>(Handle);
		return true;
	}
	return false;
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

bool FLinuxPlatformIoDispatcher::OpenFile(FFileIoStoreReadRequest* Request)
{
	check(Request->ContainerFilePartition->FileHandle);
	FLinuxFileHandle* FileHandle = reinterpret_cast<FLinuxFileHandle*>(Request->ContainerFilePartition->FileHandle);
	if (FileHandle->GetState() != FLinuxFileHandle::Fixed)
	{
		if (!FileHandle->Open())
		{
			return false;
		}
		RegisterFile(FileHandle); 
	}
	return true;
}
	
bool FLinuxPlatformIoDispatcher::CreateCustomRequests(FFileIoStoreResolvedRequest& ResolvedRequest, FFileIoStoreReadRequestList& OutRequests)
{
	return false;
}

void FLinuxPlatformIoDispatcher::UpdateRegisteredBuffers(uint8* Memory,const uint64 SizePerBuffer, const uint64 NumBuffers)
{
	const bool bNeedsReset = !RegisteredBuffers.IsEmpty();
	RegisteredBuffers.Reset();
	constexpr uint64 MaxBufferSize =  1ull << 30; // Max 1GiB
	const uint64 TotalSize = SizePerBuffer * NumBuffers;
	if (TotalSize > MaxBufferSize) 
	{
		// Break it up into smaller chunks
		const uint64 RegisteredBuffersPerAlloc = MaxBufferSize / SizePerBuffer;
		const uint64 SizePerAlloc = RegisteredBuffersPerAlloc * SizePerBuffer;
					
		uint64 BufferIndex = 0;
		while (BufferIndex + RegisteredBuffersPerAlloc <= NumBuffers) 
		{
			uint8* Current = Memory + BufferIndex * SizePerBuffer;
			RegisteredBuffers.Add(iovec{.iov_base = Current, .iov_len = SizePerAlloc});
			BufferIndex += RegisteredBuffersPerAlloc;
		}
					
		if (BufferIndex < NumBuffers) 
		{
			uint32 Remaining = NumAllocatedBuffers - BufferIndex;
			uint8* Current = Memory + BufferIndex * SizePerBuffer;
			RegisteredBuffers.Add(iovec{.iov_base = Current, .iov_len = SizePerBuffer * Remaining});
		}
	}
	else
	{
		RegisteredBuffers.Add(iovec{.iov_base = Memory, .iov_len = TotalSize});
	}
	
	if (bNeedsReset)
	{
		SubmitRequest(nullptr);
		WaitAndProcessCompletionRequests(true);
		VERIFY_URING(io_uring_unregister_buffers(&Ring));
	}
	
	VERIFY_URING(io_uring_register_buffers(&Ring, RegisteredBuffers.GetData(), RegisteredBuffers.Num()));
	
	UE_LOG(LogLinuxPlatformIO, Display, TEXT("Registered %d buffers with %llu size for io_uring"), RegisteredBuffers.Num(), SizePerBuffer);
}

int32 FLinuxPlatformIoDispatcher::GetRegisteredBufferOffset(const uint8* Memory)
{
	// Check where the pointer resides inside the registered buffers.
	for (int32 RegisteredBufferIndex = 0; RegisteredBufferIndex < RegisteredBuffers.Num(); ++RegisteredBufferIndex)
	{
		uint8* Base = static_cast<uint8*>(RegisteredBuffers[RegisteredBufferIndex].iov_base);
		const uint64 Length  = RegisteredBuffers[RegisteredBufferIndex].iov_len;

		if (Memory >= Base && Memory < Base + Length)
		{
			return RegisteredBufferIndex;
		}
	}
	return INDEX_NONE;
}

TUniquePtr<FLinuxPlatformIoDispatcher> FLinuxPlatformIoDispatcher::Create()
{
	TUniquePtr<FLinuxPlatformIoDispatcher> Impl = MakeUnique<FLinuxPlatformIoDispatcher>(FPrivateToken());
	if (!Impl->InitializeUring())
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to create io_uring dispatcher. Falling back to default"));
		return nullptr;
	}
	return MoveTemp(Impl);
}

int32 FLinuxPlatformIoDispatcher::CreateRing()
{
	const bool bIsMultiThreaded = FGenericPlatformProcess::SupportsMultithreading();
	
	int32 MaxPendingRequests = GMaxPendingRequests;
	if (GMaxPendingRequests == -1)
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
	if (bIsMultiThreaded && GUseSQPollThread)
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
		bMustEnableRing = false;
		if (io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params) == 0)
		{
			return 0;
		}
		
		Params.flags &= ~IORING_SETUP_SUBMIT_ALL;
		bSubmitAll = false;
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
		Params.flags &= ~IORING_SETUP_SUBMIT_ALL;

		bSubmitAll = false;
		return io_uring_queue_init_params(MaxPendingRequests, &Ring, &Params);
	}
}

void FLinuxPlatformIoDispatcher::FinalizeUring()
{
#if TEST_WORST_CASE
	bUseRegisteredBuffers = false;
#endif
	
	if (GUseDirectIO || bUseRegisteredBuffers)
	{
		// Set up DirectIO or RegisteredBuffers
		TArray<FFileIoStoreBuffer*> Buffers;
		while (FFileIoStoreBuffer* Buffer = BufferAllocator->AllocBuffer())
		{
			Buffers.Add(Buffer);
		}
		
		Buffers.Sort([](const FFileIoStoreBuffer& A, const FFileIoStoreBuffer& B)
		{
			return A.Memory < B.Memory;
		});
		check(!Buffers.IsEmpty());
		
		NumAllocatedBuffers = Buffers.Num();
		uint64 BufferSize = BufferAllocator->GetBufferSize();
		
		if (GUseDirectIO)
		{
			DirectIOFixupOffsets.Reserve(NumAllocatedBuffers);
			DirectIOBufferAlignment = GDirectIODefaultBufferAlignment;
			
			// This would cause a double free in FFileIoStoreBufferAllocator but luckily for us, they don't free it.
			FMemory::Free(Buffers[0]->Memory);
			
			// Reallocate the buffers with the correct size
			const uint64 ReadBufferSize = BufferAllocator->GetBufferSize();
			DirectIOBufferSize = Align(ReadBufferSize + 2 * DirectIOBufferAlignment, DirectIOBufferAlignment);
			const uint64 TotalSize = DirectIOBufferSize * NumAllocatedBuffers;
			
			BufferSize = DirectIOBufferSize;
			
			DirectIOBuffer = static_cast<uint8*>(FMemory::Malloc(TotalSize, DirectIOBufferAlignment));
			for (int64 Index = 0; Index < NumAllocatedBuffers; Index++)
			{
				Buffers[Index]->Memory = DirectIOBuffer + Index * DirectIOBufferSize;
				DirectIOFixupOffsets.Add(Buffers[Index], 0);
			}
		}
		
		if (GRegisterBuffers)
		{
			UpdateRegisteredBuffers(Buffers[0]->Memory, BufferSize, NumAllocatedBuffers);
		}
		
		for (int32 Index = 0; Index < Buffers.Num(); Index++)
		{
			BufferAllocator->FreeBuffer(Buffers[Index]); 
		}
	}
}

bool FLinuxPlatformIoDispatcher::InitializeUring()
{
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
	
	bool bCallResult = true;
	
	// Set up registered files.
	{
		rlimit RLimit;
		VERIFY_LINUX_SAFE(getrlimit(RLIMIT_NOFILE, &RLimit), bCallResult)
		if (!bCallResult)
		{
			return bCallResult;
		}
		
		TArray<int32> SparseHandles;
		SparseHandles.SetNum(FMath::Min<int32>(RLimit.rlim_cur, GMaxNumOpenFiles));
		for (int32 Index = 0; Index < SparseHandles.Num(); Index++)
		{
			SparseHandles[Index] = -1;
			FreeRegisteredFiles.Add(Index);
		}
		
		VERIFY_URING_SAFE(io_uring_register_files(&Ring, SparseHandles.GetData(), SparseHandles.Num()), bCallResult)
		if (!bCallResult)
		{
			return bCallResult;
		}
		
		Algo::Reverse(SparseHandles);
	}
	
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
				VERIFY_URING_SAFE(io_uring_register_iowq_max_workers(&Ring, Workers), bCallResult); // Loosely enforced
			}
		}
	}

	if (GRegisterBuffers)
	{
		// Get RLIMIT_MEMLOCK.
		bUseRegisteredBuffers = true;
		rlimit RLimit;
		VERIFY_LINUX_SAFE(getrlimit(RLIMIT_MEMLOCK, &RLimit), bCallResult);
		if (!bCallResult)
		{
			bUseRegisteredBuffers = false;
		}
		else
		{
			// Check if we have enough space to fit registered buffers
			auto DispatcherBufferMemory = IConsoleManager::Get().FindConsoleVariable(TEXT("s.IoDispatcherBufferMemoryMB"));
			check(DispatcherBufferMemory);
			
#if TEST_LARGE_BUFFER_SIZES
			DispatcherBufferMemory->Set(2152); // Set the dispatcher buffer memory greater than 1GB to make sure registration is still working.
#endif 
			
			
			const uint64 TotalBufferSize = static_cast<uint64>(DispatcherBufferMemory->GetInt()) << 20ull;
			
			if (TotalBufferSize > RLimit.rlim_cur)
			{
				UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Dropping registered buffers for io_uring. Not enough space within locked memory. Has %llu, Needs %llu"), RLimit.rlim_cur, TotalBufferSize);
				bUseRegisteredBuffers = false;
			}
			
			// DirectIO imposes additional restrictions for memory
			if (GUseDirectIO)
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
					bUseRegisteredBuffers = false;
				}
			}
			
			if (bUseRegisteredBuffers)
			{
				char Data[1024];
				iovec Dummy {.iov_base = Data, .iov_len = sizeof(Data)};
				VERIFY_URING_SAFE(io_uring_register_buffers(&Ring, &Dummy, 1), bCallResult);
				if (!bCallResult)
				{
					bUseRegisteredBuffers = false;
				}
				else
				{
					io_uring_unregister_buffers(&Ring);
				}
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
	FFileIoStoreReadRequest* Request = reinterpret_cast<FFileIoStoreReadRequest*>(CompletionEvent->user_data); 
	Stats->OnFilesystemReadCompleted(Request);
	
	if (Request->Size != CompletionEvent->res)
	{
		UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to complete read request. Code %d, Error %s, Request %p"), -CompletionEvent->res, UTF8_TO_TCHAR(strerror(-CompletionEvent->res)), Request);
		Request->bFailed = true;
	}
	else if (GUseDirectIO)
	{
		const uint64 FixupOffset = DirectIOFixupOffsets.FindChecked(Request->Buffer);
		Request->Buffer->Memory = Request->Buffer->Memory + FixupOffset;
	}
	
	CompletedRequests.Add(Request);
	--NumPendingCompletions;
	++NumCompletionEventsAhead;
}

void FLinuxPlatformIoDispatcher::ProcessCompletedRequests()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::ProcessCompletedRequests);
	const uint32 Ready = io_uring_peek_batch_cqe(&Ring, CompletedCqesBuffer.GetData(), CompletedCqesBuffer.Num()); // May enter the kernel for GetEvents if we are overflowing.
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

void FLinuxPlatformIoDispatcher::AllocateDirectMemory(FFileIoStoreReadRequest* Request)
{
	check(Request->ContainerFilePartition->FileHandle);
	
	FLinuxFileHandle* FileHandle = reinterpret_cast<FLinuxFileHandle*>(Request->ContainerFilePartition->FileHandle);
	
#if TEST_DIRECTIO_REALLOCATE
	static uint64 Counter = 0;
	static uint64 UpdateCounter = FMath::RandRange(1, 50);
	const uint64 BlockSize =  FMath::Min<uint64>(++Counter % UpdateCounter == 0 ? DirectIOBufferAlignment * 2 : DirectIOBufferAlignment, 65536); // clamp to 64K
#else 
	const uint64 BlockSize = FileHandle->IsDirect() ? FileHandle->GetBlockSize() : 0;
#endif
	
	if (UNLIKELY(BlockSize > DirectIOBufferAlignment)) // Reallocate. Should be rare event
	{
		UE_LOG(LogLinuxPlatformIO, Display, TEXT("Updating BlockSize from %llu to %llu for Request %p"), DirectIOBufferAlignment, BlockSize, Request);
		
		TArray<FFileIoStoreBuffer*> BufferReferences;
		DirectIOFixupOffsets.GenerateKeyArray(BufferReferences);
		
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
			
			
			// Create an entry to pending releases.	
			TArray<FFileIoStoreBuffer*> Temp = BufferReferences;
			Temp.Remove(AcquiredBuffer);
			
			// Acquire and release the free buffers
			TArray<FFileIoStoreBuffer*> FreeBuffers;
			while (FFileIoStoreBuffer* Buffer = BufferAllocator->AllocBuffer())
			{
				FreeBuffers.Add(Buffer);
			}
			
			for (FFileIoStoreBuffer* Buffer : FreeBuffers)
			{
				Temp.Remove(Buffer);
				BufferAllocator->FreeBuffer(Buffer);
			}
			
			if (!NewDirectIOBuffers.IsEmpty()) // Reallocated recently. Remove the references
			{
				TArray<FFileIoStoreBuffer*> BuffersToRemove;
				NewDirectIOBuffers.GenerateKeyArray(BuffersToRemove);
				
				for (FFileIoStoreBuffer* Buffer : BuffersToRemove)
				{
					Temp.Remove(Buffer);
				}
				NewDirectIOBuffers.Reset();
			}
			if (Temp.IsEmpty())
			{
				FMemory::Free(DirectIOBuffer);
			}
			else
			{
#if TEST_DIRECTIO_REALLOCATE
				UE_LOG(LogLinuxPlatformIO, Display, TEXT("Adding Pending Release. Num Refs %d, Memory %p"), Temp.Num(), DirectIOBuffer);
#endif 
				DirectIOPendingReleases.Add(FPendingMemoryRelease{.References = MoveTemp(Temp), .Memory = DirectIOBuffer});		
			}
		}
		
		DirectIOBufferAlignment = BlockSize;
		DirectIOBufferSize = Align(BufferAllocator->GetBufferSize() + 2 * DirectIOBufferAlignment, DirectIOBufferAlignment);
		const uint64 TotalSize = DirectIOBufferSize * NumAllocatedBuffers;
		DirectIOBuffer = static_cast<uint8*>(FMemory::Malloc(TotalSize, DirectIOBufferAlignment));
		
		for (int64 Index = 0; Index < NumAllocatedBuffers; Index++)
		{
			uint8* Ptr = DirectIOBuffer + Index * DirectIOBufferSize;
			
			if (BufferReferences[Index] == AcquiredBuffer)
			{
				BufferReferences[Index]->Memory = Ptr;
			}
			else
			{
				NewDirectIOBuffers.Add(BufferReferences[Index], Ptr);	
			}
		}
		
		if (GRegisterBuffers)
		{
			UpdateRegisteredBuffers(DirectIOBuffer, DirectIOBufferSize, NumAllocatedBuffers);	
		}
	}
	else
	{
		uint8* NewMemoryStart = nullptr;
		if (!NewDirectIOBuffers.IsEmpty() && NewDirectIOBuffers.RemoveAndCopyValue(AcquiredBuffer, NewMemoryStart))
		{
			AcquiredBuffer->Memory = NewMemoryStart; // Update the memory. Already at the starting position
		}
		else
		{
			const uint64 CurrentOffset = DirectIOFixupOffsets.FindChecked(AcquiredBuffer);	
			AcquiredBuffer->Memory -= CurrentOffset; // Back to starting position
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
	
	// Reset the fixup offset to zero.
	DirectIOFixupOffsets.Add(AcquiredBuffer, 0);
}

io_uring_sqe* FLinuxPlatformIoDispatcher::GetSubmissionQueueEvent()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::GetSubmissionEvent);
	
	io_uring_sqe* NextSqe = io_uring_get_sqe(&Ring);
	if (LIKELY(NextSqe))
	{
		return NextSqe;
	}
	
	do
	{
		if (GUseSQPollThread)
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

void FLinuxPlatformIoDispatcher::IssueDirectIORequest(FFileIoStoreReadRequest* Request, FLinuxFileHandle* File, io_uring_sqe* SubmissionEvent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::IssueDirectIORequest);
	
	const uint64 BlockSize = File->GetBlockSize();
			
	// Fixup the alignments
	const uint64 DataOffset = Request->Offset;
	const uint64 DataSize = Request->Size;
	const uint64 AlignedOffset = AlignDown(DataOffset, BlockSize);
	const uint64 AlignedEnd = Align(DataOffset + DataSize, BlockSize);
			
	const uint64 AlignedSize = AlignedEnd - AlignedOffset;
	const uint64 FixupOffset = DataOffset - AlignedOffset;
	
	check(DirectIOBufferSize >= AlignedSize);
	
	Stats->OnFilesystemReadStarted(Request);
	
	if (GRegisterBuffers)
	{
		// Segment points to a location in a registered Memory Buffer
		const int32 BufferIndex = GetRegisteredBufferOffset(Request->Buffer->Memory);
		check(BufferIndex != INDEX_NONE);
		
		SubmissionEvent->buf_index = BufferIndex;
		SubmissionEvent->opcode = IORING_OP_READ_FIXED;
	}
	else
	{
		SubmissionEvent->opcode = IORING_OP_READ;
	}
	
	SubmissionEvent->flags = IOSQE_FIXED_FILE | GetPriorityFlags();
	SubmissionEvent->fd = File->GetHandle();
	SubmissionEvent->off = AlignedOffset;
	SubmissionEvent->len = AlignedSize;
	SubmissionEvent->addr = reinterpret_cast<UPTRINT>(Request->Buffer->Memory + FixupOffset);
	SubmissionEvent->user_data = reinterpret_cast<UPTRINT>(Request);
	
	// Update the segment because the fixup offset has changed. 
	DirectIOFixupOffsets.Add(Request->Buffer, FixupOffset);
}

void FLinuxPlatformIoDispatcher::IssueNormalIORequest(FFileIoStoreReadRequest* Request, FLinuxFileHandle* File, io_uring_sqe* SubmissionEvent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::IssueNormalIORequest);
		
	Stats->OnFilesystemReadStarted(Request);
	
	if (GRegisterBuffers)
	{
		SubmissionEvent->buf_index = GetRegisteredBufferOffset(Request->Buffer->Memory);
		SubmissionEvent->opcode = IORING_OP_READ_FIXED;
	}
	else
	{
		SubmissionEvent->opcode = IORING_OP_READ;
	}
		
	SubmissionEvent->flags = IOSQE_FIXED_FILE | GetPriorityFlags();
	SubmissionEvent->fd = File->GetHandle();
	SubmissionEvent->off = Request->Offset;
	SubmissionEvent->len = Request->Size;
	SubmissionEvent->addr = reinterpret_cast<uintptr_t>(Request->Buffer->Memory);
	SubmissionEvent->user_data = reinterpret_cast<UPTRINT>(Request);
}

void FLinuxPlatformIoDispatcher::IssueRequest(FFileIoStoreReadRequest* Request)
{
	io_uring_sqe* SubmissionEvent = GetSubmissionQueueEvent();
	FLinuxFileHandle* FileHandle = reinterpret_cast<FLinuxFileHandle*>(Request->ContainerFilePartition->FileHandle);
	
	uint64 FixupOffset = 0;
	uint64 Offset = Request->Offset;
	uint64 Size = Request->Size;
	
	if (FileHandle->IsDirect())
	{
		const uint64 BlockSize = FileHandle->GetBlockSize();
		const uint64 DataOffset = Offset;
		const uint64 DataSize = Size;
		
		Offset = AlignDown(DataOffset, BlockSize);
		const uint64 AlignedEnd = Align(DataOffset + DataSize, BlockSize);
			
		Size = AlignedEnd - Offset;
		FixupOffset = DataOffset - Offset;
		
		DirectIOFixupOffsets.Add(Request->Buffer, FixupOffset);
	}
	
	if (GRegisterBuffers)
	{
		SubmissionEvent->buf_index = GetRegisteredBufferOffset(Request->Buffer->Memory);
		SubmissionEvent->opcode = IORING_OP_READ_FIXED;
	}
	else
	{
		SubmissionEvent->opcode = IORING_OP_READ;
	}
	
	SubmissionEvent->flags = IOSQE_FIXED_FILE | GetPriorityFlags();
	SubmissionEvent->fd = FileHandle->GetHandle();
	SubmissionEvent->off = Offset;
	SubmissionEvent->len = Size;
	SubmissionEvent->addr = reinterpret_cast<UPTRINT>(Request->Buffer->Memory + FixupOffset);
	SubmissionEvent->user_data = reinterpret_cast<UPTRINT>(Request);
	++NumPendingCompletions;
}

void FLinuxPlatformIoDispatcher::SubmitRequest(FFileIoStoreReadRequest* Request)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::SubmitRequests);
	
	if (GUseSQPollThread)
	{
		VERIFY_URING(io_uring_submit(&Ring));
	}
	else
	{
		// Read section IORING_SETUP_SUBMIT_ALL in https://man7.org/linux/man-pages/man2/io_uring_setup.2.html for more information about submission. 
		bool bShouldSubmit = Request == nullptr;
		if (Request)
		{
			// TODO: See if we can use EventsAhead to determine the optimal batch size
			// const int32 NumEventsAhead = NumCompletionEventsAhead.load(std::memory_order_acquire);
			
			PendingSubmits.Add(Request);
			if (PendingSubmits.Num() >= GBatchSubmitSize)
			{
				bShouldSubmit = true;
			}
		}
		
		if (bShouldSubmit)
		{
			// Read section IORING_SETUP_SUBMIT_ALL in https://man7.org/linux/man-pages/man2/io_uring_setup.2.html for more information about submission.
			do
			{
				const int32 Expected = PendingSubmits.Num();
				const int32 NumSubmitted = io_uring_submit(&Ring);
				
				if (LIKELY(Expected == NumSubmitted || bSubmitAll))
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
				
				if (PendingSubmits.Num() == NumSubmitted)
				{
					// All request should have a posted CQE
					break;
				}
				
				// CQE should've posted for the failed event. Go past it.
				PendingSubmits.RemoveAt(0, NumSubmitted);
				for (int32 Index = 0; Index < PendingSubmits.Num(); Index++)
				{
					if (GUseDirectIO)
					{
						const int32 Offset = DirectIOFixupOffsets.FindChecked(PendingSubmits[Index]->Buffer);
						PendingSubmits[Index]->Buffer->Memory -= Offset;
					}
					IssueRequest(PendingSubmits[Index]);
				}
				
			} while (true);
			
			PendingSubmits.Reset();
		}
	}
}


bool FLinuxPlatformIoDispatcher::StartRequests(FFileIoStoreRequestQueue& RequestQueue)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLinuxPlatformIoDispatcherImpl::StartRequests);
	
	const uint64 CyclesStart = FPlatformTime::Cycles64();
	
	if (UNLIKELY(bMustEnableRing))
	{
		bMustEnableRing = false;
		VERIFY_URING(io_uring_enable_rings(&Ring));
	}
	
	if (UNLIKELY(bRingNeedsRegistering))
	{
		bRingNeedsRegistering = false;
		if (const int32 ErrNo = io_uring_register_ring_fd(&Ring); ErrNo != 1)
		{
			UE_LOG(LogLinuxPlatformIO, Warning, TEXT("Failed to register ring fd, ErrorCode %d, Error %s"), -ErrNo, UTF8_TO_TCHAR(strerror(-ErrNo)));
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
	
	do
	{
		while (true)
		{
			if (!AcquiredBuffer)
			{
				AcquiredBuffer = BufferAllocator->AllocBuffer();
				if (!AcquiredBuffer)
				{
					break;
				}
			}
		
			FFileIoStoreReadRequest* NextRequest = RequestQueue.Pop();
			if (!NextRequest)
			{
				break;
			}
		
			if (NextRequest->bCancelled || NextRequest->bFailed)
			{
				OnRequestComplete(NextRequest);
				continue;
			}
		
			if (!OpenFile(NextRequest))
			{
				NextRequest->bFailed = true;
				OnRequestComplete(NextRequest);
				continue;
			}
			
			if (GUseDirectIO)
			{
				AllocateDirectMemory(NextRequest);
			}
		
			NextRequest->Buffer = AcquiredBuffer;
			AcquiredBuffer = nullptr;
		
			if (BlockCache->Read(NextRequest))
			{
				OnRequestComplete(NextRequest);
				continue;
			}
			
			IssueRequest(NextRequest);
			
			SubmitRequest(NextRequest);
			
			ProcessCompletedRequests();	
		}
		
		if (PendingSubmits.Num() > 0)
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