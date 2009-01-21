/*
PV Net Driver for Windows Xen HVM Domains
Copyright (C) 2007 James Harper
Copyright (C) 2007 Andrew Grover <andy.grover@oracle.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "xennet.h"

/*
Increase the header to a certain size
*/

BOOLEAN
XenNet_BuildHeader(packet_info_t *pi, ULONG new_header_size)
{
  ULONG bytes_remaining;
  PMDL current_mdl;

  //FUNCTION_ENTER();

  if (new_header_size <= pi->header_length)
  {
    //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ " new_header_size < pi->header_length\n"));
    return TRUE;
  }

  if (new_header_size > ARRAY_SIZE(pi->header_data))
  {
    //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ " new_header_size > ARRAY_SIZE(pi->header_data)\n"));
    return FALSE;
  }
  
  if (new_header_size <= pi->first_buffer_length)
  {
    //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ " new_header_size <= pi->first_buffer_length\n"));
    pi->header_length = new_header_size;
    return TRUE;
  }
  else if (pi->header != pi->header_data)
  {
    //KdPrint((__DRIVER_NAME "     Using header_data\n"));
    memcpy(pi->header_data, pi->header, pi->header_length);
    pi->header = pi->header_data;
  }
  
  bytes_remaining = new_header_size - pi->header_length;

  //KdPrint((__DRIVER_NAME "     A bytes_remaining = %d, pi->curr_mdl_index = %d, pi->mdl_count = %d\n",
  //  bytes_remaining, pi->curr_mdl_index, pi->mdl_count));
  while (bytes_remaining && pi->curr_mdl_index < pi->mdl_count)
  {
    ULONG copy_size;
    
    //KdPrint((__DRIVER_NAME "     B bytes_remaining = %d, pi->curr_mdl_index = %d, pi->mdl_count = %d\n",
    //  bytes_remaining, pi->curr_mdl_index, pi->mdl_count));
    current_mdl = pi->mdls[pi->curr_mdl_index];
    if (MmGetMdlByteCount(current_mdl))
    {
      copy_size = min(bytes_remaining, MmGetMdlByteCount(current_mdl) - pi->curr_mdl_offset);
      //KdPrint((__DRIVER_NAME "     B copy_size = %d\n", copy_size));
      memcpy(pi->header + pi->header_length,
        (PUCHAR)MmGetMdlVirtualAddress(current_mdl) + pi->curr_mdl_offset, copy_size);
      pi->curr_mdl_offset += copy_size;
      pi->header_length += copy_size;
      bytes_remaining -= copy_size;
    }
    if (pi->curr_mdl_offset == MmGetMdlByteCount(current_mdl))
    {
      pi->curr_mdl_index++;
      pi->curr_mdl_offset = 0;
    }
  }
  //KdPrint((__DRIVER_NAME "     C bytes_remaining = %d, pi->curr_mdl_index = %d, pi->mdl_count = %d\n",
  //  bytes_remaining, pi->curr_mdl_index, pi->mdl_count));
  if (bytes_remaining)
  {
    //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ " bytes_remaining\n"));
    return FALSE;
  }
  //FUNCTION_EXIT();
  return TRUE;
}

ULONG
XenNet_ParsePacketHeader(packet_info_t *pi)
{
  //FUNCTION_ENTER();

  ASSERT(pi->mdls[0]);
  
  NdisQueryBufferSafe(pi->mdls[0], (PVOID) &pi->header, &pi->first_buffer_length, NormalPagePriority);

  pi->header_length = 0;
  pi->curr_mdl_index = 0;
  pi->curr_mdl_offset = 0;
    
  if (!XenNet_BuildHeader(pi, (ULONG)XN_HDR_SIZE))
  {
    KdPrint((__DRIVER_NAME "     packet too small (Ethernet Header)\n"));
    return PARSE_TOO_SMALL;
  }

  switch (GET_NET_PUSHORT(&pi->header[12])) // L2 protocol field
  {
  case 0x0800:
    //KdPrint((__DRIVER_NAME "     IP\n"));
    if (pi->header_length < (ULONG)(XN_HDR_SIZE + 20))
    {
      if (!XenNet_BuildHeader(pi, (ULONG)(XN_HDR_SIZE + 20)))
      {
        KdPrint((__DRIVER_NAME "     packet too small (IP Header)\n"));
        return PARSE_TOO_SMALL;
      }
    }
    pi->ip_version = (pi->header[XN_HDR_SIZE + 0] & 0xF0) >> 4;
    if (pi->ip_version != 4)
    {
      KdPrint((__DRIVER_NAME "     ip_version = %d\n", pi->ip_version));
      return PARSE_UNKNOWN_TYPE;
    }
    pi->ip4_header_length = (pi->header[XN_HDR_SIZE + 0] & 0x0F) << 2;
    if (pi->header_length < (ULONG)(XN_HDR_SIZE + 20 + pi->ip4_header_length))
    {
      if (!XenNet_BuildHeader(pi, (ULONG)(XN_HDR_SIZE + pi->ip4_header_length + 20)))
      {
        KdPrint((__DRIVER_NAME "     packet too small (IP Header + IP Options + TCP Header)\n"));
        return PARSE_TOO_SMALL;
      }
#if 0      
      KdPrint((__DRIVER_NAME "     first buffer is only %d bytes long, must be >= %d (1)\n", pi->header_length, (ULONG)(XN_HDR_SIZE + pi->ip4_header_length + 20)));
      KdPrint((__DRIVER_NAME "     total_length = %d\n", pi->total_length));
      for (i = 0; i < pi->mdl_count; i++)
      {
        KdPrint((__DRIVER_NAME "     mdl %d length = %d\n", i, MmGetMdlByteCount(pi->mdls[i])));
      }
#endif
    }
    break;
  default:
    KdPrint((__DRIVER_NAME "     Not IP (%d)\n", GET_NET_PUSHORT(&pi->header[12])));
    return PARSE_UNKNOWN_TYPE;
  }
  pi->ip_proto = pi->header[XN_HDR_SIZE + 9];
  switch (pi->ip_proto)
  {
  case 6:  // TCP
  case 17: // UDP
    break;
  default:
    KdPrint((__DRIVER_NAME "     Not TCP/UDP (%d)\n", pi->ip_proto));
    return PARSE_UNKNOWN_TYPE;
  }
  pi->ip4_length = GET_NET_PUSHORT(&pi->header[XN_HDR_SIZE + 2]);
  pi->tcp_header_length = (pi->header[XN_HDR_SIZE + pi->ip4_header_length + 12] & 0xf0) >> 2;

  if (pi->header_length < (ULONG)(XN_HDR_SIZE + pi->ip4_header_length + pi->tcp_header_length))
  {
    if (!XenNet_BuildHeader(pi, (ULONG)(XN_HDR_SIZE + pi->ip4_header_length + 20)))
    {
      KdPrint((__DRIVER_NAME "     packet too small (IP Header + IP Options + TCP Header + TCP Options)\n"));
      return PARSE_TOO_SMALL;
    }
#if 0    
    KdPrint((__DRIVER_NAME "     first buffer is only %d bytes long, must be >= %d (2)\n", pi->header_length, (ULONG)(XN_HDR_SIZE + pi->ip4_header_length + pi->tcp_header_length)));
    return PARSE_TOO_SMALL;
#endif
  }

  pi->tcp_length = pi->ip4_length - pi->ip4_header_length - pi->tcp_header_length;
  pi->tcp_remaining = pi->tcp_length;
  pi->tcp_seq = GET_NET_PULONG(&pi->header[XN_HDR_SIZE + pi->ip4_header_length + 4]);
  pi->tcp_has_options = (BOOLEAN)(pi->tcp_header_length > 20);
  if (pi->mss > 0 && pi->tcp_length > pi->mss)
    pi->split_required = TRUE;

  //KdPrint((__DRIVER_NAME "     ip4_length = %d\n", pi->ip4_length));
  //KdPrint((__DRIVER_NAME "     tcp_length = %d\n", pi->tcp_length));
  //FUNCTION_EXIT();
  
  return PARSE_OK;
}

VOID
XenNet_SumIpHeader(
  PUCHAR header,
  USHORT ip4_header_length
)
{
  ULONG csum = 0;
  USHORT i;

  ASSERT(ip4_header_length > 12);
  ASSERT(!(ip4_header_length & 1));

  header[XN_HDR_SIZE + 10] = 0;
  header[XN_HDR_SIZE + 11] = 0;
  for (i = 0; i < ip4_header_length; i += 2)
  {
    csum += GET_NET_PUSHORT(&header[XN_HDR_SIZE + i]);
  }
  while (csum & 0xFFFF0000)
    csum = (csum & 0xFFFF) + (csum >> 16);
  csum = ~csum;
  SET_NET_USHORT(&header[XN_HDR_SIZE + 10], (USHORT)csum);
}

/* Called at DISPATCH LEVEL */
static VOID DDKAPI
XenFreelist_Timer(
  PVOID SystemSpecific1,
  PVOID FunctionContext,
  PVOID SystemSpecific2,
  PVOID SystemSpecific3
)
{
  freelist_t *fl = (freelist_t *)FunctionContext;
  PMDL mdl;
  int i;

  UNREFERENCED_PARAMETER(SystemSpecific1);
  UNREFERENCED_PARAMETER(SystemSpecific2);
  UNREFERENCED_PARAMETER(SystemSpecific3);

  if (fl->xi->device_state->resume_state != RESUME_STATE_RUNNING && !fl->grants_resumed)
    return;

  KeAcquireSpinLockAtDpcLevel(fl->lock);

  //FUNCTION_MSG((" --- timer - page_free_lowest = %d\n", fl->page_free_lowest));

  if (fl->page_free_lowest > fl->page_free_target) // lots of potential for tuning here
  {
    for (i = 0; i < (int)min(16, fl->page_free_lowest - fl->page_free_target); i++)
    {
      mdl = XenFreelist_GetPage(fl);
      if (!mdl)
        break;
      fl->xi->vectors.GntTbl_EndAccess(fl->xi->vectors.context,
        *(grant_ref_t *)(((UCHAR *)mdl) + MmSizeOfMdl(0, PAGE_SIZE)), 0);
      FreePages(mdl);
      fl->page_outstanding--;
    }
    //FUNCTION_MSG((__DRIVER_NAME " --- timer - freed %d pages\n", i));
  }

  fl->page_free_lowest = fl->page_free;

  KeReleaseSpinLockFromDpcLevel(fl->lock);
}

VOID
XenFreelist_Init(struct xennet_info *xi, freelist_t *fl, PKSPIN_LOCK lock)
{
  fl->xi = xi;
  fl->lock = lock;
  fl->page_free = 0;
  fl->page_free_lowest = 0;
  fl->page_free_target = 16; /* tune this */
  fl->page_limit = 512; /* 2MB */ /* tune this */
  fl->page_outstanding = 0;
  fl->grants_resumed = FALSE;
  NdisMInitializeTimer(&fl->timer, fl->xi->adapter_handle, XenFreelist_Timer, fl);
  NdisMSetPeriodicTimer(&fl->timer, 1000);
}

PMDL
XenFreelist_GetPage(freelist_t *fl)
{
  PMDL mdl;
  PFN_NUMBER pfn;
  grant_ref_t gref;

  //ASSERT(!KeTestSpinLock(fl->lock));

  if (fl->page_free == 0)
  {
    if (fl->page_outstanding >= fl->page_limit)
      return NULL;
    mdl = AllocatePagesExtra(1, sizeof(grant_ref_t));
    if (!mdl)
      return NULL;
    pfn = *MmGetMdlPfnArray(mdl);
    gref = fl->xi->vectors.GntTbl_GrantAccess(
      fl->xi->vectors.context, 0,
      (uint32_t)pfn, FALSE, INVALID_GRANT_REF);
    if (gref == INVALID_GRANT_REF)
      KdPrint((__DRIVER_NAME "     No more grefs\n"));
    *(grant_ref_t *)(((UCHAR *)mdl) + MmSizeOfMdl(0, PAGE_SIZE)) = gref;
    /* we really should check if our grant was successful... */
  }
  else
  {
    fl->page_free--;
    if (fl->page_free < fl->page_free_lowest)
      fl->page_free_lowest = fl->page_free;
    mdl = fl->page_list[fl->page_free];
  }
  fl->page_outstanding++;
  return mdl;
}

VOID
XenFreelist_PutPage(freelist_t *fl, PMDL mdl)
{
  //ASSERT(!KeTestSpinLock(fl->lock));

  ASSERT(NdisBufferLength(mdl) == PAGE_SIZE);

  if (fl->page_free == PAGE_LIST_SIZE)
  {
    KdPrint((__DRIVER_NAME "     page free list full - releasing page\n"));
    /* our page list is full. free the buffer instead. This will be a bit sucky performancewise... */
    fl->xi->vectors.GntTbl_EndAccess(fl->xi->vectors.context,
      *(grant_ref_t *)(((UCHAR *)mdl) + MmSizeOfMdl(0, PAGE_SIZE)), FALSE);
    FreePages(mdl);
  }
  else
  {
    fl->page_list[fl->page_free] = mdl;
    fl->page_free++;
  }
  fl->page_outstanding--;
}

VOID
XenFreelist_Dispose(freelist_t *fl)
{
  PMDL mdl;
  BOOLEAN TimerCancelled;

  NdisMCancelTimer(&fl->timer, &TimerCancelled);

  while(fl->page_free != 0)
  {
    fl->page_free--;
    mdl = fl->page_list[fl->page_free];
    fl->xi->vectors.GntTbl_EndAccess(fl->xi->vectors.context,
      *(grant_ref_t *)(((UCHAR *)mdl) + MmSizeOfMdl(0, PAGE_SIZE)), 0);
    FreePages(mdl);
  }
}

static VOID
XenFreelist_ReGrantMdl(freelist_t *fl, PMDL mdl)
{
  PFN_NUMBER pfn;
  pfn = *MmGetMdlPfnArray(mdl);
  *(grant_ref_t *)(((UCHAR *)mdl) + MmSizeOfMdl(0, PAGE_SIZE)) = fl->xi->vectors.GntTbl_GrantAccess(
    fl->xi->vectors.context, 0,
    (uint32_t)pfn, FALSE, INVALID_GRANT_REF);
}

/* re-grant all the pages, as the grant table was wiped on resume */
VOID
XenFreelist_ResumeStart(freelist_t *fl)
{
  ULONG i;
  
  for (i = 0; i < fl->page_free; i++)
  {
    XenFreelist_ReGrantMdl(fl, fl->page_list[i]);
  }
  fl->grants_resumed = TRUE;
}

VOID
XenFreelist_ResumeEnd(freelist_t *fl)
{
  fl->grants_resumed = FALSE;
}
