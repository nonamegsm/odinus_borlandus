#include "Odin4FlashOperation.h"
#include "Odin4Session.h"
#include "Odin4LayoutProfile.h"
#include <algorithm>
#include "BccCompat.h"
#include <windows.h>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

BCC_STATIC_ASSERT(sizeof(EJODIN4_FirmwareFile) == 12);
BCC_STATIC_ASSERT(sizeof(EJODIN4_FlashRequest) == 64);
BCC_STATIC_ASSERT(sizeof(EJODIN4_FlashProgress) == 537);
BCC_STATIC_ASSERT(sizeof(EJODIN4_FlashResult) == 68);
namespace {
using namespace ejodin4;
void diagnostic(char* output, uint32_t capacity, const std::string& error)  {
    if (!output || !capacity) return;
    const size_t count = std::min(error.size(), static_cast<size_t>(capacity - 1));
    std::memcpy(output, error.data(), count); output[count] = 0;
}
bool valid_request(const EJODIN4_FlashRequest* request, const EJODIN4_FlashResult* output,
                   char* text, uint32_t capacity, std::string& error) {
    if (!request || !output || (!text && capacity) || request->struct_size != sizeof(*request)) {
        error = "Invalid flash request/result/diagnostic layout"; return false;
    }
    if (request->operation > EJODIN4_FLASH_WRITE ||
        (request->flags & ~static_cast<uint32_t>(EJODIN4_FLASH_REBOOT)) ||
        request->reserved[0] || request->reserved[1] ||
        (request->io_timeout_ms && (request->io_timeout_ms < 1000 || request->io_timeout_ms > 120000)) ||
        (request->logical_sector_bytes && request->logical_sector_bytes != 512 &&
         request->logical_sector_bytes != 1024 && request->logical_sector_bytes != 2048 && request->logical_sector_bytes != 4096) ||
        request->userdata_capacity_bytes > UINT64_C(1099511627776)) {
        error = "Unknown flash operation, flags, reserved fields, timeout or verified geometry"; return false;
    }
    if (request->operation != EJODIN4_FLASH_WRITE && request->flags) {
        error = "Reboot is permitted only for an explicit write operation"; return false;
    }
    if (request->operation == EJODIN4_FLASH_PLAN_ONLY) {
        if (request->device_path || request->device_path_bytes) {
            error = "Offline planning takes no device interface"; return false;
        }
    } else if (!valid_utf8_path(request->device_path, request->device_path_bytes)) {
        error = "Flash session needs one bounded exact UTF-8 device interface"; return false;
    }
    if (request->file_count > 6 || (!request->files && request->file_count) ||
        (request->files && !request->file_count)) {
        error = "Firmware package array/count is invalid"; return false;
    }
    const bool firmware = request->operation != EJODIN4_FLASH_READ_PIT || request->file_count != 0;
    if (firmware) {
        if (!request->file_count ||
            (request->data_policy != EJODIN4_PRESERVE_DATA && request->data_policy != EJODIN4_CLEAN_INSTALL && request->data_policy != EJODIN4_UPDATE_SELECTED) ||
            !request->decoded_limit || request->decoded_limit > UINT64_C(1099511627776)) {
            error = "Firmware planning/writing needs packages, an explicit data policy and bounded decode limit"; return false;
        }
    } else if (request->data_policy || request->decoded_limit) {
        error = "A PIT-only request without files takes no firmware policy or decode budget"; return false;
    }
    uint32_t roles = 0;
    const uint32_t max_role = request->data_policy == EJODIN4_UPDATE_SELECTED ? EJODIN4_PACKAGE_PIT_REFERENCE : EJODIN4_PACKAGE_USERDATA;
    for (uint32_t i = 0; i < request->file_count; ++i) {
        const EJODIN4_FirmwareFile& file = request->files[i];
        if (file.role < EJODIN4_PACKAGE_BL || file.role > max_role ||
            (roles & (1u << file.role)) || !file.path || file.path_bytes < 2 || file.path_bytes > 32768 ||
            file.path[file.path_bytes - 1] != 0 || std::memchr(file.path, 0, file.path_bytes - 1)) {
            error = "Firmware package roles/paths are invalid or duplicated"; return false;
        }
        roles |= 1u << file.role;
    }
    if (request->data_policy == EJODIN4_UPDATE_SELECTED &&
        ((roles == (1u << EJODIN4_PACKAGE_PIT_REFERENCE)) ||
         ((roles & (1u << EJODIN4_PACKAGE_CSC)) && (roles & (1u << EJODIN4_PACKAGE_HOME_CSC))))) {
        error = "Selected update needs an image archive and at most one CSC variant"; return false;
    }
    return true;
}
void copy_name(char* destination, size_t capacity, const std::string& value) {
    if (value.size() >= capacity) throw std::runtime_error("Internal flash plan text exceeds its ABI bound");
    std::memcpy(destination, value.c_str(), value.size() + 1);
}
struct Events {
    EJODIN4_FlashResult& result;
    EJODIN4_FlashProgressCallback callback;
    void* context;
    const PlanImage* image;
    uint32_t index;
    uint64_t image_acknowledged;
    const char* geometry_source;
    uint32_t last_phase;
    uint64_t last_data;
    Events(EJODIN4_FlashResult& r,EJODIN4_FlashProgressCallback cb,void* c):result(r),callback(cb),context(c),image(0),index(0),image_acknowledged(0),geometry_source(0),last_phase(0),last_data(0){}
    static void verified(void* c,const VerificationProgress& w){static_cast<Events*>(c)->verification(w);}
    static void session_changed(void* c,const SessionState& w){static_cast<Events*>(c)->session(w);}
    void verification(const VerificationProgress& work) {
        if(!callback)return;
        EJODIN4_FlashProgress event; std::memset(&event,0,sizeof(event));event.struct_size=sizeof(event);event.phase=work.phase;
        event.package_role=work.role;event.image_index=work.archive_index;event.image_count=work.archive_count;
        event.source_offset=work.completed_bytes;event.stored_bytes=work.total_bytes;
        copy_name(event.image_name,sizeof(event.image_name),work.name);
        callback(&event,context);
    }
    void emit(uint32_t phase, const EJODIN4_PitEntry* pit = 0, uint32_t skip = 0,
              uint32_t role = 0, const std::string& name = std::string()) {
        if (!callback) return;
        const uint64_t now = GetTickCount64();
        if (phase == EJODIN4_PHASE_DATA_ACK && last_phase == phase &&
            now - last_data < 250) return;
        if (phase == EJODIN4_PHASE_DATA_ACK) last_data = now;
        last_phase = phase;
        EJODIN4_FlashProgress event; std::memset(&event,0,sizeof(event)); event.struct_size = sizeof(event); event.phase = phase;
        event.image_index = index; event.image_count = result.image_count;
        event.total_bytes = result.total_bytes; event.acknowledged_bytes = result.acknowledged_bytes;
        if (image) {
            event.package_role = image->role; event.image_bytes = image->decoded_bytes;
            event.source_offset = image->source_offset; event.stored_bytes = image->stored_bytes;
            event.expanded_bytes = image->expanded_bytes; event.partition = image->route;
            event.image_acknowledged_bytes = image_acknowledged;
            copy_name(event.image_name, sizeof(event.image_name), image->name);
            copy_name(event.sha256, sizeof(event.sha256), image->sha256);
        }
        if (!image && phase == EJODIN4_PHASE_PLAN_VALIDATED && geometry_source)
            copy_name(event.image_name, sizeof(event.image_name), geometry_source);
        if (pit) event.partition = *pit;
        if (skip) { event.skip_reason = skip; event.package_role = role; copy_name(event.image_name, sizeof(event.image_name), name); }
        callback(&event, context);
    }
    void phase(uint32_t phase) { result.phase = phase; result.last_proven_phase = phase; emit(phase); }
    void session(const SessionState& state) {
        result.protocol_version = state.protocol_version; result.transport = state.transport;
        result.images_completed = state.images_completed; result.acknowledged_bytes = state.acknowledged_bytes;
        result.last_response_id = state.last_response_id; result.last_ack = state.last_ack;
        image_acknowledged = state.image_acknowledged_bytes;
        if (state.any_write) result.flags |= EJODIN4_FLASH_ANY_WRITE;
        if (result.operation == EJODIN4_FLASH_WRITE) {
            if (state.finalized) result.flags |= EJODIN4_FLASH_FINALIZED;
            if (state.reboot_sent) result.flags |= EJODIN4_FLASH_REBOOT_SENT;
        }
        // The public operation completes only after checked native cleanup.
        const uint32_t phase = state.phase == EJODIN4_PHASE_COMPLETE ?
            static_cast<uint32_t>(state.reboot_sent ? EJODIN4_PHASE_REBOOT : EJODIN4_PHASE_FINALIZING) : state.phase;
        if (phase) {
            result.phase = phase;
            if (phase != EJODIN4_PHASE_FAILED && phase != EJODIN4_PHASE_CANCELLED) result.last_proven_phase = phase;
        }
        emit(result.phase);
    }
};
struct EncodedAuditProgress {
    Events& events; const PlanImage& image; uint32_t archive_count;
    EncodedAuditProgress(Events& e,const PlanImage& i,uint32_t count):events(e),image(i),archive_count(count){}
    static void report(void* context,uint64_t done,uint64_t total){
        EncodedAuditProgress& audit=*static_cast<EncodedAuditProgress*>(context);VerificationProgress work;
        work.phase=EJODIN4_PHASE_VERIFY_IMAGE;work.role=audit.image.role;work.archive_index=uint32_t(audit.image.archive_index);work.archive_count=audit.archive_count;
        work.name=verification_member_name("Checking transfer data",audit.image.member_index,audit.image.name);work.completed_bytes=done;work.total_bytes=total;audit.events.verification(work);
    }
};
struct EncodedReaders {
    std::vector<EncodedImageReader*> readers;
    ~EncodedReaders(){for(size_t i=0;i<readers.size();++i)delete readers[i];}
private: EncodedReaders(const EncodedReaders&);EncodedReaders& operator=(const EncodedReaders&);
public: EncodedReaders(){}
};
EJODIN4_Status perform(const EJODIN4_FlashRequest& request, EJODIN4_FlashResult& output,
                       Events& events, const Cancellation& cancel, std::string& error, Platform* injected) {
    if (cancel.requested()) { error = "Flash operation cancelled before preflight"; return EJODIN4_CANCELLED; }
    events.phase(EJODIN4_PHASE_PREFLIGHT);
    std::unique_ptr<FirmwareSet> firmware;
    EJODIN4_Status status = EJODIN4_OK;
    if (request.file_count) {
        status = prepare_firmware_set(request.files, request.file_count, request.data_policy,
            request.decoded_limit, cancel, firmware, error,VerificationCallback(Events::verified,&events));
        if (status != EJODIN4_OK) return status;
        if (!firmware.get() || firmware->images.empty() || firmware->images.size() > UINT32_MAX) {
            error = "Firmware preparation returned no bounded transfer plan"; return EJODIN4_INTERNAL_ERROR;
        }
        output.image_count = static_cast<uint32_t>(firmware->images.size());
        output.total_bytes = firmware->total_bytes; output.flags |= EJODIN4_FLASH_PLAN_READY;
        for (size_t i = 0; i < firmware->images.size(); ++i) {
            events.image = &firmware->images[i]; events.index = static_cast<uint32_t>(i);
            events.phase(EJODIN4_PHASE_PLAN_IMAGE);
        }
        events.image = 0; events.index = 0;
        for (size_t k=0;k<firmware->skips.size();++k){const PlanSkip& skip=firmware->skips[k];events.emit(EJODIN4_PHASE_PLAN_SKIP, &skip.route, skip.reason, skip.role, skip.name);}
    }
    if (request.operation == EJODIN4_FLASH_PLAN_ONLY) {
        return firmware->verify_unchanged(error);
    }
    events.phase(EJODIN4_PHASE_PREFLIGHT);
    FirmwareIdentity identity;
    if(request.operation==EJODIN4_FLASH_WRITE){
        const EJODIN4_Status authenticated=firmware->authenticate_identity(identity,cancel,VerificationCallback(Events::verified,&events));
        if(authenticated==EJODIN4_CANCELLED)return authenticated;
        // Legacy versions do not require signed DEVINFO. Retain missing/failed
        // proof in context so newer native versions cannot authorize writes.
    }
    EncodedReaders encoded;
    if(request.operation==EJODIN4_FLASH_WRITE){
        encoded.readers.resize(firmware->images.size(),static_cast<EncodedImageReader*>(0));
        for(size_t i=0;i<firmware->images.size();++i){
            const PlanImage& image=firmware->images[i];EncodedAuditProgress audit(events,image,request.file_count);std::unique_ptr<EncodedImageReader> reader;
            status=firmware->open_encoded_image(i,reader,error,cancel,ArchiveScanCallback(EncodedAuditProgress::report,&audit));
            if(status==EJODIN4_DRIVER_UNSUPPORTED){error.clear();status=EJODIN4_OK;}
            if(status!=EJODIN4_OK)return status;
            encoded.readers[i]=reader.release();
        }
    }
    if (cancel.requested()) { error = "Flash operation cancelled before connecting"; return EJODIN4_CANCELLED; }
    events.phase(EJODIN4_PHASE_CONNECTING);
    std::unique_ptr<Platform> native;
    Platform* platform = injected;
    if (!platform) { native.reset(make_native_platform(true).release()); platform = native.get(); }
    if (!platform) { error = "Native Download platform creation failed"; return EJODIN4_INTERNAL_ERROR; }
    DownloadChannel channel(*platform, cancel);
    const uint32_t open_timeout = request.io_timeout_ms ? request.io_timeout_ms : 30000;
    status = channel.open(request.device_path, request.device_path_bytes, open_timeout, error);
    if (status != EJODIN4_OK) return status;
    Session session(channel,SessionProgress(Events::session_changed,&events),request.io_timeout_ms);
    std::vector<EJODIN4_PitEntry> pit;
    status = session.begin(pit, error, request.operation == EJODIN4_FLASH_WRITE ? firmware->total_bytes : 0,
        request.operation == EJODIN4_FLASH_WRITE ? &identity : 0);
    if (status == EJODIN4_OK) {
        output.pit_entry_count = static_cast<uint32_t>(pit.size());
        for (size_t k=0;k<pit.size();++k) events.emit(EJODIN4_PHASE_PIT_ENTRY,&pit[k]);
        events.phase(EJODIN4_PHASE_PIT_READY);
    }
    if (status == EJODIN4_OK && firmware.get()) {
        LayoutResolution geometry;
        status = resolve_layout_profile(*firmware, session.pit_bytes(), request.files, request.file_count,
            request.logical_sector_bytes, geometry, error);
        if (status == EJODIN4_OK) {
            output.logical_sector_bytes = geometry.sector_bytes;
            status = firmware->validate_device_pit(pit, geometry.sector_bytes, error,
                request.userdata_capacity_bytes);
        }
        if (status == EJODIN4_OK) {
            events.geometry_source = geometry.source;
            output.flags |= EJODIN4_FLASH_PIT_MATCHED;
            if(request.operation == EJODIN4_FLASH_WRITE && geometry.per_partition_units)
                output.flags |= EJODIN4_FLASH_PER_PARTITION_PIT_UNITS;
            output.logical_sector_bytes = geometry.sector_bytes;
            events.phase(EJODIN4_PHASE_PLAN_VALIDATED);
        }
    }
    if (status == EJODIN4_OK && request.operation == EJODIN4_FLASH_WRITE) {
        // Reject every unsupported route before any earlier image can be written.
        for(size_t route_index=0;route_index<firmware->images.size();++route_index) {
            status=session.validate_image(firmware->images[route_index],error);
            if(status!=EJODIN4_OK)break;
        }
        if(status==EJODIN4_OK)status = firmware->verify_unchanged(error);
        if (status == EJODIN4_OK && channel.transport() == EJODIN4_DOWNLOAD_WINUSB)
            status = session.notify_total(firmware->total_bytes, error);
        for (size_t i = 0; status == EJODIN4_OK && i < firmware->images.size(); ++i) {
            events.image = &firmware->images[i]; events.index = static_cast<uint32_t>(i);
            std::unique_ptr<ImageReader> reader;
            status = firmware->open_image(i, reader, error);
            if (status == EJODIN4_OK) status = session.send_image(firmware->images[i], *reader, error, session.compression_supported()?encoded.readers[i]:0);
        }
        events.image = 0; events.index = 0;
        if (status == EJODIN4_OK) status = firmware->verify_unchanged(error);
    }
    if (status == EJODIN4_OK) status = session.finish(request.operation == EJODIN4_FLASH_WRITE && (request.flags & EJODIN4_FLASH_REBOOT), error);
    const std::string primary = error;
    std::string cleanup;
    const EJODIN4_Status closed = session.close(cleanup);
    if (closed != EJODIN4_OK) {
        if (status == EJODIN4_OK) { status = closed; error = cleanup; }
        else error = primary + "; native cleanup: " + cleanup;
    }
    return status;
}
}

static EJODIN4_Status flash_api(
    const EJODIN4_FlashRequest* request, EJODIN4_FlashResult* output,
    EJODIN4_FlashProgressCallback progress, EJODIN4_CancelCallback cancel,
    void* context, char* text, uint32_t capacity, bool& started, Platform* injected) {
    std::string error;
    if (!valid_request(request, output, text, capacity, error)) {
        diagnostic(text, capacity, error); return EJODIN4_INVALID_ARGUMENT;
    }
    std::memset(output,0,sizeof(*output));
    EJODIN4_FlashResult& result = *output; started = true;
    result.struct_size = sizeof(result); result.operation = request->operation;
    Events events(result,progress,context);
    EJODIN4_Status status;
    try {
        const ejodin4::Cancellation cancellation = {cancel,context};
        status = perform(*request, result, events, cancellation, error, injected);
    } catch (const std::exception& exception) {
        error = std::string("Flash operation exception: ") + exception.what(); status = EJODIN4_INTERNAL_ERROR;
    } catch (...) { error = "Flash operation raised an exception"; status = EJODIN4_INTERNAL_ERROR; }
    // perform owns the set; a throwing callback can unwind it before this emit.
    events.image = 0; events.index = 0; events.image_acknowledged = 0;
    if (status == EJODIN4_OK) {
        if (request->operation == EJODIN4_FLASH_WRITE &&
            (!(result.flags & EJODIN4_FLASH_FINALIZED) || result.images_completed != result.image_count ||
             result.acknowledged_bytes != result.total_bytes)) {
            error = "Session did not prove all planned images and finalization"; status = EJODIN4_INVALID_RESPONSE;
        }
    }
    if (status == EJODIN4_OK) { result.phase = EJODIN4_PHASE_COMPLETE; }
    else { result.phase = status == EJODIN4_CANCELLED ? EJODIN4_PHASE_CANCELLED : EJODIN4_PHASE_FAILED; }
    diagnostic(text, capacity, error);
    // A callback exception never crosses the C boundary or upgrades failure.
    if (progress) { try { events.emit(result.phase); } catch (...) {} }
    return status;
}

static EJODIN4_Status execute_flash(Platform* injected,
    const EJODIN4_FlashRequest* request, EJODIN4_FlashResult* output,
    EJODIN4_FlashProgressCallback progress, EJODIN4_CancelCallback cancel,
    void* context, char* text, uint32_t capacity) {
    bool started = false;
    try { return flash_api(request, output, progress, cancel, context, text, capacity, started, injected); }
    catch (...) {
        if (started) {
            if (output->phase != EJODIN4_PHASE_FAILED && output->phase != EJODIN4_PHASE_CANCELLED)
                output->last_proven_phase = output->phase;
            output->phase = EJODIN4_PHASE_FAILED;
        }
        const char message[] = "Flash operation could not complete; inspect partial result before recovery";
        if (text && capacity) {
            const size_t bytes = std::min(sizeof(message) - 1, static_cast<size_t>(capacity - 1));
            std::memcpy(text, message, bytes); text[bytes] = 0;
        }
        return EJODIN4_INTERNAL_ERROR;
    }
}

namespace ejodin4 {
EJODIN4_Status flash_with_platform(Platform& platform,
    const EJODIN4_FlashRequest* request, EJODIN4_FlashResult* result,
    EJODIN4_FlashProgressCallback progress, EJODIN4_CancelCallback cancel,
    void* context, char* diagnostic, uint32_t capacity) {
    return execute_flash(&platform, request, result, progress, cancel, context, diagnostic, capacity);
}
}
EJODIN4_Status EJODIN4_CALL EJODIN4_FlashFirmware(
    const EJODIN4_FlashRequest* request, EJODIN4_FlashResult* result,
    EJODIN4_FlashProgressCallback progress, EJODIN4_CancelCallback cancel,
    void* context, char* diagnostic, uint32_t capacity) {
    return execute_flash(0, request, result, progress, cancel, context, diagnostic, capacity);
}
