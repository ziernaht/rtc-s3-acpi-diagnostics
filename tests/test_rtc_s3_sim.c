/*
 * test_rtc_s3_sim.c - host-side tests for rtc_s3_sleep.c
 *
 * Simulates a CMOS RTC + ACPI PM1a/PM1b block (port I/O is replaced via
 * RTC_S3_HOST_SIM) and builds synthetic ACPI tables, so the dry run, the real
 * S3 path and the RSDP->XSDT->FADT->DSDT discovery all run on the host.
 *
 *   gcc -std=c11 -g -fsanitize=address,undefined -Wall -Wextra \
 *       test_rtc_s3_sim.c -o test_rtc_s3_sim && ./test_rtc_s3_sim
 */
#define RTC_S3_HOST_SIM 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define SIM_STS   0x400
#define SIM_EN    0x402
#define SIM_CNT   0x404
#define SIM_STS_B 0x410
#define SIM_EN_B  0x412
#define SIM_CNT_B 0x414
#define SIM_SMI   0xB2
#define SIM_ACPI_ENABLE 0xA0
#define BIT_RTC   (1u << 10)
#define BIT_WAK   (1u << 15)
#define BIT_SLPEN (1u << 13)

/* ------------------------------------------------------ simulated hardware */
static struct {
    uint8_t  cmos[128];
    uint8_t  idx, regb, regc, alarm[3];
    uint64_t io;                    /* port accesses so far                   */
    uint32_t io_per_s;              /* accesses per simulated second          */
    uint32_t base_sod, last_sod;
    uint64_t off_s;                 /* time jumped during "sleep"             */
    bool     freeze;                /* RTC not ticking                        */
    bool     alarm_works;           /* alarm hardware functional              */
    bool     wired_pm1;             /* AF&AIE reaches PM1_STS.RTC_STS         */
    bool     en_sticks;             /* PM1_EN.RTC_EN writable                 */
    bool     sts_stuck;             /* PM1_STS W1C broken                     */
    uint16_t sts, en, cnt;          /* PM1a                                   */
    uint16_t sts_b, en_b, cnt_b;    /* PM1b                                   */
    bool     en_b_saw_rtc;
    uint16_t cnt_writes[8], cnt_b_writes[8];
    int      n_cnt_writes, n_cnt_b_writes;
    int      slp_en_writes, wbinvd_calls, irq_saved, irq_restored;
} S;

static uint32_t sim_sod(void)
{
    uint64_t t = (uint64_t)S.base_sod + S.off_s + (S.freeze ? 0 : S.io / S.io_per_s);
    return (uint32_t)(t % 86400u);
}

static uint8_t sim_bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void sim_encode(uint32_t sod, uint8_t regb, uint8_t out[3])
{
    unsigned h = sod / 3600u, m = (sod / 60u) % 60u, s = sod % 60u, pm = 0;

    if (!(regb & 2)) { pm = h >= 12; h %= 12; if (h == 0) h = 12; }
    if (!(regb & 4)) { out[0] = sim_bcd(s); out[1] = sim_bcd(m); out[2] = sim_bcd(h); }
    else             { out[0] = (uint8_t)s; out[1] = (uint8_t)m; out[2] = (uint8_t)h; }
    if (pm) out[2] |= 0x80;
}

static int64_t sim_decode(const uint8_t r[3], uint8_t regb)
{
    unsigned s = r[0], m = r[1], h = r[2], pm = 0;

    if (!(regb & 2)) { pm = (h & 0x80) != 0; h &= 0x7F; }
    if (!(regb & 4)) { s = (s >> 4) * 10 + (s & 15); m = (m >> 4) * 10 + (m & 15);
                       h = (h >> 4) * 10 + (h & 15); }
    if (!(regb & 2)) { if (h == 12) h = 0; if (pm) h += 12; }
    return (int64_t)h * 3600 + (int64_t)m * 60 + s;
}

static void sim_fire(void)
{
    S.regc |= 0x20;                                 /* AF */
    if (S.regb & 0x20) {                            /* AIE -> IRQ8 */
        S.regc |= 0x80;
        if (S.wired_pm1) S.sts |= (uint16_t)BIT_RTC;
    }
}

static void sim_update(void)
{
    uint32_t cur = sim_sod();

    if (cur == S.last_sod) return;
    if (S.alarm_works) {
        int64_t a = sim_decode(S.alarm, S.regb);
        uint32_t dp = (uint32_t)((a + 86400 - S.last_sod) % 86400);
        uint32_t dc = (cur + 86400u - S.last_sod) % 86400u;
        if (dp != 0 && dp <= dc) sim_fire();
    }
    S.last_sod = cur;
}

static void sim_tick(void) { S.io++; sim_update(); }

static void sim_sleep(void)
{
    S.slp_en_writes++;
    if ((S.regb & 0x20) && (S.en & BIT_RTC) && S.alarm_works) {
        int64_t a = sim_decode(S.alarm, S.regb);
        uint32_t cur = sim_sod();
        uint32_t delta = (uint32_t)((a + 86400 - cur) % 86400);
        if (!delta) delta = 86400;
        S.off_s += delta;
        S.last_sod = sim_sod();
        sim_fire();
        S.sts |= (uint16_t)BIT_WAK;
    }
}

static inline void pio_outb(uint16_t port, uint8_t v)
{
    sim_tick();
    if (port == 0x70) {
        S.idx = v & 0x7F;
    } else if (port == 0x71) {
        switch (S.idx) {
        case 0x01: S.alarm[0] = v; break;
        case 0x03: S.alarm[1] = v; break;
        case 0x05: S.alarm[2] = v; break;
        case 0x0B: S.regb = v; break;
        default:   S.cmos[S.idx] = v; break;
        }
    } else if (port == SIM_SMI && v == SIM_ACPI_ENABLE) {
        S.cnt |= 1;
    }
}

static inline uint8_t pio_inb(uint16_t port)
{
    uint8_t t[3], r;

    sim_tick();
    if (port != 0x71) return 0xFF;
    sim_encode(sim_sod(), S.regb, t);
    switch (S.idx) {
    case 0x00: return t[0];
    case 0x02: return t[1];
    case 0x04: return t[2];
    case 0x01: return S.alarm[0];
    case 0x03: return S.alarm[1];
    case 0x05: return S.alarm[2];
    case 0x0A: return (S.io % S.io_per_s) < 3 ? 0xA6 : 0x26;   /* UIP near edge */
    case 0x0B: return S.regb;
    case 0x0C: r = S.regc; S.regc = 0; return r;
    case 0x0D: return 0x80;                                    /* VRT */
    default:   return S.cmos[S.idx];
    }
}

static inline void pio_outw(uint16_t port, uint16_t v)
{
    sim_tick();
    switch (port) {
    case SIM_STS:   if (!S.sts_stuck) S.sts = (uint16_t)(S.sts & (uint16_t)~v); break;
    case SIM_EN:    S.en = S.en_sticks ? v : (uint16_t)(v & (uint16_t)~BIT_RTC); break;
    case SIM_CNT:
        if (S.n_cnt_writes < 8) S.cnt_writes[S.n_cnt_writes++] = v;
        S.cnt = (uint16_t)(v & (uint16_t)~BIT_SLPEN);
        if (v & BIT_SLPEN) sim_sleep();
        break;
    case SIM_STS_B: S.sts_b = (uint16_t)(S.sts_b & (uint16_t)~v); break;
    case SIM_EN_B:  S.en_b = v; if (v & BIT_RTC) S.en_b_saw_rtc = true; break;
    case SIM_CNT_B:
        if (S.n_cnt_b_writes < 8) S.cnt_b_writes[S.n_cnt_b_writes++] = v;
        S.cnt_b = v;
        break;
    default: break;
    }
}

static inline uint16_t pio_inw(uint16_t port)
{
    sim_tick();
    switch (port) {
    case SIM_STS:   return S.sts;
    case SIM_EN:    return S.en;
    case SIM_CNT:   return S.cnt;
    case SIM_STS_B: return S.sts_b;
    case SIM_EN_B:  return S.en_b;
    case SIM_CNT_B: return S.cnt_b;
    default:        return 0xFFFF;
    }
}

static inline uint64_t irq_save(void) { S.irq_saved++; return 0x200; }
static inline void irq_restore(uint64_t f) { (void)f; S.irq_restored++; }
static inline void cpu_wbinvd(void) { S.wbinvd_calls++; }

#include "rtc_s3_sleep.c"

/* ---------------------------------------------------------------- helpers */
static rtc_s3_config_t cfg;

static void setup(uint8_t regb, uint32_t sod)
{
    memset(&S, 0, sizeof S);
    S.regb = regb;
    S.base_sod = sod;
    S.last_sod = sod;
    S.io_per_s = 400;
    S.alarm_works = S.wired_pm1 = S.en_sticks = true;
    S.cnt = 1;                                      /* SCI_EN */
    rtc_s3_config_defaults(&cfg);
    cfg.pm1a_evt = SIM_STS;
    cfg.pm1a_cnt = SIM_CNT;
    cfg.slp_typa = 5;
}

static void apply(void) { int rc = rtc_s3_init(&cfg); assert(rc == 0); (void)rc; }

static uint64_t sim_now_ms(void) { return S.io * 1000u / S.io_per_s; }

static char loglines[128][160];
static int  nlog;
static bool echo_log;
static void test_log(const char *s)
{
    if (nlog < 128) { strncpy(loglines[nlog], s, 159); loglines[nlog][159] = 0; nlog++; }
    if (echo_log) printf("    UART> %s\n", s);
}
static bool log_has(const char *needle)
{
    for (int i = 0; i < nlog; i++) if (strstr(loglines[i], needle)) return true;
    return false;
}

static void check_disarmed(void)
{
    assert(!(S.regb & 0x20));
    assert(!(S.en & BIT_RTC));
    assert(!(S.sts & BIT_RTC));
    assert(S.irq_saved == 1 && S.irq_restored == 1);
}

/* --------------------------------------------------------- ACPI test data */
typedef struct { uint64_t phys; uint8_t *p; uint32_t len; } region_t;
static region_t regions[8];
static int nreg;

static const void *test_map(uint64_t phys, uint32_t len)
{
    for (int i = 0; i < nreg; i++)
        if (phys >= regions[i].phys && phys + len <= regions[i].phys + regions[i].len)
            return regions[i].p + (phys - regions[i].phys);
    return NULL;
}
static void reg_add(uint64_t phys, uint8_t *p, uint32_t len)
{
    regions[nreg].phys = phys; regions[nreg].p = p; regions[nreg].len = len; nreg++;
}
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void fixsum(uint8_t *t, uint32_t len, uint32_t off)
{
    uint8_t s = 0;
    t[off] = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + t[i]);
    t[off] = (uint8_t)(0u - s);
}

static uint8_t t_rsdp[36], t_rsdt[44], t_xsdt[52], t_apic[36], t_fadt[276], t_dsdt[128];

typedef struct {
    uint8_t  rev;            /* 1: 116-byte FADT + RSDT; >=2: 276-byte FADT + XSDT */
    bool     legacy_ports, x_ports;
    uint8_t  x_space;        /* GAS address space for X_PM1 blocks (1 = I/O) */
    uint32_t flags;
    const uint8_t *dsdt_body;
    uint32_t dsdt_body_len;
    bool     bad_fadt_sum, bad_rsdp_sum;
} acpi_p;

static uint64_t build_acpi(const acpi_p *a)
{
    bool x = a->rev >= 2;
    uint64_t fadt_phys = x ? 0x17F001000ull : 0x7F001000ull;
    uint32_t flen = x ? 276 : 116, dlen = 36 + a->dsdt_body_len;

    memset(t_rsdp, 0, sizeof t_rsdp);  memset(t_rsdt, 0, sizeof t_rsdt);
    memset(t_xsdt, 0, sizeof t_xsdt);  memset(t_apic, 0, sizeof t_apic);
    memset(t_fadt, 0, sizeof t_fadt);  memset(t_dsdt, 0, sizeof t_dsdt);
    nreg = 0;

    memcpy(t_dsdt, "DSDT", 4); put32(t_dsdt + 4, dlen); t_dsdt[8] = 2;
    memcpy(t_dsdt + 36, a->dsdt_body, a->dsdt_body_len);
    fixsum(t_dsdt, dlen, 9);

    memcpy(t_apic, "APIC", 4); put32(t_apic + 4, 36); t_apic[8] = 1;
    fixsum(t_apic, 36, 9);

    memcpy(t_fadt, "FACP", 4); put32(t_fadt + 4, flen); t_fadt[8] = a->rev;
    put32(t_fadt + 36, 0x7F003000); put32(t_fadt + 40, 0x7F002000);
    put32(t_fadt + 48, 0xB2); t_fadt[52] = 0xA0; t_fadt[53] = 0xA1;
    if (a->legacy_ports) { put32(t_fadt + 56, 0x400); put32(t_fadt + 64, 0x404); }
    t_fadt[88] = 4; t_fadt[89] = 2; t_fadt[106] = 0x7D; t_fadt[107] = 0x7E;
    put32(t_fadt + 112, a->flags);
    if (x) {
        put64(t_fadt + 132, 0x7F003000); put64(t_fadt + 140, 0x7F002000);
        if (a->x_ports) {
            t_fadt[148] = a->x_space; t_fadt[149] = 32; put64(t_fadt + 152, 0x400);
            t_fadt[172] = a->x_space; t_fadt[173] = 16; put64(t_fadt + 176, 0x404);
        }
    }
    fixsum(t_fadt, flen, 9);
    if (a->bad_fadt_sum) t_fadt[9] ^= 0x55;

    if (x) {
        memcpy(t_xsdt, "XSDT", 4); put32(t_xsdt + 4, 52); t_xsdt[8] = 1;
        put64(t_xsdt + 36, 0x7F000200); put64(t_xsdt + 44, fadt_phys);   /* decoy first */
        fixsum(t_xsdt, 52, 9);
    } else {
        memcpy(t_rsdt, "RSDT", 4); put32(t_rsdt + 4, 44); t_rsdt[8] = 1;
        put32(t_rsdt + 36, 0x7F000200); put32(t_rsdt + 40, (uint32_t)fadt_phys);
        fixsum(t_rsdt, 44, 9);
    }

    memcpy(t_rsdp, "RSD PTR ", 8);
    t_rsdp[15] = x ? 2 : 0;
    put32(t_rsdp + 16, x ? 0 : 0x7F000000);
    if (x) { put32(t_rsdp + 20, 36); put64(t_rsdp + 24, 0x7F000100); }
    fixsum(t_rsdp, 20, 8);
    if (x) fixsum(t_rsdp, 36, 32);
    if (a->bad_rsdp_sum) t_rsdp[8] ^= 0x55;

    reg_add(0xE0000, t_rsdp, 36);
    if (x) reg_add(0x7F000100, t_xsdt, 52); else reg_add(0x7F000000, t_rsdt, 44);
    reg_add(0x7F000200, t_apic, 36);
    reg_add(fadt_phys, t_fadt, flen);
    reg_add(0x7F002000, t_dsdt, dlen);
    return 0xE0000;
}

/* Name(_S3_, Package(4){5,5,0,0}) */
static const uint8_t AML_S3_5_5[]  = { 0x08,0x5F,0x53,0x33,0x5F, 0x12,0x08,0x04, 0x0A,0x05, 0x0A,0x05, 0x00,0x00 };
/* Name(\_S3_, Package(4){1,0,0,0}) with OneOp/ZeroOp elements */
static const uint8_t AML_S3_ROOT[] = { 0x08,0x5C,0x5F,0x53,0x33,0x5F, 0x12,0x06,0x04, 0x01,0x00,0x00,0x00 };
/* only _S5_ present */
static const uint8_t AML_S5_ONLY[] = { 0x08,0x5F,0x53,0x35,0x5F, 0x12,0x08,0x04, 0x0A,0x07, 0x0A,0x07, 0x00,0x00 };
/* _S3_ cut off after PackageOp */
static const uint8_t AML_S3_TRUNC[] = { 0x08,0x5F,0x53,0x33,0x5F, 0x12 };
/* "_S3_" bytes that are not a Name() declaration */
static const uint8_t AML_S3_DECOY[] = { 0x00,0x5F,0x53,0x33,0x5F, 0x12,0x08,0x04, 0x0A,0x05, 0x0A,0x05, 0x00,0x00 };

static acpi_p base_params(void)
{
    acpi_p a;
    memset(&a, 0, sizeof a);
    a.rev = 5; a.legacy_ports = true; a.x_space = 1;
    a.dsdt_body = AML_S3_5_5; a.dsdt_body_len = sizeof AML_S3_5_5;
    return a;
}

/* ------------------------------------------------------------------ tests */
static void test_not_configured(void)
{
    assert(rtc_s3_dry_run(10) == RTC_S3_ERR_NOT_CONFIGURED);
    assert(configure_rtc_s3_sleep(10) == RTC_S3_ERR_NOT_CONFIGURED);
    assert(rtc_s3_selftest(10, RTC_S3_DRY_RUN) == RTC_S3_ERR_NOT_CONFIGURED);
    puts("ok  not-configured guards");
}

static void test_init_validation(void)
{
    rtc_s3_config_t c;
    assert(rtc_s3_init(NULL) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.pm1a_evt = 0;           assert(rtc_s3_init(&c) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.pm1a_cnt = 0;           assert(rtc_s3_init(&c) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.pm1_evt_len = 3;        assert(rtc_s3_init(&c) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.slp_typa = 8;           assert(rtc_s3_init(&c) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.rtc_day_alarm_idx = 0x02; assert(rtc_s3_init(&c) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.rtc_mon_alarm_idx = 0x80; assert(rtc_s3_init(&c) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.cmos_nmi_bit = 0x40;    assert(rtc_s3_init(&c) == RTC_S3_ERR_CONFIG);
    setup(6, 0); c = cfg; c.rtc_day_alarm_idx = 0x7D; assert(rtc_s3_init(&c) == 0);
    puts("ok  rtc_s3_init validation");
}

static void test_dry_happy(void)
{
    const uint8_t modes[4] = { 6, 2, 4, 0 };                 /* DM|24H, 24H, DM, BCD/12h */
    const uint32_t starts[4] = { 10 * 3600 + 17 * 60 + 5, 86400 - 10, 0, 12 * 3600 + 59 };
    int runs = 0;

    for (int m = 0; m < 4; m++)
        for (int s = 0; s < 4; s++) {
            uint32_t elapsed;
            setup(modes[m], starts[s]);
            apply();
            assert(rtc_s3_dry_run(10) == RTC_S3_DRY_ALARM_OK);
            assert(g_rtc_s3_dbg.af_seen && g_rtc_s3_dbg.sts_seen);
            assert(g_rtc_s3_dbg.rtc_ticked && g_rtc_s3_dbg.rtc_en_ok && g_rtc_s3_dbg.dry_run);
            assert(g_rtc_s3_dbg.result == 0);
            assert(((g_rtc_s3_dbg.now_s + 86400u - starts[s]) % 86400u) <= 2);
            assert(g_rtc_s3_dbg.target_s == (g_rtc_s3_dbg.now_s + 10u) % 86400u);
            assert(sim_decode(S.alarm, S.regb) == (int64_t)g_rtc_s3_dbg.target_s);
            elapsed = g_rtc_s3_dbg.elapsed_s;
            assert(elapsed >= 8 && elapsed <= 12);
            assert(g_rtc_s3_dbg.pm1_sts_post & BIT_RTC);
            assert(g_rtc_s3_dbg.rtc_regc_post & 0x20);
            assert(S.slp_en_writes == 0 && S.n_cnt_writes == 0 && S.wbinvd_calls == 0);
            check_disarmed();
            runs++;
        }
    printf("ok  dry run succeeds in 4 RTC modes x 4 start times incl. midnight wrap (%d runs)\n", runs);
}

static void test_dry_findings(void)
{
    /* alarm fires but never reaches PM1 */
    setup(6, 40000); S.wired_pm1 = false; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_DRY_NO_PM1);
    assert(g_rtc_s3_dbg.af_seen && !g_rtc_s3_dbg.sts_seen);
    check_disarmed();

    /* no alarm at all, RTC ticking: polls the full delay+5 window */
    setup(6, 40000); S.alarm_works = false; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_DRY_NO_ALARM);
    assert(!g_rtc_s3_dbg.af_seen && !g_rtc_s3_dbg.sts_seen && g_rtc_s3_dbg.rtc_ticked);
    assert(g_rtc_s3_dbg.elapsed_s == 15);
    check_disarmed();

    /* RTC not ticking */
    setup(6, 40000); S.freeze = true; cfg.dry_spin_limit = 20000; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_ERR_RTC_STUCK);
    assert(!g_rtc_s3_dbg.rtc_ticked);
    check_disarmed();

    /* external ms clock bounds the window; frozen RTC still detected */
    setup(6, 40000); S.alarm_works = false; cfg.now_ms = sim_now_ms; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_DRY_NO_ALARM);
    assert(g_rtc_s3_dbg.elapsed_s >= 15 && g_rtc_s3_dbg.elapsed_s <= 16);
    setup(6, 40000); S.freeze = true; cfg.now_ms = sim_now_ms; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_ERR_RTC_STUCK);

    /* PM1_EN.RTC_EN does not stick: status path works but wake is gated */
    setup(6, 40000); S.en_sticks = false; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_ERR_RTC_EN_MASKED);
    assert(g_rtc_s3_dbg.af_seen && g_rtc_s3_dbg.sts_seen && !g_rtc_s3_dbg.rtc_en_ok);

    /* stale RTC_STS cannot be cleared: result would be meaningless */
    setup(6, 40000); S.sts_stuck = true; S.sts = (uint16_t)BIT_RTC; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_ERR_STS_STUCK);
    assert(!(S.regb & 0x20) && !(S.en & BIT_RTC));      /* disarmed even though sim STS is stuck */
    assert(S.irq_saved == 1 && S.irq_restored == 1);

    /* FIX_RTC does not block the dry run, only real S3 */
    setup(6, 40000); cfg.fix_rtc = true; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_DRY_ALARM_OK);

    /* SCI_EN clear: logged, not touched in dry run (no SMI_CMD write) */
    setup(6, 40000); S.cnt = 0; cfg.smi_cmd = SIM_SMI; cfg.acpi_enable = SIM_ACPI_ENABLE; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_DRY_ALARM_OK);
    assert(g_rtc_s3_dbg.sci_en == 0 && S.cnt == 0);

    /* day/month alarm "match any" values are written when the FADT has them */
    setup(6, 40000); cfg.rtc_day_alarm_idx = 0x7D; cfg.rtc_mon_alarm_idx = 0x7E; apply();
    assert(rtc_s3_dry_run(10) == RTC_S3_DRY_ALARM_OK);
    assert(S.cmos[0x7D] == 0xFF && S.cmos[0x7E] == 0xFF);

    /* argument range */
    setup(6, 40000); apply();
    assert(rtc_s3_dry_run(2) == RTC_S3_ERR_ARG);
    assert(rtc_s3_dry_run(121) == RTC_S3_ERR_ARG);
    assert(S.io == 0);
    puts("ok  dry-run findings: NO_PM1, NO_ALARM, STUCK, EN_MASKED, STS_STUCK, FIX_RTC, SCI, day/mon, args");
}

static void test_real_paths(void)
{
    /* happy path, with PM1b to exercise both blocks */
    setup(6, 86400 - 3);
    cfg.pm1b_evt = SIM_STS_B; cfg.pm1b_cnt = SIM_CNT_B; cfg.slp_typb = 6; apply();
    assert(configure_rtc_s3_sleep(5) == RTC_S3_RESUMED_RTC);       /* wraps past midnight */
    assert(S.slp_en_writes == 1 && S.n_cnt_writes == 2 && S.wbinvd_calls == 1);
    assert(!(S.cnt_writes[0] & BIT_SLPEN) && ((S.cnt_writes[0] >> 10) & 7) == 5);
    assert((S.cnt_writes[1] & BIT_SLPEN) && ((S.cnt_writes[1] >> 10) & 7) == 5);
    assert((S.cnt_writes[0] & 1) && (S.cnt_writes[1] & 1));         /* SCI_EN preserved */
    assert(S.n_cnt_b_writes == 2 && ((S.cnt_b_writes[0] >> 10) & 7) == 6);
    assert((S.cnt_b_writes[1] & BIT_SLPEN) && ((S.cnt_b_writes[1] >> 10) & 7) == 6);
    assert(S.en_b_saw_rtc);
    assert(g_rtc_s3_dbg.pm1_sts_post & BIT_RTC && g_rtc_s3_dbg.pm1_sts_post & BIT_WAK);
    assert(g_rtc_s3_dbg.pm1_cnt_pre == 1 && g_rtc_s3_dbg.result == 0 && !g_rtc_s3_dbg.dry_run);
    assert(!(S.regb & 0x20) && !(S.en & BIT_RTC) && !(S.sts & BIT_RTC));

    /* woke, but RTC_STS not set */
    setup(6, 1000); S.wired_pm1 = false; apply();
    assert(configure_rtc_s3_sleep(10) == RTC_S3_RESUMED_OTHER);

    /* SLP_EN written, platform never wakes */
    setup(6, 1000); S.alarm_works = false; apply();
    assert(configure_rtc_s3_sleep(10) == RTC_S3_ERR_SLEEP_FAILED);
    check_disarmed();

    /* RTC_EN masked: never reaches SLP_EN */
    setup(6, 1000); S.en_sticks = false; apply();
    assert(configure_rtc_s3_sleep(10) == RTC_S3_ERR_RTC_EN_MASKED);
    assert(S.slp_en_writes == 0 && S.n_cnt_writes == 0);

    /* stale WAK_STS that will not clear would fake an instant "resume" */
    setup(6, 1000); S.sts_stuck = true; S.sts = (uint16_t)BIT_WAK; apply();
    assert(configure_rtc_s3_sleep(10) == RTC_S3_ERR_STS_STUCK);
    assert(S.n_cnt_writes == 0);

    /* FIX_RTC platforms are rejected before any I/O */
    setup(6, 1000); cfg.fix_rtc = true; apply();
    assert(configure_rtc_s3_sleep(10) == RTC_S3_ERR_NO_FIXED_RTC);
    assert(S.io == 0);

    /* SCI_EN clear: no way to enable -> refuse; with SMI_CMD -> enable and go */
    setup(6, 1000); S.cnt = 0; apply();
    assert(configure_rtc_s3_sleep(10) == RTC_S3_ERR_NO_SCI);
    assert(g_rtc_s3_dbg.sci_en == 0 && S.n_cnt_writes == 0);
    setup(6, 1000); S.cnt = 0; cfg.smi_cmd = SIM_SMI; cfg.acpi_enable = SIM_ACPI_ENABLE; apply();
    assert(configure_rtc_s3_sleep(10) == RTC_S3_RESUMED_RTC);
    assert(g_rtc_s3_dbg.sci_en == 1);

    /* argument range */
    setup(6, 1000); apply();
    assert(configure_rtc_s3_sleep(2) == RTC_S3_ERR_ARG);
    assert(configure_rtc_s3_sleep(86400) == RTC_S3_ERR_ARG);
    assert(S.io == 0);
    puts("ok  real S3 paths: resume, RESUMED_OTHER, SLEEP_FAILED, EN_MASKED, STS_STUCK, FIX_RTC, SCI, PM1b, args");
}

static void test_selftest_logging(void)
{
    rtc_s3_set_logger(test_log);

    echo_log = true; nlog = 0;
    setup(6, 36000); apply();
    puts("  -- selftest(10, DRY_RUN), healthy platform");
    assert(rtc_s3_selftest(10, RTC_S3_DRY_RUN) == 0);
    assert(log_has("mode=dry") && log_has("rc=0") && log_has("pm1a_evt=0x0400"));
    assert(log_has("af=1") && log_has("rtc_sts=1"));

    nlog = 0;
    setup(6, 36000); S.wired_pm1 = false; apply();
    puts("  -- selftest(10, DRY_RUN), alarm fires but is not wired to PM1");
    assert(rtc_s3_selftest(10, RTC_S3_DRY_RUN) == RTC_S3_DRY_NO_PM1);
    assert(log_has("rc=-30") && log_has("not wired to PM1"));

    nlog = 0;
    setup(6, 36000); apply();
    puts("  -- selftest(10, REAL_S3), healthy platform");
    assert(rtc_s3_selftest(10, RTC_S3_REAL_S3) == 0);
    assert(log_has("writing SLP_TYP|SLP_EN now") && log_has("state=armed") && log_has("mode=s3"));

    echo_log = false; rtc_s3_set_logger(NULL);
    setup(6, 36000); apply();
    assert(rtc_s3_selftest(10, RTC_S3_DRY_RUN) == 0);        /* no logger: must not crash */
    puts("ok  selftest + UART logging (output above)");
}

static void test_acpi(void)
{
    rtc_s3_config_t c;
    acpi_p a;
    uint64_t rsdp;

    /* FADT flags bit 5 is SLP_BUTTON and must NOT be read as FIX_RTC; bit 6 is */
    a = base_params(); a.flags = 1u << 5; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == 0);
    assert(c.pm1a_evt == 0x400 && c.pm1a_cnt == 0x404 && c.pm1b_evt == 0 && c.pm1b_cnt == 0);
    assert(c.pm1_evt_len == 4 && c.slp_typa == 5 && c.slp_typb == 5);
    assert(!c.fix_rtc && c.smi_cmd == 0xB2 && c.acpi_enable == 0xA0);
    assert(c.rtc_day_alarm_idx == 0x7D && c.rtc_mon_alarm_idx == 0x7E && c.rtc_alarm_dontcare == 0xFF);
    assert(c.facs_phys == 0x7F003000);
    assert(rtc_s3_init(&c) == 0);

    a = base_params(); a.flags = 1u << 6; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == 0 && c.fix_rtc);

    /* X_ (GAS) addresses only, legacy 32-bit fields zero */
    a = base_params(); a.legacy_ports = false; a.x_ports = true; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == 0);
    assert(c.pm1a_evt == 0x400 && c.pm1a_cnt == 0x404);

    /* only a memory-mapped PM1 block */
    a = base_params(); a.legacy_ports = false; a.x_ports = true; a.x_space = 0; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_NO_PM1);

    /* no PM1 at all (hardware-reduced style) */
    a = base_params(); a.legacy_ports = false; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_NO_PM1);

    /* ACPI 1.0: RSDP rev 0 -> RSDT, 116-byte legacy FADT */
    a = base_params(); a.rev = 1; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == 0);
    assert(c.pm1a_evt == 0x400 && c.pm1a_cnt == 0x404 && c.slp_typa == 5);

    /* checksum / structure failures */
    a = base_params(); a.bad_fadt_sum = true; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_BAD_FADT);
    a = base_params(); a.bad_rsdp_sum = true; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_RSDP);
    a = base_params(); rsdp = build_acpi(&a); t_fadt[0] = 'X';
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_NO_FADT);
    assert(rtc_s3_config_from_acpi(&c, 0x1234, test_map) == RTC_S3_ERR_ACPI_RSDP);
    assert(rtc_s3_config_from_acpi(NULL, rsdp, test_map) == RTC_S3_ERR_CONFIG);

    /* _S3_ package forms */
    a = base_params(); a.dsdt_body = AML_S3_ROOT; a.dsdt_body_len = sizeof AML_S3_ROOT; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == 0 && c.slp_typa == 1 && c.slp_typb == 0);
    a = base_params(); a.dsdt_body = AML_S5_ONLY; a.dsdt_body_len = sizeof AML_S5_ONLY; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_NO_S3);
    a = base_params(); a.dsdt_body = AML_S3_TRUNC; a.dsdt_body_len = sizeof AML_S3_TRUNC; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_NO_S3);
    a = base_params(); a.dsdt_body = AML_S3_DECOY; a.dsdt_body_len = sizeof AML_S3_DECOY; rsdp = build_acpi(&a);
    assert(rtc_s3_config_from_acpi(&c, rsdp, test_map) == RTC_S3_ERR_ACPI_NO_S3);

    puts("ok  ACPI discovery: XSDT/RSDT, X_ GAS vs legacy, FIX_RTC bit 6, _S3_ forms, checksum/structure errors");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_not_configured();          /* must run first: nothing initialised yet */
    test_init_validation();
    test_dry_happy();
    test_dry_findings();
    test_real_paths();
    test_selftest_logging();
    test_acpi();
    puts("ALL SIM TESTS OK");
    return 0;
}