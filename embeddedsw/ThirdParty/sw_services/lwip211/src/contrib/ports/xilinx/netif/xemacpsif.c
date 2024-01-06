/*
 * Copyright (C) 2010 - 2021 Xilinx, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 */

#include <stdio.h>
#include <string.h>

#include <xparameters.h>
#include "lwipopts.h"
#include "xlwipconfig.h"
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include "lwip/igmp.h"

#include "netif/etharp.h"
#include "netif/xemacpsif.h"
#include "netif/xadapter.h"
#include "netif/xpqueue.h"
#include "xparameters.h"
#include "xscugic.h"
#include "xemacps.h"

#if LWIP_IPV6
#include "lwip/ethip6.h"
#endif


/* Define those to better describe your network interface. */
#define IFNAME0 't'
#define IFNAME1 'e'

#if LWIP_IGMP
static err_t xemacpsif_mac_filter_update (struct netif *netif,
#ifndef __rtems__
							ip_addr_t *group, u8_t action);
#else /* __rtems__ */
							const ip4_addr_t *group,
							enum netif_mac_filter_action action);
#endif

static u8_t xemacps_mcast_entry_mask = 0;
#endif

#if LWIP_IPV6 && LWIP_IPV6_MLD
static err_t xemacpsif_mld6_mac_filter_update (struct netif *netif,
#ifndef __rtems__
							ip_addr_t *group, u8_t action);
#else /* __rtems__ */
							const ip6_addr_t *group,
							enum netif_mac_filter_action action);
#endif

static u8_t xemacps_mld6_mcast_entry_mask;
#endif

XEmacPs_Config *mac_config;
struct netif *NetIf;

#if !NO_SYS
#if defined(__arm__) && !defined(ARMR5)
int32_t lExpireCounter = 0;
#define RESETRXTIMEOUT 10
#endif
#endif

#if LWIP_UDP_OPT_BLOCK_TX_TILL_COMPLETE
extern volatile u32_t notifyinfo[4*XLWIP_CONFIG_N_TX_DESC];
#endif

/*
 * this function is always called with interrupts off
 * this function also assumes that there are available BD's
 */
#if LWIP_UDP_OPT_BLOCK_TX_TILL_COMPLETE
static err_t _unbuffered_low_level_output(xemacpsif_s *xemacpsif,
		struct pbuf *p, u32_t block_till_tx_complete, u32_t *to_block_index )
#else
static err_t _unbuffered_low_level_output(xemacpsif_s *xemacpsif,
													struct pbuf *p)
#endif
{
	XStatus status = 0;
	err_t err = ERR_MEM;

#if ETH_PAD_SIZE
	pbuf_header(p, -ETH_PAD_SIZE);	/* drop the padding word */
#endif
#if LWIP_UDP_OPT_BLOCK_TX_TILL_COMPLETE
	if (block_till_tx_complete == 1) {
		status = emacps_sgsend(xemacpsif, p, 1, to_block_index);
	} else {
		status = emacps_sgsend(xemacpsif, p, 0, to_block_index);
	}
#else
	status = emacps_sgsend(xemacpsif, p);
#endif
	if (status != XST_SUCCESS) {
#if LINK_STATS
		lwip_stats.link.drop++;
#endif
	} else {
		err = ERR_OK;
	}

#if ETH_PAD_SIZE
	pbuf_header(p, ETH_PAD_SIZE);	/* reclaim the padding word */
#endif

#if LINK_STATS
	lwip_stats.link.xmit++;
#endif /* LINK_STATS */

	return err;

}

/*
 * low_level_output():
 *
 * Should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    err_t err = ERR_MEM;
    s32_t freecnt;
    XEmacPs_BdRing *txring;
#if LWIP_UDP_OPT_BLOCK_TX_TILL_COMPLETE
	u32_t notfifyblocksleepcntr;
	u32_t to_block_index;
#endif

	SYS_ARCH_DECL_PROTECT(lev);
	struct xemac_s *xemac = (struct xemac_s *)(netif->state);
	xemacpsif_s *xemacpsif = (xemacpsif_s *)(xemac->state);

	SYS_ARCH_PROTECT(lev);
	/* check if space is available to send */
    freecnt = is_tx_space_available(xemacpsif);
    if (freecnt <= 5) {
	txring = &(XEmacPs_GetTxRing(&xemacpsif->emacps));
		process_sent_bds(xemacpsif, txring);
	}

    if (is_tx_space_available(xemacpsif)) {
#if LWIP_UDP_OPT_BLOCK_TX_TILL_COMPLETE
		if (netif_is_opt_block_tx_set(netif, NETIF_ENABLE_BLOCKING_TX_FOR_PACKET)) {
			err = _unbuffered_low_level_output(xemacpsif, p, 1, &to_block_index);
		} else {
			err = _unbuffered_low_level_output(xemacpsif, p, 0, &to_block_index);
		}
#else
		err = _unbuffered_low_level_output(xemacpsif, p);
#endif
	} else {
#if LINK_STATS
		lwip_stats.link.drop++;
#endif
		printf("pack dropped, no space\r\n");
		SYS_ARCH_UNPROTECT(lev);
		goto return_pack_dropped;
	}
	SYS_ARCH_UNPROTECT(lev);

#if LWIP_UDP_OPT_BLOCK_TX_TILL_COMPLETE
	if (netif_is_opt_block_tx_set(netif, NETIF_ENABLE_BLOCKING_TX_FOR_PACKET)) {
		/* Wait for approx 1 second before timing out */
		notfifyblocksleepcntr = 900000;
		while(notifyinfo[to_block_index] == 1) {
			usleep(1);
			notfifyblocksleepcntr--;
			if (notfifyblocksleepcntr <= 0) {
				err = ERR_TIMEOUT;
				break;
			}
		}
	}
	netif_clear_opt_block_tx(netif, NETIF_ENABLE_BLOCKING_TX_FOR_PACKET);
#endif
return_pack_dropped:
	return err;
}

/*
 * low_level_input():
 *
 * Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 */
static struct pbuf * low_level_input(struct netif *netif)
{
	struct xemac_s *xemac = (struct xemac_s *)(netif->state);
	xemacpsif_s *xemacpsif = (xemacpsif_s *)(xemac->state);
	struct pbuf *p;

	/* see if there is data to process */
	if (pq_qlength(xemacpsif->recv_q) == 0)
		return NULL;

	/* return one packet from receive q */
	p = (struct pbuf *)pq_dequeue(xemacpsif->recv_q);
	return p;
}

/*
 * xemacpsif_output():
 *
 * This function is called by the TCP/IP stack when an IP packet
 * should be sent. It calls the function called low_level_output() to
 * do the actual transmission of the packet.
 *
 */

static err_t xemacpsif_output(struct netif *netif, struct pbuf *p,
#ifndef __rtems__
		const ip_addr_t *ipaddr)
#else /* __rtems__ */
		const ip4_addr_t *ipaddr)
#endif
{
	/* resolve hardware address, then send (or queue) packet */
	return etharp_output(netif, p, ipaddr);
}

/*
 * xemacpsif_input():
 *
 * This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface.
 *
 * Returns the number of packets read (max 1 packet on success,
 * 0 if there are no packets)
 *
 */

s32_t xemacpsif_input(struct netif *netif)
{
	struct eth_hdr *ethhdr;
	struct pbuf *p;
	SYS_ARCH_DECL_PROTECT(lev);

#if !NO_SYS
	while (1)
#endif
	{
		/* move received packet into a new pbuf */
		SYS_ARCH_PROTECT(lev);
		p = low_level_input(netif);
		SYS_ARCH_UNPROTECT(lev);

		/* no packet could be read, silently ignore this */
		if (p == NULL) {
			return 0;
		}

		/* points to packet payload, which starts with an Ethernet header */
		ethhdr = p->payload;

	#if LINK_STATS
		lwip_stats.link.recv++;
	#endif /* LINK_STATS */

		switch (htons(ethhdr->type)) {
			/* IP or ARP packet? */
			case ETHTYPE_IP:
			case ETHTYPE_ARP:
	#if LWIP_IPV6
			/*IPv6 Packet?*/
			case ETHTYPE_IPV6:
	#endif
	#if PPPOE_SUPPORT
				/* PPPoE packet? */
			case ETHTYPE_PPPOEDISC:
			case ETHTYPE_PPPOE:
	#endif /* PPPOE_SUPPORT */
				/* full packet send to tcpip_thread to process */
				if (netif->input(p, netif) != ERR_OK) {
					LWIP_DEBUGF(NETIF_DEBUG, ("xemacpsif_input: IP input error\r\n"));
					pbuf_free(p);
					p = NULL;
				}
				break;

			default:
				pbuf_free(p);
				p = NULL;
				break;
		}
	}

	return 1;
}

#if !NO_SYS
#if defined(__arm__) && !defined(ARMR5)
#ifndef __rtems__
void vTimerCallback( TimerHandle_t pxTimer )
{
	/* Do something if the pxTimer parameter is NULL */
	configASSERT(pxTimer);
#else /* __rtems__ */
static rtems_timer_service_routine vTimerCallback(
  rtems_id  timer,
  void     *arg
)
{
#endif
	lExpireCounter++;
	/* If the timer has expired 100 times then reset RX */
	if(lExpireCounter >= RESETRXTIMEOUT) {
		lExpireCounter = 0;
		xemacpsif_resetrx_on_no_rxdata(NetIf);
	}
}
#endif
#endif

static err_t low_level_init(struct netif *netif)
{
	UINTPTR mac_address = (UINTPTR)(netif->state);
	struct xemac_s *xemac;
	xemacpsif_s *xemacpsif;
	u32 dmacrreg;

	s32_t status = XST_SUCCESS;

	NetIf = netif;

	xemacpsif = mem_malloc(sizeof *xemacpsif);
	if (xemacpsif == NULL) {
		LWIP_DEBUGF(NETIF_DEBUG, ("xemacpsif_init: out of memory\r\n"));
		return ERR_MEM;
	}

	xemac = mem_malloc(sizeof *xemac);
	if (xemac == NULL) {
		LWIP_DEBUGF(NETIF_DEBUG, ("xemacpsif_init: out of memory\r\n"));
		return ERR_MEM;
	}

	xemac->state = (void *)xemacpsif;
	xemac->topology_index = xtopology_find_index(mac_address);
	xemac->type = xemac_type_emacps;

	xemacpsif->send_q = NULL;
	xemacpsif->recv_q = pq_create_queue();
	if (!xemacpsif->recv_q)
		return ERR_MEM;

	/* maximum transfer unit */
#ifdef ZYNQMP_USE_JUMBO
	netif->mtu = XEMACPS_MTU_JUMBO - XEMACPS_HDR_SIZE;
#else
	netif->mtu = XEMACPS_MTU - XEMACPS_HDR_SIZE;
#endif

#if LWIP_IGMP
	netif->igmp_mac_filter = xemacpsif_mac_filter_update;
#endif

#if LWIP_IPV6 && LWIP_IPV6_MLD
 netif->mld_mac_filter = xemacpsif_mld6_mac_filter_update;
#endif

	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
											NETIF_FLAG_LINK_UP;

#if LWIP_IPV6 && LWIP_IPV6_MLD
	netif->flags |= NETIF_FLAG_MLD6;
#endif

#if LWIP_IGMP
	netif->flags |= NETIF_FLAG_IGMP;
#endif

#if !NO_SYS
	sys_sem_new(&xemac->sem_rx_data_available, 0);
#endif
	/* obtain config of this emac */
	mac_config = (XEmacPs_Config *)xemacps_lookup_config((unsigned)(UINTPTR)netif->state);

#if defined (__aarch64__) && (EL1_NONSECURE == 1)
	/* Request device to indicate that this library is using it */
	if (mac_config->BaseAddress == VERSAL_EMACPS_0_BASEADDR) {
		Xil_Smc(PM_REQUEST_DEVICE_SMC_FID, DEV_GEM_0, 1, 0, 100, 1, 0, 0);
	}
	if (mac_config->BaseAddress == VERSAL_EMACPS_0_BASEADDR) {
		Xil_Smc(PM_REQUEST_DEVICE_SMC_FID, DEV_GEM_1, 1, 0, 100, 1, 0, 0);
	}
#endif

	status = XEmacPs_CfgInitialize(&xemacpsif->emacps, mac_config,
						mac_config->BaseAddress);
	if (status != XST_SUCCESS) {
		xil_printf("In %s:EmacPs Configuration Failed....\r\n", __func__);
	}

	/* initialize the mac */
	init_emacps(xemacpsif, netif);

	dmacrreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress,
														XEMACPS_DMACR_OFFSET);
	dmacrreg = dmacrreg | (0x00000010);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress,
											XEMACPS_DMACR_OFFSET, dmacrreg);
#if !NO_SYS
#if defined(__arm__) && !defined(ARMR5)
#ifndef __rtems__
	/* Freertos tick is 10ms by default; set period to the same */
	xemac->xTimer = xTimerCreate("Timer", 10, pdTRUE, ( void * ) 1, vTimerCallback);
	if (xemac->xTimer == NULL) {
		xil_printf("In %s:Timer creation failed....\r\n", __func__);
	} else {
		if(xTimerStart(xemac->xTimer, 0) != pdPASS) {
			xil_printf("In %s:Timer start failed....\r\n", __func__);
		}
	}
#else /* __rtems__ */
    rtems_status_code ret = rtems_timer_create( rtems_build_name( 'L', 'W', 'M', 'R' ), &xemac->xTimer);
    if(RTEMS_SUCCESSFUL == ret){
        ret = rtems_timer_fire_after(xemac->xTimer, rtems_clock_get_ticks_per_second()/100, vTimerCallback, NULL);
    }
    if(RTEMS_SUCCESSFUL != ret){
        xil_printf("In %s:Timer setup failed....\r\n", __func__);
    }
#endif	
#endif
#endif
	setup_isr(xemac);
	init_dma(xemac);
	start_emacps(xemacpsif);

	/* replace the state in netif (currently the emac baseaddress)
	 * with the mac instance pointer.
	 */
	netif->state = (void *)xemac;

	return ERR_OK;
}

void HandleEmacPsError(struct xemac_s *xemac)
{
	xemacpsif_s   *xemacpsif;
	s32_t status = XST_SUCCESS;
	u32 dmacrreg;

	SYS_ARCH_DECL_PROTECT(lev);
	SYS_ARCH_PROTECT(lev);

	xemacpsif = (xemacpsif_s *)(xemac->state);
	free_txrx_pbufs(xemacpsif);
	status = XEmacPs_CfgInitialize(&xemacpsif->emacps, mac_config,
						mac_config->BaseAddress);
	if (status != XST_SUCCESS) {
		xil_printf("In %s:EmacPs Configuration Failed....\r\n", __func__);
	}
	/* initialize the mac */
	init_emacps_on_error(xemacpsif, NetIf);
	dmacrreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress,
														XEMACPS_DMACR_OFFSET);
	dmacrreg = dmacrreg | (0x01000000);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress,
											XEMACPS_DMACR_OFFSET, dmacrreg);
	setup_isr(xemac);
	init_dma(xemac);
	start_emacps(xemacpsif);

	SYS_ARCH_UNPROTECT(lev);
}

void HandleTxErrors(struct xemac_s *xemac)
{
	xemacpsif_s   *xemacpsif;
	u32 netctrlreg;

	SYS_ARCH_DECL_PROTECT(lev);
	SYS_ARCH_PROTECT(lev);
	xemacpsif = (xemacpsif_s *)(xemac->state);
	netctrlreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress,
												XEMACPS_NWCTRL_OFFSET);
    netctrlreg = netctrlreg & (~XEMACPS_NWCTRL_TXEN_MASK);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress,
									XEMACPS_NWCTRL_OFFSET, netctrlreg);
	free_onlytx_pbufs(xemacpsif);

	clean_dma_txdescs(xemac);
	netctrlreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress,
													XEMACPS_NWCTRL_OFFSET);
	netctrlreg = netctrlreg | (XEMACPS_NWCTRL_TXEN_MASK);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress,
										XEMACPS_NWCTRL_OFFSET, netctrlreg);
	SYS_ARCH_UNPROTECT(lev);
}

#if LWIP_IPV6 && LWIP_IPV6_MLD
#ifndef __rtems__
static u8_t xemacpsif_ip6_addr_ismulticast(ip6_addr_t* ip_addr)
#else /* __rtems__ */
static u8_t xemacpsif_ip6_addr_ismulticast(const ip6_addr_t* ip_addr)
#endif
{
	if(ip6_addr_ismulticast_linklocal(ip_addr)||
           ip6_addr_ismulticast_iflocal(ip_addr)   ||
           ip6_addr_ismulticast_adminlocal(ip_addr)||
           ip6_addr_ismulticast_sitelocal(ip_addr) ||
           ip6_addr_ismulticast_orglocal(ip_addr)  ||
           ip6_addr_ismulticast_global(ip_addr)) {
	/*Return TRUE if IPv6 is Multicast type*/
	return TRUE;
	} else {
	return FALSE;
	}
}

static void xemacpsif_mld6_mac_hash_update (struct netif *netif, u8_t *ip_addr,
#ifndef __rtems__
		u8_t action)
#else /* __rtems__ */
		enum netif_mac_filter_action action)
#endif
{
	u8_t multicast_mac_addr[6];
	struct xemac_s *xemac = (struct xemac_s *) (netif->state);
	xemacpsif_s *xemacpsif = (xemacpsif_s *) (xemac->state);
	XEmacPs_BdRing *txring;
	txring = &(XEmacPs_GetTxRing(&xemacpsif->emacps));

	multicast_mac_addr[0] = LL_IP6_MULTICAST_ADDR_0;
	multicast_mac_addr[1] = LL_IP6_MULTICAST_ADDR_1;
	multicast_mac_addr[2] = ip_addr[12];
	multicast_mac_addr[3] = ip_addr[13];
	multicast_mac_addr[4] = ip_addr[14];
	multicast_mac_addr[5] = ip_addr[15];

	/* Wait till all sent packets are acknowledged from HW */
	while(txring->HwCnt);

	SYS_ARCH_DECL_PROTECT(lev);

	SYS_ARCH_PROTECT(lev);

	/* Stop Ethernet */
	XEmacPs_Stop(&xemacpsif->emacps);

	if (action == NETIF_ADD_MAC_FILTER) {
		/* Set Mulitcast mac address in hash table */
		XEmacPs_SetHash(&xemacpsif->emacps, multicast_mac_addr);

	} else if (action == NETIF_DEL_MAC_FILTER) {
		/* Remove Mulitcast mac address in hash table */
		XEmacPs_DeleteHash(&xemacpsif->emacps, multicast_mac_addr);
	}

	/* Reset DMA */
	reset_dma(xemac);

	/* Start Ethernet */
	XEmacPs_Start(&xemacpsif->emacps);

	SYS_ARCH_UNPROTECT(lev);
}

#ifndef __rtems__
static err_t xemacpsif_mld6_mac_filter_update (struct netif *netif, ip_addr_t *group,
		u8_t action)
#else /* __rtems__ */
static err_t xemacpsif_mld6_mac_filter_update (struct netif *netif,
							const ip6_addr_t *group,
							enum netif_mac_filter_action action)
#endif
{
	u8_t temp_mask;
	unsigned int i;
	u8_t * ip_addr = (u8_t *) group;

#ifndef __rtems__
	if(!(xemacpsif_ip6_addr_ismulticast((ip6_addr_t*) ip_addr))) {
#else /* __rtems__ */
	if(!(xemacpsif_ip6_addr_ismulticast( group ))) {
#endif
		LWIP_DEBUGF(NETIF_DEBUG,
                                ("%s: The requested MAC address is not a multicast address.\r\n", __func__));								 LWIP_DEBUGF(NETIF_DEBUG,
		                ("Multicast address add operation failure !!\r\n"));
                        return ERR_ARG;
	}
	if (action == NETIF_ADD_MAC_FILTER) {
		for (i = 0; i < XEMACPS_MAX_MAC_ADDR; i++) {
			temp_mask = (0x01) << i;
			if ((xemacps_mld6_mcast_entry_mask & temp_mask) == temp_mask) {
				continue;
			}
			xemacps_mld6_mcast_entry_mask |= temp_mask;

			/* Update mac address in hash table */
			xemacpsif_mld6_mac_hash_update(netif, ip_addr, action);

			LWIP_DEBUGF(NETIF_DEBUG,
					("%s: Multicast MAC address successfully added.\r\n", __func__));

			return ERR_OK;
		}
		LWIP_DEBUGF(NETIF_DEBUG,
				("%s: No multicast address registers left.\r\n", __func__));
		LWIP_DEBUGF(NETIF_DEBUG,
				("Multicast MAC address add operation failure !!\r\n"));
		return ERR_MEM;
	} else if (action == NETIF_DEL_MAC_FILTER) {
		for (i = 0; i < XEMACPS_MAX_MAC_ADDR; i++) {
			temp_mask = (0x01) << i;
			if ((xemacps_mld6_mcast_entry_mask & temp_mask) != temp_mask) {
				continue;
			}
			xemacps_mld6_mcast_entry_mask &= (~temp_mask);

			/* Update mac address in hash table */
			xemacpsif_mld6_mac_hash_update(netif, ip_addr, action);

			LWIP_DEBUGF(NETIF_DEBUG,
					("%s: Multicast MAC address successfully removed.\r\n", __func__));

			return ERR_OK;
		}
		LWIP_DEBUGF(NETIF_DEBUG,
				("%s: No multicast address registers present with\r\n", __func__));
		LWIP_DEBUGF(NETIF_DEBUG,
				("the requested Multicast MAC address.\r\n"));
		LWIP_DEBUGF(NETIF_DEBUG,
				("Multicast MAC address removal failure!!.\r\n"));
		return ERR_MEM;
	}
	return ERR_ARG;
}
#endif

#if LWIP_IGMP
#ifndef __rtems__
static void xemacpsif_mac_hash_update (struct netif *netif, u8_t *ip_addr,
		u8_t action)
#else /* __rtems__ */
static void xemacpsif_mac_hash_update (struct netif *netif, u8_t *ip4_addr_t,
		enum netif_mac_filter_action action)
#endif
{
	u8_t multicast_mac_addr[6];
	struct xemac_s *xemac = (struct xemac_s *) (netif->state);
	xemacpsif_s *xemacpsif = (xemacpsif_s *) (xemac->state);
	XEmacPs_BdRing *txring;
	txring = &(XEmacPs_GetTxRing(&xemacpsif->emacps));

	multicast_mac_addr[0] = 0x01;
	multicast_mac_addr[1] = 0x00;
	multicast_mac_addr[2] = 0x5E;
	multicast_mac_addr[3] = ip_addr[1] & 0x7F;
	multicast_mac_addr[4] = ip_addr[2];
	multicast_mac_addr[5] = ip_addr[3];

	/* Wait till all sent packets are acknowledged from HW */
	while(txring->HwCnt);

	SYS_ARCH_DECL_PROTECT(lev);

	SYS_ARCH_PROTECT(lev);

	/* Stop Ethernet */
	XEmacPs_Stop(&xemacpsif->emacps);

	if (action == IGMP_ADD_MAC_FILTER) {
		/* Set Mulitcast mac address in hash table */
		XEmacPs_SetHash(&xemacpsif->emacps, multicast_mac_addr);

	} else if (action == IGMP_DEL_MAC_FILTER) {
		/* Remove Mulitcast mac address in hash table */
		XEmacPs_DeleteHash(&xemacpsif->emacps, multicast_mac_addr);
	}

	/* Reset DMA */
	reset_dma(xemac);

	/* Start Ethernet */
	XEmacPs_Start(&xemacpsif->emacps);

	SYS_ARCH_UNPROTECT(lev);
}

static err_t xemacpsif_mac_filter_update (struct netif *netif, ip_addr_t *group,
		u8_t action)
{
	u8_t temp_mask;
	unsigned int i;
	u8_t * ip_addr = (u8_t *) group;

	if ((ip_addr[0] < 224) && (ip_addr[0] > 239)) {
		LWIP_DEBUGF(NETIF_DEBUG,
				("%s: The requested MAC address is not a multicast address.\r\n", __func__));
		LWIP_DEBUGF(NETIF_DEBUG,
				("Multicast address add operation failure !!\r\n"));

		return ERR_ARG;
	}

	if (action == IGMP_ADD_MAC_FILTER) {

		for (i = 0; i < XEMACPS_MAX_MAC_ADDR; i++) {
			temp_mask = (0x01) << i;
			if ((xemacps_mcast_entry_mask & temp_mask) == temp_mask) {
				continue;
			}
			xemacps_mcast_entry_mask |= temp_mask;

			/* Update mac address in hash table */
			xemacpsif_mac_hash_update(netif, ip_addr, action);

			LWIP_DEBUGF(NETIF_DEBUG,
					("%s: Multicast MAC address successfully added.\r\n", __func__));

			return ERR_OK;
		}
		if (i == XEMACPS_MAX_MAC_ADDR) {
			LWIP_DEBUGF(NETIF_DEBUG,
					("%s: No multicast address registers left.\r\n", __func__));
			LWIP_DEBUGF(NETIF_DEBUG,
					("Multicast MAC address add operation failure !!\r\n"));

			return ERR_MEM;
		}
	} else if (action == IGMP_DEL_MAC_FILTER) {
		for (i = 0; i < XEMACPS_MAX_MAC_ADDR; i++) {
			temp_mask = (0x01) << i;
			if ((xemacps_mcast_entry_mask & temp_mask) != temp_mask) {
				continue;
			}
			xemacps_mcast_entry_mask &= (~temp_mask);

			/* Update mac address in hash table */
			xemacpsif_mac_hash_update(netif, ip_addr, action);

			LWIP_DEBUGF(NETIF_DEBUG,
					("%s: Multicast MAC address successfully removed.\r\n", __func__));

			return ERR_OK;
		}
		if (i == XEMACPS_MAX_MAC_ADDR) {
			LWIP_DEBUGF(NETIF_DEBUG,
					("%s: No multicast address registers present with\r\n", __func__));
			LWIP_DEBUGF(NETIF_DEBUG,
					("the requested Multicast MAC address.\r\n"));
			LWIP_DEBUGF(NETIF_DEBUG,
					("Multicast MAC address removal failure!!.\r\n"));

			return ERR_MEM;
		}
	}
	return ERR_OK;
}
#endif

/*
 * xemacpsif_init():
 *
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 */

err_t xemacpsif_init(struct netif *netif)
{
#if LWIP_SNMP
	/* ifType ethernetCsmacd(6) @see RFC1213 */
	netif->link_type = 6;
	/* your link speed here */
	netif->link_speed = ;
	netif->ts = 0;
	netif->ifinoctets = 0;
	netif->ifinucastpkts = 0;
	netif->ifinnucastpkts = 0;
	netif->ifindiscards = 0;
	netif->ifoutoctets = 0;
	netif->ifoutucastpkts = 0;
	netif->ifoutnucastpkts = 0;
	netif->ifoutdiscards = 0;
#endif

	netif->name[0] = IFNAME0;
	netif->name[1] = IFNAME1;
	netif->output = xemacpsif_output;
	netif->linkoutput = low_level_output;
#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
#endif

	low_level_init(netif);
	return ERR_OK;
}

/*
 * xemacpsif_resetrx_on_no_rxdata():
 *
 * Should be called by the user at regular intervals, typically
 * from a timer (100 msecond). This is to provide a SW workaround
 * for the HW bug (SI #692601). Please refer to the function header
 * for the function resetrx_on_no_rxdata in xemacpsif_dma.c to
 * know more about the SI.
 *
 */

void xemacpsif_resetrx_on_no_rxdata(struct netif *netif)
{
	struct xemac_s *xemac = (struct xemac_s *)(netif->state);
	xemacpsif_s *xemacpsif = (xemacpsif_s *)(xemac->state);

	resetrx_on_no_rxdata(xemacpsif);
}
