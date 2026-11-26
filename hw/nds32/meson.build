nds32_ss = ss.source_set()
nds32_ss.add(when: 'CONFIG_NDS32_VIRT', if_true: files('virt.c'))
nds32_ss.add(when: 'CONFIG_ATBM_HERA', if_true: files('atbm-hera.c'))

hw_arch += {'nds32': nds32_ss}
