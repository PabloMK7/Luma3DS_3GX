#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include "plugin.h"
#include "ifile.h"
#include "utils.h"

#define MEMPERM_RW (MEMPERM_READ | MEMPERM_WRITE)

static const char *g_swapPath = "/luma/plugins/.swap";

Result      MemoryBlock__IsReady(void)
{
    PluginLoaderContext *ctx = &PluginLoaderCtx;
    MemoryBlock *memblock = &ctx->memblock;

    if (memblock->isReady)
        return 0;

    Result  res;

    if (isN3DS)
    {
        s64     appRegionSize = 0;
        s64     appRegionUsed = 0;
        u32     appRegionFree;
        u32     gameReserveSize;
        vu32*   appMemAllocPtr = (vu32 *)PA_FROM_VA_PTR(0x1FF80040);
        u32     appMemAlloc = *appMemAllocPtr;
        u32     temp;

        svcGetSystemInfo(&appRegionSize, 0x10000, 6);
        svcGetSystemInfo(&appRegionUsed, 0, 1);

        appRegionFree = appRegionSize - appRegionUsed;

        // Check if appmemalloc reports the entire available memory
        if ((u32)appRegionSize == appMemAlloc)
            *appMemAllocPtr -= MemBlockSize; ///< Remove plugin share from available memory

        gameReserveSize = appRegionFree - MemBlockSize;

        // First reserve the game memory size (to avoid heap relocation)
        res = svcControlMemoryUnsafe((u32 *)&temp, 0x30000000,
                                    gameReserveSize, MEMOP_REGION_APP | MEMOP_ALLOC | MEMOP_LINEAR_FLAG, MEMPERM_RW);

        // Then allocate our plugin memory block
        if (R_SUCCEEDED(res))
            res = svcControlMemoryUnsafe((u32 *)&memblock->memblock, 0x07000000,
                                        MemBlockSize, MEMOP_REGION_APP | MEMOP_ALLOC | MEMOP_LINEAR_FLAG, MEMPERM_RW);

        // Finally release game reserve block
        if (R_SUCCEEDED(res))
            res = svcControlMemoryUnsafe((u32 *)&temp, temp, gameReserveSize, MEMOP_FREE, 0);
    }
    else
    {
        res = svcControlMemoryUnsafe((u32 *)&memblock->memblock, 0x07000000,
                                    MemBlockSize, MEMOP_REGION_SYSTEM | MEMOP_ALLOC | MEMOP_LINEAR_FLAG, MEMPERM_RW);
    }

    if (R_FAILED(res))
        PluginLoader__Error("Couldn't allocate memblock", res);
    else
    {
        // Clear the memblock
        memset(memblock->memblock, 0, MemBlockSize);
        memblock->isReady = true;

        /*if (isN3DS)
        {
            // Check if appmemalloc reports the entire available memory
            s64     appRegionSize = 0;
            vu32*   appMemAlloc = (vu32 *)PA_FROM_VA_PTR(0x1FF80040);

            svcGetSystemInfo(&appRegionSize, 0x10000, 6);
            if ((u32)appRegionSize == *appMemAlloc)
                *appMemAlloc -= MemBlockSize; ///< Remove plugin share from available memory
        } */
    }

    return res;
}

Result      MemoryBlock__Free(void)
{
    MemoryBlock *memblock = &PluginLoaderCtx.memblock;

    if (!memblock->isReady)
        return 0;

    MemOp   memRegion = isN3DS ? MEMOP_REGION_APP : MEMOP_REGION_SYSTEM;
    Result  res = svcControlMemoryUnsafe((u32 *)&memblock->memblock, (u32)memblock->memblock,
                                    MemBlockSize, memRegion | MEMOP_FREE, 0);

    memblock->isReady = false;
    memblock->memblock = NULL;

    if (R_FAILED(res))
        PluginLoader__Error("Couldn't free memblock", res);

    return res;
}

#define FS_OPEN_RWC (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE)

Result      MemoryBlock__ToSwapFile(void)
{
    MemoryBlock *memblock = &PluginLoaderCtx.memblock;

    u64     written = 0;
    u64     toWrite = MemBlockSize;
    IFile   file;
    Result  res = 0;

    svcFlushDataCacheRange(memblock->memblock, MemBlockSize);
    res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                    fsMakePath(PATH_ASCII, g_swapPath), FS_OPEN_RWC);

    if (R_FAILED(res)) return res;

    res = IFile_Write(&file, &written, memblock->memblock, toWrite, FS_WRITE_FLUSH);

    if (R_FAILED(res) || written != toWrite)
        svcBreak(USERBREAK_ASSERT); ///< TODO: Better error handling

    IFile_Close(&file);
    return res;
}

Result      MemoryBlock__FromSwapFile(void)
{
    MemoryBlock *memblock = &PluginLoaderCtx.memblock;

    u64     read = 0;
    u64     toRead = MemBlockSize;
    IFile   file;
    Result  res = 0;

    res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                    fsMakePath(PATH_ASCII, g_swapPath), FS_OPEN_READ);

    if (R_FAILED(res))
        svcBreak(USERBREAK_ASSERT); ///< TODO: Better error handling

    res = IFile_Read(&file, &read, memblock->memblock, toRead);

    if (R_FAILED(res) || read != toRead)
        svcBreak(USERBREAK_ASSERT); ///< TODO: Better error handling

    svcFlushDataCacheRange(memblock->memblock, MemBlockSize);
    IFile_Close(&file);
    return res;
}

Result     MemoryBlock__MountInProcess(void)
{
    Handle          target = PluginLoaderCtx.target;
    Error           *error = &PluginLoaderCtx.error;
    PluginHeader    *header = &PluginLoaderCtx.header;
    MemoryBlock     *memblock = &PluginLoaderCtx.memblock;

    Result       res = 0;

    // Executable
    if (R_FAILED((res = svcMapProcessMemoryEx(target, 0x07000000, CUR_PROCESS_HANDLE, (u32)memblock->memblock, header->exeSize))))
    {
        error->message = "Couldn't map exe memory block";
        error->code = res;
        return res;
    }

    // Heap (to be used by the plugin)
    if (R_FAILED((res = svcMapProcessMemoryEx(target, header->heapVA, CUR_PROCESS_HANDLE, (u32)memblock->memblock + header->exeSize, header->heapSize))))
    {
        error->message = "Couldn't map heap memory block";
        error->code = res;
    }

    return res;
}

Result     MemoryBlock__UnmountFromProcess(void)
{
    Handle          target = PluginLoaderCtx.target;
    PluginHeader    *header = &PluginLoaderCtx.header;

    Result  res = 0;

    res = svcUnmapProcessMemoryEx(target, 0x07000000, header->exeSize);
    res |= svcUnmapProcessMemoryEx(target, header->heapVA, header->heapSize);

    return res;
}
