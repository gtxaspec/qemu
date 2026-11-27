# atbm-hera-lmac: a lower MAC for the ATBM6441 firmware under QEMU

The guest side of the Hera board's paravirtual LMAC port
(`include/hw/net/atbm-hera-lmac.h`, ABI 1). Linked into a firmware built
from the chip's SDK in place of the vendor's lower MAC, it lets that
firmware use Wi-Fi in `qemu-system-nds32 -M atbm-hera`: scan, join an
open or WPA2 network, and carry IP traffic, on its own or for a camera
linked to it over SDIO.

This is source only. QEMU's build and CI never build it: it links the
vendor's libraries, which are not part of QEMU and cannot be shipped
with it.

## Why it exists

The SDK's `libwifi.a` holds both halves of the chip's Wi-Fi stack. The
upper MAC (HMAC) is the chip's mac80211-like layer and its supplicant.
The lower MAC (LMAC) is the WSM firmware of the cw1200 family, and it
drives the radio's MAC, PHY and RF blocks, which have no public
documentation. QEMU cannot model hardware nobody describes, so the board
models no radio. Instead this shim takes the LMAC's place and hands every
WSM message to QEMU, which answers with the WSM firmware model the other
ATBM60xx devices use (`hw/net/atbm-wifi.c`) and a simulated access point
(`hw/net/wifi-ap.c`).

## Where it plugs in

HMAC and LMAC meet in `hi_internal.o`, a set of message rings in memory.
HMAC writes a WSM request into an input buffer the LMAC posted, and the
LMAC takes it in `SYS_HiReceive()`; the LMAC hands confirms and
indications back through `HiInternal_ProgOutput()` and hears in
`SYS_HiIndMsgSent()` when HMAC is done with an indication. The messages
are the cw1200 / Linux ones: `{ le16 len; le16 id; body }`.

The build takes the objects named in `drop.list` out of a copy of
`libwifi.a` (the LMAC, the WSM layer and the MAC/PHY/RF drivers under
it) and adds the shim's:

- `src/lmac_qemu.c`: the LMAC task (`lmac_Main()`), `SYS_HiReceive()`,
  `SYS_HiIndMsgSent()`, the indication buffer pools, and the LMAC globals
  that the kept objects still reach into. Requests go to the port's `REQ`
  register and come back answered in place; indications are popped from
  the port on its interrupt.
- `src/sdio.c`: the SDIO build's direct path, where the camera's data
  frames go straight to the LMAC (`WSM_TransmitReq()`), and the received
  frame split between the camera and the chip's own network stack.
- `src/stubs.c`: the rest of what the LMAC exported (radio, rate and
  power-save tuning) as no-ops, and its settings variables.

`src/lmac_qemu.h` repeats the port's register map: keep it in step with
`include/hw/net/atbm-hera-lmac.h`.

Power save is not modelled: the shim keeps the chip from sleeping.

## Building

You need your own copy of the ATBM6441 SDK (its `sdk/` directory, with
`lib/libwifi.a` and the demo's Makefile) and its `nds32le-elf` toolchain
on `PATH`. The SDK tree is copied into the build directory and built
there; nothing is written next to these sources.

    mkdir build-lmac && cd build-lmac
    make -f <qemu>/contrib/atbm-hera-lmac/Makefile SDK=<sdk dir>
    make -f <qemu>/contrib/atbm-hera-lmac/Makefile SDK=<sdk dir> VARIANT=sdio050

`VARIANT` names the build directory and picks the memory layout: the
default (`iot050`) is the SDK's AT-command demo, any name starting with
`sdio` gets the SDIO demo's layout, for an SDK tree set up for the SDIO
build. `CROSS` sets the toolchain prefix. The result is
`<variant>/build/iot.out`. The SDK's last step, which packs OTA images
with a Windows tool, fails; `iot.out` is made before it.

## Running

The AT demo, alone, with an access point named `qemu`:

    qemu-system-nds32 -M atbm-hera -icount shift=auto,sleep=on \
        -display none -kernel iot050/build/iot.out -nic user \
        -global atbm-hera-lmac.ssid=qemu \
        -global atbm-hera-lmac.psk=<passphrase> \
        -serial null -serial stdio

The second UART is the AT console: `AT+WIFI_SCAN`, then
`AT+WIFI_JOIN_AP KEY_MGMT WPA2_PSK PWD <passphrase> SSID qemu`. The port
takes the next `-nic`; `channel` and `bssid` set the rest of the access
point. Leave out `psk` for an open network.

The SDIO build runs the same way, linked to a camera's QEMU, as
"ATBM6441 with its own firmware" in `docs/system/target-mips.rst`
describes. Run the chip with `-icount shift=auto,sleep=on` there too.

A firmware built without the shim still boots on the board, but its LMAC
finds no radio.
