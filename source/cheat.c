#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cheat.h"
#include "util.h"
#include "maputils.h"
#include "atmosphere-pm.h"

Handle debughandle = 0;
Handle processHandle = 0;

Mutex actionLock;

u64 map_base = 0;

void mapGameMemory()
{
    // Broken right now :/
    if (processHandle != 0)
    {
        printf("Handle already exists unmapping not implemented yet, returning now\n");
        return;
        /*printf("Unmapping...\n");
        svcUnmapProcessMemory(MAPPED_BASE, processHandle, 0x0, MAPPED_SIZE);
        svcCloseHandle(processHandle);
        processHandle = 0;
        printf("Unmapped the previous process, mapping new one...\n");*/
    }

    u64 pids[300];
    u32 numProc;
    svcGetProcessList(&numProc, pids, 300);
    u64 pid = pids[numProc - 1];

    pmdmntAtmosphereGetProcessHandle(&processHandle, pid);


    attach();
    MemoryInfo meminfo;
    meminfo.addr = 0;
    meminfo.size = 0;

    u64 fullSize = 0;

    u64 lastaddr = 0;
    do
    {
        lastaddr = meminfo.addr;
        u32 pageinfo;
        svcQueryDebugProcessMemory(&meminfo, &pageinfo, debughandle, meminfo.addr + meminfo.size);

        if (meminfo.type != MemType_Unmapped)
        {
            //void *map_addr = virtmemReserveMap(meminfo.size);
            
            void *map_addr;
            Result rc = LocateSpaceForMapDeprecated((u64*) &map_addr, meminfo.size);
            printf("Location with the deprecated thing because I'm desperate\n");
            if(rc != 0) {
                printf("LocateSpaceForMap failed :/ %x\n", rc);
            }

            printf("Mapping %p to %p\n", (void *)meminfo.addr, (void *)map_addr);
            if(map_addr == NULL)
                return;
            

            rc = svcMapProcessMemory((void *)map_addr, processHandle, meminfo.addr, meminfo.size);
            if (rc != 0)
            {
                printf("Error during process mapping! Rc: %x\n", rc);
            }
            else
            {
                printf("Success mapping!\n");
                fullSize += meminfo.size;
            }
            printf("Type: %x Perm: %x Size: %lx      So far: %lx\n", meminfo.type, meminfo.perm, meminfo.size, fullSize);
        }
    } while (lastaddr < meminfo.addr + meminfo.size);
    detach();
}

int attach()
{
    if (debughandle != 0)
        svcCloseHandle(debughandle);
    u64 pids[300];
    u32 numProc;
    svcGetProcessList(&numProc, pids, 300);
    u64 pid = pids[numProc - 1];

    Result rc = svcDebugActiveProcess(&debughandle, pid);
    if (R_FAILED(rc))
    {
        printf("Couldn't open the process (Error: %x)\r\n"
               "Make sure that you actually started a game.\r\n",
               rc);
        return 1;
    }
    return 0;
}

void detach()
{
    if (debughandle != 0)
        svcCloseHandle(debughandle);
    debughandle = 0;
}

void poke(int valSize, u64 addr, u64 val)
{
    svcWriteDebugProcessMemory(debughandle, &val, addr, valSize);
}

u64 peek(u64 addr)
{
    u64 out;
    svcReadDebugProcessMemory(&out, debughandle, addr, sizeof(u64));
    return out;
}

MemoryInfo getRegionOfType(int index, u32 type)
{
    MemoryInfo meminfo;
    memset(&meminfo, 0, sizeof(MemoryInfo));

    int curInd = 0;

    u64 lastaddr;
    do
    {
        lastaddr = meminfo.addr;
        u32 pageinfo;
        svcQueryDebugProcessMemory(&meminfo, &pageinfo, debughandle, meminfo.addr + meminfo.size);
        if (meminfo.type == type)
        {
            if (curInd == index)
                return meminfo;
            curInd++;
        }
    } while (lastaddr < meminfo.addr + meminfo.size);
    meminfo.addr = 0;
    meminfo.size = 0;
    return meminfo;
}

int search = VAL_NONE;
#define SEARCH_ARR_SIZE 200000
u64 searchArr[SEARCH_ARR_SIZE];
int searchSize;

int searchSection(u64 valMin, u64 valMax, u32 valType, MemoryInfo meminfo, void *buffer, u64 bufSize)
{
    search = valType;
    int valSize = valSizes[valType];
    u64 off = 0;

    u64 valMinReal = 0;
    memcpy(&valMinReal, &valMin, valSize);
    u64 valMaxReal = 0;
    memcpy(&valMaxReal, &valMax, valSize);

    u64 searchVal = 0;
    while (off < meminfo.size)
    {
        if (meminfo.size - off < bufSize)
            bufSize = meminfo.size - off;
        if (R_FAILED(svcReadDebugProcessMemory(buffer, debughandle, meminfo.addr + off, bufSize)))
            return 0;

        for (u64 i = 0; i < bufSize; i += valSize)
        {
            memcpy(&searchVal, buffer + i, valSize);
            if (valMinReal <= searchVal && valMaxReal >= searchVal)
            {
                if (searchSize < SEARCH_ARR_SIZE)
                    searchArr[searchSize++] = meminfo.addr + off + i;
                else
                    return 1;
            }
        }
        off += bufSize;
    }
    return 0;
}

int startSearch(u64 valMin, u64 valMax, u32 valType, u32 memtype)
{
    search = valType;

    MemoryInfo meminfo;
    memset(&meminfo, 0, sizeof(MemoryInfo));

    searchSize = 0;

    u64 lastaddr = 0;
    void *outbuf = malloc(SEARCH_CHUNK_SIZE);

    do
    {
        lastaddr = meminfo.addr;
        u32 pageinfo;
        svcQueryDebugProcessMemory(&meminfo, &pageinfo, debughandle, meminfo.addr + meminfo.size);
        if (meminfo.type == memtype && searchSection(valMin, valMax, valType, meminfo, outbuf, SEARCH_CHUNK_SIZE))
        {
            free(outbuf);
            return 1;
        }
    } while (lastaddr < meminfo.addr + meminfo.size);

    free(outbuf);
    return 0;
}

int contSearch(u64 valMin, u64 valMax)
{
    int valSize = valSizes[search];
    int newSearchSize = 0;
    if (search == VAL_NONE)
    {
        printf("You need to start a search first!");
        return 1;
    }

    u64 newValReal = 0;

    for (int i = 0; i < searchSize; i++)
    {
        u64 newVal = peek(searchArr[i]);
        memcpy(&newValReal, &newVal, valSize);
        if (valMin <= newValReal && valMax >= newValReal)
        {
            searchArr[newSearchSize++] = searchArr[i];
        }
    }

    searchSize = newSearchSize;
    return 0;
}

u64 freezeAddrs[FREEZE_LIST_LEN];
int freezeTypes[FREEZE_LIST_LEN];
u64 freezeVals[FREEZE_LIST_LEN];
int numFreezes = 0;

void freezeList()
{
    for (int i = 0; i < numFreezes; i++)
    {
        printf("%d) %lx (%s) = %ld\r\n", i, freezeAddrs[i], valtypes[freezeTypes[i]], freezeVals[i]);
    }
}

void freezeAdd(u64 addr, int type, u64 value)
{
    if (numFreezes >= FREEZE_LIST_LEN)
    {
        printf("Can't add any more frozen values!\r\n"
               "Please remove some of the old ones!\r\n");
    }
    freezeAddrs[numFreezes] = addr;
    freezeTypes[numFreezes] = type;
    freezeVals[numFreezes] = value;
    numFreezes++;
}

void freezeDel(int index)
{
    if (numFreezes <= index)
    {
        printf("That number doesn't exit!");
    }
    numFreezes--;
    for (int i = index; i < numFreezes; i++)
    {
        freezeAddrs[i] = freezeAddrs[i + 1];
        freezeTypes[i] = freezeTypes[i + 1];
        freezeVals[i] = freezeVals[i + 1];
    }
}

void freezeLoop()
{
    while (1)
    {
        mutexLock(&actionLock);
        for (int i = 0; i < numFreezes; i++)
        {
            if (attach())
            {
                printf("The process apparently died. Cleaning the freezes up!\r\n");
                while (numFreezes > 0)
                {
                    freezeDel(0);
                }
                break;
            }

            poke(valSizes[freezeTypes[i]], freezeAddrs[i], freezeVals[i]);

            detach();
        }
        mutexUnlock(&actionLock);
        svcSleepThread(5e+8L);
    }
}