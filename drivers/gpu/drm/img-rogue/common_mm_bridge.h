/*******************************************************************************
@File
@Title          Common bridge header for mm
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Declares common defines and structures used by both the client
                and server side of the bridge for mm
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*******************************************************************************/

#ifndef COMMON_MM_BRIDGE_H
#define COMMON_MM_BRIDGE_H

#include <powervr/mem_types.h>

#include "img_defs.h"
#include "img_types.h"
#include "pvrsrv_error.h"

#include "pvrsrv_memallocflags.h"
#include "pvrsrv_memalloc_physheap.h"
#include "devicemem_typedefs.h"

#define PVRSRV_BRIDGE_MM_CMD_FIRST			0
#define PVRSRV_BRIDGE_MM_PMREXPORTPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+0
#define PVRSRV_BRIDGE_MM_PMRUNEXPORTPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+1
#define PVRSRV_BRIDGE_MM_PMRGETUID			PVRSRV_BRIDGE_MM_CMD_FIRST+2
#define PVRSRV_BRIDGE_MM_PMRMAKELOCALIMPORTHANDLE			PVRSRV_BRIDGE_MM_CMD_FIRST+3
#define PVRSRV_BRIDGE_MM_PMRUNMAKELOCALIMPORTHANDLE			PVRSRV_BRIDGE_MM_CMD_FIRST+4
#define PVRSRV_BRIDGE_MM_PMRIMPORTPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+5
#define PVRSRV_BRIDGE_MM_PMRLOCALIMPORTPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+6
#define PVRSRV_BRIDGE_MM_PMRUNREFPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+7
#define PVRSRV_BRIDGE_MM_PMRUNREFUNLOCKPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+8
#define PVRSRV_BRIDGE_MM_PHYSMEMNEWRAMBACKEDPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+9
#define PVRSRV_BRIDGE_MM_DEVMEMINTCTXCREATE			PVRSRV_BRIDGE_MM_CMD_FIRST+10
#define PVRSRV_BRIDGE_MM_DEVMEMINTCTXDESTROY			PVRSRV_BRIDGE_MM_CMD_FIRST+11
#define PVRSRV_BRIDGE_MM_DEVMEMINTHEAPCREATE			PVRSRV_BRIDGE_MM_CMD_FIRST+12
#define PVRSRV_BRIDGE_MM_DEVMEMINTHEAPDESTROY			PVRSRV_BRIDGE_MM_CMD_FIRST+13
#define PVRSRV_BRIDGE_MM_DEVMEMINTMAPPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+14
#define PVRSRV_BRIDGE_MM_DEVMEMINTUNMAPPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+15
#define PVRSRV_BRIDGE_MM_DEVMEMINTRESERVERANGE			PVRSRV_BRIDGE_MM_CMD_FIRST+16
#define PVRSRV_BRIDGE_MM_DEVMEMINTRESERVERANGEANDMAPPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+17
#define PVRSRV_BRIDGE_MM_DEVMEMINTUNRESERVERANGEANDUNMAPPMR			PVRSRV_BRIDGE_MM_CMD_FIRST+18
#define PVRSRV_BRIDGE_MM_DEVMEMINTUNRESERVERANGE			PVRSRV_BRIDGE_MM_CMD_FIRST+19
#define PVRSRV_BRIDGE_MM_CHANGESPARSEMEM			PVRSRV_BRIDGE_MM_CMD_FIRST+20
#define PVRSRV_BRIDGE_MM_DEVMEMISVDEVADDRVALID			PVRSRV_BRIDGE_MM_CMD_FIRST+21
#define PVRSRV_BRIDGE_MM_DEVMEMINVALIDATEFBSCTABLE			PVRSRV_BRIDGE_MM_CMD_FIRST+22
#define PVRSRV_BRIDGE_MM_HEAPCFGHEAPCONFIGCOUNT			PVRSRV_BRIDGE_MM_CMD_FIRST+23
#define PVRSRV_BRIDGE_MM_HEAPCFGHEAPCOUNT			PVRSRV_BRIDGE_MM_CMD_FIRST+24
#define PVRSRV_BRIDGE_MM_HEAPCFGHEAPCONFIGNAME			PVRSRV_BRIDGE_MM_CMD_FIRST+25
#define PVRSRV_BRIDGE_MM_HEAPCFGHEAPDETAILS			PVRSRV_BRIDGE_MM_CMD_FIRST+26
#define PVRSRV_BRIDGE_MM_DEVMEMINTREGISTERPFNOTIFYKM			PVRSRV_BRIDGE_MM_CMD_FIRST+27
#define PVRSRV_BRIDGE_MM_PHYSHEAPGETMEMINFO			PVRSRV_BRIDGE_MM_CMD_FIRST+28
#define PVRSRV_BRIDGE_MM_GETDEFAULTPHYSICALHEAP			PVRSRV_BRIDGE_MM_CMD_FIRST+29
#define PVRSRV_BRIDGE_MM_DEVMEMGETFAULTADDRESS			PVRSRV_BRIDGE_MM_CMD_FIRST+30
#define PVRSRV_BRIDGE_MM_PVRSRVSTATSUPDATEOOMSTAT			PVRSRV_BRIDGE_MM_CMD_FIRST+31
#define PVRSRV_BRIDGE_MM_DEVMEMXINTRESERVERANGE			PVRSRV_BRIDGE_MM_CMD_FIRST+32
#define PVRSRV_BRIDGE_MM_DEVMEMXINTUNRESERVERANGE			PVRSRV_BRIDGE_MM_CMD_FIRST+33
#define PVRSRV_BRIDGE_MM_DEVMEMXINTMAPPAGES			PVRSRV_BRIDGE_MM_CMD_FIRST+34
#define PVRSRV_BRIDGE_MM_DEVMEMXINTUNMAPPAGES			PVRSRV_BRIDGE_MM_CMD_FIRST+35
#define PVRSRV_BRIDGE_MM_DEVMEMXINTMAPVRANGETOBACKINGPAGE			PVRSRV_BRIDGE_MM_CMD_FIRST+36
#define PVRSRV_BRIDGE_MM_CMD_LAST			(PVRSRV_BRIDGE_MM_CMD_FIRST+36)

/*******************************************
            PMRExportPMR
 *******************************************/

/* Bridge in structure for PMRExportPMR */
typedef struct PVRSRV_BRIDGE_IN_PMREXPORTPMR_TAG
{
	IMG_HANDLE hPMR;
} __packed PVRSRV_BRIDGE_IN_PMREXPORTPMR;

/* Bridge out structure for PMRExportPMR */
typedef struct PVRSRV_BRIDGE_OUT_PMREXPORTPMR_TAG
{
	IMG_UINT64 ui64Password;
	IMG_UINT64 ui64Size;
	IMG_HANDLE hPMRExport;
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32Log2Contig;
} __packed PVRSRV_BRIDGE_OUT_PMREXPORTPMR;

/*******************************************
            PMRUnexportPMR
 *******************************************/

/* Bridge in structure for PMRUnexportPMR */
typedef struct PVRSRV_BRIDGE_IN_PMRUNEXPORTPMR_TAG
{
	IMG_HANDLE hPMRExport;
} __packed PVRSRV_BRIDGE_IN_PMRUNEXPORTPMR;

/* Bridge out structure for PMRUnexportPMR */
typedef struct PVRSRV_BRIDGE_OUT_PMRUNEXPORTPMR_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRUNEXPORTPMR;

/*******************************************
            PMRGetUID
 *******************************************/

/* Bridge in structure for PMRGetUID */
typedef struct PVRSRV_BRIDGE_IN_PMRGETUID_TAG
{
	IMG_HANDLE hPMR;
} __packed PVRSRV_BRIDGE_IN_PMRGETUID;

/* Bridge out structure for PMRGetUID */
typedef struct PVRSRV_BRIDGE_OUT_PMRGETUID_TAG
{
	IMG_UINT64 ui64UID;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRGETUID;

/*******************************************
            PMRMakeLocalImportHandle
 *******************************************/

/* Bridge in structure for PMRMakeLocalImportHandle */
typedef struct PVRSRV_BRIDGE_IN_PMRMAKELOCALIMPORTHANDLE_TAG
{
	IMG_HANDLE hBuffer;
} __packed PVRSRV_BRIDGE_IN_PMRMAKELOCALIMPORTHANDLE;

/* Bridge out structure for PMRMakeLocalImportHandle */
typedef struct PVRSRV_BRIDGE_OUT_PMRMAKELOCALIMPORTHANDLE_TAG
{
	IMG_HANDLE hExtMem;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRMAKELOCALIMPORTHANDLE;

/*******************************************
            PMRUnmakeLocalImportHandle
 *******************************************/

/* Bridge in structure for PMRUnmakeLocalImportHandle */
typedef struct PVRSRV_BRIDGE_IN_PMRUNMAKELOCALIMPORTHANDLE_TAG
{
	IMG_HANDLE hExtMem;
} __packed PVRSRV_BRIDGE_IN_PMRUNMAKELOCALIMPORTHANDLE;

/* Bridge out structure for PMRUnmakeLocalImportHandle */
typedef struct PVRSRV_BRIDGE_OUT_PMRUNMAKELOCALIMPORTHANDLE_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRUNMAKELOCALIMPORTHANDLE;

/*******************************************
            PMRImportPMR
 *******************************************/

/* Bridge in structure for PMRImportPMR */
typedef struct PVRSRV_BRIDGE_IN_PMRIMPORTPMR_TAG
{
	IMG_UINT64 ui64uiPassword;
	IMG_UINT64 ui64uiSize;
	IMG_HANDLE hPMRExport;
	IMG_UINT32 ui32uiLog2Contig;
} __packed PVRSRV_BRIDGE_IN_PMRIMPORTPMR;

/* Bridge out structure for PMRImportPMR */
typedef struct PVRSRV_BRIDGE_OUT_PMRIMPORTPMR_TAG
{
	IMG_HANDLE hPMR;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRIMPORTPMR;

/*******************************************
            PMRLocalImportPMR
 *******************************************/

/* Bridge in structure for PMRLocalImportPMR */
typedef struct PVRSRV_BRIDGE_IN_PMRLOCALIMPORTPMR_TAG
{
	IMG_HANDLE hExtHandle;
} __packed PVRSRV_BRIDGE_IN_PMRLOCALIMPORTPMR;

/* Bridge out structure for PMRLocalImportPMR */
typedef struct PVRSRV_BRIDGE_OUT_PMRLOCALIMPORTPMR_TAG
{
	IMG_DEVMEM_ALIGN_T uiAlign;
	IMG_DEVMEM_SIZE_T uiSize;
	IMG_HANDLE hPMR;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRLOCALIMPORTPMR;

/*******************************************
            PMRUnrefPMR
 *******************************************/

/* Bridge in structure for PMRUnrefPMR */
typedef struct PVRSRV_BRIDGE_IN_PMRUNREFPMR_TAG
{
	IMG_HANDLE hPMR;
} __packed PVRSRV_BRIDGE_IN_PMRUNREFPMR;

/* Bridge out structure for PMRUnrefPMR */
typedef struct PVRSRV_BRIDGE_OUT_PMRUNREFPMR_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRUNREFPMR;

/*******************************************
            PMRUnrefUnlockPMR
 *******************************************/

/* Bridge in structure for PMRUnrefUnlockPMR */
typedef struct PVRSRV_BRIDGE_IN_PMRUNREFUNLOCKPMR_TAG
{
	IMG_HANDLE hPMR;
} __packed PVRSRV_BRIDGE_IN_PMRUNREFUNLOCKPMR;

/* Bridge out structure for PMRUnrefUnlockPMR */
typedef struct PVRSRV_BRIDGE_OUT_PMRUNREFUNLOCKPMR_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PMRUNREFUNLOCKPMR;

/*******************************************
            PhysmemNewRamBackedPMR
 *******************************************/

/* Bridge in structure for PhysmemNewRamBackedPMR */
typedef struct PVRSRV_BRIDGE_IN_PHYSMEMNEWRAMBACKEDPMR_TAG
{
	IMG_DEVMEM_SIZE_T uiSize;
	IMG_UINT32 *pui32MappingTable;
	const IMG_CHAR *puiAnnotation;
	IMG_UINT32 ui32AnnotationLength;
	IMG_UINT32 ui32Log2PageSize;
	IMG_UINT32 ui32NumPhysChunks;
	IMG_UINT32 ui32NumVirtChunks;
	IMG_UINT32 ui32PDumpFlags;
	IMG_PID ui32PID;
	PVRSRV_MEMALLOCFLAGS_T uiFlags;
} __packed PVRSRV_BRIDGE_IN_PHYSMEMNEWRAMBACKEDPMR;

/* Bridge out structure for PhysmemNewRamBackedPMR */
typedef struct PVRSRV_BRIDGE_OUT_PHYSMEMNEWRAMBACKEDPMR_TAG
{
	IMG_HANDLE hPMRPtr;
	PVRSRV_ERROR eError;
	PVRSRV_MEMALLOCFLAGS_T uiOutFlags;
} __packed PVRSRV_BRIDGE_OUT_PHYSMEMNEWRAMBACKEDPMR;

/*******************************************
            DevmemIntCtxCreate
 *******************************************/

/* Bridge in structure for DevmemIntCtxCreate */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTCTXCREATE_TAG
{
	IMG_BOOL bbKernelMemoryCtx;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTCTXCREATE;

/* Bridge out structure for DevmemIntCtxCreate */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTCTXCREATE_TAG
{
	IMG_HANDLE hDevMemServerContext;
	IMG_HANDLE hPrivData;
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32CPUCacheLineSize;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTCTXCREATE;

/*******************************************
            DevmemIntCtxDestroy
 *******************************************/

/* Bridge in structure for DevmemIntCtxDestroy */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTCTXDESTROY_TAG
{
	IMG_HANDLE hDevmemServerContext;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTCTXDESTROY;

/* Bridge out structure for DevmemIntCtxDestroy */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTCTXDESTROY_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTCTXDESTROY;

/*******************************************
            DevmemIntHeapCreate
 *******************************************/

/* Bridge in structure for DevmemIntHeapCreate */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTHEAPCREATE_TAG
{
	IMG_DEV_VIRTADDR sHeapBaseAddr;
	IMG_HANDLE hDevmemCtx;
	IMG_UINT32 ui32HeapConfigIndex;
	IMG_UINT32 ui32HeapIndex;
	IMG_UINT32 ui32Log2DataPageSize;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTHEAPCREATE;

/* Bridge out structure for DevmemIntHeapCreate */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTHEAPCREATE_TAG
{
	IMG_HANDLE hDevmemHeapPtr;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTHEAPCREATE;

/*******************************************
            DevmemIntHeapDestroy
 *******************************************/

/* Bridge in structure for DevmemIntHeapDestroy */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTHEAPDESTROY_TAG
{
	IMG_HANDLE hDevmemHeap;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTHEAPDESTROY;

/* Bridge out structure for DevmemIntHeapDestroy */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTHEAPDESTROY_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTHEAPDESTROY;

/*******************************************
            DevmemIntMapPMR
 *******************************************/

/* Bridge in structure for DevmemIntMapPMR */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTMAPPMR_TAG
{
	IMG_HANDLE hDevmemServerHeap;
	IMG_HANDLE hPMR;
	IMG_HANDLE hReservation;
	PVRSRV_MEMALLOCFLAGS_T uiMapFlags;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTMAPPMR;

/* Bridge out structure for DevmemIntMapPMR */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTMAPPMR_TAG
{
	IMG_HANDLE hMapping;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTMAPPMR;

/*******************************************
            DevmemIntUnmapPMR
 *******************************************/

/* Bridge in structure for DevmemIntUnmapPMR */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTUNMAPPMR_TAG
{
	IMG_HANDLE hMapping;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTUNMAPPMR;

/* Bridge out structure for DevmemIntUnmapPMR */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTUNMAPPMR_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTUNMAPPMR;

/*******************************************
            DevmemIntReserveRange
 *******************************************/

/* Bridge in structure for DevmemIntReserveRange */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTRESERVERANGE_TAG
{
	IMG_DEV_VIRTADDR sAddress;
	IMG_DEVMEM_SIZE_T uiLength;
	IMG_HANDLE hDevmemServerHeap;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTRESERVERANGE;

/* Bridge out structure for DevmemIntReserveRange */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTRESERVERANGE_TAG
{
	IMG_HANDLE hReservation;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTRESERVERANGE;

/*******************************************
            DevmemIntReserveRangeAndMapPMR
 *******************************************/

/* Bridge in structure for DevmemIntReserveRangeAndMapPMR */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTRESERVERANGEANDMAPPMR_TAG
{
	IMG_DEV_VIRTADDR sAddress;
	IMG_DEVMEM_SIZE_T uiLength;
	IMG_HANDLE hDevmemServerHeap;
	IMG_HANDLE hPMR;
	PVRSRV_MEMALLOCFLAGS_T uiMapFlags;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTRESERVERANGEANDMAPPMR;

/* Bridge out structure for DevmemIntReserveRangeAndMapPMR */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTRESERVERANGEANDMAPPMR_TAG
{
	IMG_HANDLE hMapping;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTRESERVERANGEANDMAPPMR;

/*******************************************
            DevmemIntUnreserveRangeAndUnmapPMR
 *******************************************/

/* Bridge in structure for DevmemIntUnreserveRangeAndUnmapPMR */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTUNRESERVERANGEANDUNMAPPMR_TAG
{
	IMG_HANDLE hMapping;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTUNRESERVERANGEANDUNMAPPMR;

/* Bridge out structure for DevmemIntUnreserveRangeAndUnmapPMR */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTUNRESERVERANGEANDUNMAPPMR_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTUNRESERVERANGEANDUNMAPPMR;

/*******************************************
            DevmemIntUnreserveRange
 *******************************************/

/* Bridge in structure for DevmemIntUnreserveRange */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTUNRESERVERANGE_TAG
{
	IMG_HANDLE hReservation;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTUNRESERVERANGE;

/* Bridge out structure for DevmemIntUnreserveRange */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTUNRESERVERANGE_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTUNRESERVERANGE;

/*******************************************
            ChangeSparseMem
 *******************************************/

/* Bridge in structure for ChangeSparseMem */
typedef struct PVRSRV_BRIDGE_IN_CHANGESPARSEMEM_TAG
{
	IMG_DEV_VIRTADDR sDevVAddr;
	IMG_UINT64 ui64CPUVAddr;
	IMG_HANDLE hPMR;
	IMG_HANDLE hSrvDevMemHeap;
	IMG_UINT32 *pui32AllocPageIndices;
	IMG_UINT32 *pui32FreePageIndices;
	IMG_UINT32 ui32AllocPageCount;
	IMG_UINT32 ui32FreePageCount;
	IMG_UINT32 ui32SparseFlags;
	PVRSRV_MEMALLOCFLAGS_T uiFlags;
} __packed PVRSRV_BRIDGE_IN_CHANGESPARSEMEM;

/* Bridge out structure for ChangeSparseMem */
typedef struct PVRSRV_BRIDGE_OUT_CHANGESPARSEMEM_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_CHANGESPARSEMEM;

/*******************************************
            DevmemIsVDevAddrValid
 *******************************************/

/* Bridge in structure for DevmemIsVDevAddrValid */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMISVDEVADDRVALID_TAG
{
	IMG_DEV_VIRTADDR sAddress;
	IMG_HANDLE hDevmemCtx;
} __packed PVRSRV_BRIDGE_IN_DEVMEMISVDEVADDRVALID;

/* Bridge out structure for DevmemIsVDevAddrValid */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMISVDEVADDRVALID_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMISVDEVADDRVALID;

/*******************************************
            DevmemInvalidateFBSCTable
 *******************************************/

/* Bridge in structure for DevmemInvalidateFBSCTable */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINVALIDATEFBSCTABLE_TAG
{
	IMG_UINT64 ui64FBSCEntries;
	IMG_HANDLE hDevmemCtx;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINVALIDATEFBSCTABLE;

/* Bridge out structure for DevmemInvalidateFBSCTable */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINVALIDATEFBSCTABLE_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINVALIDATEFBSCTABLE;

/*******************************************
            HeapCfgHeapConfigCount
 *******************************************/

/* Bridge in structure for HeapCfgHeapConfigCount */
typedef struct PVRSRV_BRIDGE_IN_HEAPCFGHEAPCONFIGCOUNT_TAG
{
	IMG_UINT32 ui32EmptyStructPlaceholder;
} __packed PVRSRV_BRIDGE_IN_HEAPCFGHEAPCONFIGCOUNT;

/* Bridge out structure for HeapCfgHeapConfigCount */
typedef struct PVRSRV_BRIDGE_OUT_HEAPCFGHEAPCONFIGCOUNT_TAG
{
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32NumHeapConfigs;
} __packed PVRSRV_BRIDGE_OUT_HEAPCFGHEAPCONFIGCOUNT;

/*******************************************
            HeapCfgHeapCount
 *******************************************/

/* Bridge in structure for HeapCfgHeapCount */
typedef struct PVRSRV_BRIDGE_IN_HEAPCFGHEAPCOUNT_TAG
{
	IMG_UINT32 ui32HeapConfigIndex;
} __packed PVRSRV_BRIDGE_IN_HEAPCFGHEAPCOUNT;

/* Bridge out structure for HeapCfgHeapCount */
typedef struct PVRSRV_BRIDGE_OUT_HEAPCFGHEAPCOUNT_TAG
{
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32NumHeaps;
} __packed PVRSRV_BRIDGE_OUT_HEAPCFGHEAPCOUNT;

/*******************************************
            HeapCfgHeapConfigName
 *******************************************/

/* Bridge in structure for HeapCfgHeapConfigName */
typedef struct PVRSRV_BRIDGE_IN_HEAPCFGHEAPCONFIGNAME_TAG
{
	IMG_CHAR *puiHeapConfigName;
	IMG_UINT32 ui32HeapConfigIndex;
	IMG_UINT32 ui32HeapConfigNameBufSz;
} __packed PVRSRV_BRIDGE_IN_HEAPCFGHEAPCONFIGNAME;

/* Bridge out structure for HeapCfgHeapConfigName */
typedef struct PVRSRV_BRIDGE_OUT_HEAPCFGHEAPCONFIGNAME_TAG
{
	IMG_CHAR *puiHeapConfigName;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_HEAPCFGHEAPCONFIGNAME;

/*******************************************
            HeapCfgHeapDetails
 *******************************************/

/* Bridge in structure for HeapCfgHeapDetails */
typedef struct PVRSRV_BRIDGE_IN_HEAPCFGHEAPDETAILS_TAG
{
	IMG_CHAR *puiHeapNameOut;
	IMG_UINT32 ui32HeapConfigIndex;
	IMG_UINT32 ui32HeapIndex;
	IMG_UINT32 ui32HeapNameBufSz;
} __packed PVRSRV_BRIDGE_IN_HEAPCFGHEAPDETAILS;

/* Bridge out structure for HeapCfgHeapDetails */
typedef struct PVRSRV_BRIDGE_OUT_HEAPCFGHEAPDETAILS_TAG
{
	IMG_DEV_VIRTADDR sDevVAddrBase;
	IMG_DEVMEM_SIZE_T uiHeapLength;
	IMG_DEVMEM_SIZE_T uiReservedRegionLength;
	IMG_CHAR *puiHeapNameOut;
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32Log2DataPageSizeOut;
	IMG_UINT32 ui32Log2ImportAlignmentOut;
} __packed PVRSRV_BRIDGE_OUT_HEAPCFGHEAPDETAILS;

/*******************************************
            DevmemIntRegisterPFNotifyKM
 *******************************************/

/* Bridge in structure for DevmemIntRegisterPFNotifyKM */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMINTREGISTERPFNOTIFYKM_TAG
{
	IMG_HANDLE hDevmemCtx;
	IMG_BOOL bRegister;
} __packed PVRSRV_BRIDGE_IN_DEVMEMINTREGISTERPFNOTIFYKM;

/* Bridge out structure for DevmemIntRegisterPFNotifyKM */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMINTREGISTERPFNOTIFYKM_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMINTREGISTERPFNOTIFYKM;

/*******************************************
            PhysHeapGetMemInfo
 *******************************************/

/* Bridge in structure for PhysHeapGetMemInfo */
typedef struct PVRSRV_BRIDGE_IN_PHYSHEAPGETMEMINFO_TAG
{
	PHYS_HEAP_MEM_STATS *pasapPhysHeapMemStats;
	PVRSRV_PHYS_HEAP *peaPhysHeapID;
	IMG_UINT32 ui32PhysHeapCount;
} __packed PVRSRV_BRIDGE_IN_PHYSHEAPGETMEMINFO;

/* Bridge out structure for PhysHeapGetMemInfo */
typedef struct PVRSRV_BRIDGE_OUT_PHYSHEAPGETMEMINFO_TAG
{
	PHYS_HEAP_MEM_STATS *pasapPhysHeapMemStats;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PHYSHEAPGETMEMINFO;

/*******************************************
            GetDefaultPhysicalHeap
 *******************************************/

/* Bridge in structure for GetDefaultPhysicalHeap */
typedef struct PVRSRV_BRIDGE_IN_GETDEFAULTPHYSICALHEAP_TAG
{
	IMG_UINT32 ui32EmptyStructPlaceholder;
} __packed PVRSRV_BRIDGE_IN_GETDEFAULTPHYSICALHEAP;

/* Bridge out structure for GetDefaultPhysicalHeap */
typedef struct PVRSRV_BRIDGE_OUT_GETDEFAULTPHYSICALHEAP_TAG
{
	PVRSRV_ERROR eError;
	PVRSRV_PHYS_HEAP eHeap;
} __packed PVRSRV_BRIDGE_OUT_GETDEFAULTPHYSICALHEAP;

/*******************************************
            DevmemGetFaultAddress
 *******************************************/

/* Bridge in structure for DevmemGetFaultAddress */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMGETFAULTADDRESS_TAG
{
	IMG_HANDLE hDevmemCtx;
} __packed PVRSRV_BRIDGE_IN_DEVMEMGETFAULTADDRESS;

/* Bridge out structure for DevmemGetFaultAddress */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMGETFAULTADDRESS_TAG
{
	IMG_DEV_VIRTADDR sFaultAddress;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMGETFAULTADDRESS;

/*******************************************
            PVRSRVStatsUpdateOOMStat
 *******************************************/

/* Bridge in structure for PVRSRVStatsUpdateOOMStat */
typedef struct PVRSRV_BRIDGE_IN_PVRSRVSTATSUPDATEOOMSTAT_TAG
{
	IMG_PID ui32pid;
	IMG_UINT32 ui32ui32StatType;
} __packed PVRSRV_BRIDGE_IN_PVRSRVSTATSUPDATEOOMSTAT;

/* Bridge out structure for PVRSRVStatsUpdateOOMStat */
typedef struct PVRSRV_BRIDGE_OUT_PVRSRVSTATSUPDATEOOMSTAT_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_PVRSRVSTATSUPDATEOOMSTAT;

/*******************************************
            DevmemXIntReserveRange
 *******************************************/

/* Bridge in structure for DevmemXIntReserveRange */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMXINTRESERVERANGE_TAG
{
	IMG_DEV_VIRTADDR sAddress;
	IMG_DEVMEM_SIZE_T uiLength;
	IMG_HANDLE hDevmemServerHeap;
} __packed PVRSRV_BRIDGE_IN_DEVMEMXINTRESERVERANGE;

/* Bridge out structure for DevmemXIntReserveRange */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMXINTRESERVERANGE_TAG
{
	IMG_HANDLE hReservation;
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMXINTRESERVERANGE;

/*******************************************
            DevmemXIntUnreserveRange
 *******************************************/

/* Bridge in structure for DevmemXIntUnreserveRange */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMXINTUNRESERVERANGE_TAG
{
	IMG_HANDLE hReservation;
} __packed PVRSRV_BRIDGE_IN_DEVMEMXINTUNRESERVERANGE;

/* Bridge out structure for DevmemXIntUnreserveRange */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMXINTUNRESERVERANGE_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMXINTUNRESERVERANGE;

/*******************************************
            DevmemXIntMapPages
 *******************************************/

/* Bridge in structure for DevmemXIntMapPages */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMXINTMAPPAGES_TAG
{
	IMG_HANDLE hPMR;
	IMG_HANDLE hReservation;
	IMG_UINT32 ui32PageCount;
	IMG_UINT32 ui32PhysPageOffset;
	IMG_UINT32 ui32VirtPageOffset;
	PVRSRV_MEMALLOCFLAGS_T uiFlags;
} __packed PVRSRV_BRIDGE_IN_DEVMEMXINTMAPPAGES;

/* Bridge out structure for DevmemXIntMapPages */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMXINTMAPPAGES_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMXINTMAPPAGES;

/*******************************************
            DevmemXIntUnmapPages
 *******************************************/

/* Bridge in structure for DevmemXIntUnmapPages */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMXINTUNMAPPAGES_TAG
{
	IMG_HANDLE hReservation;
	IMG_UINT32 ui32PageCount;
	IMG_UINT32 ui32VirtPageOffset;
} __packed PVRSRV_BRIDGE_IN_DEVMEMXINTUNMAPPAGES;

/* Bridge out structure for DevmemXIntUnmapPages */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMXINTUNMAPPAGES_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMXINTUNMAPPAGES;

/*******************************************
            DevmemXIntMapVRangeToBackingPage
 *******************************************/

/* Bridge in structure for DevmemXIntMapVRangeToBackingPage */
typedef struct PVRSRV_BRIDGE_IN_DEVMEMXINTMAPVRANGETOBACKINGPAGE_TAG
{
	IMG_HANDLE hReservation;
	IMG_UINT32 ui32PageCount;
	IMG_UINT32 ui32VirtPageOffset;
	PVRSRV_MEMALLOCFLAGS_T uiFlags;
} __packed PVRSRV_BRIDGE_IN_DEVMEMXINTMAPVRANGETOBACKINGPAGE;

/* Bridge out structure for DevmemXIntMapVRangeToBackingPage */
typedef struct PVRSRV_BRIDGE_OUT_DEVMEMXINTMAPVRANGETOBACKINGPAGE_TAG
{
	PVRSRV_ERROR eError;
} __packed PVRSRV_BRIDGE_OUT_DEVMEMXINTMAPVRANGETOBACKINGPAGE;

#endif /* COMMON_MM_BRIDGE_H */
