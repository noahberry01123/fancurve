// Shared fan/temperature API used by the daemon and the GUI.
// Reads work unprivileged; the FanSet* writes require root.
#ifndef FANAPI_H
#define FANAPI_H

int    FanOpen(void);    // open AppleSMC; returns 1 on success
void   FanClose(void);
int    FanCount(void);   // number of fans (FNum)

// Fill any non-NULL out-params for fan `idx`. Returns 1 on success.
// `manual` is 1 when the fan is under forced (manual) control.
int    FanInfo(int idx, double *rpm, double *mn, double *mx,
               double *tg, int *manual);

double MaxTemp(void);    // hottest CPU/GPU sensor in C, or -1 if none

// Writes (root):
int    FanSetMode(int idx, int manual);    // 1 = manual, 0 = auto
int    FanSetPercent(int idx, double pct); // 0..100 across the fan's own range

// Config file shared between GUI (writer) and daemon (reader).
extern const char *kConfigPath;

typedef struct {
    char   mode[16];   // "auto" | "manual" | "curve"
    double percent;    // manual speed 0..100
    double low, high;  // curve bounds in C
} FanConfig;

void   ConfigDefaults(FanConfig *c);
int    ConfigRead(FanConfig *out);         // returns 1 if a file was read
int    ConfigWrite(const FanConfig *c);

// String-friendly wrappers so Swift never has to touch the C struct.
int    ConfigWriteValues(const char *mode, double percent,
                         double low, double high);
int    ConfigReadValues(char *modeBuf, int modeBufLen,
                        double *percent, double *low, double *high);

#endif
