#include "combo_file.h"
#include "npc_combo_model.h"
#include <filesystem>
#include <fstream>
#include <iterator>

int wmain(int argc,wchar_t** argv){
    if(argc!=2){std::printf("usage: sample_combo_test <sample.combo>\n");return 2;}
    std::ifstream file(std::filesystem::path(argv[1]),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    ComboFile::Set profiles;std::string why;
    if(!ComboFile::Unpack(bytes,profiles,why)||profiles.empty()){std::printf("invalid sample: %s\n",why.c_str());return 1;}
    size_t nodes=0;bool first=true;
    std::printf("{\"profiles\":[");
    for(const auto& [name,text]:profiles){
        const auto config=NpcCombo::Decode(text);
        if(config.chain.empty()){std::printf("empty profile %s\n",name.c_str());return 1;}
        const auto roundtrip=NpcCombo::Decode(NpcCombo::Encode(config));
        if(NpcCombo::Encode(config)!=NpcCombo::Encode(roundtrip))return 1;
        std::printf("%s{\"id\":\"%s\",\"enabled\":%s,\"nodes\":%zu}",first?"":",",name.c_str(),config.enabled?"true":"false",config.chain.size());
        first=false;nodes+=config.chain.size();
    }
    std::printf("],\"total_nodes\":%zu,\"crc_valid\":true,\"roundtrip_valid\":true,\"runtime\":false}\n",nodes);
    return 0;
}
