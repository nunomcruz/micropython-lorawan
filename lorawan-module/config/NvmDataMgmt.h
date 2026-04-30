// NvmDataMgmt.h — stub for LmhpCompliance.c which includes this header but
// does not call any of these functions directly. The MAC's NVM persistence
// is handled by modlorawan.c via MIB_NVM_CTXS (nvram_save / nvram_restore).
#ifndef __NVMDATAMGMT_H__
#define __NVMDATAMGMT_H__

#include <stdint.h>
#include <stdbool.h>

void     NvmDataMgmtEvent(uint16_t notifyFlags);
uint16_t NvmDataMgmtStore(void);
uint16_t NvmDataMgmtRestore(void);
bool     NvmDataMgmtFactoryReset(void);

#endif // __NVMDATAMGMT_H__
