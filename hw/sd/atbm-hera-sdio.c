/*
 * The SDIO card of an ATBM6441 "Hera": the host's face of its HIF
 *
 * Function 1 is the HIF's (hw/misc/atbm-hera-hif.c); every function 1
 * CMD52 and CMD53 is handed to it, and its interrupt to the host is the
 * card's function 1 interrupt. It is the "hif" link's device, in the same
 * machine; a machine that owns the SDIO host and not the chip would put
 * a link to the chip's machine in its place, with the same two calls.
 * The IDs are the ones the vendor's driver binds to (007a:6011, class 7).
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/atbm-hera-hif.h"
#include "hw/sd/sdio.h"

#define TYPE_ATBM_HERA_SDIO "atbm-hera-sdio"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraSdioState, ATBM_HERA_SDIO)

struct AtbmHeraSdioState {
    SDIOCard parent_obj;

    AtbmHeraHifState *hif;
};

static void sdio_host_irq(void *opaque, int n, int level)
{
    sdio_card_set_irq(SDIO_CARD(opaque), 1, level);
}

static void sdio_read(SDIOCard *card, unsigned fn, uint32_t addr, bool incr,
                      uint8_t *buf, size_t len)
{
    AtbmHeraSdioState *s = ATBM_HERA_SDIO(card);

    atbm_hera_hif_host_read(s->hif, addr, incr, buf, len);
}

static void sdio_write(SDIOCard *card, unsigned fn, uint32_t addr, bool incr,
                       const uint8_t *buf, size_t len)
{
    AtbmHeraSdioState *s = ATBM_HERA_SDIO(card);

    atbm_hera_hif_host_write(s->hif, addr, incr, buf, len);
}

static void sdio_f0_write(SDIOCard *card, uint32_t addr, uint8_t val)
{
    AtbmHeraSdioState *s = ATBM_HERA_SDIO(card);

    atbm_hera_hif_set_block_size(s->hif, card->blksz[1]);
}

static void sdio_io_reset(SDIOCard *card)
{
    AtbmHeraSdioState *s = ATBM_HERA_SDIO(card);

    /* the chip keeps running through a bus I/O reset */
    atbm_hera_hif_set_block_size(s->hif, 0);
}

static void sdio_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraSdioState *s = ATBM_HERA_SDIO(dev);

    if (!s->hif) {
        error_setg(errp, "the \"hif\" link must name the chip's HIF");
        return;
    }
    qdev_connect_gpio_out_named(DEVICE(s->hif), "host-irq", 0,
                                qdev_get_gpio_in_named(dev, "host-irq", 0));
    sdio_card_set_irq(SDIO_CARD(dev), 1, atbm_hera_hif_host_irq(s->hif));
}

static void sdio_init(Object *obj)
{
    qdev_init_gpio_in_named(DEVICE(obj), sdio_host_irq, "host-irq", 1);
}

static const Property sdio_props[] = {
    DEFINE_PROP_LINK("hif", AtbmHeraSdioState, hif, TYPE_ATBM_HERA_HIF,
                     AtbmHeraHifState *),
};

static void sdio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SDIOCardClass *sc = SDIO_CARD_CLASS(oc);

    dc->realize = sdio_realize;
    dc->desc = "AltoBeam ATBM6441 SDIO slave";
    device_class_set_props(dc, sdio_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);

    sc->vendor = 0x007a;
    sc->device = 0x6011;
    sc->func_class = 0x07;
    sc->num_funcs = 1;
    sc->max_blksz = 512;
    sc->read = sdio_read;
    sc->write = sdio_write;
    sc->f0_write = sdio_f0_write;
    sc->io_reset = sdio_io_reset;
}

static const TypeInfo sdio_info = {
    .name = TYPE_ATBM_HERA_SDIO,
    .parent = TYPE_SDIO_CARD,
    .instance_size = sizeof(AtbmHeraSdioState),
    .instance_init = sdio_init,
    .class_init = sdio_class_init,
};

static void sdio_register_types(void)
{
    type_register_static(&sdio_info);
}

type_init(sdio_register_types)
