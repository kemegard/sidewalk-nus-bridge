# sidewalk-nus-bridge

Amazon Sidewalk and the Nordic UART Service (NUS) running side by side on one Bluetooth LE stack,
on an nRF54L15 DK.

The application is the Sidewalk add-on `sid_end_device` sample (hello variant) with NUS added.
It shows that Sidewalk BLE and a regular BLE peripheral service can share the stack, and how to
handle the conflicts (identities, advertising sets, connection callbacks, GATT access).

| From                       | To                                                                         |
|----------------------------|----------------------------------------------------------------------------|
| Command written by NUS peer | Handled on the device, reply over NUS (see [Commands over NUS](#commands-over-nus)) |
| Any other NUS input        | Ignored (not sent to Sidewalk), reply with the list of allowed commands    |
| Sidewalk downlink          | NUS notification, mirrored on NUS UART                                     |
| Line typed on NUS UART     | NUS notification                                                           |

Logs stay on the console UART (`uart20`). NUS traffic is mirrored on the DK's second virtual COM
port (`uart30`) with the tags `[nus]` (input from the NUS peer), `[cmd]` (replies),
`[sidewalk->nus]` and `[uart->nus]`. The NUS link does not use pairing.

## Commands over NUS

The NUS peer can only send these commands. Any other input, including unknown `!` commands and
empty lines, is ignored and answered with `ignored, allowed commands: !status !mtu !ping !help`.
Nothing written over NUS is forwarded to Sidewalk; the only uplink is the one `!ping` sends.

| Command   | Reply                                                                                       |
|-----------|---------------------------------------------------------------------------------------------|
| `!help`   | `commands: !status !mtu !ping !help`                                                        |
| `!status` | Sidewalk state, registration, time sync, and BLE / FSK / LoRa link up or down               |
| `!mtu`    | Maximum payload size per enabled link (`sid_get_mtu()`)                                     |
| `!ping`   | Sends `ping` as an uplink with an ack requested (3 retries, 60 s), then reports `queued`, `sent`, and `ack received` or `failed <error>` |

Replies come back as NUS notifications and are mirrored on the NUS UART as `[cmd] ...`.
If Sidewalk isn't running, `!status`, `!mtu` and `!ping` say why, for example
`status: sidewalk not started (not provisioned, see log for the guide)`.
Commands are case sensitive. A trailing CR/LF or space from the terminal app is ignored.

`!ping` checks the whole path: phone → device → Sidewalk gateway → AWS IoT and back. One ping can
be outstanding at a time. Ping replies arrive asynchronously, after the command's first reply.

## Prerequisites

Hardware

- nRF54L15 DK (`nrf54l15dk/nrf54l15/cpuapp`)
- For Sidewalk traffic: a Sidewalk gateway in range (for example a recent Echo or Ring device with Sidewalk enabled)
- A NUS central: phone with nRF Connect for Mobile or nRF Toolbox, or a PC with Bluetooth LE for the test script

Software

- nRF Connect SDK **v3.4.0** toolchain (`nrfutil sdk-manager install v3.4.0`)
- Amazon Sidewalk add-on **v1.3.0-add-on** as its own west workspace:

  ```
  west init -m https://github.com/nrfconnect/sdk-sidewalk --mr v1.3.0-add-on sidewalk-workspace
  cd sidewalk-workspace
  west update --narrow --fetch-opt=--depth=1
  ```

- nRF Util with the `device` command (for flashing)
- Optional, for the host test: Python 3 with `bleak` and `pyserial`

Sidewalk credentials

- An AWS account, and the device onboarded in AWS IoT Core for Amazon Sidewalk.
  Without credentials Sidewalk does not start (the log points to the provisioning guide), but NUS still works.

## Build and flash

Run west from inside the Sidewalk workspace so the add-on modules are found:

```
cd sidewalk-workspace
west build -b nrf54l15dk/nrf54l15/cpuapp -d <path-to-this-repo>/build <path-to-this-repo>
west flash -d <path-to-this-repo>/build --erase     # first flash only
```

`--erase` on the first flash is required. It erases the chip and writes the MCUboot verification key
(`build/keyfile.json`) to the KMU. Without the key MCUboot cannot verify the application, and because
MCUboot logging is off the board stays silent. Later flashes don't need `--erase`.

On Windows, [build.ps1](build.ps1) does the same (`-Toolchain`, `-Workspace`, `-Pristine`, `-Flash -Snr <J-Link serial> [-Erase]`).

## Provision Sidewalk

Follow [Setting up your Sidewalk prototype](https://nrfconnectdocs.nordicsemi.com/addons/addon-sidewalk/latest/setting_up_sidewalk_environment/setting_up_sidewalk_prototype.html).
With the build directory of this project:

```
west sid provision -d <path-to-this-repo>/build -C certificate.json
```

The manufacturing data partition (`mfg_storage`) is at `0x17c000` on the nRF54L15.

## How the BLE conflicts are handled

| Conflict                      | Resolution                                                                                |
|-------------------------------|-------------------------------------------------------------------------------------------|
| Second `bt_enable()`          | `sid_ble_bt_enable()` (reference counted), called on the Sidewalk thread (counter has no lock) |
| Advertising sets              | NUS uses its own extended advertising set (legacy PDUs); `BT_EXT_ADV_MAX_ADV_SET` +1       |
| Connections                   | `BT_MAX_CONN` +1 (3 with Sidewalk DFU)                                                    |
| Identity                      | NUS uses `BT_ID_DEFAULT` (0); Sidewalk uses 1, SMP DFU uses 2                             |
| Connection callbacks          | NUS handles only connections whose `bt_conn_get_info()` identity is 0                     |
| GATT visible on every link    | Authorization callback: NUS only on identity 0; Sidewalk and SMP rejected on identity 0   |
| Console UART                  | Logs on `uart20`; NUS UART is `uart30` (`nordic,nus-uart`), disabled again for MCUboot   |

Code: [src/nus_bridge/nus_bridge.c](src/nus_bridge/nus_bridge.c), hooks in [src/hello/app.c](src/hello/app.c),
Kconfig option `SID_END_DEVICE_NUS_BRIDGE` in [Kconfig](Kconfig).

## Host test

[tools/nus_test.py](tools/nus_test.py) uses the PC's Bluetooth adapter as the NUS central:

```
python tools/nus_test.py --uart <COM port of the NUS UART> [--provisioned]
```

It checks UART → NUS, the replies to `!help`, `!status`, `!mtu` and `!ping`, that other input
(an unknown `!` command, plain text, an empty line) gets the list of allowed commands, the mirroring
on the NUS UART, that Sidewalk and SMP characteristics are rejected on the NUS link, and that
advertising restarts after a disconnect.
Without `--provisioned` it expects the "sidewalk not started" replies.

## Status

Tested on an nRF54L15 DK without Sidewalk credentials: the NUS commands (including their
"not started" replies), ignoring other input, UART → NUS, GATT isolation, and repeated reconnects
work (21/21 host test checks). **Not yet tested:** `!status` / `!mtu` / `!ping` on a running Sidewalk
stack (including the ping ack), Sidewalk downlinks to NUS, and a Sidewalk gateway connected at the
same time as a NUS peer.

## License

[LicenseRef-Nordic-5-Clause](LICENSE), as the Sidewalk add-on sample this is based on.
