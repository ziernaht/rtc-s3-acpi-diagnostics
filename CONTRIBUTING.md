# Contributing

This repository is shared for technical review and possible reuse by low-level systems developers.

- Keep dry-run behavior non-destructive: it must never write `PM1_CNT.SLP_EN`.
- Add host-simulation coverage for changes to RTC/PM1 logic and ACPI parsing.
- Clearly distinguish simulated results from physical-hardware results.
- Do not claim real S3 support without documenting the complete platform resume path.
- Include compiler version, build command, platform/firmware details, and relevant logs in reports.

Please open an issue before proposing a large integration-specific change.
