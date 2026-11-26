gen = [
  decodetree.process('insn32.decode',
                     extra_args: ['--static-decode=decode_insn32']),
  decodetree.process('insn16.decode',
                     extra_args: ['--static-decode=decode_insn16',
                                  '--insnwidth=16']),
]

nds32_ss = ss.source_set()
nds32_ss.add(gen)
nds32_ss.add(files(
  'cpu.c',
  'disas.c',
  'helper.c',
  'translate.c',
))

nds32_common_system_ss = ss.source_set()
nds32_common_system_ss.add(files('gdbstub.c'))

nds32_system_ss = ss.source_set()
nds32_system_ss.add(files('machine.c'))

target_arch += {'nds32': nds32_ss}
target_system_arch += {'nds32': nds32_system_ss}
target_common_system_arch += {'nds32': nds32_common_system_ss}
