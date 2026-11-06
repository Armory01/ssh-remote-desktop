#include "desktop.hpp"
#include "sessions.hpp"
#include <uiautomation.h>
#include <wincodec.h>
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <memory>
#include <future>
#include <thread>

namespace ra {
namespace {
struct DesktopLock {
    Handle mutex;
    DesktopLock(DWORD timeout) : mutex(CreateMutexW(nullptr,FALSE,(L"Local\\RemoteAgent.Desktop."+wide(user_sid())).c_str())) {
        check_win(mutex.valid(),"Create desktop mutex");DWORD r=WaitForSingleObject(mutex.get(),timeout);
        if(r!=WAIT_OBJECT_0 && r!=WAIT_ABANDONED) throw Error("DESKTOP_BUSY","Another desktop request is in progress");
    }
    ~DesktopLock(){ReleaseMutex(mutex.get());}
};
json rect_json(const RECT& r){return {{"x",r.left},{"y",r.top},{"width",r.right-r.left},{"height",r.bottom-r.top}};}
RECT virtual_rect(){LONG x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN);return {x,y,x+GetSystemMetrics(SM_CXVIRTUALSCREEN),y+GetSystemMetrics(SM_CYVIRTUALSCREEN)};}
std::string bstr_text(BSTR text,size_t limit=4096) {
    if(!text) return {};
    auto n=std::min<size_t>(SysStringLen(text),limit);
    std::wstring s(text,n);if(!s.empty() && s.back()>=0xD800 && s.back()<=0xDBFF)s.pop_back();
    std::string out;try{out=utf8(s);}catch(const Error&){out="<invalid text>";}SysFreeString(text);return out;
}
uint64_t process_start(DWORD pid) {
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!p.valid())return 0;
    FILETIME c{},e{},k{},u{};if(!GetProcessTimes(p.get(),&c,&e,&k,&u))return 0;
    return (static_cast<uint64_t>(c.dwHighDateTime)<<32)|c.dwLowDateTime;
}
json window_info(HWND hwnd) {
    if(!hwnd || !IsWindow(hwnd))throw Error("STALE_TARGET","Window no longer exists");
    DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);RECT r{};check_win(GetWindowRect(hwnd,&r),"GetWindowRect");
    wchar_t title[2048]{},type[512]{};GetWindowTextW(hwnd,title,2048);GetClassNameW(hwnd,type,512);
    json info={{"hwnd",reinterpret_cast<uintptr_t>(hwnd)},{"pid",pid},{"process_start",process_start(pid)},
        {"title",utf8(title)},{"class",utf8(type)},{"bounds",rect_json(r)},
        {"visible",IsWindowVisible(hwnd)!=FALSE},{"minimized",IsIconic(hwnd)!=FALSE},{"foreground",GetForegroundWindow()==hwnd}};
    try{info["privileges"]=process_privileges(pid).describe();}catch(const Error&){info["privileges"]=nullptr;}
    return info;
}
HWND target_window(const json& params,bool required=false) {
    if(!params.contains("window")) {
        if(required)throw Error("INVALID_REQUEST","params.window is required");return nullptr;
    }
    if(!params["window"].is_number_integer())throw Error("INVALID_REQUEST","window must be a numeric HWND");
    HWND hwnd=reinterpret_cast<HWND>(params["window"].get<uintptr_t>());
    auto info=window_info(hwnd);DWORD sid=0;
    if(!ProcessIdToSessionId(info["pid"].get<DWORD>(),&sid) || sid!=process_session())throw Error("STALE_TARGET","Window is not in this desktop session");
    if(params.contains("expected")) {
        const auto& expected=params["expected"];
        if(!expected.is_object())throw Error("INVALID_REQUEST","expected must be an object");
        for(const auto* key:{"pid","process_start","bounds"})if(expected.contains(key) && expected[key]!=info[key])
            throw Error("STALE_TARGET",std::string("Window ")+key+" changed; observe again",{{"current",info}});
    }
    if(required && info["privileges"].is_object() && info["privileges"]["integrity_rid"].get<DWORD>()>process_privileges().integrity_rid)
        throw Error("ELEVATION_REQUIRED","Target window has higher integrity; request params.elevated=true from an elevated SSH process",{{"target",info}});
    return hwnd;
}
json list_windows() {
    json list=json::array();
    EnumWindows([](HWND hwnd,LPARAM param)->BOOL {
        auto& out=*reinterpret_cast<json*>(param);
        if(!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd)==0)return TRUE;
        try{out.push_back(window_info(hwnd));}catch(...){}
        return out.size()<256;
    },reinterpret_cast<LPARAM>(&list));return list;
}
void activate(HWND hwnd) {
    if(!IsWindowVisible(hwnd))throw Error("STALE_TARGET","Target window is not visible");
    if(IsIconic(hwnd))ShowWindow(hwnd,SW_RESTORE);
    SetForegroundWindow(hwnd);
    if(GetForegroundWindow()!=hwnd) {
        DWORD fg=GetWindowThreadProcessId(GetForegroundWindow(),nullptr),self=GetCurrentThreadId();
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
        BOOL attached=fg && fg!=self && AttachThreadInput(self,fg,TRUE);
        SetForegroundWindow(hwnd);BringWindowToTop(hwnd);
        if(attached)AttachThreadInput(self,fg,FALSE);
    }
    if(GetForegroundWindow()!=hwnd) {
        DWORD foregroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);bool higher=false;
        try{higher=foregroundPid && process_privileges(foregroundPid).integrity_rid>process_privileges().integrity_rid;}catch(const Error&){}
        if(higher)throw Error("ELEVATION_REQUIRED","Windows refused focus while a higher-integrity window is foreground; use an authorized elevated worker",{{"foreground_pid",foregroundPid}});
        throw Error("FOCUS_FAILED","Windows refused to activate the target window");
    }
}
void send(std::vector<INPUT>& events) {
    if(events.empty())return;
    UINT n=SendInput(static_cast<UINT>(events.size()),events.data(),sizeof(INPUT));
    if(n!=events.size())throw Error("INPUT_REJECTED","Windows accepted only part of the input; check application integrity level and desktop state",{{"accepted",n},{"requested",events.size()},{"outcome_unknown",true}});
}
INPUT mouse_event(DWORD flags,LONG x=0,LONG y=0,DWORD data=0) {INPUT i{};i.type=INPUT_MOUSE;i.mi.dx=x;i.mi.dy=y;i.mi.dwFlags=flags;i.mi.mouseData=data;return i;}
void move_mouse(LONG x,LONG y,HWND window) {
    RECT vr=virtual_rect();auto pt=normalize_point(x,y,vr);
    if(window) {RECT r{};check_win(GetWindowRect(window,&r),"GetWindowRect");if(x<r.left || x>=r.right || y<r.top || y>=r.bottom)throw Error("INVALID_COORDINATES","Point is outside the target window");}
    std::vector<INPUT> e{mouse_event(MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK,pt.first,pt.second)};send(e);
}
struct MouseDown {
    DWORD up;
    explicit MouseDown(const std::string& button) {
        DWORD down;
        if(button=="left"){down=MOUSEEVENTF_LEFTDOWN;up=MOUSEEVENTF_LEFTUP;}
        else if(button=="right"){down=MOUSEEVENTF_RIGHTDOWN;up=MOUSEEVENTF_RIGHTUP;}
        else if(button=="middle"){down=MOUSEEVENTF_MIDDLEDOWN;up=MOUSEEVENTF_MIDDLEUP;}
        else throw Error("INVALID_REQUEST","button must be left, right or middle");
        std::vector<INPUT> e{mouse_event(down)};
        try{send(e);}catch(...){INPUT release=mouse_event(up);SendInput(1,&release,sizeof(INPUT));throw;}
    }
    ~MouseDown(){INPUT e=mouse_event(up);SendInput(1,&e,sizeof(INPUT));}
};
WORD key_code(std::wstring name) {
    std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    if(name==L"CTRL" || name==L"CONTROL")return VK_CONTROL;if(name==L"SHIFT")return VK_SHIFT;
    if(name==L"ALT")return VK_MENU;if(name==L"WIN" || name==L"WINDOWS")return VK_LWIN;
    if(name==L"ENTER" || name==L"RETURN")return VK_RETURN;if(name==L"ESC" || name==L"ESCAPE")return VK_ESCAPE;
    if(name==L"TAB")return VK_TAB;if(name==L"SPACE")return VK_SPACE;if(name==L"BACKSPACE")return VK_BACK;
    if(name==L"DELETE" || name==L"DEL")return VK_DELETE;if(name==L"INSERT")return VK_INSERT;
    if(name==L"HOME")return VK_HOME;if(name==L"END")return VK_END;if(name==L"PAGEUP")return VK_PRIOR;if(name==L"PAGEDOWN")return VK_NEXT;
    if(name==L"LEFT")return VK_LEFT;if(name==L"RIGHT")return VK_RIGHT;if(name==L"UP")return VK_UP;if(name==L"DOWN")return VK_DOWN;
    if(name.size()==1 && ((name[0]>=L'A' && name[0]<=L'Z') || (name[0]>=L'0' && name[0]<=L'9')))return static_cast<WORD>(name[0]);
    if(name.size()>=2 && name[0]==L'F') {try{int n=std::stoi(name.substr(1));if(n>=1 && n<=24)return static_cast<WORD>(VK_F1+n-1);}catch(...){} }
    throw Error("INVALID_REQUEST","Unsupported key: "+utf8(name));
}
bool extended(WORD k){return k==VK_LEFT||k==VK_RIGHT||k==VK_UP||k==VK_DOWN||k==VK_HOME||k==VK_END||k==VK_PRIOR||k==VK_NEXT||k==VK_DELETE||k==VK_INSERT||k==VK_LWIN;}
void press_keys(const json& value) {
    std::vector<WORD> keys;
    if(value.is_array()){for(const auto& v:value)keys.push_back(key_code(wide(v.get<std::string>())));}
    else if(value.is_string()) {std::wstring s=wide(value.get<std::string>());size_t pos=0;for(;;){size_t end=s.find(L'+',pos);keys.push_back(key_code(s.substr(pos,end==std::wstring::npos?end:end-pos)));if(end==std::wstring::npos)break;pos=end+1;}}
    else throw Error("INVALID_REQUEST","keys must be a string or an array");
    if(keys.empty() || keys.size()>8)throw Error("INVALID_REQUEST","Specify between 1 and 8 keys");
    struct Release {std::vector<WORD> down;~Release(){for(auto it=down.rbegin();it!=down.rend();++it){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=*it;i.ki.dwFlags=KEYEVENTF_KEYUP|(extended(*it)?KEYEVENTF_EXTENDEDKEY:0);SendInput(1,&i,sizeof(i));}}} release;
    for(WORD key:keys) {if(GetAsyncKeyState(key)&0x8000)throw Error("INPUT_BUSY","A requested key is already held; release it and retry");INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=key;i.ki.dwFlags=extended(key)?KEYEVENTF_EXTENDEDKEY:0;release.down.push_back(key);std::vector<INPUT> e{i};send(e);}
}
void type_text(const std::string& text) {
    auto s=wide(text);if(s.size()>100000)throw Error("INVALID_REQUEST","Text exceeds 100000 UTF-16 code units");
    for(size_t offset=0;offset<s.size();) {
        std::vector<INPUT> e;
        for(size_t end=std::min(s.size(),offset+256);offset<end;++offset) {INPUT a{};a.type=INPUT_KEYBOARD;a.ki.wScan=s[offset];a.ki.dwFlags=KEYEVENTF_UNICODE;INPUT b=a;b.ki.dwFlags|=KEYEVENTF_KEYUP;e.push_back(a);e.push_back(b);}
        send(e);
    }
}
RECT screenshot_rect(const json& p,HWND window) {
    RECT r{};
    if(window){if(IsIconic(window))throw Error("WINDOW_MINIMIZED","Restore the target window before observing it");check_win(GetWindowRect(window,&r),"GetWindowRect");}
    else r={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    if(p.contains("region")){const auto& j=p["region"];r.left=j.at("x").get<LONG>();r.top=j.at("y").get<LONG>();LONG w=j.at("width").get<LONG>(),h=j.at("height").get<LONG>();if(w<=0||h<=0||w>16384||h>16384)throw Error("INVALID_REQUEST","Invalid capture region size");r.right=r.left+w;r.bottom=r.top+h;}
    RECT vr=virtual_rect(),clipped{};if(!IntersectRect(&clipped,&r,&vr))throw Error("INVALID_REQUEST","Capture region is outside the desktop");
    LONG w=clipped.right-clipped.left,h=clipped.bottom-clipped.top;
    if(w<=0||h<=0||static_cast<uint64_t>(w)*h>32*1024*1024)throw Error("IMAGE_TOO_LARGE","Capture region exceeds 32 megapixels");
    return clipped;
}
std::vector<uint8_t> capture(const RECT& r,bool jpeg) {
    LONG w=r.right-r.left,h=r.bottom-r.top;
    HDC screen=GetDC(nullptr);if(!screen)throw Error("CAPTURE_FAILED","GetDC failed");
    HDC memory=CreateCompatibleDC(screen);if(!memory){ReleaseDC(nullptr,screen);throw Error("CAPTURE_FAILED","CreateCompatibleDC failed");}
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap){DeleteDC(memory);ReleaseDC(nullptr,screen);throw Error("CAPTURE_FAILED","CreateDIBSection failed");}
    HGDIOBJ previous=SelectObject(memory,bitmap);
    BOOL copied=BitBlt(memory,0,0,w,h,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);DWORD error=GetLastError();GdiFlush();
    SelectObject(memory,previous);DeleteDC(memory);ReleaseDC(nullptr,screen);
    struct DeleteBitmap {HBITMAP h;~DeleteBitmap(){DeleteObject(h);}} cleanup{bitmap};
    if(!copied)throw Error("CAPTURE_FAILED","BitBlt: "+win_message(error)+". If this is an RDP desktop, keep its client restored; minimized clients can suspend rendering.");
    // Desktop DIB alpha is undefined. PNG must not become fully transparent.
    auto* raw=static_cast<uint8_t*>(pixels);for(size_t i=3;i<static_cast<size_t>(w)*h*4;i+=4)raw[i]=255;
    ComPtr<IWICImagingFactory> factory;check_hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
    ComPtr<IWICBitmap> source;check_hr(factory->CreateBitmapFromMemory(w,h,GUID_WICPixelFormat32bppBGRA,static_cast<UINT>(w*4),static_cast<UINT>(w*h*4),raw,&source),"WIC bitmap");
    ComPtr<IStream> stream;check_hr(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"Image stream");
    ComPtr<IWICBitmapEncoder> encoder;check_hr(factory->CreateEncoder(jpeg?GUID_ContainerFormatJpeg:GUID_ContainerFormatPng,nullptr,&encoder),"Image encoder");
    check_hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Initialize encoder");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> options;check_hr(encoder->CreateNewFrame(&frame,&options),"Encoder frame");
    if(jpeg){PROPBAG2 option{};option.pstrName=const_cast<LPWSTR>(L"ImageQuality");VARIANT v{};v.vt=VT_R4;v.fltVal=0.9f;options->Write(1,&option,&v);}
    check_hr(frame->Initialize(options.Get()),"Initialize frame");check_hr(frame->SetSize(w,h),"Frame size");
    WICPixelFormatGUID format=jpeg?GUID_WICPixelFormat24bppBGR:GUID_WICPixelFormat32bppBGRA;check_hr(frame->SetPixelFormat(&format),"Frame pixel format");
    ComPtr<IWICFormatConverter> converter;check_hr(factory->CreateFormatConverter(&converter),"Format converter");
    check_hr(converter->Initialize(source.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Convert image pixels");
    check_hr(frame->WriteSource(converter.Get(),nullptr),"Encode pixels");check_hr(frame->Commit(),"Commit frame");check_hr(encoder->Commit(),"Commit encoder");
    STATSTG stats{};check_hr(stream->Stat(&stats,STATFLAG_NONAME),"Image stream size");auto n=stats.cbSize.QuadPart;if(n>max_image)throw Error("IMAGE_TOO_LARGE","Encoded image exceeds 32 MiB");
    HGLOBAL global=nullptr;check_hr(GetHGlobalFromStream(stream.Get(),&global),"Image stream memory");
    void* data=GlobalLock(global);if(!data)throw Error("CAPTURE_FAILED","Cannot access encoded image");
    std::vector<uint8_t> out(static_cast<uint8_t*>(data),static_cast<uint8_t*>(data)+static_cast<size_t>(n));GlobalUnlock(global);return out;
}
struct Uia {
    ComPtr<IUIAutomation> api;ComPtr<IUIAutomationTreeWalker> walker;
    Uia(){check_hr(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&api)),"Create UIAutomation","UIA_UNAVAILABLE");check_hr(api->get_ControlViewWalker(&walker),"UIA walker","UIA_UNAVAILABLE");}
    ComPtr<IUIAutomationElement> root(HWND w){ComPtr<IUIAutomationElement> e;check_hr(api->ElementFromHandle(w,&e),"UIA window","UIA_UNAVAILABLE");return e;}
};
json runtime_id(IUIAutomationElement* element) {
    SAFEARRAY* values=nullptr;if(FAILED(element->GetRuntimeId(&values)) || !values)return nullptr;
    json out=json::array();LONG low=0,high=-1;SafeArrayGetLBound(values,1,&low);SafeArrayGetUBound(values,1,&high);
    for(LONG i=low;i<=high && i-low<64;++i){int v=0;if(SUCCEEDED(SafeArrayGetElement(values,&i,&v)))out.push_back(v);}
    SafeArrayDestroy(values);return out;
}
json element_info(IUIAutomationElement* e,int depth) {
    json out={{"depth",depth},{"runtime_id",runtime_id(e)}};BSTR text=nullptr;
    if(SUCCEEDED(e->get_CurrentName(&text)))out["name"]=bstr_text(text,512);
    text=nullptr;if(SUCCEEDED(e->get_CurrentAutomationId(&text)))out["automation_id"]=bstr_text(text,256);
    CONTROLTYPEID type=0;if(SUCCEEDED(e->get_CurrentControlType(&type)))out["control_type"]=type;
    RECT r{};if(SUCCEEDED(e->get_CurrentBoundingRectangle(&r)))out["bounds"]=rect_json(r);
    BOOL enabled=FALSE,offscreen=FALSE,password=FALSE;e->get_CurrentIsEnabled(&enabled);e->get_CurrentIsOffscreen(&offscreen);e->get_CurrentIsPassword(&password);
    out["enabled"]=enabled!=FALSE;out["offscreen"]=offscreen!=FALSE;out["password"]=password!=FALSE;
    if(!password) {ComPtr<IUIAutomationValuePattern> v;if(SUCCEEDED(e->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&v))) && v){text=nullptr;if(SUCCEEDED(v->get_CurrentValue(&text)))out["value"]=bstr_text(text,4000);}}
    return out;
}
template<class Visitor> bool walk_uia(Uia& u,HWND hwnd,int maxNodes,int maxDepth,uint64_t deadline,Visitor visitor) {
    struct Entry{ComPtr<IUIAutomationElement> element;int depth;};std::vector<Entry> stack{{u.root(hwnd),0}};int count=0;bool incomplete=false;
    while(!stack.empty() && count<maxNodes && time_left(deadline)) {
        auto item=std::move(stack.back());stack.pop_back();++count;
        if(!visitor(item.element.Get(),item.depth))return false;
        std::vector<ComPtr<IUIAutomationElement>> children;ComPtr<IUIAutomationElement> child;
        if(FAILED(u.walker->GetFirstChildElement(item.element.Get(),&child))){incomplete=true;continue;}
        if(item.depth>=maxDepth){if(child)incomplete=true;continue;}
        while(child && static_cast<int>(children.size())<maxNodes-count && time_left(deadline)) {
            children.push_back(child);ComPtr<IUIAutomationElement> next;if(FAILED(u.walker->GetNextSiblingElement(child.Get(),&next))){incomplete=true;break;}child=std::move(next);
        }
        if(child)incomplete=true;
        for(auto it=children.rbegin();it!=children.rend();++it)stack.push_back({*it,item.depth+1});
    }
    return incomplete || !stack.empty() || !time_left(deadline);
}
json inspect_uia(HWND window,uint64_t requestDeadline) {
    Uia u;json elements=json::array();uint64_t deadline=std::min<uint64_t>(requestDeadline,deadline_after(3000));
    bool truncated=walk_uia(u,window,200,8,deadline,[&](IUIAutomationElement* e,int depth){elements.push_back(element_info(e,depth));return true;});
    return {{"available",true},{"elements",elements},{"truncated",truncated}};
}
json bounded_inspect_uia(HWND window,uint64_t requestDeadline) {
    // A provider can block inside a single COM call, beyond traversal checks.
    // Keep read-only observation on its own COM thread so the screenshot can be
    // returned on time. This command process exits immediately after its reply.
    DWORD left=time_left(requestDeadline);DWORD budget=std::min<DWORD>(left>500?left-500:0,3000);
    if(!budget)throw Error("UIA_TIMEOUT","No time remains for UIA observation");
    std::packaged_task<json()> task([window,requestDeadline]{ComInit com;return inspect_uia(window,requestDeadline);});
    auto result=task.get_future();std::thread(std::move(task)).detach();
    if(result.wait_for(std::chrono::milliseconds(budget))!=std::future_status::ready)throw Error("UIA_TIMEOUT","UIA provider exceeded observation budget; screenshot remains available");
    return result.get();
}
ComPtr<IUIAutomationElement> find_element(Uia& u,HWND hwnd,const json& selector,uint64_t deadline) {
    if(!selector.is_object() || (!selector.contains("runtime_id") && !selector.contains("automation_id") && !selector.contains("name")))throw Error("INVALID_REQUEST","UIA selector requires runtime_id, automation_id or name");
    ComPtr<IUIAutomationElement> match;int count=0;
    bool truncated=walk_uia(u,hwnd,500,12,deadline,[&](IUIAutomationElement* e,int depth){
        auto info=element_info(e,depth);bool matches=true;
        for(const auto* key:{"runtime_id","automation_id","name","control_type"})if(selector.contains(key) && (!info.contains(key) || selector[key]!=info[key]))matches=false;
        if(matches){match=e;++count;}return count<2;
    });
    if(count!=1 || truncated)throw Error("STALE_TARGET",count>1?"UIA selector matches multiple elements":"UIA target missing or search incomplete; observe again");
    return match;
}
void uia_action(HWND hwnd,const json& action,uint64_t deadline) {
    Uia u;auto e=find_element(u,hwnd,action.at("selector"),deadline);auto type=action.at("type").get<std::string>();
    BOOL enabled=FALSE;check_hr(e->get_CurrentIsEnabled(&enabled),"UIA enabled");if(!enabled)throw Error("STALE_TARGET","UIA element is disabled");
    if(type=="invoke") {ComPtr<IUIAutomationInvokePattern> p;check_hr(e->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&p)),"UIA Invoke pattern","UNSUPPORTED_PATTERN");check_hr(p->Invoke(),"UIA Invoke","UIA_FAILED");}
    else if(type=="set_value") {ComPtr<IUIAutomationValuePattern> p;check_hr(e->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&p)),"UIA Value pattern","UNSUPPORTED_PATTERN");Bstr text(wide(action.at("text").get<std::string>()));check_hr(p->SetValue(text),"UIA SetValue","UIA_FAILED");}
    else if(type=="uia_scroll") {ComPtr<IUIAutomationScrollPattern> p;check_hr(e->GetCurrentPatternAs(UIA_ScrollPatternId,IID_PPV_ARGS(&p)),"UIA Scroll pattern","UNSUPPORTED_PATTERN");int direction=action.value("direction",1);check_hr(p->Scroll(ScrollAmount_NoAmount,direction>0?ScrollAmount_SmallIncrement:ScrollAmount_SmallDecrement),"UIA Scroll","UIA_FAILED");}
}
Reply observe(const json& request,HWND window,uint64_t deadline) {
    const auto& p=request["params"];json result={{"session_id",process_session()},{"desktop",rect_json(virtual_rect())}};
    if(window)result["window"]=window_info(window);
    Reply reply=success(request,result);
    if(p.value("screenshot",true)) {
        RECT rect=screenshot_rect(p,window);std::string format=p.value("format",std::string("png"));
        if(format!="png" && format!="jpeg")throw Error("INVALID_REQUEST","format must be png or jpeg");
        reply.image=capture(rect,format=="jpeg");reply.header["mime_type"]=format=="jpeg"?"image/jpeg":"image/png";
        reply.header["result"]["capture"]={{"bounds",rect_json(rect)},{"width",rect.right-rect.left},{"height",rect.bottom-rect.top},{"scale",1},{"coordinate_space","physical_pixels"}};
    }
    if(window && p.value("uia",true)) {try{reply.header["result"]["uia"]=bounded_inspect_uia(window,deadline);}catch(const std::exception& e){reply.header["result"]["uia"]={{"available",false},{"warning",e.what()}};}}
    return reply;
}
Reply launch(const json& request) {
    const auto& p=request["params"];std::wstring exe=wide(p.at("exe").get<std::string>()),command=quote_arg(exe);
    if(exe.empty())throw Error("INVALID_REQUEST","exe is required");
    if(p.contains("args")) {if(!p["args"].is_array())throw Error("INVALID_REQUEST","args must be an array");for(const auto& a:p["args"])command+=L" "+quote_arg(wide(a.get<std::string>()));}
    std::wstring cwd=p.contains("cwd")?wide(p["cwd"].get<std::string>()):L"";
    std::vector<wchar_t> cmd(command.begin(),command.end());cmd.push_back(0);
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(STARTUPINFOW);si.StartupInfo.lpDesktop=const_cast<LPWSTR>(L"winsta0\\default");PROCESS_INFORMATION pi{};
    BOOL inJob=FALSE;check_win(IsProcessInJob(GetCurrentProcess(),nullptr,&inJob),"IsProcessInJob");
    bool preserveHigh=process_privileges().high();
    DWORD flags=CREATE_NEW_PROCESS_GROUP|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED|(inJob?CREATE_BREAKAWAY_FROM_JOB:0);
    if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,flags,nullptr,cwd.empty()?nullptr:cwd.c_str(),&si.StartupInfo,&pi)) {
        DWORD error=GetLastError();
        if(error==ERROR_ELEVATION_REQUIRED)throw Error("ELEVATION_REQUIRED","Application requires administrator privileges; request params.elevated=true from an elevated SSH process");
        if(!inJob || error!=ERROR_ACCESS_DENIED)throw Error("LAUNCH_FAILED","CreateProcess: "+win_message(error));
        // Task Scheduler may forbid breakaway. Use the same user's desktop shell
        // as parent so the application inherits the shell's job, not this task's.
        DWORD shellPid=0;GetWindowThreadProcessId(GetShellWindow(),&shellPid);DWORD shellSession=MAXDWORD;
        if(!shellPid || !ProcessIdToSessionId(shellPid,&shellSession) || shellSession!=process_session())throw Error("LAUNCH_DETACH_DENIED","No shell process in this desktop session");
        Handle shell(OpenProcess(PROCESS_CREATE_PROCESS|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,shellPid));
        if(!shell.valid())throw Error("LAUNCH_DETACH_DENIED","Cannot use desktop shell as process parent: "+win_message(GetLastError()));
        Handle shellToken;HANDLE rawToken=nullptr;check_win(OpenProcessToken(shell.get(),TOKEN_QUERY,&rawToken),"Shell token");shellToken.reset(rawToken);
        DWORD tokenBytes=0;GetTokenInformation(shellToken.get(),TokenUser,nullptr,0,&tokenBytes);std::vector<uint8_t> token(tokenBytes);
        check_win(GetTokenInformation(shellToken.get(),TokenUser,token.data(),tokenBytes,&tokenBytes),"Shell user");
        LPWSTR sidText=nullptr;check_win(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(token.data())->User.Sid,&sidText),"Shell SID");
        auto shellSid=utf8(sidText);LocalFree(sidText);if(shellSid!=user_sid())throw Error("LAUNCH_DETACH_DENIED","Desktop shell belongs to another account");
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<uint8_t> storage(bytes);
        si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        check_win(InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes),"Process attributes");
        struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST p;~Attributes(){DeleteProcThreadAttributeList(p);}} attributes{si.lpAttributeList};
        HANDLE parent=shell.get();check_win(UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,&parent,sizeof(parent),nullptr,nullptr),"Shell parent attribute");
        si.StartupInfo.cb=sizeof(si);cmd.assign(command.begin(),command.end());cmd.push_back(0);
        DWORD detachedFlags=CREATE_NEW_PROCESS_GROUP|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT;
        BOOL created=FALSE;
        if(preserveHigh) {
            // Explicitly keep the worker token: the shell parent is ordinarily
            // medium integrity, so inheriting its token would lose elevation.
            HANDLE rawSelf=nullptr;check_win(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY,&rawSelf),"Launch token");Handle self(rawSelf);
            created=CreateProcessAsUserW(self.get(),exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,detachedFlags,nullptr,cwd.empty()?nullptr:cwd.c_str(),&si.StartupInfo,&pi);
        } else created=CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,detachedFlags,nullptr,cwd.empty()?nullptr:cwd.c_str(),&si.StartupInfo,&pi);
        if(!created)throw Error("LAUNCH_DETACH_DENIED","Create detached process with shell parent: "+win_message(GetLastError()));
    }
    Handle process(pi.hProcess),thread(pi.hThread);
    struct AbortLaunch {HANDLE process;bool resumed=false;~AbortLaunch(){if(!resumed)TerminateProcess(process,1);}} abort{process.get()};
    auto child=process_privileges(pi.dwProcessId);DWORD childSession=MAXDWORD;
    check_win(ProcessIdToSessionId(pi.dwProcessId,&childSession),"Launched process session");
    if(childSession!=process_session())throw Error("LAUNCH_FAILED","Detached application started in the wrong desktop session");
    if(preserveHigh && !child.high())throw Error("ELEVATION_FAILED","Detached application lost elevation; it was stopped before execution");
    if(ResumeThread(thread.get())==static_cast<DWORD>(-1))check_win(FALSE,"Resume launched program");
    abort.resumed=true;
    return success(request,{{"pid",pi.dwProcessId},{"process_start",process_start(pi.dwProcessId)},{"exe",utf8(exe)},{"session_id",childSession},{"detached",true},{"privileges",child.describe()}});
}
Reply act(const json& request,HWND window,uint64_t deadline) {
    const auto& p=request["params"];
    if(!p.contains("actions") || !p["actions"].is_array() || p["actions"].empty() || p["actions"].size()>50)throw Error("INVALID_REQUEST","actions must contain between 1 and 50 operations");
    json completed=json::array();size_t index=0;
    try {
        for(const auto& a:p["actions"]) {
            if(!time_left(deadline))throw Error("TIMEOUT","Action batch timed out");require_input_desktop();target_window(p,true);
            auto type=a.at("type").get<std::string>();
            if(type=="invoke" || type=="set_value" || type=="uia_scroll")uia_action(window,a,deadline);
            else if(type=="wait") {DWORD ms=a.value("ms",100);if(ms>10000 || ms>time_left(deadline))throw Error("INVALID_REQUEST","Wait exceeds request budget or 10000 ms");Sleep(ms);}
            else {
                activate(window);
                if(type=="focus") {}
                else if(type=="click" || type=="double_click") {LONG x=a.at("x").get<LONG>(),y=a.at("y").get<LONG>();move_mouse(x,y,window);for(int count=0;count<(type=="double_click"?2:1);++count){ {MouseDown down(a.value("button",std::string("left")));Sleep(20);}if(type=="double_click")Sleep(60);}}
                else if(type=="type")type_text(a.at("text").get<std::string>());
                else if(type=="key")press_keys(a.at("keys"));
                else if(type=="scroll") {if(a.contains("x"))move_mouse(a.at("x").get<LONG>(),a.at("y").get<LONG>(),window);else{RECT r{};GetWindowRect(window,&r);move_mouse((r.left+r.right)/2,(r.top+r.bottom)/2,window);}int delta=a.value("delta",-120);if(delta<-12000 || delta>12000)throw Error("INVALID_REQUEST","Invalid scroll delta");std::vector<INPUT> e{mouse_event(MOUSEEVENTF_WHEEL,0,0,static_cast<DWORD>(delta))};send(e);}
                else if(type=="drag") {LONG x=a.at("x").get<LONG>(),y=a.at("y").get<LONG>(),tx=a.at("to_x").get<LONG>(),ty=a.at("to_y").get<LONG>();DWORD ms=a.value("duration_ms",300);if(ms>2000 || ms>time_left(deadline))throw Error("INVALID_REQUEST","Invalid drag duration");move_mouse(x,y,window);MouseDown down("left");for(int step=1;step<=20;++step){move_mouse(x+(tx-x)*step/20,y+(ty-y)*step/20,window);Sleep(ms/20);}}
                else throw Error("INVALID_REQUEST","Unknown action: "+type);
            }
            completed.push_back({{"index",index},{"type",type}});++index;
        }
    } catch(const Error& e) {auto out=failure(request,e);out.header["result"]={{"completed",completed},{"failed_index",index},{"outcome_unknown",e.details.value("outcome_unknown",false)}};return out;}
    catch(const std::exception& e) {auto out=failure(request,Error("INVALID_REQUEST",e.what()));out.header["result"]={{"completed",completed},{"failed_index",index}};return out;}
    Reply result;
    try {if(p.value("observe",false))result=observe(request,window,deadline);else result=success(request,{{"window",window_info(window)}});}
    catch(const Error& e){result=failure(request,e);result.header["result"]={{"actions_completed",true},{"observation_failed",true}};}
    result.header["result"]["completed"]=completed;return result;
}
}
Reply observe_input_desktop(const json& request) {
    return observe(request,GetDesktopWindow(),deadline_after(request["timeout_ms"].get<DWORD>()));
}
Reply execute_desktop(const json& request) {
    auto privileges=process_privileges();Reply reply;
    try {
        if(request["params"].value("elevated",false) && !privileges.high())throw Error("ELEVATION_FAILED","Desktop worker is not elevated; no actions were performed");
        auto timeout=request["timeout_ms"].get<DWORD>();auto deadline=deadline_after(timeout);DesktopLock lock(timeout);require_input_desktop();
        auto op=request.at("op").get<std::string>();const auto& p=request["params"];
        if(op=="windows")reply=success(request,{{"windows",list_windows()},{"session_id",process_session()}});
        else if(op=="launch")reply=launch(request);
        else {
            HWND window=target_window(p,op=="act");
            if(op=="observe")reply=observe(request,window,deadline);
            else if(op=="act")reply=act(request,window,deadline);
            else throw Error("UNKNOWN_OPERATION","Unknown desktop operation");
        }
    } catch(const Error& e){reply=failure(request,e);}
    catch(const std::exception& e){reply=failure(request,Error("INVALID_REQUEST",e.what()));}
    reply.header["worker"]=privileges.describe();reply.header["worker"]["session_id"]=process_session();reply.header["worker"]["pid"]=GetCurrentProcessId();
    return reply;
}
}
