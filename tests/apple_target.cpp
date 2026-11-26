#include "../src/cli/AppleTarget.hpp"
#include <iostream>
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="version") {
            std::cout<<bolt::apple_target::version_text(bolt::apple_target::version(argv[2]));return 0;
        }
        if(argc==4&&std::string(argv[1])=="sdk") {
            std::cout<<bolt::apple_target::sdk_version(argv[2],argv[3]);return 0;
        }
        if(argc!=4&&argc!=5)return 3;
        auto path=bolt::path_from_utf8(argv[2]);
        if(argc==5)bolt::apple_target::require_compatible(path,argv[4],std::string(argv[3])=="arm64");
        std::cout<<bolt::apple_target::version_text(bolt::apple_target::runtime_minimum(path,std::string(argv[3])=="arm64"));return 0;
    } catch(const std::exception& e){std::cerr<<e.what();return 2;}
}
