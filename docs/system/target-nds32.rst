.. _NDS32-System-emulator:

NDS32 System emulator
=====================

``qemu-system-nds32`` emulates little-endian Andes NDS32 (AndeStar V3)
systems. The target exists to run the firmware of AltoBeam's ATBM6441 and
ATBM6041 "Hera" Wi-Fi/MCU chips, whose CPU is an AndeStar V3 core, so
the CPU model covers what that firmware and its bootloader use.

CPU models
----------

``-cpu n9`` (the default) and ``-cpu n12`` are available. They differ only
in the core id they report in ``CPU_VER``.

The model has:

- the 32-bit instruction set and the 16-bit forms, including
  ``push25``/``pop25``, ``lmw``/``smw``, multiply, divide, the MAC
  accumulators, the byte search and ``pbsad`` instructions. Loads and
  stores must be naturally aligned and raise an alignment check exception
  otherwise, as the silicon does
- ``ifcall`` and ``ifcall9`` with their link behaviour. ``ex9.it`` and
  ``ex9.it5`` are not executed
- the system registers ``PSW``, ``IPSW``, ``IPC``, ``IVB`` and ``ITYPE``,
  nested interrupt levels with ``iret``, and the IVIC interrupt controller
  with ``INT_MASK``/``INT_MASK2``, ``INT_PEND``/``INT_PEND2``, per-source
  priorities and a software interrupt (IVIC version 1). The CPU's
  ``IRQ`` inputs 0 to 31 and ``NMI`` input are named GPIOs, and it has
  ``standby`` and ``power-hold`` lines for a power controller
- standby, and the general exceptions
- a gdbstub whose register layout matches the vendor's gdb, and a
  disassembler for ``-d in_asm``

The ``swi-vector`` property sets the exception vector number the
``swi`` instruction raises (the default is 14).

It has no FPU, MMU, coprocessor or audio extension.

Machines
--------

``nds32-virt``
   A generic machine with 16 MiB of RAM at 0, 1 MiB of SRAM at
   ``0x09000000``, a test finisher at ``0x0f000000`` (the SiFive test
   device: writing it ends the run) and a 16550 UART at ``0x0f001000``
   on interrupt 19. ``-kernel`` takes an ELF file.

``atbm-hera``
   The ATBM6441 chip: the machine is described below.

The ATBM6441 ("atbm-hera")
--------------------------

The ``atbm-hera`` machine is the chip as the SDK's linker scripts and
headers lay it out:

=================== ==========================================================
Address             What
=================== ==========================================================
``0x00030000``      ILM, 64 KiB, code the bootloader copies from flash
``0x00060000``      boot ROM, empty unless ``-bios`` loads one
``0x00080000``      flash, cached XIP window, up to 3.5 MiB of it
``0x00400000``      flash, uncached XIP window
``0x00800000``      DTCM, 256 KiB
``0x01400000``      flash from ``0x1000`` up, where the bootloader is linked
``0x09000000``      SRAM, 256 KiB, shared with the Wi-Fi MAC
``0x0a000000``      Wi-Fi MAC, host interface (HIF) and PHY
``0x14000000``      DMA controller
``0x16000000``      APB peripherals
``0x17000000``      more APB peripherals
=================== ==========================================================

The peripherals modelled are three UARTs, three timers, two watchdogs,
two RTCs, the GPIO block, three SPI controllers (SPI1 also being the
flash's controller), I2C, the DMA controller, the eFuse, the cipher and
hash engine, the power controller (SMU) and the SDIO host interface. The
flash is a SPI NOR device, 2 MiB or, for an image above 2 MiB, 4 MiB, given
with ``-drive if=mtd,format=raw``. The first ``-serial`` is UART1, the
second UART2 and the third UART3. The Wi-Fi MAC and PHY are register
stubs that read what was written; they are not a radio.

There are two ways to boot it.

``-kernel <firmware ELF>``
   A segment in the ILM, DTCM or SRAM is loaded there. A segment in a flash
   window goes into the flash, and every segment is also stored in the
   flash image at its load address plus ``0x10000`` (the 64 KiB the
   bootloader takes in front of the firmware), which is where the
   firmware's startup code copies its data from. The CPU starts at the
   ELF's entry.

flash image
   With ``-drive if=mtd`` and no ``-kernel``, a ROM stand-in does the boot
   ROM's job, which is not available: it reads the ``boot_config`` at
   flash 0, wants ``0xab45`` as its first word, and jumps to the boot1
   address it holds (the flash bootloader through the uncached window).
   The vendor's bootloader then copies itself and the firmware to RAM.
   A bad header stops the CPU; the ROM's UART download mode is not
   modelled. A checksum that does not match is logged and booted anyway.

Machine properties:

``wire-cpu=on|off``
   connect the CPU's interrupt inputs to the chip's interrupts (on by
   default)
``pir-pad``, ``pir``
   a PIR sensor on a chip pad (0 to 27, ``-1`` for none) and its level;
   ``qom-set /machine pir true`` raises it at run time
``charger-pad``, ``charger``
   the same for a charger's detect line
``hif-host-port``
   map the HIF's host test port at an address, for a test where a CPU plays
   the SDIO host

``-global atbm-hera-nor.restore-on-reset=on`` puts the flash back as it was
loaded at every chip reset. A chip that reboots keeps what the firmware
wrote to its flash, and that can change how it boots the next time.

To pair the chip with a camera's QEMU, give it a link socket with
``-chardev socket,...`` and ``-global atbm-hera-link.chardev=<id>``. The
walkthrough, including the second UART and what the camera does when the
chip resets, is in :ref:`atbm-own-firmware`. For Wi-Fi, see
``contrib/atbm-hera-lmac/README.md``: the firmware is linked with a
stand-in for the lower MAC, and the board's paravirtual port takes the
next ``-nic``.

Run the chip with ``-icount shift=auto,sleep=on``. Without it the chip's
time follows the host's, and its watchdog resets it whenever its process
falls a few seconds behind. With ``sleep=on`` a chip in standby, or in the
light sleep its firmware's idle hook enters, uses next to no host CPU. In
light sleep the core and the timers are off and come back from reset on
the wake, while the memories, the GPIO pads, the RTCs and the Wi-Fi port
keep their state. An RTC alarm, a wake pad (a PIR, for example) or a frame
for the chip wakes it.

Example, running a firmware ELF with the console on UART2::

   qemu-system-nds32 -M atbm-hera -icount shift=auto,sleep=on \
       -display none -kernel iot.out -serial null -serial stdio

Example, booting a flash image::

   qemu-system-nds32 -M atbm-hera -icount shift=auto,sleep=on \
       -display none -drive file=flash.bin,format=raw,if=mtd \
       -serial null -serial stdio
