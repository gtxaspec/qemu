nds32_ss = ss.source_set()
nds32_ss.add(when: 'CONFIG_NDS32_VIRT', if_true: files('virt.c'))

hw_arch += {'nds32': nds32_ss}

# The Hera machine has no CPU of its own here: it builds in any softmmu
# target so its devices can be tested before the nds32 core is there.
system_ss.add(when: 'CONFIG_ATBM_HERA', if_true: files('atbm-hera.c'))
