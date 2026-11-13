// 身材（骨骼）改写。
//
// 做三件事：
//   1) 找到引擎的 component-space 姿势 buffer（属性名优先，问不到用 UUEPBS 在 PBZ 上实测的偏移）；
//   2) 在引擎刚算完姿势、送渲染之前改这份 buffer：经 BoneMotionRuntime 的钩子（和「骨骼变换」节点共用一个，
//      2026-11-13 起；原先自己改整个类的 vtable 第 380 项，和骨骼变换的钩子互相挡掉）；
//   3) 改法照抄 UUEPBS：部位 = 一组骨，组带「子骨怎么跟」的模式，自顶向下重算。
//
// 第 3 点是「大腿变粗但小腿不变粗」的关键：父骨放大之后，重算子骨时要把父骨那份额除回去。
#include "npc_body.h"

#include "npc_bridge.h"

#include "npc_body_rig.h"
#include "npc_body_preset.h"
#include "npc_anim.h"
#include "ue_runtime.h"
#include "bone_motion.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace NpcBody
{
    namespace
    {
        // ---- 姿势数学：照抄 UUEPBS src/core/xform.hpp（UE5 FTransform 的双精度布局）----
        namespace xf
        {
            constexpr double kTiny = 1.0e-8;

            Xform Identity()
            {
                Xform x{};
                x.rot[3] = 1.0;
                x.scl[0] = x.scl[1] = x.scl[2] = 1.0;
                return x;
            }

            void QMul(const double a[4], const double b[4], double out[4])
            {
                const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
                const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
                const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
                const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
                out[0] = x;
                out[1] = y;
                out[2] = z;
                out[3] = w;
            }

            void QConj(const double q[4], double out[4])
            {
                out[0] = -q[0];
                out[1] = -q[1];
                out[2] = -q[2];
                out[3] = q[3];
            }

            void QRotate(const double q[4], const double v[3], double out[3])
            {
                const double tx = 2.0 * (q[1] * v[2] - q[2] * v[1]);
                const double ty = 2.0 * (q[2] * v[0] - q[0] * v[2]);
                const double tz = 2.0 * (q[0] * v[1] - q[1] * v[0]);
                out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
                out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
                out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
            }

            Xform Compose(const Xform& child, const Xform& parent)
            {
                Xform r{};
                QMul(parent.rot, child.rot, r.rot);
                const double scaled[3] = {parent.scl[0] * child.pos[0], parent.scl[1] * child.pos[1], parent.scl[2] * child.pos[2]};
                double turned[3];
                QRotate(parent.rot, scaled, turned);
                r.pos[0] = turned[0] + parent.pos[0];
                r.pos[1] = turned[1] + parent.pos[1];
                r.pos[2] = turned[2] + parent.pos[2];
                r.scl[0] = child.scl[0] * parent.scl[0];
                r.scl[1] = child.scl[1] * parent.scl[1];
                r.scl[2] = child.scl[2] * parent.scl[2];
                return r;
            }

            double SafeInverse(double v) { return std::fabs(v) <= kTiny ? 0.0 : 1.0 / v; }

            Xform Relative(const Xform& x, const Xform& base)
            {
                Xform r{};
                const double inv[3] = {SafeInverse(base.scl[0]), SafeInverse(base.scl[1]), SafeInverse(base.scl[2])};
                double inv_rot[4];
                QConj(base.rot, inv_rot);
                const double delta[3] = {x.pos[0] - base.pos[0], x.pos[1] - base.pos[1], x.pos[2] - base.pos[2]};
                double local[3];
                QRotate(inv_rot, delta, local);
                r.pos[0] = local[0] * inv[0];
                r.pos[1] = local[1] * inv[1];
                r.pos[2] = local[2] * inv[2];
                QMul(inv_rot, x.rot, r.rot);
                r.scl[0] = x.scl[0] * inv[0];
                r.scl[1] = x.scl[1] * inv[1];
                r.scl[2] = x.scl[2] * inv[2];
                return r;
            }

            bool SameBits(const Xform& a, const Xform& b) { return std::memcmp(&a, &b, sizeof(Xform)) == 0; }

            bool Finite(const Xform& x)
            {
                for (int i = 0; i < 4; ++i)
                {
                    if (!std::isfinite(x.rot[i]) || !std::isfinite(x.pos[i]) || !std::isfinite(x.scl[i]))
                    {
                        return false;
                    }
                }
                return true;
            }

            // UUEPBS 的同一道闸：数值有限还不算数，量级也得像个人。变身时引擎会把主角骨架切到
            // AnimationRelative（子骨按参考姿势比例缩放），我们写进去的绝对值会被再乘一次；一旦乘爆，
            // 引擎算包围盒时直接踩到执行地址 0。写之前挡掉。
            bool Plausible(const Xform& x)
            {
                const double q = x.rot[0] * x.rot[0] + x.rot[1] * x.rot[1] + x.rot[2] * x.rot[2] + x.rot[3] * x.rot[3];
                if (std::fabs(q - 1.0) > 0.05)
                {
                    return false;
                }
                for (int i = 0; i < 3; ++i)
                {
                    const double s = std::fabs(x.scl[i]);
                    if (s < 1.0e-4 || s > 1.0e4)
                    {
                        return false;
                    }
                    if (std::fabs(x.pos[i]) > 1.0e7)
                    {
                        return false;
                    }
                }
                return true;
            }
        }  // namespace xf

        // ---- 布局：属性名问不到时用 UUEPBS 在 PBZ 上实测的值 ----
        constexpr int32_t kPinBuffers = 0x5F8;
        constexpr int32_t kPinReadIndex = 0x640;
        constexpr int32_t kArrayStride = 16;
        constexpr int32_t kElemSize = 96;
        constexpr int32_t kMaxBones = 40000;
        constexpr int32_t kMemoSlots = 4;
        constexpr int32_t kMaxGroups = 64;

        int32_t buffers_ = -1;
        int32_t read_index_ = -1;
        bool pinned_ = false;
        void* attached_ = nullptr;
        bool ready_ = false;
        int32_t count_ = 0;
        int32_t read_ = -1;
        void* data_ = nullptr;
        int32_t step_ = 0;
        long ticks_ = 0;
        long writes_ = 0;
        long misses_ = 0; long bad_values_ = 0;
        long faults_ = 0;
        char report_[512] = {};
        char dump_[2048] = {};

        // ---- 骨骼名 / 父子表 ----
        std::vector<std::wstring> names_;
        std::vector<std::string> narrow_;
        std::vector<int32_t> parents_;
        std::map<std::string, int32_t> name_index_;
        std::map<std::string, int32_t> lower_index_;
        int32_t names_count_ = 0;
        void* names_for_ = nullptr;
        bool parents_ok_ = false;
        SRWLOCK names_lock_ = SRWLOCK_INIT;

        // ---- 编辑书：骨索引 -> 编辑（名字层算完落这里） ----
        std::map<int32_t, NpcBodyRig::BoneEdit> book_;
        bool book_dirty_ = true;

        struct SlotEdit
        {
            int32_t bone = -1;
            int32_t axis = 3;
            double factor = 1.0;
        };
        std::vector<SlotEdit> slots_;  // 详细页：一根骨一个

        // ---- 部位组 ----
        std::vector<NpcBodyRig::Group> groups_;
        bool groups_dirty_ = true;
        volatile long group_request_[kMaxGroups] = {};
        volatile long group_factor_[kMaxGroups] = {};
        // 移动状态层：4 档 × 部位，和滑条层相乘叠加（照抄 v1.3 registry.cpp 的叠加规则）。
        volatile long loco_group_factor_[NpcBody::kLocoStates][kMaxGroups] = {};
        volatile long loco_state_ = 0;
        volatile long loco_driving_ = 0;
        // 预设层：载入的那一份是基底，画布上的滑块盖在它上面。
        NpcBodyPreset::Book preset_book_;
        volatile long preset_request_ = 0;  // 1 = 存, 2 = 载, 3 = 删
        char preset_name_[96] = {};
        char preset_message_[256] = {};
// 程序化时间轴动画层（照抄 v1.3 registry.cpp）：每帧推进，叠在滑条与预设之上。
std::vector<NpcAnim::Animation> anims_;
std::vector<NpcAnim::AnimRuntime> anim_runtimes_;
std::map<std::string, NpcBodyRig::BoneEdit> anim_layer_;
std::chrono::steady_clock::time_point anim_last_tick_{};
bool anim_playing_ = true;
bool anim_loaded_ = false;
// 早期自写面板的直接写入（滑条 / 预设 / 动画层，在组件 tick 之后写 buffer）：那套面板已经不用了，写入也关掉，
// 免得和搬进来的 UUEPBS 编辑器（在姿势 finalize 里写）各写一遍、动画播两次。只留读骨架的部分。
bool legacy_writes_ = false;

        // ---- plan / 重算用的裸数组（SEH 里只碰这些，不构造对象）----
        int32_t plan_[kMaxBones] = {};
        int32_t plan_count_ = 0;
        int32_t edit_slot_[kMaxBones] = {};
        uint8_t in_plan_[kMaxBones] = {};
        uint8_t pushes_[kMaxBones] = {};
        NpcBodyRig::BoneEdit scales_[kMaxBones] = {};
        double turns_[kMaxBones][4] = {};
        int32_t scale_count_ = 0;
        std::vector<Xform> source_;
        std::vector<Xform> fresh_;
        std::vector<Xform> for_children_;

        struct Memo
        {
            const void* buffer = nullptr;
            unsigned long long last_use = 0;
            std::vector<Xform> source;
            std::vector<Xform> result;
            std::vector<uint8_t> marked;
            std::vector<int32_t> marked_list;
        };
        Memo memos_[kMemoSlots];
        unsigned long long clock_ = 0;

        void Report(bool ok, const char* why)
        {
            _snprintf_s(report_, _countof(report_), _TRUNCATE,
                        "%s step=%d bones=%d read=%d buffers=0x%X read_index=0x%X (%s) plan=%d groups=%d ticks=%ld writes=%ld "
                        "misses=%ld bad=%ld faults=%ld why=%s",
                        ok ? "ok=1" : "ok=0", step_, count_, read_, buffers_, read_index_, pinned_ ? "pinned" : "reflected",
                        plan_count_, static_cast<int>(groups_.size()), ticks_, writes_, misses_, bad_values_, faults_,
                        why ? why : "-");
        }

        bool Resolve(void* component)
        {
            ready_ = false;
            pinned_ = false;
            const ue::Object* cls = ue::ClassOf(static_cast<ue::Object*>(component));
            int32_t buffers = cls ? ue::Offset(const_cast<ue::Object*>(cls), "ComponentSpaceTransformsArray") : -1;
            int32_t read_index = cls ? ue::Offset(const_cast<ue::Object*>(cls), "CurrentReadComponentTransforms") : -1;
            if (buffers < 0 || read_index < 0)
            {
                buffers = kPinBuffers;
                read_index = kPinReadIndex;
                pinned_ = true;
            }
            if (buffers < 0 || read_index < 0)
            {
                return false;
            }
            buffers_ = buffers;
            read_index_ = read_index;
            attached_ = component;
            ready_ = true;
            return true;
        }

        std::wstring FoldWide(const std::wstring& text)
        {
            std::wstring out(text);
            for (wchar_t& c : out)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return out;
        }

        void TurnQuaternion(const NpcBodyRig::BoneEdit& edit, double out[4])
        {
            constexpr double kHalfRad = 3.14159265358979323846 / 360.0;
            const double hx = edit.turn[0] * kHalfRad;
            const double hy = edit.turn[1] * kHalfRad;
            const double hz = edit.turn[2] * kHalfRad;
            const double qx[4] = {std::sin(hx), 0.0, 0.0, std::cos(hx)};
            const double qy[4] = {0.0, std::sin(hy), 0.0, std::cos(hy)};
            const double qz[4] = {0.0, 0.0, std::sin(hz), std::cos(hz)};
            double yx[4];
            xf::QMul(qy, qx, yx);
            xf::QMul(qz, yx, out);
        }

        // 把 slots_ + groups_ 的结果写进 book_，重算 plan。
        void Bind()
        {
            book_.clear();
            // 预设是基底：先铺预设，再让滑块和部位盖上去。
            for (const auto& [preset_name, preset_edit] : preset_book_)
            {
                const auto preset_hit = lower_index_.find(preset_name);
                if (preset_hit == lower_index_.end() || preset_hit->second < 0 || preset_hit->second >= count_)
                {
                    continue;
                }
                book_[preset_hit->second] = preset_edit;
            }
            for (const SlotEdit& s : slots_)
            {
                if (s.bone < 0 || s.bone >= count_)
                {
                    continue;
                }
                NpcBodyRig::BoneEdit edit;
                edit.axis[0] = edit.axis[1] = edit.axis[2] = s.factor;
                if (s.axis >= 0 && s.axis < 3)
                {
                    edit.axis[0] = edit.axis[1] = edit.axis[2] = 1.0;
                    edit.axis[s.axis] = s.factor;
                }
                edit.spread = NpcBodyRig::Spread::Chain;
                book_[s.bone] = edit;
            }
            // 简化页的部位：每组按当前倍数展开成一条条骨编辑。
            for (int32_t g = 0; g < static_cast<int32_t>(groups_.size()) && g < kMaxGroups; ++g)
            {
                double factor = static_cast<double>(group_factor_[g]) / 1000.0;
                // 移动状态层：照抄 v1.3 的叠加 —— 滑条 × 状态，两层的缩放相乘。
                if (loco_driving_)
                {
                    factor *= static_cast<double>(loco_group_factor_[loco_state_][g]) / 1000.0;
                }
                if (std::fabs(factor - 1.0) < 1.0e-6)
                {
                    continue;
                }
                const NpcBodyRig::Group& group = groups_[static_cast<size_t>(g)];
                NpcBodyRig::BoneEdit edit;
                edit.axis[0] = edit.axis[1] = edit.axis[2] = factor;
                edit.spread = group.spread;
                for (const std::string& bone_name : group.bones)
                {
                    const auto hit = name_index_.find(bone_name);
                    if (hit == name_index_.end() || hit->second < 0 || hit->second >= count_)
                    {
                        continue;
                    }
                    book_[hit->second] = edit;
                }
            }
            // 时间轴动画层：照抄 v1.3 registry.cpp:79 的 add_layer —— 旋转和位移相加，缩放相乘。
            for (const auto& [anim_name, anim_edit] : anim_layer_)
            {
                const auto hit = lower_index_.find(anim_name);
                if (hit == lower_index_.end() || hit->second < 0 || hit->second >= count_)
                {
                    continue;
                }
                const auto slot = book_.find(hit->second);
                if (slot == book_.end())
                {
                    book_[hit->second] = anim_edit;
                    continue;
                }
                NpcBodyRig::BoneEdit merged = slot->second;
                for (int a = 0; a < 3; ++a)
                {
                    merged.axis[a] *= anim_edit.axis[a];
                    merged.turn[a] += anim_edit.turn[a];
                    merged.shift[a] += anim_edit.shift[a];
                }
                merged.clamp();
                slot->second = merged;
            }
            book_dirty_ = false;
            plan_count_ = 0;
            scale_count_ = 0;
            if (count_ <= 0 || book_.empty() || parents_.size() != names_.size())
            {
                for (int32_t i = 0; i < kMaxBones; ++i)
                {
                    edit_slot_[i] = -1;
                    in_plan_[i] = 0;
                }
                return;
            }

            const int32_t n = count_ < kMaxBones ? count_ : kMaxBones;
            for (int32_t i = 0; i < n; ++i)
            {
                edit_slot_[i] = -1;
                in_plan_[i] = 0;
                pushes_[i] = 0;
            }
            for (const auto& entry : book_)
            {
                const int32_t bone = entry.first;
                if (bone < 0 || bone >= n || entry.second.neutral())
                {
                    continue;
                }
                NpcBodyRig::BoneEdit edit = entry.second;
                edit.clamp();
                edit_slot_[bone] = scale_count_;
                if (scale_count_ < kMaxBones)
                {
                    scales_[scale_count_] = edit;
                    TurnQuaternion(edit, turns_[scale_count_]);
                    ++scale_count_;
                }
            }            for (int32_t i = 0; i < n; ++i)
            {
                const int32_t p = i < static_cast<int32_t>(parents_.size()) ? parents_[i] : -1;
                const bool moved = p >= 0 && p < n && pushes_[p] != 0;
                const int32_t slot = edit_slot_[i];
                const bool edited = slot >= 0;
                if (!edited && !moved)
                {
                    continue;
                }
                if (plan_count_ < kMaxBones)
                {
                    plan_[plan_count_++] = i;
                }
                in_plan_[i] = 1;
                pushes_[i] = (moved || (edited && scales_[slot].spread != NpcBodyRig::Spread::Solo)) ? 1 : 0;
            }
            const size_t size = static_cast<size_t>(n);
            source_.assign(size, xf::Identity());
            fresh_.assign(size, xf::Identity());
            for_children_.assign(size, xf::Identity());
        }

        Memo& MemoFor(const void* buffer)
        {
            const size_t n = static_cast<size_t>(count_);
            ++clock_;
            Memo* oldest = &memos_[0];
            for (Memo& m : memos_)
            {
                if (m.buffer == buffer && m.marked.size() == n)
                {
                    m.last_use = clock_;
                    return m;
                }
                if (m.last_use < oldest->last_use)
                {
                    oldest = &m;
                }
            }
            Memo& m = *oldest;
            m.buffer = buffer;
            m.last_use = clock_;
            m.source.assign(n, xf::Identity());
            m.result.assign(n, xf::Identity());
            m.marked.assign(n, 0);
            m.marked_list.clear();
            return m;
        }

        // 照抄 UUEPBS RigSculptor::apply：还原 -> 自顶向下重算 -> 提交。
        int32_t Sculpt(void* buffer, int32_t count)
        {
            const int32_t n = count_;
            if (!buffer || count != n || n <= 0 || plan_count_ <= 0)
            {
                return 0;
            }
            Xform* pose = static_cast<Xform*>(buffer);
            Memo& memo = MemoFor(buffer);
            int32_t written = 0;

            for (const int32_t i : memo.marked_list)
            {
                if (!in_plan_[i])
                {
                    if (xf::SameBits(pose[i], memo.result[static_cast<size_t>(i)]))
                    {
                        pose[i] = memo.source[static_cast<size_t>(i)];
                        ++written;
                    }
                    memo.marked[static_cast<size_t>(i)] = 0;
                }
            }
            for (int32_t k = 0; k < plan_count_; ++k)
            {
                const int32_t i = plan_[k];
                const bool ours = memo.marked[static_cast<size_t>(i)] != 0 && xf::SameBits(pose[i], memo.result[static_cast<size_t>(i)]);
                source_[static_cast<size_t>(i)] = ours ? memo.source[static_cast<size_t>(i)] : pose[i];
            }

            const Xform unit = xf::Identity();
            for (int32_t k = 0; k < plan_count_; ++k)
            {
                const int32_t i = plan_[k];
                const int32_t p = i < static_cast<int32_t>(parents_.size()) ? parents_[i] : -1;
                const bool parent_planned = p >= 0 && in_plan_[p] != 0;
                const Xform& parent_before = p < 0 ? unit : (parent_planned ? source_[static_cast<size_t>(p)] : pose[p]);
                const Xform& parent_after = p < 0 ? unit : (parent_planned ? for_children_[static_cast<size_t>(p)] : pose[p]);

                Xform local = p < 0 ? source_[static_cast<size_t>(i)] : xf::Relative(source_[static_cast<size_t>(i)], parent_before);

                // 父骨是「子骨保持粗细」：把它那份缩放除回去。
                if (p >= 0 && p < n && edit_slot_[p] >= 0)
                {
                    const NpcBodyRig::BoneEdit& ps = scales_[edit_slot_[p]];
                    if (ps.spread == NpcBodyRig::Spread::Keep)
                    {
                        for (int a = 0; a < 3; ++a)
                        {
                            local.scl[a] /= ps.axis[a];
                        }
                    }
                }

                const int32_t slot = edit_slot_[i];
                if (slot >= 0)
                {
                    const NpcBodyRig::BoneEdit& s = scales_[slot];
                    Xform edited = local;
                    if (s.has_shift())
                    {
                        double offset[3];
                        xf::QRotate(local.rot, s.shift, offset);
                        for (int a = 0; a < 3; ++a)
                        {
                            edited.pos[a] += offset[a] / (std::fabs(parent_after.scl[a]) > xf::kTiny ? parent_after.scl[a] : 1.0);
                        }
                    }
                    if (s.has_turn())
                    {
                        xf::QMul(local.rot, turns_[slot], edited.rot);
                    }
                    for (int a = 0; a < 3; ++a)
                    {
                        edited.scl[a] *= s.axis[a];
                    }
                    fresh_[static_cast<size_t>(i)] = xf::Compose(edited, parent_after);
                    for_children_[static_cast<size_t>(i)] =
                        s.spread == NpcBodyRig::Spread::Solo ? xf::Compose(local, parent_after) : fresh_[static_cast<size_t>(i)];
                }
                else
                {
                    fresh_[static_cast<size_t>(i)] = xf::Compose(local, parent_after);
                    for_children_[static_cast<size_t>(i)] = fresh_[static_cast<size_t>(i)];
                }
                fresh_[static_cast<size_t>(i)].pos[3] = 0.0;
                fresh_[static_cast<size_t>(i)].scl[3] = 0.0;
            }

            memo.marked_list.clear();
            for (int32_t k = 0; k < plan_count_; ++k)
            {
                const int32_t i = plan_[k];
                const Xform& value = fresh_[static_cast<size_t>(i)];
                if (!xf::Finite(value) || !xf::Plausible(value))
                {
                    ++bad_values_;
                    continue;
                }
                pose[i] = value;
                memo.source[static_cast<size_t>(i)] = source_[static_cast<size_t>(i)];
                memo.result[static_cast<size_t>(i)] = value;
                memo.marked[static_cast<size_t>(i)] = 1;
                memo.marked_list.push_back(i);
                ++written;
            }
            return written;
        }

        // 面板攒下的部位请求 -> 当前值；真正的展开在 Bind() 里做（它才拥有 book_）。
        void ApplyGroupRequests()
        {
            if (groups_.empty())
            {
                return;
            }
            for (int32_t g = 0; g < static_cast<int32_t>(groups_.size()) && g < kMaxGroups; ++g)
            {
                const long want = InterlockedExchange(&group_request_[g], 0);
                if (want == 0)
                {
                    continue;
                }
                group_factor_[g] = want;
                book_dirty_ = true;
            }
        }

        // 面板发来的预设请求：存 / 载 / 删，都由游戏线程做（book_ 只在这里动）。
        void ApplyPresetRequests()
        {
            const long kind = InterlockedExchange(&preset_request_, 0);
            if (kind == 0)
            {
                return;
            }
            const std::string name(preset_name_);
            std::string message;
            if (kind == 1)
            {
                NpcBodyPreset::Book book;
                for (const auto& [bone, edit] : book_)
                {
                    if (bone < 0 || bone >= static_cast<int32_t>(names_.size()))
                    {
                        continue;
                    }
                    book[NpcBodyRig::FoldCase(std::string(narrow_[static_cast<size_t>(bone)]))] = edit;
                }
                NpcBodyPreset::Save(name, book, message);
            }
            else if (kind == 2)
            {
                NpcBodyPreset::Book book;
                if (NpcBodyPreset::Load(name, book, message))
                {
                    preset_book_.swap(book);
                    slots_.clear();
                    for (int32_t g = 0; g < kMaxGroups; ++g)
                    {
                        group_factor_[g] = 1000;
                        InterlockedExchange(&group_request_[g], 0);
                    }
                    book_dirty_ = true;
                }
            }
            else if (kind == 3)
            {
                NpcBodyPreset::Remove(name, message);
            }
            _snprintf_s(preset_message_, sizeof(preset_message_), _TRUNCATE, "%s", message.c_str());
        }

        // 照抄 v1.3 registry.cpp:1117 的 update_anim_layer_locked —— 逐动画逐轨道推进，
        // 把结果叠成一本「名字 -> 编辑」的书；同名骨按 add_layer 的规则合并。
        void UpdateAnimLayer()
        {
            const auto now = std::chrono::steady_clock::now();
            const bool first_tick = anim_last_tick_ == std::chrono::steady_clock::time_point{};
            double dt = first_tick ? 0.0 : std::chrono::duration<double>(now - anim_last_tick_).count();
            if (dt > 0.25)
            {
                dt = 0.25;  // 卡顿/暂停之后不要把整段动画一下跳过
            }
            const bool advance = anim_playing_ && dt >= 0.0005;
            if (advance || first_tick)
            {
                anim_last_tick_ = now;
            }

            std::map<std::string, NpcBodyRig::BoneEdit> layer;
            for (size_t a = 0; a < anims_.size(); ++a)
            {
                NpcAnim::Animation& animation = anims_[a];
                if (!animation.enabled)
                {
                    continue;
                }
                NpcAnim::AnimRuntime& runtime = anim_runtimes_[a];
                runtime.tracks.resize(animation.tracks.size());
                for (size_t t = 0; t < animation.tracks.size(); ++t)
                {
                    NpcAnim::AnimTrack& track = animation.tracks[t];
                    if (!track.enabled || track.bones.empty())
                    {
                        continue;
                    }
                    const NpcBodyRig::BoneEdit value = NpcAnim::anim_track_advance(track, runtime.tracks[t], advance ? dt : 0.0);
                    if (value.neutral())
                    {
                        continue;
                    }
                    NpcBodyRig::BoneEdit edit = value;
                    edit.spread = track.spread;
                    for (const std::string& bone : track.bones)
                    {
                        const std::string key = NpcBodyRig::FoldCase(bone);
                        if (key.empty())
                        {
                            continue;
                        }
                        const auto it = layer.find(key);
                        if (it == layer.end())
                        {
                            layer.emplace(key, edit);
                            continue;
                        }
                        for (int i = 0; i < 3; ++i)
                        {
                            it->second.axis[i] *= edit.axis[i];
                            it->second.turn[i] += edit.turn[i];
                            it->second.shift[i] += edit.shift[i];
                        }
                        it->second.clamp();
                    }
                }
            }
            if (advance || !anim_layer_.empty() || !layer.empty())
            {
                anim_layer_ = std::move(layer);
            }
        }

        // 单独一个函数包住 __try：C2712 不允许「有对象展开的函数」里出现 __try，
        // 而 Tick() 现在要调 UpdateAnimLayer()（std::map 局部对象），只能把这段搬出来。
        bool WritePose(void* data, int32_t num)
        {
            __try
            {
                writes_ += Sculpt(data, num);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                ++faults_;
                attached_ = nullptr;
                buffers_ = -1;
                read_index_ = -1;
                ready_ = false;
                step_ = 9;
                Report(false, "fault while writing the pose");
                return false;
            }
            return true;
        }
    }  // namespace

    bool Tick(void* component)
    {
        ++ticks_;
        if (!component)
        {
            Report(false, "no component");
            return false;
        }
        step_ = 1;
        if (!ready_ || attached_ != component)
        {
            if (!Resolve(component))
            {
                Report(false, "no offset for the pose buffer");
                return false;
            }
        }

        // 第一次进来（或换了组件）要问骨骼名与父子表；问过就缓存，不再花这几百毫秒。
        if (names_count_ <= 0 || parents_.size() != names_.size())
        {
            Names(component);
            Parents(component);
        }
        if (groups_dirty_ && count_ > 0)
        {
            ResolveGroups(component);
        }
        if (legacy_writes_)
        {
            if (!anim_loaded_)
            {
                anim_loaded_ = true;
                anims_ = NpcAnim::anim_load_all(NpcBodyPreset::Folder());
                anim_runtimes_.assign(anims_.size(), NpcAnim::AnimRuntime{});
            }
            ApplyPresetRequests();
            ApplyGroupRequests();
            UpdateAnimLayer();
            if (!anims_.empty())
            {
                book_dirty_ = true;  // 动画每帧都在变，绑定要跟着重算
            }
            if (book_dirty_)
            {
                Bind();
            }
        }

        step_ = 2;
        const int32_t read = ue::At<int32_t>(component, read_index_);
        if (read != 0 && read != 1)
        {
            ++misses_;
            Report(false, "read index is not 0/1");
            return false;
        }
        read_ = read;

        step_ = 3;
        const ue::Array pose = ue::At<ue::Array>(component, buffers_ + read * kArrayStride);
        if (!pose.data || pose.num <= 0 || pose.num > kMaxBones)
        {
            ++misses_;
            Report(false, "no pose buffer");
            return false;
        }
        count_ = pose.num;
        data_ = pose.data;

        step_ = 4;
        if (legacy_writes_ && !WritePose(pose.data, pose.num))
        {
            return false;
        }
        step_ = 5;
        Report(true, nullptr);
        return true;
    }

    bool Ready() { return ready_ && count_ > 0; }
    int32_t Count() { return count_; }
    const char* Report() { return report_; }

    const char* Dump(int32_t want)
    {
        dump_[0] = 0;
        if (!data_ || count_ <= 0)
        {
            return dump_;
        }
        const int32_t n = want < count_ ? want : count_;
        size_t used = 0;
        __try
        {
            for (int32_t i = 0; i < n; ++i)
            {
                const Xform* bone = reinterpret_cast<const Xform*>(static_cast<const unsigned char*>(data_) + static_cast<size_t>(i) * kElemSize);
                const int written = _snprintf_s(dump_ + used, sizeof(dump_) - used, _TRUNCATE, "bone[%d] scl=(%.3f,%.3f,%.3f) pos=(%.2f,%.2f,%.2f)|", i,
                                                bone->scl[0], bone->scl[1], bone->scl[2], bone->pos[0], bone->pos[1], bone->pos[2]);
                if (written <= 0)
                {
                    break;
                }
                used += static_cast<size_t>(written);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _snprintf_s(dump_, _countof(dump_), _TRUNCATE, "dump fault code=0x%08lX", GetExceptionCode());
        }
        return dump_;
    }

    int32_t Names(void* component)
    {
        if (names_count_ > 0 && names_for_ == component)
        {
            return names_count_;
        }
        names_count_ = 0;
        parents_ok_ = false;
        groups_dirty_ = true;
        if (!component)
        {
            return 0;
        }
        ue::Call total(static_cast<ue::Object*>(component), "GetNumBones");
        const int32_t count = total.Invoke() ? total.Get<int32_t>("ReturnValue") : 0;
        if (count <= 0 || count > kMaxBones)
        {
            return 0;
        }
        std::vector<std::wstring> built;
        std::vector<std::string> narrow;
        built.reserve(static_cast<size_t>(count));
        narrow.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            ue::Call get(static_cast<ue::Object*>(component), "GetBoneName");
            get.Set<int32_t>("BoneIndex", i);
            wchar_t text[128] = {};
            if (get.Invoke())
            {
                ue::NameWide(get.Get<ue::Name>("ReturnValue"), text, _countof(text));
            }
            if (text[0] == 0)
            {
                _snwprintf_s(text, _countof(text), _TRUNCATE, L"#%d", i);
            }
            built.emplace_back(text);
            std::string narrow_name;
            for (const wchar_t c : built.back())
            {
                narrow_name += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
            }
            narrow.push_back(narrow_name);
        }
        AcquireSRWLockExclusive(&names_lock_);
        names_.swap(built);
        narrow_.swap(narrow);
        names_count_ = static_cast<int32_t>(names_.size());
        ReleaseSRWLockExclusive(&names_lock_);
        name_index_.clear();
        lower_index_.clear();
        for (int32_t i = 0; i < static_cast<int32_t>(narrow_.size()); ++i)
        {
            const std::string& one = narrow_[static_cast<size_t>(i)];
            name_index_.emplace(one, i);
            std::string lower(one);
            for (char& c : lower)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            lower_index_.emplace(lower, i);
        }
        names_for_ = component;
        return names_count_;
    }

    const wchar_t* NameAt(int32_t index)
    {
        static const wchar_t kNone[] = L"";
        if (index < 0 || index >= static_cast<int32_t>(names_.size()))
        {
            return kNone;
        }
        return names_[static_cast<size_t>(index)].c_str();
    }

    int32_t SnapshotNames(wchar_t* out, int32_t slotChars, int32_t maxSlots)
    {
        if (!out || slotChars <= 0 || maxSlots <= 0)
        {
            return 0;
        }
        AcquireSRWLockShared(&names_lock_);
        const int32_t total = static_cast<int32_t>(names_.size());
        const int32_t n = total < maxSlots ? total : maxSlots;
        for (int32_t i = 0; i < n; ++i)
        {
            wcsncpy_s(out + static_cast<size_t>(i) * slotChars, static_cast<size_t>(slotChars), names_[static_cast<size_t>(i)].c_str(), _TRUNCATE);
        }
        ReleaseSRWLockShared(&names_lock_);
        return n;
    }

    int32_t SnapshotCount() { return names_count_; }

    // 按名字找骨（大小写无关），一次问完；找过的名字已经缓存在这里，不再碰引擎。
    int32_t FindBones(void* component, const char* const* wanted, int32_t countNames, int32_t* out)
    {
        if (!out || !wanted || countNames <= 0)
        {
            return 0;
        }
        for (int32_t i = 0; i < countNames; ++i)
        {
            out[i] = -1;
        }
        if (names_.empty() && component)
        {
            Names(component);
        }
        int32_t hits = 0;
        for (int32_t i = 0; i < countNames; ++i)
        {
            if (!wanted[i])
            {
                continue;
            }
            std::string lower(wanted[i]);
            for (char& c : lower)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            const auto hit = lower_index_.find(lower);
            if (hit != lower_index_.end())
            {
                out[i] = hit->second;
                ++hits;
            }
        }
        return hits;
    }

    int32_t Parents(void* component)
    {
        if (parents_ok_ && parents_.size() == names_.size() && !names_.empty())
        {
            return static_cast<int32_t>(parents_.size());
        }
        parents_ok_ = false;
        if (!component || names_.empty())
        {
            return 0;
        }
        const int32_t n = static_cast<int32_t>(names_.size());
        std::map<std::wstring, int32_t> index;
        for (int32_t i = 0; i < n; ++i)
        {
            index.emplace(FoldWide(names_[static_cast<size_t>(i)]), i);
        }
        std::vector<int32_t> built(static_cast<size_t>(n), -1);
        int32_t found = 0;
        for (int32_t i = 0; i < n; ++i)
        {
            ue::Call get(static_cast<ue::Object*>(component), "GetBoneName");
            get.Set<int32_t>("BoneIndex", i);
            if (!get.Invoke())
            {
                break;
            }
            ue::Call up(static_cast<ue::Object*>(component), "GetParentBone");
            up.Set<ue::Name>("BoneName", get.Get<ue::Name>("ReturnValue"));
            if (!up.Invoke())
            {
                continue;
            }
            const ue::Name parent = up.Get<ue::Name>("ReturnValue");
            if (parent.None())
            {
                continue;
            }
            wchar_t text[128] = {};
            ue::NameWide(parent, text, _countof(text));
            const auto it = index.find(FoldWide(text));
            if (it != index.end() && it->second != i)
            {
                built[static_cast<size_t>(i)] = it->second;
                ++found;
            }
        }
        parents_.swap(built);
        parents_ok_ = found > 0;
        groups_dirty_ = true;
        return parents_ok_ ? n : 0;
    }

    int32_t ParentOf(int32_t bone)
    {
        if (bone < 0 || bone >= static_cast<int32_t>(parents_.size()))
        {
            return -1;
        }
        return parents_[static_cast<size_t>(bone)];
    }

    void SetEdit(int32_t slot, int32_t bone, int32_t axis, double factor)
    {
        const double value = factor > 0.0 ? factor : 1.0;
        if (slot >= 0 && slot < static_cast<int32_t>(slots_.size()))
        {
            slots_[static_cast<size_t>(slot)].bone = bone;
            slots_[static_cast<size_t>(slot)].axis = axis;
            slots_[static_cast<size_t>(slot)].factor = value;
        }
        else
        {
            slots_.push_back({bone, axis, value});
        }
        book_dirty_ = true;
    }

    void ClearEdits()
    {
        slots_.clear();
        book_dirty_ = true;
    }

    int32_t EditCount() { return static_cast<int32_t>(slots_.size()); }

    void SetStretch(int32_t bone, double factor) { SetEdit(-1, bone, 3, factor); }

    int32_t GroupCount() { return NpcBodyRig::GroupCount(); }

    int32_t ResolveGroups(void* component)
    {
        if (!component)
        {
            return 0;
        }
        if (names_.empty())
        {
            Names(component);
        }
        if (parents_.size() != names_.size())
        {
            Parents(component);
        }
        groups_.clear();
        if (!names_.empty())
        {
            groups_ = NpcBodyRig::Resolve(narrow_, parents_);
        }
        for (int32_t g = 0; g < kMaxGroups; ++g)
        {
            group_request_[g] = 0;
            group_factor_[g] = 1000;
        }
        groups_dirty_ = false;
        book_dirty_ = true;
        return static_cast<int32_t>(groups_.size());
    }

    int32_t SnapshotGroups(GroupInfo* out, int32_t maxGroups)
    {
        if (!out || maxGroups <= 0)
        {
            return 0;
        }
        const int32_t total = static_cast<int32_t>(groups_.size());
        const int32_t n = total < maxGroups ? total : maxGroups;
        for (int32_t i = 0; i < n; ++i)
        {
            const NpcBodyRig::Group& group = groups_[static_cast<size_t>(i)];
            GroupInfo& info = out[i];
            std::wmemset(info.title, 0, _countof(info.title));
            std::wmemset(info.note, 0, _countof(info.note));
            std::wmemset(info.spread, 0, _countof(info.spread));
            for (size_t c = 0; c < group.title.size() && c + 1 < _countof(info.title); ++c)
            {
                info.title[c] = static_cast<wchar_t>(group.title[c]);
            }
            for (size_t c = 0; c < group.note.size() && c + 1 < _countof(info.note); ++c)
            {
                const char ch = group.note[c];
                info.note[c] = (static_cast<unsigned char>(ch) & 0x80) ? L'?' : static_cast<wchar_t>(ch);
            }
            MultiByteToWideChar(CP_UTF8, 0, group.note.c_str(), -1, info.note, static_cast<int>(_countof(info.note)));
            const wchar_t* spread = NpcBodyRig::SpreadLabel(group.spread);
            wcsncpy_s(info.spread, _countof(info.spread), spread, _TRUNCATE);
            info.bones = static_cast<int32_t>(group.bones.size());
            info.factor_milli = static_cast<int32_t>(group_factor_[i]);
        }
        return n;
    }

    void SetGroup(int32_t group, double factor)
    {
        if (group < 0 || group >= static_cast<int32_t>(groups_.size()) || group >= kMaxGroups)
        {
            return;
        }
        double value = factor < 0.25 ? 0.25 : factor > 4.0 ? 4.0 : factor;
        const long milli = static_cast<long>(value * 1000.0 + 0.5);
        group_factor_[group] = milli;
        group_request_[group] = milli;
    }

    double GroupFactor(int32_t group)
    {
        if (group < 0 || group >= kMaxGroups)
        {
            return 1.0;
        }
        return static_cast<double>(group_factor_[group]) / 1000.0;
    }

    // ---- 移动状态：按速度选档，状态层与滑条层相乘（照抄 v1.3 的 Locomotion）----
    void SetLocoSpeed(double cmps)
    {
        int32_t state = 0;
        if (cmps >= NpcBody::kLocoRun) { state = 3; }
        else if (cmps >= NpcBody::kLocoWalk) { state = 2; }
        else if (cmps >= NpcBody::kLocoIdle) { state = 1; }
        if (state != loco_state_)
        {
            loco_state_ = state;
            book_dirty_ = true;  // 换档才重算，不是每帧
        }
    }

    int32_t LocoState() { return static_cast<int32_t>(loco_state_); }

    void SetLocoDriving(bool on)
    {
        const long want = on ? 1 : 0;
        if (want != loco_driving_)
        {
            loco_driving_ = want;
            book_dirty_ = true;
        }
    }

    bool LocoDriving() { return loco_driving_ != 0; }

    void SetLocoFactor(int32_t state, int32_t group, double factor)
    {
        if (state < 0 || state >= NpcBody::kLocoStates || group < 0 || group >= kMaxGroups)
        {
            return;
        }
        double value = factor < 0.25 ? 0.25 : factor > 4.0 ? 4.0 : factor;
        const long milli = static_cast<long>(value * 1000.0 + 0.5);
        if (milli != loco_group_factor_[state][group])
        {
            loco_group_factor_[state][group] = milli;
            loco_driving_ = 1;  // 第一次碰状态层就启用，不用另外开关
            if (state == loco_state_) { book_dirty_ = true; }
        }
    }

    double LocoFactor(int32_t state, int32_t group)
    {
        if (state < 0 || state >= NpcBody::kLocoStates || group < 0 || group >= kMaxGroups)
        {
            return 1.0;
        }
        return static_cast<double>(loco_group_factor_[state][group]) / 1000.0;
    }

    void RequestPreset(int32_t kind, const char* name)
    {
        _snprintf_s(preset_name_, sizeof(preset_name_), _TRUNCATE, "%s", name ? name : "");
        InterlockedExchange(&preset_request_, kind);
    }

    const char* PresetMessage() { return preset_message_; }

    void ResetAll()
    {
        preset_book_.clear();
        slots_.clear();
        for (int32_t g = 0; g < kMaxGroups; ++g)
        {
            group_factor_[g] = 1000;
            InterlockedExchange(&group_request_[g], 0);
        }
        book_dirty_ = true;
        _snprintf_s(preset_message_, sizeof(preset_message_), _TRUNCATE, "已把一切改回原样");
    }

    // ---- 姿势钩子：BoneMotionRuntime 的那一个（和「骨骼变换」节点共用）----
    // 引擎刚算完这个组件的姿势、骨骼变换节点也写完之后，把这一帧交给搬进来的 UUEPBS 编辑器。
    namespace
    {
        constexpr int32_t kHookSlot = 380;  // BoneMotionRuntime 换的那一项（组件私有 vtable 副本）
        void* g_component = nullptr;
        volatile long g_hook_calls = 0;

        void OnPose(ue::Object* self)
        {
            InterlockedIncrement(&g_hook_calls);
            NpcBridge::OnPoseFinalized(self);
        }
    }  // namespace

    bool Hook(void* component)
    {
        if (!component)
        {
            return false;
        }
        if (g_component != component)
        {
            g_component = component;
            InterlockedExchange(&g_hook_calls, 0);
        }
        BoneMotionRuntime::Listen(static_cast<ue::Object*>(component), &OnPose);
        return true;
    }

    void Unhook()
    {
        BoneMotionRuntime::Listen(nullptr, nullptr);
        g_component = nullptr;
    }

    bool Hooked() { return g_component != nullptr && BoneMotionRuntime::Listening(); }
    void* HookedComponent() { return g_component; }
    int32_t HookSlot() { return kHookSlot; }
    long long HookCalls() { return g_hook_calls; }
    long long HookForeign() { return 0; }
    const char* HookReport() { return BoneMotionRuntime::ListenNote(); }

    int32_t BufferOffset() { return buffers_; }
    int32_t ReadIndexOffset() { return read_index_; }
}  // namespace NpcBody
