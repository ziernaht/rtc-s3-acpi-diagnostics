/*
 * rtc_s3_sleep.h - RTC-alarm-timed ACPI S3 entry, plus a non-destructive dry
 *                  run, for a bare-metal x86-64 loader.
 *
 * Typical use, right before kernel handoff:
 *
 *     rtc_s3_config_t cfg;
 *
 *     rtc_s3_set_logger(uart_log_line);            // optional; no newline in line
 *     if (rtc_s3_config_from_acpi(&cfg, rsdp_phys, NULL) == 0) {
 *         cfg.now_ms = loader_uptime_ms;            // optional, makes timeouts exact
 *         if (rtc_s3_init(&cfg) == 0)
 *             rtc_s3_selftest(10, RTC_S3_DRY_RUN);  // later: RTC_S3_REAL_S3
 *     }
 *
 * Or fill rtc_s3_config_t by hand (rtc_s3_config_defaults() first) if the
 * platform has no usable ACPI tables.
 *
 * The dry run executes everything up to (not including) the SLP_EN write and
 * then polls PM1_STS.RTC_STS and RTC Reg C.AF for delay+5 s. It runs with
 * interrupts disabled, so the logger must be a polled UART.
 */
#ifndef RTC_S3_SLEEP_H
#define RTC_S3_SLEEP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Readable names for the `dry_run` argument of rtc_s3_selftest(). */
#define RTC_S3_DRY_RUN  true
#define RTC_S3_REAL_S3  false

/*
 * Result codes. 0 means "the RTC alarm demonstrably reached PM1/wake" in both
 * modes (dry run: AF and PM1_STS.RTC_STS both seen; real S3: woke with
 * RTC_STS set). Negative values are failures/negative findings.
 */
enum rtc_s3_result {
    RTC_S3_OK                   =   0,
    RTC_S3_RESUMED_RTC          =   0,   /* real S3: woke, RTC_STS set        */
    RTC_S3_DRY_ALARM_OK         =   0,   /* dry run: AF + RTC_STS both seen   */
    RTC_S3_RESUMED_OTHER        =   1,   /* real S3: woke, RTC_STS NOT set    */

    RTC_S3_ERR_ARG              =  -1,
    RTC_S3_ERR_NO_FIXED_RTC     =  -2,   /* FADT FIX_RTC set (real S3 only)   */
    RTC_S3_ERR_RTC_INVALID      =  -3,
    RTC_S3_ERR_RTC_TIMEOUT      =  -4,
    RTC_S3_ERR_RTC_EN_MASKED    =  -5,   /* PM1_EN.RTC_EN did not stick       */
    RTC_S3_ERR_MARGIN           =  -6,   /* alarm too close / already passed  */
    RTC_S3_ERR_SLEEP_FAILED     =  -7,   /* SLP_EN written, WAK_STS never set */
    RTC_S3_ERR_NOT_CONFIGURED   =  -8,   /* rtc_s3_init() not called          */
    RTC_S3_ERR_CONFIG           =  -9,   /* config rejected by rtc_s3_init()  */
    RTC_S3_ERR_NO_SCI           = -10,   /* SCI_EN clear and could not enable */
    RTC_S3_ERR_RTC_STUCK        = -11,   /* RTC seconds never advanced        */
    RTC_S3_ERR_STS_STUCK        = -12,   /* PM1_STS RTC/WAK bits won't clear  */

    RTC_S3_DRY_NO_PM1           = -30,   /* AF seen, RTC_STS never            */
    RTC_S3_DRY_NO_ALARM         = -31,   /* neither AF nor RTC_STS seen       */
    RTC_S3_DRY_PM1_ONLY         = -32,   /* RTC_STS seen, AF never            */

    RTC_S3_ERR_ACPI_RSDP        = -20,
    RTC_S3_ERR_ACPI_SDT         = -21,   /* no usable XSDT/RSDT               */
    RTC_S3_ERR_ACPI_NO_FADT     = -22,
    RTC_S3_ERR_ACPI_BAD_FADT    = -23,   /* bad checksum / too short          */
    RTC_S3_ERR_ACPI_NO_PM1      = -24,   /* PM1a missing or not I/O-port      */
    RTC_S3_ERR_ACPI_NO_S3       = -25,   /* no static _S3 package in DSDT     */
};

/* Maps a physical range to a readable pointer. NULL map = identity. Earlier
 * mappings must stay valid while later ones are made (identity or a pool). */
typedef const void *(*rtc_s3_map_fn)(uint64_t phys, uint32_t len);
/* Receives one log line (no trailing newline). */
typedef void (*rtc_s3_log_fn)(const char *line);
/* Monotonic millisecond clock (optional). */
typedef uint64_t (*rtc_s3_now_ms_fn)(void);

typedef struct {
    uint16_t pm1a_evt;          /* PM1a_EVT_BLK I/O port; EN is at +pm1_evt_len/2 */
    uint16_t pm1b_evt;          /* 0 = no PM1b                                    */
    uint16_t pm1a_cnt;          /* PM1a_CNT_BLK I/O port                          */
    uint16_t pm1b_cnt;          /* 0 = no PM1b                                    */
    uint8_t  pm1_evt_len;       /* FADT PM1_EVT_LEN, normally 4                   */
    uint8_t  slp_typa;          /* SLP_TYPa from the _S3 package (0..7)           */
    uint8_t  slp_typb;          /* SLP_TYPb (only used if pm1b_cnt != 0)          */
    bool     fix_rtc;           /* FADT flags bit 6 FIX_RTC (NOT bit 5)           */
    uint16_t smi_cmd;           /* FADT SMI_CMD port, 0 = unknown                 */
    uint8_t  acpi_enable;       /* FADT ACPI_ENABLE value                         */
    uint8_t  rtc_day_alarm_idx; /* FADT DAY_ALRM CMOS index, 0 = none             */
    uint8_t  rtc_mon_alarm_idx; /* FADT MON_ALRM CMOS index, 0 = none             */
    uint8_t  rtc_alarm_dontcare;/* "match any" value for day/mon alarm (0xFF)     */
    uint8_t  cmos_nmi_bit;      /* 0x80 if the loader keeps NMI masked            */
    uint64_t facs_phys;         /* FACS address (informational, for resume setup) */
    rtc_s3_now_ms_fn now_ms;    /* optional; NULL = time the poll with RTC itself */
    uint32_t dry_spin_limit;    /* poll-iteration failsafe, 0 = default (30M)     */
} rtc_s3_config_t;

/* Snapshot for UART logging after the call / after resume. */
typedef struct {
    uint32_t now_s, target_s;   /* seconds of day                                 */
    uint32_t remaining_s;       /* alarm distance at the last margin check        */
    uint32_t elapsed_s;         /* dry run: seconds polled                        */
    uint32_t first_hit_s;       /* dry run: elapsed when AF/RTC_STS first seen    */
    uint16_t pm1_en_armed;      /* PM1_EN right after arming                      */
    uint16_t pm1_sts_pre;       /* PM1_STS before clearing stale flags            */
    uint16_t pm1_sts_cleared;   /* PM1_STS right after the clear (bits must be 0) */
    uint16_t pm1_sts_post;      /* OR of PM1_STS reads while waiting / after wake */
    uint16_t pm1_cnt_pre;       /* PM1a_CNT before the SLP_TYP write              */
    uint8_t  rtc_regb;
    uint8_t  rtc_regc_post;     /* OR of Reg C reads while waiting (AF = bit 5)   */
    uint8_t  sci_en, rtc_en_ok, af_seen, sts_seen, rtc_ticked, dry_run;
    int32_t  result;
} rtc_s3_dbg_t;

extern volatile rtc_s3_dbg_t g_rtc_s3_dbg;

/* Configuration. */
void rtc_s3_config_defaults(rtc_s3_config_t *cfg);
/* RSDP -> XSDT/RSDT -> FADT -> DSDT (_S3_). On failure cfg is partly filled
 * and must not be used. */
int  rtc_s3_config_from_acpi(rtc_s3_config_t *cfg, uint64_t rsdp_phys,
                             rtc_s3_map_fn map);
int  rtc_s3_init(const rtc_s3_config_t *cfg);   /* validate + store a copy */
void rtc_s3_set_logger(rtc_s3_log_fn fn);

/* Operations (all need rtc_s3_init() first). */
int  configure_rtc_s3_sleep(uint32_t seconds_delay);  /* real S3, 3..86399 s   */
int  rtc_s3_dry_run(uint32_t seconds_delay);          /* no SLP_EN, 3..120 s   */
int  rtc_s3_selftest(uint32_t delay_seconds, bool dry_run);

/* Diagnostics. */
void rtc_s3_dbg_dump(void);                 /* g_rtc_s3_dbg -> logger         */
const char *rtc_s3_strerror(int rc);

#ifdef __cplusplus
}
#endif
#endif /* RTC_S3_SLEEP_H */
