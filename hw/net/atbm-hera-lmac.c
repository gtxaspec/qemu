/*
 * ATBM6441 (Hera) paravirtual LMAC port
 *
 * The register interface is in include/hw/net/atbm-hera-lmac.h. Behind it
 * is the ATBM60xx firmware model (atbm-wifi.c) in its plain WSM dialect,
 * the one a Linux host speaks: the Hera's own upper MAC, running on the
 * chip's CPU, plays that host.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qemu/timer.h"
#include "net/eth.h"
#include "hw/net/atbm-hera-lmac.h"
#include "atbm-wifi.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraLmacState, ATBM_HERA_LMAC)

#define WSM_CNF                 0x0400
#define WSM_IND                 0x0800
/* the bits of an id past its sequence number */
#define WSM_ID_MASK             0x1fff

/* the largest WSM message the port reads: a frame and its descriptor */
#define REQ_MAX                 4096

/* a received frame's indication: the frame follows header and descriptor */
#define WSM_RECEIVE_IND         0x0804
#define WSM_ID_TYPE             0x1c3f
#define RX_FRAME                (4 + 16)
#define FC_TYPE_DATA            2

struct AtbmHeraLmacState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    AtbmCore core;

    uint32_t ctrl;
    uint32_t irq_en;
    uint32_t req_size;
    uint32_t req_result;
    uint32_t ind_addr;
    uint32_t ind_size;
    uint32_t ind_dropped;

    qemu_irq wake;
    QEMUTimer *wake_timer;
    uint32_t ps;
    uint32_t wake_mask;
    uint32_t wake_delay;
    uint32_t wake_count;
    bool waking;
};

/* A frame waiting for the sleeping chip that it asked to be woken for */
static bool atbm_hera_lmac_wanted(AtbmHeraLmacState *s)
{
    for (GList *l = s->core.out.head; l; l = l->next) {
        GByteArray *m = l->data;
        const uint8_t *f = m->data + RX_FRAME;
        bool group;

        if (m->len < RX_FRAME + 10 ||
            (lduw_le_p(m->data + 2) & WSM_ID_TYPE) != WSM_RECEIVE_IND ||
            ((lduw_le_p(f) >> 2) & 3) != FC_TYPE_DATA) {
            continue;
        }
        group = is_multicast_ether_addr(f + 4);
        if (s->wake_mask & (group ? ATBM_HERA_LMAC_WAKE_GROUP :
                                    ATBM_HERA_LMAC_WAKE_UNICAST)) {
            return true;
        }
    }
    return false;
}

static void atbm_hera_lmac_wake_now(void *opaque)
{
    AtbmHeraLmacState *s = opaque;

    if (s->ps & ATBM_HERA_LMAC_PS_SLEEP) {
        s->wake_count++;
        trace_atbm_hera_lmac_wake(s->wake_count);
        qemu_set_irq(s->wake, 1);
    }
}

static void atbm_hera_lmac_notify(AtbmCore *c)
{
    AtbmHeraLmacState *s = container_of(c, AtbmHeraLmacState, core);

    if (s->ps & ATBM_HERA_LMAC_PS_SLEEP) {
        qemu_set_irq(s->irq, 0);
        if (!s->waking && atbm_hera_lmac_wanted(s)) {
            s->waking = true;
            timer_mod(s->wake_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)s->wake_delay * SCALE_MS);
        }
        return;
    }
    qemu_set_irq(s->irq, (s->irq_en & 1) && atbm_core_peek(c));
}

static MemTxResult atbm_hera_lmac_dma(AtbmHeraLmacState *s, uint32_t addr,
                                      void *buf, size_t len, bool write)
{
    if (write) {
        return address_space_write(&address_space_memory, addr,
                                   MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    return address_space_read(&address_space_memory, addr,
                              MEMTXATTRS_UNSPECIFIED, buf, len);
}

/* The confirm the model queued for a request, taken off the queue */
static GByteArray *atbm_hera_lmac_take_cnf(AtbmHeraLmacState *s, uint16_t id)
{
    uint16_t cnf = (id & 0x03ff) | WSM_CNF;

    for (GList *l = s->core.out.head; l; l = l->next) {
        GByteArray *m = l->data;

        if (m->len >= 4 && (lduw_le_p(m->data + 2) & WSM_ID_MASK) == cnf) {
            g_queue_delete_link(&s->core.out, l);
            return m;
        }
    }
    return NULL;
}

static void atbm_hera_lmac_req(AtbmHeraLmacState *s, uint32_t addr)
{
    g_autofree uint8_t *msg = NULL;
    g_autoptr(GByteArray) cnf = NULL;
    uint8_t hdr[4];
    uint16_t len, id;

    s->req_result = 0;
    if (!(s->ctrl & ATBM_HERA_LMAC_CTRL_RUN) ||
        atbm_hera_lmac_dma(s, addr, hdr, sizeof(hdr), false) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: request at 0x%08x dropped\n",
                      __func__, addr);
        return;
    }
    len = lduw_le_p(&hdr[0]);
    id = lduw_le_p(&hdr[2]) & WSM_ID_MASK;
    if (len < 4 || len > REQ_MAX || len > s->req_size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: request 0x%04x length %u\n",
                      __func__, id, len);
        return;
    }
    msg = g_malloc(len);
    if (atbm_hera_lmac_dma(s, addr, msg, len, false) != MEMTX_OK) {
        return;
    }
    trace_atbm_hera_lmac_req(addr, id, len);
    atbm_core_wsm(&s->core, id, msg + 4, len - 4);

    cnf = atbm_hera_lmac_take_cnf(s, id);
    if (cnf) {
        if (cnf->len > s->req_size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: confirm 0x%04x of %u bytes, buffer %u\n",
                          __func__, id, cnf->len, s->req_size);
        } else {
            atbm_hera_lmac_dma(s, addr, cnf->data, cnf->len, true);
            s->req_result = ATBM_HERA_LMAC_RESULT_CNF | cnf->len << 16;
        }
        atbm_hera_lmac_notify(&s->core);
    }
}

static uint32_t atbm_hera_lmac_pop(AtbmHeraLmacState *s)
{
    g_autoptr(GByteArray) m = atbm_core_pop(&s->core);
    uint32_t len = 0;

    if (m) {
        if (m->len > s->ind_size) {
            s->ind_dropped++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: message 0x%04x of %u bytes, buffer %u\n",
                          __func__, lduw_le_p(m->data + 2), m->len,
                          s->ind_size);
        } else if (atbm_hera_lmac_dma(s, s->ind_addr, m->data, m->len,
                                      true) == MEMTX_OK) {
            len = m->len;
            trace_atbm_hera_lmac_ind(s->ind_addr, lduw_le_p(m->data + 2),
                                     len);
        }
    }
    atbm_hera_lmac_notify(&s->core);
    return len;
}

static void atbm_hera_lmac_set_ctrl(AtbmHeraLmacState *s, uint32_t val)
{
    bool was = s->ctrl & ATBM_HERA_LMAC_CTRL_RUN;

    s->ctrl = val & ATBM_HERA_LMAC_CTRL_RUN;
    if (was && !(s->ctrl & ATBM_HERA_LMAC_CTRL_RUN)) {
        atbm_core_hold(&s->core);
    } else if (!was && (s->ctrl & ATBM_HERA_LMAC_CTRL_RUN)) {
        atbm_core_hold(&s->core);
        atbm_core_run(&s->core);
    }
}

static void atbm_hera_lmac_set_ps(AtbmHeraLmacState *s, uint32_t val)
{
    s->ps = val & ATBM_HERA_LMAC_PS_SLEEP;
    trace_atbm_hera_lmac_ps(s->ps, g_queue_get_length(&s->core.out));
    if (!s->ps) {
        timer_del(s->wake_timer);
        s->waking = false;
        qemu_set_irq(s->wake, 0);
    }
    atbm_hera_lmac_notify(&s->core);
}

static uint64_t atbm_hera_lmac_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraLmacState *s = opaque;
    const uint8_t *mac = s->core.conf.macaddr.a;
    GByteArray *m;

    switch (addr) {
    case ATBM_HERA_LMAC_ID:
        return ATBM_HERA_LMAC_MAGIC;
    case ATBM_HERA_LMAC_VERSION:
        return ATBM_HERA_LMAC_ABI;
    case ATBM_HERA_LMAC_CTRL:
        return s->ctrl;
    case ATBM_HERA_LMAC_STATUS:
        m = atbm_core_peek(&s->core);
        return m ? ATBM_HERA_LMAC_STATUS_PEND | MIN(m->len, 0xffff) << 16 : 0;
    case ATBM_HERA_LMAC_IRQ_EN:
        return s->irq_en;
    case ATBM_HERA_LMAC_REQ_SIZE:
        return s->req_size;
    case ATBM_HERA_LMAC_REQ_RESULT:
        return s->req_result;
    case ATBM_HERA_LMAC_IND_ADDR:
        return s->ind_addr;
    case ATBM_HERA_LMAC_IND_SIZE:
        return s->ind_size;
    case ATBM_HERA_LMAC_IND_POP:
        return atbm_hera_lmac_pop(s);
    case ATBM_HERA_LMAC_IND_DROPPED:
        return s->ind_dropped;
    case ATBM_HERA_LMAC_MAC_LO:
        return ldl_le_p(mac);
    case ATBM_HERA_LMAC_MAC_HI:
        return lduw_le_p(mac + 4);
    case ATBM_HERA_LMAC_PS:
        return s->ps;
    case ATBM_HERA_LMAC_WAKE_MASK:
        return s->wake_mask;
    case ATBM_HERA_LMAC_WAKE_DELAY:
        return s->wake_delay;
    case ATBM_HERA_LMAC_WAKE_COUNT:
        return s->wake_count;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void atbm_hera_lmac_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    AtbmHeraLmacState *s = opaque;

    switch (addr) {
    case ATBM_HERA_LMAC_CTRL:
        atbm_hera_lmac_set_ctrl(s, val);
        return;
    case ATBM_HERA_LMAC_IRQ_EN:
        s->irq_en = val & 1;
        atbm_hera_lmac_notify(&s->core);
        return;
    case ATBM_HERA_LMAC_REQ_SIZE:
        s->req_size = val;
        return;
    case ATBM_HERA_LMAC_REQ:
        atbm_hera_lmac_req(s, val);
        return;
    case ATBM_HERA_LMAC_IND_ADDR:
        s->ind_addr = val;
        return;
    case ATBM_HERA_LMAC_IND_SIZE:
        s->ind_size = val;
        return;
    case ATBM_HERA_LMAC_PS:
        atbm_hera_lmac_set_ps(s, val);
        return;
    case ATBM_HERA_LMAC_WAKE_MASK:
        s->wake_mask = val & (ATBM_HERA_LMAC_WAKE_UNICAST |
                              ATBM_HERA_LMAC_WAKE_GROUP);
        return;
    case ATBM_HERA_LMAC_WAKE_DELAY:
        s->wake_delay = val;
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                  __func__, addr);
}

static const MemoryRegionOps atbm_hera_lmac_ops = {
    .read = atbm_hera_lmac_read,
    .write = atbm_hera_lmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void atbm_hera_lmac_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraLmacState *s = ATBM_HERA_LMAC(obj);

    s->ctrl = 0;
    s->irq_en = 0;
    s->req_size = 0;
    s->req_result = 0;
    s->ind_addr = 0;
    s->ind_size = 0;
    s->ind_dropped = 0;
    s->ps = 0;
    s->wake_mask = ATBM_HERA_LMAC_WAKE_UNICAST;
    s->wake_delay = 0;
    s->wake_count = 0;
    s->waking = false;
    timer_del(s->wake_timer);
    qemu_set_irq(s->wake, 0);
    atbm_core_hold(&s->core);
}

static void atbm_hera_lmac_init(Object *obj)
{
    AtbmHeraLmacState *s = ATBM_HERA_LMAC(obj);

    memory_region_init_io(&s->mmio, obj, &atbm_hera_lmac_ops, s,
                          TYPE_ATBM_HERA_LMAC, ATBM_HERA_LMAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->wake, "wake", 1);
    s->wake_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, atbm_hera_lmac_wake_now,
                                 s);
    s->core.dev = DEVICE(obj);
    s->core.notify = atbm_hera_lmac_notify;
    /* the Hera's PHY version, as the 6041's driver reads it */
    s->core.chip_id = 0x64;
    atbm_core_init(&s->core);
}

static void atbm_hera_lmac_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraLmacState *s = ATBM_HERA_LMAC(dev);

    atbm_core_realize(&s->core, errp);
}

static void atbm_hera_lmac_unrealize(DeviceState *dev)
{
    atbm_core_unrealize(&ATBM_HERA_LMAC(dev)->core);
}

static void atbm_hera_lmac_finalize(Object *obj)
{
    timer_free(ATBM_HERA_LMAC(obj)->wake_timer);
    atbm_core_finalize(&ATBM_HERA_LMAC(obj)->core);
}

static int atbm_hera_lmac_post_load(void *opaque, int version_id)
{
    AtbmHeraLmacState *s = opaque;

    atbm_hera_lmac_notify(&s->core);
    return 0;
}

static bool atbm_hera_lmac_ps_needed(void *opaque)
{
    AtbmHeraLmacState *s = opaque;

    return s->ps || s->wake_count || s->wake_delay ||
           s->wake_mask != ATBM_HERA_LMAC_WAKE_UNICAST;
}

static const VMStateDescription vmstate_atbm_hera_lmac_ps = {
    .name = TYPE_ATBM_HERA_LMAC "/ps",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = atbm_hera_lmac_ps_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ps, AtbmHeraLmacState),
        VMSTATE_UINT32(wake_mask, AtbmHeraLmacState),
        VMSTATE_UINT32(wake_delay, AtbmHeraLmacState),
        VMSTATE_UINT32(wake_count, AtbmHeraLmacState),
        VMSTATE_BOOL(waking, AtbmHeraLmacState),
        VMSTATE_TIMER_PTR(wake_timer, AtbmHeraLmacState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_atbm_hera_lmac = {
    .name = TYPE_ATBM_HERA_LMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = atbm_hera_lmac_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, AtbmHeraLmacState),
        VMSTATE_UINT32(irq_en, AtbmHeraLmacState),
        VMSTATE_UINT32(req_size, AtbmHeraLmacState),
        VMSTATE_UINT32(req_result, AtbmHeraLmacState),
        VMSTATE_UINT32(ind_addr, AtbmHeraLmacState),
        VMSTATE_UINT32(ind_size, AtbmHeraLmacState),
        VMSTATE_UINT32(ind_dropped, AtbmHeraLmacState),
        VMSTATE_ATBM_CORE(core, AtbmHeraLmacState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_atbm_hera_lmac_ps,
        NULL
    },
};

static const Property atbm_hera_lmac_properties[] = {
    DEFINE_ATBM_PROPERTIES(AtbmHeraLmacState, core),
};

static void atbm_hera_lmac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "ATBM6441 paravirtual LMAC port (WSM firmware model)";
    dc->realize = atbm_hera_lmac_realize;
    dc->unrealize = atbm_hera_lmac_unrealize;
    dc->vmsd = &vmstate_atbm_hera_lmac;
    rc->phases.hold = atbm_hera_lmac_reset_hold;
    device_class_set_props(dc, atbm_hera_lmac_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo atbm_hera_lmac_info = {
    .name = TYPE_ATBM_HERA_LMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraLmacState),
    .instance_init = atbm_hera_lmac_init,
    .instance_finalize = atbm_hera_lmac_finalize,
    .class_init = atbm_hera_lmac_class_init,
};

static void atbm_hera_lmac_register_types(void)
{
    type_register_static(&atbm_hera_lmac_info);
}

type_init(atbm_hera_lmac_register_types)
