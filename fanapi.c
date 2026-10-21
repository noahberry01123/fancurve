#include "fanapi.h"
#include "smc.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

const char *kConfigPath = "/Library/Application Support/FanCurve/config";

// P-core, E-core and GPU sensors on Apple Silicon. Missing keys are skipped.
static const char *kSensors[] = {
    "Tp01","Tp05","Tp09","Tp0D","Tp0H","Tp0L","Tp0P","Tp0T","Tp0X","Tp0b",
    "Tp02","Tp06","Tp0A","Tp0E","Tp0I","Tp0M","Tp0Q","Tp0U","Tp0Y","Tp0c",
    "Tg05","Tg0D",
    NULL
};

static void fkey(char *dst, int fan, const char *sfx) {
    snprintf(dst, 5, "F%d%s", fan, sfx);
}

static int rd(const char *key, double *out) {
    SMCVal_t v;
    if (SMCReadKey(key, &v) != kIOReturnSuccess) return 0;
    return SMCDecode(&v, out);
}

// Encode a double into a value using its (pre-read) native type.
static int encode(SMCVal_t *v, double value) {
    const char *t = v->dataType;
    memset(v->bytes, 0, sizeof(v->bytes));
    if (strcmp(t, "flt ") == 0 && v->dataSize == 4) {
        float f = (float)value; memcpy(v->bytes, &f, 4); return 1;
    }
    if (strcmp(t, "fpe2") == 0 && v->dataSize == 2) {
        uint16_t u = (uint16_t)(value * 4.0);
        v->bytes[0] = u >> 8; v->bytes[1] = u & 0xff; return 1;
    }
    if ((strcmp(t, "ui8 ") == 0 || strcmp(t, "ui8") == 0) && v->dataSize == 1) {
        v->bytes[0] = (uint8_t)value; return 1;
    }
    if (strcmp(t, "ui16") == 0 && v->dataSize == 2) {
        uint16_t u = (uint16_t)value;
        v->bytes[0] = u >> 8; v->bytes[1] = u & 0xff; return 1;
    }
    return 0;
}

int FanOpen(void)  { return SMCOpen() == kIOReturnSuccess; }
void FanClose(void){ SMCClose(); }

int FanCount(void) { double n; return rd("FNum", &n) ? (int)n : 0; }

int FanInfo(int idx, double *rpm, double *mn, double *mx,
            double *tg, int *manual) {
    char k[5]; double v;
    if (rpm)    { fkey(k, idx, "Ac"); *rpm = rd(k, &v) ? v : 0; }
    if (mn)     { fkey(k, idx, "Mn"); *mn  = rd(k, &v) ? v : 0; }
    if (mx)     { fkey(k, idx, "Mx"); *mx  = rd(k, &v) ? v : 0; }
    if (tg)     { fkey(k, idx, "Tg"); *tg  = rd(k, &v) ? v : 0; }
    if (manual) { fkey(k, idx, "Md"); *manual = (rd(k, &v) && v >= 1) ? 1 : 0; }
    return 1;
}

double MaxTemp(void) {
    double mx = 0; int any = 0;
    for (int i = 0; kSensors[i]; i++) {
        double v;
        if (rd(kSensors[i], &v) && v > 0 && v < 130) {
            if (!any || v > mx) { mx = v; any = 1; }
        }
    }
    return any ? mx : -1;
}

int FanSetMode(int idx, int manual) {
    char k[5]; fkey(k, idx, "Md");
    SMCVal_t md;
    if (SMCReadKey(k, &md) != kIOReturnSuccess) return 0;
    if (!encode(&md, manual ? 1 : 0)) return 0;
    strncpy(md.key, k, 4);
    return SMCWriteKey(&md) == kIOReturnSuccess;
}

int FanSetPercent(int idx, double pct) {
    // The fan's own limits must be read successfully and make sense; otherwise write nothing
    // (a failed read used to give mn = mx = 0 and so a 0 rpm target, below the firmware minimum).
    char k[5]; double mn, mx;
    fkey(k, idx, "Mn"); if (!rd(k, &mn)) return 0;
    fkey(k, idx, "Mx"); if (!rd(k, &mx)) return 0;
    if (!isfinite(mn) || !isfinite(mx) || mn <= 0 || mx <= mn) return 0;
    if (!isfinite(pct)) return 0;                // NaN/inf would pass the range checks below
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    double rpm = mn + pct / 100.0 * (mx - mn);
    if (rpm < mn) rpm = mn;                      // belt and braces: never outside [mn, mx]
    if (rpm > mx) rpm = mx;

    char ktg[5]; fkey(ktg, idx, "Tg");
    SMCVal_t tg;
    if (SMCReadKey(ktg, &tg) != kIOReturnSuccess) return 0;
    if (!encode(&tg, rpm)) return 0;
    strncpy(tg.key, ktg, 4);
    return SMCWriteKey(&tg) == kIOReturnSuccess;
}

void ConfigDefaults(FanConfig *c) {
    memset(c->mode, 0, sizeof c->mode);
    strcpy(c->mode, "auto");
    c->percent = 50; c->low = 55; c->high = 90;
}

// Numbers from the config file: finite and within [lo, hi], else the default.
static double sane(const char *s, double lo, double hi, double def) {
    char *end; double v = strtod(s, &end);
    return (end != s && isfinite(v) && v >= lo && v <= hi) ? v : def;
}

int ConfigRead(FanConfig *out) {
    ConfigDefaults(out);
    // The file is user-writable and read by a root daemon: refuse symlinks, FIFOs, devices and
    // anything large, so it can never block or flood the daemon (O_NONBLOCK: a FIFO cannot hang open).
    int fd = open(kConfigPath, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 4096) { close(fd); return 0; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }
    char line[128], key[32], sval[32];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %31s", key, sval) != 2) continue;
        if      (!strcmp(key, "mode"))    { strncpy(out->mode, sval, sizeof out->mode - 1); out->mode[sizeof out->mode - 1] = 0; }
        else if (!strcmp(key, "percent")) out->percent = sane(sval, 0, 100, 50);
        else if (!strcmp(key, "low"))     out->low  = sane(sval, 20, 110, 55);
        else if (!strcmp(key, "high"))    out->high = sane(sval, 20, 110, 90);
    }
    fclose(f);
    if (out->high <= out->low) { out->low = 55; out->high = 90; }   // a curve needs low < high
    return 1;
}

int ConfigWrite(const FanConfig *c) {
    FILE *f = fopen(kConfigPath, "w");
    if (!f) return 0;
    fprintf(f, "mode %s\npercent %.0f\nlow %.0f\nhigh %.0f\n",
            c->mode, c->percent, c->low, c->high);
    fclose(f);
    return 1;
}

int ConfigWriteValues(const char *mode, double percent, double low, double high) {
    FanConfig c;
    strncpy(c.mode, mode, sizeof c.mode - 1);
    c.mode[sizeof c.mode - 1] = 0;
    c.percent = percent; c.low = low; c.high = high;
    return ConfigWrite(&c);
}

int ConfigReadValues(char *modeBuf, int modeBufLen,
                     double *percent, double *low, double *high) {
    FanConfig c;
    int ok = ConfigRead(&c);
    if (modeBuf && modeBufLen > 0) {
        strncpy(modeBuf, c.mode, modeBufLen - 1);
        modeBuf[modeBufLen - 1] = 0;
    }
    if (percent) *percent = c.percent;
    if (low)     *low  = c.low;
    if (high)    *high = c.high;
    return ok;
}
