

# About

Implementation of io_uring for Unreal Engine. Designed to be used with cooked content. 
Requires no modifications to the source code of Unreal Engine, and is considerably faster than the default.


# Benchmarks

### Loading 180000 Uncompressed Textures(87GB) with FStreamableManager.

###### Fixed Files, Registered Ring/Buffer, Defer Taskrun[1], Single Issuer[1], Coop Taskrun[1], Submit All,

```c++
// Benchmark code.
// Note Unreal Engine was modified to prevent the texture from uploading to the GPU.
void AUringBenchmark::LoadTextures(TSharedPtr<struct FStreamableHandle> Streamable)
{
	constexpr int32 BatchSize = 200;
	if (Streamable)
	{
		// Releases Textures so we don't go OOM...
	}
	
	if (TextureIndex >= TexturePaths.Num())
	{
		double TimeElapsed = FPlatformTime::Seconds() - LoadingStartTime;
		// Output Logs... We record cycles with our dispatcher.
		return;
	}
	TArray<FSoftObjectPath> AssetPaths;
	AssetPaths.Reserve(BatchSize);
	const int32 Last = TextureIndex + BatchSize;
	for (; TextureIndex < Last && TextureIndex < TexturePaths.Num(); ++TextureIndex)
	{
		AssetPaths.Add(TexturePaths[RandomOrdering[TextureIndex]]); // Random ordering
	}
	
	FStreamableAsyncLoadParams Params {.TargetsToStream = MoveTemp(AssetPaths), .OnComplete = FStreamableDelegateWithHandle::CreateUObject(this, &APCDesignerMain::LoadTextures)};
	UAssetManager::Get().GetStreamableManager().RequestAsyncLoad(MoveTemp(Params));
}
```

| Backend   | Batch Size | Priority | SQPoll | IOPoll | NVME PassThrough | Avg TimeElapsed (s) | Avg Actual Time (s) | Improvement vs Default (%) |
|-----------|------------|----------|--------|--------|------------------|---------------------|---------------------|-----------------------------|
| io_uring  | 4          | High     | ✕      | ✕      | ✕                | 56.05               | 14.91               | 49.05%                      |
| io_uring  | 4          | Normal   | ✕      | ✕      | ✕                | 66.46               | 21.99               | 39.58%                      |
| io_uring  | 4          | —        | ✓      | ✓      | ✓                | 52.80               | 11.44               | 52.00%                      |
| io_uring  | 4          | —        | ✕      | ✓      | ✓                | 51.44               | 11.23               | 53.24%                      |
| io_uring  | 16         | High     | ✕      | ✕      | ✕                | 55.90               | 15.12               | 49.18%                      |
| io_uring  | 16         | Normal   | ✕      | ✕      | ✕                | 67.54               | 22.75               | 38.60%                      |
| io_uring  | 16         | —        | ✓      | ✓      | ✓                | 52.67               | 11.41               | 52.12%                      |
| io_uring  | 16         | —        | ✕      | ✓      | ✓                | 51.36               | 11.25               | 53.31%                      |
| io_uring  | 32         | High     | ✕      | ✕      | ✕                | 56.31               | 15.13               | 48.81%                      |
| io_uring  | 32         | Normal   | ✕      | ✕      | ✕                | 67.39               | 22.53               | 38.74%                      |
| io_uring  | 32         | —        | ✓      | ✓      | ✓                | 53.06               | 11.41               | 51.76%                      |
| io_uring  | 32         | —        | ✕      | ✓      | ✓                | 51.88               | 11.31               | 52.84%                      |
| Default   | —          | —        | —      | —      | —                | 110.00              | —                   | 0.00%                       |

###### [1] Certain flags were removed when SQPoll was used.


# Features
- Priority - Useful for during non-interactive moments. Adds force async flag to every request to effectively punt request to worker threads rather than attempt to complete it inline. Use carefully as it does increase CPU usage.
- SQPoll - Is slow and not worth the cost in most situations.
- IoPoll - Isn't useful most of the time because the way Linux manages multi bio requests. User must enable nvme.poll_queues for it to work.
- DirectIO - Is slow and only useful with IOPoll.
- NVME Passthrough - Very fast, but not portable. Needs more testing.
- Fixed Buffers
- Fixed Files
- Registered Ring
- Falls back to default if not supported.


# Installation
- Clone the repository into the plugins folder of your project
- Enable the plugin


# Console Variables

```r.Linux.Streaming.RegisterBuffers``` Whether to register buffers. May have a performance improvement.

```r.Linux.Streaming.UseSQPollThread``` Whether to use a SQPoll thread. This isn't a free performance switch. Currently makes the performance worse.

```r.Linux.Streaming.SqPollIdleTimeMS``` SQPoll sleep timer,

```r.Linux.Streaming.UseDirectIO``` Tries to use O_DIRECT. Experimental. Does not increase performance.

```r.Linux.Streaming.UseIOPoll``` Tries to use IOPoll. Checks /sys/module/nvme/parameters/poll_queues to see if it's available. Do not use this. Most of the time it will not work.

```r.Linux.Streaming.NVMEPassthrough``` Whether use NVME Passthrough directly with io_uring. Experimental.

```r.Linux.Streaming.DefaultBufferAlignment``` This value can different meanings. When used with DirectIO it represents optimal read size, when used with NvmeDirect it represents the logical block size. For both of these, it will increase if necessary.

```r.Linux.Streaming.MaxFixedFiles``` Maximum number of fixed Files. Does not change the process max. If you need more files, increase the process max too. Default 1024

```r.Linux.Streaming.QueueDepth``` Sets the queue depth of the submission queue entries. Default matches the number of read buffers.

```r.Linux.Streaming.BatchSize``` Batch size before we submit. Be careful setting this value too high because it can starve decompression workers.

```r.Linux.Streaming.IoWqMaxBoundedWorkers``` Maximum number of bounded workers created io_uring. Softly enforced. Supported since version 5.15.

```r.Linux.Streaming.IoWqMaxUnboundedWorkers``` Maximum number of Unbounded workers created io_uring. Softly enforced. Supported since version 5.15.
