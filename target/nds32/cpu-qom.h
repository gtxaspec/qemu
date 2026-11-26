/*
 * QEMU NDS32 CPU QOM header (target agnostic)
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_NDS32_CPU_QOM_H
#define QEMU_NDS32_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_NDS32_CPU "nds32-cpu"

OBJECT_DECLARE_CPU_TYPE(NDS32CPU, NDS32CPUClass, NDS32_CPU)

#define NDS32_CPU_TYPE_SUFFIX "-" TYPE_NDS32_CPU
#define NDS32_CPU_TYPE_NAME(model) model NDS32_CPU_TYPE_SUFFIX

#endif
