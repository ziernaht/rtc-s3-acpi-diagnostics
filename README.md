# RTC/ACPI S3 diagnostics for bare-metal x86-64

[![Host tests](https://github.com/ziernaht/rtc-s3-acpi-diagnostics/actions/workflows/host-tests.yml/badge.svg)](https://github.com/ziernaht/rtc-s3-acpi-diagnostics/actions/workflows/host-tests.yml)

An experimental C reference utility for testing whether a CMOS RTC alarm is observed by the ACPI PM1 fixed-event block. It is intended for bare-metal bootloader, firmware, hypervisor, and low-level platform developers.

**Start here:** run the host simulations with `make test`. You do not need real hardware to build or run those tests.

> **Important:** this is not a complete ACPI suspend/resume implementation. The normal test mode does not enter S3, but it still performs RTC and PM1 register I/O. The optional real-S3 function can leave the CPU without a usable resume path unless the surrounding loader implements the missing platform work. Do not enable real S3 on a machine just to see whether it works.

## What problem does it investigate?

An RTC alarm and an ACPI wake event are related, but they are not the same observation. This utility records two signals:

1. **RTC Reg C.AF** — the CMOS RTC reports that its alarm fired.
2. **PM1_STS.RTC_STS** — the ACPI PM1 event block reports the RTC fixed event.

Seeing both is useful evidence that the RTC alarm reached the PM1 event-status path in the environment being tested. Seeing only one helps narrow down where to investigate.

The result is diagnostic evidence, not proof that a complete S3 suspend/resume cycle will work.

## Quick start: run the host simulations

### Requirements

- GCC or Clang
- GNU Make
- A C11-capable compiler (the hook simulation uses GNU11)

### Build and run

```sh
git clone https://github.com/ziernaht/rtc-s3-acpi-diagnostics.git
cd rtc-s3-acpi-diagnostics
make test
```

The test target builds and runs two host-side programs:

- `tests/test_rtc_s3_sim.c` exercises the RTC/PM1 model, dry-run and real-S3 control flow, result reporting, and synthetic ACPI tables.
- `tests/test_rtc_s3_hook_sim.c` checks the optional UART hook, including failure logging and the guarantee that the hook uses dry-run mode.

A successful run ends with:

```text
ALL SIM TESTS OK
ALL HOOK TESTS OK
```

To remove the generated test executables:

```sh
make clean
```

The simulations are the safe starting point for development. They do not access the host machine's physical RTC, PM1 registers, or S3 sleep state.

## What the code does

- Reads RTC time with update-in-progress (UIP) guarding and repeated reads.
- Handles BCD/binary and 12/24-hour RTC encodings.
- Programs the RTC seconds, minutes, and hours alarm, plus optional FADT day/month alarm registers.
- Discovers ACPI information through RSDP → RSDT/XSDT → FADT → DSDT and looks for a static `_S3_` package.
- Clears and verifies stale RTC/PM1 status, enables the RTC alarm event, and checks whether the alarm appears in PM1 status.
- Provides a **dry-run** path that never writes `PM1_CNT.SLP_EN`.
- Includes an optional real-S3 entry path for integration experiments.

See [Architecture](docs/architecture.md) for the execution flow and parser limitations.

## Interpreting results

| Observation/result | What it indicates |
| --- | --- |
| `RTC_S3_OK` / result `0` | The expected RTC and PM1 status signals were both observed for the selected path. |
| `RTC_S3_DRY_NO_PM1` / `-30` | RTC alarm flag was observed, but PM1 RTC status was not. Investigate routing, PM1 mapping, enable bits, and platform behavior. |
| `RTC_S3_DRY_NO_ALARM` / `-31` | Neither signal was observed during the polling window. |
| `RTC_S3_DRY_PM1_ONLY` / `-32` | PM1 RTC status was observed, but the RTC alarm flag was not captured. Investigate timing and read-to-clear behavior. |
| `RTC_S3_ERR_ACPI_*` | ACPI discovery or supported-table parsing failed. |
| Other negative result | See `rtc_s3_strerror()` in `rtc_s3_sleep.c` and the enum in `rtc_s3_sleep.h`. |

A result from a host simulation describes the simulated model only. A result from physical hardware should include the platform, firmware version, compiler, exact test mode, return code, and relevant UART log. Use the [hardware report template](docs/hardware-report-template.md).

## Repository layout

```text
.
├── rtc_s3_sleep.c                 # RTC/ACPI implementation
├── rtc_s3_sleep.h                 # Public configuration, API, and result codes
├── shellcode_hv/
│   ├── rtc_s3_hook.c              # Optional boot-time UART hook example
│   └── rtc_s3_hook.h
├── tests/
│   ├── test_rtc_s3_sim.c          # Core host simulation
│   ├── test_rtc_s3_hook_sim.c     # Hook host simulation
│   └── support/utils.h            # Test-only UART declaration shim
├── integrations/
│   └── vivalalvent/
│       └── rtc-s3-selftest-hook.diff
├── docs/
│   ├── architecture.md
│   └── hardware-report-template.md
├── .github/workflows/host-tests.yml
├── CONTRIBUTING.md
└── LICENSE
```

## Using it in a loader

This repository is a standalone reference and host-test project; it is not the full VivaLaVent loader and does not produce a ready-to-install PS5 payload by itself.

The patch in `integrations/vivalalvent/rtc-s3-selftest-hook.diff` is a **project-specific integration example**. It adds an opt-in boot-time dry run and a polled-UART hook to a particular loader layout. It is not a generic patch for other loaders or arbitrary revisions.

Before applying it to a separate VivaLaVent checkout:

1. Review the patch and identify the target loader revision/layout it expects.
2. From this repository, run `git apply --stat integrations/vivalalvent/rtc-s3-selftest-hook.diff` to verify the diff is syntactically valid.
3. In the target loader repository, run `git apply --check /path/to/rtc-s3-selftest-hook.diff` to verify that its context matches your exact checkout.
4. Apply it only if the check succeeds; inspect the resulting changes before building.
5. Follow the target loader's build instructions and verify its code-cave/blob-size limit. The supplied patch includes an 8 KiB size check for the layout it targets.
6. Keep the feature opt-in. The patch is intended to run the dry-run diagnostic, not to enter S3.

CI checks patch syntax with `git apply --stat` and verifies that the embedded C/header snapshots match the canonical files in this repository. It cannot verify applicability without the exact target loader source revision. The repository's top-level `Makefile` runs host tests only. The optional `RTC_S3_SELFTEST=1` build flag belongs to the separate loader integration patch, not to this standalone repository.

## Safety and limitations

### Dry run is not “no hardware access”

The dry-run path does not write `SLP_EN` or enter S3. It still performs privileged RTC/PM1 I/O: it programs the alarm registers, changes the RTC alarm-enable bit and PM1 RTC event-enable bit, reads RTC Reg C (which is read-to-clear), and clears stale PM1 status flags. Use it only in an environment where direct port I/O is appropriate and where those register effects are understood.

### Real S3 is incomplete

The real-S3 function is an entry experiment, not a full sleep-state implementation. The surrounding loader/platform code must separately provide, at minimum:

- ACPI `_PTS` / `_GTS` methods and platform-specific preparation;
- device quiescing and chipset/platform state save;
- coordination or parking of additional CPUs;
- FACS waking-vector setup and a valid resume path;
- restoration of CPU and platform execution state after S3;
- correct PM1 addresses, lengths, sleep types, and platform-specific handling.

The code uses privileged x86 instructions including port I/O, `cli`, `wbinvd`, and `sti` outside host simulation. Compile for the target environment; the source notes the `-mno-red-zone` requirement.

### ACPI parsing is deliberately limited

The built-in AML scan is a small heuristic for the common static `Name(_S3_, Package(...))` form with constant byte values. It is not a complete AML interpreter. Firmware using computed AML, unusual encodings, or unsupported PM1 address spaces may require a real ACPI/AML library or explicitly supplied configuration.

### What this project is not

- Not a Linux utility, desktop application, or replacement for Linux ACPI/RTC drivers.
- Not a PS5 performance tweak or general compatibility fix.
- Not proof that a console, PC, motherboard, or emulator supports RTC-triggered S3 wake.
- Not a maintained cross-platform library or a ready-made suspend/resume implementation.

## Project status

Experimental reference code. Independent review, simulation improvements, portability work, and clearly documented hardware reports are welcome. See [Contributing](CONTRIBUTING.md).

## License

MIT. See [LICENSE](LICENSE).
