<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# HPM RISC-V RTT support

The generic OpenOCD RTT implementation uses `target_read_buffer()` and
`target_write_buffer()` and therefore does not require an architecture-specific
transport. The HPM RISC-V target already provides those memory operations, but
its command table did not register `rtt_target_command_handlers`.

This tree registers the generic RTT command chain for the RISC-V target. It
also retains an optional intrusive fallback for targets without working
run-time memory access:

```text
<target> rtt setup <address> <size> [ID]
<target> rtt start
<target> rtt stop
<target> rtt channels
<target> rtt polling_interval [interval]
<target> rtt halt_polling [on|off]
```

## Non-intrusive HPM6750 configuration

HPM6750 implements RISC-V Debug Module System Bus Access v1 with a 32-bit
address bus and 8/16/32-bit transfers. Configure OpenOCD as follows:

```tcl
riscv set_mem_access sysbus
riscv virt2phys_mode off
hpm6750.cpu0 rtt halt_polling off
```

The `virt2phys_mode off` setting is essential for this bare-metal target.
Otherwise a normal `target_read_buffer()` first tries to inspect privilege and
page-table state, which fails while the core is running before SBA is reached.

RTT storage must be mapped non-cacheable so SBA and the CPU observe coherent
control fields and ring-buffer data. `halt_polling` is disabled by default and
must remain off for non-intrusive HPM6750 RTT.

## macOS build

```sh
./bootstrap
mkdir -p build-rtt-native
cd build-rtt-native
env CC=/usr/bin/clang CXX=/usr/bin/clang++ CCACHE_DISABLE=1 \
  CCACHE_TEMPDIR=/private/tmp/openocd-hpm-rtt-ccache \
  ../configure \
    --prefix="$(cd .. && pwd)/local-rtt" \
    --enable-cmsis-dap \
    --enable-cmsis-dap-v2 \
    --enable-internal-jimtcl \
    --disable-werror \
    --disable-amtjtagaccel
env CCACHE_DISABLE=1 \
  CCACHE_TEMPDIR=/private/tmp/openocd-hpm-rtt-ccache \
  make -j8 install
```

`--disable-amtjtagaccel` avoids building the Linux parallel-port driver on
macOS. The installation remains local to the source tree and does not replace
the SDK-provided OpenOCD.

## HPM6750 validation checklist

- Start RTT while the target is running and confirm it stays running.
- Exercise both target-to-host logging and host-to-target shell input.
- Confirm that stopping RTT releases the CMSIS-DAP interface.
- Flash immediately after leaving the RTT client without reconnecting USB.
- Run sustained logging long enough to detect stale SBA or cache data.
