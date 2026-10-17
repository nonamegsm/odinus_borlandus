#include "Odin4UsbTransport.h"
#include "Odin4SamsungSerial.h"
#include <windows.h>
#include <setupapi.h>
#include "Odin4WinUsbApi.h"
#include <objbase.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <stdexcept>

namespace ejodin4 {
namespace {
std::string win_error(const char* action, DWORD code = GetLastError()) {
    char number[32];std::sprintf(number,"%lu",(unsigned long)code);
    return std::string(action)+" (Windows error "+number+")";
}
struct DevInfo {
    HDEVINFO value;
    explicit DevInfo(HDEVINFO handle) : value(handle) {}
    ~DevInfo() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
    DevInfo(const DevInfo&);
    DevInfo& operator=(const DevInfo&);
};
struct Key {
    HKEY value;
    explicit Key(HKEY handle = 0) : value(handle) {}
    ~Key() { if (value && value != INVALID_HANDLE_VALUE) RegCloseKey(value); }
    Key(const Key&);
    Key& operator=(const Key&);
};

std::wstring property(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD property_id) {
    DWORD size = 0, type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, property_id, &type, 0, 0, &size)) {
        const DWORD code = GetLastError();
        if (code == ERROR_INVALID_DATA || code == ERROR_FILE_NOT_FOUND) return std::wstring();
        if (code != ERROR_INSUFFICIENT_BUFFER) throw std::runtime_error(win_error("Read USB property", code));
    }
    if (!size || size > 65536 || size % sizeof(wchar_t)) throw std::runtime_error("Invalid USB property length");
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, 0);
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, property_id, &type,
        reinterpret_cast<PBYTE>((&buffer[0])), size, 0)) throw std::runtime_error(win_error("Read USB property"));
    if (type != REG_SZ && type != REG_MULTI_SZ) throw std::runtime_error("Invalid USB property type");
    return std::wstring((&buffer[0]));
}
std::wstring instance_id(HDEVINFO set, SP_DEVINFO_DATA& device) {
    DWORD size = 0;
    SetupDiGetDeviceInstanceIdW(set, &device, 0, 0, &size);
    if (!size || size > 32768) throw std::runtime_error(win_error("Read USB instance length"));
    std::vector<wchar_t> buffer(size + 1, 0);
    if (!SetupDiGetDeviceInstanceIdW(set, &device, (&buffer[0]), size, 0))
        throw std::runtime_error(win_error("Read USB instance"));
    return std::wstring((&buffer[0]));
}
template<size_t N> void utf8_field(char (&output)[N], const std::wstring& value) {
    if (value.empty()) { output[0] = 0; return; }
    if (value.size() > static_cast<size_t>(INT_MAX)) throw std::runtime_error("USB identity is too long");
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, (&value[0]),
        static_cast<int>(value.size()), 0, 0, 0, 0);
    if (size <= 0 || static_cast<size_t>(size) >= N) throw std::runtime_error("USB identity exceeds UTF-8 ABI capacity");
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, (&value[0]),
        static_cast<int>(value.size()), output, size, 0, 0) != size)
        throw std::runtime_error(win_error("Convert USB identity"));
    output[size] = 0;
}
bool parse_hardware_word(const std::wstring& hardware,size_t offset,uint16_t& result) {
    unsigned value=0;for(unsigned i=0;i<4;++i){const wchar_t c=hardware[offset+i];unsigned digit;
    if(c>=L'0'&&c<=L'9')digit=unsigned(c-L'0');else if(c>=L'A'&&c<=L'F')digit=unsigned(c-L'A'+10);else return false;value=(value<<4)|digit;}result=uint16_t(value);return true;
}
bool hardware_identity(std::wstring hardware, uint16_t& vendor, uint16_t& product) {
    for(size_t i=0;i<hardware.size();++i)hardware[i]=static_cast<wchar_t>(towupper(hardware[i]));
    const size_t vid = hardware.find(L"VID_"), pid = hardware.find(L"PID_");
    if (vid == std::wstring::npos || pid == std::wstring::npos || hardware.size() < vid + 8 || hardware.size() < pid + 8) return false;
    return parse_hardware_word(hardware,vid+4,vendor) && parse_hardware_word(hardware,pid+4,product) && known_samsung(vendor,product);
}

void registry_guids(HKEY key, std::vector<GUID>& result) {
    DWORD size = 0, type = 0;
    LONG code = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", 0, &type, 0, &size);
    if (code == ERROR_FILE_NOT_FOUND) return;
    if (code != ERROR_SUCCESS) throw std::runtime_error(win_error("Read USB interface GUIDs", static_cast<DWORD>(code)));
    if ((type != REG_MULTI_SZ && type != REG_SZ) || size > 65536 || size % sizeof(wchar_t))
        throw std::runtime_error("Invalid DeviceInterfaceGUIDs registry value");
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, 0);
    code = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", 0, &type,
        reinterpret_cast<BYTE*>((&buffer[0])), &size);
    if (code != ERROR_SUCCESS) throw std::runtime_error(win_error("Read USB interface GUIDs", static_cast<DWORD>(code)));
    for (wchar_t* p = (&buffer[0]); *p; p += std::wcslen(p) + 1) {
        GUID guid;
        if (FAILED(CLSIDFromString(p, &guid))) throw std::runtime_error("Malformed USB interface GUID");
        bool already=false;for(size_t index=0;index<result.size();++index)if(IsEqualGUID(result[index],guid)){already=true;break;}
        if(!already) result.push_back(guid);
    }
}
std::vector<GUID> interface_guids(HDEVINFO set, SP_DEVINFO_DATA& device) {
    std::vector<GUID> result;
    Key hardware(SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ));
    if (hardware.value == INVALID_HANDLE_VALUE) return result;
    registry_guids(hardware.value, result);
    Key parameters;
    const LONG code = RegOpenKeyExW(hardware.value, L"Device Parameters", 0, KEY_READ, &parameters.value);
    if (code == ERROR_SUCCESS) registry_guids(parameters.value, result);
    else if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
        throw std::runtime_error(win_error("Open USB device parameters", static_cast<DWORD>(code)));
    return result;
}

std::wstring registry_port(HKEY key) {
    DWORD type = 0, size = 0;
    LONG code = RegQueryValueExW(key, L"PortName", 0, &type, 0, &size);
    if (code != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t) || size > 64 || size % sizeof(wchar_t)) return std::wstring();
    std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, 0);
    code = RegQueryValueExW(key, L"PortName", 0, &type, reinterpret_cast<BYTE*>((&value[0])), &size);
    if (code != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t) || size > 64 ||
        size % sizeof(wchar_t) || value[size / sizeof(wchar_t) - 1] != 0 ||
        std::wcslen((&value[0])) + 1 != size / sizeof(wchar_t)) return std::wstring();
    return (&value[0]);
}

std::vector<std::wstring> paths_for_instance(const GUID& guid, const std::wstring& identity,
    const Cancellation& cancel) {
    std::vector<std::wstring> paths;
    DevInfo set(SetupDiGetClassDevsW(&guid, 0, 0, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));
    if (set.value == INVALID_HANDLE_VALUE) throw std::runtime_error(win_error("Enumerate USB interface class"));
    for (DWORD index = 0;; ++index) {
        if (cancel.requested()) return paths;
        SP_DEVICE_INTERFACE_DATA item = {0};
        item.cbSize = sizeof(item);
        if (!SetupDiEnumDeviceInterfaces(set.value, 0, &guid, index, &item)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            throw std::runtime_error(win_error("Enumerate USB interface"));
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set.value, &item, 0, 0, &size, 0);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || size > 65536)
            throw std::runtime_error(win_error("Read USB interface detail length"));
        std::vector<uint8_t> storage(size + sizeof(wchar_t), 0);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>((&storage[0]));
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA device = {0};
        device.cbSize = sizeof(device);
        if (!SetupDiGetDeviceInterfaceDetailW(set.value, &item, detail, size, 0, &device))
            throw std::runtime_error(win_error("Read USB interface detail"));
        if (lstrcmpiW(instance_id(set.value, device).c_str(), identity.c_str()) == 0)
            paths.push_back(detail->DevicePath);
    }
    return paths;
}

class WinUsbLibrary {
    HMODULE module_;
    template<class T> void symbol(T& output, const char* name) {
        const FARPROC address = GetProcAddress(module_, name);
        if (!address) throw std::runtime_error(win_error("Resolve system WinUSB API"));
        static_assert(sizeof(output) == sizeof(address), "WinUSB function pointer");
        std::memcpy(&output, &address, sizeof(output));
    }
public:
    BOOL (WINAPI *initialize)(HANDLE,PWINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *free_interface)(WINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *associated)(WINUSB_INTERFACE_HANDLE,UCHAR,PWINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *query_interface)(WINUSB_INTERFACE_HANDLE,UCHAR,PUSB_INTERFACE_DESCRIPTOR);
    BOOL (WINAPI *query_pipe)(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR,PWINUSB_PIPE_INFORMATION);
    BOOL (WINAPI *current_alternate)(WINUSB_INTERFACE_HANDLE,PUCHAR);
    BOOL (WINAPI *descriptor)(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR,USHORT,PUCHAR,ULONG,PULONG);
    BOOL (WINAPI *read)(WINUSB_INTERFACE_HANDLE,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
    BOOL (WINAPI *write)(WINUSB_INTERFACE_HANDLE,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
    BOOL (WINAPI *abort_pipe)(WINUSB_INTERFACE_HANDLE,UCHAR);
    WinUsbLibrary() : module_(0) {
        wchar_t system[MAX_PATH + 1] = {0};
        const UINT size = GetSystemDirectoryW(system, MAX_PATH);
        if (!size || size >= MAX_PATH) throw std::runtime_error("Cannot resolve Windows system directory");
        const std::wstring path = std::wstring(system, size) + L"\\winusb.dll";
        module_ = LoadLibraryW(path.c_str());
        if (!module_) throw std::runtime_error(win_error("Load system WinUSB"));
        try {
            symbol(initialize, "WinUsb_Initialize"); symbol(free_interface, "WinUsb_Free");
            symbol(associated, "WinUsb_GetAssociatedInterface"); symbol(query_interface, "WinUsb_QueryInterfaceSettings");
            symbol(query_pipe, "WinUsb_QueryPipe"); symbol(read, "WinUsb_ReadPipe"); symbol(write, "WinUsb_WritePipe");
            symbol(current_alternate, "WinUsb_GetCurrentAlternateSetting"); symbol(descriptor, "WinUsb_GetDescriptor");
            symbol(abort_pipe, "WinUsb_AbortPipe");
        } catch (...) { FreeLibrary(module_); module_ = 0; throw; }
    }
    ~WinUsbLibrary() { if (module_) FreeLibrary(module_); }
    WinUsbLibrary(const WinUsbLibrary&);
    WinUsbLibrary& operator=(const WinUsbLibrary&);
};

class NativePending : public PendingIo {
    WinUsbLibrary& api_;
    WINUSB_INTERFACE_HANDLE interface_;
    HANDLE file_;
    uint8_t pipe_;
    bool active_;
    DWORD wait_error_;
public:
    OVERLAPPED overlapped;
    NativePending(WinUsbLibrary& api, WINUSB_INTERFACE_HANDLE interface_handle, HANDLE file, uint8_t pipe)
        : api_(api), interface_(interface_handle), file_(file), pipe_(pipe), active_(false), wait_error_(0) {
        ZeroMemory(&overlapped,sizeof(overlapped));
        overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
        if (!overlapped.hEvent) throw std::runtime_error(win_error("Create USB completion event"));
    }
    ~NativePending() {
        if (active_) { abort_and_cancel(); drain(); }
        CloseHandle(overlapped.hEvent);
    }
    void submitted() { active_ = true; }
    uint64_t now() const { return GetTickCount64(); }
    bool wait(uint32_t milliseconds) {
        const DWORD outcome = WaitForSingleObject(overlapped.hEvent, milliseconds);
        if (outcome == WAIT_FAILED) { wait_error_ = GetLastError(); return true; }
        return outcome == WAIT_OBJECT_0;
    }
    bool completion(uint32_t& actual, std::string& error) {
        if (wait_error_) {
            abort_and_cancel(); drain();
            error = win_error("Wait for USB transfer", wait_error_);
            return false;
        }
        DWORD transferred = 0;
        const BOOL ok = GetOverlappedResult(file_, &overlapped, &transferred, FALSE);
        const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
        if (!HasOverlappedIoCompleted(&overlapped)) { abort_and_cancel(); drain(); }
        else active_ = false;
        actual = static_cast<uint32_t>(transferred);
        if (!ok) error = win_error("Complete USB transfer", code);
        return ok != FALSE;
    }
    void abort_and_cancel() {
        api_.abort_pipe(interface_, pipe_);
        CancelIoEx(file_, &overlapped);
    }
    void drain() {
        if (!active_) return;
        DWORD transferred = 0;
        // May exceed the caller deadline if the installed driver is faulty.
        // Returning while the kernel still owns this buffer is not safe.
        do { GetOverlappedResult(file_, &overlapped, &transferred, TRUE); }
        while (!HasOverlappedIoCompleted(&overlapped));
        active_ = false;
    }
};

struct NativeInterruption {
    const Cancellation& cancel;uint64_t deadline;std::string& error;
    NativeInterruption(const Cancellation& c,uint64_t d,std::string& e):cancel(c),deadline(d),error(e){}
    EJODIN4_Status operator()()const {if(cancel.requested()){error="USB initialization cancelled";return EJODIN4_CANCELLED;}if(GetTickCount64()>=deadline){error="USB initialization timed out";return EJODIN4_TIMEOUT;}return EJODIN4_OK;}
};
struct AlternateQuery {
    WinUsbLibrary& api;WINUSB_INTERFACE_HANDLE handle;NativeInterruption& interrupted;EJODIN4_Status& status;std::string& error;
    AlternateQuery(WinUsbLibrary& a,WINUSB_INTERFACE_HANDLE h,NativeInterruption& i,EJODIN4_Status& s,std::string& e):api(a),handle(h),interrupted(i),status(s),error(e){}
    bool operator()(uint8_t ordinal,USB_INTERFACE_DESCRIPTOR& found)const {EJODIN4_Status stop=interrupted();if(stop!=EJODIN4_OK){status=stop;return false;}if(api.query_interface(handle,ordinal,&found))return true;DWORD code=GetLastError();if(code!=ERROR_NO_MORE_ITEMS){status=EJODIN4_USB_IO_ERROR;error=win_error("Query WinUSB alternate interface descriptor",code);}return false;}
};
class NativeConnection : public Connection {
    WinUsbLibrary api_;
    HANDLE file_;
    std::vector<WINUSB_INTERFACE_HANDLE> handles_;
    std::vector<Interface> interfaces_;
public:
    NativeConnection() : file_(INVALID_HANDLE_VALUE) {}
    ~NativeConnection() {
        for (size_t i = handles_.size(); i > 0; --i) api_.free_interface(handles_[i - 1]);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    EJODIN4_Status open(const EJODIN4_UsbDevice& device, uint64_t deadline,
        const Cancellation& cancel, std::string& error) {
        NativeInterruption interrupted(cancel,deadline,error);
        EJODIN4_Status stop = interrupted();
        if (stop != EJODIN4_OK) return stop;
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.device_path, -1, 0, 0);
        if (!size) { error = "Invalid enumerated USB path encoding"; return EJODIN4_INTERNAL_ERROR; }
        std::vector<wchar_t> path(static_cast<size_t>(size));
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.device_path, -1, (&path[0]), size)) {
            error = win_error("Convert USB interface path"); return EJODIN4_INTERNAL_ERROR;
        }
        file_ = CreateFileW((&path[0]), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
        if (file_ == INVALID_HANDLE_VALUE) { error = win_error("Open selected WinUSB interface"); return EJODIN4_USB_IO_ERROR; }
        stop = interrupted();
        if (stop != EJODIN4_OK) return stop;
        WINUSB_INTERFACE_HANDLE base = 0;
        if (!api_.initialize(file_, &base)) { error = win_error("Initialize selected WinUSB interface"); return EJODIN4_DRIVER_UNSUPPORTED; }
        try { handles_.push_back(base); }
        catch (...) { api_.free_interface(base); throw; }
        stop = interrupted();
        if (stop != EJODIN4_OK) return stop;
        USB_DEVICE_DESCRIPTOR actual = {0};
        ULONG actual_size = 0;
        if (!api_.descriptor(base, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
            reinterpret_cast<PUCHAR>(&actual), sizeof(actual), &actual_size)) {
            error = win_error("Verify selected USB device descriptor"); return EJODIN4_USB_IO_ERROR;
        }
        if (actual_size != sizeof(actual) || actual.bLength != sizeof(actual) ||
            actual.bDescriptorType != USB_DEVICE_DESCRIPTOR_TYPE ||
            actual.idVendor != device.vendor_id || actual.idProduct != device.product_id) {
            error = "Opened USB descriptor differs from selected Samsung identity"; return EJODIN4_NO_DEVICE;
        }
        for (unsigned index = 0; index < 256; ++index) {
            stop = interrupted();
            if (stop != EJODIN4_OK) return stop;
            WINUSB_INTERFACE_HANDLE next = 0;
            if (!api_.associated(base, static_cast<UCHAR>(index), &next)) {
                const DWORD code = GetLastError();
                if (code == ERROR_NO_MORE_ITEMS) break;
                error = win_error("Inspect associated WinUSB interface", code); return EJODIN4_USB_IO_ERROR;
            }
            try { handles_.push_back(next); }
            catch (...) { api_.free_interface(next); throw; }
            if (index == 255) { error = "Too many associated WinUSB interfaces"; return EJODIN4_DRIVER_UNSUPPORTED; }
        }
        for (size_t handle_index=0;handle_index<handles_.size();++handle_index) {
            WINUSB_INTERFACE_HANDLE handle=handles_[handle_index];
            stop = interrupted();
            if (stop != EJODIN4_OK) return stop;
            USB_INTERFACE_DESCRIPTOR descriptor = {0};
            UCHAR alternate = 0;
            if (!api_.current_alternate(handle, &alternate)) {
                error = win_error("Query WinUSB interface settings"); return EJODIN4_USB_IO_ERROR;
            }
            // QueryInterfaceSettings takes an ordinal, not bAlternateSetting.
            // Keep the active setting; never switch the device implicitly.
            UCHAR alternate_index = 0;
            EJODIN4_Status query_status = EJODIN4_DRIVER_UNSUPPORTED;
            const bool matched = find_current_alternate(alternate, descriptor, alternate_index,
                AlternateQuery(api_,handle,interrupted,query_status,error));
            if (!matched) {
                if (query_status == EJODIN4_DRIVER_UNSUPPORTED) error = "Active WinUSB alternate setting has no matching descriptor";
                return query_status;
            }
            Interface item; item.number=descriptor.bInterfaceNumber;item.device_class=descriptor.bInterfaceClass;
            for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
                stop = interrupted();
                if (stop != EJODIN4_OK) return stop;
                WINUSB_PIPE_INFORMATION pipe;std::memset(&pipe,0,sizeof(pipe));
                if (!api_.query_pipe(handle, alternate_index, index, &pipe)) {
                    error = win_error("Query WinUSB interface endpoint"); return EJODIN4_USB_IO_ERROR;
                }
                Pipe endpoint={pipe.PipeId,pipe.MaximumPacketSize,pipe.PipeType==UsbdPipeTypeBulk};item.pipes.push_back(endpoint);
            }
            interfaces_.push_back(item);
        }
        return EJODIN4_OK;
    }
    EJODIN4_Status interfaces(std::vector<Interface>& result, std::string&) {
        result = interfaces_; return EJODIN4_OK;
    }
    EJODIN4_Status transfer(size_t index, uint8_t pipe, uint8_t* data, uint32_t capacity,
        uint32_t& actual, uint64_t deadline, const Cancellation& cancel, std::string& error) {
        actual = 0;
        if (index >= handles_.size()) { error = "Invalid selected USB interface"; return EJODIN4_INTERNAL_ERROR; }
        if (cancel.requested()) { error = "USB transfer cancelled"; return EJODIN4_CANCELLED; }
        if (GetTickCount64() >= deadline) { error = "USB transfer timed out"; return EJODIN4_TIMEOUT; }
        NativePending pending(api_, handles_[index], file_, pipe);
        ULONG transferred = 0;
        const BOOL ok = (pipe & 0x80) ? api_.read(handles_[index], pipe, data, capacity, &transferred, &pending.overlapped)
            : api_.write(handles_[index], pipe, data, capacity, &transferred, &pending.overlapped);
        if (ok) { actual = static_cast<uint32_t>(transferred); return EJODIN4_OK; }
        const DWORD code = GetLastError();
        if (code != ERROR_IO_PENDING) { error = win_error("Submit USB transfer", code); return EJODIN4_USB_IO_ERROR; }
        pending.submitted();
        return finish_pending(pending, deadline, cancel, actual, error);
    }
};

class NativePlatform : public Platform {
    bool include_serial_;
public:
    explicit NativePlatform(bool include_serial) : include_serial_(include_serial) {}
    uint64_t now() const { return GetTickCount64(); }
    EJODIN4_Status enumerate(std::vector<EJODIN4_UsbDevice>& devices,
        const Cancellation& cancel, std::string& error) {
        DevInfo set(SetupDiGetClassDevsW(0, L"USB", 0, DIGCF_ALLCLASSES | DIGCF_PRESENT));
        if (set.value == INVALID_HANDLE_VALUE) { error = win_error("Enumerate present USB devices"); return EJODIN4_USB_IO_ERROR; }
        for (DWORD index = 0;; ++index) {
            if (cancel.requested()) { error = "USB enumeration cancelled"; return EJODIN4_CANCELLED; }
            SP_DEVINFO_DATA device = {0};
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(set.value, index, &device)) {
                if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
                error = win_error("Enumerate present USB device"); return EJODIN4_USB_IO_ERROR;
            }
            uint16_t vendor = 0, product = 0;
            if (!hardware_identity(property(set.value, device, SPDRP_HARDWAREID), vendor, product)) continue;
            EJODIN4_UsbDevice record = {0};
            record.vendor_id = vendor; record.product_id = product;
            const std::wstring identity = instance_id(set.value, device);
            const std::wstring driver = property(set.value, device, SPDRP_SERVICE);
            std::wstring description = property(set.value, device, SPDRP_FRIENDLYNAME);
            if (description.empty()) description = property(set.value, device, SPDRP_DEVICEDESC);
            utf8_field(record.instance_id, identity); utf8_field(record.driver, driver); utf8_field(record.description, description);
            std::vector<std::wstring> paths;
            uint32_t transport = EJODIN4_DOWNLOAD_WINUSB;
            if (lstrcmpiW(driver.c_str(), L"WinUSB") == 0) {
                const std::vector<GUID> guids=interface_guids(set.value,device);
                for(size_t guid_index=0;guid_index<guids.size();++guid_index) {
                    const GUID& guid=guids[guid_index];
                    const std::vector<std::wstring> found = paths_for_instance(guid, identity, cancel);
                    for(size_t path_index=0;path_index<found.size();++path_index) {const std::wstring& path=found[path_index];
                        if(std::find(paths.begin(),paths.end(),path)==paths.end())paths.push_back(path); }
                }
            }
            // Modem setup class is not a device-interface GUID. Enumerate the
            // modem interface class and retain only paths bound to this exact
            // present Samsung child instance; COM aliases are never opened.
            const GUID modem_class = {0x4d36e96d,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
            const GUID modem_interface = {0x2c7089aa,0x2e0e,0x11d1,{0xb1,0x14,0x00,0xc0,0x4f,0xc2,0xaa,0xe4}};
            if (include_serial_ && lstrcmpiW(driver.c_str(), L"WinUSB") != 0 && IsEqualGUID(device.ClassGuid, modem_class)) {
                Key hardware(SetupDiOpenDevRegKey(set.value, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ));
                std::wstring port;
                if (hardware.value != INVALID_HANDLE_VALUE) {
                    port = registry_port(hardware.value);
                    if (port.empty()) {
                        Key parameters;
                        if (RegOpenKeyExW(hardware.value, L"Device Parameters", 0, KEY_READ, &parameters.value) == ERROR_SUCCESS)
                            port = registry_port(parameters.value);
                    }
                }
                if (ejodin4::valid_com_port(port)) {
                    paths = paths_for_instance(modem_interface, identity, cancel);
                    if (!paths.empty()) {
                        transport = EJODIN4_DOWNLOAD_SAMSUNG_SERIAL;
                        if (description.find(port) == std::wstring::npos) description += L" (" + port + L")";
                        utf8_field(record.description, description);
                    }
                }
            }
            if (paths.empty()) devices.push_back(record);
            else for(size_t path_index=0;path_index<paths.size();++path_index) {
                const std::wstring& path=paths[path_index];
                utf8_field(record.device_path, path);
                record.flags = transport;
                devices.push_back(record);
            }
        }
        return EJODIN4_OK;
    }
    EJODIN4_Status open(const EJODIN4_UsbDevice& device,
        std::unique_ptr<Connection>& connection, uint64_t deadline,
        const Cancellation& cancel, std::string& error) {
        std::unique_ptr<NativeConnection> native(new NativeConnection);
        const EJODIN4_Status status = native->open(device, deadline, cancel, error);
        if (status == EJODIN4_OK) connection.reset(native.release());
        return status;
    }
    EJODIN4_Status open_serial(const EJODIN4_UsbDevice& device,
        std::unique_ptr<SerialConnection>& connection, uint64_t deadline,
        const Cancellation& cancel, std::string& error) {
        if (!include_serial_ || device.flags != EJODIN4_DOWNLOAD_SAMSUNG_SERIAL) {
            error = "Samsung serial transport was not selected"; return EJODIN4_DRIVER_UNSUPPORTED;
        }
        return open_native_serial(device, connection, deadline, cancel, error);
    }
};
}
std::unique_ptr<Platform> make_native_platform(bool include_serial) {
    return std::unique_ptr<Platform>(new NativePlatform(include_serial));
}
}
