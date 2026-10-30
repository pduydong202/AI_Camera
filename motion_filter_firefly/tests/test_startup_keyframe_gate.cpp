#include "startup_keyframe_gate.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

void check(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
int main() {
    try {
        StartupKeyframeGate avc;
        const std::vector<std::uint8_t> headers{0,0,0,1,0x67,0x42,0,0,1,0x68,1};
        const std::vector<std::uint8_t> delta{0,0,1,0x41,1,2,3};
        const std::vector<std::uint8_t> idr{0,0,0,1,0x67,1,0,0,1,0x65,7};
        check(!avc.accept(headers.data(),headers.size(),false),"header-only startup accepted");
        check(!avc.accept(delta.data(),delta.size(),false),"leading P-frame accepted");
        check(avc.accept(idr.data(),idr.size(),false),"H264 IDR rejected");
        check(avc.dropped()==2,"startup drops not counted");
        check(avc.accept(delta.data(),delta.size(),false),"gate filtered after startup");
        check(avc.accept(nullptr,0,false),"one-shot gate re-inspected later input");
        StartupKeyframeGate hevc;
        const std::vector<std::uint8_t> vps{0,0,0,1,64,1,9};
        const std::vector<std::uint8_t> trail{0,0,1,2,1,9};
        const std::vector<std::uint8_t> cra{0,0,0,1,42,1,9};
        check(!hevc.accept(vps.data(),vps.size(),true),"HEVC VPS accepted as picture");
        check(!hevc.accept(trail.data(),trail.size(),true),"leading HEVC TRAIL accepted");
        check(hevc.accept(cra.data(),cra.size(),true),"HEVC CRA rejected");
        for (unsigned type=16;type<=21;++type) {
            StartupKeyframeGate gate;
            const std::vector<std::uint8_t> unit{0,0,1,static_cast<std::uint8_t>(type<<1),1,9};
            check(gate.accept(unit.data(),unit.size(),true),"HEVC random access type rejected");
        }
        for (const auto& bytes : std::vector<std::vector<std::uint8_t>>{
                {},{0},{0,0,1},{0,0,0,1},{0,0,1,0x85},{0,0,1,38},{0,0,1,38,0}}) {
            StartupKeyframeGate gate;
            check(!gate.accept(bytes.data(),bytes.size(),true),"malformed HEVC unit accepted");
        }
        std::cout << "PASS: startup waits for AVC/HEVC random access, skips headers, then preserves every AU\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
