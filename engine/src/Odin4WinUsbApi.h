#ifndef EASYJTAG_ODIN4_WINUSB_API_H
#define EASYJTAG_ODIN4_WINUSB_API_H

#include <windows.h>
#include <cstddef>

#if defined(__MINGW32__) || defined(__BORLANDC__)
// TDM-GCC32's bundled Win32 headers omit winusb.h. These are only the Win32
// ABI declarations used by this adapter, checked against the SDK declarations
// already present in src/device_controller/WinUSB/Include/{usb100,winusb,winusbio}.h.
// No legacy wrapper, global handle, import library, policy, or driver is reused.
#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080
#endif
extern "C" {
// Present on the supported Windows 8+ target, absent from TDM's old headers.
WINBASEAPI ULONGLONG WINAPI GetTickCount64(void);
WINBASEAPI BOOL WINAPI CancelIoEx(HANDLE, LPOVERLAPPED);
}
typedef PVOID WINUSB_INTERFACE_HANDLE;
typedef WINUSB_INTERFACE_HANDLE* PWINUSB_INTERFACE_HANDLE;
#pragma pack(push, 1)
typedef struct {
    UCHAR bLength;
    UCHAR bDescriptorType;
    USHORT bcdUSB;
    UCHAR bDeviceClass;
    UCHAR bDeviceSubClass;
    UCHAR bDeviceProtocol;
    UCHAR bMaxPacketSize0;
    USHORT idVendor;
    USHORT idProduct;
    USHORT bcdDevice;
    UCHAR iManufacturer;
    UCHAR iProduct;
    UCHAR iSerialNumber;
    UCHAR bNumConfigurations;
} USB_DEVICE_DESCRIPTOR, *PUSB_DEVICE_DESCRIPTOR;
typedef struct {
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bInterfaceNumber;
    UCHAR bAlternateSetting;
    UCHAR bNumEndpoints;
    UCHAR bInterfaceClass;
    UCHAR bInterfaceSubClass;
    UCHAR bInterfaceProtocol;
    UCHAR iInterface;
} USB_INTERFACE_DESCRIPTOR, *PUSB_INTERFACE_DESCRIPTOR;
#pragma pack(pop)
enum USBD_PIPE_TYPE { UsbdPipeTypeControl, UsbdPipeTypeIsochronous, UsbdPipeTypeBulk, UsbdPipeTypeInterrupt };
typedef struct {
    USBD_PIPE_TYPE PipeType;
    UCHAR PipeId;
    USHORT MaximumPacketSize;
    UCHAR Interval;
} WINUSB_PIPE_INFORMATION, *PWINUSB_PIPE_INFORMATION;
#define USB_DEVICE_DESCRIPTOR_TYPE 1
extern "C" {
BOOL WINAPI WinUsb_Initialize(HANDLE, PWINUSB_INTERFACE_HANDLE);
BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE);
BOOL WINAPI WinUsb_GetAssociatedInterface(WINUSB_INTERFACE_HANDLE, UCHAR, PWINUSB_INTERFACE_HANDLE);
BOOL WINAPI WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR, USHORT, PUCHAR, ULONG, PULONG);
BOOL WINAPI WinUsb_GetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE, PUCHAR);
BOOL WINAPI WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE, UCHAR, PUSB_INTERFACE_DESCRIPTOR);
BOOL WINAPI WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR, PWINUSB_PIPE_INFORMATION);
BOOL WINAPI WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
BOOL WINAPI WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE, UCHAR);
}
#else
#include <winusb.h>
#endif

// Both toolchains assert the same SDK layout; public callers never see these.
static_assert(sizeof(USB_DEVICE_DESCRIPTOR) == 18, "Windows USB device descriptor ABI");
static_assert(offsetof(USB_DEVICE_DESCRIPTOR, idVendor) == 8, "Windows USB VID offset");
static_assert(sizeof(USB_INTERFACE_DESCRIPTOR) == 9, "Windows USB interface descriptor ABI");
static_assert(offsetof(USB_INTERFACE_DESCRIPTOR, bAlternateSetting) == 3, "Windows USB alternate offset");
static_assert(sizeof(WINUSB_PIPE_INFORMATION) == 12, "Windows WinUSB pipe information ABI");
static_assert(offsetof(WINUSB_PIPE_INFORMATION, PipeId) == 4, "Windows WinUSB pipe id offset");
static_assert(offsetof(WINUSB_PIPE_INFORMATION, MaximumPacketSize) == 6, "Windows WinUSB packet size offset");
static_assert(offsetof(WINUSB_PIPE_INFORMATION, Interval) == 8, "Windows WinUSB interval offset");
#endif
