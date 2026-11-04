/*
 * test_rtc_s3_hook_sim.c - host-side test of shellcode_hv/rtc_s3_hook.c
 *
 * Reuses the simulated CMOS RTC / PM1 block and the synthetic ACPI tables of
 * test_rtc_s3_sim.c, and replaces the HV shellcode's polled-UART printf() with
 * a capture buffer, so the whole boot-time path runs on the host:
 *   rtc_s3_boot_selftest -> ACPI discovery -> rtc_s3_init -> dry run -> UART.
 *
 *   gcc -std=gnu11 -g -fsanitize=address,undefined -Wall -Wextra \
 *       -I<repo>/shellcode_hv test_rtc_s3_hook_sim.c -o test_hook && ./test_hook
 *
 * (test_rtc_s3_sim.c and rtc_s3_sleep.c/.h must be in the include path.)
 */
#define main sim_main                  /* keep the sim's own main() out of the way */
#include "test_rtc_s3_sim.c"
#undef main

/* The HV shellcode declares  int printf(const uint8_t *msg);  - rename it so it
 * can coexist with libc's printf on the host. */
#define printf hv_printf_capture
#include "rtc_s3_hook.c"
#undef printf

static char uart[8192];
static size_t uart_n;

int hv_printf_capture(const uint8_t *msg)
{
    for (size_t i = 0; msg[i] && i < 255; i++) {      /* same 255-char cap as the HV */
        if (msg[i] == '\n' && uart_n < sizeof uart - 2) uart[uart_n++] = '\r';
        if (uart_n < sizeof uart - 1) uart[uart_n++] = (char)msg[i];
    }
    uart[uart_n] = 0;
    return 0;
}

static void uart_reset(void) { uart_n = 0; uart[0] = 0; }
static bool uart_has(const char *s) { return strstr(uart, s) != NULL; }

static void test_hook_healthy(void)
{
    acpi_p a = base_params();
    uint64_t rsdp = build_acpi(&a);

    setup(6, 36000);                                   /* sim hardware, healthy */
    uart_reset();
    rtc_s3_boot_selftest(rsdp, test_map);

    puts("  -- captured UART:"); fputs(uart, stdout);
    assert(uart_has("rtc-s3: hook: start"));
    assert(uart_has("rtc-s3: hook: done"));
    assert(uart_has("mode=dry") && uart_has("rc=0") && uart_has("af=1") && uart_has("rtc_sts=1"));
    assert(uart_has("pm1a_evt=0x0400") && uart_has("slp_typa=5"));
    /* every line ends in CRLF exactly once: no bare '\n', no empty lines */
    for (size_t i = 0; i < uart_n; i++) {
        if (uart[i] == '\n') assert(i > 0 && uart[i - 1] == '\r');
        if (uart[i] == '\r') assert(i + 1 < uart_n && uart[i + 1] == '\n');
    }
    assert(!strstr(uart, "\r\n\r\n"));
    assert(g_rtc_s3_dbg.result == 0 && g_rtc_s3_dbg.dry_run);
    assert(g_cfg.cmos_nmi_bit == 0x80);                /* NMI stays masked */
    assert(S.slp_en_writes == 0 && S.n_cnt_writes == 0 && S.wbinvd_calls == 0);
    check_disarmed();
    puts("ok  hook: healthy platform, dry run result reaches the UART, no SLP_EN");
}

static void test_hook_findings(void)
{
    acpi_p a = base_params();
    uint64_t rsdp = build_acpi(&a);

    setup(6, 36000); S.wired_pm1 = false;              /* alarm not wired to PM1 */
    uart_reset();
    rtc_s3_boot_selftest(rsdp, test_map);
    assert(uart_has("rc=-30") && uart_has("not wired to PM1") && uart_has("hook: done"));
    check_disarmed();

    /* bad RSDP: logs, returns, touches no hardware at all */
    setup(6, 36000);
    uart_reset();
    rtc_s3_boot_selftest(0x1234, test_map);
    assert(uart_has("ACPI discovery failed rc=-20") && uart_has("RSDP missing"));
    assert(!uart_has("hook: done") && S.io == 0 && S.irq_saved == 0);

    /* ACPI ok but no static _S3_ package: same story */
    a = base_params(); a.dsdt_body = AML_S5_ONLY; a.dsdt_body_len = sizeof AML_S5_ONLY;
    rsdp = build_acpi(&a);
    setup(6, 36000);
    uart_reset();
    rtc_s3_boot_selftest(rsdp, test_map);
    assert(uart_has("rc=-25") && S.io == 0);

    /* FIX_RTC platform: the dry run is still allowed (only real S3 is blocked) */
    a = base_params(); a.flags = 1u << 6; rsdp = build_acpi(&a);
    setup(6, 36000);
    uart_reset();
    rtc_s3_boot_selftest(rsdp, test_map);
    assert(uart_has("fix_rtc=1") && uart_has("rc=0"));
    puts("ok  hook: NO_PM1 finding, bad RSDP, missing _S3_, FIX_RTC");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_hook_healthy();
    test_hook_findings();
    puts("ALL HOOK TESTS OK");
    return 0;
}
