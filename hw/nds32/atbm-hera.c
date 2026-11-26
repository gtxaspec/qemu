/*
 * ATBM6441 "Hera" Wi-Fi/IoT MCU board
 *
 * The chip's own CPU is an Andes NDS32 core: its interrupt controller's
 * inputs "IRQ" (0..31) get the chip's interrupt numbers (dev_int.h) and
 * its "NMI" the watchdogs', unless -machine wire-cpu=off. A CPU without
 * those inputs (the qtests' address-space CPU of another target) is left
 * unwired.
 *
 * Memory (from the SDK's linker scripts and config headers):
 *   0x00030000 ILM, 64 KiB     code the bootloader copies from flash
 *   0x00060000 boot ROM        not in the SDK: empty, or -bios
 *   0x00080000 flash, cached   XIP window, up to 0x380000 of it
 *   0x00400000 flash, uncached the same flash
 *   0x00800000 DTCM, 256 KiB
 *   0x09000000 SRAM            shared with the Wi-Fi MAC; 256 KiB here
 *   0x0a000000 .. 0x0adfffff   Wi-Fi MAC, host interface and PHY
 *   0x14000000 DMA, 0x16000000 and 0x17000000 the APB peripherals
 *
 * -kernel takes the SDK's ELF: a segment whose address is in a RAM is
 * loaded there, one in the flash window goes into the flash, and every
 * segment's load address plus 0x10000 (the boot loader's 64 KiB in front
 * of ap.bin) is its place in the flash image, which the firmware's startup
 * code copies its data from, including the zeros of .bss (the SDK's image
 * tool stores them, and __c_init copies them from flash: a segment's
 * memory size, not its file size, is what lands in the flash).
 *
 * Boot from the flash: with a flash image and no -kernel the machine runs
 * what the boot ROM does (the ROM is not in the SDK, so this is inferred
 * from the SDK's flash layout in IOT_spec and from the bootloader files):
 * the ROM reads the 36 byte boot_config at flash 0, requires config_valid
 * 0xab45, and jumps to config_boot1_addr (0x401000: the bootloader at flash
 * 0x1000 through the uncached XIP window), which then copies itself and the
 * firmware to RAM. config_checksum (0x12341234 plus the byte sum of the
 * eight words before it: true for all eight boot_harechip files) is checked
 * and only reported: whether the ROM refuses a mismatch is not known. A
 * ROM that refuses a header would enter its UART download mode: that is not
 * modelled, the core stays stopped. The bootloader's code is linked at
 * 0x01400000, a window on the flash from 0x1000 (its vector base and its
 * data/rodata references; the 6461's bootloader is the same).
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/core/boards.h"
#include "target/nds32/cpu.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/block/atbm-hera-nor.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/block-backend.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"

#define TYPE_ATBM_HERA_MACHINE MACHINE_TYPE_NAME("atbm-hera")
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraMachine, ATBM_HERA_MACHINE)

#define TYPE_ATBM_HERA_INTSINK "atbm-hera-intsink"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraIntSink, ATBM_HERA_INTSINK)

#define ILM_BASE        0x00030000
#define ILM_SIZE        (64 * KiB)
#define ROM_BASE        0x00060000
#define ROM_SIZE        (64 * KiB)
#define XIP_BASE        0x00080000
#define XIP_SIZE        0x00380000
#define XIPNC_BASE      0x00400000
#define DTCM_BASE       0x00800000
#define DTCM_SIZE       (256 * KiB)
#define SRAM_BASE       0x09000000
#define SRAM_SIZE       (256 * KiB)
#define FLASH_BOOT_LEN  0x10000
#define BOOTWIN_BASE    0x01400000
#define BOOTWIN_OFFSET  0x1000

/* boot_config, flash 0: IOT_spec "boot_config" */
#define BOOTCFG_VALID       0xab45
#define BOOTCFG_WORDS       8
#define BOOTCFG_CKSUM_SEED  0x12341234

/*
 * A chip interrupt number is a CPU interrupt input (dev_int.h); the
 * watchdogs' interrupt is the CPU's NMI, one more sink input
 */
#define N_INTS          32
#define NMI_IN          N_INTS

/*
 * The place where the sources meet: the CPU's inputs are its outputs,
 * and each input's level is kept to look at.
 */
struct AtbmHeraIntSink {
    DeviceState parent_obj;

    qemu_irq out[N_INTS];
    qemu_irq nmi;
    uint32_t levels;
};

static void intsink_set(void *opaque, int n, int level)
{
    AtbmHeraIntSink *s = opaque;

    if (n == NMI_IN) {
        qemu_set_irq(s->nmi, level);
        return;
    }
    s->levels = deposit32(s->levels, n, 1, !!level);
    qemu_set_irq(s->out[n], level);
}

static void intsink_init(Object *obj)
{
    AtbmHeraIntSink *s = ATBM_HERA_INTSINK(obj);

    qdev_init_gpio_in(DEVICE(obj), intsink_set, N_INTS + 1);
    qdev_init_gpio_out_named(DEVICE(obj), s->out, "int", N_INTS);
    qdev_init_gpio_out_named(DEVICE(obj), &s->nmi, "nmi", 1);
}

static void intsink_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->user_creatable = false;
}

static const TypeInfo intsink_info = {
    .name = TYPE_ATBM_HERA_INTSINK,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(AtbmHeraIntSink),
    .instance_init = intsink_init,
    .class_init = intsink_class_init,
};

struct AtbmHeraMachine {
    MachineState parent_obj;

    MemoryRegion ilm;
    MemoryRegion rom;
    MemoryRegion dtcm;
    MemoryRegion sram;
    CPUState *cpu;
    MemoryRegion flash_cached;
    MemoryRegion flash_uncached;
    MemoryRegion flash_bootwin;
    AtbmHeraIntSink *ints;
    /* the interrupt numbers that two sources share, through an OR */
    DeviceState *pit_or;
    DeviceState *nmi_or;
    AtbmHeraNorState *nor;
    uint32_t entry;
    uint32_t held;
    qemu_irq *ctl;
    uint64_t hif_port;
    bool wire_cpu;
    bool flash_boot;
    bool rom_stopped;
};

static void atbm_hera_connect(AtbmHeraMachine *m, DeviceState *dev, int n,
                              int irq)
{
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), n,
                       qdev_get_gpio_in(DEVICE(m->ints), irq));
}

static DeviceState *atbm_hera_or(AtbmHeraMachine *m, const char *name,
                                 int n, int irq)
{
    DeviceState *or = qdev_new(TYPE_OR_IRQ);

    object_property_add_child(OBJECT(m), name, OBJECT(or));
    qdev_prop_set_uint16(or, "num-lines", n);
    qdev_realize_and_unref(or, NULL, &error_fatal);
    qdev_connect_gpio_out(or, 0, qdev_get_gpio_in(DEVICE(m->ints), irq));
    return or;
}

static DeviceState *atbm_hera_dev(AtbmHeraMachine *m, const char *type,
                                  const char *name, hwaddr base, int irq,
                                  Chardev *chr)
{
    DeviceState *dev = qdev_new(type);

    object_property_add_child(OBJECT(m), name, OBJECT(dev));
    if (chr) {
        qdev_prop_set_chr(dev, "chardev", chr);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    if (irq >= 0) {
        atbm_hera_connect(m, dev, 0, irq);
    }
    return dev;
}

/* Where a segment lands in the flash image, or -1 */
static bool atbm_hera_in(uint32_t addr, uint32_t len, uint32_t base,
                         uint32_t size)
{
    return addr >= base && (uint64_t)addr + len <= (uint64_t)base + size;
}

static void atbm_hera_load_elf(AtbmHeraMachine *m, const char *filename)
{
    g_autofree char *data = NULL;
    gsize len;
    Elf32_Ehdr *eh;
    Elf32_Phdr *ph;
    uint64_t flash_size = m->nor->size;

    if (!g_file_get_contents(filename, &data, &len, NULL) ||
        len < sizeof(*eh)) {
        error_report("atbm-hera: cannot read '%s'", filename);
        exit(1);
    }
    eh = (Elf32_Ehdr *)data;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(*ph) > len) {
        error_report("atbm-hera: '%s' is not a 32-bit little-endian ELF",
                     filename);
        exit(1);
    }
    m->entry = eh->e_entry;
    ph = (Elf32_Phdr *)(data + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        uint32_t va = ph[i].p_vaddr, pa = ph[i].p_paddr;
        uint32_t fl = ph[i].p_filesz, n = ph[i].p_memsz;
        g_autofree char *name = g_strdup_printf("atbm-hera.seg%d", i);
        /* the image of a segment is its file bytes, then zeros up to memsz */
        g_autofree uint8_t *img = NULL;

        if (ph[i].p_type != PT_LOAD || !n || fl > n ||
            (uint64_t)ph[i].p_offset + fl > len) {
            continue;
        }
        img = g_malloc0(n);
        memcpy(img, data + ph[i].p_offset, fl);
        if (atbm_hera_in(va, n, ILM_BASE, ILM_SIZE) ||
            atbm_hera_in(va, n, DTCM_BASE, DTCM_SIZE) ||
            atbm_hera_in(va, n, SRAM_BASE, SRAM_SIZE)) {
            rom_add_blob_fixed(name, img, n, va);
        } else if (atbm_hera_in(va, n, XIP_BASE, XIP_SIZE) &&
                   va - XIP_BASE + n <= flash_size) {
            rom_add_blob_fixed(name, img, n, va);
            continue;
        } else if (atbm_hera_in(va, n, XIPNC_BASE, XIP_SIZE) &&
                   va - XIPNC_BASE + n <= flash_size) {
            rom_add_blob_fixed(name, img, n, va);
            continue;
        } else {
            warn_report("atbm-hera: segment %d at 0x%x not in a memory", i,
                        va);
            continue;
        }
        if (pa + FLASH_BOOT_LEN + n <= flash_size) {
            g_autofree char *lma = g_strdup_printf("atbm-hera.lma%d", i);

            rom_add_blob_fixed(lma, img, n, XIP_BASE + FLASH_BOOT_LEN + pa);
        }
    }
}

/* A named gpio list of the device, with at least n inputs */
static bool atbm_hera_has_in(DeviceState *dev, const char *name, int n)
{
    NamedGPIOList *ngl;

    QLIST_FOREACH(ngl, &dev->gpios, node) {
        if (!g_strcmp0(ngl->name, name)) {
            return ngl->num_in >= n;
        }
    }
    return false;
}

/*
 * Chip interrupt n is the CPU's "IRQ" input n and the watchdogs' NMI its
 * "NMI" input, for a CPU that has them (the nds32 one does); another
 * target's CPU, as the qtests use, is left alone
 */
static void atbm_hera_wire_cpu(AtbmHeraMachine *m)
{
    DeviceState *cpu = DEVICE(m->cpu);

    if (!m->wire_cpu || !atbm_hera_has_in(cpu, "IRQ", N_INTS)) {
        return;
    }
    for (int i = 0; i < N_INTS; i++) {
        qdev_connect_gpio_out_named(DEVICE(m->ints), "int", i,
                                    qdev_get_gpio_in_named(cpu, "IRQ", i));
    }
    if (atbm_hera_has_in(cpu, "NMI", 1)) {
        qdev_connect_gpio_out_named(DEVICE(m->ints), "nmi", 0,
                                    qdev_get_gpio_in_named(cpu, "NMI", 0));
    }
}

/*
 * The boot ROM stand-in: the jump a valid boot_config asks for. Reads go
 * through the uncached XIP window, which is how the ROM sees the flash.
 */
static void atbm_hera_rom_boot(AtbmHeraMachine *m)
{
    uint32_t cfg[BOOTCFG_WORDS + 1], sum = BOOTCFG_CKSUM_SEED;
    int i;

    if (address_space_read(&address_space_memory, XIPNC_BASE,
                           MEMTXATTRS_UNSPECIFIED, cfg,
                           sizeof(cfg)) != MEMTX_OK) {
        goto fail;
    }
    for (i = 0; i < BOOTCFG_WORDS + 1; i++) {
        cfg[i] = le32_to_cpu(cfg[i]);
    }
    for (i = 0; i < BOOTCFG_WORDS * 4; i++) {
        sum += extract32(cfg[i / 4], (i % 4) * 8, 8);
    }
    if (cfg[0] != BOOTCFG_VALID) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "atbm-hera: boot ROM: flash 0 is not a boot_config "
                      "(0x%x)\n", cfg[0]);
        goto fail;
    }
    if (sum != cfg[BOOTCFG_WORDS]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "atbm-hera: boot ROM: config_checksum 0x%x, 0x%x "
                      "expected (booting anyway)\n", cfg[BOOTCFG_WORDS], sum);
    }
    if (!atbm_hera_in(cfg[2], 4, XIPNC_BASE, XIP_SIZE) &&
        !atbm_hera_in(cfg[2], 4, XIP_BASE, XIP_SIZE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "atbm-hera: boot ROM: boot1 address 0x%x is not in "
                      "the flash window\n", cfg[2]);
        goto fail;
    }
    cpu_set_pc(m->cpu, cfg[2]);
    return;
fail:
    m->rom_stopped = true;
    cpu_pause(m->cpu);
}

/*
 * The CPU's reset puts its program counter at the ELF's entry, or, booting
 * from the flash, runs the ROM stand-in
 */
static void atbm_hera_cpu_restart(AtbmHeraMachine *m)
{
    cpu_reset(m->cpu);
    if (m->flash_boot) {
        atbm_hera_rom_boot(m);
    } else if (m->entry) {
        cpu_set_pc(m->cpu, m->entry);
    }
}

static void atbm_hera_restart_work(CPUState *cs, run_on_cpu_data data)
{
    atbm_hera_cpu_restart(data.host_ptr);
}

/*
 * A restart a device asks for while the core runs (the lines below) is
 * done by the core's own thread: it would otherwise write the registers
 * back over a reset made from outside, at the end of its translation
 * block, and a pulse too short for the core to stop in would be lost
 */
static void atbm_hera_restart_later(AtbmHeraMachine *m)
{
    async_run_on_cpu(m->cpu, atbm_hera_restart_work, RUN_ON_CPU_HOST_PTR(m));
}

static void atbm_hera_cpu_reset(void *opaque)
{
    AtbmHeraMachine *m = opaque;

    if (m->cpu) {
        if (m->rom_stopped) {
            m->rom_stopped = false;
            cpu_resume(m->cpu);
        }
        if (m->held) {
            m->held = 0;
            cpu_resume(m->cpu);
        }
        atbm_hera_cpu_restart(m);
    }
}

/*
 * Lines into the core: 0 the SMU's CPU_CTRL hold bit and 1 the SDIO
 * host's reset (HIF CONFIG[14]) keep it in reset while high and restart
 * it from the entry when released; 2 is the watchdogs' reset stage, a
 * restart. A flash boot runs the ROM stand-in again after each restart.
 * 3 is the camera SoC's hold (the chip off or its reset pin, from the
 * link) and 4 the power-up that follows it, a reset of every device.
 */

static void atbm_hera_ctl(void *opaque, int n, int level)
{
    AtbmHeraMachine *m = opaque;
    uint32_t was = m->held;

    if (n == 2) {
        if (level && !was) {
            atbm_hera_restart_later(m);
        }
        return;
    }
    if (n == 4) {
        if (level) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        return;
    }
    m->held = deposit32(m->held, n, 1, !!level);
    if (!was && m->held) {
        cpu_pause(m->cpu);
        atbm_hera_restart_later(m);
    } else if (was && !m->held) {
        atbm_hera_restart_later(m);
        cpu_resume(m->cpu);
    }
}

/* A register file that answers what the LMAC's init waits for */
static void atbm_hera_wifi_stub(AtbmHeraMachine *m, const char *name,
                                hwaddr base, uint32_t size)
{
    DeviceState *dev = qdev_new("atbm-hera-wifi-stub");

    object_property_add_child(OBJECT(m), name, OBJECT(dev));
    qdev_prop_set_uint32(dev, "base", base);
    qdev_prop_set_uint32(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
}

/*
 * The link to a camera SoC's QEMU (-global atbm-hera-link.chardev=<id>):
 * without a chardev it does nothing. The HIF's interrupt to the host and
 * the GPIO block's pads go through it, and it holds and resets the chip.
 */
static void atbm_hera_link(AtbmHeraMachine *m, DeviceState *hif,
                           DeviceState *gpio)
{
    DeviceState *dev = qdev_new("atbm-hera-link");

    object_property_add_child(OBJECT(m), "link", OBJECT(dev));
    object_property_set_link(OBJECT(dev), "hif", OBJECT(hif), &error_abort);
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    qdev_connect_gpio_out_named(hif, "host-irq", 0,
                                qdev_get_gpio_in_named(dev, "host-irq", 0));
    for (int i = 0; i < 28; i++) {
        qdev_connect_gpio_out_named(gpio, "out", i,
                                    qdev_get_gpio_in_named(dev, "pad-out", i));
        qdev_connect_gpio_out_named(dev, "pad-in", i,
                                    qdev_get_gpio_in_named(gpio, "pin", i));
    }
    qdev_connect_gpio_out_named(dev, "hold", 0, m->ctl[3]);
    qdev_connect_gpio_out_named(dev, "reset", 0, m->ctl[4]);
}

static void atbm_hera_init(MachineState *machine)
{
    AtbmHeraMachine *m = ATBM_HERA_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Chardev *chr0 = serial_hd(0), *chr1 = serial_hd(1);
    DeviceState *dev;
    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
    DeviceState *spi1, *dmac, *hif;

    m->ints = ATBM_HERA_INTSINK(qdev_new(TYPE_ATBM_HERA_INTSINK));
    object_property_add_child(OBJECT(m), "intsink", OBJECT(m->ints));
    qdev_realize_and_unref(DEVICE(m->ints), NULL, &error_fatal);

    memory_region_init_ram(&m->ilm, NULL, "atbm-hera.ilm", ILM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, ILM_BASE, &m->ilm);
    memory_region_init_rom(&m->rom, NULL, "atbm-hera.rom", ROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, ROM_BASE, &m->rom);
    if (machine->firmware) {
        load_image_targphys(machine->firmware, ROM_BASE, ROM_SIZE,
                            &error_fatal);
    }
    memory_region_init_ram(&m->dtcm, NULL, "atbm-hera.dtcm", DTCM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, DTCM_BASE, &m->dtcm);
    memory_region_init_ram(&m->sram, NULL, "atbm-hera.sram", SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, SRAM_BASE, &m->sram);

    /*
     * The flash: 2 MiB, or 4 if the image is bigger. It sits behind SPI1,
     * which is also its controller, and its array is what the XIP windows
     * show, read-only
     */
    spi1 = atbm_hera_dev(m, "atbm-hera-spi", "spi1", 0x16a00000, -1, NULL);
    m->nor = ATBM_HERA_NOR(qdev_new(TYPE_ATBM_HERA_NOR));
    object_property_add_child(OBJECT(m), "flash", OBJECT(m->nor));
    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);

        qdev_prop_set_drive_err(DEVICE(m->nor), "drive", blk, &error_fatal);
        qdev_prop_set_uint32(DEVICE(m->nor), "size",
                             blk_getlength(blk) > 2 * MiB ? 4 * MiB : 2 * MiB);
    }
    qdev_realize_and_unref(DEVICE(m->nor), qdev_get_child_bus(spi1, "ssi"),
                           &error_fatal);
    qdev_connect_gpio_out_named(spi1, "cs", 0,
                                qdev_get_gpio_in_named(DEVICE(m->nor),
                                                       SSI_GPIO_CS, 0));
    memory_region_init_alias(&m->flash_cached, NULL, "atbm-hera.flash-c",
                             &m->nor->array, 0, MIN(m->nor->size, XIP_SIZE));
    memory_region_set_readonly(&m->flash_cached, true);
    memory_region_add_subregion(sysmem, XIP_BASE, &m->flash_cached);
    memory_region_init_alias(&m->flash_uncached, NULL, "atbm-hera.flash-nc",
                             &m->nor->array, 0, MIN(m->nor->size, XIP_SIZE));
    memory_region_set_readonly(&m->flash_uncached, true);
    memory_region_add_subregion(sysmem, XIPNC_BASE, &m->flash_uncached);

    memory_region_init_alias(&m->flash_bootwin, NULL, "atbm-hera.flash-boot",
                             &m->nor->array, BOOTWIN_OFFSET,
                             m->nor->size - BOOTWIN_OFFSET);
    memory_region_set_readonly(&m->flash_bootwin, true);
    memory_region_add_subregion(sysmem, BOOTWIN_BASE, &m->flash_bootwin);

    atbm_hera_dev(m, "atbm-hera-uart", "uart1", 0x16200000, 19, chr0);
    atbm_hera_dev(m, "atbm-hera-uart", "uart2", 0x16300000, 20, chr1);
    atbm_hera_dev(m, "atbm-hera-uart", "uart3", 0x17600000, 31, serial_hd(2));
    atbm_hera_dev(m, "atbm-hera-pit", "pit1", 0x16400000, 18, NULL);
    /*
     * PIT2 and PIT3 share an interrupt: the lib's handler for 17 and 18
     * is one routine that polls all three PITs (assumption: the 3rd PIT
     * has no number of its own in dev_int.h)
     */
    m->pit_or = atbm_hera_or(m, "pit-or", 2, 17);
    dev = atbm_hera_dev(m, "atbm-hera-pit", "pit2", 0x16500000, -1, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(m->pit_or, 0));
    dev = atbm_hera_dev(m, "atbm-hera-pit", "pit3", 0x17000000, -1, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(m->pit_or, 1));

    atbm_hera_dev(m, "atbm-hera-efuse", "efuse", 0x16b00000, -1, NULL);
    /*
     * The DMA controller: the SPIs' request lines are the data sheet's
     * numbers (SPI1 TX 6, RX 7, SPI3 TX 8, RX 9; SPI2 TX 4, RX 5)
     */
    dmac = atbm_hera_dev(m, "atbm-hera-dmac", "dmac", 0x14000000, 7, NULL);
    qdev_connect_gpio_out_named(spi1, "dreq-tx", 0,
                                qdev_get_gpio_in_named(dmac, "req", 6));
    qdev_connect_gpio_out_named(spi1, "dreq-rx", 0,
                                qdev_get_gpio_in_named(dmac, "req", 7));
    /* SPI2 is the slave-capable one: see atbm-hera-spi.c */
    dev = qdev_new("atbm-hera-spi");
    object_property_add_child(OBJECT(m), "spi2", OBJECT(dev));
    qdev_prop_set_bit(dev, "slave", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x16c00000);
    qdev_connect_gpio_out_named(dev, "dreq-tx", 0,
                                qdev_get_gpio_in_named(dmac, "req", 4));
    qdev_connect_gpio_out_named(dev, "dreq-rx", 0,
                                qdev_get_gpio_in_named(dmac, "req", 5));
    dev = atbm_hera_dev(m, "atbm-hera-spi", "spi3", 0x16d00000, 13, NULL);
    qdev_connect_gpio_out_named(dev, "dreq-tx", 0,
                                qdev_get_gpio_in_named(dmac, "req", 8));
    qdev_connect_gpio_out_named(dev, "dreq-rx", 0,
                                qdev_get_gpio_in_named(dmac, "req", 9));
    m->ctl = qemu_allocate_irqs(atbm_hera_ctl, m, 5);
    dev = atbm_hera_dev(m, "atbm-hera-hif", "hif", 0x0ab00000, 10, NULL);
    hif = dev;
    qdev_connect_gpio_out_named(dev, "cpu-reset", 0, m->ctl[1]);
    if (m->hif_port) {
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, m->hif_port);
    }
    atbm_hera_dev(m, "atbm-hera-i2c", "i2c", 0x16900000, 22, NULL);
    dev = atbm_hera_dev(m, "atbm-hera-smu", "smu", 0x16100000, 28, NULL);
    qdev_connect_gpio_out_named(dev, "cpu-hold", 0, m->ctl[0]);
    atbm_hera_connect(m, dev, 1, 30);
    dev = atbm_hera_dev(m, "atbm-hera-gpio", "gpio", 0x16800000, 23, NULL);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0x17400000);
    atbm_hera_link(m, hif, dev);
    dev = atbm_hera_dev(m, "atbm-hera-rtc", "rtc1", 0x16700000, 25, NULL);
    atbm_hera_connect(m, dev, 1, 26);
    dev = atbm_hera_dev(m, "atbm-hera-rtc", "rtc2", 0x17100000, 15, NULL);
    atbm_hera_connect(m, dev, 1, 16);

    m->nmi_or = atbm_hera_or(m, "nmi-or", 2, NMI_IN);
    dev = atbm_hera_dev(m, "atbm-hera-wdt", "wdt1", 0x16600000, -1, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(m->nmi_or, 0));
    qdev_connect_gpio_out_named(dev, "cpu-reset", 0, m->ctl[2]);
    dev = atbm_hera_dev(m, "atbm-hera-wdt", "wdt2", 0x17500000, -1, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(m->nmi_or, 1));
    qdev_connect_gpio_out_named(dev, "cpu-reset", 0, m->ctl[2]);

    /*
     * The Wi-Fi MAC and PHY: undocumented, out of scope here (a shim at
     * the WSM boundary is the plan), so reads give 0 and are logged
     */
    atbm_hera_wifi_stub(m, "pac", 0x0a500000, 0x20000);
    create_unimplemented_device("atbm-hera.rab", 0x0a520000, 0x20000);
    create_unimplemented_device("atbm-hera.smart", 0x0a540000, 0x20000);
    create_unimplemented_device("atbm-hera.pac-seq", 0x0a560000, 0x20000);
    atbm_hera_wifi_stub(m, "phy", 0x0ac00000, 0x100000);
    atbm_hera_dev(m, "atbm-hera-crypto", "crypto", 0x17200000, -1, NULL);
    create_unimplemented_device("atbm-hera.wgt", 0x0ad00000, 0x100000);

    if (machine->cpu_type) {
        m->cpu = CPU(cpu_create(machine->cpu_type));
        atbm_hera_wire_cpu(m);
    }
    if (machine->kernel_filename) {
        atbm_hera_load_elf(m, machine->kernel_filename);
    } else if (dinfo) {
        m->flash_boot = true;
    }
    qemu_register_reset(atbm_hera_cpu_reset, m);
}

static bool atbm_hera_get_wire(Object *obj, Error **errp)
{
    return ATBM_HERA_MACHINE(obj)->wire_cpu;
}

static void atbm_hera_set_wire(Object *obj, bool value, Error **errp)
{
    ATBM_HERA_MACHINE(obj)->wire_cpu = value;
}

static void atbm_hera_instance_init(Object *obj)
{
    AtbmHeraMachine *m = ATBM_HERA_MACHINE(obj);

    m->wire_cpu = true;
    object_property_add_uint64_ptr(obj, "hif-host-port", &m->hif_port,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "hif-host-port",
        "map the HIF's host test port (a CPU or qtest plays the SDIO host) "
        "at this address; 0 leaves it out");
}

static void atbm_hera_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "ATBM6441 Hera Wi-Fi/IoT MCU";
    mc->default_cpu_type = NDS32_CPU_TYPE_NAME("n9");
    mc->init = atbm_hera_init;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->max_cpus = 1;
    object_class_property_add_bool(oc, "wire-cpu", atbm_hera_get_wire,
                                   atbm_hera_set_wire);
    object_class_property_set_description(oc, "wire-cpu",
        "wire the CPU's interrupt inputs 0..31 to the chip's interrupts");
}

static const TypeInfo atbm_hera_machine_info = {
    .name = TYPE_ATBM_HERA_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(AtbmHeraMachine),
    .instance_init = atbm_hera_instance_init,
    .class_init = atbm_hera_machine_class_init,
};

static void atbm_hera_register_types(void)
{
    type_register_static(&intsink_info);
    type_register_static(&atbm_hera_machine_info);
}

type_init(atbm_hera_register_types)
