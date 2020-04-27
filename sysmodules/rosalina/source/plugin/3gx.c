#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include "plugin.h"
#include "ifile.h"
#include "utils.h"

u32         g_decExeArgs[0x10];
void        decExeFunc(void* startAddr, void* endAddr, void* args);

static inline u32 invertEndianness(u32 val)
{
    return ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) | ((val & 0xFF0000) >> 8) | ((val & 0xFF000000) >> 24);
}

Result  Check_3gx_Magic(IFile *file)
{
    u64     magic;
    u64     total;
    Result  res;
    int     verDif;

    file->pos = 0;
    if (R_FAILED((res = IFile_Read(file, &total, &magic, sizeof(u64)))))
        return res;

    if ((u32)magic != (u32)_3GX_MAGIC) //Invalid file type
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, 1);

    else if ((verDif = invertEndianness((u32)(magic >> 32)) - invertEndianness((u32)(_3GX_MAGIC >> 32))) != 0) //Invalid plugin version (2 -> outdated plugin; 3 -> outdated loader)
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, (verDif < 0) ? 2 : 3);

    else return 0;
}

Result  Read_3gx_Header(IFile *file, _3gx_Header *header)
{
    u64     total;
    char *  dst;
    Result  res = 0;

    file->pos = 0;
    res = IFile_Read(file, &total, header, sizeof(_3gx_Header));
    if (R_FAILED(res))
        return res;

    // Read author
    file->pos = (u32)header->infos.authorMsg;
    dst = (char *)header + sizeof(_3gx_Header);
    res = IFile_Read(file, &total, dst, header->infos.authorLen);
    if (R_FAILED(res))
        return res;

    // Relocate ptr
    header->infos.authorMsg = dst;

    // Read title
    file->pos = (u32)header->infos.titleMsg;
    dst += header->infos.authorLen;
    res = IFile_Read(file, &total, dst, header->infos.titleLen);
    if (R_FAILED(res))
        return res;

    // Relocate ptr
    header->infos.titleMsg = dst;

    // Declare other members as null (unused in our case)
    header->infos.summaryLen = 0;
    header->infos.summaryMsg = NULL;
    header->infos.descriptionLen = 0;
    header->infos.descriptionMsg = NULL;

    // Read targets compatibility
    file->pos = (u32)header->targets.titles;
    dst += header->infos.titleLen;
    dst += 4 - ((u32)dst & 3); // 4 bytes aligned
    res = IFile_Read(file, &total, dst, header->targets.count * sizeof(u32));
    if (R_FAILED(res))
        return res;

    // Relocate ptr
    header->targets.titles = (u32 *)dst;
    return res;
}

Result  Read_3gx_LoadSegments(IFile *file, _3gx_Header *header, void *dst)
{
    u32                 size;
    u64                 total;
    Result              res = 0;
    _3gx_Executable     *exeHdr = &header->executable;

    file->pos = exeHdr->codeOffset;
    size = exeHdr->codeSize + exeHdr->rodataSize + exeHdr->dataSize;
    res = IFile_Read(file, &total, dst, size);

    decExeFunc(dst, dst + size, g_decExeArgs);
    Reset_3gx_DecParams();

    return res;
}

void       Reset_3gx_DecParams(void)
{
	u32* decExeFuncAddr = PA_FROM_VA_PTR((u32)decExeFunc); //Bypass mem permissions

	memset(g_decExeArgs, 0, sizeof(g_decExeArgs));

	decExeFuncAddr[0] = 0xE12FFF1E; // BX LR

	for (int i = 1; i < 32; i++) {
		decExeFuncAddr[i] = 0xE320F000; // NOP
	}

	svcInvalidateEntireInstructionCache();
}
