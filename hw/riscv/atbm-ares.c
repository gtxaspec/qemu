/*
 * ATBM6461 ("Ares") Wi-Fi/IoT MCU
 *
 * An Andes AndeStar V5 RV32IMC core running the chip's own bootloader and
 * firmware from the user's 4 MiB SPI flash (-drive if=mtd,format=raw,file=
 * FLASH), which nothing here contains. The memory map, read from the
 * bootloader and from the firmware's boot log:
 *   0x00070000  ITCM, 64 KiB, the bootloader copies the firmware's part 1
 *               into it from the flash (0x410000)
 *   0x00100000  flash, cached window (the firmware's part 2 runs from it)
 *   0x00400000  flash, the same, uncached
 *   0x00800000  DTCM, 320 KiB (the RTOS heap and stacks)
 *   0x01400000  flash from 0x1000 up: where the bootloader is linked
 *   0x09000000  the bootloader's RAM; its RAM-resident code runs from here
 *   0x14000000  DMA, 0x16000000 and 0x17000000 the APB peripherals (the
 *               6441's models, as the IP is the same), 0x0a000000 the Wi-Fi
 *               MAC and PHY (stubs that read what was written, and log)
 *   0xe4000000  the PLIC, 0xe6400000 the software-interrupt PLIC
 * Address 0 faults, as on the chip (a PMP guard).
 *
 * The boot ROM is not available. A stand-in at the reset vector jumps into
 * the bootloader, which is all of the ROM's job a boot shows. The firmware
 * copies the BLE core's code and data to 0x20060000 and 0x20800000 (RAM
 * here); that second core, whose 0x60000 is that ILM, is not modelled.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/loader.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/block/atbm-hera-nor.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/unimp.h"
#include "hw/ssi/ssi.h"
#include "system/block-backend.h"
#include "hw/core/or-irq.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "hw/intc/sifive_plic.h"
#include "hw/riscv/riscv_hart.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/riscv/cpu.h"

#define TYPE_ATBM6461_MACHINE MACHINE_TYPE_NAME("atbm6461")
OBJECT_DECLARE_SIMPLE_TYPE(Atbm6461State, ATBM6461_MACHINE)

#define ROM_BASE        0xff000000
#define ROM_SIZE        0x2000
#define FLASH_SIZE      (4 * MiB)
#define FLASH_NOCACHE   0x00400000      /* the window the bootloader runs in */
#define FLASH_CACHED    0x00100000
/* flash 0x1000 up, where the bootloader is linked */
#define FLASH_IMAGE_WIN 0x01400000
#define BOOT_ENTRY      FLASH_IMAGE_WIN
#define ITCM_BASE       0x00070000
#define ITCM_SIZE       (64 * KiB)
#define DTCM_BASE       0x00800000
#define DTCM_SIZE       (320 * KiB)
/* the bootloader's data, stack and the code it runs from RAM */
#define SRAM_BASE       0x09000000
#define SRAM_SIZE       (256 * KiB)
#define PLIC_BASE       0xe4000000
/* machine software interrupts, the RTOS's yield */
#define PLICSW_BASE     0xe6400000
#define PERIPH_BASE     0x10000000
#define PERIPH_SIZE     0x10000000
#define MAC_BASE        0x0a000000
#define MAC_SIZE        0x01000000

typedef struct PeriphWin {
    Atbm6461State *s;
    hwaddr base;
} PeriphWin;

struct Atbm6461State {
    MachineState parent_obj;

    RISCVHartArrayState cpus;
    MemoryRegion itcm, dtcm, sram, mac_a, mac_b, mac_c, rom, flash;
    MemoryRegion flash_nocache, flash_cached, flash_image;
    MemoryRegion periph, mac, sys1;
    PeriphWin pwin[2];
    GHashTable *regs;
    AtbmHeraNorState *nor;
    DeviceState *smu;
    int64_t cnt_start;
    MemoryRegion plicsw;
    MemoryRegion reboot;
    MemoryRegion *pads;
    qemu_irq plicsw_irq, yield_irq, *uart_irq;
    uint32_t uart_lvl;
    uint32_t held;
    qemu_irq *ctl;
    DeviceState *uart[3];
    uint32_t plicsw_pend, plicsw_en;
    DeviceState *plic;
};

/*
 * Every peripheral register not modelled yet: reads return what was last
 * written (else 0), and each access is logged, so what the firmware waits
 * on shows in -d unimp.
 */
/*
 * Bits of the unmodelled registers the firmware polls, forced on: what the
 * MAC does to set them is not known, so they read as set
 */
static const struct {
    hwaddr addr;
    uint32_t set;
} mac_ready[] = {
    { 0x0a500a20, 0x80000000 },     /* the LMAC init waits for bit 31 */
    { 0x0ac3899c, 0x1 },            /* the TX calibration's done flag */
    { 0x0acc017c, 0xb1 },           /* the chip version, 6461 */
};

/* and bits it waits to see clear */
static const struct {
    hwaddr addr;
    uint32_t clr;
} mac_clear[] = {
    { 0x0a560884, 0x1 },            /* PAS_FlushEbmTxRequest's WaitClr */
    { 0x0a500a48, 0x80 },           /* WaitImStop */
};

static uint64_t periph_read(void *opaque, hwaddr addr, unsigned size)
{
    PeriphWin *w = opaque;
    uint32_t v = GPOINTER_TO_UINT(g_hash_table_lookup(w->s->regs,
                                  GUINT_TO_POINTER(w->base + addr)));
    int i;

    for (i = 0; i < ARRAY_SIZE(mac_ready); i++) {
        if (mac_ready[i].addr == w->base + (addr & ~3)) {
            v |= mac_ready[i].set;
        }
    }
    for (i = 0; i < ARRAY_SIZE(mac_clear); i++) {
        if (mac_clear[i].addr == w->base + (addr & ~3)) {
            v &= ~mac_clear[i].clr;
        }
    }
    qemu_log_mask(LOG_UNIMP, "atbm6461: read  0x%08" HWADDR_PRIx " -> 0x%x\n",
                  w->base + addr, v);
    return v;
}

static void periph_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    PeriphWin *w = opaque;

    /*
     * The EBM queues' pointer registers (+0x10 to +0x2f of each block of
     * 0x200 from 0x0a560000, as far as 0x0a561fff) keep an offset
     * into the descriptor memory at 0x18100000, though the firmware writes
     * the address: its check adds the base to what it reads back. An empty
     * queue then has head == tail, a zero next field and its +0x20 clear,
     * and passes.
     */
    if (w->base + addr >= 0x0a560000 && w->base + addr < 0x0a562000 &&
        (addr & 0x1ff) >= 0x10 && (addr & 0x1ff) < 0x30) {
        val &= 0xfffff;
    }

    qemu_log_mask(LOG_UNIMP, "atbm6461: write 0x%08" HWADDR_PRIx " <- 0x%"
                  PRIx64 "\n", w->base + addr, val);
    g_hash_table_insert(w->s->regs, GUINT_TO_POINTER(w->base + addr),
                        GUINT_TO_POINTER((uint32_t)val));
}

static const MemoryRegionOps periph_ops = {
    .read = periph_read,
    .write = periph_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * The clock and power block, 0x16100000, is the 6441's SMU model with the
 * status bits the bootloader polls forced on: what sets them on silicon is
 * not known (PLL lock and the like), so they read as set.
 */
static const struct {
    hwaddr off;
    uint32_t set;
} sys_ready[] = {
    { 0x188, 0x2 },     /* polled until bit 1 after bit 31 is set */
    { 0x098, 0x2 },     /* the second calibration's done bit */
    { 0x094, 0x3070 },  /* the sleep clock calibration's result, as logged */
};

static MemoryRegion *smu_mr(Atbm6461State *s)
{
    return sysbus_mmio_get_region(SYS_BUS_DEVICE(s->smu), 0);
}

static uint64_t sys_read(void *opaque, hwaddr addr, unsigned size)
{
    Atbm6461State *s = opaque;
    uint64_t v = 0;
    int i;

    memory_region_dispatch_read(smu_mr(s), addr, &v, size_memop(size) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    if ((addr & ~3) == 0x1e0) {
        /*
         * UART1, UART2 and UART3: a received byte with its interrupt on.
         * The firmware's handler is for those, and runs before its UART
         * structures are set up if a transmit interrupt left on by the
         * bootloader shows here
         */
        static const int bit[3] = { 6, 7, 8 };
        int u;

        for (u = 0; u < 3; u++) {
            uint64_t ier = 0, lsr = 0;
            MemoryRegion *mr = sysbus_mmio_get_region(
                SYS_BUS_DEVICE(s->uart[u]), 0);

            memory_region_dispatch_read(mr, 0x24, &ier, MO_8,
                                        MEMTXATTRS_UNSPECIFIED);
            memory_region_dispatch_read(mr, 0x34, &lsr, MO_8,
                                        MEMTXATTRS_UNSPECIFIED);
            if ((ier & 1) && (lsr & 1)) {
                        v |= 1u << bit[u];
            }
        }
    }
    if ((addr & ~3) == 0x1100) {
        /* 10 us a tick from when 0x10f4 bit 0 is set (wdt_calib's start) */
        return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->cnt_start,
                        100000, NANOSECONDS_PER_SECOND);
    }
    for (i = 0; i < ARRAY_SIZE(sys_ready); i++) {
        if (sys_ready[i].off == (addr & ~3)) {
            v |= sys_ready[i].set;
        }
    }
    return v;
}

static void sys_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Atbm6461State *s = opaque;

    if ((addr & ~3) == 0xc) {
        /* bit 0 asks for the RTOS's yield, interrupt 27, until it is cleared */
        qemu_set_irq(s->yield_irq, val & 1);
    }
    if ((addr & ~3) == 0x10f4 && (val & 1)) {
        s->cnt_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    memory_region_dispatch_write(smu_mr(s), addr, val,
                                 size_memop(size) | MO_LE,
                                 MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps sys_ops = {
    .read = sys_read,
    .write = sys_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* lui t0, 0x1400; jr t0: into the bootloader as the flash maps it */
static void ares_load_rom(Atbm6461State *s)
{
    static const uint32_t stub[] = {
        0x014002b7,     /* lui   t0, 0x1400 */
        0x00028067,     /* jr    t0 */
    };

    memory_region_init_rom(&s->rom, NULL, "atbm6461.rom", ROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), ROM_BASE, &s->rom);
    rom_add_blob_fixed("atbm6461.rom-stub", stub, sizeof(stub), ROM_BASE);
    assert(BOOT_ENTRY == 0x01400000);
}

static void ares_connect(Atbm6461State *s, DeviceState *dev, int n, int irq)
{
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), n,
                       qdev_get_gpio_in(s->plic, irq));
}

static DeviceState *ares_dev(Atbm6461State *s, const char *type,
                             const char *name, hwaddr base, int irq,
                             Chardev *chr)
{
    DeviceState *dev = qdev_new(type);

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    if (chr) {
        qdev_prop_set_chr(dev, "chardev", chr);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    if (irq >= 0) {
        ares_connect(s, dev, 0, irq);
    }
    return dev;
}

/*
 * The software-interrupt PLIC (Andes' PLIC_SW), as the RTOS uses it to yield:
 * source 1 is this hart's. A write to the pending word raises it, a read of
 * the claim register returns and clears it, a write of the same to complete
 * is dropped; with it enabled, the line is the hart's machine software
 * interrupt.
 */
static void plicsw_update(Atbm6461State *s)
{
    qemu_set_irq(s->plicsw_irq, !!(s->plicsw_pend & s->plicsw_en));
}

static uint64_t plicsw_read(void *opaque, hwaddr addr, unsigned size)
{
    Atbm6461State *s = opaque;
    uint32_t v = 0;

    qemu_log_mask(LOG_UNIMP, "atbm6461: plicsw read 0x%" HWADDR_PRIx
                  " (pend 0x%x en 0x%x)\n", addr, s->plicsw_pend,
                  s->plicsw_en);
    switch (addr) {
    case 0x1000:
        return s->plicsw_pend;
    case 0x2000:
        return s->plicsw_en;
    case 0x200004:
        v = s->plicsw_pend & s->plicsw_en ? 1 : 0;
        s->plicsw_pend &= ~(v << 1);
        plicsw_update(s);
        return v;
    default:
        return 0;
    }
}

static void plicsw_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    Atbm6461State *s = opaque;

    qemu_log_mask(LOG_UNIMP, "atbm6461: plicsw write 0x%" HWADDR_PRIx
                  " <- 0x%" PRIx64 "\n", addr, val);
    switch (addr) {
    case 0x1000:
        s->plicsw_pend |= val;
        break;
    case 0x2000:
        s->plicsw_en = val;
        break;
    default:
        break;
    }
    plicsw_update(s);
}

static const MemoryRegionOps plicsw_ops = {
    .read = plicsw_read,
    .write = plicsw_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * The firmware's Hardware_Reboot() (the stock image's system_reboot() ends
 * in it) prints, then sets the pad function of pins 24 and 25 to 8 with one
 * write of 0x26082608 to 0x17400034 and spins. A guess from that code and
 * its name: those pads drive the chip's own reset, so the chip restarts.
 * Boot never writes this function there. The dev's clean image stubs the
 * call out, and its chip then sits in the trap handler.
 */
#define PADS_BASE         0x17400000
#define PADS24_25         (PADS_BASE + 0x34)
#define PADS24_25_RESET   0x00080008
#define PADS24_25_MASK    0x000f000f

static void pads_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    Atbm6461State *s = opaque;

    memory_region_dispatch_write(s->pads, addr, val, MO_32,
                                 MEMTXATTRS_UNSPECIFIED);
    if ((val & PADS24_25_MASK) == PADS24_25_RESET) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t pads_read(void *opaque, hwaddr addr, unsigned size)
{
    Atbm6461State *s = opaque;
    uint64_t val = 0;

    memory_region_dispatch_read(s->pads, addr, &val, MO_32,
                                MEMTXATTRS_UNSPECIFIED);
    return val;
}

static const MemoryRegionOps pads_ops = {
    .read = pads_read,
    .write = pads_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Lines into the core: 0 the SDIO host's hold of the CPU in reset (HIF
 * CONFIG[14]) and 3 its hold through the SMU's CPU_CTRL, 1 the camera SoC's
 * hold (the chip off, or its reset pin, from the link), 2 the power-up after
 * that, a reset of every device as a power cycle does. A hold stops the core
 * and a release restarts it from the boot ROM's stand-in.
 */
static void ares_ctl(void *opaque, int n, int level)
{
    Atbm6461State *s = opaque;
    uint32_t was = s->held;

    if (n == 2) {
        if (level) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        return;
    }
    s->held = deposit32(s->held, n, 1, !!level);
    if (!was && s->held) {
        cpu_pause(CPU(&s->cpus.harts[0]));
        cpu_reset(CPU(&s->cpus.harts[0]));
    } else if (was && !s->held) {
        cpu_reset(CPU(&s->cpus.harts[0]));
        cpu_resume(CPU(&s->cpus.harts[0]));
    }
}

static void ares_uart_irq(void *opaque, int n, int level)
{
    Atbm6461State *s = opaque;

    s->uart_lvl = deposit32(s->uart_lvl, n, 1, level);
    qemu_set_irq(qdev_get_gpio_in(s->plic, 17), !!s->uart_lvl);
}

/*
 * The SDIO slave (the 6441's HIF block, at the same address) and the link
 * to a camera SoC's QEMU (-global atbm-hera-link.chardev=<id>; without a
 * chardev the link does nothing). The HIF's interrupt to the host and the
 * GPIO block's pads go through it, and it holds and resets the chip.
 */
static void ares_link(Atbm6461State *s, DeviceState *gpio)
{
    DeviceState *hif, *link = qdev_new("atbm-hera-link");
    int i;

    s->ctl = qemu_allocate_irqs(ares_ctl, s, 4);
    /* the 6461's message buffers are named by word offsets */
    hif = qdev_new("atbm-hera-hif");
    qdev_prop_set_uint32(hif, "offset-shift", 2);
    object_property_add_child(OBJECT(s), "hif", OBJECT(hif));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(hif), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(hif), 0, 0x0ab00000);
    ares_connect(s, hif, 0, 10);
    qdev_connect_gpio_out_named(hif, "cpu-reset", 0, s->ctl[0]);
    object_property_add_child(OBJECT(s), "link", OBJECT(link));
    object_property_set_link(OBJECT(link), "hif", OBJECT(hif), &error_abort);
    qdev_realize_and_unref(link, NULL, &error_fatal);
    qdev_connect_gpio_out_named(hif, "host-irq", 0,
                                qdev_get_gpio_in_named(link, "host-irq", 0));
    for (i = 0; i < 28; i++) {
        qdev_connect_gpio_out_named(gpio, "out", i,
                                    qdev_get_gpio_in_named(link, "pad-out", i));
        qdev_connect_gpio_out_named(link, "pad-in", i,
                                    qdev_get_gpio_in_named(gpio, "pin", i));
    }
    qdev_connect_gpio_out_named(link, "hold", 0, s->ctl[1]);
    qdev_connect_gpio_out_named(link, "reset", 0, s->ctl[2]);
    /* the host also holds the core through the SMU's CPU_CTRL, by a window */
    qdev_connect_gpio_out_named(s->smu, "cpu-hold", 0, s->ctl[3]);
}

static void ares_init(MachineState *machine)
{
    Atbm6461State *s = ATBM6461_MACHINE(machine);
    MemoryRegion *sys = get_system_memory();
    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
    DeviceState *spi1, *dmac, *dev, *pit_or, *dreq_or;

    object_initialize_child(OBJECT(machine), "cpus", &s->cpus,
                            TYPE_RISCV_HART_ARRAY);
    object_property_set_str(OBJECT(&s->cpus), "cpu-type",
                            machine->cpu_type, &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "num-harts", 1, &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "resetvec", ROM_BASE,
                            &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->cpus), &error_fatal);

    memory_region_init_ram(&s->itcm, NULL, "atbm6461.itcm",
                           ITCM_SIZE, &error_fatal);
    memory_region_add_subregion(sys, ITCM_BASE, &s->itcm);
    memory_region_init_ram(&s->dtcm, NULL, "atbm6461.dtcm",
                           DTCM_SIZE, &error_fatal);
    memory_region_add_subregion(sys, DTCM_BASE, &s->dtcm);
    /*
     * The LMAC's code and data memories: the firmware copies images from
     * its flash into 0x20060000 (96 KiB) and 0x20800000 (32 KiB); the real
     * sizes of the windows are not known
     */
    memory_region_init_ram(&s->mac_a, NULL, "atbm6461.mac-ram-a", 1 * MiB,
                           &error_fatal);
    memory_region_add_subregion(sys, 0x20000000, &s->mac_a);
    memory_region_init_ram(&s->mac_b, NULL, "atbm6461.mac-ram-b", 1 * MiB,
                           &error_fatal);
    memory_region_add_subregion(sys, 0x20800000, &s->mac_b);
    /* the MAC's descriptor memory: the EBM queue entries live here */
    memory_region_init_ram(&s->mac_c, NULL, "atbm6461.mac-ram-c", 1 * MiB,
                           &error_fatal);
    memory_region_add_subregion(sys, 0x18100000, &s->mac_c);
    memory_region_init_ram(&s->sram, NULL, "atbm6461.sram", SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sys, SRAM_BASE, &s->sram);

    memory_region_init_io(&s->periph, NULL, &periph_ops, &s->pwin[0],
                          "atbm6461.periph", PERIPH_SIZE);
    memory_region_add_subregion_overlap(sys, PERIPH_BASE, &s->periph, -1);
    memory_region_init_io(&s->mac, NULL, &periph_ops, &s->pwin[1],
                          "atbm6461.mac", MAC_SIZE);
    memory_region_add_subregion_overlap(sys, MAC_BASE, &s->mac, -1);
    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->pwin[0] = (PeriphWin){ s, PERIPH_BASE };
    s->pwin[1] = (PeriphWin){ s, MAC_BASE };

    s->plic = sifive_plic_create(sys, PLIC_BASE, (char *)"M", 1, 0, 64, 7,
                                 0x0, 0x1000, 0x2000, 0x80, 0x200000, 0x1000,
                                 0x400000);

    /*
     * The flash, 4 MiB, behind SPI1 (the controller the bootloader reads
     * it with). Its array is what the XIP windows show, read-only.
     */
    spi1 = ares_dev(s, "atbm-hera-spi", "spi1", 0x16a00000, -1, NULL);
    s->nor = ATBM_HERA_NOR(qdev_new(TYPE_ATBM_HERA_NOR));
    object_property_add_child(OBJECT(machine), "flash", OBJECT(s->nor));
    if (dinfo) {
        qdev_prop_set_drive_err(DEVICE(s->nor), "drive",
                                blk_by_legacy_dinfo(dinfo), &error_fatal);
    }
    qdev_prop_set_uint32(DEVICE(s->nor), "size", FLASH_SIZE);
    /* the bootloader reads status 0x202: the part has QE set */
    qdev_prop_set_uint8(DEVICE(s->nor), "status2", 0x02);
    qdev_realize_and_unref(DEVICE(s->nor), qdev_get_child_bus(spi1, "ssi"),
                           &error_fatal);
    qdev_connect_gpio_out_named(spi1, "cs", 0,
                                qdev_get_gpio_in_named(DEVICE(s->nor),
                                                       SSI_GPIO_CS, 0));
    memory_region_init_alias(&s->flash_nocache, NULL, "atbm6461.flash-nc",
                             &s->nor->array, 0, FLASH_SIZE);
    memory_region_set_readonly(&s->flash_nocache, true);
    memory_region_add_subregion(sys, FLASH_NOCACHE, &s->flash_nocache);
    memory_region_init_alias(&s->flash_cached, NULL, "atbm6461.flash-c",
                             &s->nor->array, 0,
                             FLASH_NOCACHE - FLASH_CACHED);
    memory_region_set_readonly(&s->flash_cached, true);
    memory_region_add_subregion(sys, FLASH_CACHED, &s->flash_cached);
    memory_region_init_alias(&s->flash_image, NULL, "atbm6461.flash-image",
                             &s->nor->array, 0x1000, FLASH_SIZE - 0x1000);
    memory_region_set_readonly(&s->flash_image, true);
    memory_region_add_subregion(sys, FLASH_IMAGE_WIN, &s->flash_image);

    /*
     * The UARTs' interrupts go through the clock and power block: its
     * status at 0x1e0 has them at bits 6 (UART1), 7 (UART2) and 8 (UART3),
     * and the firmware's handler for interrupt 17 reads that; the
     * bootloader's handler for 17 polls UART2 itself
     */
    s->uart_irq = qemu_allocate_irqs(ares_uart_irq, s, 3);
    dev = ares_dev(s, "atbm-hera-uart", "uart1", 0x16200000, -1, serial_hd(0));
    s->uart[0] = dev;
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s->uart_irq[0]);
    dev = ares_dev(s, "atbm-hera-uart", "uart2", 0x16300000, -1, serial_hd(1));
    s->uart[1] = dev;
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s->uart_irq[1]);
    dev = ares_dev(s, "atbm-hera-uart", "uart3", 0x17600000, -1, serial_hd(2));
    s->uart[2] = dev;
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s->uart_irq[2]);
    /* one handler, number 18, polls all three PITs */
    pit_or = qdev_new(TYPE_OR_IRQ);
    object_property_add_child(OBJECT(machine), "pit-or", OBJECT(pit_or));
    qdev_prop_set_uint16(pit_or, "num-lines", 3);
    qdev_realize_and_unref(pit_or, NULL, &error_fatal);
    qdev_connect_gpio_out(pit_or, 0, qdev_get_gpio_in(s->plic, 18));
    dev = ares_dev(s, "atbm-hera-pit", "pit1", 0x16400000, -1, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(pit_or, 0));
    dev = ares_dev(s, "atbm-hera-pit", "pit2", 0x16500000, -1, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(pit_or, 1));
    dev = ares_dev(s, "atbm-hera-pit", "pit3", 0x17000000, -1, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(pit_or, 2));
    ares_dev(s, "atbm-hera-efuse", "efuse", 0x16b00000, -1, NULL);
    dmac = ares_dev(s, "atbm-hera-dmac", "dmac", 0x14000000, 7, NULL);
    /*
     * The bootloader and the firmware both set request number 0 for the
     * flash SPI, as the source of a read and as the destination of a
     * write (the 6441's data sheet has TX 6 and RX 7): the two lines
     * are one request here
     */
    dreq_or = qdev_new(TYPE_OR_IRQ);
    object_property_add_child(OBJECT(machine), "spi1-dreq", OBJECT(dreq_or));
    qdev_prop_set_uint16(dreq_or, "num-lines", 2);
    qdev_realize_and_unref(dreq_or, NULL, &error_fatal);
    qdev_connect_gpio_out(dreq_or, 0, qdev_get_gpio_in_named(dmac, "req", 0));
    qdev_connect_gpio_out_named(spi1, "dreq-rx", 0,
                                qdev_get_gpio_in(dreq_or, 0));
    qdev_connect_gpio_out_named(spi1, "dreq-tx", 0,
                                qdev_get_gpio_in(dreq_or, 1));
    dev = ares_dev(s, "atbm-hera-smu", "smu", 0x16100000, 28, NULL);
    s->smu = dev;
    s->yield_irq = qdev_get_gpio_in(s->plic, 27);
    memory_region_init_io(&s->sys1, NULL, &sys_ops, s, "atbm6461.sys", 0x2000);
    memory_region_add_subregion_overlap(sys, 0x16100000, &s->sys1, 1);
    ares_connect(s, dev, 1, 30);
    dev = ares_dev(s, "atbm-hera-gpio", "gpio", 0x16800000, 23, NULL);
    ares_link(s, dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, PADS_BASE);
    s->pads = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_io(&s->reboot, NULL, &pads_ops, s, "atbm6461.pads24",
                          4);
    memory_region_add_subregion_overlap(sys, PADS24_25, &s->reboot, 1);
    dev = ares_dev(s, "atbm-hera-rtc", "rtc1", 0x16700000, 25, NULL);
    ares_connect(s, dev, 1, 26);
    ares_dev(s, "atbm-hera-wdt", "wdt1", 0x16600000, -1, NULL);
    ares_dev(s, "atbm-hera-wdt", "wdt2", 0x17500000, -1, NULL);

    memory_region_init_io(&s->plicsw, NULL, &plicsw_ops, s, "atbm6461.plicsw",
                          0x400000);
    memory_region_add_subregion(sys, PLICSW_BASE, &s->plicsw);
    s->plicsw_irq = qdev_get_gpio_in(DEVICE(qemu_get_cpu(0)), IRQ_M_SOFT);
    ares_load_rom(s);
}

static void ares_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        TYPE_RISCV_CPU_ANDES_ARES,
        NULL
    };

    mc->desc = "ATBM6461 Wi-Fi/IoT MCU (Andes V5), with the user's flash";
    mc->init = ares_init;
    mc->default_cpu_type = TYPE_RISCV_CPU_ANDES_ARES;
    mc->valid_cpu_types = valid_cpu_types;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
    mc->default_ram_id = "atbm6461.ram";
}

static const TypeInfo ares_info = {
    .name = TYPE_ATBM6461_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Atbm6461State),
    .class_init = ares_class_init,
};

static void ares_register(void)
{
    type_register_static(&ares_info);
}
type_init(ares_register)
