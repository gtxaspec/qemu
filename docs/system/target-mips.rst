.. _MIPS-System-emulator:

MIPS System emulator
--------------------

Four executables cover simulation of 32 and 64-bit MIPS systems in both
endian options, ``qemu-system-mips``, ``qemu-system-mipsel``
``qemu-system-mips64`` and ``qemu-system-mips64el``. Five different
machine types are emulated:

-  The MIPS Malta prototype board \"malta\"

-  An ACER Pica \"pica61\". This machine needs the 64-bit emulator.

-  A MIPS Magnum R4000 machine \"magnum\". This machine needs the
   64-bit emulator.

The Malta emulation supports the following devices:

-  Core board with MIPS 24Kf CPU and Galileo system controller

-  PIIX4 PCI/USB/SMbus controller

-  The Multi-I/O chip's serial device

-  PCI network cards (PCnet32 and others)

-  Malta FPGA serial device

-  Cirrus (default) or any other PCI VGA graphics card

The Boston board emulation supports the following devices:

-  Xilinx FPGA, which includes a PCIe root port and an UART

-  Intel EG20T PCH connects the I/O peripherals, but only the SATA bus
   is emulated

The ACER Pica emulation supports:

-  MIPS R4000 CPU

-  PC-style IRQ and DMA controllers

-  PC Keyboard

-  IDE controller

The MIPS Magnum R4000 emulation supports:

-  MIPS R4000 CPU

-  PC-style IRQ controller

-  PC Keyboard

-  SCSI controller

-  G364 framebuffer

The Fuloong 2E emulation supports:

-  Loongson 2E CPU

-  Bonito64 system controller as North Bridge

-  VT82C686 chipset as South Bridge

-  RTL8139D as a network card chipset

The Loongson-3 virtual platform emulation supports:

-  Loongson 3A CPU

-  LIOINTC as interrupt controller

-  GPEX and virtio as peripheral devices

Ingenic XBurst SoCs
~~~~~~~~~~~~~~~~~~~

Eight XBurst1 single-core MIPS32r2 SoCs (T10, T20, T21, T23, T30, T31,
T32, T33) and three XBurst2 dual-core SMP SoCs (A1, T40, T41) used in
IP cameras, embedded recorders and IoT devices.

QEMU machine names match the part numbers: ``ingenic-t10`` through
``ingenic-t33``, plus ``ingenic-a1``, ``ingenic-t40`` and
``ingenic-t41``. Each XBurst1 machine has subvariants selectable via
``-global ingenic-t31.soc-variant=<name>`` (for example, ``t31x``,
``t32nq``, ``t20l``); subvariant controls cpuid, DDR geometry and the
clocking configuration. The C100 runs as a T31 variant (``c100``), the
T32Pro and CW080 as T32 variants (``t32prozn``, ``cw080sn`` and others).
``-m`` is the board's DDR: at most 256 MiB on an XBurst1 SoC, below its
peripherals; on the T40, T41 and A1 (up to 512 MiB, 512 MiB and 1 GiB)
all of it shows again from 0x20000000. QEMU warns when ``-m`` is less
than the subvariant has.

Supported peripherals:

-  CPM (clock and PLL control), DDR2/DDR3 controller with InnoSilicon
   PHY
-  SPI-NOR flash controller (V1 on T10-T31, V2 on T32/T33/XBurst2),
   its part 8 (for an image of exactly 8 MiB), 16, 32 or 64 MiB to fit the
   image (``jedec-id`` sets its ID)
-  Ethernet: vendor MAC (older dwmac) on T-series and T40/T41,
   dwxgmac2 on A1
-  DWC2 USB host (3 controllers on A1, one on T40/T41 and the
   T-series)
-  TCU + OST register page: 8 timer/PWM channels, watchdog (channel
   16) and the embedded 64-bit OST counter
-  Dedicated PWM controller (V2) at 0x13450000 on T32/T33 and at
   0x13460000 on A1/T40/T41
-  Real-time clock, true random number generator, I2C, SDHCI/MSC,
   PDMA, GPIO, SADC, INTC and UART
-  I2C0 bit-banged through its pads as GPIOs on the T-series, T40 and
   T41, as vendor factory probes and the T10's kernels do
-  SDIO cards on the MSC and SDHCI controllers, whose buses are named
   ``msc0`` and ``msc1`` on the T-series, T40 and T41
-  Wi-Fi as the cameras' Linux drivers see it: AICSemi AIC8800,
   AIC8800D80, AIC8800DC and AIC8800DW on USB and AIC8800D80 on SDIO,
   AltoBeam ATBM60xx on SDIO or USB, Wi-Fi 6 parts included, Broadcom
   BCM43438 and BCM43436 on SDIO, HiSilicon Hi3881 on SDIO and WS73V100
   on USB, Huge-IC TXW901U on USB, iComm-semi SSV6155P and SSV6355 on
   USB and SSV6158 on SDIO, MediaTek MT7601U on USB, Realtek RTL8189FS,
   RTL8189ES and RTL8192FS on SDIO and RTL8188FU, RTL8188EUS and
   RTL8733BU on USB, Seekwave SWT6621S on USB and WuQi WQ9001 on USB,
   with a WPA2 access point to join
   (``-device atbm6031,bus=msc1,netdev=...``) and a client for the
   chip's own access point (``client-netdev=...``)
-  SPI NAND on the flash controller, and the LZMA engines of the T32,
   T33, T40 and T41
-  AES, DES, HASH and RSA engines; the RSA engine needs GMP to build
-  Camera video on every T-series SoC and the T40 and T41: image
   sensors on I2C (``-device image-sensor``), every model thingino's
   cameras and the vendor SDK demos use, the ISP with its OSD, the IPU,
   the IVDC line ring and the H.264, H.265 and JPEG encoders
-  Audio: the AIC with the internal codec, and the DMIC of the T30,
   T31, T32, T33, T40 and T41
-  AHCI SATA (A1 only)
-  Per-core INTC, Core OST and CCU on the XBurst2 family

Blocks without a model, such as the NPUs, the LDC and the display
outputs, are unimplemented stubs. The Ethernet MAC takes the netdev
named ``n0``. ``contrib/ingenic/README.md``, shipped with the release
binaries, covers video, audio, SPI NOR and NAND, the test inputs
(``-global ingenic-gpio.in-pb=...``), the pin trace
(``-global ingenic-gpio.trace=<chardev>``), the ISP and image sensor
trace events (``-trace 'ingenic_isp_*'``, read with ``isp-trace.py``)
and Wi-Fi in detail.

.. _atbm-own-firmware:

ATBM6441 with its own firmware
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``-device atbm6441`` models the chip's behaviour from the host's side. To
run the chip's own firmware instead, start it in ``qemu-system-nds32``
(machine ``atbm-hera``, ``-kernel <firmware ELF>``) with a link socket
and give the camera's QEMU the matching card. The chip is the server, so
start it first::

   qemu-system-nds32 -M atbm-hera -icount shift=auto,sleep=on \
       -display none -kernel iot.out \
       -chardev socket,id=hl,path=/tmp/hera.link,server=on,wait=off \
       -global atbm-hera-link.chardev=hl \
       -chardev socket,id=hu,path=/tmp/hera.uart,server=on,wait=off \
       -serial chardev:hu -serial stdio
   qemu-system-mipsel -M ingenic-t23 ... \
       -chardev socket,id=hl,path=/tmp/hera.link \
       -chardev socket,id=mcu,path=/tmp/hera.uart -serial chardev:mcu \
       -device atbm-hera-link-sdio,bus=msc1,chardev=hl

The card forwards SDIO function 1 and the vendor area of function 0 to
the chip's HIF block, takes the SDIO interrupt from it, and tells it of
CCCR and FBR writes. The UART between them is a plain chardev socket,
the camera's UART0 to the chip's UART1. ``power-pin``, ``reset-pin``,
``from-chip`` and ``to-chip`` (pins as ``32 * port + bit``, chip pads as
the chip's GPIO numbers) wire the camera's GPIO block to the chip's power,
reset and pads, and ``pad<N>=0|1`` drives a chip pad from the start or with
``qom-set``; the SDK's SDIO firmware sets its SDIO up only once the pad
numbered 24 is driven high (its wake level, in the firmware's GPIO
numbers). The same lines are named gpios of the card. The protocol
is in ``include/hw/misc/atbm-hera-link.h``.

The board models no radio: the firmware's lower MAC drives hardware with
no public description. For Wi-Fi, link the firmware with the stand-in in
``contrib/atbm-hera-lmac/``, which hands its WSM messages to the board's
paravirtual LMAC port; the port takes the next ``-nic`` and answers as a
simulated access point (its ``ssid`` and ``psk`` properties).
Its README says how to build it from the chip's SDK.

Run the chip with ``-icount shift=auto,sleep=on``: without it the chip's
time follows the host's, and its watchdog resets it whenever its process
falls a few seconds behind, which a loaded host does to it. The camera
runs in real time. The ATBM6461 pairs the same way, ``-M atbm6461`` with
the chip's flash image as ``-drive if=mtd``; the host driver needs no
pad numbered 24 there.

When the chip resets, its own firmware's watchdog or reboot included, the
board of a real camera cuts the camera's power with it, and the camera's
driver has no way to follow the chip that restarted under it. The chip
tells the card (the reasons are a reset the firmware asked for and a
watchdog's last stage), and ``on-chip-reset`` says what the card does:

``ignore``
   nothing (the default): the camera runs on, as it did before the
   message existed
``card-reset``
   the card leaves the bus, starts over, and comes back after
   ``reinsert-ms`` (200) of virtual time, with the host's card-detect
   line moving, and ``cd-pin`` (a camera pin, low while the card is in)
   following it; the kernel must watch that line to enumerate it again
``soc-reset``
   the whole camera resets and boots again, as the real board does

A chip that reboots again and again starts each time as from power-on,
but for its flash, which keeps what the firmware writes: the 6461's
saves its configuration there and then runs another way, which ends in a
crash without the BLE core. ``-global atbm-hera-nor.restore-on-reset=on``
puts the flash back as it was loaded at every reset.

The chip's first word after it starts is not acted on in any mode. A
camera or a chip of an older build ignores or is not sent the message
(it is sent only to a card that says in its hello that it takes it).

The chip sleeps the way its SDK has it sleep: the firmware's idle hook
puts it in light sleep (the CPU and the timers off, the memories, the
GPIOs and the Wi-Fi association kept) until an RTC alarm, a wake pin or
a frame for it wakes it, and it then resumes through its own retention
code; ``AT+DEEP_SLEEP`` powers it down to boot again through the ROM and
its bootloader. A sleeping chip uses next to no host CPU. With the
stand-in lower MAC the port holds what the access point sends while the
chip sleeps and wakes it for a data frame. ``-machine pir-pad=<pad>``
gives the chip a PIR on that pad, whose level is the machine's ``pir``
property (``qom-set /machine pir true``); ``charger-pad`` and ``charger``
are the same for a charger's detect line. On the camera's card,
``host-power-pad=<pad>`` makes that chip pad the camera's power switch,
as the SDK's SDIO demo can use its pad 25: low stops the camera, high
boots it again. The power controller's registers, and what is
inferred about them, are described in ``hw/misc/atbm-hera-smu.c``.

Example - boot a flash image::

   qemu-system-mipsel -M ingenic-t31 -m 256M -display none \
       -serial null -serial stdio \
       -drive file=<flash_image>.bin,format=raw,if=none,id=flash0

A uImage given with ``-kernel`` boots directly, unpacked first when it is
gzip or LZMA compressed as thingino's kernels are, and handed over as
thingino's U-Boot hands it: without ``-append``, the command line is the
one the flash image's U-Boot environment builds, or else thingino's default
for the part and RAM size (``mem=``, ``rmem=`` and the console). An
``-initrd`` goes at the top of the kernel's memory, with ``rd_start=``
and ``rd_size=`` added to the command line::

   qemu-system-mipsel -M ingenic-t31 -m 128M -display none \
       -serial null -serial stdio -kernel uImage -initrd rootfs.cpio

The UARTs send and receive at the line rate the guest programs. With
``-global ingenic-uart.line-rate=off``, bytes move at once.

Example - a T31 camera with its GC2053 sensor, its RTSP port forwarded
to the host::

   qemu-system-mipsel -M ingenic-t31 -m 128M -display none \
       -serial null -serial stdio \
       -drive file=<flash_image>.bin,format=raw,if=none,id=flash0 \
       -netdev user,id=n0,hostfwd=tcp::8554-:554 \
       -device image-sensor,model=gc2053,bus=i2c0

.. include:: cpu-models-mips.rst.inc

.. _nanoMIPS-System-emulator:

nanoMIPS System emulator
~~~~~~~~~~~~~~~~~~~~~~~~

Executable ``qemu-system-mipsel`` also covers simulation of 32-bit
nanoMIPS system in little endian mode:

-  nanoMIPS I7200 CPU

Example of ``qemu-system-mipsel`` usage for nanoMIPS is shown below:

Download ``<disk_image_file>`` from
https://mipsdistros.mips.com/LinuxDistro/nanomips/buildroot/index.html.

Download ``<kernel_image_file>`` from
https://mipsdistros.mips.com/LinuxDistro/nanomips/kernels/v4.15.18-432-gb2eb9a8b07a1-20180627102142/index.html.

Start system emulation of Malta board with nanoMIPS I7200 CPU::

   qemu-system-mipsel -cpu I7200 -kernel <kernel_image_file> \
       -M malta -serial stdio -m <memory_size> -drive file=<disk_image_file>,format=raw \
       -append "mem=256m@0x0 rw console=ttyS0 vga=cirrus vesa=0x111 root=/dev/sda"
