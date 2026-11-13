#include "npc_anim.h"

#include "json.hpp"
// The port named its copy of UUEPBS's JSON reader pbzjson: it is uuepbs::json (src/uuepbs/core/json.hpp).
namespace pbzjson { using uuepbs::json::JsonValue; using uuepbs::json::JsonReader; using uuepbs::fold_case; }
#include "npc_body_preset.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <utility>

namespace fs = std::filesystem;

namespace NpcAnim
{
    namespace
    {
        // 名字对齐垫片：原版叫 spread_token / spread_from_token / PresetShelf::clean_name，
        // 我们这边是 SpreadToken / SpreadFromChildren / NpcBodyPreset::CleanName，语义一致，逻辑一行没改。
        const char* spread_token(NpcBodyRig::Spread spread) { return NpcBodyRig::SpreadToken(spread); }
        bool spread_from_token(std::string_view token, NpcBodyRig::Spread& out) { return NpcBodyRig::SpreadFromChildren(std::string(token), out); }
        std::string clean_name(const std::string& name) { return NpcBodyPreset::CleanName(name); }
        std::string path_to_utf8(const fs::path& path)
        {
            const std::wstring wide = path.wstring();
            if (wide.empty())
            {
                return {};
            }
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (need <= 0)
            {
                return {};
            }
            std::string out(static_cast<size_t>(need), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &out[0], need, nullptr, nullptr);
            return out;
        }
        fs::path path_from_utf8(const std::string& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int need = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            if (need <= 0)
            {
                return {};
            }
            std::wstring wide(static_cast<size_t>(need), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], need);
            return fs::path(wide);
        }

        constexpr double kEpsilon = 1e-9;

        std::mt19937& rng()
        {
            static std::mt19937 engine{std::random_device{}()};
            return engine;
        }

        double clamp01(double v)
        {
            return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        }

        // 把 a..b 按 t（0..1）插值；缩放按倍数线性插值。
        BoneEdit blend_edit(const BoneEdit& a, const BoneEdit& b, double t)
        {
            BoneEdit out;
            for (int i = 0; i < 3; ++i)
            {
                out.axis[i] = a.axis[i] + (b.axis[i] - a.axis[i]) * t;
                out.turn[i] = a.turn[i] + (b.turn[i] - a.turn[i]) * t;
                out.shift[i] = a.shift[i] + (b.shift[i] - a.shift[i]) * t;
            }
            out.spread = b.spread;
            return out;
        }

        // 节点长度：驱动节点是 duration，等待节点是实际等待时间（随机按下限+上限的中点估）。
        double node_seconds(const AnimNode& node)
        {
            if (node.kind == AnimNodeKind::Move)
            {
                return std::max(0.001, node.duration);
            }
            if (node.random)
            {
                const double lo = std::min(node.wait_min, node.wait_max);
                const double hi = std::max(node.wait_min, node.wait_max);
                return std::max(0.0, (lo + hi) * 0.5);
            }
            return std::max(0.0, node.wait);
        }

        std::string json_escape(const std::string& text)
        {
            std::string out;
            out.reserve(text.size() + 2);
            for (const char c : text)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
                }
            }
            return out;
        }

        std::string number_text(double value)
        {
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "%.4f", value);
            std::string text = buffer;
            while (text.size() > 1 && text.back() == '0')
            {
                text.pop_back();
            }
            if (!text.empty() && text.back() == '.')
            {
                text.pop_back();
            }
            if (text == "-0")
            {
                text = "0";
            }
            return text;
        }

        std::string array_text(const double values[3])
        {
            return "[" + number_text(values[0]) + ", " + number_text(values[1]) + ", " + number_text(values[2]) + "]";
        }

        bool read_number(const pbzjson::JsonValue* value, double& out)
        {
            if (value == nullptr || value->kind != pbzjson::JsonValue::Kind::Number)
            {
                return false;
            }
            out = value->number;
            return true;
        }

        void read_triple(const pbzjson::JsonValue* value, double out[3], const double fallback[3])
        {
            for (int i = 0; i < 3; ++i)
            {
                out[i] = fallback[i];
            }
            if (value == nullptr || value->kind != pbzjson::JsonValue::Kind::Array || value->items.size() != 3)
            {
                return;
            }
            for (int i = 0; i < 3; ++i)
            {
                if (value->items[static_cast<size_t>(i)].kind == pbzjson::JsonValue::Kind::Number)
                {
                    out[i] = value->items[static_cast<size_t>(i)].number;
                }
            }
        }

        bool read_bool(const pbzjson::JsonValue* value, bool fallback)
        {
            return value != nullptr && value->kind == pbzjson::JsonValue::Kind::Bool ? value->boolean : fallback;
        }

        std::string read_string(const pbzjson::JsonValue* value, const std::string& fallback = {})
        {
            return value != nullptr && value->kind == pbzjson::JsonValue::Kind::String ? value->text : fallback;
        }

        bool parse_node(const pbzjson::JsonValue& value, AnimNode& out, std::string& message)
        {
            if (value.kind != pbzjson::JsonValue::Kind::Object)
            {
                message = "node is not an object";
                return false;
            }
            const std::string kind = read_string(value.find("type"), "move");
            AnimNodeKind parsed = AnimNodeKind::Move;
            if (!node_kind_from_token(kind, parsed))
            {
                message = "unknown node type: " + kind;
                return false;
            }
            out = AnimNode{};
            out.kind = parsed;
            if (parsed == AnimNodeKind::Move)
            {
                double duration = out.duration;
                if (read_number(value.find("duration"), duration))
                {
                    out.duration = std::max(0.001, std::min(60.0, duration));
                }
                Ease ease = out.ease;
                if (!ease_from_token(read_string(value.find("ease"), "smooth"), ease))
                {
                    ease = out.ease;
                }
                out.ease = ease;
                const double one[3] = {1.0, 1.0, 1.0};
                const double zero[3] = {0.0, 0.0, 0.0};
                read_triple(value.find("scale"), out.target.axis, one);
                read_triple(value.find("rotate"), out.target.turn, zero);
                read_triple(value.find("move"), out.target.shift, zero);
                out.target.clamp();
            }
            else
            {
                out.random = read_bool(value.find("random"), read_string(value.find("mode"), "fixed") == "random");
                double wait = out.wait;
                if (read_number(value.find("wait"), wait))
                {
                    out.wait = std::max(0.0, std::min(600.0, wait));
                }
                double lo = out.wait_min;
                double hi = out.wait_max;
                read_number(value.find("min"), lo);
                read_number(value.find("max"), hi);
                out.wait_min = std::max(0.0, std::min(600.0, lo));
                out.wait_max = std::max(out.wait_min, std::min(600.0, hi));
            }
            return true;
        }

        bool parse_track(const pbzjson::JsonValue& value, AnimTrack& out, std::string& message)
        {
            if (value.kind != pbzjson::JsonValue::Kind::Object)
            {
                message = "track is not an object";
                return false;
            }
            out = AnimTrack{};
            const pbzjson::JsonValue* bones = value.find("bones");
            if (bones != nullptr && bones->kind == pbzjson::JsonValue::Kind::Array)
            {
                for (const pbzjson::JsonValue& bone : bones->items)
                {
                    if (bone.kind == pbzjson::JsonValue::Kind::String && !bone.text.empty())
                    {
                        out.bones.push_back(bone.text);
                    }
                }
            }
            Spread spread = out.spread;
            if (spread_from_token(read_string(value.find("children"), "scale_too"), spread))
            {
                out.spread = spread;
            }
            out.loop = read_bool(value.find("loop"), true);
            out.enabled = read_bool(value.find("enabled"), true);
            const pbzjson::JsonValue* nodes = value.find("nodes");
            if (nodes != nullptr && nodes->kind == pbzjson::JsonValue::Kind::Array)
            {
                for (const pbzjson::JsonValue& node_value : nodes->items)
                {
                    AnimNode node;
                    if (!parse_node(node_value, node, message))
                    {
                        return false;
                    }
                    out.nodes.push_back(std::move(node));
                }
            }
            return true;
        }
    } // namespace

    // ---------------------------------------------------------------- tokens
    const char* ease_token(Ease ease)
    {
        switch (ease)
        {
        case Ease::Linear: return "linear";
        case Ease::Smooth: return "smooth";
        case Ease::In: return "in";
        case Ease::Out: return "out";
        case Ease::InOut: return "inout";
        }
        return "smooth";
    }

    bool ease_from_token(std::string_view token, Ease& out)
    {
        const std::string folded = pbzjson::fold_case(token);
        if (folded == "linear")
        {
            out = Ease::Linear;
            return true;
        }
        if (folded == "smooth")
        {
            out = Ease::Smooth;
            return true;
        }
        if (folded == "in" || folded == "ease_in" || folded == "easein")
        {
            out = Ease::In;
            return true;
        }
        if (folded == "out" || folded == "ease_out" || folded == "easeout")
        {
            out = Ease::Out;
            return true;
        }
        if (folded == "inout" || folded == "ease_in_out" || folded == "easeinout")
        {
            out = Ease::InOut;
            return true;
        }
        return false;
    }

    double ease_curve(Ease ease, double t)
    {
        t = clamp01(t);
        switch (ease)
        {
        case Ease::Linear: return t;
        case Ease::Smooth: return t * t * (3.0 - 2.0 * t);
        case Ease::In: return t * t;
        case Ease::Out: return 1.0 - (1.0 - t) * (1.0 - t);
        case Ease::InOut: return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
        }
        return t;
    }

    const char* node_kind_token(AnimNodeKind kind)
    {
        return kind == AnimNodeKind::Delay ? "delay" : "move";
    }

    bool node_kind_from_token(std::string_view token, AnimNodeKind& out)
    {
        const std::string folded = pbzjson::fold_case(token);
        if (folded == "move" || folded == "drive")
        {
            out = AnimNodeKind::Move;
            return true;
        }
        if (folded == "delay" || folded == "wait")
        {
            out = AnimNodeKind::Delay;
            return true;
        }
        return false;
    }

    double anim_random01()
    {
        static std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(rng());
    }

    // -------------------------------------------------------------- runtime
    BoneEdit anim_track_advance(const AnimTrack& track, TrackRuntime& runtime, double dt)
    {
        if (!track.enabled || track.nodes.empty())
        {
            return BoneEdit{};
        }

        const int count = static_cast<int>(track.nodes.size());

        // 首次进入（或数据换了导致下标越界）：清空重来。
        if (runtime.node < 0 || runtime.node >= count)
        {
            runtime = TrackRuntime{};
            runtime.current = BoneEdit{};
            runtime.node = 0;
            runtime.elapsed = 0.0;
            const AnimNode& first = track.nodes[0];
            if (first.kind == AnimNodeKind::Move)
            {
                runtime.from = BoneEdit{};
            }
            else if (first.random)
            {
                const double lo = std::min(first.wait_min, first.wait_max);
                const double hi = std::max(first.wait_min, first.wait_max);
                runtime.wait = lo + (hi - lo) * anim_random01();
            }
            else
            {
                runtime.wait = std::max(0.0, first.wait);
            }
        }

        if (dt <= 0.0 || runtime.finished)
        {
            return runtime.current;
        }

        // 进入某个节点时把状态准备好：驱动节点记下起点，等待节点当场抽一次随机时间。
        auto enter = [&](int index) {
            runtime.node = index;
            runtime.elapsed = 0.0;
            const AnimNode& node = track.nodes[static_cast<size_t>(index)];
            if (node.kind == AnimNodeKind::Move)
            {
                runtime.from = runtime.current;
            }
            else if (node.random)
            {
                const double lo = std::min(node.wait_min, node.wait_max);
                const double hi = std::max(node.wait_min, node.wait_max);
                runtime.wait = lo + (hi - lo) * anim_random01();
            }
            else
            {
                runtime.wait = std::max(0.0, node.wait);
            }
        };

        double remaining = dt;
        int guard = 0;
        while (remaining > kEpsilon && guard++ < 4096)
        {
            const AnimNode& node = track.nodes[static_cast<size_t>(runtime.node)];
            const double limit = node.kind == AnimNodeKind::Move ? std::max(0.001, node.duration) : std::max(0.0, runtime.wait);

            if (node.kind == AnimNodeKind::Move)
            {
                const double t = clamp01((runtime.elapsed + remaining) / limit);
                runtime.current = blend_edit(runtime.from, node.target, ease_curve(node.ease, t));
            }

            if (runtime.elapsed + remaining < limit)
            {
                runtime.elapsed += remaining;
                break; // 还在这个节点里
            }

            // 这个节点走完了：把剩余时间交给下一个节点。
            remaining -= std::max(0.0, limit - runtime.elapsed);
            runtime.elapsed = limit;
            if (node.kind == AnimNodeKind::Move)
            {
                runtime.current = node.target;
            }

            const int next = runtime.node + 1;
            if (next >= count)
            {
                if (!track.loop)
                {
                    runtime.finished = true;
                    break;
                }
                enter(0);
            }
            else
            {
                enter(next);
            }
        }
        return runtime.current;
    }

    double anim_track_cycle(const AnimTrack& track)
    {
        double total = 0.0;
        for (const AnimNode& node : track.nodes)
        {
            total += node_seconds(node);
        }
        return total;
    }

    void anim_track_progress(const AnimTrack& track, const TrackRuntime& runtime, int& node, double& progress)
    {
        node = runtime.node;
        progress = 0.0;
        if (node < 0 || node >= static_cast<int>(track.nodes.size()))
        {
            return;
        }
        const AnimNode& current = track.nodes[static_cast<size_t>(node)];
        const double limit = node_seconds(current);
        progress = limit <= kEpsilon ? 1.0 : clamp01(runtime.elapsed / limit);
    }

    // --------------------------------------------------------------- JSON
    std::string anim_to_json(const Animation& animation)
    {
        std::string out;
        out += "{\n";
        out += "  \"format\": \"UUEPBS animation\",\n";
        out += "  \"version\": 1,\n";
        out += "  \"name\": \"" + json_escape(animation.name) + "\",\n";
        out += std::string("  \"enabled\": ") + (animation.enabled ? "true" : "false") + ",\n";
        out += "  \"tracks\": [";
        if (animation.tracks.empty())
        {
            out += "]\n";
        }
        else
        {
            out += "\n";
            for (size_t t = 0; t < animation.tracks.size(); ++t)
            {
                const AnimTrack& track = animation.tracks[t];
                out += "    {\n";
                out += "      \"bones\": [";
                for (size_t b = 0; b < track.bones.size(); ++b)
                {
                    if (b > 0)
                    {
                        out += ", ";
                    }
                    out += "\"" + json_escape(track.bones[b]) + "\"";
                }
                out += "],\n";
                out += std::string("      \"children\": \"") + spread_token(track.spread) + "\",\n";
                out += std::string("      \"loop\": ") + (track.loop ? "true" : "false") + ",\n";
                out += std::string("      \"enabled\": ") + (track.enabled ? "true" : "false") + ",\n";
                out += "      \"nodes\": [";
                if (track.nodes.empty())
                {
                    out += "]\n";
                }
                else
                {
                    out += "\n";
                    for (size_t n = 0; n < track.nodes.size(); ++n)
                    {
                        const AnimNode& node = track.nodes[n];
                        out += "        {";
                        if (node.kind == AnimNodeKind::Move)
                        {
                            out += "\"type\": \"move\", \"duration\": " + number_text(node.duration);
                            out += ", \"ease\": \"" + std::string(ease_token(node.ease)) + "\"";
                            out += ", \"scale\": " + array_text(node.target.axis);
                            out += ", \"rotate\": " + array_text(node.target.turn);
                            out += ", \"move\": " + array_text(node.target.shift);
                        }
                        else
                        {
                            const char* mode = node.random ? "random" : "fixed";
                            out += std::string("\"type\": \"delay\", \"mode\": \"") + mode + "\"";
                            out += ", \"wait\": " + number_text(node.wait);
                            out += ", \"min\": " + number_text(node.wait_min);
                            out += ", \"max\": " + number_text(node.wait_max);
                        }
                        out += n + 1 < track.nodes.size() ? "},\n" : "}\n";
                    }
                    out += "      ]\n";
                }
                out += t + 1 < animation.tracks.size() ? "    },\n" : "    }\n";
            }
            out += "  ]\n";
        }
        out += "}\n";
        return out;
    }

    bool anim_from_json(const std::string& text, Animation& out, std::string& message)
    {
        pbzjson::JsonValue root;
        pbzjson::JsonReader reader(text);
        std::string error;
        if (!reader.read(root, error))
        {
            message = error;
            return false;
        }
        if (root.kind != pbzjson::JsonValue::Kind::Object)
        {
            message = "root is not an object";
            return false;
        }
        Animation animation;
        animation.name = read_string(root.find("name"));
        animation.enabled = read_bool(root.find("enabled"), true);
        const pbzjson::JsonValue* tracks = root.find("tracks");
        if (tracks != nullptr && tracks->kind == pbzjson::JsonValue::Kind::Array)
        {
            for (const pbzjson::JsonValue& track_value : tracks->items)
            {
                AnimTrack track;
                if (!parse_track(track_value, track, message))
                {
                    return false;
                }
                animation.tracks.push_back(std::move(track));
            }
        }
        out = std::move(animation);
        return true;
    }

    // --------------------------------------------------------------- files
    std::filesystem::path anim_folder(const std::filesystem::path& preset_folder)
    {
        return preset_folder / L"Animations";
    }

    std::vector<std::string> anim_list(const std::filesystem::path& preset_folder)
    {
        std::vector<std::string> names;
        if (preset_folder.empty())
        {
            return names;
        }
        std::error_code ec;
        const fs::path folder = anim_folder(preset_folder);
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
            {
                continue;
            }
            if (it->path().extension() != L".json")
            {
                continue;
            }
            names.push_back(path_to_utf8(it->path().stem()));
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool anim_load(const std::filesystem::path& preset_folder, const std::string& name, Animation& out, std::string& message)
    {
        if (preset_folder.empty())
        {
            message = "preset folder is not ready";
            return false;
        }
        const std::string clean = clean_name(name);
        if (clean.empty())
        {
            message = "invalid animation name";
            return false;
        }
        const fs::path file = anim_folder(preset_folder) / path_from_utf8(clean + ".json");
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            message = "cannot open " + path_to_utf8(file);
            return false;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!anim_from_json(text, out, message))
        {
            message = path_to_utf8(file) + ": " + message;
            return false;
        }
        if (out.name.empty())
        {
            out.name = clean;
        }
        return true;
    }

    bool anim_save(const std::filesystem::path& preset_folder, const Animation& animation, std::string& message)
    {
        if (preset_folder.empty())
        {
            message = "preset folder is not ready";
            return false;
        }
        const std::string clean = clean_name(animation.name);
        if (clean.empty())
        {
            message = "invalid animation name";
            return false;
        }
        std::error_code ec;
        const fs::path folder = anim_folder(preset_folder);
        fs::create_directories(folder, ec);
        if (ec)
        {
            message = "cannot create " + path_to_utf8(folder);
            return false;
        }
        const fs::path file = folder / path_from_utf8(clean + ".json");
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            message = "cannot write " + path_to_utf8(file);
            return false;
        }
        const std::string text = anim_to_json(animation);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out)
        {
            message = "write failed: " + path_to_utf8(file);
            return false;
        }
        message = path_to_utf8(file);
        return true;
    }

    bool anim_remove(const std::filesystem::path& preset_folder, const std::string& name, std::string& message)
    {
        if (preset_folder.empty())
        {
            message = "preset folder is not ready";
            return false;
        }
        const std::string clean = clean_name(name);
        if (clean.empty())
        {
            message = "invalid animation name";
            return false;
        }
        std::error_code ec;
        const fs::path file = anim_folder(preset_folder) / path_from_utf8(clean + ".json");
        const bool removed = fs::remove(file, ec);
        if (ec || !removed)
        {
            message = "cannot delete " + path_to_utf8(file);
            return false;
        }
        message = path_to_utf8(file);
        return true;
    }

    std::vector<Animation> anim_load_all(const std::filesystem::path& preset_folder, std::vector<std::string>* errors)
    {
        std::vector<Animation> animations;
        for (const std::string& name : anim_list(preset_folder))
        {
            Animation animation;
            std::string message;
            if (anim_load(preset_folder, name, animation, message))
            {
                animations.push_back(std::move(animation));
            }
            else if (errors != nullptr)
            {
                errors->push_back(message);
            }
        }
        return animations;
    }
} // namespace NpcAnim
