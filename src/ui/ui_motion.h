#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace UiMotion {
inline double Unit(double value) { return std::clamp(value,0.0,1.0); }
inline double EaseOutCubic(double value) { const double t=1.0-Unit(value); return 1.0-t*t*t; }
inline double EaseInCubic(double value) { const double t=Unit(value); return t*t*t; }
inline double EaseInOutCubic(double value) { const double t=Unit(value); return t<0.5?4.0*t*t*t:1.0-std::pow(-2.0*t+2.0,3.0)/2.0; }
inline double EaseOutExpo(double value) { const double t=Unit(value); return t>=1.0?1.0:1.0-std::pow(2.0,-10.0*t); }
inline double EaseOutQuint(double value) { const double t=1.0-Unit(value); return 1.0-t*t*t*t*t; }
inline bool ReducedMotion() {
    BOOL animated=TRUE; HIGHCONTRASTW contrast{sizeof(contrast),0,nullptr};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0)&&(contrast.dwFlags&HCF_HIGHCONTRASTON)) return true;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animated,0)&&!animated;
}
// A value that eases from where it currently is towards a new target.
struct Tween {
    double from=0,to=0; ULONGLONG start=0; DWORD duration=0; double (*ease)(double)=EaseOutCubic;
    double At(ULONGLONG now) const {
        if (!duration||now>=start+duration) return to;
        return from+(to-from)*ease(static_cast<double>(now-start)/duration);
    }
    bool Running(ULONGLONG now) const { return duration&&now<start+duration; }
    void Set(double target,DWORD milliseconds,ULONGLONG now,bool reduced) {
        if (target==to&&Running(now)) return;
        from=At(now); to=target; start=now; duration=reduced?0:milliseconds;
    }
    void Jump(double value) { from=to=value; duration=0; }
};
}
