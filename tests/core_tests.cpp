#include "common.hpp"
#include <iostream>
using namespace ra;
static void expect(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main() {
    try {
        expect(wide(utf8(L"中文路径 ' \" 测试"))==L"中文路径 ' \" 测试","UTF-8 roundtrip");
        auto r=normalize_request({{"protocol",1},{"id","test"},{"op","status"}});
        expect(r["timeout_ms"]==30000 && r["params"].is_object(),"request defaults");
        expect(normalize_request({{"op","login"}})["timeout_ms"]==120000,"login allows profile initialization by default");
        expect(r["params"]["elevated"]==false,"ordinary worker by default");
        expect(normalize_request({{"op","windows"},{"params",{{"elevated",true}}}})["params"]["elevated"]==true,"explicit elevated worker request");
        bool badElevation=false;try{normalize_request({{"op","windows"},{"params",{{"elevated","true"}}}});}catch(const Error& e){badElevation=e.code=="INVALID_REQUEST";}expect(badElevation,"elevation must be an actual boolean");
        bool rejected=false;try { normalize_request({{"protocol",2},{"op","status"}}); }catch(const Error& e){ rejected=e.code=="PROTOCOL_MISMATCH"; }expect(rejected,"protocol rejection");
        rejected=false;try { normalize_request({{"protocol",1},{"op","act"},{"timeout_ms",-1}}); }catch(const Error& e){ rejected=e.code=="INVALID_REQUEST"; }expect(rejected,"invalid timeout");
        RECT desktop{-1920,0,1920,1080}; auto a=normalize_point(-1920,0,desktop),b=normalize_point(1919,1079,desktop);
        expect(a.first>=0 && a.first<100 && b.first>65000 && b.first<=65535,"negative-origin coordinates");
        rejected=false;try{normalize_point(1920,0,desktop);}catch(const Error&){rejected=true;}expect(rejected,"out of bounds");
        expect(quote_arg(L"C:\\test folder\\")==L"\"C:\\test folder\\\\\"","Windows trailing slash quoting");
        auto e=failure(r,Error("TEST","test"));expect(!e.header["ok"].get<bool>() && e.header["image_bytes"]==0,"error envelope");
        std::cout<<"core tests passed\n";return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
}
