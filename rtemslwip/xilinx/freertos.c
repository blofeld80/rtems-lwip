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

#include <rtems/rtems/intr.h>
#include <rtems/score/threadimpl.h>
#include <stdio.h>
#include <string.h>
#include "xil_types.h"
#include "FreeRTOS.h"


/*
 * XInterruptHandler function pointer signature just happens to exactly match
 * rtems_interrupt_handler
 */
BaseType_t xPortInstallInterruptHandler(
  uint8_t           ucInterruptID,
  XInterruptHandler pxHandler,
  void             *pvCallBackRef
)
{
  char name[10];

  /* Is this running in the context of any interrupt server tasks? */
  _Thread_Get_name( _Thread_Get_executing(), name, sizeof( name ) );
  if (strcmp(name, "IRQS") == 0) {
    /* Can't run this from within an IRQ Server thread context */
    return RTEMS_ILLEGAL_ON_SELF;
  }

  rtems_status_code sc = rtems_interrupt_server_handler_install(
    RTEMS_INTERRUPT_SERVER_DEFAULT,
    ucInterruptID,
    "CGEM Handler",
    RTEMS_INTERRUPT_UNIQUE,
    pxHandler,
    pvCallBackRef
  );

  return sc;
} 
