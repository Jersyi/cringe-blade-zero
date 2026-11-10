#pragma once
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace NetPuppetRules {
constexpr uint16_t None = 0xffff;
constexpr size_t MaxParts = 512, MaxBones = 1500;
// A mask handles combined brow/lash cards without showing a second set of brows.
inline unsigned HairMask(std::string path, bool groom=false) {
    path=path.substr(path.find_last_of('/')+1);
    for(char& c:path)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if(!groom&&(path.find("band")!=std::string::npos||path.find("hat")!=std::string::npos||path.find("head")!=std::string::npos))return 0;
    unsigned mask=0;
    if(path.find("brow")!=std::string::npos)mask|=2;
    if(path.find("lash")!=std::string::npos)mask|=4;
    if(path.find("beard")!=std::string::npos||path.find("bread")!=std::string::npos||path.find("mustache")!=std::string::npos)mask|=8;
    if(!mask&&(groom||path.find("hair")!=std::string::npos||path.find("bangs")!=std::string::npos))mask=1;
    return mask;
}
struct HairChoice { unsigned mask=0; bool groom=false, available=false; };
inline std::vector<bool> ChooseHair(const std::vector<HairChoice>& parts,bool prefer_groom) {
    unsigned preferred=0,fallback=0;
    for(const auto& p:parts)if(p.available&&p.groom==prefer_groom)preferred|=p.mask;
    for(const auto& p:parts)if(p.available&&p.groom!=prefer_groom&&(p.mask&~preferred))fallback|=p.mask;
    std::vector<bool> result;for(const auto& p:parts)result.push_back(p.available&&p.mask&&
        (p.groom==prefer_groom?!(p.mask&fallback):!!(p.mask&~preferred)));
    return result;
}
inline bool AssetPath(const std::string& path) {
    return path.size() >= 4 && path.size() <= 2048 && path[0] == '/' &&
        path.find("..") == std::string::npos && path.find('\\') == std::string::npos &&
        path.find('\0') == std::string::npos && path.find('.') != std::string::npos &&
        path.rfind("/Engine/Transient", 0) != 0 && path.rfind("/Script/", 0) != 0;
}
struct Description { uint16_t id, parent, leader, bones; uint8_t kind, posed; uint16_t spline=None; uint8_t render_static=0; };
inline bool Descriptions(const std::vector<Description>& parts) {
    if (parts.size() > MaxParts) return false;
    std::vector<uint16_t> ids;
    for (const auto& p : parts) {
        if (!p.id || p.id == None || p.kind > 6 || p.posed > 1 || p.bones > MaxBones ||
            (p.posed && p.kind != 1) || (!p.posed && p.bones) || p.render_static>1 || (p.render_static&&p.kind!=1)) return false;
        const auto seen = [&](uint16_t id) { for (uint16_t v : ids) if (v == id) return true; return false; };
        if (seen(p.id) || (p.parent != None && !seen(p.parent)) || (p.leader != None && !seen(p.leader))) return false;
        if(p.spline!=None){
            if(p.kind!=4||!seen(p.spline))return false;
            bool spline=false;for(const auto& q:parts)if(q.id==p.spline)spline=q.kind==6;
            if(!spline)return false;
        }
        ids.push_back(p.id);
    }
    return true;
}
// Serial numbers are compared modulo 2^32. A mismatched parts generation must never write a pose into reused IDs.
inline bool AcceptPose(uint32_t parts, uint32_t current_parts, bool have_parts, uint32_t seq, uint32_t last, bool have_pose) {
    return have_parts && parts == current_parts && (!have_pose || static_cast<int32_t>(seq - last) > 0);
}
inline bool ProjectileVisible(bool owned, uint8_t status, bool hidden, bool destroying) {
    return owned && status==2 && !hidden && !destroying;
}
struct Point { double x=0, y=0, z=0; };
inline double DistanceToSegment(Point p, Point a, Point b) {
    const Point d{b.x-a.x,b.y-a.y,b.z-a.z};
    const double n=d.x*d.x+d.y*d.y+d.z*d.z;
    double t=n>1e-9?((p.x-a.x)*d.x+(p.y-a.y)*d.y+(p.z-a.z)*d.z)/n:0;
    if(t<0)t=0;if(t>1)t=1;
    return std::hypot(std::hypot(p.x-a.x-t*d.x,p.y-a.y-t*d.y),p.z-a.z-t*d.z);
}
// Local duel-lab collision only; it never writes the game's HP or accepts remote damage claims.
inline bool SweepHit(Point previous, Point current, Point target, double radius) {
    const double move=std::hypot(std::hypot(current.x-previous.x,current.y-previous.y),current.z-previous.z);
    return std::isfinite(move) && std::isfinite(radius) && radius>0 && move<500 &&
        DistanceToSegment(target,previous,current)<=radius;
}
// Deliberately separate from game HP: one accepted contact per attack and target.
struct Duel {
    int health[2]{1000,1000}; uint32_t hits[2]{}, attacks=0;
    bool touched[2]{};
    void BeginAttack(){++attacks;touched[0]=touched[1]=false;}
    bool Hit(int target){
        if(target<0||target>1||!attacks||touched[target]||health[0]<=0||health[1]<=0)return false;
        touched[target]=true;health[target]-=100;++hits[target];return true;
    }
};

}
