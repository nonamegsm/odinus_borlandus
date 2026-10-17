#ifndef EASYJTAG_ODIN4_FLASH_OPERATION_H
#define EASYJTAG_ODIN4_FLASH_OPERATION_H
#include "Odin4UsbTransport.h"
namespace ejodin4 {
// Private composition seam. Only statically linked tests supply a fake platform;
// the exported operation always constructs the native platform internally.
EJODIN4_Status flash_with_platform(Platform& platform,
    const EJODIN4_FlashRequest* request, EJODIN4_FlashResult* result,
    EJODIN4_FlashProgressCallback progress, EJODIN4_CancelCallback cancel,
    void* context, char* diagnostic, uint32_t capacity);
}
#endif
