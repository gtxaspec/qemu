/*
 * ATBM6441 "Hera" Wi-Fi/IoT MCU board
 *
 * The chip's own CPU is an Andes NDS32 core, which QEMU gets with
 * target/nds32. Until that is in, the machine is built for every target
 * and has no CPU; it keeps its devices, memories and interrupt lines, so
 * a qtest drives them through MMIO (qtest needs a CPU to have an address
 * space: any "-cpu" will do, its lines unwired). With "-cpu" and
 * -machine wire-cpu=on the CPU's interrupt inputs 0..31 are wired to the
 * chip's interrupt numbers.
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
 * code copies its data from.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
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

/* A chip interrupt number is a CPU interrupt input (dev_int.h) */
#define N_INTS          32

/*
 * The place where the sources meet: the CPU's inputs are its outputs,
 * and each input's level is kept to look at.
 */
struct AtbmHeraIntSink {
    DeviceState parent_obj;

    qemu_irq out[N_INTS];
    uint32_t levels;
};

static void intsink_set(void *opaque, int n, int level)
{
    AtbmHeraIntSink *s = opaque;

    s->levels = deposit32(s->levels, n, 1, !!level);
    qemu_set_irq(s->out[n], level);
}

static void intsink_init(Object *obj)
{
    AtbmHeraIntSink *s = ATBM_HERA_INTSINK(obj);

    qdev_init_gpio_in(DEVICE(obj), intsink_set, N_INTS);
    qdev_init_gpio_out_named(DEVICE(obj), s->out, "int", N_INTS);
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
    MemoryRegion flash;
    MemoryRegion flash_cached;
    MemoryRegion flash_uncached;
    AtbmHeraIntSink *ints;
    uint32_t entry;
    bool wire_cpu;
};

static void atbm_hera_connect(AtbmHeraMachine *m, DeviceState *dev, int n,
                              int irq)
{
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), n,
                       qdev_get_gpio_in(DEVICE(m->ints), irq));
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
    uint64_t flash_size = memory_region_size(&m->flash);

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
        uint32_t va = ph[i].p_vaddr, pa = ph[i].p_paddr, n = ph[i].p_filesz;
        g_autofree char *name = g_strdup_printf("atbm-hera.seg%d", i);

        if (ph[i].p_type != PT_LOAD || !n ||
            (uint64_t)ph[i].p_offset + n > len) {
            continue;
        }
        if (atbm_hera_in(va, n, ILM_BASE, ILM_SIZE) ||
            atbm_hera_in(va, n, DTCM_BASE, DTCM_SIZE) ||
            atbm_hera_in(va, n, SRAM_BASE, SRAM_SIZE)) {
            rom_add_blob_fixed(name, data + ph[i].p_offset, n, va);
        } else if (atbm_hera_in(va, n, XIP_BASE, XIP_SIZE) &&
                   va - XIP_BASE + n <= flash_size) {
            rom_add_blob_fixed(name, data + ph[i].p_offset, n, va);
            continue;
        } else if (atbm_hera_in(va, n, XIPNC_BASE, XIP_SIZE) &&
                   va - XIPNC_BASE + n <= flash_size) {
            rom_add_blob_fixed(name, data + ph[i].p_offset, n, va);
            continue;
        } else {
            warn_report("atbm-hera: segment %d at 0x%x not in a memory", i,
                        va);
            continue;
        }
        if (pa + FLASH_BOOT_LEN + n <= flash_size) {
            g_autofree char *lma = g_strdup_printf("atbm-hera.lma%d", i);

            rom_add_blob_fixed(lma, data + ph[i].p_offset, n,
                               XIP_BASE + FLASH_BOOT_LEN + pa);
        }
    }
}

static void atbm_hera_init(MachineState *machine)
{
    AtbmHeraMachine *m = ATBM_HERA_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Chardev *chr0 = serial_hd(0), *chr1 = serial_hd(1);
    CPUState *cpu = NULL;
    uint64_t flash_size = 2 * MiB;

    m->ints = ATBM_HERA_INTSINK(qdev_new(TYPE_ATBM_HERA_INTSINK));
    object_property_add_child(OBJECT(m), "intsink", OBJECT(m->ints));
    qdev_realize_and_unref(DEVICE(m->ints), NULL, &error_fatal);

    memory_region_init_ram(&m->ilm, NULL, "atbm-hera.ilm", ILM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, ILM_BASE, &m->ilm);
    memory_region_init_ram(&m->rom, NULL, "atbm-hera.rom", ROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, ROM_BASE, &m->rom);
    memory_region_init_ram(&m->dtcm, NULL, "atbm-hera.dtcm", DTCM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, DTCM_BASE, &m->dtcm);
    memory_region_init_ram(&m->sram, NULL, "atbm-hera.sram", SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, SRAM_BASE, &m->sram);

    memory_region_init_ram(&m->flash, NULL, "atbm-hera.flash", flash_size,
                           &error_fatal);
    memset(memory_region_get_ram_ptr(&m->flash), 0xff, flash_size);
    memory_region_init_alias(&m->flash_cached, NULL, "atbm-hera.flash-c",
                             &m->flash, 0, MIN(flash_size, XIP_SIZE));
    memory_region_add_subregion(sysmem, XIP_BASE, &m->flash_cached);
    memory_region_init_alias(&m->flash_uncached, NULL, "atbm-hera.flash-nc",
                             &m->flash, 0, MIN(flash_size, XIP_SIZE));
    memory_region_add_subregion(sysmem, XIPNC_BASE, &m->flash_uncached);

    atbm_hera_dev(m, "atbm-hera-uart", "uart1", 0x16200000, 19, chr0);
    atbm_hera_dev(m, "atbm-hera-uart", "uart2", 0x16300000, 20, chr1);

    if (machine->cpu_type) {
        cpu = CPU(cpu_create(machine->cpu_type));
        for (int i = 0; m->wire_cpu && i < N_INTS; i++) {
            qdev_connect_gpio_out_named(DEVICE(m->ints), "int", i,
                                        qdev_get_gpio_in(DEVICE(cpu), i));
        }
    }
    if (machine->kernel_filename) {
        atbm_hera_load_elf(m, machine->kernel_filename);
        if (cpu) {
            cpu_set_pc(cpu, m->entry);
        }
    }
}

static bool atbm_hera_get_wire(Object *obj, Error **errp)
{
    return ATBM_HERA_MACHINE(obj)->wire_cpu;
}

static void atbm_hera_set_wire(Object *obj, bool value, Error **errp)
{
    ATBM_HERA_MACHINE(obj)->wire_cpu = value;
}

static void atbm_hera_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "ATBM6441 Hera Wi-Fi/IoT MCU";
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
    .class_init = atbm_hera_machine_class_init,
};

static void atbm_hera_register_types(void)
{
    type_register_static(&intsink_info);
    type_register_static(&atbm_hera_machine_info);
}

type_init(atbm_hera_register_types)
