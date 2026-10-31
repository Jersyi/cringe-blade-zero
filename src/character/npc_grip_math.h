#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Pure FTransform math for weapon attachments. No UObject, allocation or engine
// calls: the socket getter can also be queried by native animation workers.
namespace NpcGripMath {
inline void Mul(const double* a, const double* b, double* out) {
    const double q[4] = {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    for (size_t i=0;i<4;++i) out[i]=q[i];
}
inline void Inverse(const double* q, double* out) {
    const double norm=q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
    for (size_t i=0;i<4;++i) out[i]=(i==3?q[i]:-q[i])/norm;
}
inline void Rotate(const double* q, const double* v, double* out) {
    double iq[4],p[4]={v[0],v[1],v[2],0},t[4],result[4];
    Inverse(q,iq); Mul(q,p,t); Mul(t,iq,result);
    for (size_t i=0;i<3;++i) out[i]=result[i];
}
template<class T> bool Valid(const T& t) {
    double length=0;
    for (size_t i=0;i<4;++i) { if (!std::isfinite(t.rotation[i])) return false; length+=t.rotation[i]*t.rotation[i]; }
    if (std::abs(length-1)>0.001) return false;
    for (size_t i=0;i<3;++i)
        if (!std::isfinite(t.translation[i]) || !std::isfinite(t.scale[i]) || t.scale[i]<1e-6 || t.scale[i]>100) return false;
    return true;
}
template<class T> bool FiniteMount(const T& t) {
    double n=0;for(size_t i=0;i<4;++i){if(!std::isfinite(t.rotation[i]))return false;n+=t.rotation[i]*t.rotation[i];}if(std::abs(n-1)>0.001)return false;
    for(size_t i=0;i<3;++i)if(!std::isfinite(t.translation[i])||!std::isfinite(t.scale[i]))return false;return true;
}
template<class T> bool RelativeMount(const T& value,const T& parent,T& result) {
    if(!FiniteMount(value)||!Valid(parent))return false;T out;double qi[4],delta[3],rotated[3];Inverse(parent.rotation,qi);Mul(qi,value.rotation,out.rotation);
    for(size_t i=0;i<3;++i)delta[i]=value.translation[i]-parent.translation[i];Rotate(qi,delta,rotated);
    for(size_t i=0;i<3;++i){out.translation[i]=rotated[i]/parent.scale[i];out.scale[i]=value.scale[i]/parent.scale[i];}result=out;return FiniteMount(result);
}
template<class T> bool RelativeNativeSafe(const T& value,const T& parent,T& result) {
    if(!FiniteMount(value)||!FiniteMount(parent))return false;for(size_t i=0;i<3;++i)if(value.scale[i]<0||parent.scale[i]<0)return false;T out;double qi[4],delta[3],rotated[3];Inverse(parent.rotation,qi);Mul(qi,value.rotation,out.rotation);
    for(size_t i=0;i<3;++i)delta[i]=value.translation[i]-parent.translation[i];Rotate(qi,delta,rotated);
    for(size_t i=0;i<3;++i){const double reciprocal=std::abs(parent.scale[i])<=1e-8?0:1/parent.scale[i];out.translation[i]=rotated[i]*reciprocal;out.scale[i]=value.scale[i]*reciprocal;}result=out;return FiniteMount(result);
}
template<class T> bool ComposeMount(const T& local,const T& parent,T& result) {
    if(!FiniteMount(local)||!FiniteMount(parent))return false;T out;double t[3],rotated[3];Mul(parent.rotation,local.rotation,out.rotation);
    for(size_t i=0;i<3;++i)t[i]=local.translation[i]*parent.scale[i];Rotate(parent.rotation,t,rotated);
    for(size_t i=0;i<3;++i){out.translation[i]=parent.translation[i]+rotated[i];out.scale[i]=local.scale[i]*parent.scale[i];}result=out;return FiniteMount(result);
}
template<class T> bool Compose(const T& local, const T& parent, T& result) {
    if (!Valid(local)||!Valid(parent)) return false;
    T out; Mul(parent.rotation,local.rotation,out.rotation);
    double scaled[3],rotated[3];
    for (size_t i=0;i<3;++i) scaled[i]=local.translation[i]*parent.scale[i];
    Rotate(parent.rotation,scaled,rotated);
    for (size_t i=0;i<3;++i) { out.translation[i]=parent.translation[i]+rotated[i]; out.scale[i]=local.scale[i]*parent.scale[i]; }
    result=out; return Valid(result);
}
template<class T> bool Relative(const T& value, const T& parent, T& result) {
    if (!Valid(value)||!Valid(parent)) return false;
    T out; double iq[4],delta[3],rotated[3]; Inverse(parent.rotation,iq); Mul(iq,value.rotation,out.rotation);
    for (size_t i=0;i<3;++i) delta[i]=value.translation[i]-parent.translation[i];
    Rotate(iq,delta,rotated);
    for (size_t i=0;i<3;++i) { out.translation[i]=rotated[i]/parent.scale[i]; out.scale[i]=value.scale[i]/parent.scale[i]; }
    result=out; return Valid(result);
}
template<class T> bool RecoverSource(const T& target, const T& source_ref, const T& target_ref, const double* q0, T& source) {
    if (!Valid(target)||!Valid(source_ref)||!Valid(target_ref)) return false;
    // Exact PBZ native path: compatible-skeleton mapping, then AnimationRelative:
    // Rout=(Q0*Rin*Q1skel)*inverse(Q0*RauthRef*Q1skel)*RbodyRef.
    // Expanding the inverse cancels Q1skel. Its effective right factor is
    // inverse(RauthRef)*inverse(Q0)*RbodyRef. target_ref MUST be the BodyMesh
    // reference, while q0 still comes from the two Skeleton reference frames.
    // T/S similarly use bodyRef - Q0(authRef), and bodyRef.S/authRef.S.
    double qi[4],si[4],q1[4],q1i[4],tmp[4]; Inverse(q0,qi); Inverse(source_ref.rotation,si);
    Mul(si,qi,tmp); Mul(tmp,target_ref.rotation,q1); Inverse(q1,q1i);
    T out; Mul(qi,target.rotation,tmp); Mul(tmp,q1i,out.rotation);
    double mapped_source[3],delta[3]; Rotate(q0,source_ref.translation,mapped_source);
    for (size_t i=0;i<3;++i) delta[i]=target.translation[i]-target_ref.translation[i]+mapped_source[i];
    Rotate(qi,delta,out.translation);
    for (size_t i=0;i<3;++i) out.scale[i]=target.scale[i]*source_ref.scale[i]/target_ref.scale[i];
    source=out; return Valid(source);
}
template<class T> bool MountAtPalm(const T& source_mount, const T& source_palm_hand, const T& target_palm_hand, const double* hand_basis, T& target_mount) {
    if (!FiniteMount(source_mount)||!Valid(source_palm_hand)||!Valid(target_palm_hand)) return false;
    T out; Mul(hand_basis,source_mount.rotation,out.rotation);
    double offset[3],rotated[3];
    for (size_t i=0;i<3;++i) offset[i]=source_mount.translation[i]-source_palm_hand.translation[i];
    Rotate(hand_basis,offset,rotated);
    // Preserve the rigid weapon's dimensions and original distance from its
    // grip point. Move that grip point to the NPC's current knuckle/palm anchor;
    // do not scale an entire sword or reuse Hun's longer hand proportions.
    for (size_t i=0;i<3;++i) { out.translation[i]=target_palm_hand.translation[i]+rotated[i];out.scale[i]=source_mount.scale[i]; }
    target_mount=out;return FiniteMount(target_mount);
}
inline bool GeometryFrame(const double* origin,const double* longitudinal,const double* lateralA,const double* lateralB,double frame[3][3]) {
    double forward[3],across[3],n=0;for(size_t i=0;i<3;++i){forward[i]=longitudinal[i]-origin[i];across[i]=lateralA[i]-lateralB[i];n+=forward[i]*forward[i];}
    if(!std::isfinite(n)||n<1e-10)return false;n=std::sqrt(n);for(double& v:forward)v/=n;
    double d=0;for(size_t i=0;i<3;++i)d+=across[i]*forward[i];n=0;for(size_t i=0;i<3;++i){across[i]-=d*forward[i];n+=across[i]*across[i];}
    if(!std::isfinite(n)||n<1e-10)return false;n=std::sqrt(n);for(double& v:across)v/=n;
    const double normal[3]={forward[1]*across[2]-forward[2]*across[1],forward[2]*across[0]-forward[0]*across[2],forward[0]*across[1]-forward[1]*across[0]};
    for(size_t i=0;i<3;++i){frame[i][0]=forward[i];frame[i][1]=across[i];frame[i][2]=normal[i];}return true;
}
inline bool GeometryBasis(const double a[3][3],const double b[3][3],double* q) {
    double m[3][3]{};for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)for(size_t k=0;k<3;++k)m[i][j]+=b[i][k]*a[j][k];
    const double trace=m[0][0]+m[1][1]+m[2][2];
    if(trace>0){const double s=std::sqrt(trace+1)*2;q[3]=s/4;q[0]=(m[2][1]-m[1][2])/s;q[1]=(m[0][2]-m[2][0])/s;q[2]=(m[1][0]-m[0][1])/s;}
    else{size_t i=0;if(m[1][1]>m[i][i])i=1;if(m[2][2]>m[i][i])i=2;const size_t j=(i+1)%3,k=(i+2)%3;const double s=std::sqrt(1+m[i][i]-m[j][j]-m[k][k])*2;q[i]=s/4;q[j]=(m[j][i]+m[i][j])/s;q[k]=(m[k][i]+m[i][k])/s;q[3]=(m[k][j]-m[j][k])/s;}
    double n=0;for(size_t i=0;i<4;++i)n+=q[i]*q[i];if(!std::isfinite(n)||n<1e-10)return false;n=std::sqrt(n);for(size_t i=0;i<4;++i)q[i]/=n;return true;
}

template<class T> bool MapSourceMountPose(const T* source,const T* target,size_t anchor,size_t target_anchor,int32_t palm,int32_t target_palm,const T& mount,const double* basis,const T& component_space,T& result) {
    // This is a rigid left mapping in component space. Scaling by either
    // character's hand/torso would resize swords and would not commute with
    // the weapon actor's existing relative TRS. Native source mount scale is
    // retained, including authored zero-scale visibility / mirrored effects.
    T mapped;if(!FiniteMount(mount)||!FiniteMount(source[anchor])||!FiniteMount(target[target_anchor]))return false;
    double inverse[4],tmp[4],rotation[4],offset[3],moved[3];Inverse(source[anchor].rotation,inverse);Mul(target[target_anchor].rotation,basis,tmp);Mul(tmp,inverse,rotation);Mul(rotation,mount.rotation,mapped.rotation);
    const T& source_pivot=palm>=0?source[static_cast<size_t>(palm)]:source[anchor];const T& target_pivot=target_palm>=0?target[static_cast<size_t>(target_palm)]:target[target_anchor];
    if(!FiniteMount(source_pivot)||!FiniteMount(target_pivot))return false;
    for(size_t i=0;i<3;++i)offset[i]=mount.translation[i]-source_pivot.translation[i];Rotate(rotation,offset,moved);
    for(size_t i=0;i<3;++i){mapped.translation[i]=target_pivot.translation[i]+moved[i];mapped.scale[i]=mount.scale[i];}
    return ComposeMount(mapped,component_space,result);
}

template<class T> bool MapSourceMount(const T* source,const T* target,size_t bone,size_t anchor,size_t target_anchor,int32_t palm,int32_t target_palm,const T& socket_local,const double* basis,const T& component_space,T& result) {
    T mount;if(!ComposeMount(socket_local,source[bone],mount))return false;
    return MapSourceMountPose(source,target,anchor,target_anchor,palm,target_palm,mount,basis,component_space,result);
}

}
