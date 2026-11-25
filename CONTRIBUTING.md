# Contributing

Thanks for taking the time to review or improve this experimental RTC/ACPI diagnostic project.

## Before changing code

- Keep the project focused on RTC alarm and ACPI PM1 diagnostics.
- Preserve the distinction between host-simulated behavior and physical-hardware observations.
- Do not describe the project as a complete S3 implementation.
- Treat changes to the real-S3 path as high risk. Do not enable real S3 in the default hook or host-test workflow.
- For large loader-specific changes, open an issue first so the target project and revision can be agreed.

## Development setup

Requirements: GCC or Clang, GNU Make, and a C11/GNU11-capable toolchain.

Run the complete host test suite from the repository root:

```sh
make clean
make test
```

The tests are expected to finish with `ALL SIM TESTS OK` and `ALL HOOK TESTS OK`. CI runs the same target with GCC and Clang.

Remove generated executables before preparing a patch:

```sh
make clean
```

## What to include with a change

For changes to RTC/PM1 logic or ACPI parsing:

- Add or update host-simulation coverage.
- Cover both the expected path and relevant failure paths.
- Verify that dry-run mode never writes `PM1_CNT.SLP_EN`.
- Keep the test-only hardware model separate from physical-hardware claims.
- Document any new result code in `rtc_s3_sleep.h` and its text in `rtc_s3_strerror()`.

For changes to the loader hook:

- Keep it opt-in and default to `RTC_S3_DRY_RUN`.
- Preserve the behavior where a diagnostic failure is logged and the loader continues booting.
- Keep NMI masked when the environment has no usable IDT, as the current hook does.
- Run `git apply --stat integrations/vivalalvent/rtc-s3-selftest-hook.diff` to validate patch syntax.
- Recheck applicability, code size, and integration assumptions against the exact target loader revision.
- Do not present the supplied VivaLaVent patch as universally applicable.

## Reporting hardware results

Please state clearly whether a result came from:

1. the repository's host simulation;
2. an emulator or virtual machine; or
3. physical hardware.

For real hardware, include the machine/model, firmware version, CPU/platform context where relevant, compiler and build flags, test mode, return code, and relevant UART output. Remove serial numbers, personal identifiers, private paths, network details, and other sensitive information before posting logs.

Use [the hardware report template](docs/hardware-report-template.md) to make reports comparable.

## Pull requests

- Keep changes focused and explain the motivation.
- Include the exact commands used to test.
- Mention tests that were not run and why.
- Do not include build outputs, private logs, credentials, or unrelated machine-specific files.
- For hardware-only changes, include the platform and test evidence without claiming broader compatibility.

## Safety rule

The dry run must never write `SLP_EN`. The real-S3 path must not be made the default behavior. A passing simulator test is not sufficient evidence that a real platform can safely suspend and resume.
