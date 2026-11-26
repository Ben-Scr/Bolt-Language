#pragma once
// Read only Mach-O headers/load commands, not a whole runtime image. This is
// independent of Apple SDK headers so the compatibility check can be tested on
// other hosts. Fat binaries are checked for the architecture being linked.
#include "../tooling/Json.hpp"
#include <bolt/Path.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bolt::apple_target {
inline std::uint32_t version(std::string_view text) {
    std::uint32_t pieces[3]{};
    std::size_t start=0;
    int count=0;
    while(start<text.size() && count<3) {
        const auto end=text.find('.',start);
        const auto token=text.substr(start,end==text.npos?text.size()-start:end-start);
        if(token.empty())throw std::runtime_error("Invalid macOS deployment version");
        std::uint32_t value=0;
        for(char c:token) {
            if(c<'0'||c>'9'||value>6553)throw std::runtime_error("Invalid macOS deployment version");
            value=value*10+static_cast<unsigned>(c-'0');
        }
        if(value>(count==0?65535u:255u))throw std::runtime_error("macOS deployment version component is out of range");
        pieces[count++]=value;
        if(end==text.npos){start=text.size();break;}
        start=end+1;
        if(start==text.size())throw std::runtime_error("Invalid macOS deployment version");
    }
    if(start!=text.size()||count<2)throw std::runtime_error("macOS deployment version must be major.minor[.patch]");
    return (pieces[0]<<16)|(pieces[1]<<8)|pieces[2];
}
inline std::string version_text(std::uint32_t value) {
    return std::to_string(value>>16)+"."+std::to_string((value>>8)&255)+"."+std::to_string(value&255);
}
inline std::uint32_t u32(const std::uint8_t* p,bool little) {
    std::uint32_t value=0;
    for(int i=0;i<4;++i)value=(value<<8)|p[little?3-i:i];
    return value;
}
inline std::uint64_t u64(const std::uint8_t* p,bool little) {
    return (std::uint64_t(u32(p+(little?4:0),little))<<32)|u32(p+(little?0:4),little);
}
class HeaderReader {
    std::ifstream input;
    std::uint64_t bytes;
public:
    explicit HeaderReader(const std::filesystem::path& path):input(path,std::ios::binary),bytes(std::filesystem::file_size(path)) {
        if(!input)throw std::runtime_error("Cannot read macOS runtime header: "+bolt::path_to_utf8(path));
    }
    std::uint64_t size()const{return bytes;}
    std::vector<std::uint8_t> read(std::uint64_t offset,std::uint64_t length) {
        if(offset>bytes||length>bytes-offset||length>16*1024*1024)
            throw std::runtime_error("Truncated or oversized Mach-O runtime header");
        std::vector<std::uint8_t> out(static_cast<std::size_t>(length));
        input.seekg(static_cast<std::streamoff>(offset));
        input.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(length));
        if(!input)throw std::runtime_error("Cannot read Mach-O runtime load commands");
        return out;
    }
};
inline std::uint32_t runtime_minimum(const std::filesystem::path& path,bool arm64) {
    HeaderReader reader(path);
    const auto wanted=arm64?0x0100000cu:0x01000007u;
    std::uint64_t offset=0,extent=reader.size();
    auto head=reader.read(0,8);
    const auto fat_magic=u32(head.data(),false);
    const bool fat=fat_magic==0xcafebabeu||fat_magic==0xcafebabfu||fat_magic==0xbebafecau||fat_magic==0xbfbafecau;
    if(fat) {
        const bool little=fat_magic==0xbebafecau||fat_magic==0xbfbafecau;
        const bool wide=fat_magic==0xcafebabfu||fat_magic==0xbfbafecau;
        const auto count=u32(head.data()+4,little);
        if(count==0||count>64)throw std::runtime_error("Invalid Mach-O universal runtime architecture table");
        const auto stride=wide?32u:20u;
        const auto table=reader.read(8,std::uint64_t(count)*stride);
        bool found=false;
        for(std::uint32_t i=0;i<count;++i){const auto* p=table.data()+i*stride;
            if(u32(p,little)!=wanted)continue;
            offset=wide?u64(p+8,little):u32(p+8,little);
            extent=wide?u64(p+16,little):u32(p+12,little);
            if(offset<8+std::uint64_t(count)*stride||offset>reader.size()||extent>reader.size()-offset)
                throw std::runtime_error("Invalid Mach-O universal runtime slice");
            found=true;break;
        }
        if(!found)throw std::runtime_error("BoltRuntime has no slice for the selected macOS architecture");
    }
    if(extent<32)throw std::runtime_error("Truncated Mach-O 64-bit runtime header");
    head=reader.read(offset,32);
    const auto magic=u32(head.data(),true);
    if(magic!=0xfeedfacfu&&magic!=0xcffaedfeu)throw std::runtime_error("BoltRuntime is not a 64-bit Mach-O image");
    const bool little=magic==0xfeedfacfu;
    if(u32(head.data()+4,little)!=wanted)throw std::runtime_error("BoltRuntime macOS CPU architecture does not match the compiler");
    const auto count=u32(head.data()+16,little),length=u32(head.data()+20,little);
    if(count==0||count>length/8||length>extent-32)throw std::runtime_error("Invalid Mach-O runtime load command table");
    const auto commands=reader.read(offset+32,length);
    std::size_t at=0;
    std::optional<std::uint32_t> minimum;
    for(std::uint32_t i=0;i<count;++i) {
        if(commands.size()-at<8)throw std::runtime_error("Truncated Mach-O load command");
        const auto cmd=u32(commands.data()+at,little),size=u32(commands.data()+at+4,little);
        if(size<8||size%4!=0||size>commands.size()-at)throw std::runtime_error("Invalid Mach-O load command length");
        std::optional<std::uint32_t> current;
        if(cmd==0x32u){
            if(size<24)throw std::runtime_error("Truncated LC_BUILD_VERSION");
            if(u32(commands.data()+at+8,little)==1)current=u32(commands.data()+at+12,little);
        } else if(cmd==0x24u){
            if(size<16)throw std::runtime_error("Truncated LC_VERSION_MIN_MACOSX");
            current=u32(commands.data()+at+8,little);
        }
        if(current)minimum=std::max(minimum.value_or(0),*current);
        at+=size;
    }
    if(!minimum)throw std::runtime_error("Cannot determine the macOS deployment target of BoltRuntime; rebuild/install the matching runtime");
    return *minimum;
}
inline void require_compatible(const std::filesystem::path& runtime,std::string_view target,bool arm64) {
    const auto required=runtime_minimum(runtime,arm64);
    if(required>version(target))throw std::runtime_error(
        "BoltRuntime requires macOS "+version_text(required)+", but this compiler targets macOS "+std::string(target)+
        ". Rebuild and install the compiler and runtime together with the same CMAKE_OSX_DEPLOYMENT_TARGET; do not mix runtime dylibs.");
}
inline std::string sdk_version(const std::filesystem::path& sdk,std::string_view fallback) {
    // The SDK and minimum OS are different fields in -platform_version. Prefer
    // the selected SDK's own metadata; older SDKs lacking JSON keep the minimum.
    const auto metadata=sdk/"SDKSettings.json";
    std::error_code ec;
    if(std::filesystem::is_regular_file(metadata,ec)&&std::filesystem::file_size(metadata,ec)<=1024*1024&&!ec) {
        try {
            std::ifstream in(metadata,std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),{});
            const auto settings=bolt::tooling::Json::parse(text);
            const auto result=settings["Version"].string(settings["MinimalDisplayName"].string());
            (void)version(result);
            return result;
        }catch(const std::exception&){/* deterministic fallback for older SDK metadata */}
    }
    return std::string(fallback);
}
}
