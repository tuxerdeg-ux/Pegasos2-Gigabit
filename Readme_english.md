# Pegasos II Gigabit Driver

If you find this project useful, voluntary donations are appreciated:
[PayPal](https://paypal.me/cfoltin). Donations are, of course, not required
to use or redistribute the software.

This project contains an AmigaOS 4.1 SANA-II device for Ethernet port 1 of the
Marvell MV64360/MV64361 in the Pegasos II. The hardware layer manages
transmit and receive rings and communicates with the controller and PHY using
polling. The unit process polls the port at short intervals (500 microseconds
by default).

## Requirements

- Pegasos II with AmigaOS 4.1 and Marvell MV64360/MV64361
- PowerPC AmigaOS cross-compiler `ppc-amigaos-gcc` and AmigaOS SDK/NDK
- GNU Make

The hardware layer accesses the controller registers directly and requires
DMA-capable memory. During initialization, the device maps the register range
with the required memory attributes. Hardware and network tests are intended
for a suitable Pegasos II system, not a standard PC.

## Building

Run these commands from the project directory:

| Command | Result |
| --- | --- |
| `make dev` | Creates `mv643xx_log.device` in the project directory. This variant writes diagnostic messages to `RAM:mv643xx.log`. |
| `make build` | Creates `build/mv643xx.device` without a file logger. The `build/` directory is created if needed. |
| `make s2test` | Builds the SANA-II test client `s2test`. |
| `make hwtest` | Builds the hardware diagnostic program `pegasos2-gbit-info`. |
| `make all` | Builds `hwtest`, `dev`, and `s2test`. |
| `make clean` | Removes object files and generated programs/devices, including `mv643xx_log.device` and `build/mv643xx.device`. |

`make dev` and `make build` produce differently named device files with
different logging behavior. Before installation, the selected file must be
named `mv643xx.device`, which is the device's internal name. Rename the
logging-enabled build when copying it into place.

## Using the Device

The device is named `mv643xx.device` and provides unit 0 (Ethernet port 1).
Install it where AmigaOS or the TCP/IP stack in use can find SANA-II devices.
Select `mv643xx.device` with unit 0 in the network configuration. The exact
setup depends on the AmigaOS networking software being used. An installation
guide for AmigaOS 4.1 is available in [Install.md](./Install.md).

A typical client workflow is:

1. Call `OpenDevice("mv643xx.device", 0, ...)` with a complete `IOSana2Req`
   and the required buffer-management callbacks.
2. Configure the interface with `S2_CONFIGINTERFACE`.
3. Start the hardware with `S2_ONLINE`.
4. Submit receive requests with `CMD_READ` and send frames with `CMD_WRITE`,
   `S2_BROADCAST`, or `S2_MULTICAST`.
5. Stop the device with `S2_OFFLINE` and close it.

The device registers the implemented SANA-II command set. Not all SANA-II
functions are fully implemented: for example, `S2_GETTYPESTATS` currently
returns `S2ERR_NOT_SUPPORTED`; `S2_READORPHAN` and statistics queries are also
limited.

## Tests and Diagnostics

### SANA-II Test Client

`s2test` runs basic open/close, query, configuration, online/offline, and
request-abort tests. Usage:

```text
s2test [device name or path]
```

Without an argument, `mv643xx.device` is used. The buffer callbacks in this
test program are placeholders: they report success but do not copy payload
data. Therefore, the client primarily tests device commands and request
handling; it is not a reliable test of actual network traffic.

### Hardware Diagnostic Program

`pegasos2-gbit-info` is a separate diagnostic program for registers, memory
mapping, the PHY, and hardware tests. It always performs a register and PHY
diagnostic pass and writes to `RAM:pegasos2-gbit.log` by default. Options:

| Option | Function and effect |
| --- | --- |
| `-log <path>` | Writes the log to the specified file instead of `RAM:pegasos2-gbit.log`. |
| `-rxstoptest` | Stops RX queue 0 on port 1, logs queue registers and the descriptor pointer, then attempts to start the queue again. Run only if the device did not open successfully; this briefly changes the hardware state. |
| `-txprep` | Prepares a DMA buffer and sample TX descriptor and logs them. According to the program code, it does not start the Ethernet port or transmit a frame. |
| `-txtest JA` | Runs a TX test that sends one ARP packet. It may start the Ethernet port and temporarily changes its registers. |
| `-rxtest JA` | Runs an RX test that receives up to four frames. It actively accesses the RX hardware. |
| `-echotest JA` | Runs a 20-second echo/ARP test for `192.168.1.80` and may send ARP replies. |
| `-hwtest JA` | Starts the hardware-layer self-test. It runs for up to 20 seconds, checks the link, and responds to ARP requests for `192.168.1.80`. |
| `-dsoff` | Experiment: changes PHY register 20 (bit 11) and initiates a PHY reset/autonegotiation. This changes the PHY state. |
| `-g1000` | Experiment: configures the PHY advertisement for 1000BASE-T full duplex and restarts autonegotiation. This changes the PHY state. |
| `-restore` | Writes hard-coded values to PHY registers 4, 9, 20, and 0, then restarts autonegotiation. This may not restore individually customized previous PHY settings. |
| `-ip <address>` | Currently parsed but not used. The echo tests use the fixed address `192.168.1.80`. |

Examples:

```text
pegasos2-gbit-info
pegasos2-gbit-info -log RAM:port1-test.log
pegasos2-gbit-info -txtest JA
pegasos2-gbit-info -log RAM:rx-test.log -rxtest JA
```

The TX, RX, echo, and hardware tests actively access the Ethernet port.
`-dsoff`, `-g1000`, and `-restore` change PHY settings. Run these options only
deliberately, individually, and on suitable hardware. Do not combine PHY
experiments casually; `-restore` writes hard-coded values and is not a general
way to restore the individually customized previous state.

## Project Structure

```text
.
├── device/
│   ├── init.c, dev_unit.c, dev_io.c, dev_cmd.c
│   ├── dev_log.c, dev_log_stub.c
│   ├── dev_hold.h, dev_wait.h
│   └── mv643xx.h, mv643xx.device_rev.h
├── src/
│   ├── mv64361.c, mv64361.h, mv64361_test.c
│   ├── pegasos2-gbit-info.c
│   └── s2test.c
├── build/                 # output of make build
├── Makefile
├── LICENSE
├── Install.md
├── Readme_german.md
└── Readme_english.md
```

`obj/` contains the object files generated during builds. Depending on the
Make target, device files and test programs are placed in the project
directory or under `build/`.

`make dev` creates `mv643xx_log.device`, the build with a file logger. It
writes diagnostic messages to `RAM:mv643xx.log`. Before installation, rename
the file to `mv643xx.device`, which is the device's internal name.

## Known Limitations

- The controller is serviced by polling rather than interrupts.
- Restarting a stalled RX queue, repeated start/stop cycles, and link changes
  during operation have not been tested sufficiently.
- After an AmigaOS restart without a computer reset, the device attempts to
  recover a stale port configuration only if the SDMA and Serial Control
  registers match this driver's known startup configuration. Unknown active
  configurations are not taken over. Recovery appeared to work after a
  Mode 1 restart on Pegasos II hardware; other configurations still need
  verification.
- `S2_GETTYPESTATS` is not supported; other SANA-II statistics and orphan-read
  functions are incomplete.
- Currently, only `configure=dhcp` is supported. With other configurations,
  the required routes are not added reliably.
- The no-logging build (`make build`) discards diagnostic messages. The
  regular `make dev` build writes them to `RAM:mv643xx.log`.

## License

This project is released under the **GNU General Public License, version 2 or
any later version (GPL-2.0-or-later)**. The full text of GPL version 2 is
available in [LICENSE](./LICENSE) and on the [GNU website](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).

When distributing the device or other program files generated from this
project, you must comply with the license terms. In particular, license and
copyright notices must be retained, and the corresponding source code must be
made available under the GPL.

## Trademarks

Amiga, the Boing Ball logo, the stylized A logo, and the checkmark logo are
identified as trademarks of [Amiga Corporation](https://amiga.com/). AmigaOS
and other marks mentioned here belong to their respective rights holders.
This independent third-party driver is not affiliated with, endorsed by, or
presented as an official product of Amiga Corporation or Hyperion Entertainment
BV. AmigaOS is not included with this driver and must be obtained separately.

I ask Hyperion Entertainment BV to obtain my prior written consent before
including this driver in an AmigaOS package distributed by Hyperion. This
request does not restrict the redistribution rights of other recipients under
the GPL.

If you find this project useful, voluntary donations are appreciated:
[PayPal](https://paypal.me/cfoltin). Donations are, of course, not required
to use or redistribute the software.
