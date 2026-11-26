# SPDX-License-Identifier: GPL-2.0-or-later
#
# The nds32 tests are freestanding programs for the nds32-virt machine.  They
# need an NDS32 cross compiler (nds32le-elf-gcc), they are skipped otherwise.

if 'qemu-system-nds32' in emulators
  nds32_cc = find_program('nds32le-elf-gcc', required: false)
  nds32_cc_ok = false
  if nds32_cc.found()
    nds32_cmd = run_command([nds32_cc, files('../test_cc.c'), '-nostdlib',
                             '-ffreestanding', '-r', '-o', '/dev/null'],
                            check: false)
    nds32_cc_ok = nds32_cmd.returncode() == 0
  endif
  tcg_tests_summary += {'cc for nds32-softmmu': nds32_cc_ok ? nds32_cc.full_path() : false}

  if nds32_cc_ok
    nds32_qemu = emulators['qemu-system-nds32']
    nds32_build = meson.current_build_dir()

    nds32_exc = custom_target('nds32-exceptions.elf',
                              input: 'system/exceptions.S',
                              output: 'exceptions.elf',
                              command: [nds32_cc, '-nostdlib',
                                        '-Wl,-T', files('nds32.ld'),
                                        '-o', '@OUTPUT@', '@INPUT@'],
                              build_by_default: false)
    test('tcg-nds32-exceptions', nds32_qemu,
         args: ['-M', 'nds32-virt', '-display', 'none', '-serial', 'none',
                '-monitor', 'none', '-kernel', nds32_exc],
         depends: nds32_exc,
         suite: ['tcg', 'tcg-nds32'])

    foreach group: ['alu', 'imm', 'mul', 'div', 'mem', 'br', 'lsmw', 'stack',
                    'misc16', 'ifc', 'bytes', 'corner']
      test('tcg-nds32-' + group, find_program('run-diff.py'),
           args: [nds32_qemu, nds32_cc, group],
           suite: ['tcg', 'tcg-nds32'])
    endforeach
  endif
endif
