# Hardware test report template

Copy this template when reporting a test performed outside the repository's host simulation. Fill in what you know and mark unknown fields as unknown; do not guess.

## Environment

- **Test type:** physical hardware / emulator / virtual machine
- **Device or platform model:**
- **CPU / chipset (if known):**
- **Firmware / BIOS version:**
- **Bootloader or integration revision:**
- **RTC implementation (if known):**

## Build

- **Compiler and version:**
- **Build command:**
- **Build flags:**
- **Source commit:**

## Test

- **Function or hook used:** `rtc_s3_dry_run` / `rtc_s3_selftest(..., RTC_S3_DRY_RUN)` / other
- **Delay requested:**
- **Return code:**
- **Text from `rtc_s3_strerror()`:**
- **Relevant `g_rtc_s3_dbg` fields:**
- **Did the machine remain operational?**
- **Was an actual S3 transition attempted?** yes / no

## Output

Paste only the relevant log lines here:

```text
<redact private or identifying details before posting>
```

## Interpretation

- What did you expect?
- What happened?
- Was the result repeatable?
- What changed between successful and unsuccessful runs?

## Safety and privacy checklist

- [ ] I have clearly labelled this as simulation, emulation, VM, or physical hardware.
- [ ] I have not described a dry-run result as proof of successful S3 resume.
- [ ] I have removed serial numbers, personal names, email addresses, private filesystem paths, IP/MAC addresses, and credentials from logs.
- [ ] I have not included private firmware dumps or unrelated proprietary data.
