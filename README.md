# RTC-alarm ACPI S3 utility for bare-metal x86-64 loaders

A small, runtime-configured C implementation for arming the CMOS RTC alarm and checking whether it reaches the ACPI PM1 fixed-event status bit. It includes a non-destructive dry-run mode, an optional real S3 entry path, ACPI table discovery, diagnostic logging, and host-side simulations.

This code was originally developed/tested in the context of a bare-metal x86-64 loader. It is published here as an experimental, standalone reference for firmware, bootloader, and low-level power-management developers to review and adapt.

## What is included

- `rtc_s3_sleep.c`, `rtc_s3_sleep.h` — core RTC/ACPI implementation and public API.
- `tests/test_rtc_s3_sim.c` — host simulation of CMOS RTC, PM1 registers, ACPI tables, dry-run and real-S3 control flow.
- `tests/test_rtc_s3_hook_sim.c` — host test of the optional boot-time UART hook.
- `shellcode_hv/rtc_s3_hook.c`, `shellcode_hv/rtc_s3_hook.h` — example integration hook extracted from the supplied patch.
- `integrations/vivalalvent/rtc-s3-selftest-hook.diff` — original integration patch for the target loader tree; it is not a generic patch for arbitrary projects.

## Capabilities

- Reads RTC time with UIP guarding and repeated reads.
- Handles BCD/binary and 12/24-hour RTC encodings.
- Programs the RTC seconds/minutes/hours alarm and optional FADT day/month alarm registers.
- Clears and verifies stale PM1 status, arms the RTC fixed-event enable, and checks whether the alarm reaches `PM1_STS.RTC_STS`.
- Supports a dry run that does **not** write `SLP_EN`.
- Includes an optional real-S3 path that writes `SLP_TYP` and `SLP_EN` after arming and checking the alarm.
- Discovers relevant ACPI information through RSDP → RSDT/XSDT → FADT → DSDT (`_S3_`).

## Build and run host tests

Requirements: GCC or Clang with C11/GNU11 support. The tests use AddressSanitizer and UndefinedBehaviorSanitizer when available.

```sh
make test
```

Or compile individually:

```sh
gcc -std=c11 -g -fsanitize=address,undefined -Wall -Wextra -I. \\
  tests/test_rtc_s3_sim.c -o test_rtc_s3_sim
./test_rtc_s3_sim

gcc -std=gnu11 -g -fsanitize=address,undefined -Wall -Wextra -I. \\
  -Ishellcode_hv -Itests/support tests/test_rtc_s3_hook_sim.c \\
  -o test_rtc_s3_hook_sim
./test_rtc_s3_hook_sim
```

The hook test uses a tiny `tests/support/utils.h` shim for the bootloader's UART `printf` declaration. The actual loader integration must use the target project's real `utils.h` and build environment.

## API outline

A typical loader integration is:

```c
rtc_s3_config_t cfg;
rtc_s3_set_logger(loader_uart_log);

if (rtc_s3_config_from_acpi(&cfg, rsdp_phys, physical_to_virtual) == 0 &&
    rtc_s3_init(&cfg) == 0) {
    rtc_s3_selftest(10, RTC_S3_DRY_RUN);
}
```

See `rtc_s3_sleep.h` for configuration fields, return codes, and the full API. `rtc_s3_dry_run()` is limited to short delays; `configure_rtc_s3_sleep()` is the real-S3 entry function.

## Important safety and portability limitations

**This is not a complete suspend/resume implementation and must not be treated as safe to enable on arbitrary hardware.**

The core does not perform the platform's complete ACPI sleep transition. In particular, a loader integrating real S3 must separately handle:

- ACPI `_PTS` / `_GTS` methods and platform-specific preparation;
- device quiescing and chipset/platform state save;
- parking or otherwise coordinating additional CPUs;
- FACS waking-vector setup and a valid resume path;
- restoring execution state after the CPU loses state during S3;
- correct PM1 register addresses, lengths, sleep types, and platform-specific quirks.

The dry-run path is intended for diagnostics and never writes `SLP_EN`; it still performs RTC/PM1 I/O and should only be used in an environment where direct port I/O is appropriate. The host simulator validates software logic against a model, **not** real firmware, chipset, or motherboard behavior. Passing the tests is not evidence that a machine supports RTC-triggered S3 wake.

The implementation uses privileged x86 port I/O, `cli`, `wbinvd`, and `sti` instructions outside host simulation. Build for the target environment, and compile the loader with `-mno-red-zone` as noted in the source.

## Integration patch

The patch in `integrations/vivalalvent/` was written against a specific version/layout of the VivaLaVent PS5 loader. It adds an opt-in boot-time dry run, a UART hook, and a blob-size check. It is supplied for provenance and review; it may not apply cleanly to current upstream revisions. Review every hunk and verify the code-cave size and memory layout before use.

## Status

Experimental reference code, not a maintained cross-platform library. Contributions, independent review, hardware reports, and portability fixes are welcome. Please include the exact platform, firmware, ACPI details, and whether results came from simulation or physical hardware when reporting findings.

## License

No license has been assigned yet. Until the author chooses and adds a license, default copyright applies; viewing and reviewing the code does not automatically grant permission to redistribute or incorporate it. Please ask before reusing it in another project.
