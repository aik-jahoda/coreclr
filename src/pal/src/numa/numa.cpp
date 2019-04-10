// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

/*++



Module Name:

    numa.cpp

Abstract:

    Implementation of NUMA related APIs

--*/

#include "pal/dbgmsg.h"
SET_DEFAULT_DEBUG_CHANNEL(NUMA);

#include "pal/palinternal.h"
#include "pal/dbgmsg.h"
#include "pal/numa.h"
#include "pal/corunix.hpp"
#include "pal/thread.hpp"

#include <dlfcn.h>
#ifdef __FreeBSD__
#include <stdlib.h>
#else
#include <alloca.h>
#endif

#include <algorithm>

#include "numashim.h"

using namespace CorUnix;

// The highest NUMA node available
int g_highestNumaNode = 0;
// Is numa available
bool g_numaAvailable = false;

void* numaHandle = nullptr;

#if HAVE_NUMA_H
#define PER_FUNCTION_BLOCK(fn) decltype(fn)* fn##_ptr;
FOR_ALL_NUMA_FUNCTIONS
#undef PER_FUNCTION_BLOCK
#endif // HAVE_NUMA_H


/*++
Function:
  NUMASupportInitialize

Initialize data structures for getting and setting thread affinities to processors and
querying NUMA related processor information.
On systems with no NUMA support, it behaves as if there was a single NUMA node with
a single group of processors.
--*/
BOOL
NUMASupportInitialize()
{
#if HAVE_NUMA_H
    numaHandle = dlopen("libnuma.so", RTLD_LAZY);
    if (numaHandle == 0)
    {
        numaHandle = dlopen("libnuma.so.1", RTLD_LAZY);
    }
    if (numaHandle != 0)
    {
        dlsym(numaHandle, "numa_allocate_cpumask");
#define PER_FUNCTION_BLOCK(fn) \
    fn##_ptr = (decltype(fn)*)dlsym(numaHandle, #fn); \
    if (fn##_ptr == NULL) { fprintf(stderr, "Cannot get symbol " #fn " from libnuma\n"); abort(); }
FOR_ALL_NUMA_FUNCTIONS
#undef PER_FUNCTION_BLOCK

        if (numa_available() == -1)
        {
            dlclose(numaHandle);
        }
        else
        {
            g_numaAvailable = true;
            g_highestNumaNode = numa_max_node();
        }
    }
#endif // HAVE_NUMA_H
    if (!g_numaAvailable)
    {
        // No NUMA
        g_highestNumaNode = 0;
    }

    return TRUE;
}

/*++
Function:
  NUMASupportCleanup

Cleanup of the NUMA support data structures
--*/
VOID
NUMASupportCleanup()
{
#if HAVE_NUMA_H
    if (g_numaAvailable)
    {
        dlclose(numaHandle);
    }
#endif // HAVE_NUMA_H
}

/*++
Function:
  GetNumaHighestNodeNumber

See MSDN doc.
--*/
BOOL
PALAPI
GetNumaHighestNodeNumber(
  OUT PULONG HighestNodeNumber
)
{
    PERF_ENTRY(GetNumaHighestNodeNumber);
    ENTRY("GetNumaHighestNodeNumber(HighestNodeNumber=%p)\n", HighestNodeNumber);
    *HighestNodeNumber = (ULONG)g_highestNumaNode;

    BOOL success = TRUE;

    LOGEXIT("GetNumaHighestNodeNumber returns BOOL %d\n", success);
    PERF_EXIT(GetNumaHighestNodeNumber);

    return success;
}

/*++
Function:
  PAL_GetNumaProcessorNode

Abstract
  Get NUMA node of a processor

Parameters:
  procNo - number of the processor to get the NUMA node for
  node   - the resulting NUMA node

Return value:
  TRUE if the function was able to get the NUMA node, FALSE if it has failed.
--*/
BOOL
PALAPI
PAL_GetNumaProcessorNode(WORD procNo, WORD* node)
{
#if HAVE_NUMA_H
    if (g_numaAvailable)
    {
        int result = numa_node_of_cpu(procNo);
        if (result >= 0)
        {
            *node = (WORD)result;
            return TRUE;
        }
    }
#endif // HAVE_NUMA_H

    return FALSE;
}

/*++
Function:
  VirtualAllocExNuma

See MSDN doc.
--*/
LPVOID
PALAPI
VirtualAllocExNuma(
  IN HANDLE hProcess,
  IN OPTIONAL LPVOID lpAddress,
  IN SIZE_T dwSize,
  IN DWORD flAllocationType,
  IN DWORD flProtect,
  IN DWORD nndPreferred
)
{
    PERF_ENTRY(VirtualAllocExNuma);
    ENTRY("VirtualAllocExNuma(hProcess=%p, lpAddress=%p, dwSize=%u, flAllocationType=%#x, flProtect=%#x, nndPreferred=%d\n",
        hProcess, lpAddress, dwSize, flAllocationType, flProtect, nndPreferred);

    LPVOID result = NULL;

    if (hProcess == GetCurrentProcess())
    {
        if ((int)nndPreferred <= g_highestNumaNode)
        {
            result = VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
#if HAVE_NUMA_H
            if (result != NULL && g_numaAvailable)
            {
                int usedNodeMaskBits = g_highestNumaNode + 1;
                int nodeMaskLength = (usedNodeMaskBits + sizeof(unsigned long) - 1) / sizeof(unsigned long);
                unsigned long *nodeMask = (unsigned long*)alloca(nodeMaskLength * sizeof(unsigned long));
                memset(nodeMask, 0, nodeMaskLength);

                int index = nndPreferred / sizeof(unsigned long);
                int mask = ((unsigned long)1) << (nndPreferred & (sizeof(unsigned long) - 1));
                nodeMask[index] = mask;

                int st = mbind(result, dwSize, MPOL_PREFERRED, nodeMask, usedNodeMaskBits, 0);

                _ASSERTE(st == 0);
                // If the mbind fails, we still return the allocated memory since the nndPreferred is just a hint
            }
#endif // HAVE_NUMA_H
        }
        else
        {
            // The specified node number is larger than the maximum available one
            SetLastError(ERROR_INVALID_PARAMETER);
        }
    }
    else
    {
        // PAL supports allocating from the current process virtual space only
        SetLastError(ERROR_INVALID_PARAMETER);
    }

    LOGEXIT("VirtualAllocExNuma returns %p\n", result);
    PERF_EXIT(VirtualAllocExNuma);

    return result;
}
