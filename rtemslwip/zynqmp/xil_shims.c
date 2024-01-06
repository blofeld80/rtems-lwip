/*
 * Copyright (C) 2022 On-Line Applications Research Corporation (OAR)
 * Written by Kinsey Moore <kinsey.moore@oarcorp.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "xil_mmu.h"
#include <rtems/rtems/cache.h>
#include <rtems/rtems/intr.h>
#include <rtems/score/threadimpl.h>
#include <libcpu/mmu-vmsav8-64.h>
#include <stdio.h>
#include <string.h>

#define TWO_MB (2*1024*1024)
#define ONE_GB (1024*1024*1024)

/*
 * When altering memory attributes, Xilinx sets them for differing memory sizes
 * depending on what area of memory they are in. Any attribute changes below 4GB
 * apply to 2MB chunks while any changes above 4GB apply to 1GB chunks.
 */
void Xil_SetTlbAttributes( UINTPTR Addr, u64 attrib )
{
  rtems_status_code sc;
  sc = aarch64_mmu_map(
    Addr,
    Addr < 0x100000000 ? TWO_MB : ONE_GB,
    attrib
  );
  if ( sc == RTEMS_NO_MEMORY ) {
    printf("Out of memory setting MMU attributes on ptr %p: 0x%lx\n", (void*)Addr, attrib);
  } else if ( sc == RTEMS_INVALID_ADDRESS ) {
    printf("Attempted to set MMU attributes on invalid ptr %p: 0x%lx\n", (void*)Addr, attrib);
  } else if ( sc != RTEMS_SUCCESSFUL ) {
    printf("Failed setting MMU attributes on ptr %p: 0x%lx\n", (void*)Addr, attrib);
  }
}

/*
 * The Xilinx code was written such that it assumed there was no invalidate-only
 * functionality on A53 cores. This function must flush and invalidate because
 * of how they mapped things.
 */
void Xil_DCacheInvalidateRange( INTPTR adr, INTPTR len )
{
  rtems_cache_flush_multiple_data_lines( (const void *) adr, len );
}
