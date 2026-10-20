#include "smc.h"
#include <string.h>

#define KERNEL_INDEX_SMC     2
#define SMC_CMD_READ_BYTES   5
#define SMC_CMD_WRITE_BYTES  6
#define SMC_CMD_READ_INDEX   8
#define SMC_CMD_READ_KEYINFO 9

typedef struct {
    uint8_t  major, minor, build, reserved[1];
    uint16_t release;
} SMCVers_t;

typedef struct {
    uint16_t version, length;
    uint32_t cpuPLimit, gpuPLimit, memPLimit;
} SMCPLimit_t;

typedef struct {
    uint32_t dataSize;
    uint32_t dataType;
    uint8_t  dataAttributes;
} SMCKeyInfo_t;

typedef struct {
    uint32_t     key;
    SMCVers_t    vers;
    SMCPLimit_t  pLimit;
    SMCKeyInfo_t keyInfo;
    uint8_t      result;
    uint8_t      status;
    uint8_t      data8;
    uint32_t     data32;
    uint8_t      bytes[32];
} SMCParamStruct_t;

static io_connect_t g_conn = 0;

static uint32_t key_to_u32(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8)  |  (uint32_t)(uint8_t)s[3];
}

static void u32_to_str(char *dst /* char[5] */, uint32_t v) {
    dst[0] = (v >> 24) & 0xff;
    dst[1] = (v >> 16) & 0xff;
    dst[2] = (v >> 8)  & 0xff;
    dst[3] =  v        & 0xff;
    dst[4] = 0;
}

kern_return_t SMCOpen(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("AppleSMC"));
    if (!svc) return kIOReturnNotFound;
    kern_return_t r = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    return r;
}

void SMCClose(void) {
    if (g_conn) { IOServiceClose(g_conn); g_conn = 0; }
}

static kern_return_t call(SMCParamStruct_t *in, SMCParamStruct_t *out) {
    size_t outSize = sizeof(SMCParamStruct_t);
    return IOConnectCallStructMethod(g_conn, KERNEL_INDEX_SMC,
                                     in,  sizeof(SMCParamStruct_t),
                                     out, &outSize);
}

kern_return_t SMCReadKey(const char *key, SMCVal_t *out) {
    SMCParamStruct_t in = {0}, res = {0};
    memset(out, 0, sizeof(*out));
    strncpy(out->key, key, 4);

    // 1) key info (size + type)
    in.key   = key_to_u32(key);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = call(&in, &res);
    if (r != kIOReturnSuccess) return r;
    if (res.result != 0) return kIOReturnError;

    out->dataSize = res.keyInfo.dataSize;
    u32_to_str(out->dataType, res.keyInfo.dataType);

    // 2) read the bytes
    in.keyInfo.dataSize = res.keyInfo.dataSize;
    in.data8            = SMC_CMD_READ_BYTES;
    r = call(&in, &res);
    if (r != kIOReturnSuccess) return r;
    if (res.result != 0) return kIOReturnError;

    memcpy(out->bytes, res.bytes, sizeof(res.bytes));
    return kIOReturnSuccess;
}

kern_return_t SMCWriteKey(const SMCVal_t *val) {
    SMCParamStruct_t in = {0}, res = {0};
    in.key              = key_to_u32(val->key);
    in.data8            = SMC_CMD_WRITE_BYTES;
    in.keyInfo.dataSize = val->dataSize;
    memcpy(in.bytes, val->bytes, sizeof(in.bytes));

    kern_return_t r = call(&in, &res);
    if (r != kIOReturnSuccess) return r;
    if (res.result != 0) return kIOReturnError;
    return kIOReturnSuccess;
}

uint32_t SMCKeyCount(void) {
    SMCVal_t v;
    if (SMCReadKey("#KEY", &v) != kIOReturnSuccess) return 0;
    // #KEY is a big-endian ui32
    return ((uint32_t)v.bytes[0] << 24) | ((uint32_t)v.bytes[1] << 16) |
           ((uint32_t)v.bytes[2] << 8)  |  (uint32_t)v.bytes[3];
}

kern_return_t SMCKeyAtIndex(uint32_t idx, char *keyOut) {
    SMCParamStruct_t in = {0}, res = {0};
    in.data8  = SMC_CMD_READ_INDEX;
    in.data32 = idx;
    kern_return_t r = call(&in, &res);
    if (r != kIOReturnSuccess) return r;
    u32_to_str(keyOut, res.key);
    return kIOReturnSuccess;
}

int SMCDecode(const SMCVal_t *val, double *out) {
    const char *t = val->dataType;
    uint32_t n = val->dataSize;

    if (strcmp(t, "flt ") == 0 && n == 4) {           // IEEE-754 little-endian
        float f; memcpy(&f, val->bytes, 4); *out = f; return 1;
    }
    if (strcmp(t, "fpe2") == 0 && n == 2) {           // big-endian fixed point /4
        uint16_t u = ((uint16_t)val->bytes[0] << 8) | val->bytes[1];
        *out = u / 4.0; return 1;
    }
    if (strcmp(t, "fp2e") == 0 && n == 2) {
        uint16_t u = ((uint16_t)val->bytes[0] << 8) | val->bytes[1];
        *out = u / 16384.0; return 1;
    }
    if ((strcmp(t, "ui8 ") == 0 || strcmp(t, "ui8") == 0) && n == 1) {
        *out = val->bytes[0]; return 1;
    }
    if (strcmp(t, "ui16") == 0 && n == 2) {
        *out = ((uint16_t)val->bytes[0] << 8) | val->bytes[1]; return 1;
    }
    if (strcmp(t, "ui32") == 0 && n == 4) {
        *out = ((uint32_t)val->bytes[0] << 24) | ((uint32_t)val->bytes[1] << 16) |
               ((uint32_t)val->bytes[2] << 8)  |  (uint32_t)val->bytes[3];
        return 1;
    }
    return 0;
}
