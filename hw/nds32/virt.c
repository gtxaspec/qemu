/*
 * Generic NDS32 machine: flat RAM, a test finisher and a 16550 UART
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/sifive_test.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/address-spaces.h"
#include "target/nds32/cpu.h"
#include "elf.h"

#define VIRT_RAM_BASE    0x00000000
#define VIRT_RAM_SIZE    (16 * MiB)
#define VIRT_SRAM_BASE   0x09000000
#define VIRT_SRAM_SIZE   (1 * MiB)
#define VIRT_TEST_BASE   0x0f000000
#define VIRT_UART_BASE   0x0f001000
#define VIRT_UART_IRQ    19

typedef struct {
    NDS32CPU *cpu;
    uint64_t entry;
} VirtResetData;

static void virt_cpu_reset(void *opaque)
{
    VirtResetData *d = opaque;

    cpu_reset(CPU(d->cpu));
    cpu_set_pc(CPU(d->cpu), d->entry);
}

static void nds32_virt_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    VirtResetData *rd = g_new0(VirtResetData, 1);
    NDS32CPU *cpu;
    SerialMM *uart;

    cpu = NDS32_CPU(cpu_create(machine->cpu_type));
    rd->cpu = cpu;

    memory_region_init_ram(ram, NULL, "nds32.ram", VIRT_RAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, VIRT_RAM_BASE, ram);
    memory_region_init_ram(sram, NULL, "nds32.sram", VIRT_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, VIRT_SRAM_BASE, sram);

    sifive_test_create(VIRT_TEST_BASE);
    uart = serial_mm_init(sysmem, VIRT_UART_BASE, 2,
                          qdev_get_gpio_in_named(DEVICE(cpu), "IRQ",
                                                 VIRT_UART_IRQ),
                          115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);
    (void)uart;

    if (machine->kernel_filename) {
        uint64_t entry;

        if (load_elf(machine->kernel_filename, NULL, NULL, NULL, &entry,
                     NULL, NULL, NULL, ELFDATA2LSB, EM_NDS32, 0, 0) < 0) {
            error_report("could not load '%s'", machine->kernel_filename);
            exit(1);
        }
        rd->entry = entry;
    }
    qemu_register_reset(virt_cpu_reset, rd);
}

static void nds32_virt_machine_init(MachineClass *mc)
{
    mc->desc = "Generic NDS32 machine with flat RAM";
    mc->init = nds32_virt_init;
    mc->max_cpus = 1;
    mc->is_default = true;
    mc->default_cpu_type = NDS32_CPU_TYPE_NAME("n9");
}

DEFINE_MACHINE("nds32-virt", nds32_virt_machine_init)
