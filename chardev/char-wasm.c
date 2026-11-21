/*
 * Byte input for chardevs from JavaScript on Emscripten hosts.
 *
 * poll() does not work on the QEMU main thread under Emscripten, so a
 * chardev cannot wait on an input fd. The embedder instead writes the
 * id of any chardev (for example one made with
 * "-chardev file,id=uart0,path=/dev/uartout") into
 * qemu_wasm_chr_idbuf(), the bytes into qemu_wasm_chr_inbuf(), and calls
 * qemu_wasm_chr_input(). The bytes reach the chardev's front end as if
 * the backend had read them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "chardev/char.h"
#include <emscripten.h>

static uint8_t input_buf[4096];
static char id_buf[64];

/* The chardev id is passed through a static buffer, like the data. */
EMSCRIPTEN_KEEPALIVE char *qemu_wasm_chr_idbuf(void)
{
    return id_buf;
}

EMSCRIPTEN_KEEPALIVE uint8_t *qemu_wasm_chr_inbuf(void)
{
    return input_buf;
}

/* Deliver up to @len bytes of the input buffer; returns the number taken. */
EMSCRIPTEN_KEEPALIVE int qemu_wasm_chr_input(int len)
{
    Chardev *chr;
    int done = 0;

    len = MIN(len, (int)sizeof(input_buf));
    bql_lock();
    chr = qemu_chr_find(id_buf);
    while (chr && done < len) {
        int room = MIN(qemu_chr_be_can_write(chr), len - done);

        if (room <= 0) {
            break;
        }
        qemu_chr_be_write(chr, input_buf + done, room);
        done += room;
    }
    bql_unlock();
    return done;
}
