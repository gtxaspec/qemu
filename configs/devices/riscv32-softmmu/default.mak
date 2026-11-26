# Default configuration for riscv32-softmmu

# Uncomment the following lines to disable these optional devices:
# CONFIG_PCI_DEVICES=n
# CONFIG_TEST_DEVICES=n

# Boards are selected by default, uncomment to keep out of the build.
# CONFIG_SPIKE=n
# CONFIG_SIFIVE_E=n
# CONFIG_SIFIVE_U=n
# CONFIG_RISCV_VIRT=n
# CONFIG_OPENTITAN=n

# The ATBM6441 "Hera" machine, with no CPU until target/nds32 exists
CONFIG_ATBM_HERA=y
