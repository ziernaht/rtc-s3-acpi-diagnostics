// Boot-time RTC/ACPI S3 dry run, wired to the polled UART of the HV shellcode.
// Only built with `make RTC_S3_SELFTEST=1`, see Makefile.
#include "rtc_s3_hook.h"
#include "utils.h"
#include <stddef.h>

static void uart_str(const char *s) { printf((const uint8_t *)s); }

// rtc_s3 hands over one line without a newline; printf() turns '\n' into "\r\n".
static void rtc_s3_uart_log(const char *line) {
  uart_str(line);
  uart_str("\n");
}

static void log_rc(const char *what, int rc) {
  char num[16];
  unsigned u = rc < 0 ? 0u - (unsigned)rc : (unsigned)rc;
  int i = (int)sizeof(num) - 1;

  num[i] = '\0';
  do {
    num[--i] = (char)('0' + u % 10u);
    u /= 10u;
  } while (u);
  if (rc < 0)
    num[--i] = '-';

  uart_str("rtc-s3: hook: ");
  uart_str(what);
  uart_str(" rc=");
  uart_str(&num[i]);
  uart_str(" (");
  uart_str(rtc_s3_strerror(rc));
  uart_str(")\n");
}

void rtc_s3_boot_selftest(uint64_t rsdp_phys, rtc_s3_map_fn map) {
  rtc_s3_config_t cfg;
  int rc;

  uart_str("rtc-s3: hook: start (dry run, SLP_EN is never written)\n");
  rtc_s3_set_logger(rtc_s3_uart_log);

  rc = rtc_s3_config_from_acpi(&cfg, rsdp_phys, map);
  if (rc) {
    log_rc("ACPI discovery failed", rc);
    return;
  }

  // Our IDT is gone here: never unmask NMI through bit 7 of port 0x70.
  cfg.cmos_nmi_bit = 0x80;

  rc = rtc_s3_init(&cfg);
  if (rc) {
    log_rc("config rejected", rc);
    return;
  }

  // Logs cfg, the armed state and the full result (g_rtc_s3_dbg) itself.
  (void)rtc_s3_selftest(RTC_S3_SELFTEST_DELAY_S, RTC_S3_DRY_RUN);

  uart_str("rtc-s3: hook: done\n");
}
