/*
 * rtc_s3_sleep.c - RTC-alarm-timed S3 entry (and dry run) for a bare-metal
 *                  x86-64 loader. Runtime-configured: no build-time ports.
 *
 * Shared arming sequence (rtc_s3_arm), steps 0-7:
 *   0. cli; validate args / config (real S3 rejects FADT FIX_RTC, needs SCI_EN)
 *   1. Check RTC valid (Reg D VRT) and running (Reg B SET clear)
 *   2. Disarm any old alarm: Reg B AIE off, PM1_EN.RTC_EN off
 *   3. Read h:m:s (UIP-guarded, double-read), normalise to seconds-of-day
 *   4. target = (now + N) mod 86400; encode per Reg B (BCD/binary, 12/24 h)
 *   5. Program alarm regs 0x01/0x03/0x05 (+ optional FADT day/month alarm)
 *   6. Clear stale flags: RTC Reg C (read), PM1_STS.RTC_STS | WAK_STS (W1C),
 *      then verify they really read back 0
 *   7. Arm: Reg B AIE, then PM1_EN.RTC_EN (read back to confirm it sticks)
 * Then either
 *   dry run : confirm margin, poll PM1_STS.RTC_STS and Reg C.AF for N+5 s,
 *             never touch PM1_CNT
 *   real S3 : wbinvd, confirm margin, SLP_TYP then SLP_TYP|SLP_EN in PM1_CNT
 *
 * NOT handled here (your loader must do these):
 *   - ACPI _PTS/_GTS, device quiescing, CPU/chipset context save, parking APs
 *   - FACS firmware waking vector setup
 *   - On real S3 the CPU loses all state. This only "returns" if your resume
 *     path restores context and continues after the SLP_EN write. Otherwise
 *     execution restarts at your resume vector; g_rtc_s3_dbg (if RAM survives)
 *     and rtc_s3_dbg_dump() tell you what happened.
 *
 * Build notes: compile the loader with -mno-red-zone (irq_save uses pushfq).
 * Host simulation: define RTC_S3_HOST_SIM and provide pio_outb/pio_inb/
 * pio_outw/pio_inw/irq_save/irq_restore/cpu_wbinvd before including this file.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "rtc_s3_sleep.h"

/* ------------------------------------------------------- port I/O + CPU */
#ifndef RTC_S3_HOST_SIM
static inline void pio_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t pio_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void pio_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t pio_inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags)
{
    if (flags & (1u << 9))
        __asm__ volatile ("sti" : : : "memory");
}
static inline void cpu_wbinvd(void)
{
    __asm__ volatile ("wbinvd" : : : "memory");
}
#endif /* !RTC_S3_HOST_SIM */

/* ------------------------------------------------------------------ ACPI */
#define PM1_STS_RTC           ((uint16_t)(1u << 10))
#define PM1_STS_WAK           ((uint16_t)(1u << 15))
#define PM1_EN_RTC            ((uint16_t)(1u << 10))
#define PM1_CNT_SCI_EN        ((uint16_t)(1u << 0))
#define PM1_CNT_SLP_TYP_SHIFT 10
#define PM1_CNT_SLP_TYP_MASK  ((uint16_t)(7u << 10))
#define PM1_CNT_SLP_EN        ((uint16_t)(1u << 13))

#define ACPI_FADT_FIX_RTC     (1u << 6)   /* bit 5 is SLP_BUTTON, not FIX_RTC */
#define ACPI_HDR_LEN          36u
#define ACPI_MAX_TABLE        (8u * 1024u * 1024u)

/* -------------------------------------------------------------------- RTC */
#define RTC_S3_IDX        0x70
#define RTC_S3_DATA       0x71

#define RTC_REG_SEC       0x00
#define RTC_REG_SEC_ALRM  0x01
#define RTC_REG_MIN       0x02
#define RTC_REG_MIN_ALRM  0x03
#define RTC_REG_HOUR      0x04
#define RTC_REG_HOUR_ALRM 0x05
#define RTC_REG_A         0x0A
#define RTC_REG_B         0x0B
#define RTC_REG_C         0x0C
#define RTC_REG_D         0x0D

#define RTC_A_UIP  0x80
#define RTC_B_SET  0x80
#define RTC_B_AIE  0x20
#define RTC_B_DM   0x04   /* 1 = binary, 0 = BCD */
#define RTC_B_24H  0x02   /* 1 = 24 h,   0 = 12 h (bit 7 of hour = PM) */
#define RTC_C_AF   0x20
#define RTC_D_VRT  0x80

#define RTC_S3_MIN_DELAY_S      3u
#define RTC_S3_MAX_DELAY_S      86399u  /* h/m/s alarm only: must stay < 24 h */
#define RTC_S3_DRY_MAX_DELAY_S  120u
#define RTC_S3_DRY_GRACE_S      5u      /* poll window = delay + grace         */
#define RTC_S3_DRY_HIT_GRACE_S  2u      /* keep polling this long after a hit  */
#define RTC_S3_MIN_MARGIN_S     2u
#define UIP_SPIN_LIMIT          200000u
#define WAK_SPIN_LIMIT          5000000u
#define SCI_SPIN_LIMIT          1000000u
#define RTC_S3_DRY_SPIN_DEFAULT 30000000u

/* ----------------------------------------------------------------- state */
static rtc_s3_config_t g_cfg;
static bool            g_cfg_ok;
static rtc_s3_log_fn   g_log;
volatile rtc_s3_dbg_t  g_rtc_s3_dbg;

typedef struct { uint8_t sec, min, hour; } rtc_raw_t;
typedef struct { uint8_t regb; uint32_t now_s, target_s; } arm_t;

/* ------------------------------------------------------------------- PM1 */
static inline uint16_t pm1a_en_port(void)
{
    return (uint16_t)(g_cfg.pm1a_evt + g_cfg.pm1_evt_len / 2);
}
static inline uint16_t pm1b_en_port(void)
{
    return (uint16_t)(g_cfg.pm1b_evt + g_cfg.pm1_evt_len / 2);
}

static uint16_t pm1_sts_read(void)
{
    uint16_t v = pio_inw(g_cfg.pm1a_evt);
    if (g_cfg.pm1b_evt)
        v = (uint16_t)(v | pio_inw(g_cfg.pm1b_evt));
    return v;
}

static void pm1_sts_clear(uint16_t mask)             /* write-1-to-clear */
{
    pio_outw(g_cfg.pm1a_evt, mask);
    if (g_cfg.pm1b_evt)
        pio_outw(g_cfg.pm1b_evt, mask);
}

static uint16_t pm1_en_read(void)
{
    uint16_t v = pio_inw(pm1a_en_port());
    if (g_cfg.pm1b_evt)
        v = (uint16_t)(v | pio_inw(pm1b_en_port()));
    return v;
}

static void pm1_en_modify(uint16_t set, uint16_t clr)
{
    pio_outw(pm1a_en_port(),
             (uint16_t)((pio_inw(pm1a_en_port()) | set) & (uint16_t)~clr));
    if (g_cfg.pm1b_evt)
        pio_outw(pm1b_en_port(),
                 (uint16_t)((pio_inw(pm1b_en_port()) | set) & (uint16_t)~clr));
}

/* Write SLP_TYP (and optionally SLP_EN), preserving SCI_EN and other bits. */
static void pm1_cnt_slp(bool slp_en)
{
    uint16_t en = slp_en ? PM1_CNT_SLP_EN : 0;
    uint16_t a = pio_inw(g_cfg.pm1a_cnt);

    a = (uint16_t)((a & (uint16_t)~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN)) |
                   ((uint16_t)(g_cfg.slp_typa & 7u) << PM1_CNT_SLP_TYP_SHIFT) | en);
    pio_outw(g_cfg.pm1a_cnt, a);
    if (g_cfg.pm1b_cnt) {
        uint16_t b = pio_inw(g_cfg.pm1b_cnt);
        b = (uint16_t)((b & (uint16_t)~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN)) |
                       ((uint16_t)(g_cfg.slp_typb & 7u) << PM1_CNT_SLP_TYP_SHIFT) | en);
        pio_outw(g_cfg.pm1b_cnt, b);
    }
}

/* Real S3 needs ACPI mode on. Enable it via SMI_CMD if the FADT told us how. */
static int ensure_sci_en(void)
{
    uint16_t cnt = pio_inw(g_cfg.pm1a_cnt);

    if (!(cnt & PM1_CNT_SCI_EN)) {
        if (!g_cfg.smi_cmd || !g_cfg.acpi_enable) {
            g_rtc_s3_dbg.sci_en = 0;
            return RTC_S3_ERR_NO_SCI;
        }
        pio_outb(g_cfg.smi_cmd, g_cfg.acpi_enable);
        for (uint32_t i = 0; i < SCI_SPIN_LIMIT; i++) {
            cnt = pio_inw(g_cfg.pm1a_cnt);
            if (cnt & PM1_CNT_SCI_EN)
                break;
        }
        if (!(cnt & PM1_CNT_SCI_EN)) {
            g_rtc_s3_dbg.sci_en = 0;
            return RTC_S3_ERR_NO_SCI;
        }
    }
    g_rtc_s3_dbg.sci_en = 1;
    return 0;
}

/* ------------------------------------------------------------- CMOS RTC */
static inline uint8_t cmos_read(uint8_t reg)
{
    pio_outb(RTC_S3_IDX, (uint8_t)(g_cfg.cmos_nmi_bit | reg));
    return pio_inb(RTC_S3_DATA);
}

static inline void cmos_write(uint8_t reg, uint8_t val)
{
    pio_outb(RTC_S3_IDX, (uint8_t)(g_cfg.cmos_nmi_bit | reg));
    pio_outb(RTC_S3_DATA, val);
}

static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool rtc_wait_not_updating(void)
{
    for (uint32_t i = 0; i < UIP_SPIN_LIMIT; i++)
        if (!(cmos_read(RTC_REG_A) & RTC_A_UIP))
            return true;
    return false;
}

/* Read h:m:s twice across UIP-clear windows; accept only identical reads. */
static bool rtc_read_raw(rtc_raw_t *out)
{
    for (int tries = 0; tries < 4; tries++) {
        rtc_raw_t a, b;
        if (!rtc_wait_not_updating()) return false;
        a.sec = cmos_read(RTC_REG_SEC);
        a.min = cmos_read(RTC_REG_MIN);
        a.hour = cmos_read(RTC_REG_HOUR);
        if (!rtc_wait_not_updating()) return false;
        b.sec = cmos_read(RTC_REG_SEC);
        b.min = cmos_read(RTC_REG_MIN);
        b.hour = cmos_read(RTC_REG_HOUR);
        if (a.sec == b.sec && a.min == b.min && a.hour == b.hour) {
            *out = a;
            return true;
        }
    }
    return false;
}

/* Cheap single read of the seconds register (poll-loop clock). */
static bool rtc_peek_sec(uint8_t regb, uint8_t *sec)
{
    uint8_t s;

    if (cmos_read(RTC_REG_A) & RTC_A_UIP)
        return false;
    s = cmos_read(RTC_REG_SEC);
    if (!(regb & RTC_B_DM))
        s = bcd2bin(s);
    if (s > 59)
        return false;
    *sec = s;
    return true;
}

/* Raw RTC bytes (in the encoding given by Reg B) -> seconds of day. */
static bool raw_to_sod(const rtc_raw_t *t, uint8_t regb, uint32_t *sod)
{
    uint8_t s = t->sec, m = t->min, h = t->hour, pm = 0;

    if (!(regb & RTC_B_24H)) { pm = (uint8_t)(h & 0x80); h &= 0x7F; }
    if (!(regb & RTC_B_DM))  { s = bcd2bin(s); m = bcd2bin(m); h = bcd2bin(h); }
    if (!(regb & RTC_B_24H)) {
        if (h < 1 || h > 12) return false;
        h %= 12;                        /* 12 AM -> 0, 12 PM -> 12 after +12 */
        if (pm) h = (uint8_t)(h + 12);
    }
    if (s > 59 || m > 59 || h > 23) return false;
    *sod = (uint32_t)h * 3600u + (uint32_t)m * 60u + s;
    return true;
}

/* Seconds of day -> alarm register bytes {sec, min, hour} per Reg B. */
static void sod_to_alarm(uint32_t sod, uint8_t regb, uint8_t out[3])
{
    uint8_t h = (uint8_t)(sod / 3600u);
    uint8_t m = (uint8_t)((sod / 60u) % 60u);
    uint8_t s = (uint8_t)(sod % 60u);
    uint8_t pm = 0;

    if (!(regb & RTC_B_24H)) {
        pm = (h >= 12) ? 0x80 : 0;
        h %= 12;
        if (h == 0) h = 12;
    }
    if (!(regb & RTC_B_DM)) { s = bin2bcd(s); m = bin2bcd(m); h = bin2bcd(h); }
    out[0] = s;
    out[1] = m;
    out[2] = (uint8_t)(h | pm);
}

static void rtc_disarm(void)
{
    cmos_write(RTC_REG_B, (uint8_t)(cmos_read(RTC_REG_B) & ~RTC_B_AIE));
    pm1_en_modify(0, PM1_EN_RTC);
    (void)cmos_read(RTC_REG_C);
    pm1_sts_clear(PM1_STS_RTC);
}

/* -------------------------------------------------------------- debug */
static void dbg_reset(bool dry)
{
    g_rtc_s3_dbg.now_s = 0;
    g_rtc_s3_dbg.target_s = 0;
    g_rtc_s3_dbg.remaining_s = 0;
    g_rtc_s3_dbg.elapsed_s = 0;
    g_rtc_s3_dbg.first_hit_s = 0;
    g_rtc_s3_dbg.pm1_en_armed = 0;
    g_rtc_s3_dbg.pm1_sts_pre = 0;
    g_rtc_s3_dbg.pm1_sts_cleared = 0;
    g_rtc_s3_dbg.pm1_sts_post = 0;
    g_rtc_s3_dbg.pm1_cnt_pre = 0;
    g_rtc_s3_dbg.rtc_regb = 0;
    g_rtc_s3_dbg.rtc_regc_post = 0;
    g_rtc_s3_dbg.sci_en = 0;
    g_rtc_s3_dbg.rtc_en_ok = 0;
    g_rtc_s3_dbg.af_seen = 0;
    g_rtc_s3_dbg.sts_seen = 0;
    g_rtc_s3_dbg.rtc_ticked = 0;
    g_rtc_s3_dbg.dry_run = dry ? 1 : 0;
    g_rtc_s3_dbg.result = 0;
}

/* --------------------------------------------------------- arm + margin */
static int rtc_s3_arm(uint32_t delay, bool real, arm_t *st)
{
    rtc_raw_t now;
    uint8_t alarm[3];
    uint16_t sts, en;
    int rc;

    if (!(cmos_read(RTC_REG_D) & RTC_D_VRT))
        return RTC_S3_ERR_RTC_INVALID;           /* RTC battery / power lost */
    if (cmos_read(RTC_REG_B) & RTC_B_SET)
        return RTC_S3_ERR_RTC_INVALID;           /* updates halted */

    if (real) {
        rc = ensure_sci_en();
        if (rc)
            return rc;
    } else {
        g_rtc_s3_dbg.sci_en = (pio_inw(g_cfg.pm1a_cnt) & PM1_CNT_SCI_EN) ? 1 : 0;
    }

    g_rtc_s3_dbg.pm1_sts_pre = pm1_sts_read();
    rtc_disarm();
    st->regb = cmos_read(RTC_REG_B);
    g_rtc_s3_dbg.rtc_regb = st->regb;

    if (!rtc_read_raw(&now))
        return RTC_S3_ERR_RTC_TIMEOUT;
    if (!raw_to_sod(&now, st->regb, &st->now_s))
        return RTC_S3_ERR_RTC_INVALID;

    st->target_s = (st->now_s + delay) % 86400u;
    sod_to_alarm(st->target_s, st->regb, alarm);
    g_rtc_s3_dbg.now_s = st->now_s;
    g_rtc_s3_dbg.target_s = st->target_s;

    cmos_write(RTC_REG_SEC_ALRM,  alarm[0]);
    cmos_write(RTC_REG_MIN_ALRM,  alarm[1]);
    cmos_write(RTC_REG_HOUR_ALRM, alarm[2]);
    if (g_cfg.rtc_day_alarm_idx)
        cmos_write(g_cfg.rtc_day_alarm_idx, g_cfg.rtc_alarm_dontcare);
    if (g_cfg.rtc_mon_alarm_idx)
        cmos_write(g_cfg.rtc_mon_alarm_idx, g_cfg.rtc_alarm_dontcare);

    /* Clear anything stale only after the alarm is programmed, then make sure
     * the bits really cleared: a stuck bit would fake a wake / a hit. */
    (void)cmos_read(RTC_REG_C);
    pm1_sts_clear((uint16_t)(PM1_STS_RTC | PM1_STS_WAK));
    sts = pm1_sts_read();
    g_rtc_s3_dbg.pm1_sts_cleared = sts;
    if (sts & (real ? (uint16_t)(PM1_STS_RTC | PM1_STS_WAK) : PM1_STS_RTC))
        return RTC_S3_ERR_STS_STUCK;

    cmos_write(RTC_REG_B, (uint8_t)(st->regb | RTC_B_AIE));
    pm1_en_modify(PM1_EN_RTC, 0);
    en = pm1_en_read();
    g_rtc_s3_dbg.pm1_en_armed = en;
    g_rtc_s3_dbg.rtc_en_ok = (en & PM1_EN_RTC) ? 1 : 0;
    if (real && !(en & PM1_EN_RTC))
        return RTC_S3_ERR_RTC_EN_MASKED;
    return 0;
}

/* Last chance to bail: alarm must still be in the future. */
static int rtc_s3_check_margin(const arm_t *st, uint32_t delay)
{
    rtc_raw_t now;
    uint32_t now_s, remaining;

    if (!rtc_read_raw(&now) || !raw_to_sod(&now, st->regb, &now_s))
        return RTC_S3_ERR_RTC_TIMEOUT;
    remaining = (st->target_s + 86400u - now_s) % 86400u;
    g_rtc_s3_dbg.remaining_s = remaining;
    if (remaining < RTC_S3_MIN_MARGIN_S || remaining > delay)
        return RTC_S3_ERR_MARGIN;
    return 0;
}

/* ------------------------------------------------------------ logging */
typedef struct { char b[120]; uint32_t n; } lb_t;

static void lb_putc(lb_t *l, char c)
{
    if (l->n < sizeof(l->b) - 1)
        l->b[l->n++] = c;
}
static void lb_str(lb_t *l, const char *s)
{
    while (*s)
        lb_putc(l, *s++);
}
static void lb_begin(lb_t *l)
{
    l->n = 0;
    lb_str(l, "rtc-s3:");
}
static void lb_end(lb_t *l)
{
    l->b[l->n] = 0;
    if (g_log)
        g_log(l->b);
}
static void lb_key(lb_t *l, const char *k)
{
    lb_putc(l, ' ');
    lb_str(l, k);
    lb_putc(l, '=');
}
static void lb_dec(lb_t *l, const char *k, uint32_t v)
{
    char t[10];
    int n = 0;

    lb_key(l, k);
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n)
        lb_putc(l, t[--n]);
}
static void lb_int(lb_t *l, const char *k, int32_t v)
{
    lb_key(l, k);
    if (v < 0) {
        lb_putc(l, '-');
        v = -v;
    }
    {
        char t[10];
        int n = 0;
        uint32_t u = (uint32_t)v;
        do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
        while (n)
            lb_putc(l, t[--n]);
    }
}
static void lb_hex(lb_t *l, const char *k, uint32_t v, int digits)
{
    lb_key(l, k);
    lb_str(l, "0x");
    for (int i = digits - 1; i >= 0; i--)
        lb_putc(l, "0123456789ABCDEF"[(v >> (4 * i)) & 0xFu]);
}
static void log_line(const char *s)
{
    if (g_log)
        g_log(s);
}

static void log_cfg(void)
{
    lb_t l;

    lb_begin(&l);
    lb_str(&l, " cfg");
    lb_hex(&l, "pm1a_evt", g_cfg.pm1a_evt, 4);
    lb_hex(&l, "pm1a_cnt", g_cfg.pm1a_cnt, 4);
    lb_hex(&l, "pm1b_evt", g_cfg.pm1b_evt, 4);
    lb_hex(&l, "pm1b_cnt", g_cfg.pm1b_cnt, 4);
    lb_end(&l);

    lb_begin(&l);
    lb_str(&l, " cfg");
    lb_dec(&l, "evt_len", g_cfg.pm1_evt_len);
    lb_dec(&l, "slp_typa", g_cfg.slp_typa);
    lb_dec(&l, "slp_typb", g_cfg.slp_typb);
    lb_dec(&l, "fix_rtc", g_cfg.fix_rtc ? 1u : 0u);
    lb_hex(&l, "smi_cmd", g_cfg.smi_cmd, 4);
    lb_hex(&l, "acpi_en", g_cfg.acpi_enable, 2);
    lb_end(&l);

    lb_begin(&l);
    lb_str(&l, " cfg");
    lb_hex(&l, "day_idx", g_cfg.rtc_day_alarm_idx, 2);
    lb_hex(&l, "mon_idx", g_cfg.rtc_mon_alarm_idx, 2);
    lb_hex(&l, "dontcare", g_cfg.rtc_alarm_dontcare, 2);
    lb_hex(&l, "nmi", g_cfg.cmos_nmi_bit, 2);
    lb_end(&l);
}

static void dbg_dump_impl(bool final)
{
    lb_t l;

    lb_begin(&l);
    lb_str(&l, g_rtc_s3_dbg.dry_run ? " mode=dry" : " mode=s3");
    if (final) {
        lb_int(&l, "rc", g_rtc_s3_dbg.result);
        lb_str(&l, " (");
        lb_str(&l, rtc_s3_strerror(g_rtc_s3_dbg.result));
        lb_str(&l, ")");
    } else {
        lb_str(&l, " state=armed");
    }
    lb_end(&l);

    lb_begin(&l);
    lb_dec(&l, "now_s", g_rtc_s3_dbg.now_s);
    lb_dec(&l, "target_s", g_rtc_s3_dbg.target_s);
    lb_dec(&l, "remain_s", g_rtc_s3_dbg.remaining_s);
    lb_hex(&l, "regb", g_rtc_s3_dbg.rtc_regb, 2);
    lb_end(&l);

    lb_begin(&l);
    lb_str(&l, " pm1_sts");
    lb_hex(&l, "pre", g_rtc_s3_dbg.pm1_sts_pre, 4);
    lb_hex(&l, "clr", g_rtc_s3_dbg.pm1_sts_cleared, 4);
    lb_hex(&l, "post", g_rtc_s3_dbg.pm1_sts_post, 4);
    lb_hex(&l, "en_armed", g_rtc_s3_dbg.pm1_en_armed, 4);
    lb_end(&l);

    lb_begin(&l);
    lb_hex(&l, "regc", g_rtc_s3_dbg.rtc_regc_post, 2);
    lb_dec(&l, "sci_en", g_rtc_s3_dbg.sci_en);
    lb_dec(&l, "rtc_en_ok", g_rtc_s3_dbg.rtc_en_ok);
    lb_dec(&l, "af", g_rtc_s3_dbg.af_seen);
    lb_dec(&l, "rtc_sts", g_rtc_s3_dbg.sts_seen);
    lb_dec(&l, "ticked", g_rtc_s3_dbg.rtc_ticked);
    lb_end(&l);

    if (g_rtc_s3_dbg.dry_run) {
        lb_begin(&l);
        lb_dec(&l, "elapsed_s", g_rtc_s3_dbg.elapsed_s);
        lb_dec(&l, "first_hit_s", g_rtc_s3_dbg.first_hit_s);
        lb_end(&l);
    }
}

void rtc_s3_dbg_dump(void)
{
    dbg_dump_impl(true);
}

void rtc_s3_set_logger(rtc_s3_log_fn fn)
{
    g_log = fn;
}

const char *rtc_s3_strerror(int rc)
{
    switch (rc) {
    case RTC_S3_OK:               return "ok: RTC alarm signalled PM1 (RTC_STS)";
    case RTC_S3_RESUMED_OTHER:    return "woke, but PM1_STS.RTC_STS not set";
    case RTC_S3_ERR_ARG:          return "bad delay argument";
    case RTC_S3_ERR_NO_FIXED_RTC: return "FADT FIX_RTC set: no fixed-feature RTC wake";
    case RTC_S3_ERR_RTC_INVALID:  return "RTC invalid (VRT clear, SET set, or bad time)";
    case RTC_S3_ERR_RTC_TIMEOUT:  return "RTC read timeout (UIP stuck / unstable reads)";
    case RTC_S3_ERR_RTC_EN_MASKED:return "PM1_EN.RTC_EN did not stick";
    case RTC_S3_ERR_MARGIN:       return "alarm too close or already passed";
    case RTC_S3_ERR_SLEEP_FAILED: return "SLP_EN written but WAK_STS never set";
    case RTC_S3_ERR_NOT_CONFIGURED:return "rtc_s3_init() not called";
    case RTC_S3_ERR_CONFIG:       return "config rejected";
    case RTC_S3_ERR_NO_SCI:       return "SCI_EN clear and ACPI_ENABLE failed/unknown";
    case RTC_S3_ERR_RTC_STUCK:    return "RTC seconds never advanced";
    case RTC_S3_ERR_STS_STUCK:    return "PM1_STS RTC/WAK bits will not clear";
    case RTC_S3_DRY_NO_PM1:       return "AF fired but RTC_STS never set: RTC not wired to PM1 fixed event";
    case RTC_S3_DRY_NO_ALARM:     return "no alarm seen: neither AF nor RTC_STS";
    case RTC_S3_DRY_PM1_ONLY:     return "RTC_STS set but AF never seen";
    case RTC_S3_ERR_ACPI_RSDP:    return "RSDP missing or bad checksum";
    case RTC_S3_ERR_ACPI_SDT:     return "no usable XSDT/RSDT";
    case RTC_S3_ERR_ACPI_NO_FADT: return "no FADT in root table";
    case RTC_S3_ERR_ACPI_BAD_FADT:return "FADT bad checksum or too short";
    case RTC_S3_ERR_ACPI_NO_PM1:  return "PM1a block missing or not in I/O space";
    case RTC_S3_ERR_ACPI_NO_S3:   return "no static _S3 package in DSDT";
    default:                      return "unknown";
    }
}

/* ------------------------------------------------------------ polling */
/*
 * Dry-run poll. Reads PM1_STS first, then Reg C (read-to-clear), latching
 * every bit seen. Time base: cfg.now_ms if given, else the RTC seconds
 * register itself (which also proves the RTC ticks).
 */
static int rtc_s3_poll(const arm_t *st, uint32_t delay)
{
    const uint32_t window = delay + RTC_S3_DRY_GRACE_S;
    const uint32_t spin_limit = g_cfg.dry_spin_limit ? g_cfg.dry_spin_limit
                                                      : RTC_S3_DRY_SPIN_DEFAULT;
    const uint64_t t0 = g_cfg.now_ms ? g_cfg.now_ms() : 0;
    uint32_t rtc_elapsed = 0, elapsed = 0, first_hit = 0;
    uint16_t sts_acc = 0;
    uint8_t regc_acc = 0, sec = 0, last_sec = 0;
    bool have_last = false, af = false, sts = false, any = false;
    int rc;

    for (uint32_t i = 0; i < spin_limit; i++) {
        uint16_t p = pm1_sts_read();
        uint8_t  c = cmos_read(RTC_REG_C);

        sts_acc = (uint16_t)(sts_acc | p);
        regc_acc = (uint8_t)(regc_acc | c);
        if (p & PM1_STS_RTC)
            sts = true;
        if (c & RTC_C_AF)
            af = true;

        if (rtc_peek_sec(st->regb, &sec)) {
            if (!have_last) {
                last_sec = sec;
                have_last = true;
            } else if (sec != last_sec) {
                rtc_elapsed += (uint32_t)(sec + 60u - last_sec) % 60u;
                last_sec = sec;
            }
        }
        elapsed = g_cfg.now_ms ? (uint32_t)((g_cfg.now_ms() - t0) / 1000u)
                               : rtc_elapsed;

        if (!any && (af || sts)) {
            any = true;
            first_hit = elapsed;
        }
        if (af && sts)
            break;
        if (any && elapsed >= first_hit + RTC_S3_DRY_HIT_GRACE_S)
            break;
        if (elapsed >= window)
            break;
    }

    g_rtc_s3_dbg.elapsed_s = elapsed;
    g_rtc_s3_dbg.first_hit_s = first_hit;
    g_rtc_s3_dbg.pm1_sts_post = sts_acc;
    g_rtc_s3_dbg.rtc_regc_post = regc_acc;
    g_rtc_s3_dbg.af_seen = af ? 1 : 0;
    g_rtc_s3_dbg.sts_seen = sts ? 1 : 0;
    g_rtc_s3_dbg.rtc_ticked = rtc_elapsed > 0 ? 1 : 0;

    if (rtc_elapsed == 0 && !af && !sts)
        return RTC_S3_ERR_RTC_STUCK;

    if (af && sts)
        rc = RTC_S3_DRY_ALARM_OK;
    else if (af)
        rc = RTC_S3_DRY_NO_PM1;
    else if (sts)
        rc = RTC_S3_DRY_PM1_ONLY;
    else
        rc = RTC_S3_DRY_NO_ALARM;

    if (rc == RTC_S3_DRY_ALARM_OK && !g_rtc_s3_dbg.rtc_en_ok)
        rc = RTC_S3_ERR_RTC_EN_MASKED;    /* status works, but wake is gated */
    return rc;
}

/* ------------------------------------------------------------- core ops */
static int rtc_s3_dry_locked(uint32_t delay)
{
    arm_t st;
    int rc = rtc_s3_arm(delay, false, &st);

    if (rc)
        return rc;
    rc = rtc_s3_check_margin(&st, delay);
    if (rc)
        return rc;
    return rtc_s3_poll(&st, delay);
}

static int rtc_s3_sleep_locked(uint32_t delay)
{
    arm_t st;
    int rc = rtc_s3_arm(delay, true, &st);

    if (rc)
        return rc;

    cpu_wbinvd();

    rc = rtc_s3_check_margin(&st, delay);
    if (rc)
        return rc;

    g_rtc_s3_dbg.pm1_cnt_pre = pio_inw(g_cfg.pm1a_cnt);
    log_line("rtc-s3: armed, writing SLP_TYP|SLP_EN now");
    dbg_dump_impl(false);                        /* reaches UART before power-down */

    pm1_cnt_slp(false);                          /* SLP_TYP */
    pm1_cnt_slp(true);                           /* SLP_TYP | SLP_EN */

    for (uint32_t i = 0; i < WAK_SPIN_LIMIT; i++) {
        if (pm1_sts_read() & PM1_STS_WAK) {
            g_rtc_s3_dbg.rtc_regc_post = cmos_read(RTC_REG_C);
            g_rtc_s3_dbg.pm1_sts_post  = pm1_sts_read();
            g_rtc_s3_dbg.af_seen  = (g_rtc_s3_dbg.rtc_regc_post & RTC_C_AF) ? 1 : 0;
            g_rtc_s3_dbg.sts_seen = (g_rtc_s3_dbg.pm1_sts_post & PM1_STS_RTC) ? 1 : 0;
            return (g_rtc_s3_dbg.pm1_sts_post & PM1_STS_RTC)
                       ? RTC_S3_RESUMED_RTC : RTC_S3_RESUMED_OTHER;
        }
    }
    return RTC_S3_ERR_SLEEP_FAILED;
}

int rtc_s3_dry_run(uint32_t seconds_delay)
{
    uint64_t flags;
    int rc;

    if (!g_cfg_ok)
        return RTC_S3_ERR_NOT_CONFIGURED;
    if (seconds_delay < RTC_S3_MIN_DELAY_S || seconds_delay > RTC_S3_DRY_MAX_DELAY_S)
        return RTC_S3_ERR_ARG;

    dbg_reset(true);
    flags = irq_save();
    rc = rtc_s3_dry_locked(seconds_delay);
    rtc_disarm();                                /* never leave a stale alarm armed */
    irq_restore(flags);
    g_rtc_s3_dbg.result = rc;
    return rc;
}

int configure_rtc_s3_sleep(uint32_t seconds_delay)
{
    uint64_t flags;
    int rc;

    if (!g_cfg_ok)
        return RTC_S3_ERR_NOT_CONFIGURED;
    if (seconds_delay < RTC_S3_MIN_DELAY_S || seconds_delay > RTC_S3_MAX_DELAY_S)
        return RTC_S3_ERR_ARG;
    if (g_cfg.fix_rtc)
        return RTC_S3_ERR_NO_FIXED_RTC;

    dbg_reset(false);
    flags = irq_save();
    rc = rtc_s3_sleep_locked(seconds_delay);
    rtc_disarm();                                /* never leave a stale alarm armed */
    irq_restore(flags);
    g_rtc_s3_dbg.result = rc;
    return rc;
}

int rtc_s3_selftest(uint32_t delay_seconds, bool dry_run)
{
    lb_t l;
    int rc;

    if (!g_cfg_ok) {
        log_line("rtc-s3: selftest: not configured (call rtc_s3_init)");
        return RTC_S3_ERR_NOT_CONFIGURED;
    }
    log_cfg();
    lb_begin(&l);
    lb_str(&l, dry_run ? " selftest: DRY RUN (no SLP_EN)"
                       : " selftest: REAL S3 (CPU state is lost until resume)");
    lb_dec(&l, "delay_s", delay_seconds);
    lb_end(&l);

    rc = dry_run ? rtc_s3_dry_run(delay_seconds)
                 : configure_rtc_s3_sleep(delay_seconds);

    if (rc == RTC_S3_ERR_ARG || rc == RTC_S3_ERR_NO_FIXED_RTC ||
        rc == RTC_S3_ERR_NOT_CONFIGURED) {
        lb_begin(&l);
        lb_int(&l, "rc", rc);
        lb_str(&l, " (");
        lb_str(&l, rtc_s3_strerror(rc));
        lb_str(&l, ")");
        lb_end(&l);
    } else {
        rtc_s3_dbg_dump();
    }
    return rc;
}

/* ------------------------------------------------------------- config */
void rtc_s3_config_defaults(rtc_s3_config_t *c)
{
    c->pm1a_evt = 0;
    c->pm1b_evt = 0;
    c->pm1a_cnt = 0;
    c->pm1b_cnt = 0;
    c->pm1_evt_len = 4;
    c->slp_typa = 0;
    c->slp_typb = 0;
    c->fix_rtc = false;
    c->smi_cmd = 0;
    c->acpi_enable = 0;
    c->rtc_day_alarm_idx = 0;
    c->rtc_mon_alarm_idx = 0;
    c->rtc_alarm_dontcare = 0xFF;   /* what Linux rtc-cmos writes for "any" */
    c->cmos_nmi_bit = 0;
    c->facs_phys = 0;
    c->now_ms = NULL;
    c->dry_spin_limit = 0;
}

static bool cmos_alarm_idx_ok(uint8_t idx)
{
    return idx == 0 || (idx > RTC_REG_D && idx < 0x80);
}

int rtc_s3_init(const rtc_s3_config_t *c)
{
    if (!c)
        return RTC_S3_ERR_CONFIG;
    if (!c->pm1a_evt || !c->pm1a_cnt)
        return RTC_S3_ERR_CONFIG;
    if (c->pm1_evt_len < 4 || (c->pm1_evt_len & 1))
        return RTC_S3_ERR_CONFIG;
    if (c->slp_typa > 7 || c->slp_typb > 7)
        return RTC_S3_ERR_CONFIG;
    if (!cmos_alarm_idx_ok(c->rtc_day_alarm_idx) ||
        !cmos_alarm_idx_ok(c->rtc_mon_alarm_idx))
        return RTC_S3_ERR_CONFIG;               /* never write the clock regs */
    if (c->cmos_nmi_bit != 0 && c->cmos_nmi_bit != 0x80)
        return RTC_S3_ERR_CONFIG;

    g_cfg.pm1a_evt = c->pm1a_evt;
    g_cfg.pm1b_evt = c->pm1b_evt;
    g_cfg.pm1a_cnt = c->pm1a_cnt;
    g_cfg.pm1b_cnt = c->pm1b_cnt;
    g_cfg.pm1_evt_len = c->pm1_evt_len;
    g_cfg.slp_typa = c->slp_typa;
    g_cfg.slp_typb = c->slp_typb;
    g_cfg.fix_rtc = c->fix_rtc;
    g_cfg.smi_cmd = c->smi_cmd;
    g_cfg.acpi_enable = c->acpi_enable;
    g_cfg.rtc_day_alarm_idx = c->rtc_day_alarm_idx;
    g_cfg.rtc_mon_alarm_idx = c->rtc_mon_alarm_idx;
    g_cfg.rtc_alarm_dontcare = c->rtc_alarm_dontcare;
    g_cfg.cmos_nmi_bit = c->cmos_nmi_bit;
    g_cfg.facs_phys = c->facs_phys;
    g_cfg.now_ms = c->now_ms;
    g_cfg.dry_spin_limit = c->dry_spin_limit;
    g_cfg_ok = true;
    return 0;
}

/* ------------------------------------------------------- ACPI discovery */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool sig_eq(const uint8_t *p, const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i] != (uint8_t)s[i])
            return false;
    return true;
}

static bool sum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t s = 0;

    for (uint32_t i = 0; i < len; i++)
        s = (uint8_t)(s + p[i]);
    return s == 0;
}

static const uint8_t *map_tbl(rtc_s3_map_fn map, uint64_t phys, uint32_t len)
{
    if (!phys || !len)
        return NULL;
    if (map)
        return (const uint8_t *)map(phys, len);
    return (const uint8_t *)(uintptr_t)phys;     /* identity */
}

/* Map an SDT: header first for the length, then the whole table. */
static const uint8_t *sdt_map(rtc_s3_map_fn map, uint64_t phys, uint32_t *len_out,
                              bool verify)
{
    const uint8_t *h = map_tbl(map, phys, ACPI_HDR_LEN);
    const uint8_t *t;
    uint32_t len;

    if (!h)
        return NULL;
    len = rd32(h + 4);
    if (len < ACPI_HDR_LEN || len > ACPI_MAX_TABLE)
        return NULL;
    t = map_tbl(map, phys, len);
    if (!t || (verify && !sum_ok(t, len)))
        return NULL;
    *len_out = len;
    return t;
}

static int acpi_find_fadt(rtc_s3_map_fn map, uint64_t rsdp_phys,
                          const uint8_t **out, uint32_t *out_len)
{
    const uint8_t *r = map_tbl(map, rsdp_phys, 20);
    uint64_t root[2];
    uint8_t esz[2];
    int nroot = 0;

    if (!r || !sig_eq(r, "RSD PTR ", 8) || !sum_ok(r, 20))
        return RTC_S3_ERR_ACPI_RSDP;

    if (r[15] >= 2) {                            /* ACPI 2.0+ RSDP */
        uint32_t rl;
        uint64_t xsdt;

        r = map_tbl(map, rsdp_phys, 36);
        if (!r)
            return RTC_S3_ERR_ACPI_RSDP;
        rl = rd32(r + 20);
        if (rl < 36 || rl > 64)
            return RTC_S3_ERR_ACPI_RSDP;
        r = map_tbl(map, rsdp_phys, rl);
        if (!r || !sum_ok(r, rl))
            return RTC_S3_ERR_ACPI_RSDP;
        xsdt = rd64(r + 24);
        if (xsdt) {
            root[nroot] = xsdt;
            esz[nroot++] = 8;
        }
    }
    {
        uint32_t rsdt = rd32(r + 16);
        if (rsdt) {
            root[nroot] = rsdt;
            esz[nroot++] = 4;
        }
    }
    if (!nroot)
        return RTC_S3_ERR_ACPI_SDT;

    for (int k = 0; k < nroot; k++) {
        uint32_t len;
        const uint8_t *tbl = sdt_map(map, root[k], &len, true);
        uint32_t n;

        if (!tbl || !sig_eq(tbl, esz[k] == 8 ? "XSDT" : "RSDT", 4))
            continue;
        n = (len - ACPI_HDR_LEN) / esz[k];
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = tbl + ACPI_HDR_LEN + (uint64_t)i * esz[k];
            uint64_t pa = esz[k] == 8 ? rd64(e) : rd32(e);
            const uint8_t *h = map_tbl(map, pa, ACPI_HDR_LEN);

            if (!h || !sig_eq(h, "FACP", 4))
                continue;
            *out = sdt_map(map, pa, out_len, true);
            return *out ? 0 : RTC_S3_ERR_ACPI_BAD_FADT;
        }
    }
    return RTC_S3_ERR_ACPI_NO_FADT;
}

/* Port numbers must fit 16 bits; 0 means "absent". */
static bool io_port(uint64_t a, uint16_t *out)
{
    if (a > 0xFFFFu)
        return false;
    *out = (uint16_t)a;
    return true;
}
static int fadt_parse(const uint8_t *f, uint32_t len, rtc_s3_config_t *c,
                      uint64_t *dsdt)
{
    uint8_t rev = f[8];
    uint64_t addr[4];                 /* pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt */
    uint64_t facs;

    if (len < 116)
        return RTC_S3_ERR_ACPI_BAD_FADT;

    c->fix_rtc = (rd32(f + 112) & ACPI_FADT_FIX_RTC) != 0;
    if (!io_port(rd32(f + 48), &c->smi_cmd))
        c->smi_cmd = 0;
    c->acpi_enable = f[52];
    c->pm1_evt_len = f[88];
    c->rtc_day_alarm_idx = f[106];
    c->rtc_mon_alarm_idx = f[107];

    addr[0] = rd32(f + 56);
    addr[1] = rd32(f + 60);
    addr[2] = rd32(f + 64);
    addr[3] = rd32(f + 68);
    facs = rd32(f + 36);
    *dsdt = rd32(f + 40);

    if (rev >= 2 && len >= 196) {
        for (int k = 0; k < 4; k++) {            /* X_PM1a_EVT, b_EVT, a_CNT, b_CNT */
            const uint8_t *g = f + 148 + 12 * k;
            uint64_t x = rd64(g + 4);

            if (x && g[0] == 1)
                addr[k] = x;                     /* System I/O GAS wins */
            else if (x && !addr[k])
                return RTC_S3_ERR_ACPI_NO_PM1;   /* only a memory-mapped PM1 */
        }
    }
    if (rev >= 2 && len >= 140) {
        if (rd64(f + 132))
            facs = rd64(f + 132);
        if (len >= 148 && rd64(f + 140))
            *dsdt = rd64(f + 140);
    }
    c->facs_phys = facs;

    if (!io_port(addr[0], &c->pm1a_evt) || !io_port(addr[1], &c->pm1b_evt) ||
        !io_port(addr[2], &c->pm1a_cnt) || !io_port(addr[3], &c->pm1b_cnt))
        return RTC_S3_ERR_ACPI_NO_PM1;
    if (!c->pm1a_evt || !c->pm1a_cnt)
        return RTC_S3_ERR_ACPI_NO_PM1;
    return 0;
}

/*
 * Heuristic AML scan for  Name(_S3_, Package(){ a, b, ... })  with constant
 * byte elements. Good for the usual static package; if _S3 is computed in AML
 * this fails and you need a real parser (uACPI / lai) or explicit values.
 */
static bool aml_find_s3(const uint8_t *d, uint32_t len, uint8_t *a, uint8_t *b)
{
    const uint8_t *end = d + len;

    for (uint32_t i = 1; i + 4 <= len; i++) {
        const uint8_t *p;
        uint8_t v[2];
        bool ok = true;

        if (d[i] != '_' || d[i + 1] != 'S' || d[i + 2] != '3' || d[i + 3] != '_')
            continue;
        if (!(d[i - 1] == 0x08 || (d[i - 1] == 0x5C && i >= 2 && d[i - 2] == 0x08)))
            continue;                            /* Name( or Name(\ */
        p = d + i + 4;
        if (p >= end || *p++ != 0x12)            /* PackageOp */
            continue;
        if (p >= end)
            continue;
        p += 1 + ((*p >> 6) & 3);                /* PkgLength */
        if (p >= end)
            continue;
        p++;                                     /* NumElements */
        for (int k = 0; k < 2 && ok; k++) {
            if (p >= end) {
                ok = false;
            } else if (*p == 0x0A) {             /* BytePrefix */
                p++;
                if (p >= end)
                    ok = false;
                else
                    v[k] = *p++;
            } else if (*p == 0x00 || *p == 0x01) {   /* ZeroOp / OneOp */
                v[k] = *p++;
            } else {
                ok = false;
            }
        }
        if (ok) {
            *a = v[0];
            *b = v[1];
            return true;
        }
    }
    return false;
}

int rtc_s3_config_from_acpi(rtc_s3_config_t *cfg, uint64_t rsdp_phys,
                            rtc_s3_map_fn map)
{
    const uint8_t *fadt, *dsdt;
    uint32_t flen, dlen;
    uint64_t dsdt_phys = 0;
    uint8_t a, b;
    int rc;

    if (!cfg)
        return RTC_S3_ERR_CONFIG;
    rtc_s3_config_defaults(cfg);

    rc = acpi_find_fadt(map, rsdp_phys, &fadt, &flen);
    if (rc)
        return rc;
    rc = fadt_parse(fadt, flen, cfg, &dsdt_phys);   /* fadt not used after this */
    if (rc)
        return rc;

    dsdt = dsdt_phys ? sdt_map(map, dsdt_phys, &dlen, false) : NULL;
    if (!dsdt || !aml_find_s3(dsdt + ACPI_HDR_LEN, dlen - ACPI_HDR_LEN, &a, &b))
        return RTC_S3_ERR_ACPI_NO_S3;
    cfg->slp_typa = a;
    cfg->slp_typb = b;
    return 0;
}