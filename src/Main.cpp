// Standalone C++Builder XE7 / classic BCC32 Windows client.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <process.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#define EJ_ODIN4_CORE_STATIC
#include "../include/EasyJtagOdin4Core.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

// No STL object, exception or native handle crosses the engine's packed C ABI.
typedef uint32_t (__cdecl *AbiFn)();
typedef EJODIN4_Status (__cdecl *EnumFn)(EJODIN4_UsbDevice*,uint32_t,uint32_t*,EJODIN4_CancelCallback,void*,char*,uint32_t);
typedef EJODIN4_Status (__cdecl *ProbeFn)(const char*,uint32_t,uint32_t,EJODIN4_DownloadProbe*,EJODIN4_CancelCallback,void*,char*,uint32_t);
typedef EJODIN4_Status (__cdecl *InspectFn)(const char*,uint32_t,EJODIN4_ArchiveEntry*,uint32_t,uint32_t*,EJODIN4_ArchiveSummary*,EJODIN4_CancelCallback,void*,char*,uint32_t);
typedef EJODIN4_Status (__cdecl *FlashFn)(const EJODIN4_FlashRequest*,EJODIN4_FlashResult*,EJODIN4_FlashProgressCallback,EJODIN4_CancelCallback,void*,char*,uint32_t);
typedef char CheckPointerSize[sizeof(void*)==4?1:-1];
typedef char CheckRequestSize[sizeof(EJODIN4_FlashRequest)==64?1:-1];
typedef char CheckProgressSize[sizeof(EJODIN4_FlashProgress)==537?1:-1];
typedef char CheckResultSize[sizeof(EJODIN4_FlashResult)==68?1:-1];

static HWND mainWindow,deviceBox,policyBox,rebootBox,logBox,statusText,overallBar,imageBar,overallText,imageText;
static HWND titleText,subtitleText,deviceLabel,policyLabel,firmwareLabel;
static HWND paths[6],labels[6],browseButtons[6],clearButtons[6],actions[7];
static HFONT uiFont,titleFont;static bool engine=false;static HANDLE worker;
static EnumFn enumDevices;static ProbeFn probeDevice;static InspectFn inspectArchive;static FlashFn flashFirmware;
static CRITICAL_SECTION progressLock;static EJODIN4_FlashProgress latestProgress;static bool progressPending=false;
static volatile LONG cancelFlag=0;static bool busy=false,closing=false;static std::vector<EJODIN4_UsbDevice> devices;
static std::wstring baseDirectory;static std::wstring logFilename;static FILE* sessionLog=0;
static const UINT WM_LOG_LINE=WM_APP+1,WM_FINISHED=WM_APP+2;
static const int ID_REFRESH=201,ID_PROBE=202,ID_PLAN=203,ID_PIT=204,ID_FLASH=205,ID_CANCEL=206,ID_SAVE=207;

static std::wstring widen(const std::string& value){if(value.empty())return L"";int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.c_str(),(int)value.size(),0,0);if(!n)throw std::runtime_error("Invalid UTF-8 from engine");std::wstring s(n,L' ');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.c_str(),(int)value.size(),&s[0],n);return s;}
static std::string utf8(const std::wstring& value){if(value.empty())return "";int n=WideCharToMultiByte(CP_UTF8,0,value.c_str(),(int)value.size(),0,0,0,0);if(!n)throw std::runtime_error("Cannot encode path");std::string s(n,' ');WideCharToMultiByte(CP_UTF8,0,value.c_str(),(int)value.size(),&s[0],n,0,0);return s;}
static std::wstring textOf(HWND h){int n=GetWindowTextLengthW(h);std::vector<wchar_t> s(n+1);GetWindowTextW(h,&s[0],n+1);return std::wstring(&s[0]);}
static std::wstring absolutePath(const std::wstring& p){if(p.empty())return p;DWORD n=GetFullPathNameW(p.c_str(),0,0,0);if(!n)throw std::runtime_error("Cannot resolve firmware path");std::vector<wchar_t> s(n+1);if(!GetFullPathNameW(p.c_str(),n+1,&s[0],0))throw std::runtime_error("Cannot resolve firmware path");return std::wstring(&s[0]);}
static std::string lower(std::string s){for(size_t i=0;i<s.size();++i)if(s[i]>='A'&&s[i]<='Z')s[i]=(char)(s[i]-'A'+'a');return s;}
static std::string basename(const std::string& s){size_t i=s.find_last_of("\\/");return s.substr(i==std::string::npos?0:i+1);}
static std::string folder(const std::string& s){size_t i=s.find_last_of("\\/");if(i==std::string::npos)return std::string();return s.substr(0,i+1);}
static std::string bounded(const char* p,size_t n){size_t i=0;while(i<n&&p[i])++i;if(i==n)throw std::runtime_error("Unterminated engine text");return std::string(p,i);}
static std::string decimal64(uint64_t n){char b[32];char* p=b+31;*p=0;do{*--p=(char)('0'+n%10);n/=10;}while(n);return std::string(p);}
static std::string fieldText(const char* p,size_t n){size_t i=0;while(i<n&&p[i])++i;return std::string(p,i);}
static std::wstring number(double x){wchar_t s[64];swprintf(s,L"%.1f",x);return s;}
static std::wstring mib(uint64_t n){return number((double)n/1048576.0)+L" MiB";}
static void appendLog(const std::wstring& text){std::wstring s=text+L"\r\n";int len=GetWindowTextLengthW(logBox);if(len>500000){SendMessageW(logBox,EM_SETSEL,0,100000);SendMessageW(logBox,EM_REPLACESEL,FALSE,(LPARAM)L"");}SendMessageW(logBox,EM_SETSEL,(WPARAM)-1,(LPARAM)-1);SendMessageW(logBox,EM_REPLACESEL,FALSE,(LPARAM)s.c_str());SendMessageW(logBox,EM_SCROLLCARET,0,0);if(sessionLog){std::string bytes=utf8(s);fwrite(bytes.c_str(),1,bytes.size(),sessionLog);fflush(sessionLog);}}
static void postLog(const std::string& text){std::wstring* s=new std::wstring(widen(text));if(!PostMessageW(mainWindow,WM_LOG_LINE,0,(LPARAM)s))delete s;}
static int32_t __cdecl cancelled(void*){return InterlockedCompareExchange(&cancelFlag,0,0)?1:0;}
static const char* phaseName(uint32_t n){switch(n){case 1:return "Validating firmware";case 2:return "Planned image";case 3:return "Opening selected device";case 4:return "Download handshake";case 5:return "Negotiating session";case 6:return "Device PIT entry";case 7:return "Device PIT read";case 8:return "Plan validated";case 9:return "Transferring image";case 10:return "Flashing";case 11:return "Image acknowledged";case 12:return "Finalizing session";case 13:return "Reboot requested";case 14:return "Operation complete";case 15:return "Operation failed";case 16:return "Operation cancelled";case 17:return "Skipped member";case 20:return "Verifying archive";case 21:return "Verifying image";default:return "Unknown progress";}}
static void __cdecl progressCallback(const EJODIN4_FlashProgress* p,void*){
    try{if(!p||p->struct_size!=sizeof(*p)){InterlockedExchange(&cancelFlag,1);return;}EnterCriticalSection(&progressLock);latestProgress=*p;progressPending=true;LeaveCriticalSection(&progressLock);
    if(p->phase==EJODIN4_PHASE_DATA_ACK||p->phase==EJODIN4_PHASE_VERIFY_ARCHIVE||p->phase==EJODIN4_PHASE_VERIFY_IMAGE)return;
    std::string line=phaseName(p->phase);std::string name=bounded(p->image_name,sizeof(p->image_name));if(!name.empty())line+=" | "+name;
    if(p->phase==EJODIN4_PHASE_PLAN_IMAGE||p->phase==EJODIN4_PHASE_IMAGE_BEGIN){char b[160];sprintf(b," | image %lu/%lu | slot %lu | PIT %lu",(unsigned long)p->image_index+1,(unsigned long)p->image_count,(unsigned long)p->package_role,(unsigned long)p->partition.identifier);line+=b;line+=" | SHA256 "+bounded(p->sha256,sizeof(p->sha256));}
    if(p->phase==EJODIN4_PHASE_PIT_ENTRY){line+=" | "+fieldText(p->partition.partition_name,32)+" | file "+fieldText(p->partition.file_name,32)+" | ID "+decimal64(p->partition.identifier)+" | block "+decimal64(p->partition.block_size_or_offset)+" | count "+decimal64(p->partition.block_count);}
    if(p->phase==EJODIN4_PHASE_PLAN_IMAGE)line+=" | decoded "+decimal64(p->image_bytes)+" | expanded "+decimal64(p->expanded_bytes);
    if(p->phase==EJODIN4_PHASE_PLAN_SKIP)line+=" | skip reason "+decimal64(p->skip_reason);
    postLog(line);
    }catch(...){InterlockedExchange(&cancelFlag,1);}
}
struct File {uint32_t role;std::string path;File(uint32_t r,const std::string& p):role(r),path(p){}};
struct Job {int action;uint32_t policy;bool reboot;std::string device;std::vector<File> files;};
struct Finished {int action;bool success;std::string message;std::vector<EJODIN4_UsbDevice> devices;EJODIN4_FlashResult result;Finished():action(0),success(false){ZeroMemory(&result,sizeof(result));}};
static bool byRole(const File& a,const File& b){return a.role<b.role;}
static bool containsPit(const std::string& path){
    postLog("Checking PIT reference: "+path);std::vector<EJODIN4_ArchiveEntry> entries(512);EJODIN4_ArchiveSummary sum;ZeroMemory(&sum,sizeof(sum));uint32_t count=0;char error[2048]={0};
    EJODIN4_Status s=inspectArchive(path.c_str(),(uint32_t)path.size()+1,&entries[0],512,&count,&sum,cancelled,0,error,sizeof(error));
    if(s!=EJODIN4_OK)throw std::runtime_error(std::string("Cannot inspect reference: ")+error);
    if(!count||count>512||sum.entry_count!=count)throw std::runtime_error("Invalid archive inventory from engine");
    bool pit=false;for(uint32_t i=0;i<count;++i)if(entries[i].flags&EJODIN4_ARCHIVE_ENTRY_PIT)pit=true;return pit;
}
static std::string matchingCsc(const std::string& home){std::string n=basename(home);if(lower(n).compare(0,9,"home_csc_")!=0)return "";return folder(home)+"CSC_"+n.substr(9);}
static void resolveReference(std::vector<File>& files,uint32_t policy){
    size_t ref=files.size(),home=files.size();bool csc=false;for(size_t i=0;i<files.size();++i){if(files[i].role==7)ref=i;if(files[i].role==5)home=i;if(files[i].role==4)csc=true;}
    if(ref<files.size()){
        if(containsPit(files[ref].path))return;std::string candidate=matchingCsc(files[ref].path);if(candidate.empty())throw std::runtime_error("Reference has no PIT. Select the matching CSC_ archive as PIT reference.");
        if(!containsPit(candidate))throw std::runtime_error("Matching CSC reference has no PIT");files[ref].path=candidate;postLog("Using matching CSC for PIT metadata only: "+candidate);return;
    }
    if(csc)return;
    if(home<files.size()){
        std::string candidate=matchingCsc(files[home].path);if(candidate.empty())throw std::runtime_error("HOME_CSC requires its matching CSC_ reference");
        if(!containsPit(candidate))throw std::runtime_error("Matching CSC reference has no PIT");
        // Preserve ABI uses CSC role as metadata when HOME_CSC is present.
        files.push_back(File(policy==EJODIN4_PRESERVE_DATA?4:7,candidate));postLog("PIT metadata only: "+candidate);return;
    }
    if(policy!=EJODIN4_UPDATE_SELECTED)return;
    std::vector<std::string> found;for(size_t i=0;i<files.size();++i){std::string dir=folder(files[i].path);WIN32_FIND_DATAW data;HANDLE search=FindFirstFileW(widen(dir+"CSC_*").c_str(),&data);if(search==INVALID_HANDLE_VALUE)continue;
        do{if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;std::string candidate=dir+utf8(data.cFileName),lname=lower(candidate);if(lname.size()<4)continue;if(lname.substr(lname.size()-4)!=".tar"&&(lname.size()<8||lname.substr(lname.size()-8)!=".tar.md5"))continue;bool exists=false;for(size_t j=0;j<found.size();++j)if(lower(found[j])==lname)exists=true;if(!exists)found.push_back(candidate);}while(FindNextFileW(search,&data));FindClose(search);}
    if(found.size()>1)throw std::runtime_error("Multiple CSC reference archives found. Select PIT reference explicitly.");
    if(found.size()==1){if(!containsPit(found[0]))throw std::runtime_error("Discovered CSC has no PIT");files.push_back(File(7,found[0]));postLog("PIT metadata only: "+found[0]);}
}
static unsigned __stdcall runWorker(void* parameter){
    Job* job=(Job*)parameter;Finished* done=new Finished;done->action=job->action;char error[2048]={0};
    try{
        if(job->action==ID_REFRESH){std::vector<EJODIN4_UsbDevice> list(128);uint32_t count=0;EJODIN4_Status s=enumDevices(&list[0],128,&count,cancelled,0,error,sizeof(error));if(s!=EJODIN4_OK)throw std::runtime_error(error);if(count>128)throw std::runtime_error("Too many Download devices");list.resize(count);done->devices.swap(list);done->success=true;done->message="Device list refreshed. Select a supported interface explicitly.";}
        else if(job->action==ID_PROBE){EJODIN4_DownloadProbe p;ZeroMemory(&p,sizeof(p));EJODIN4_Status s=probeDevice(job->device.c_str(),(uint32_t)job->device.size()+1,30000,&p,cancelled,0,error,sizeof(error));if(s!=EJODIN4_OK)throw std::runtime_error(error);done->success=p.handshake_complete!=0;done->message=done->success?"ODIN / LOKE handshake confirmed. No firmware session started.":"Handshake was not confirmed.";}
        else{
            if(job->action!=ID_PIT)resolveReference(job->files,job->policy);std::sort(job->files.begin(),job->files.end(),byRole);std::vector<EJODIN4_FirmwareFile> native;
            for(size_t i=0;i<job->files.size();++i){EJODIN4_FirmwareFile f;f.role=job->files[i].role;f.path=job->files[i].path.c_str();f.path_bytes=(uint32_t)job->files[i].path.size()+1;native.push_back(f);char b[60];sprintf(b,"Archive role %lu: ",(unsigned long)f.role);postLog(std::string(b)+job->files[i].path);}
            EJODIN4_FlashRequest request;ZeroMemory(&request,sizeof(request));request.struct_size=sizeof(request);request.operation=job->action==ID_PLAN?EJODIN4_FLASH_PLAN_ONLY:job->action==ID_PIT?EJODIN4_FLASH_READ_PIT:EJODIN4_FLASH_WRITE;
            request.data_policy=job->action==ID_PIT?0:job->policy;request.flags=job->action==ID_FLASH&&job->reboot?EJODIN4_FLASH_REBOOT:0;
            if(job->action!=ID_PLAN){request.device_path=job->device.c_str();request.device_path_bytes=(uint32_t)job->device.size()+1;}
            request.files=native.empty()?0:&native[0];request.file_count=(uint32_t)native.size();request.decoded_limit=job->action==ID_PIT?0:((uint64_t)64*1024*1024*1024);request.io_timeout_ms=job->action==ID_FLASH?0:30000;
            EJODIN4_Status s=flashFirmware(&request,&done->result,progressCallback,cancelled,0,error,sizeof(error));done->success=s==EJODIN4_OK;
            done->message=done->success?std::string(job->action==ID_PLAN?"Offline plan complete. Live PIT and geometry checks still run before writing.":job->action==ID_PIT?"Device PIT read completed. No image transfers or reboot.":"All image transfers acknowledged and session finalized. Verify Android boot."):std::string(error);
            char b[350];sprintf(b,"Final state: protocol %lu | images %lu/%lu | last response 0x%lX / ACK 0x%lX | any write: %s | finalized: %s",(unsigned long)done->result.protocol_version,(unsigned long)done->result.images_completed,(unsigned long)done->result.image_count,(unsigned long)done->result.last_response_id,(unsigned long)done->result.last_ack,(done->result.flags&1)?"yes":"no",(done->result.flags&2)?"yes":"no");postLog(b);postLog("Bytes acknowledged: "+decimal64(done->result.acknowledged_bytes)+"/"+decimal64(done->result.total_bytes));
        }
    }catch(const std::exception& e){done->message=e.what();}catch(...){done->message="Unexpected operation exception";}
    delete job;PostMessageW(mainWindow,WM_FINISHED,0,(LPARAM)done);return 0;
}
static void setBusy(bool value){busy=value;for(int i=0;i<6;++i){EnableWindow(paths[i],!value);EnableWindow(browseButtons[i],!value);EnableWindow(clearButtons[i],!value);}for(int i=0;i<7;++i)EnableWindow(actions[i],engine&&(i==5?value:!value));EnableWindow(deviceBox,!value);EnableWindow(policyBox,!value);EnableWindow(rebootBox,!value);}
static uint32_t selectedPolicy(){int n=(int)SendMessageW(policyBox,CB_GETCURSEL,0,0);return n==1?EJODIN4_PRESERVE_DATA:n==2?EJODIN4_CLEAN_INSTALL:EJODIN4_UPDATE_SELECTED;}
static void beginOperation(int action){
    if(busy||!engine)return;Job* job=new Job;job->action=action;job->policy=selectedPolicy();job->reboot=SendMessageW(rebootBox,BM_GETCHECK,0,0)==BST_CHECKED;
    try{
        if(action==ID_PROBE||action==ID_PIT||action==ID_FLASH){int n=(int)SendMessageW(deviceBox,CB_GETCURSEL,0,0);if(n<0||(size_t)n>=devices.size()||!(devices[n].flags==1||devices[n].flags==2))throw std::runtime_error("Refresh and explicitly select a supported Download device first.");job->device=bounded(devices[n].device_path,sizeof(devices[n].device_path));}
        if(action==ID_PLAN||action==ID_FLASH){for(int i=0;i<6;++i){std::wstring path=textOf(paths[i]);if(path.empty())continue;std::string p=utf8(absolutePath(path));uint32_t role=i<3?i+1:i==3?(lower(basename(p)).compare(0,9,"home_csc_")==0?5:4):i==4?6:7;job->files.push_back(File(role,p));}
            if(job->files.empty()||(job->files.size()==1&&job->files[0].role==7))throw std::runtime_error("Choose at least one firmware image archive.");
            if(job->policy!=EJODIN4_UPDATE_SELECTED)for(size_t j=0;j<job->files.size();++j)if(job->files[j].role==7)throw std::runtime_error("Explicit PIT reference is for Update selected slots. Preserve auto-resolves the HOME_CSC sibling.");
            if(action==ID_FLASH){std::wstring message=L"Write the selected firmware to this Download device?\n\n"+widen(job->device)+L"\n\n"+(job->policy==EJODIN4_CLEAN_INSTALL?std::wstring(L"Clean installation permits user-data erasure."):std::wstring(L"Data retention is not guaranteed."))+L"\nCancellation cannot undo partition writes.";if(MessageBoxW(mainWindow,message.c_str(),L"Confirm firmware write",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES){delete job;return;}}
        }
        InterlockedExchange(&cancelFlag,0);EnterCriticalSection(&progressLock);progressPending=false;LeaveCriticalSection(&progressLock);setBusy(true);SetWindowTextW(statusText,L"Operation started...");
        if(action==ID_PLAN||action==ID_FLASH){SendMessageW(overallBar,PBM_SETPOS,0,0);SendMessageW(imageBar,PBM_SETPOS,0,0);SetWindowTextW(overallText,L"Verification pending");SetWindowTextW(imageText,L"");}
        appendLog(L"--- "+std::wstring(action==ID_PLAN?L"Offline plan":action==ID_FLASH?L"Flash selected firmware":action==ID_REFRESH?L"Refresh devices":action==ID_PROBE?L"Check connection":L"Read device PIT")+L" ---");
        unsigned id=0;worker=(HANDLE)_beginthreadex(0,0,runWorker,job,0,&id);if(!worker){setBusy(false);throw std::runtime_error("Cannot start operation worker");}
    }catch(const std::exception& e){delete job;appendLog(widen(e.what()));SetWindowTextW(statusText,widen(e.what()).c_str());}
}
static void updateProgress(){EJODIN4_FlashProgress p;bool valid=false;EnterCriticalSection(&progressLock);if(progressPending){p=latestProgress;progressPending=false;valid=true;}LeaveCriticalSection(&progressLock);if(!valid)return;
    try{std::wstring name=widen(bounded(p.image_name,sizeof(p.image_name)));SetWindowTextW(statusText,(widen(phaseName(p.phase))+(name.empty()?std::wstring():std::wstring(L": ")+name)).c_str());
    if(p.phase==20||p.phase==21){unsigned pos=p.stored_bytes?(unsigned)((double)p.source_offset*1000.0/(double)p.stored_bytes):0;SendMessageW(overallBar,PBM_SETPOS,std::min(pos,1000u),0);SetWindowTextW(overallText,(L"Verification: "+number((double)pos/10)+L"%  |  "+mib(p.source_offset)+L" / "+mib(p.stored_bytes)).c_str());SetWindowTextW(imageText,name.c_str());}
    else if(p.phase==9||p.phase==10||p.phase==11){unsigned a=p.total_bytes?(unsigned)((double)p.acknowledged_bytes*1000/(double)p.total_bytes):0,b=p.image_bytes?(unsigned)((double)p.image_acknowledged_bytes*1000/(double)p.image_bytes):0;SendMessageW(overallBar,PBM_SETPOS,std::min(a,1000u),0);SendMessageW(imageBar,PBM_SETPOS,std::min(b,1000u),0);SetWindowTextW(overallText,(L"Flashing: "+number((double)p.acknowledged_bytes*100.0/(double)(p.total_bytes?p.total_bytes:1))+L"%  |  "+mib(p.acknowledged_bytes)+L" / "+mib(p.total_bytes)).c_str());SetWindowTextW(imageText,(name+L"  |  "+mib(p.image_acknowledged_bytes)+L" / "+mib(p.image_bytes)).c_str());}
    }catch(...){InterlockedExchange(&cancelFlag,1);}}
static HWND control(const wchar_t* cls,const wchar_t* text,DWORD style,int id){HWND h=CreateWindowExW(cls==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,mainWindow,(HMENU)(INT_PTR)id,GetModuleHandleW(0),0);SendMessageW(h,WM_SETFONT,(WPARAM)uiFont,TRUE);return h;}
static void position(HWND h,int x,int y,int w,int height){MoveWindow(h,x,y,std::max(1,w),height,TRUE);}
static void layout(){if(!titleText)return;RECT r;GetClientRect(mainWindow,&r);int w=r.right,h=r.bottom;position(titleText,20,14,w-40,30);position(subtitleText,20,47,w-40,22);position(deviceLabel,20,83,90,22);position(deviceBox,112,78,w-362,250);position(actions[0],w-238,78,100,29);position(actions[1],w-128,78,108,29);
    position(firmwareLabel,20,122,w-40,22);for(int i=0;i<6;++i){int y=150+i*38;position(labels[i],20,y+4,125,24);position(paths[i],150,y,w-310,28);position(browseButtons[i],w-150,y,86,28);position(clearButtons[i],w-58,y,38,28);}
    position(policyLabel,20,387,125,22);position(policyBox,150,382,w-430,150);position(rebootBox,w-265,383,245,26);
    position(actions[2],20,426,190,33);position(actions[3],220,426,140,33);position(actions[4],370,426,210,33);position(actions[5],590,426,100,33);position(actions[6],w-150,426,130,33);
    position(overallText,20,473,w-40,20);position(overallBar,20,497,w-40,16);position(imageText,20,522,w-40,20);position(imageBar,20,546,w-40,12);position(logBox,20,574,w-40,h-620);position(statusText,20,h-35,w-40,26);}
static void browse(int slot){wchar_t file[32768]={0};std::wstring current=textOf(paths[slot]);if(current.size()<32768)wcscpy(file,current.c_str());OPENFILENAMEW of;ZeroMemory(&of,sizeof(of));of.lStructSize=sizeof(of);of.hwndOwner=mainWindow;of.lpstrFilter=L"Samsung firmware (*.tar;*.tar.md5)\0*.tar;*.tar.md5\0All files\0*.*\0";of.lpstrFile=file;of.nMaxFile=32768;of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&of))SetWindowTextW(paths[slot],file);}
static void saveLog(){wchar_t path[32768]=L"odin-session.log";OPENFILENAMEW of;ZeroMemory(&of,sizeof(of));of.lStructSize=sizeof(of);of.hwndOwner=mainWindow;of.lpstrFilter=L"Log files\0*.log\0All files\0*.*\0";of.lpstrFile=path;of.nMaxFile=32768;of.Flags=OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR;if(GetSaveFileNameW(&of)){if(!CopyFileW(logFilename.c_str(),path,FALSE))MessageBoxW(mainWindow,L"Cannot save log file.",L"Save log",MB_OK|MB_ICONERROR);}}
static void loadEngine(){
    if(EJODIN4_GetAbiVersion()!=0x10009){appendLog(L"Native engine version mismatch.");setBusy(false);return;}
    enumDevices=&EJODIN4_EnumerateDownloadDevices;probeDevice=&EJODIN4_ProbeDownloadConnection;
    inspectArchive=&EJODIN4_InspectFirmwareArchive;flashFirmware=&EJODIN4_FlashFirmware;
    engine=true;setBusy(false);appendLog(L"Odin BCC ready. Native Borland engine linked directly; no engine DLL and no USB device opened.");
    appendLog(L"Session log: "+logFilename);SetWindowTextW(statusText,L"Ready. Choose archives; prepare an offline plan before flashing.");
}
static LRESULT CALLBACK windowProcedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){case WM_CREATE:{mainWindow=window;uiFont=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");titleFont=CreateFontW(-25,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        titleText=control(L"STATIC",L"Odin BCC",0,100);SendMessageW(titleText,WM_SETFONT,(WPARAM)titleFont,TRUE);subtitleText=control(L"STATIC",L"Samsung Download-mode firmware client",0,101);deviceLabel=control(L"STATIC",L"Device",0,102);deviceBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,103);firmwareLabel=control(L"STATIC",L"Firmware archives  |  Leave unselected slots empty. PIT references supply metadata only.",0,104);
        const wchar_t* names[]={L"BL bootloader",L"AP application",L"CP modem",L"CSC / HOME_CSC",L"USERDATA archive",L"PIT reference"};for(int i=0;i<6;++i){labels[i]=control(L"STATIC",names[i],0,110+i);paths[i]=control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,120+i);browseButtons[i]=control(L"BUTTON",L"Browse...",WS_TABSTOP,300+i);clearButtons[i]=control(L"BUTTON",L"X",WS_TABSTOP,310+i);}
        policyLabel=control(L"STATIC",L"Data policy",0,140);policyBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,141);SendMessageW(policyBox,CB_ADDSTRING,0,(LPARAM)L"Update selected slots (exclude USERDATA / MISC / OMR)");SendMessageW(policyBox,CB_ADDSTRING,0,(LPARAM)L"Preserve data (requires OEM download list)");SendMessageW(policyBox,CB_ADDSTRING,0,(LPARAM)L"Clean installation (allow data erasure)");SendMessageW(policyBox,CB_SETCURSEL,0,0);rebootBox=control(L"BUTTON",L"Reboot after successful flash",BS_AUTOCHECKBOX|WS_TABSTOP,142);SendMessageW(rebootBox,BM_SETCHECK,BST_CHECKED,0);
        const wchar_t* an[]={L"Refresh",L"Check link",L"Prepare offline plan",L"Read device PIT",L"Flash selected firmware",L"Cancel",L"Save log..."};for(int i=0;i<7;++i)actions[i]=control(L"BUTTON",an[i],WS_TABSTOP,ID_REFRESH+i);
        overallText=control(L"STATIC",L"Verification / overall transfer",0,160);overallBar=control(PROGRESS_CLASSW,L"",PBS_SMOOTH,161);imageText=control(L"STATIC",L"Current image",0,162);imageBar=control(PROGRESS_CLASSW,L"",PBS_SMOOTH,163);SendMessageW(overallBar,PBM_SETRANGE32,0,1000);SendMessageW(imageBar,PBM_SETRANGE32,0,1000);logBox=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_HSCROLL,164);SendMessageW(logBox,EM_SETLIMITTEXT,600000,0);statusText=control(L"STATIC",L"Loading engine...",SS_PATHELLIPSIS,165);layout();SetTimer(window,1,100,0);loadEngine();return 0;}
    case WM_SIZE:layout();return 0;
    case WM_GETMINMAXINFO:{MINMAXINFO* m=(MINMAXINFO*)lp;m->ptMinTrackSize.x=1040;m->ptMinTrackSize.y=760;return 0;}
    case WM_COMMAND:{int id=LOWORD(wp);if(id>=300&&id<306&&!busy){browse(id-300);return 0;}if(id>=310&&id<316&&!busy){SetWindowTextW(paths[id-310],L"");return 0;}if(id==ID_CANCEL){if(busy&&InterlockedExchange(&cancelFlag,1)==0){appendLog(L"Cancellation requested. Waiting for engine/driver to release the session safely.");SetWindowTextW(statusText,L"Cancelling...");}return 0;}if(id==ID_SAVE&&!busy){saveLog();return 0;}if(id>=ID_REFRESH&&id<=ID_FLASH){beginOperation(id);return 0;}break;}
    case WM_TIMER:updateProgress();if(closing&&!busy)DestroyWindow(window);return 0;
    case WM_LOG_LINE:{std::wstring* s=(std::wstring*)lp;appendLog(*s);delete s;return 0;}
    case WM_FINISHED:{Finished* done=(Finished*)lp;updateProgress();if(worker){WaitForSingleObject(worker,INFINITE);CloseHandle(worker);worker=0;}setBusy(false);
        if(done->action==ID_REFRESH&&done->success){devices.swap(done->devices);SendMessageW(deviceBox,CB_RESETCONTENT,0,0);for(size_t i=0;i<devices.size();++i){std::string d=bounded(devices[i].description,sizeof(devices[i].description));d+=" | "+bounded(devices[i].driver,sizeof(devices[i].driver));if(!devices[i].flags)d+=" | unsupported";SendMessageW(deviceBox,CB_ADDSTRING,0,(LPARAM)widen(d).c_str());}SendMessageW(deviceBox,CB_SETCURSEL,(WPARAM)-1,0);}
        appendLog((done->success?L"SUCCESS: ":L"FAILED: ")+widen(done->message));SetWindowTextW(statusText,widen(done->message).c_str());if(done->success&&(done->action==ID_PLAN||done->action==ID_FLASH)){SendMessageW(overallBar,PBM_SETPOS,1000,0);if(done->action==ID_FLASH)SendMessageW(imageBar,PBM_SETPOS,1000,0);}delete done;return 0;}
    case WM_CLOSE:if(busy){closing=true;InterlockedExchange(&cancelFlag,1);SetWindowTextW(statusText,L"Closing after safe cancellation completes...");return 0;}DestroyWindow(window);return 0;
    case WM_DESTROY:KillTimer(window,1);PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,msg,wp,lp);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int show){
    wchar_t exe[32768];DWORD n=GetModuleFileNameW(0,exe,32768);if(!n||n>=32768)return 2;baseDirectory=exe;size_t slash=baseDirectory.find_last_of(L"\\/");baseDirectory=baseDirectory.substr(0,slash+1);
    CreateDirectoryW((baseDirectory+L"logs").c_str(),0);SYSTEMTIME now;GetLocalTime(&now);wchar_t name[160];swprintf(name,L"logs\\odin-%04u%02u%02u-%02u%02u%02u-%lu.log",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,(unsigned long)GetCurrentProcessId());logFilename=baseDirectory+name;sessionLog=_wfopen(logFilename.c_str(),L"wb");InitializeCriticalSection(&progressLock);
    INITCOMMONCONTROLSEX controls;controls.dwSize=sizeof(controls);controls.dwICC=ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES;InitCommonControlsEx(&controls);
    WNDCLASSEXW cls;ZeroMemory(&cls,sizeof(cls));cls.cbSize=sizeof(cls);cls.lpfnWndProc=windowProcedure;cls.hInstance=instance;cls.hCursor=LoadCursor(0,IDC_ARROW);cls.hIcon=LoadIcon(0,IDI_APPLICATION);cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);cls.lpszClassName=L"OdinBCCMainWindow";RegisterClassExW(&cls);
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"Odin BCC - Samsung Firmware",WS_OVERLAPPEDWINDOW,80,60,1140,840,0,0,instance,0);if(!window){if(sessionLog)fclose(sessionLog);DeleteCriticalSection(&progressLock);return 3;}ShowWindow(window,show);UpdateWindow(window);
    MSG msg;while(GetMessageW(&msg,0,0,0)>0){if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(sessionLog)fclose(sessionLog);DeleteObject(uiFont);DeleteObject(titleFont);DeleteCriticalSection(&progressLock);return (int)msg.wParam;
}
