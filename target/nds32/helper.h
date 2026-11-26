/*
 * NDS32 helpers
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

DEF_HELPER_4(raise_exception, noreturn, env, i32, i32, i32)
DEF_HELPER_FLAGS_2(mfsr, TCG_CALL_NO_WG, i32, env, i32)
DEF_HELPER_3(mtsr, void, env, i32, i32)
DEF_HELPER_1(iret, void, env)
DEF_HELPER_1(standby, noreturn, env)
DEF_HELPER_1(div0, void, env)
DEF_HELPER_2(setgie, void, env, i32)
