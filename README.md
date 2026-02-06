

# About

Implementation of io_uring for Unreal Engine. Designed to be used with cooked content. Requires no modifications to the source code of Unreal Engine, and is considerably faster than the default.


# Benchmarks

### Loading 180000 Textures 87GB~ with FStreamableManager 

| Backend / Priority   | Batch Size | Avg TimeElapsed (s) | Avg Actual Time (s) |
|----------------------| ---------- | ------------------- | ------------------- |
| LinuxIOUring / Normal | 4          | 59.335              | 19.168              |
| LinuxIOUring / Normal | 16         | 59.302              | 19.152              |
| LinuxIOUring / Normal | 32         | 60.091              | 19.762              |
| LinuxIOUring / High  | 4          | 51.131              | 9.707               |
| LinuxIOUring / High  | 16         | 51.074              | 9.802               |
| LinuxIOUring / High  | 32         | 51.007              | 9.834               |
| Default              | —          | 83.318              | N/A                 |

### Loading 180000 Textures 87GB~ with FBulkDataBatchRequest::FBatchBuilder
 
| Backend / Priority    | Batch Size | Avg TimeElapsed (s) | Avg Actual Time (s) |
| --------------------- | ---------- | ------------------- | ------------------- |
| LinuxIOUring / Normal | 4          | 27.287              | 19.667              |
| LinuxIOUring / Normal | 16         | 26.662              | 19.319              |
| LinuxIOUring / Normal | 32         | 26.776              | 19.586              |
| LinuxIOUring / High   | 4          | 17.524              | 9.743               |
| LinuxIOUring / High   | 16         | 17.611              | 9.924               |
| LinuxIOUring / High   | 32         | 17.505              | 9.918               |
| Default               | —          | 52.281              | N/A                 |



# Installation

- Clone the repository into the plugins folder of your project
- Enable the plugin
- That's it!


# Features
- Has 'Go Faster' button. Enabled with ```SetLoadingPriority```. It forces every request to async punted to a kernel worker thread rather than executed inline. Use carefully as it does increase CPU usage.
- SQPoll - is slow and doesn't do anything
- IoPoll - Doesn't work most of the time because of how Linux handles bio / polling
- DirectIO - is slow but interesting.
- Falls back to the default dispatcher if the platform does not support io_uring.


# Console Variables

```r.Linux.Streaming.RegisterBuffers``` - Whether to register buffers. May have a performance improvement. Default Enabled.

```r.Linux.Streaming.UseSQPollThread``` - Whether to use a SQPoll thread. This isn't a free performance button. It currently makes the performance worse. Default Disabled.

```r.Linux.Streaming.SqPollIdleTimeMS``` - SQPoll sleep timer. Default 50.

```r.Linux.Streaming.UseDirectIO``` - Tries to use O_DIRECT. Experimental. Does not increase performance. Default Disabled.

```r.Linux.Streaming.DirectIOBufferAlignment```  - Default alignment to for DirectIO buffers in bytes. Increases automatically if a larger alignment is required. Default 4096.

```r.Linux.Streaming.MaxOpenFiles``` - Maximum number of fixed Files. Does not change the process max. If you need more files, increase the process max too. Default 1024.

```r.Linux.Streaming.MaxPendingRequests``` - Sets the maximum number of in submission queue entries. Default matches the number of read buffers.

```r.Linux.Streaming.BatchSize``` - Batch size before we submit. Should be small to prevent starving decompression workers. Don't set lower than 2, otherwise this is a glorified read(). Default 4.

```r.Linux.Streaming.IoWqMaxBoundedWorkers``` - Maximum number of bounded workers created io_uring. Softly enforced. Supported since version 5.15. Default 0(Disabled).

```r.Linux.Streaming.IoWqMaxUnboundedWorkers``` - Maximum number of Unbounded workers created io_uring. Softly enforced. Supported since version 5.15. Default 0(Disabled)

```r.Linux.Streaming.UseIOPoll``` - Tries to use IOPoll. Checks /sys/module/nvme/parameters/poll_queues to see if it's available. Do not use this. Most of the time it will not work. Default Disabled.