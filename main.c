// fancurve — minimal, safe Mac fan control.
//
//   fancurve list                 show every fan (rpm / min / max / target / mode)
//   fancurve dump [PREFIX]        raw SMC keys (optionally filtered, e.g. "F")
//   fancurve get  <key>           read one SMC key
//   fancurve set  <fan> <rpm>     force <fan> to <rpm> (clamped to hardware max)
//   fancurve set  <fan> max       force <fan> to its maximum
//   fancurve set  <fan> min       force <fan> to its minimum
//   fancurve auto [fan]           return fan(s) to firmware/auto control
//   fancurve curve [LOW HIGH [SEC]]  temp-driven curve (default 55 90 3)
//
// Safety: target rpm is always clamped to [F<n>Mn, F<n>Mx] as reported by the
// SMC itself, so it can never be driven past the hardware maximum (or below the
// firmware minimum). Writes require root.

#include "smc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>

static void fan_key(char *dst, int fan, const char *suffix) {
    // e.g. fan 0, "Tg" -> "F0Tg"
    snprintf(dst, 5, "F%d%s", fan, suffix);
}

// Encode a double into val->bytes according to val->dataType/dataSize,
// which the caller pre-populates from a read of the same key.
static int encode(SMCVal_t *val, double value) {
    const char *t = val->dataType;
    memset(val->bytes, 0, sizeof(val->bytes));

    if (strcmp(t, "flt ") == 0 && val->dataSize == 4) {
        float f = (float)value; memcpy(val->bytes, &f, 4); return 1;
    }
    if (strcmp(t, "fpe2") == 0 && val->dataSize == 2) {
        uint16_t u = (uint16_t)(value * 4.0);
        val->bytes[0] = u >> 8; val->bytes[1] = u & 0xff; return 1;
    }
    if ((strcmp(t, "ui8 ") == 0 || strcmp(t, "ui8") == 0) && val->dataSize == 1) {
        val->bytes[0] = (uint8_t)value; return 1;
    }
    if (strcmp(t, "ui16") == 0 && val->dataSize == 2) {
        uint16_t u = (uint16_t)value;
        val->bytes[0] = u >> 8; val->bytes[1] = u & 0xff; return 1;
    }
    return 0;
}

static int read_num(const char *key, double *out) {
    SMCVal_t v;
    if (SMCReadKey(key, &v) != kIOReturnSuccess) return 0;
    return SMCDecode(&v, out);
}

static int fan_count(void) {
    double n;
    if (read_num("FNum", &n)) return (int)n;
    return 0;
}

static void cmd_list(void) {
    int fans = fan_count();
    if (fans <= 0) { printf("No fans reported (FNum).\n"); return; }
    printf("%-4s %8s %8s %8s %8s %6s\n", "Fan", "RPM", "Min", "Max", "Target", "Mode");
    for (int i = 0; i < fans; i++) {
        char k[5]; double ac=0, mn=0, mx=0, tg=0, md=-1;
        fan_key(k, i, "Ac"); read_num(k, &ac);
        fan_key(k, i, "Mn"); read_num(k, &mn);
        fan_key(k, i, "Mx"); read_num(k, &mx);
        fan_key(k, i, "Tg"); read_num(k, &tg);
        fan_key(k, i, "Md"); int hasMode = read_num(k, &md);
        printf("%-4d %8.0f %8.0f %8.0f %8.0f %6s\n",
               i, ac, mn, mx, tg, hasMode ? (md >= 1 ? "manual" : "auto") : "?");
    }
}

static void cmd_dump(const char *prefix) {
    uint32_t n = SMCKeyCount();
    if (!n) { printf("Could not read key table (#KEY).\n"); return; }
    for (uint32_t i = 0; i < n; i++) {
        char key[5];
        if (SMCKeyAtIndex(i, key) != kIOReturnSuccess) continue;
        if (prefix && *prefix && strncmp(key, prefix, strlen(prefix)) != 0) continue;
        SMCVal_t v;
        if (SMCReadKey(key, &v) != kIOReturnSuccess) continue;
        double d; int ok = SMCDecode(&v, &d);
        printf("%-4s  %-4s  size=%u  ", v.key, v.dataType, v.dataSize);
        if (ok) printf("value=%.2f", d);
        else {
            printf("bytes=");
            for (uint32_t b = 0; b < v.dataSize && b < sizeof(v.bytes); b++)
                printf("%02x", v.bytes[b]);
        }
        printf("\n");
    }
}

static void cmd_get(const char *key) {
    SMCVal_t v;
    if (SMCReadKey(key, &v) != kIOReturnSuccess) {
        fprintf(stderr, "read %s failed\n", key); return;
    }
    double d; int ok = SMCDecode(&v, &d);
    printf("%s  type=%s size=%u  ", v.key, v.dataType, v.dataSize);
    if (ok) printf("value=%.2f\n", d);
    else {
        printf("bytes=");
        for (uint32_t b = 0; b < v.dataSize && b < sizeof(v.bytes); b++)
            printf("%02x", v.bytes[b]);
        printf("\n");
    }
}

// Set fan mode to manual(1)/auto(0), matching the key's native type.
static int set_mode(int fan, int manual) {
    char k[5]; fan_key(k, fan, "Md");
    SMCVal_t md;
    if (SMCReadKey(k, &md) != kIOReturnSuccess) return 0; // no per-fan mode key
    if (!encode(&md, manual ? 1 : 0)) return 0;
    strncpy(md.key, k, 4);
    return SMCWriteKey(&md) == kIOReturnSuccess;
}

// Write a fan's target rpm, clamped to its own [min,max]. No printing (used by
// the curve loop). Returns 1 on success.
static int apply_target(int fan, double rpm) {
    char kmn[5], kmx[5], ktg[5];
    fan_key(kmn, fan, "Mn"); fan_key(kmx, fan, "Mx"); fan_key(ktg, fan, "Tg");
    double mn, mx;
    if (!read_num(kmn, &mn) || !read_num(kmx, &mx)) return 0;
    if (rpm > mx) rpm = mx;          // never exceed the hardware maximum
    if (rpm < mn) rpm = mn;
    SMCVal_t tg;
    if (SMCReadKey(ktg, &tg) != kIOReturnSuccess) return 0;
    if (!encode(&tg, rpm)) return 0;
    strncpy(tg.key, ktg, 4);
    return SMCWriteKey(&tg) == kIOReturnSuccess;
}

// P-core, E-core and GPU sensors on Apple Silicon. Missing keys are skipped, so
// the same list is harmless across different chips.
static const char *kSensors[] = {
    "Tp01","Tp05","Tp09","Tp0D","Tp0H","Tp0L","Tp0P","Tp0T","Tp0X","Tp0b",
    "Tp02","Tp06","Tp0A","Tp0E","Tp0I","Tp0M","Tp0Q","Tp0U","Tp0Y","Tp0c",
    "Tg05","Tg0D",
    NULL
};

// Hottest plausible sensor reading, or -1 if none could be read.
static double read_max_temp(void) {
    double mx = 0; int any = 0;
    for (int i = 0; kSensors[i]; i++) {
        double v;
        if (read_num(kSensors[i], &v) && v > 0 && v < 130) {
            if (!any || v > mx) { mx = v; any = 1; }
        }
    }
    return any ? mx : -1;
}

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

// Temperature-driven fan curve. Below `low` fans idle at their minimum; at/above
// `high` they run at their maximum; in between they scale linearly. Each fan
// scales to its own min/max. Restores auto control on exit (Ctrl-C / SIGTERM).
static int cmd_curve(double low, double high, int interval) {
    int fans = fan_count();
    if (fans <= 0) { fprintf(stderr, "no fans\n"); return 1; }
    if (high <= low) { fprintf(stderr, "high must be greater than low\n"); return 1; }
    if (interval < 1) interval = 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    for (int i = 0; i < fans; i++) set_mode(i, 1); // manual

    printf("curve: %.0f-%.0fC -> min-max rpm, every %ds, %d fan(s). Ctrl-C to stop.\n",
           low, high, interval, fans);

    while (!g_stop) {
        double t = read_max_temp();
        if (t >= 0) {
            double frac = (t - low) / (high - low);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            for (int i = 0; i < fans; i++) {
                char kmn[5], kmx[5]; fan_key(kmn, i, "Mn"); fan_key(kmx, i, "Mx");
                double mn, mx;
                if (!read_num(kmn, &mn) || !read_num(kmx, &mx)) continue;
                apply_target(i, mn + frac * (mx - mn));
            }
        }
        for (int s = 0; s < interval && !g_stop; s++) sleep(1);
    }

    for (int i = 0; i < fans; i++) set_mode(i, 0); // back to auto
    printf("\ncurve stopped; fans returned to auto.\n");
    return 0;
}

static int cmd_set(int fan, const char *rpmArg) {
    int fans = fan_count();
    if (fan < 0 || fan >= fans) {
        fprintf(stderr, "fan %d out of range (0..%d)\n", fan, fans - 1);
        return 1;
    }

    char kmn[5], kmx[5], ktg[5];
    fan_key(kmn, fan, "Mn"); fan_key(kmx, fan, "Mx"); fan_key(ktg, fan, "Tg");

    double mn = 0, mx = 0;
    if (!read_num(kmn, &mn) || !read_num(kmx, &mx)) {
        fprintf(stderr, "could not read fan %d min/max\n", fan);
        return 1;
    }

    double rpm;
    if (strcmp(rpmArg, "max") == 0)      rpm = mx;
    else if (strcmp(rpmArg, "min") == 0) rpm = mn;
    else                                 rpm = atof(rpmArg);

    // Safety clamp to hardware-reported range. Never exceed the maximum.
    double clamped = rpm;
    if (clamped > mx) clamped = mx;
    if (clamped < mn) clamped = mn;
    if (clamped != rpm)
        fprintf(stderr, "note: %.0f clamped to %.0f (range %.0f..%.0f)\n",
                rpm, clamped, mn, mx);

    // Prepare the target write using the target key's native type.
    SMCVal_t tg;
    if (SMCReadKey(ktg, &tg) != kIOReturnSuccess) {
        fprintf(stderr, "could not read %s\n", ktg);
        return 1;
    }
    if (!encode(&tg, clamped)) {
        fprintf(stderr, "unsupported target type '%s' for %s\n", tg.dataType, ktg);
        return 1;
    }
    strncpy(tg.key, ktg, 4);

    set_mode(fan, 1); // best-effort switch to manual; some Macs force via target alone
    if (SMCWriteKey(&tg) != kIOReturnSuccess) {
        fprintf(stderr, "write %s failed (need sudo? is another controller running?)\n", ktg);
        return 1;
    }
    printf("fan %d target set to %.0f rpm\n", fan, clamped);
    return 0;
}

static int cmd_auto(int fan) {
    int fans = fan_count();
    if (fans <= 0) { fprintf(stderr, "no fans\n"); return 1; }
    int lo = fan >= 0 ? fan : 0;
    int hi = fan >= 0 ? fan + 1 : fans;
    for (int i = lo; i < hi && i < fans; i++) {
        if (set_mode(i, 0)) printf("fan %d returned to auto\n", i);
        else fprintf(stderr, "fan %d: could not set auto (need sudo?)\n", i);
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "fancurve — safe Mac fan control\n"
        "  fancurve list\n"
        "  fancurve dump [PREFIX]\n"
        "  fancurve get <KEY>\n"
        "  fancurve set <fan> <rpm|max|min>\n"
        "  fancurve auto [fan]\n"
        "  fancurve curve [LOW HIGH [SEC]]   (default 55 90 3)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }

    if (SMCOpen() != kIOReturnSuccess) {
        fprintf(stderr, "could not open AppleSMC\n");
        return 1;
    }

    int rc = 0;
    const char *cmd = argv[1];
    if (strcmp(cmd, "list") == 0) {
        cmd_list();
    } else if (strcmp(cmd, "dump") == 0) {
        cmd_dump(argc > 2 ? argv[2] : "");
    } else if (strcmp(cmd, "get") == 0 && argc > 2) {
        cmd_get(argv[2]);
    } else if (strcmp(cmd, "set") == 0 && argc > 3) {
        rc = cmd_set(atoi(argv[2]), argv[3]);
    } else if (strcmp(cmd, "auto") == 0) {
        rc = cmd_auto(argc > 2 ? atoi(argv[2]) : -1);
    } else if (strcmp(cmd, "curve") == 0) {
        double low  = argc > 2 ? atof(argv[2]) : 55.0;
        double high = argc > 3 ? atof(argv[3]) : 90.0;
        int    sec  = argc > 4 ? atoi(argv[4]) : 3;
        rc = cmd_curve(low, high, sec);
    } else {
        usage(); rc = 2;
    }

    SMCClose();
    return rc;
}
