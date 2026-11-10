#pragma once
#include "bone_motion_model.h"
#include <limits>
namespace BoneDebug {
struct Point {double x=0,y=0;bool shown=false;};
inline std::vector<Point> Project(const std::vector<std::string>& names,const std::vector<std::array<double,3>>& positions,
                                  double yaw,int width,int height,double pad=24){
    std::vector<Point> result(names.size());if(positions.size()!=names.size()||width<=2*pad||height<=2*pad)return result;
    double xmin=1e30,xmax=-1e30,ymin=1e30,ymax=-1e30;const double c=std::cos(yaw),s=std::sin(yaw);
    for(size_t i=0;i<names.size();++i){const auto& p=positions[i];if(BoneMotion::Facial(names[i])||!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2]))continue;
        auto& q=result[i];q={p[1]*c-p[0]*s,-p[2],true};xmin=(std::min)(xmin,q.x);xmax=(std::max)(xmax,q.x);ymin=(std::min)(ymin,q.y);ymax=(std::max)(ymax,q.y);}
    if(xmin>xmax)return result;const double scale=(std::min)((width-2*pad)/(std::max)(1.0,xmax-xmin),(height-2*pad)/(std::max)(1.0,ymax-ymin));
    for(auto& q:result)if(q.shown){q.x=width*0.5+(q.x-(xmin+xmax)*0.5)*scale;q.y=height*0.5+(q.y-(ymin+ymax)*0.5)*scale;}return result;
}
}
