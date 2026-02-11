#pragma once
#include "CoreMinimal.h"
#include "LinuxPlatformIoDispatcherModule.h"
#include "Runtime/PakFile/Internal/IoDispatcherFileBackendTypes.h"
#include "uring/liburing.h"

class FLinuxPlatformIOMemory;

struct FLinuxVersionHelper
{
	int32 Major = -1;
	int32 Minor = -1;
	
	bool IsSupported(const int32 InMajor, const int32 InMinor) const
	{
		if (Major > InMajor)
		{
			return true;
		}
		if (Major == InMajor)
		{
			if (Minor >= InMinor)
			{
				return true;
			}
		}
		return false;
	}
};

struct FPendingMemoryRelease
{
	TArray<FFileIoStoreBuffer*> References;
	uint8* Memory = nullptr;
};


struct FFileIOStoreBufferProperties
{
	uint64 FixupOffset = 0;
	uint8* NewBuffer = nullptr;
	int32 BufferIndex = 0;
};


struct FNvmeRequest
{
	FFileIoStoreReadRequest* Request = nullptr;
	int32 RemainingRequests = 0;
	uint32 ExpectedLbas = 0;
};


enum class EUringFlags
{
	EnableRing = (1 << 0),
	SubmitAll = (1 << 1),
	RegisterRing = (1 << 2),
	RegisterBuffers = (1 << 3),
	DirectIO = (1 << 4),
	IOPoll = (1 << 5),
	SQPoll = (1 << 6),
	NvmeDirect = (1 << 7),
	
	Default = RegisterRing | EnableRing | SubmitAll,
};

ENUM_CLASS_FLAGS(EUringFlags);



class FLinuxPlatformIoDispatcher : public IPlatformFileIoStore
{
	struct FPrivateToken
	{
		explicit FPrivateToken() = default;
	};
public:
	FLinuxPlatformIoDispatcher(FPrivateToken);
	
	virtual ~FLinuxPlatformIoDispatcher() override;
	
	virtual void Initialize(const FInitializePlatformFileIoStoreParams& Params) override;
	
	virtual bool OpenContainer(const TCHAR* ContainerFilePath, uint64& ContainerFileHandle, uint64& ContainerFileSize) override;
	
	virtual void CloseContainer(uint64 ContainerFileHandle) override;
	
	virtual bool CreateCustomRequests(FFileIoStoreResolvedRequest& ResolvedRequest, FFileIoStoreReadRequestList& OutRequests) override;
	
	virtual bool StartRequests(FFileIoStoreRequestQueue& RequestQueue) override;
	
	virtual void GetCompletedRequests(FFileIoStoreReadRequestList& OutRequests) override;

	virtual void ServiceNotify() override;
	
	virtual void ServiceWait() override;
	
	static EDispatcherPriority GetLoadingPriority()
	{
		return Priority;
	}
	
	static void UpdatePriority(const EDispatcherPriority InPriority)
	{
		Priority = InPriority;
	}
	
	static void ResetCyclesCounter()
	{
		CyclesCounter = 0;
	}
	
	static uint64 GetCycleCounter()
	{
		return CyclesCounter;
	}
	
	static TUniquePtr<FLinuxPlatformIoDispatcher> Create();
private:
	uint8 GetPriorityFlags() const;
	
	int32 OpenFile(FFileIoStoreReadRequest* Request);
	
	void ProcessCompletedRequests();
	
	void RegisterFile(class FLinuxFileHandle* File);
	
	void UnregisterFile(class FLinuxFileHandle* File);
	
	void UpdateRegisteredBuffers(uint8* Start);
	
	void UpdateRegisteredBuffersOffset();
	
	void Finalize();
	
	bool Initialize();
	
	int32 CreateRing();
	
	int32 RegisterNvmeFile(class FLinuxFileHandle* File);
	
	void UnregisterNvmeFile(class FLinuxFileHandle* File);
	
	void UpdateMemory(const uint64 BlockSize);
	
	void AllocateMemory(FFileIoStoreReadRequest* Request, const int32 NvmeDeviceIndex);
	
	io_uring_sqe* GetSubmissionQueueEvent();
	
	FFileIoStoreReadRequest* GetNextRequest(FFileIoStoreRequestQueue& RequestQueue);
	
	void SubmitRequest(FFileIoStoreReadRequest* Request);
	
	void PrepareRequestNvme(FFileIoStoreReadRequest* Request, const int32 NvmeDeviceIndex);
	
	void PrepareRequestNormal(FFileIoStoreReadRequest* Request);
	
	void IssueRequest(FFileIoStoreReadRequest* Request, const int32 NvmeDeviceIndex);
	
	void ProcessCompletionEvent(io_uring_cqe* CompletionEvent);
	
	void WaitAndProcessCompletionRequests(const bool bDrain);
	
	void OnRequestComplete(FFileIoStoreReadRequest* Request);
private:
	static std::atomic<EDispatcherPriority> Priority;
	static std::atomic<uint64> CyclesCounter;
	
	const FWakeUpIoDispatcherThreadDelegate* WakeUpDispatcherThreadDelegate = nullptr;
	FFileIoStoreBufferAllocator* BufferAllocator = nullptr;
	FFileIoStoreBlockCache* BlockCache = nullptr;
	FFileIoStoreStats* Stats = nullptr;
	FFileIoStoreBuffer* AcquiredBuffer = nullptr;
	
	TArray<TUniquePtr<class FNvmeDevice>> RegisteredNvmeDevices;
	TArray<FNvmeRequest*> NvmeRequestPool;
	
	io_uring Ring = {};
	
	uint8* DirectIOBuffer = nullptr;
	uint32 BufferAlignment = 0;
	uint64 ActualBufferSize = 0;
	
	TArray<FPendingMemoryRelease> DirectIOPendingReleases;
	TMap<FFileIoStoreBuffer*, FFileIOStoreBufferProperties> AcquiredBufferProperties;

	FCriticalSection CompletedRequestsCritical;
	FFileIoStoreReadRequestList CompletedRequests;
	
	TArray<FFileIoStoreReadRequest*> PendingSubmits;
	
	FCriticalSection UnregisterCritical;
	TArray<class FLinuxFileHandle*> FilesToUnregister;
	
	TArray<iovec> RegisteredBuffers;
	TArray<int32> FreeRegisteredFiles;
	
	TArray<io_uring_cqe*> CompletedCqesBuffer;
	
	FEventRef Event;
	
	std::atomic_int NumCompletionEventsAhead = 0;
	int32 NumPendingCompletions = 0;
	int32 CurrentAlignment = -1;
	int32 NumAllocatedBuffers = 0;
	int32 NumPollQueues = 0;
	
	EUringFlags Flags = EUringFlags::Default;
};
