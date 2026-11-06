#include "display.hpp"
#include "desktop.hpp"
#include "sessions.hpp"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <sddl.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <cwctype>
#include <cstring>

namespace ra {
namespace {
const GUID displayClass={0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
const wchar_t* hardwareId=L"Root\\MttVDD";
const wchar_t* ownershipKey=L"SOFTWARE\\SSHRemoteDesktop\\VirtualDisplay";
const wchar_t* configFolder=L"C:\\VirtualDisplayDriver";
const char* publisherThumbprint="3cf8cf26d8ba266c3a483ab7d26d4a818e317d76";
const std::map<std::wstring,std::string> packageHashes={
    {L"mttvdd.cat","08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6"},
    {L"MttVDD.dll","c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05"},
    {L"MttVDD.inf","550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d"},
    {L"vdd_settings.xml","edb2501d6d5da17f66d15d4b97a6f4a3f0d8963165ac4a6a6259d95118288020"}
};
std::string hex(const BYTE* p,size_t n) {const char* h="0123456789abcdef";std::string s;for(size_t i=0;i<n;++i){s+=h[p[i]>>4];s+=h[p[i]&15];}return s;}
void api_error(const char* operation,DWORD error=GetLastError()) {throw Error("DISPLAY_DRIVER_ERROR",std::string(operation)+": "+win_message(error),{{"win32_error",error}});}
struct Devices {HDEVINFO h;explicit Devices(bool enumerate=true):h(enumerate?SetupDiGetClassDevsW(&displayClass,nullptr,nullptr,DIGCF_PRESENT):SetupDiCreateDeviceInfoList(&displayClass,nullptr)){if(h==INVALID_HANDLE_VALUE)api_error("Open display device set");}~Devices(){SetupDiDestroyDeviceInfoList(h);}};
std::wstring property(HDEVINFO set,SP_DEVINFO_DATA& d,DWORD name) {
    DWORD type=0,n=0;SetupDiGetDeviceRegistryPropertyW(set,&d,name,&type,nullptr,0,&n);if(!n)return {};
    std::vector<BYTE> bytes(n+sizeof(wchar_t),0);if(!SetupDiGetDeviceRegistryPropertyW(set,&d,name,&type,bytes.data(),n,nullptr))return {};
    return reinterpret_cast<wchar_t*>(bytes.data());
}
bool matching(HDEVINFO set,SP_DEVINFO_DATA& d) {
    DWORD n=0;SetupDiGetDeviceRegistryPropertyW(set,&d,SPDRP_HARDWAREID,nullptr,nullptr,0,&n);if(!n)return false;
    std::vector<BYTE> bytes(n+sizeof(wchar_t)*2,0);if(!SetupDiGetDeviceRegistryPropertyW(set,&d,SPDRP_HARDWAREID,nullptr,bytes.data(),n,nullptr))return false;
    for(auto p=reinterpret_cast<wchar_t*>(bytes.data());*p;p+=wcslen(p)+1)if(_wcsicmp(p,hardwareId)==0)return true;
    return false;
}
std::wstring instance_id(HDEVINFO set,SP_DEVINFO_DATA& d) {wchar_t s[MAX_DEVICE_ID_LEN]{};if(!SetupDiGetDeviceInstanceIdW(set,&d,s,MAX_DEVICE_ID_LEN,nullptr))api_error("Read device instance");return s;}
std::wstring managed_id() {wchar_t value[MAX_DEVICE_ID_LEN]{};DWORD n=sizeof(value);if(RegGetValueW(HKEY_LOCAL_MACHINE,ownershipKey,L"InstanceId",RRF_RT_REG_SZ,nullptr,value,&n)!=ERROR_SUCCESS)return {};return value;}
void save_managed(const std::wstring& id) {HKEY key=nullptr;LSTATUS s=RegCreateKeyExW(HKEY_LOCAL_MACHINE,ownershipKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);if(s!=ERROR_SUCCESS)api_error("Create driver ownership record",s);s=RegSetValueExW(key,L"InstanceId",0,REG_SZ,reinterpret_cast<const BYTE*>(id.c_str()),static_cast<DWORD>((id.size()+1)*sizeof(wchar_t)));RegCloseKey(key);if(s!=ERROR_SUCCESS)api_error("Save driver ownership",s);}
void require_platform() {
    if constexpr(sizeof(void*)!=8)throw Error("UNSUPPORTED_PLATFORM","Virtual display installation currently requires the x64 helper on Windows 11 x64");
    auto os=platform_info();
    if(os["architecture"]!="x64" || os["major"]!=10 || os["build"].get<DWORD>()<22000 || os["workstation"]!=true)throw Error("UNSUPPORTED_PLATFORM","Virtual display installation currently requires the x64 helper on Windows 11 x64");
    if(!process_privileges().high())throw Error("ELEVATION_REQUIRED","Virtual display installation requires an already elevated SSH login");
}
std::string sha256(const std::wstring& file) {
    Handle input(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));if(!input.valid())api_error("Open pinned driver file");
    struct Hash {BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;~Hash(){if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);}} h;
    if(BCryptOpenAlgorithmProvider(&h.alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw Error("HASH_FAILED","Cannot initialize SHA256");
    DWORD size=0,n=0;if(BCryptGetProperty(h.alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&n,0)<0)throw Error("HASH_FAILED","Cannot query SHA256");
    std::vector<BYTE> object(size);if(BCryptCreateHash(h.alg,&h.hash,object.data(),size,nullptr,0,0)<0)throw Error("HASH_FAILED","Cannot create SHA256");
    BYTE buffer[65536],digest[32];DWORD read=0;while(true){if(!ReadFile(input.get(),buffer,sizeof(buffer),&read,nullptr))api_error("Read pinned driver file");if(!read)break;if(BCryptHashData(h.hash,buffer,read,0)<0)throw Error("HASH_FAILED","Cannot hash driver file");}
    if(BCryptFinishHash(h.hash,digest,sizeof(digest),0)<0)throw Error("HASH_FAILED","Cannot finish SHA256");return hex(digest,sizeof(digest));
}
void secure_directory(const std::wstring& path,bool allowExisting) {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    check_win(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;WD)",SDDL_REVISION_1,&descriptor,nullptr),"Driver directory ACL");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};BOOL ok=CreateDirectoryW(path.c_str(),&attributes);DWORD error=GetLastError();LocalFree(descriptor);
    if(!ok && !(allowExisting && error==ERROR_ALREADY_EXISTS))api_error("Create protected driver directory",error);
    DWORD flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES || !(flags&FILE_ATTRIBUTE_DIRECTORY) || (flags&FILE_ATTRIBUTE_REPARSE_POINT))throw Error("UNSAFE_DRIVER_DIRECTORY","Driver directory must be a regular local directory");
}
struct PackageStage {
    std::wstring path;
    explicit PackageStage(const std::wstring& source) {
        wchar_t windows[MAX_PATH]{};check_win(GetWindowsDirectoryW(windows,MAX_PATH),"Get Windows directory");path=std::wstring(windows)+L"\\Temp\\SSHRemoteDesktop-VDD-"+wide(random_id());secure_directory(path,false);
        try{for(const auto& entry:packageHashes){auto target=path+L"\\"+entry.first;check_win(CopyFileW((source+L"\\"+entry.first).c_str(),target.c_str(),TRUE),"Stage pinned driver file");if(sha256(target)!=entry.second)throw Error("DRIVER_HASH_MISMATCH","Pinned driver package file differs from release",{{"file",utf8(entry.first)}});}}catch(...){cleanup();throw;}
    }
    void cleanup(){for(const auto& entry:packageHashes)DeleteFileW((path+L"\\"+entry.first).c_str());RemoveDirectoryW(path.c_str());}
    ~PackageStage(){cleanup();}
};
struct Certificate {
    PCCERT_CONTEXT value=nullptr;
    ~Certificate(){if(value)CertFreeCertificateContext(value);}
};
bool trust_publisher(const std::wstring& file,bool authorized) {
    WINTRUST_FILE_INFO f{sizeof(f),file.c_str(),nullptr,nullptr};WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&f;trust.dwStateAction=WTD_STATEACTION_VERIFY;trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;LONG verified=WinVerifyTrust(nullptr,&action,&trust);trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&trust);
    if(verified!=ERROR_SUCCESS)throw Error("DRIVER_SIGNATURE_INVALID","Driver Authenticode validation failed",{{"trust_status",static_cast<uint32_t>(verified)}});
    HCERTSTORE store=nullptr;HCRYPTMSG msg=nullptr;DWORD encoding=0,content=0,format=0;
    check_win(CryptQueryObject(CERT_QUERY_OBJECT_FILE,file.c_str(),CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,CERT_QUERY_FORMAT_FLAG_BINARY,0,&encoding,&content,&format,&store,&msg,nullptr),"Read driver publisher");
    struct Close {HCERTSTORE s;HCRYPTMSG m;~Close(){if(m)CryptMsgClose(m);if(s)CertCloseStore(s,0);}} close{store,msg};
    DWORD n=0;check_win(CryptMsgGetParam(msg,CMSG_SIGNER_INFO_PARAM,0,nullptr,&n),"Query driver signer");std::vector<BYTE> bytes(n);check_win(CryptMsgGetParam(msg,CMSG_SIGNER_INFO_PARAM,0,bytes.data(),&n),"Read driver signer");auto signer=reinterpret_cast<CMSG_SIGNER_INFO*>(bytes.data());
    CERT_INFO info{};info.Issuer=signer->Issuer;info.SerialNumber=signer->SerialNumber;Certificate certificate;certificate.value=CertFindCertificateInStore(store,encoding,0,CERT_FIND_SUBJECT_CERT,&info,nullptr);if(!certificate.value)api_error("Find driver signing certificate");
    BYTE thumb[20];n=sizeof(thumb);check_win(CertGetCertificateContextProperty(certificate.value,CERT_SHA1_HASH_PROP_ID,thumb,&n),"Read driver publisher thumbprint");if(hex(thumb,n)!=publisherThumbprint)throw Error("DRIVER_PUBLISHER_MISMATCH","Unexpected driver signing publisher");
    HCERTSTORE publishers=CertOpenStore(CERT_STORE_PROV_SYSTEM_W,0,0,CERT_SYSTEM_STORE_LOCAL_MACHINE,L"TrustedPublisher");if(!publishers)api_error("Open machine TrustedPublisher store");struct StoreClose {HCERTSTORE s;~StoreClose(){CertCloseStore(s,0);}} publisherClose{publishers};
    CRYPT_HASH_BLOB blob{n,thumb};Certificate existing;existing.value=CertFindCertificateInStore(publishers,encoding,0,CERT_FIND_SHA1_HASH,&blob,nullptr);if(existing.value)return false;
    if(!authorized)throw Error("PUBLISHER_TRUST_REQUIRED","Installing this driver silently requires trusting its pinned SignPath publisher; set trust_publisher=true for an authorized installation",{{"store","LocalMachine/TrustedPublisher"},{"thumbprint",publisherThumbprint}});
    check_win(CertAddCertificateContextToStore(publishers,certificate.value,CERT_STORE_ADD_NEW,nullptr),"Trust pinned driver publisher");return true;
}
bool write_configuration() {
    DWORD attributes=GetFileAttributesW(configFolder);bool created=attributes==INVALID_FILE_ATTRIBUTES;
    secure_directory(configFolder,!created);
    std::wstring path=std::wstring(configFolder)+L"\\vdd_settings.xml";
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file.valid()){if(GetLastError()==ERROR_FILE_EXISTS)return false;api_error("Create single-monitor configuration");}
    const char* xml="<?xml version='1.0' encoding='utf-8'?><vdd_settings><monitors><count>1</count></monitors><gpu><friendlyname>default</friendlyname></gpu><resolutions><resolution><width>1920</width><height>1080</height><refresh_rate>60</refresh_rate></resolution></resolutions><options><CustomEdid>false</CustomEdid><HardwareCursor>true</HardwareCursor><SDR10bit>false</SDR10bit><HDRPlus>false</HDRPlus><logging>false</logging><debuglogging>false</debuglogging></options></vdd_settings>";
    DWORD done=0;check_win(WriteFile(file.get(),xml,static_cast<DWORD>(strlen(xml)),&done,nullptr),"Write virtual display settings");return true;
}
struct Topology {std::vector<DISPLAYCONFIG_PATH_INFO> paths;std::vector<DISPLAYCONFIG_MODE_INFO> modes;};
Topology topology(UINT32 flags) {
    for(int attempt=0;attempt<4;++attempt){
        UINT32 p=0,m=0;LONG e=GetDisplayConfigBufferSizes(flags,&p,&m);if(e!=ERROR_SUCCESS)api_error("Query display configuration sizes",e);
        if(!p)return {};
        // QueryDisplayConfig requires non-null array pointers even when no modes
        // are active. A genuinely headless console can report a zero mode count.
        Topology t;t.paths.resize(p);t.modes.resize(std::max<UINT32>(m,1));
        e=QueryDisplayConfig(flags,&p,t.paths.data(),&m,t.modes.data(),nullptr);
        if(e==ERROR_INSUFFICIENT_BUFFER)continue;if(e!=ERROR_SUCCESS)api_error("Query console display configuration",e);
        t.paths.resize(p);t.modes.resize(m);return t;
    }throw Error("DISPLAY_NOT_READY","Display topology kept changing");
}
std::wstring adapter_path(const DISPLAYCONFIG_PATH_INFO& p) {DISPLAYCONFIG_ADAPTER_NAME name{};name.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;name.header.size=sizeof(name);name.header.adapterId=p.sourceInfo.adapterId;name.header.id=p.sourceInfo.id;if(DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS)return {};return name.adapterDevicePath;}
bool is_vdd(const DISPLAYCONFIG_PATH_INFO& p,const json& devices) {
    auto path=adapter_path(p);std::transform(path.begin(),path.end(),path.begin(),::towupper);
    for(const auto& d:devices){auto id=wide(d["instance_id"].get<std::string>());std::replace(id.begin(),id.end(),L'\\',L'#');std::transform(id.begin(),id.end(),id.begin(),::towupper);if(path.find(id)!=std::wstring::npos)return true;}return false;
}
json path_json(const DISPLAYCONFIG_PATH_INFO& p,const json& devices) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};source.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;source.header.size=sizeof(source);source.header.adapterId=p.sourceInfo.adapterId;source.header.id=p.sourceInfo.id;DisplayConfigGetDeviceInfo(&source.header);
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};target.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;target.header.size=sizeof(target);target.header.adapterId=p.targetInfo.adapterId;target.header.id=p.targetInfo.id;DisplayConfigGetDeviceInfo(&target.header);
    return {{"source",utf8(source.viewGdiDeviceName)},{"monitor_name",utf8(target.monitorFriendlyDeviceName)},{"monitor_path",utf8(target.monitorDevicePath)},{"adapter_path",utf8(adapter_path(p))},{"target_available",p.targetInfo.targetAvailable!=FALSE},{"active",(p.flags&DISPLAYCONFIG_PATH_ACTIVE)!=0},{"output_technology",static_cast<UINT32>(p.targetInfo.outputTechnology)},{"virtualdrivers_vdd",is_vdd(p,devices)}};
}
}
json virtual_display_devices() {
    json out=json::array();Devices devices;auto managed=managed_id();SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
    for(DWORD i=0;SetupDiEnumDeviceInfo(devices.h,i,&d);++i)if(matching(devices.h,d)) {ULONG status=0,problem=0;CONFIGRET result=CM_Get_DevNode_Status(&status,&problem,d.DevInst,0);auto id=instance_id(devices.h,d);out.push_back({{"instance_id",utf8(id)},{"description",utf8(property(devices.h,d,SPDRP_DEVICEDESC))},{"managed",_wcsicmp(id.c_str(),managed.c_str())==0},{"started",result==CR_SUCCESS && (status&DN_STARTED)!=0},{"problem_code",problem},{"device_status",status}});}
    return out;
}
json install_virtual_display(const json& params) {
    require_platform();if(!params.contains("package_path") || !params["package_path"].is_string())throw Error("DRIVER_PACKAGE_REQUIRED","The pinned VirtualDrivers 25.7.23 driver-only package is required",{{"provider","VirtualDrivers"},{"version","25.7.23"}});
    auto current=virtual_display_devices();if(!current.empty())throw Error("DRIVER_STATE_CONFLICT","A VirtualDrivers device already exists; inspect its status before installing another",{{"devices",current}});
    PackageStage package(wide(params["package_path"].get<std::string>()));
    bool publisherAdded=trust_publisher(package.path+L"\\MttVDD.dll",params.value("trust_publisher",false));bool configCreated=write_configuration();
    Devices devices(false);SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
    check_win(SetupDiCreateDeviceInfoW(devices.h,L"Display",&displayClass,L"SSH Remote Desktop virtual display",nullptr,DICD_GENERATE_ID,&d),"Create virtual display device");
    const wchar_t ids[]=L"Root\\MttVDD\0";
    check_win(SetupDiSetDeviceRegistryPropertyW(devices.h,&d,SPDRP_HARDWAREID,reinterpret_cast<const BYTE*>(ids),sizeof(ids)),"Set virtual display hardware ID");
    SP_DEVINSTALL_PARAMS_W settings{};settings.cbSize=sizeof(settings);
    check_win(SetupDiGetDeviceInstallParamsW(devices.h,&d,&settings),"Read device installation settings");settings.Flags|=DI_QUIETINSTALL;
    check_win(SetupDiSetDeviceInstallParamsW(devices.h,&d,&settings),"Set quiet device installation");
    bool registered=false;BOOL reboot=FALSE;
    try {
        check_win(SetupDiCallClassInstaller(DIF_REGISTERDEVICE,devices.h,&d),"Register virtual display device");registered=true;
        BOOL nonInteractive=SetupSetNonInteractiveMode(TRUE);
        BOOL installed=UpdateDriverForPlugAndPlayDevicesW(nullptr,hardwareId,(package.path+L"\\MttVDD.inf").c_str(),INSTALLFLAG_FORCE|INSTALLFLAG_NONINTERACTIVE,&reboot);DWORD error=GetLastError();SetupSetNonInteractiveMode(nonInteractive);
        if(!installed)throw Error("DRIVER_INSTALL_FAILED","Windows rejected the virtual display driver: "+win_message(error),{{"win32_error",error},{"publisher_added",publisherAdded},{"reboot_required",reboot!=FALSE},{"setup_log","C:\\Windows\\INF\\setupapi.dev.log"}});
        auto id=instance_id(devices.h,d);save_managed(id);
        return {{"installed",true},{"instance_id",utf8(id)},{"publisher_added",publisherAdded},{"publisher_thumbprint",publisherThumbprint},{"publisher_store","LocalMachine/TrustedPublisher"},{"config_created",configCreated},{"config_path",utf8(std::wstring(configFolder)+L"\\vdd_settings.xml")},{"reboot_required",reboot!=FALSE},{"driver_release","25.7.23"}};
    }catch(...){if(registered)SetupDiCallClassInstaller(DIF_REMOVE,devices.h,&d);throw;}
}
json change_virtual_device(const std::string& action) {
    require_platform();auto id=managed_id();if(id.empty())throw Error("NOT_MANAGED","No virtual display device created by SSH Remote Desktop is recorded");
    Devices devices;SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
    if(!SetupDiOpenDeviceInfoW(devices.h,id.c_str(),nullptr,0,&d)){if(GetLastError()==ERROR_NO_SUCH_DEVINST)return {{"changed",false},{"present",false}};api_error("Open managed virtual display");}
    if(!matching(devices.h,d))throw Error("DRIVER_STATE_CONFLICT","Managed device no longer matches the pinned virtual display provider");
    if(action=="remove") {check_win(SetupDiCallClassInstaller(DIF_REMOVE,devices.h,&d),"Remove managed virtual display");RegDeleteTreeW(HKEY_LOCAL_MACHINE,ownershipKey);return {{"changed",true},{"removed",true},{"publisher_trust_retained",true},{"driver_store_retained",true}};}
    CONFIGRET result=action=="enable"?CM_Enable_DevNode(d.DevInst,0):CM_Disable_DevNode(d.DevInst,0);if(result!=CR_SUCCESS)throw Error("DISPLAY_DRIVER_ERROR","Cannot change virtual display device state",{{"configret",result}});
    return {{"changed",true},{"enabled",action=="enable"},{"instance_id",utf8(id)}};
}
Reply execute_display_worker(const json& request) {
    const auto& p=request["params"];auto action=p.value("action",std::string("status"));auto devices=virtual_display_devices();bool changed=false;
    if(action=="_activate") {
        Topology active;DISPLAYCONFIG_PATH_INFO chosen{};bool found=false,already=false;
        for(int attempt=0;attempt<20 && !found;++attempt) {
            auto all=topology(QDC_ALL_PATHS);active=topology(QDC_ONLY_ACTIVE_PATHS);
            for(const auto& path:active.paths)if(is_vdd(path,devices)){chosen=path;found=true;already=true;break;}
            if(!found)for(const auto& path:all.paths)if(path.targetInfo.targetAvailable && is_vdd(path,devices)){chosen=path;found=true;break;}
            if(!found)Sleep(250);
        }
        if(!found)throw Error("VIRTUAL_DISPLAY_NOT_READY","The installed driver has not exposed a monitor yet",{{"devices",devices}});
        bool virtualOnly=p.value("virtual_only",false);
        if(!already || (virtualOnly && active.paths.size()!=1)) {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths;if(!virtualOnly)for(auto path:active.paths)if(!is_vdd(path,devices))paths.push_back(path);
            chosen.flags=DISPLAYCONFIG_PATH_ACTIVE;paths.insert(paths.begin(),chosen);
            for(auto& path:paths){path.sourceInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;}
            LONG error=SetDisplayConfig(static_cast<UINT32>(paths.size()),paths.data(),0,nullptr,SDC_APPLY|SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_ALLOW_CHANGES|SDC_SAVE_TO_DATABASE);
            if(error!=ERROR_SUCCESS)api_error("Activate and persist virtual monitor",error);changed=true;Sleep(1000);
        }
    }
    auto active=topology(QDC_ONLY_ACTIVE_PATHS);json paths=json::array();bool virtualActive=false;for(const auto& path:active.paths){paths.push_back(path_json(path,devices));if(is_vdd(path,devices))virtualActive=true;}
    auto state=session_state(process_session());bool locked=state["locked"].is_boolean() && state["locked"].get<bool>();
    json info={{"session_id",process_session()},{"console_session_id",WTSGetActiveConsoleSessionId()},{"active_paths",paths},{"active_path_count",active.paths.size()},{"width",GetSystemMetrics(SM_CXVIRTUALSCREEN)},{"height",GetSystemMetrics(SM_CYVIRTUALSCREEN)},{"monitor_count",GetSystemMetrics(SM_CMONITORS)},{"devices",devices},{"virtual_active",virtualActive},{"locked",state["locked"]},{"changed",changed},{"capture_ok",false}};
    Reply reply=success(request);json observation=request;observation["params"]["screenshot"]=true;observation["params"]["uia"]=false;
    try{reply=observe_input_desktop(observation);info["capture_ok"]=!reply.image.empty();}catch(const Error& error){info["capture_error"]={{"code",error.code},{"message",error.what()}};}
    info["usable_display"]=!active.paths.empty() && GetSystemMetrics(SM_CXVIRTUALSCREEN)>0 && GetSystemMetrics(SM_CYVIRTUALSCREEN)>0 && info["capture_ok"]==true;
    info["desktop_ready"]=info["usable_display"]==true && !locked && on_input_desktop();
    info["needs_virtual_display"]=active.paths.empty();reply.header["result"].update(info);
    if(!p.value("screenshot",false)){reply.image.clear();reply.header["image_bytes"]=0;reply.header["mime_type"]=nullptr;}
    return reply;
}
}
