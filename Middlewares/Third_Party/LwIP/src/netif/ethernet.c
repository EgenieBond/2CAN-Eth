/**
 * @file
 * Ethernet common functions
 *
 * @defgroup ethernet Ethernet
 * @ingroup callbackstyle_api
 */

/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * Copyright (c) 2003-2004 Leon Woestenberg <leon.woestenberg@axon.tv>
 * Copyright (c) 2003-2004 Axon Digital Design B.V., The Netherlands.
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

#include "lwip/opt.h"

#if LWIP_ARP || LWIP_ETHERNET

#include "netif/ethernet.h"
#include "lwip/def.h"
#include "lwip/stats.h"
#include "lwip/etharp.h"
#include "lwip/ip.h"
#include "lwip/snmp.h"
#include "debug_uart.h"

#include <string.h>

#include "netif/ppp/ppp_opts.h"
#if PPPOE_SUPPORT
#include "netif/ppp/pppoe.h"
#endif /* PPPOE_SUPPORT */

#ifdef LWIP_HOOK_FILENAME
#include LWIP_HOOK_FILENAME
#endif

const struct eth_addr ethbroadcast = {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
const struct eth_addr ethzero = {{0, 0, 0, 0, 0, 0}};

/**
 * @ingroup lwip_nosys
 * Process received ethernet frames. Using this function instead of directly
 * calling ip_input and passing ARP frames through etharp in ethernetif_input,
 * the ARP cache is protected from concurrent access.\n
 * Don't call directly, pass to netif_add() and call netif->input().
 *
 * @param p the received packet, p->payload pointing to the ethernet header
 * @param netif the network interface on which the packet was received
 *
 * @see LWIP_HOOK_UNKNOWN_ETH_PROTOCOL
 * @see ETHARP_SUPPORT_VLAN
 * @see LWIP_HOOK_VLAN_CHECK
 */
err_t
ethernet_input(struct pbuf *p, struct netif *netif)
{
  struct eth_hdr *ethhdr;

  DebugUART_Print("[ETH-IN] sizeof(struct eth_addr)=%u sizeof(struct eth_hdr)=%u\r\n",
                  (unsigned)sizeof(struct eth_addr),
                  (unsigned)sizeof(struct eth_hdr));

  u16_t type;
#if LWIP_ARP || ETHARP_SUPPORT_VLAN || LWIP_IPV6
  u16_t next_hdr_offset = SIZEOF_ETH_HDR;
#endif /* LWIP_ARP || ETHARP_SUPPORT_VLAN || LWIP_IPV6 */

  LWIP_ASSERT_CORE_LOCKED();

  if ((p == NULL) || (netif == NULL)) {
    DebugUART_Print("[ETH-IN] NULL arg: p=%p netif=%p\r\n", (void *)p, (void *)netif);
    if (p != NULL) {
      pbuf_free(p);
    }
    return ERR_ARG;
  }

  DebugUART_Print("[ETH-IN] ENTER p=%p len=%u tot=%u ref=%u payload=%p netif=%p flags=0x%02X\r\n",
                  (void *)p,
                  (unsigned)p->len,
                  (unsigned)p->tot_len,
                  (unsigned)p->ref,
                  p->payload,
                  (void *)netif,
                  (unsigned)netif->flags);

  if (p->len <= SIZEOF_ETH_HDR) {
    DebugUART_Print("[ETH-IN] DROP short frame: len=%u <= ETH_HDR=%u\r\n",
                    (unsigned)p->len,
                    (unsigned)SIZEOF_ETH_HDR);
    ETHARP_STATS_INC(etharp.proterr);
    ETHARP_STATS_INC(etharp.drop);
    MIB2_STATS_NETIF_INC(netif, ifinerrors);
    goto free_and_return;
  }

  if (p->if_idx == NETIF_NO_INDEX) {
    p->if_idx = netif_get_index(netif);
  }

  ethhdr = (struct eth_hdr *)p->payload;

  DebugUART_Print("[ETH-IN] ETH hdr dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X type=0x%04X\r\n",
                  ethhdr->dest.addr[0], ethhdr->dest.addr[1], ethhdr->dest.addr[2],
                  ethhdr->dest.addr[3], ethhdr->dest.addr[4], ethhdr->dest.addr[5],
                  ethhdr->src.addr[0],  ethhdr->src.addr[1],  ethhdr->src.addr[2],
                  ethhdr->src.addr[3],  ethhdr->src.addr[4],  ethhdr->src.addr[5],
                  (unsigned)lwip_ntohs(ethhdr->type));

  type = ethhdr->type;

#if ETHARP_SUPPORT_VLAN
  if (type == PP_HTONS(ETHTYPE_VLAN)) {
    struct eth_vlan_hdr *vlan = (struct eth_vlan_hdr *)(((char *)ethhdr) + SIZEOF_ETH_HDR);
    next_hdr_offset = SIZEOF_ETH_HDR + SIZEOF_VLAN_HDR;

    if (p->len <= SIZEOF_ETH_HDR + SIZEOF_VLAN_HDR) {
      DebugUART_Print("[ETH-IN] DROP short VLAN frame len=%u\r\n", (unsigned)p->len);
      ETHARP_STATS_INC(etharp.proterr);
      ETHARP_STATS_INC(etharp.drop);
      MIB2_STATS_NETIF_INC(netif, ifinerrors);
      goto free_and_return;
    }

    DebugUART_Print("[ETH-IN] VLAN detected: prio_vid=0x%04X inner_type=0x%04X\r\n",
                    (unsigned)lwip_ntohs(vlan->prio_vid),
                    (unsigned)lwip_ntohs(vlan->tpid));

#if defined(LWIP_HOOK_VLAN_CHECK) || defined(ETHARP_VLAN_CHECK) || defined(ETHARP_VLAN_CHECK_FN)
#ifdef LWIP_HOOK_VLAN_CHECK
    if (!LWIP_HOOK_VLAN_CHECK(netif, ethhdr, vlan)) {
#elif defined(ETHARP_VLAN_CHECK_FN)
    if (!ETHARP_VLAN_CHECK_FN(ethhdr, vlan)) {
#elif defined(ETHARP_VLAN_CHECK)
    if (VLAN_ID(vlan) != ETHARP_VLAN_CHECK) {
#endif
      DebugUART_Print("[ETH-IN] VLAN check failed -> DROP silently\r\n");
      pbuf_free(p);
      return ERR_OK;
    }
#endif
    type = vlan->tpid;
  }
#endif /* ETHARP_SUPPORT_VLAN */

#if LWIP_ARP_FILTER_NETIF
  netif = LWIP_ARP_FILTER_NETIF_FN(p, netif, lwip_htons(type));
  DebugUART_Print("[ETH-IN] after ARP_FILTER_NETIF netif=%p\r\n", (void *)netif);
#endif /* LWIP_ARP_FILTER_NETIF */

  if (ethhdr->dest.addr[0] & 1) {
    if (ethhdr->dest.addr[0] == LL_IP4_MULTICAST_ADDR_0) {
#if LWIP_IPV4
      if ((ethhdr->dest.addr[1] == LL_IP4_MULTICAST_ADDR_1) &&
          (ethhdr->dest.addr[2] == LL_IP4_MULTICAST_ADDR_2)) {
        p->flags |= PBUF_FLAG_LLMCAST;
        DebugUART_Print("[ETH-IN] mark LLMCAST (IPv4 multicast)\r\n");
      }
#endif /* LWIP_IPV4 */
    }
#if LWIP_IPV6
    else if ((ethhdr->dest.addr[0] == LL_IP6_MULTICAST_ADDR_0) &&
             (ethhdr->dest.addr[1] == LL_IP6_MULTICAST_ADDR_1)) {
      p->flags |= PBUF_FLAG_LLMCAST;
      DebugUART_Print("[ETH-IN] mark LLMCAST (IPv6 multicast)\r\n");
    }
#endif /* LWIP_IPV6 */
    else if (eth_addr_cmp(&ethhdr->dest, &ethbroadcast)) {
      p->flags |= PBUF_FLAG_LLBCAST;
      DebugUART_Print("[ETH-IN] mark LLBCAST\r\n");
    }
  }

  switch (type) {
#if LWIP_IPV4 && LWIP_ARP
    case PP_HTONS(ETHTYPE_IP):
      DebugUART_Print("[ETH-IN] CASE ETHTYPE_IP flags=0x%02X\r\n", (unsigned)netif->flags);

      if (!(netif->flags & NETIF_FLAG_ETHARP)) {
        DebugUART_Print("[ETH-IN] DROP IPv4: NETIF_FLAG_ETHARP not set\r\n");
        goto free_and_return;
      }

      if (pbuf_remove_header(p, next_hdr_offset)) {
        DebugUART_Print("[ETH-IN] DROP IPv4: pbuf_remove_header failed tot=%u off=%u\r\n",
                        (unsigned)p->tot_len,
                        (unsigned)next_hdr_offset);
        goto free_and_return;
      } else {
        DebugUART_Print("[ETH-IN] IPv4 -> ip4_input p=%p payload=%p len=%u tot=%u\r\n",
                        (void *)p,
                        p->payload,
                        (unsigned)p->len,
                        (unsigned)p->tot_len);
        ip4_input(p, netif);
      }
      break;

    case PP_HTONS(ETHTYPE_ARP):
      DebugUART_Print("[ETH-IN] CASE ETHTYPE_ARP flags=0x%02X\r\n", (unsigned)netif->flags);

      if (!(netif->flags & NETIF_FLAG_ETHARP)) {
        DebugUART_Print("[ETH-IN] DROP ARP: NETIF_FLAG_ETHARP not set\r\n");
        goto free_and_return;
      }

      if (pbuf_remove_header(p, next_hdr_offset)) {
        DebugUART_Print("[ETH-IN] DROP ARP: pbuf_remove_header failed tot=%u off=%u\r\n",
                        (unsigned)p->tot_len,
                        (unsigned)next_hdr_offset);
        ETHARP_STATS_INC(etharp.lenerr);
        ETHARP_STATS_INC(etharp.drop);
        goto free_and_return;
      } else {
        DebugUART_Print("[ETH-IN] ARP -> etharp_input p=%p payload=%p len=%u tot=%u\r\n",
                        (void *)p,
                        p->payload,
                        (unsigned)p->len,
                        (unsigned)p->tot_len);
        etharp_input(p, netif);
        DebugUART_Print("[ETH-IN] returned from etharp_input\r\n");
      }
      break;
#endif /* LWIP_IPV4 && LWIP_ARP */

#if PPPOE_SUPPORT
    case PP_HTONS(ETHTYPE_PPPOEDISC):
      DebugUART_Print("[ETH-IN] CASE PPPOEDISC\r\n");
      pppoe_disc_input(netif, p);
      break;

    case PP_HTONS(ETHTYPE_PPPOE):
      DebugUART_Print("[ETH-IN] CASE PPPOE\r\n");
      pppoe_data_input(netif, p);
      break;
#endif /* PPPOE_SUPPORT */

#if LWIP_IPV6
    case PP_HTONS(ETHTYPE_IPV6):
      DebugUART_Print("[ETH-IN] CASE ETHTYPE_IPV6\r\n");

      if ((p->len < next_hdr_offset) || pbuf_remove_header(p, next_hdr_offset)) {
        DebugUART_Print("[ETH-IN] DROP IPv6: too short tot=%u off=%u\r\n",
                        (unsigned)p->tot_len,
                        (unsigned)next_hdr_offset);
        goto free_and_return;
      } else {
        DebugUART_Print("[ETH-IN] IPv6 -> ip6_input p=%p payload=%p len=%u tot=%u\r\n",
                        (void *)p,
                        p->payload,
                        (unsigned)p->len,
                        (unsigned)p->tot_len);
        ip6_input(p, netif);
      }
      break;
#endif /* LWIP_IPV6 */

    default:
      DebugUART_Print("[ETH-IN] DEFAULT type=0x%04X -> DROP\r\n",
                      (unsigned)lwip_ntohs(type));
#ifdef LWIP_HOOK_UNKNOWN_ETH_PROTOCOL
      if (LWIP_HOOK_UNKNOWN_ETH_PROTOCOL(p, netif) == ERR_OK) {
        DebugUART_Print("[ETH-IN] unknown protocol handled by hook\r\n");
        break;
      }
#endif
      ETHARP_STATS_INC(etharp.proterr);
      ETHARP_STATS_INC(etharp.drop);
      MIB2_STATS_NETIF_INC(netif, ifinunknownprotos);
      goto free_and_return;
  }

  DebugUART_Print("[ETH-IN] EXIT ERR_OK (packet consumed)\r\n");
  return ERR_OK;

free_and_return:
  DebugUART_Print("[ETH-IN] free_and_return p=%p\r\n", (void *)p);
  pbuf_free(p);
  DebugUART_Print("[ETH-IN] EXIT ERR_OK after free\r\n");
  return ERR_OK;
}

/**
 * @ingroup ethernet
 * Send an ethernet packet on the network using netif->linkoutput().
 * The ethernet header is filled in before sending.
 *
 * @see LWIP_HOOK_VLAN_SET
 *
 * @param netif the lwIP network interface on which to send the packet
 * @param p the packet to send. pbuf layer must be @ref PBUF_LINK.
 * @param src the source MAC address to be copied into the ethernet header
 * @param dst the destination MAC address to be copied into the ethernet header
 * @param eth_type ethernet type (@ref lwip_ieee_eth_type)
 * @return ERR_OK if the packet was sent, any other err_t on failure
 */
err_t
ethernet_output(struct netif * netif, struct pbuf * p,
                const struct eth_addr * src, const struct eth_addr * dst,
                u16_t eth_type) {
  struct eth_hdr *ethhdr;
  u16_t eth_type_be = lwip_htons(eth_type);

#if ETHARP_SUPPORT_VLAN && defined(LWIP_HOOK_VLAN_SET)
  s32_t vlan_prio_vid = LWIP_HOOK_VLAN_SET(netif, p, src, dst, eth_type);
  if (vlan_prio_vid >= 0) {
    struct eth_vlan_hdr *vlanhdr;

    LWIP_ASSERT("prio_vid must be <= 0xFFFF", vlan_prio_vid <= 0xFFFF);

    if (pbuf_add_header(p, SIZEOF_ETH_HDR + SIZEOF_VLAN_HDR) != 0) {
      goto pbuf_header_failed;
    }
    vlanhdr = (struct eth_vlan_hdr *)(((u8_t *)p->payload) + SIZEOF_ETH_HDR);
    vlanhdr->tpid     = eth_type_be;
    vlanhdr->prio_vid = lwip_htons((u16_t)vlan_prio_vid);

    eth_type_be = PP_HTONS(ETHTYPE_VLAN);
  } else
#endif /* ETHARP_SUPPORT_VLAN && defined(LWIP_HOOK_VLAN_SET) */
  {
    if (pbuf_add_header(p, SIZEOF_ETH_HDR) != 0) {
      goto pbuf_header_failed;
    }
  }

  LWIP_ASSERT_CORE_LOCKED();

  ethhdr = (struct eth_hdr *)p->payload;
  ethhdr->type = eth_type_be;
  SMEMCPY(&ethhdr->dest, dst, ETH_HWADDR_LEN);
  SMEMCPY(&ethhdr->src,  src, ETH_HWADDR_LEN);

  LWIP_ASSERT("netif->hwaddr_len must be 6 for ethernet_output!",
              (netif->hwaddr_len == ETH_HWADDR_LEN));
  LWIP_DEBUGF(ETHARP_DEBUG | LWIP_DBG_TRACE,
              ("ethernet_output: sending packet %p\n", (void *)p));

  /* send the packet */
  return netif->linkoutput(netif, p);

pbuf_header_failed:
  LWIP_DEBUGF(ETHARP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_SERIOUS,
              ("ethernet_output: could not allocate room for header.\n"));
  LINK_STATS_INC(link.lenerr);
  return ERR_BUF;
}

#endif /* LWIP_ARP || LWIP_ETHERNET */
