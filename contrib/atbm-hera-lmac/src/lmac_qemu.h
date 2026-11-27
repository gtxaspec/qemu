/*
 * lmac_qemu: a stand-in for the ATBM6441's lower MAC, for QEMU
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LMAC_QEMU_H
#define LMAC_QEMU_H

#include <stdint.h>

/*
 * The paravirtual port, as QEMU's include/hw/net/atbm-hera-lmac.h defines
 * it (ABI 1). Kept in step by hand: the shim builds without QEMU's tree.
 */
#define QL_BASE             0x0a580000u
#define QL_IRQ              0
#define QL_ID               0x00
#define QL_VERSION          0x04
#define QL_CTRL             0x08
#define QL_STATUS           0x0c
#define QL_IRQ_EN           0x10
#define QL_REQ_SIZE         0x14
#define QL_REQ              0x18
#define QL_REQ_RESULT       0x1c
#define QL_IND_ADDR         0x20
#define QL_IND_SIZE         0x24
#define QL_IND_POP          0x28
#define QL_IND_DROPPED      0x2c

#define QL_MAGIC            0x434d4c51u
#define QL_ABI              1
#define QL_CTRL_RUN         (1u << 0)
#define QL_STATUS_PEND      (1u << 0)
#define QL_RESULT_CNF       (1u << 0)

static inline uint32_t ql_read(unsigned reg)
{
    return *(volatile uint32_t *)(QL_BASE + reg); /* MMIO */
}

static inline void ql_write(unsigned reg, uint32_t val)
{
    *(volatile uint32_t *)(QL_BASE + reg) = val; /* MMIO */
}

/*
 * What the firmware's kept objects provide and the shim calls. These are
 * the signatures their callers in the SDK's libraries use; none of them is
 * in a public header.
 */
typedef void (*ql_irq_fn)(unsigned prio);
typedef void (*ql_event_fn)(void);

void IRQ_RegisterHandler(unsigned irq, ql_irq_fn fn);
void lmac_RegisterEventHandler(uint32_t event, ql_event_fn fn);
void lmac_IrqSetEvent(uint32_t event);
void lmac_SetEvent(uint32_t event);
void lmac_Scheduler(void);
uint32_t OS_DisableFiqIrq_Internal(void);
void OS_RestoreFiqIrq_Internal(uint32_t psw);
void OS_EnableFiqIrq(void);
void APB_Init(void);
void HIF_Init(void);
void TI_Init(void);
void change_cpu_freq(int fast);
void HiInternal_Init(void **bufs, unsigned n);
void HiInternal_ProgOutput(void *msg);
void HiInternal_ProgramInput(void *msg);
int printf(const char *fmt, ...);

/* the LMAC packet memory (sm_region.o, kept) */
extern uint8_t SM_Region[];

/* the SDIO build: hi_sdio.o, and the demo's receive filters (user_main.c) */
void HiSdio_ProgInput(void *msg);
void HiSdio_ProgOutput(void *msg);
int atbmwifi_tcpfilter(uint16_t dport, uint16_t sport, void *tcp, void *ip,
                       int ipv6);
int atbmwifi_udpfilter(uint16_t dport, uint16_t sport, void *udp, void *ip,
                       int ipv6);
int atbmwifi_dnsfilter(uint16_t dport, uint16_t sport, int ipv6);
int atbmwifi_icmpfilter(int ipv6);
int atbmwifi_arpfilter(void);
int HOST_SDIO_INIT_DONE(void);
int HOST_SDIO_can_wakeup_host(void);
int HI_SDIO_Support_OtherIP_In_Sleep(void);
int test_match_frag_identifier(uint16_t id);
void set_udp_frag_identifier(uint16_t dport, uint16_t sport, uint16_t id);

/* the LMAC's MIB flags, in stubs.c */
extern uint32_t MIB_Flags;

/*
 * Hand a WSM request in a buffer of size bytes to the port; true if its
 * confirm is now in the buffer
 */
int lmac_qemu_request(void *msg, unsigned size);

/*
 * A received frame's indication from the port: what else takes it. HMAC
 * gets it (RX_HMAC); the SDIO host took it (RX_TAKEN, it frees it with
 * SYS_HiIndMsgSent); or no one wants it (RX_DROP)
 */
#define LMAC_QEMU_RX_HMAC   0
#define LMAC_QEMU_RX_TAKEN  1
#define LMAC_QEMU_RX_DROP   (-1)
int lmac_qemu_rx_route(void *msg);
void *lmac_qemu_rx_buf(void);
void SYS_HiIndMsgSent(void *msg);

#endif
