// Minimal AppleSMC access layer (read / write / enumerate keys).
// Works on Apple Silicon and Intel Macs via the IOKit AppleSMC service.
#ifndef SMC_H
#define SMC_H

#include <stdint.h>
#include <IOKit/IOKitLib.h>

typedef struct {
    char     key[5];      // 4 chars + NUL
    uint32_t dataSize;    // bytes of meaningful data
    char     dataType[5]; // e.g. "flt ", "ui8 ", "fpe2"
    uint8_t  bytes[32];   // raw payload
} SMCVal_t;

// Lifecycle. SMCOpen returns kIOReturnSuccess on success.
kern_return_t SMCOpen(void);
void          SMCClose(void);

// Read a key's type + current bytes.
kern_return_t SMCReadKey(const char *key, SMCVal_t *out);

// Write bytes to a key. Caller must set key, dataSize and bytes.
// dataType is not required for writing.
kern_return_t SMCWriteKey(const SMCVal_t *val);

// Enumerate the full SMC key table.
uint32_t      SMCKeyCount(void);                    // value of "#KEY"
kern_return_t SMCKeyAtIndex(uint32_t idx, char *keyOut /* char[5] */);

// Decode a value's bytes to a double based on its dataType.
// Returns 1 if the type is numeric and understood, 0 otherwise.
int           SMCDecode(const SMCVal_t *val, double *out);

#endif
