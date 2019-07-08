#include <3ds.h>
#include "3gx.h"
#include "ifile.h"
#include "utils.h" // for makeARMBranch
#include "plgloader.h"
#include "plgldr.h"
#include "fmt.h"
#include "menu.h"
#include "menus.h"
#include "memory.h"
#include "sleep.h"

#define MEMPERM_RW (MEMPERM_READ | MEMPERM_WRITE)
#define PLGLDR_VERSION (SYSTEM_VERSION(1, 0, 0))
#define MemBlockSize (5*1024*1024) /* 5 MiB */
#define THREADVARS_MAGIC  0x21545624 // !TV$

typedef struct
{
    Result          code;
    const char *    message;
}   Error;

typedef struct
{
    bool    isReady;
    u8 *    memblock;
}   MemoryBlock;

typedef struct
{
    bool    isEnabled;
    bool    noFlash;
    u32     titleid;
    char    path[256];
    u32     config[32];
}   PluginLoadParametersI;

#define HeaderMagic (0x24584733) /* "3GX$" */

typedef enum
{
    PLG_CFG_NONE = 0,
    PLG_CFG_RUNNING = 1,
    PLG_CFG_SWAPPED = 2,

    PLG_CFG_SWAP_EVENT = 1 << 16,
    PLG_CFG_EXIT_EVENT = 2 << 16
}   PLG_CFG_STATUS;

static bool         g_isEnabled;
static MemoryBlock  g_memBlock = {0};
static PluginHeader g_pluginHeader = {0};
static char         g_path[256];
static const char * g_pathCurrent;
static s32          g_plgEvent = PLG_OK;
static s32          g_plgReply = PLG_OK;
static s32 *        g_plgEventPA;
static s32 *        g_plgReplyPA;
static Handle       g_process = 0;
static Handle       g_arbiter = 0;
static Error        g_error;
static PluginLoadParametersI   g_userDefinedLoadParameters;

static MyThread     g_pluginLoaderThread;
static u8 ALIGN(8)  g_pluginLoaderThreadStack[0x4000];

static const char *g_title = "Plugin loader";
static const char *g_defaultPath = "/luma/plugins/default.3gx";
static const char *g_swapPath = "/luma/plugins/.swap";

// pluginLoader.s
void        gamePatchFunc(void);
void        IR__Patch(void);
void        IR__Unpatch(void);

static void PluginLoader__ThreadMain(void);
MyThread *  PluginLoader__CreateThread(void)
{
    s64 out;

    svcGetSystemInfo(&out, 0x10000, 0x102);
    g_isEnabled = out & 1;
    g_userDefinedLoadParameters.isEnabled = false;
    g_plgEventPA = (s32 *)PA_FROM_VA_PTR(&g_plgEvent);
    g_plgReplyPA = (s32 *)PA_FROM_VA_PTR(&g_plgReply);
    if(R_FAILED(MyThread_Create(&g_pluginLoaderThread, PluginLoader__ThreadMain, g_pluginLoaderThreadStack, 0x4000, 20, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &g_pluginLoaderThread;
}

bool        PluginLoader__IsEnabled(void)
{
    return g_isEnabled;
}

void        PluginLoader__MenuCallback(void)
{
    g_isEnabled = !g_isEnabled;
    SaveSettings();
    PluginLoader__UpdateMenu();
}

void        PluginLoader__UpdateMenu(void)
{
    static const char *status[2] =
    {
        "Plugin Loader: [Disabled]",
        "Plugin Loader: [Enabled]"
    };

    rosalinaMenu.items[isN3DS + 1].title = status[g_isEnabled];
}

static u32     strlen16(const u16 *str)
{
    if (!str) return 0;

    const u16 *strEnd = str;

    while (*strEnd) ++strEnd;

    return strEnd - str;
}


void CheckMemory(void);

static Result      MemoryBlock__IsReady(void)
{
    if (g_memBlock.isReady)
        return 0;

    Result  res;
    //MemOp   memRegion = isN3DS ? MEMOP_REGION_APP : MEMOP_REGION_SYSTEM;

    if (isN3DS)
    {
        s64     appRegionSize = 0;
        s64     appRegionUsed = 0;
        u32     appRegionFree;
        u32     gameReserveSize;
        vu32*   appMemAllocPtr = (vu32 *)PA_FROM_VA_PTR(0x1FF80040);
        u32     appMemAlloc = *appMemAllocPtr;
      //  u32     memop = MEMOP_REGION_APP | MEMOP_ALLOC | MEMOP_LINEAR_FLAG;
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
            res = svcControlMemoryUnsafe((u32 *)&g_memBlock.memblock, 0x07000000,
                                        MemBlockSize, MEMOP_REGION_APP | MEMOP_ALLOC | MEMOP_LINEAR_FLAG, MEMPERM_RW);

        // Finally release game reserve block
        if (R_SUCCEEDED(res))
            res = svcControlMemoryUnsafe((u32 *)&temp, temp, gameReserveSize, MEMOP_FREE, 0);
    }
    else
    {
        res = svcControlMemoryUnsafe((u32 *)&g_memBlock.memblock, 0x07000000,
                                    MemBlockSize, MEMOP_REGION_SYSTEM | MEMOP_ALLOC | MEMOP_LINEAR_FLAG, MEMPERM_RW);
    }

    if (R_FAILED(res))
        DispErrMessage(g_title, "Couldn't allocate memblock", res);
    else
    {
        // Clear the memblock
        memset(g_memBlock.memblock, 0, MemBlockSize);
        g_memBlock.isReady = true;

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

static Result      MemoryBlock__Free(void)
{
    if (!g_memBlock.isReady)
        return 0;

    MemOp   memRegion = isN3DS ? MEMOP_REGION_APP : MEMOP_REGION_SYSTEM;
    Result  res = svcControlMemoryUnsafe((u32 *)&g_memBlock.memblock, (u32)g_memBlock.memblock,
                                    MemBlockSize, memRegion | MEMOP_FREE, 0);

    g_memBlock.isReady = false;
    g_memBlock.memblock = NULL;

    if (R_FAILED(res))
        DispErrMessage(g_title, "Couldn't free memblock", res);

    return res;
}

#define FS_OPEN_RWC (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE)

static Result      MemoryBlock__ToSwapFile(void)
{
    u64     written = 0;
    u64     toWrite = MemBlockSize;
    IFile   file;
    Result  res = 0;

    svcFlushDataCacheRange(g_memBlock.memblock, MemBlockSize);
    res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                    fsMakePath(PATH_ASCII, g_swapPath), FS_OPEN_RWC);

    if (R_FAILED(res)) return res;

    res = IFile_Write(&file, &written, g_memBlock.memblock, toWrite, FS_WRITE_FLUSH);

    if (R_FAILED(res) || written != toWrite)
        svcBreak(USERBREAK_ASSERT); ///< TODO: Better error handling

    IFile_Close(&file);
    return res;
}

static Result      MemoryBlock__FromSwapFile(void)
{
    u64     read = 0;
    u64     toRead = MemBlockSize;
    IFile   file;
    Result  res = 0;

    res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                    fsMakePath(PATH_ASCII, g_swapPath), FS_OPEN_READ);

    if (R_FAILED(res))
        svcBreak(USERBREAK_ASSERT); ///< TODO: Better error handling

    res = IFile_Read(&file, &read, g_memBlock.memblock, toRead);

    if (R_FAILED(res) || read != toRead)
        svcBreak(USERBREAK_ASSERT); ///< TODO: Better error handling

    svcFlushDataCacheRange(g_memBlock.memblock, MemBlockSize);
    IFile_Close(&file);
    return res;
}

static Result     MemoryBlock__MountInProcess(void)
{
    Result       res = 0;

    // Executable
    if (R_FAILED((res = svcMapProcessMemoryEx(g_process, 0x07000000, CUR_PROCESS_HANDLE, (u32)g_memBlock.memblock, g_pluginHeader.exeSize))))
    {
        g_error.message = "Couldn't map exe memory block";
        g_error.code = res;
        return res;
    }

    // Heap (to be used by the plugin)
    if (R_FAILED((res = svcMapProcessMemoryEx(g_process, g_pluginHeader.heapVA, CUR_PROCESS_HANDLE, (u32)g_memBlock.memblock + g_pluginHeader.exeSize, g_pluginHeader.heapSize))))
    {
        g_error.message = "Couldn't map heap memory block";
        g_error.code = res;
        goto exit;
    }

exit:
    return res;
}

static Result     MemoryBlock__UnmountFromProcess(void)
{
    Result  res = 0;

    res = svcUnmapProcessMemoryEx(g_process, 0x07000000, g_pluginHeader.exeSize);
    res |= svcUnmapProcessMemoryEx(g_process, g_pluginHeader.heapVA, g_pluginHeader.heapSize);

    return res;
}

// Update config memory field(used by k11 extension)
static void     SetConfigMemoryStatus(u32 status)
{
    *(vu32 *)PA_FROM_VA_PTR(0x1FF800F0) = status;
}

static u32      GetConfigMemoryEvent(void)
{
    return (*(vu32 *)PA_FROM_VA_PTR(0x1FF800F0)) & ~0xFFFF;
}

static Result   FindPluginFile(u64 tid)
{
    char                filename[256];
    u32                 entriesNb = 0;
    bool                found = false;
    Handle              dir = 0;
    Result              res;
    FS_Archive          sdmcArchive = 0;
    FS_DirectoryEntry   entries[10];

    sprintf(g_path, "/luma/plugins/%016llX", tid);

    if (R_FAILED((res =FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))))
        goto exit;

    if (R_FAILED((res = FSUSER_OpenDirectory(&dir, sdmcArchive, fsMakePath(PATH_ASCII, g_path)))))
        goto exit;

    strcat(g_path, "/");
    while (!found && R_SUCCEEDED(FSDIR_Read(dir, &entriesNb, 10, entries)))
    {
        if (entriesNb == 0)
            break;

        static const u16 *   validExtension = u"3gx";

        for (u32 i = 0; i < entriesNb; ++i)
        {
            FS_DirectoryEntry *entry = &entries[i];

            // If entry is a folder, skip it
            if (entry->attributes & 1)
                continue;

            // Check extension
            u32 size = strlen16(entry->name);
            if (size <= 5)
                continue;

            u16 *fileExt = entry->name + size - 3;

            if (memcmp(fileExt, validExtension, 3 * sizeof(u16)))
                continue;

            // Convert name from utf16 to utf8
            int units = utf16_to_utf8((u8 *)filename, entry->name, 100);
            if (units == -1)
                continue;
            filename[units] = 0;
            found = true;
            break;
        }
    }

    if (!found)
        res = MAKERESULT(28, 4, 0, 1018);
    else
    {
        u32 len = strlen(g_path);
        filename[256 - len] = 0;
        strcat(g_path, filename);
        g_pathCurrent = g_path;
    }

exit:
    FSDIR_Close(dir);
    FSUSER_CloseArchive(sdmcArchive);

    return res;
}

static Result   OpenFile(IFile *file, const char *path)
{
    return IFile_Open(file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_READ);
}

static Result   OpenPluginFile(u64 tid, IFile *plugin)
{
    if (R_FAILED(FindPluginFile(tid)) || OpenFile(plugin, g_path))
    {
        // Try to open default plugin
        if (OpenFile(plugin, g_defaultPath))
            return -1;

        g_pathCurrent = g_defaultPath;
        g_pluginHeader.isDefaultPlugin = 1;
        return 0;
    }

    return 0;
}

static Result   CheckPluginCompatibility(_3gx_Header *header, u32 processTitle)
{
    static char   errorBuf[0x100];

    if (header->targets.count == 0)
        return 0;

    for (u32 i = 0; i < header->targets.count; ++i)
    {
        if (header->targets.titles[i] == processTitle)
            return 0;
    }

    sprintf(errorBuf, "The plugin - %s -\nis not compatible with this game.\n" \
                      "Contact \"%s\" for more infos.", header->infos.titleMsg, header->infos.authorMsg);
    g_error.message = errorBuf;

    return -1;
}

void    PLG__NotifyEvent(PLG_Event event, bool signal);
static bool     TryToLoadPlugin(Handle process)
{
    u64             tid;
    u64             fileSize;
    IFile           plugin;
    Result          res;
    _3gx_Header     *header;
    _3gx_Executable *exeHdr;

    // Get title id
    svcGetProcessInfo((s64 *)&tid, process, 0x10001);

    memset(&g_pluginHeader, 0, sizeof(PluginHeader));
    g_pluginHeader.magic = HeaderMagic;

    // Try to open plugin file
    if (g_userDefinedLoadParameters.isEnabled && (u32)tid == g_userDefinedLoadParameters.titleid)
    {
        g_userDefinedLoadParameters.isEnabled = false;
        if (OpenFile(&plugin, g_userDefinedLoadParameters.path))
            return false;

        g_pathCurrent = g_userDefinedLoadParameters.path;
        if (MemoryBlock__IsReady())
        {
            IFile_Close(&plugin);
            return false;
        }

        memcpy(g_pluginHeader.config, g_userDefinedLoadParameters.config, 32 * sizeof(u32));
    }
    else
    {
        if (R_FAILED(OpenPluginFile(tid, &plugin)))
            return false;

        if (MemoryBlock__IsReady())
        {
            IFile_Close(&plugin);
            return false;
        }
    }

    if (R_FAILED((res = IFile_GetSize(&plugin, &fileSize))))
        g_error.message = "Couldn't get file size";

    // Check 3GX file signature
    if ((res = Check_3gx_Magic(&plugin)))
        g_error.message = "File signature mismatch!\nCheck your plugin file and for an update.";
    // Plugins will not exceed 1MB so this is fine
    header = (_3gx_Header *)(g_memBlock.memblock + MemBlockSize - (u32)fileSize);

    // Read header
    if (!res && R_FAILED((res = Read_3gx_Header(&plugin, header))))
        g_error.message = "Couldn't read file";

    // Check titles compatibility
    if (!res) res = CheckPluginCompatibility(header, (u32)tid);

    // Read code
    if (!res && R_FAILED(res = Read_3gx_LoadSegments(&plugin, header, g_memBlock.memblock + sizeof(PluginHeader))))
        g_error.message = "Couldn't read plugin's code";

    if (R_FAILED(res))
    {
        g_error.code = res;
        goto exitFail;
    }

    g_pluginHeader.version = header->version;
    // Code size must be page aligned
    exeHdr = &header->executable;
    g_pluginHeader.exeSize = (sizeof(PluginHeader) + exeHdr->codeSize + exeHdr->rodataSize + exeHdr->dataSize + exeHdr->bssSize + 0x1000) & ~0xFFF;
    g_pluginHeader.heapVA = 0x06000000;
    g_pluginHeader.heapSize = MemBlockSize - g_pluginHeader.exeSize;
    g_pluginHeader.plgldrEvent = g_plgEventPA;
    g_pluginHeader.plgldrReply = g_plgReplyPA;

    // Clear old event data
    PLG__NotifyEvent(PLG_OK, false);

    // Copy header to memblock
    memcpy(g_memBlock.memblock, &g_pluginHeader, sizeof(PluginHeader));
    // Clear heap
    memset(g_memBlock.memblock + g_pluginHeader.exeSize, 0, g_pluginHeader.heapSize);

    // Enforce RWX mmu mapping
    svcControlProcess(process, PROCESSOP_SET_MMU_TO_RWX, 0, 0);
    // Ask the kernel to signal when the process is about to be terminated
    svcControlProcess(process, PROCESSOP_SIGNAL_ON_EXIT, 0, 0);

    // Mount the plugin memory in the process
    if (R_SUCCEEDED(MemoryBlock__MountInProcess()))
    // Install hook
    {
        u32  procStart = 0x00100000;
        u32 *game = (u32 *)procStart;

        extern u32  g_savedGameInstr[2];

        if (R_FAILED((res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, procStart, process, procStart, 0x1000))))
        {
            g_error.message = "Couldn't map process";
            g_error.code = res;
            goto exitFail;
        }

        g_savedGameInstr[0] = game[0];
        g_savedGameInstr[1] = game[1];

        game[0] = 0xE51FF004; // ldr pc, [pc, #-4]
        game[1] = (u32)PA_FROM_VA_PTR(gamePatchFunc);
        svcFlushEntireDataCache();
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, procStart, 0x1000);
    }
    else
        goto exitFail;


    IFile_Close(&plugin);
    return true;

exitFail:
    IFile_Close(&plugin);
    MemoryBlock__Free();

    return false;
}

static void     PluginLoader_HandleCommands(void)
{
    u32    *cmdbuf = getThreadCommandBuffer();

    switch (cmdbuf[0] >> 16)
    {
        case 1: // Load plugin
        {
            if (cmdbuf[0] != IPC_MakeHeader(1, 0, 2))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            g_process = cmdbuf[2];

            if (g_isEnabled && TryToLoadPlugin(g_process))
            {
                if (!g_userDefinedLoadParameters.isEnabled && g_userDefinedLoadParameters.noFlash)
                    g_userDefinedLoadParameters.noFlash = false;
                else
                {
                    // A little flash to notify the user that the plugin is loaded
                    for (u32 i = 0; i < 64; i++)
                    {
                        REG32(0x10202204) = 0x01FF9933;
                        svcSleepThread(5000000);
                    }
                    REG32(0x10202204) = 0;
                }
                IR__Patch();
                SetConfigMemoryStatus(PLG_CFG_RUNNING);
            }
            else
            {
                svcCloseHandle(g_process);
                g_process = 0;
            }

            cmdbuf[0] = IPC_MakeHeader(1, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 2: // Check if plugin loader is enabled
        {
            if (cmdbuf[0] != IPC_MakeHeader(2, 0, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            cmdbuf[0] = IPC_MakeHeader(2, 2, 0);
            cmdbuf[1] = 0;
            cmdbuf[2] = (u32)g_isEnabled;
            break;
        }

        case 3: // Enable / Disable plugin loader
        {
            if (cmdbuf[0] != IPC_MakeHeader(3, 1, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            if (cmdbuf[1] != g_isEnabled)
            {
                g_isEnabled = cmdbuf[1];
                SaveSettings();
                PluginLoader__UpdateMenu();
            }

            cmdbuf[0] = IPC_MakeHeader(3, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 4: // Define next plugin load settings
        {
            if (cmdbuf[0] != IPC_MakeHeader(4, 2, 4))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            g_userDefinedLoadParameters.isEnabled = true;
            g_userDefinedLoadParameters.noFlash = cmdbuf[1];
            g_userDefinedLoadParameters.titleid = cmdbuf[2];
            strncpy(g_userDefinedLoadParameters.path, (const char *)cmdbuf[4], 255);
            memcpy(g_userDefinedLoadParameters.config, (void *)cmdbuf[6], 32 * sizeof(u32));

            cmdbuf[0] = IPC_MakeHeader(4, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 5: // Display menu
        {
            if (cmdbuf[0] != IPC_MakeHeader(5, 1, 8))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            u32 nbItems = cmdbuf[1];
            u32 states = cmdbuf[3];
            DisplayPluginMenu(cmdbuf);

            cmdbuf[0] = IPC_MakeHeader(5, 1, 2);
            cmdbuf[1] = 0;
            cmdbuf[2] = IPC_Desc_Buffer(nbItems, IPC_BUFFER_RW);
            cmdbuf[3] = states;
            break;
        }

        case 6: // Display message
        {
            if (cmdbuf[0] != IPC_MakeHeader(6, 0, 4))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            const char *title = (const char *)cmdbuf[2];
            const char *body = (const char *)cmdbuf[4];

            DispMessage(title, body);

            cmdbuf[0] = IPC_MakeHeader(6, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 7: // Display error message
        {
            if (cmdbuf[0] != IPC_MakeHeader(7, 1, 4))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            const char *title = (const char *)cmdbuf[3];
            const char *body = (const char *)cmdbuf[5];

            DispErrMessage(title, body, cmdbuf[1]);

            cmdbuf[0] = IPC_MakeHeader(7, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 8: // Get PLGLDR Version
        {
            if (cmdbuf[0] != IPC_MakeHeader(8, 0, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            cmdbuf[0] = IPC_MakeHeader(8, 2, 0);
            cmdbuf[1] = 0;
            cmdbuf[2] = PLGLDR_VERSION;
            break;
        }

        case 9: // Get the arbiter (events)
        {
            if (cmdbuf[0] != IPC_MakeHeader(9, 0, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            cmdbuf[0] = IPC_MakeHeader(9, 1, 2);
            cmdbuf[1] = 0;
            cmdbuf[2] = IPC_Desc_SharedHandles(1);
            cmdbuf[3] = g_arbiter;
            break;
        }

        case 10: // Get plugin path
        {
            if (cmdbuf[0] != IPC_MakeHeader(10, 0, 2))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            char *path = (char *)cmdbuf[2];
            strncpy(path, g_pathCurrent, 255);

            cmdbuf[0] = IPC_MakeHeader(10, 1, 2);
            cmdbuf[1] = 0;
            cmdbuf[2] = IPC_Desc_Buffer(255, IPC_BUFFER_RW);
            cmdbuf[3] = (u32)path;

            break;
        }

        default: // Unknown command
        {
            error(cmdbuf, 0xD900182F);
            break;
        }
    }
}

static void     EnableNotificationLED(void)
{
    struct
    {
        u32     animation;
        u8      r[32];
        u8      g[32];
        u8      b[32];
    }       pattern;
    u32 *   cmdbuf = getThreadCommandBuffer();
    Handle  ptmsysmHandle;

    if (R_FAILED(srvGetServiceHandle(&ptmsysmHandle, "ptm:sysm")))
        return;

    pattern.animation = 0x50;
    for (u32 i = 0; i < 32; ++i)
    {
        pattern.r[i] = 0xFF;
        pattern.g[i] = 0;
        pattern.b[i] = 0xFF;
    }
    cmdbuf[0] = IPC_MakeHeader(0x801, 25, 0);
    memcpy(&cmdbuf[1], &pattern, sizeof(pattern));

    svcSendSyncRequest(ptmsysmHandle);
    svcCloseHandle(ptmsysmHandle);
}

static void     DisableNotificationLED(void)
{
    struct
    {
        u32     animation;
        u8      r[32];
        u8      g[32];
        u8      b[32];
    }       pattern = {0};
    u32 *   cmdbuf = getThreadCommandBuffer();
    Handle  ptmsysmHandle;

    if (R_FAILED(srvGetServiceHandle(&ptmsysmHandle, "ptm:sysm")))
        return;

    cmdbuf[0] = IPC_MakeHeader(0x801, 25, 0);
    memcpy(&cmdbuf[1], &pattern, sizeof(pattern));

    svcSendSyncRequest(ptmsysmHandle);
    svcCloseHandle(ptmsysmHandle);
}

static bool     ThreadPredicate(u32 *kthread)
{
    // Check if the thread is part of the plugin
    u32 *tls = (u32 *)kthread[0x26];

    return *tls == THREADVARS_MAGIC;
}

static void     __strex__(s32 *addr, s32 val)
{
    do
        __ldrex(addr);
    while (__strex(addr, val));
}

void    PLG__NotifyEvent(PLG_Event event, bool signal)
{
    if (!g_plgEventPA) return;

    __strex__(g_plgEventPA, event);
    if (signal)
        svcArbitrateAddress(g_arbiter, (u32)g_plgEventPA, ARBITRATION_SIGNAL, 1, 0);
}

void    PLG__WaitForReply(void)
{
    __strex__(g_plgReplyPA, PLG_WAIT);
    svcArbitrateAddress(g_arbiter, (u32)g_plgReplyPA, ARBITRATION_WAIT_IF_LESS_THAN_TIMEOUT, PLG_OK, 5000000000ULL);
}

static void     PluginLoader__ThreadMain(void)
{
    bool    pluginIsSwapped = false;
    Result  res = 0;
    Handle  handles[4];
    Handle  kernelEvent = 0;
    Handle  serverHandle, clientHandle, sessionHandle = 0;

    u32 *cmdbuf = getThreadCommandBuffer();
    u32 replyTarget = 0;
    u32 nbHandle;
    s32 index;

    assertSuccess(svcCreateAddressArbiter(&g_arbiter));
    assertSuccess(svcCreateEvent(&kernelEvent, RESET_ONESHOT));
    assertSuccess(svcCreatePort(&serverHandle, &clientHandle, "plg:ldr", 1));

    svcKernelSetState(0x10007, kernelEvent, 0, 0);
    do
    {
        g_error.message = NULL;
        g_error.code = 0;
        handles[0] = kernelEvent;
        handles[1] = serverHandle;
        handles[2] = sessionHandle == 0 ? g_process : sessionHandle;
        handles[3] = g_process;

        if(replyTarget == 0) // k11
            cmdbuf[0] = 0xFFFF0000;

        nbHandle = 2 + (sessionHandle != 0) + (g_process != 0);
        res = svcReplyAndReceive(&index, handles, nbHandle, replyTarget);

        if(R_FAILED(res))
        {
            if((u32)res == 0xC920181A) // session closed by remote
            {
                svcCloseHandle(sessionHandle);
                sessionHandle = 0;
                replyTarget = 0;
            }
            else
                svcBreak(USERBREAK_PANIC);
        }
        else
        {
            if (index == 0) // k11 event (swap / process exiting)
            {
                u32 event = GetConfigMemoryEvent();

                if (event == PLG_CFG_EXIT_EVENT)
                {
                    // Signal the plugin that the game is exiting
                    PLG__NotifyEvent(PLG_ABOUT_TO_EXIT, false);
                    // Wait for plugin reply
                    PLG__WaitForReply();
                }
                else if (event == PLG_CFG_SWAP_EVENT)
                {
                    EnableNotificationLED();
                    if (pluginIsSwapped)
                    {
                        // Reload data from swap file
                        MemoryBlock__IsReady();
                        MemoryBlock__FromSwapFile();
                        MemoryBlock__MountInProcess();
                        // Unlock plugin threads
                        svcControlProcess(g_process, PROCESSOP_SCHEDULE_THREADS, 0, (u32)ThreadPredicate);
                        // Resume plugin execution
                        PLG__NotifyEvent(PLG_OK, true);
                        SetConfigMemoryStatus(PLG_CFG_RUNNING);
                    }
                    else
                    {
                        // Signal plugin that it's about to be swapped
                        PLG__NotifyEvent(PLG_ABOUT_TO_SWAP, false);
                        // Wait for plugin reply
                        PLG__WaitForReply();
                        // Lock plugin threads
                        svcControlProcess(g_process, PROCESSOP_SCHEDULE_THREADS, 1, (u32)ThreadPredicate);
                        // Put data into file and release memory
                        MemoryBlock__UnmountFromProcess();
                        MemoryBlock__ToSwapFile();
                        MemoryBlock__Free();
                        SetConfigMemoryStatus(PLG_CFG_SWAPPED);
                    }
                    pluginIsSwapped = !pluginIsSwapped;
                    DisableNotificationLED();
                }
                svcSignalEvent(kernelEvent);
                replyTarget = 0;
            }
            else if(index == 1) // Server handle
            {
                Handle session;
                assertSuccess(svcAcceptSession(&session, serverHandle));

                if(sessionHandle == 0)
                    sessionHandle = session;
                else
                    svcCloseHandle(session);
                replyTarget = 0;
            }
            else if (index == 2 && handles[2] == sessionHandle) // Session handle
            {
                PluginLoader_HandleCommands();
                replyTarget = sessionHandle;
            }
            else ///< The process in which we injected the plugin is terminating
            {
                // Unmap plugin's memory before closing the process
                MemoryBlock__UnmountFromProcess();
                MemoryBlock__Free();
                svcCloseHandle(g_process);
                SetConfigMemoryStatus(PLG_CFG_NONE);
                pluginIsSwapped = false;
                g_process = replyTarget = 0;
                IR__Unpatch();
            }
        }

        // If there's'an error, display it
        if (g_error.message != NULL)
            DispErrMessage(g_title, g_error.message, g_error.code);

    } while(!terminationRequest);

    if (g_process) svcCloseHandle(g_process);
    if (g_arbiter) svcCloseHandle(g_arbiter);
    if (kernelEvent) svcCloseHandle(kernelEvent);
    svcCloseHandle(sessionHandle);
    svcCloseHandle(clientHandle);
    svcCloseHandle(serverHandle);
}
