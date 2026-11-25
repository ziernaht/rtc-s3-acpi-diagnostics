# Architecture and execution flow

This document describes the implementation in `rtc_s3_sleep.c` and the public interface in `rtc_s3_sleep.h`. It is a reading guide, not a claim that every firmware implementation follows the same path.

## High-level flow

```text
RSDP
  └─ RSDT/XSDT
       └─ FADT
            ├─ PM1 event/control register information
            ├─ RTC alarm register indices and platform flags
            └─ DSDT
                 └─ static _S3_ package (sleep types)

rtc_s3_init()
  └─ validate and copy configuration

rtc_s3_dry_run()
  ├─ validate RTC and current time
  ├─ disarm old alarm state
  ├─ calculate and program alarm
  ├─ clear and verify stale event status
  ├─ enable RTC alarm + PM1 RTC event
  ├─ poll RTC Reg C.AF and PM1_STS.RTC_STS
  └─ disarm alarm state before returning

configure_rtc_s3_sleep()
  └─ shares the arming sequence, then attempts the SLP_TYP/SLP_EN transition
```

## Configuration discovery

`rtc_s3_config_from_acpi()` starts from a physical RSDP address and an optional physical-to-virtual mapping callback. With a null callback, the code assumes identity mapping.

The discovery path looks for an RSDT or XSDT, locates the FADT, obtains the PM1 blocks and related firmware fields, then scans the DSDT for a static `_S3_` package.

The AML scan is intentionally limited. It recognizes a common static package containing constant byte values; it is not an AML interpreter and cannot evaluate methods or computed objects. The FADT handling supports the I/O-port PM1 registers this implementation needs and rejects unsupported memory-mapped-only PM1 layouts.

On discovery failure, the output configuration may be partially populated. Do not pass it to `rtc_s3_init()` after a non-zero discovery return code.

## Shared RTC/PM1 arming sequence

The dry-run and real-S3 paths share the setup logic:

1. Validate arguments and the stored configuration.
2. Check RTC validity and ensure the clock is not held in SET mode.
3. Disable any previous RTC alarm and PM1 RTC event-enable state.
4. Read the current time with UIP guarding and repeated reads.
5. Calculate the target time and encode it for the RTC's BCD/binary and 12/24-hour mode.
6. Program the seconds/minutes/hours alarm and optional day/month alarm registers.
7. Read RTC Reg C and clear stale PM1 RTC/wake status bits, then verify the PM1 status bits cleared.
8. Enable the RTC alarm interrupt and PM1 RTC event, reading back the PM1 enable state.

The code records diagnostic state in `g_rtc_s3_dbg`. The logger callback receives complete lines without a trailing newline. The optional loader hook adapts this to a polled UART.

## Dry-run path

`rtc_s3_dry_run()` accepts delays from 3 to 120 seconds. It polls the PM1 RTC status bit and RTC Reg C.AF for the expected interval plus a grace period. It never writes the PM1 control register's `SLP_EN` bit and does not enter S3.

It is still a hardware I/O operation. In particular, it changes alarm and event-enable state, reads the read-to-clear RTC Reg C register, and clears stale PM1 status flags. “Dry run” means “do not request the S3 sleep transition”; it does not mean “perform no register writes.”

## Real-S3 path

`configure_rtc_s3_sleep()` accepts delays from 3 to 86,399 seconds and attempts the PM1 sleep-control transition after arming the alarm. The implementation may execute cache and interrupt-control instructions as part of this sequence.

The code does not perform the full ACPI/firmware/platform suspend and resume protocol. It does not replace the loader's responsibility for device quiescing, CPU coordination, FACS waking-vector setup, context preservation, or restoration after resume. Calling the real-S3 path without that surrounding implementation can leave the machine unable to resume normally.

## Host simulation boundary

The host tests compile the implementation with `RTC_S3_HOST_SIM` and replace port I/O and CPU helpers with a deterministic simulated RTC/PM1 model. The synthetic ACPI tables exercise supported parsing cases and selected malformed/unsupported inputs.

This validates code paths against the model. It does not validate actual motherboard wiring, firmware behavior, chipset quirks, physical wake signals, or S3 resume.

## Key source locations

- Public API and result codes: `rtc_s3_sleep.h`
- RTC/PM1 implementation and ACPI discovery: `rtc_s3_sleep.c`
- Core model and ACPI tests: `tests/test_rtc_s3_sim.c`
- UART hook and hook tests: `shellcode_hv/rtc_s3_hook.c`, `tests/test_rtc_s3_hook_sim.c`
- VivaLaVent-specific example patch: `integrations/vivalalvent/rtc-s3-selftest-hook.diff`
