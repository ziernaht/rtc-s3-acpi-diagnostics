#ifndef RTC_S3_HOOK_H
#define RTC_S3_HOOK_H

#include "rtc_s3_sleep.h"
#include <stdint.h>

// Seconds between arming the RTC alarm and the expected PM1_STS.RTC_STS hit.
#ifndef RTC_S3_SELFTEST_DELAY_S
#define RTC_S3_SELFTEST_DELAY_S 10
#endif

// Boot-time RTC alarm -> PM1 dry run. Never writes SLP_EN, never returns an
// error: every failure is logged on the UART and the caller carries on booting.
// `map` is the ACPI table mapper (NULL = physical == virtual, true in the HV
// shellcode because CR3 is the 1:1 map).
void rtc_s3_boot_selftest(uint64_t rsdp_phys, rtc_s3_map_fn map);

#endif
