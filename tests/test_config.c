// Tests ConfigRead hardening without touching the SMC: invalid numbers, low >= high, long mode,
// symlink to /dev/zero, FIFO, oversized file. Includes fanapi.c to point kConfigPath at a temp file.
#include "../fanapi.c"
#include <assert.h>
static char P[256];
static void put(const char *t) { unlink(P); FILE *f = fopen(P, "w"); fputs(t, f); fclose(f); }
int main(void) {
    snprintf(P, sizeof P, "%s/fancurve-test-config-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
    kConfigPath = P; FanConfig c; int fails = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s\n", #x); fails++; } else printf("ok   %s\n", #x); } while (0)
    put("mode manual\npercent 70\nlow 50\nhigh 85\n");
    CHECK(ConfigRead(&c) == 1 && !strcmp(c.mode, "manual") && c.percent == 70 && c.low == 50 && c.high == 85);
    put("mode manual\npercent nan\nlow -inf\nhigh inf\n");
    CHECK(ConfigRead(&c) == 1 && c.percent == 50 && c.low == 55 && c.high == 90);
    put("mode curve\npercent 500\nlow 80\nhigh 60\n");
    CHECK(ConfigRead(&c) == 1 && c.percent == 50 && c.low == 55 && c.high == 90);
    put("mode curvecurvecurvecurvecurve\n");
    CHECK(ConfigRead(&c) == 1 && strlen(c.mode) == 15);
    unlink(P); symlink("/dev/zero", P);
    CHECK(ConfigRead(&c) == 0 && !strcmp(c.mode, "auto"));
    unlink(P); mkfifo(P, 0600);
    CHECK(ConfigRead(&c) == 0);                         // returns at once instead of hanging
    unlink(P); FILE *f = fopen(P, "w"); for (int i = 0; i < 1000; i++) fputs("mode manual\n", f); fclose(f);
    CHECK(ConfigRead(&c) == 0);
    unlink(P);
    printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
