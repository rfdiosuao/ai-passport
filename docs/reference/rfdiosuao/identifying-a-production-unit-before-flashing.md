<p align="right">
  <a href="identifying-a-production-unit-before-flashing.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Identify a Production Unit and Its Shipped Layout Before Flashing

Captured while preparing a retail AI Passport (ESP32-C3, 8 MB, native
USB-Serial-JTAG) for custom firmware from Windows. The unit arrived running the
shipped **AppStore-AI-Passport** firmware with a downloaded play sitting in the
OTA slot. This entry records what a read-only identification pass actually
revealed, and why the shipped partition table makes a merged `0x0` flash more
destructive on a retail unit than the layout document's wording suggests.

Everything below was observed with `esptool` 5.4.0 against this one unit. No
flash write, build, or erase was performed.

## `esptool` 5 renamed every subcommand

v5 dropped the underscore spelling. The names in nearly every existing tutorial
fail outright:

| v4 (widely documented) | v5 (current) |
| --- | --- |
| `write_flash` | `write-flash` |
| `read_flash` | `read-flash` |
| `erase_flash` | `erase-flash` |
| `flash_id` | `flash-id` |
| `chip_id` | `chip-id` |
| `merge_bin` | `merge-bin` |

`--version` is no longer an option either (`No such option '--version'`); the
subcommand is `esptool version`. Run `esptool --help` once after installing and
work from that list. `skills/passport-device-test/SKILL.md` already asks for
exactly this check, and it is worth doing before copying any command out of a
guide.

## Three read-only commands decide whether the unit is flashable at all

`chip-id`, `flash-id`, and `get-security-info` need nothing but the port. The
third is the go/no-go check that is easy to skip:

```sh
esptool --chip esp32c3 --port COM6 get-security-info
```

On this unit it reported `Secure Boot: Disabled`, `Flash Encryption: Disabled`,
and all six key blocks `USER/EMPTY`. That combination is what makes a self-built
image bootable **and** makes a full read-back a plaintext, restorable copy. Had
either protection been enabled, the custom image would have been rejected at
boot and a read-back would have returned ciphertext — two different workflows,
decided by one command.

Two details that look like errors but are not:

- `chip-id` prints `WARNING: ESP32-C3 has no chip ID. Reading MAC address
  instead.` The ESP32-C3 genuinely has no chip ID register, so the warning is
  expected output, not a failure.
- No BOOT button press was needed. esptool's default reset drives RTS through
  the native USB interface, and the composite device exposes both a CDC serial
  port and a JTAG interface.

## The shipped layout is not the baseline layout

The device's own partition table is the part worth reading before anything
else. Read it with `read-flash 0x8000 0x1000` and parse the 32-byte entries
(magic `0x50AA`). It did not match the repository baseline at all:

| Partition | Shipped unit | Repository baseline |
| --- | --- | --- |
| `nvs` | `0x9000`, 24 KB | `0x9000`, 24 KB |
| `phy_init` | `0xF000`, 4 KB | `0xF000`, 4 KB |
| `factory` (app) | `0x10000`, 3 MB | `0x10000`, `0x7F0000` (all remaining flash) |
| `otadata` | `0x310000`, 8 KB | — |
| `cardid` (data/nvs) | `0x356000`, 16 KB | — |
| `ota_0` (app) | `0x360000`, 3 MB | — |
| `store` (data/nvs) | `0x660000`, 16 KB | — |
| `recovery` (app/test) | `0x700000`, 1 MB | — |

The baseline's `factory` entry reserves `0x10000`–`0x800000`, but a merged file
is only as long as its highest image, so the reserved extent is not the written
extent. That gap changes which shipped partitions actually lose their bytes; the
next section measures it.

`firmware-layout.md` says a merged flash "can reset the NVS and PHY data
regions", and that is precisely right — the write covers `nvs` and `phy_init`
completely. The shipped `nvs` region was about 85% non-blank, so whatever the
product stored there is gone. What the sentence does not mention is the
partition table itself, which is replaced along with the bootloader and
application. The policy also states plainly that the original firmware cannot be
promised restorable.

## `image-info` tells you what you are about to erase

Both application partitions parse cleanly, and the identity fields are worth
capturing as the "before" evidence:

- `factory`: project `AppStore-AI-Passport`, version 1, ESP-IDF v5.5.3, checksum
  and validation hash both valid.
- `ota_0`: project `ai-passport-pinball`, version `6291c82`, ESP-IDF v5.5.5,
  checksum and validation hash both valid.

The unit was therefore not a blank board. It shipped with the product
application and carried a downloaded play in the OTA slot. Both are destroyed by
a merged flash from `0x0`, and neither exists in this repository.

## Measure the merged file instead of assuming its footprint

The artifact built from this repository is **1,589,824 bytes (`0x184240`)**, so a
`write-flash 0x0` covers `0x0`–`0x184240`: 18% of the 8 MB part. Everything past
that offset keeps its bytes.

| Shipped partition | Range | Bytes written | In the new partition table |
| --- | --- | --- | --- |
| `nvs` | `0x9000`–`0xF000` | all | yes |
| `phy_init` | `0xF000`–`0x10000` | all | yes |
| `factory` | `0x10000`–`0x310000` | head only | yes |
| `otadata` | `0x310000`–`0x312000` | none | no |
| `cardid` | `0x356000`–`0x35A000` | none | no |
| `ota_0` | `0x360000`–`0x660000` | none | no |
| `store` | `0x660000`–`0x664000` | none | no |
| `recovery` | `0x700000`–`0x800000` | none | no |

Byte overwrite and layout replacement are separate effects, and conflating them
gives the wrong recovery story:

- **Bytes:** only `nvs`, `phy_init`, and the head of the application region are
  rewritten. The downloaded play in `ota_0`, the OTA state, and both additional
  data partitions still hold their contents.
- **Layout:** the write also installs this repository's three-entry partition
  table, so all five of those partitions become unaddressable. The unit boots the
  baseline as a bare board with no OTA slot and no product data, even though the
  bytes are still present.

For recovery that distinction is the whole point: because the bytes survive, a
restore from the 8 MB read-back returns the unit to its shipped state. Had the
merged image filled the reserved range, that would not hold.

## Validate the output path before reading 8 MB

`read-flash` streams the whole requested range and only opens the output file
after the transfer finishes. A single failed `mkdir` in the host shell was
enough to lose a completed 52-second read at the last step:

```text
FileNotFoundError: [Errno 2] No such file or directory: 'backup/passport-8MB.bin'
```

Create and confirm the directory first, prefer an absolute output path, and add
`--no-progress` to keep the log readable. The re-run took 43 seconds at 921600
baud, so the mistake cost about a minute of wall clock and a confusing
traceback — not data.

## A full read-back is cheap insurance even though it is not a prerequisite

The flashing policy explicitly does not require a firmware backup, and that
holds when provisioning a blank board. On a retail unit it is still the only
rollback that will ever exist, and here it is neither slow nor expensive: 8 MB
in 43–53 seconds at 921600 baud over USB-Serial-JTAG, plaintext because flash
encryption is off.

Cross-check the result instead of trusting the transfer blindly. Reading
`0x8000`–`0x9000` separately and comparing those bytes against the same offset
inside the full dump is a one-second check that the read is faithful.

## An unconfirmed partition worth asking about

`recovery` is declared as `app/test` at `0x700000` (1 MB) but contains no valid
image header: the first 64 KB is a repeating `fe 02` pattern and no `0xE9`
application magic appears anywhere in it. `firmware-layout.md` only describes
the default layout, so the shipped meaning of this partition was not determined
here. Treat it as occupied until upstream confirms otherwise.

## Observing the console afterwards

Opening the CDC port does not reset this board, and pulsing RTS from `pyserial`
alone does not reset it either. A monitor that only opens the port therefore
prints nothing, which looks exactly like a wrong port or a dead device. What
worked was letting `esptool` perform the reset and opening the port immediately
afterwards, in the same script and with no sleep in between:

```sh
esptool --chip esp32c3 --port COM6 --after hard-reset flash-id   # then open COM6 at once
```

The port and the composite device identity are unchanged after flashing
(`VID_303A&PID_1001&MI_00`), so nothing has to be rediscovered.

Asserting DTR and RTS together is *not* a reset: it puts the chip into download
mode, where it stays indefinitely.

```text
rst:0x15 (USB_UART_CHIP_RESET),boot:0x6 (DOWNLOAD(USB/UART0))
waiting for download
```

A terminal or monitor that drives both lines can therefore hold the board out of
its application, and the symptom is again "no logs". Recover with a normal
`esptool` reset rather than more line toggling.

`verify-flash` after a reset is a cheap persistence check that the write
survived, independent of any log:

```sh
esptool --chip esp32c3 --port COM6 --after hard-reset verify-flash 0x0 <merged.bin>
# Verification successful (digest matched).
```

Once the board is running, its own console output is the most useful acceptance
evidence available over the port, because the baseline names every peripheral it
brings up. On this unit the log reached `Ready: Display=1 Button=1 Audio=1
Battery=1`, with `ES8311` at I2C `0x18`, `CW2017` at `0x63`, the 240×320 panel,
and the button ADC ladder all reporting ready — none of which a build or a
flashed hash can tell you.

## Takeaways

- Check the installed `esptool`'s own help first; v5 renamed every subcommand.
- Run `get-security-info` before planning anything. Secure Boot and Flash
  Encryption turn "flash custom firmware" into two different projects.
- `chip-id`'s "no chip ID" warning on ESP32-C3 is normal output; the MAC is the
  answer.
- Read the device's partition table before assuming the repository baseline
  applies, and measure the merged file's length. The written range ends at the
  last image, not at the end of the reserved `factory` partition.
- Keep byte overwrite and layout replacement apart. A merged image can leave a
  shipped partition's bytes intact and still make it unreachable, because the
  partition table it installs is shorter than the one already on the device.
- The `Build firmware` workflow has a `workflow_dispatch` trigger, so a fork can
  produce the merged image without installing ESP-IDF locally; the artifact is
  `FoloToy-AI-Passport-full.bin`.
- A CDC port on this SoC does not reset on open, and single-line pulses may not
  either. Reset with the flasher and attach immediately to see a boot log.
- DTR and RTS asserted together mean download mode, not reset. A monitor can
  silently hold the board out of its application.
- Reflash from `0x0` is fast (1.5 MB in about 5.5 s at 921600 baud), so the
  useful cost of a test cycle is the console capture, not the write.
- Use `image-info` on the shipped app partitions to record what is being
  replaced; project name, version, and build time are enough to identify it.
- Create and verify the output directory before an 8 MB read, because the file
  is only written at the end of the transfer.
- A read-back is optional by policy and still worth doing on a retail unit; it
  is the only way back to the shipped firmware and its data.
- Take a host device-enumeration baseline *before* plugging the target in, so a
  later diff can actually show that it appeared.

## What was and was not verified

- Device identification, flash identification, security state, partition table,
  and an 8 MB read-back: performed on the unit described above.
- Firmware build: **run** through the fork's GitHub Actions run of
  `Build firmware` (`workflow_dispatch`), producing a 1,589,824-byte merged image
  with `SHA-256 2820b28766cf5967f7d1e2e4783ee503a46e5a1df339ca2d5c8a8779ba5a6e94`.
  Nothing was built on a local host.
- Flash write: **run** after explicit authorization — `write-flash 0x0` of the
  merged image, followed by `verify-flash` (digest matched) and a partition-table
  re-read confirming the baseline's three entries.
- Boot and on-board bring-up: **observed** on the unit — the application boots
  from `0x10000` and reports Display/Button/Audio/Battery ready.
- On-device rendering, audio output, button behaviour, RF performance, or sleep
  current: **not measured**; the log shows initialization, not quality.

## Related

- `docs/development/engineering/firmware-layout.md` — the layout and flashing
  policy this entry measured against.
- `docs/reference/y2lin/serial-screenshot-protocol.md` — the USB-serial-JTAG
  driver traps to expect once custom firmware is running.
- `skills/passport-device-test/SKILL.md` — the authorization and verification
  workflow that gates the flash write.
