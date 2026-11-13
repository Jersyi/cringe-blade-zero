#include "npc_body_preset.h"

#include "json.hpp"
// The port named its copy of UUEPBS's JSON reader pbzjson: it is uuepbs::json (src/uuepbs/core/json.hpp).
namespace pbzjson { using uuepbs::json::JsonValue; using uuepbs::json::JsonReader; using uuepbs::fold_case; }

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>

#include <windows.h>

namespace NpcBodyPreset
{
    namespace
    {
        constexpr const wchar_t* kFolderName = L"UUEPBS Presets";
        constexpr const char* kSessionName = "_last_session";

        std::string ToNarrow(const std::wstring& text)
        {
            if (text.empty())
            {
                return std::string();
            }
            const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (need <= 0)
            {
                return std::string();
            }
            std::string out(static_cast<size_t>(need), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need, nullptr, nullptr);
            return out;
        }

        std::wstring ToWide(const std::string& text)
        {
            if (text.empty())
            {
                return std::wstring();
            }
            const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            if (need <= 0)
            {
                return std::wstring();
            }
            std::wstring out(static_cast<size_t>(need), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
            return out;
        }

        std::string json_escape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            out.push_back('"');
            for (const char c : s)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out.push_back(c);
                    }
                }
            }
            out.push_back('"');
            return out;
        }

        // 最短的能还原到 4 位小数的写法：1.25 而不是 1.2500，1 而不是 1.0000。
        std::string json_number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            std::string s = buf;
            while (!s.empty() && s.back() == '0')
            {
                s.pop_back();
            }
            if (!s.empty() && s.back() == '.')
            {
                s.pop_back();
            }
            return s == "-0" ? "0" : s;
        }

        std::wstring file_for(const std::string& clean)
        {
            if (!Ready())
            {
                return std::wstring();
            }
            return Folder() + L"\\" + ToWide(clean) + L".json";
        }

        std::wstring legacy_file_for(const std::string& clean)
        {
            if (!Ready())
            {
                return std::wstring();
            }
            return Folder() + L"\\" + ToWide(clean) + L".rbs";
        }

        bool read_file(const std::wstring& path, std::string& text)
        {
            std::ifstream in(path.c_str(), std::ios::binary);
            if (!in)
            {
                return false;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
            return true;
        }

        std::string trim(std::string_view s)
        {
            size_t b = 0;
            size_t e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
            {
                ++b;
            }
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
            {
                --e;
            }
            return std::string(s.substr(b, e - b));
        }
    } // namespace

    const std::wstring& Folder()
    {
        static std::wstring folder;
        static bool resolved = false;
        if (resolved)
        {
            return folder;
        }
        resolved = true;
        wchar_t path[MAX_PATH] = {};
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&Folder), &self))
        {
            return folder;
        }
        if (GetModuleFileNameW(self, path, MAX_PATH) == 0)
        {
            return folder;
        }
        std::wstring text(path);
        const size_t slash = text.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return folder;
        }
        folder = text.substr(0, slash + 1) + kFolderName;
        return folder;
    }

    bool Ready()
    {
        if (Folder().empty())
        {
            return false;
        }
        const DWORD attrs = GetFileAttributesW(Folder().c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::string CleanName(const std::string& raw)
    {
        const std::string trimmed = trim(raw);
        std::string out;
        out.reserve(trimmed.size());
        for (const char c : trimmed)
        {
            switch (c)
            {
            case '\\':
            case '/':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                out.push_back('_');
                break;
            default:
                out.push_back(c);
            }
        }
        while (!out.empty() && (out.front() == ' ' || out.front() == '.'))
        {
            out.erase(out.begin());
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        {
            out.pop_back();
        }
        if (out.size() > 64)
        {
            out.resize(64);
        }
        return out;
    }

    std::vector<std::string> List()
    {
        std::vector<std::string> names;
        if (!Ready())
        {
            return names;
        }
        WIN32_FIND_DATAW entry = {};
        const std::wstring pattern = Folder() + L"\\*";
        HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE)
        {
            return names;
        }
        do
        {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                continue;
            }
            std::wstring leaf(entry.cFileName);
            const size_t dot = leaf.find_last_of(L'.');
            if (dot == std::wstring::npos)
            {
                continue;
            }
            std::wstring ext = leaf.substr(dot);
            for (wchar_t& c : ext)
            {
                c = static_cast<wchar_t>(towlower(c));
            }
            if (ext != L".json" && ext != L".rbs")
            {
                continue;
            }
            const std::string stem = ToNarrow(leaf.substr(0, dot));
            if (stem.empty() || stem == kSessionName)
            {
                continue;
            }
            if (std::find(names.begin(), names.end(), stem) == names.end())
            {
                names.push_back(stem);
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            return NpcBodyRig::FoldCase(a) < NpcBodyRig::FoldCase(b);
        });
        return names;
    }

    bool Exists(const std::string& name)
    {
        const std::string clean = CleanName(name);
        if (clean.empty() || !Ready())
        {
            return false;
        }
        return GetFileAttributesW(file_for(clean).c_str()) != INVALID_FILE_ATTRIBUTES ||
               GetFileAttributesW(legacy_file_for(clean).c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::string Serialize(const Book& book)
    {
        std::string out;
        out += "{\n";
        out += "  \"format\": \"UUEPBS preset\",\n";
        out += "  \"version\": 2,\n";
        out += "  \"help\": \"length = along the bone (X), width = Y, depth = Z (1 = unchanged). "
               "rotate = degrees about X, Y, Z. move = cm along X, Y, Z. children: scale_too | keep_size | this_bone_only\",\n";
        out += "  \"bones\": {";
        bool first = true;
        for (const auto& [key, edit] : book)
        {
            if (edit.neutral())
            {
                continue;
            }
            out += first ? "\n" : ",\n";
            first = false;
            auto triple = [](const double v[3]) {
                return "[" + json_number(v[0]) + ", " + json_number(v[1]) + ", " + json_number(v[2]) + "]";
            };
            out += "    " + json_escape(key) + ": { \"length\": " + json_number(edit.axis[0]) + ", \"width\": " + json_number(edit.axis[1]) +
                   ", \"depth\": " + json_number(edit.axis[2]);
            if (edit.has_turn())
            {
                out += ", \"rotate\": " + triple(edit.turn);
            }
            if (edit.has_shift())
            {
                out += ", \"move\": " + triple(edit.shift);
            }
            out += ", \"children\": \"" + std::string(NpcBodyRig::SpreadToken(edit.spread)) + "\" }";
        }
        out += first ? "}" : "\n  }";
        out += "\n}\n";
        return out;
    }

    bool Parse(const std::string& text, Book& out, std::string& message)
    {
        out.clear();
        std::string_view body = text;
        if (body.size() >= 3 && body.substr(0, 3) == "\xEF\xBB\xBF")
        {
            body.remove_prefix(3);
        }
        const std::string head = trim(body.substr(0, std::min<size_t>(64, body.size())));
        if (head.empty() || head[0] != '{')
        {
            message = "不是 JSON 预设（只认 {\"format\":\"UUEPBS preset\"...}）";
            return false;
        }

        pbzjson::JsonValue root;
        std::string error;
        if (!pbzjson::JsonReader(body).read(root, error))
        {
            message = "JSON 解析失败：" + error;
            return false;
        }
        if (root.kind != pbzjson::JsonValue::Kind::Object)
        {
            message = "文件内容必须是一个 { ... } 对象";
            return false;
        }
        const pbzjson::JsonValue* bones = root.find("bones");
        if (!bones || bones->kind != pbzjson::JsonValue::Kind::Object)
        {
            message = "缺少 \"bones\": { ... } 段";
            return false;
        }

        int skipped = 0;
        for (const auto& [bone, v] : bones->members)
        {
            if (bone.empty() || v.kind != pbzjson::JsonValue::Kind::Object)
            {
                ++skipped;
                continue;
            }
            NpcBodyRig::BoneEdit edit;
            bool any = false;

            auto number_of = [&](std::initializer_list<const char*> names, double& target) {
                for (const char* n : names)
                {
                    const pbzjson::JsonValue* f = v.find(n);
                    if (f && f->kind == pbzjson::JsonValue::Kind::Number)
                    {
                        target = f->number;
                        any = true;
                        return;
                    }
                }
            };
            auto vector_of = [&](std::initializer_list<const char*> names, double (&target)[3]) {
                for (const char* n : names)
                {
                    const pbzjson::JsonValue* f = v.find(n);
                    if (!f)
                    {
                        continue;
                    }
                    if (f->kind == pbzjson::JsonValue::Kind::Array && f->items.size() == 3)
                    {
                        for (int a = 0; a < 3; ++a)
                        {
                            if (f->items[static_cast<size_t>(a)].kind == pbzjson::JsonValue::Kind::Number)
                            {
                                target[a] = f->items[static_cast<size_t>(a)].number;
                                any = true;
                            }
                        }
                    }
                    else if (f->kind == pbzjson::JsonValue::Kind::Object)
                    {
                        static const char* keys[3] = {"x", "y", "z"};
                        for (int a = 0; a < 3; ++a)
                        {
                            const pbzjson::JsonValue* c = f->find(keys[a]);
                            if (c && c->kind == pbzjson::JsonValue::Kind::Number)
                            {
                                target[a] = c->number;
                                any = true;
                            }
                        }
                    }
                    return;
                }
            };

            if (const pbzjson::JsonValue* sc = v.find("scale"))
            {
                if (sc->kind == pbzjson::JsonValue::Kind::Number)
                {
                    edit.axis[0] = edit.axis[1] = edit.axis[2] = sc->number;
                    any = true;
                }
                else if (sc->kind == pbzjson::JsonValue::Kind::Array && sc->items.size() == 3)
                {
                    for (int a = 0; a < 3; ++a)
                    {
                        if (sc->items[static_cast<size_t>(a)].kind == pbzjson::JsonValue::Kind::Number)
                        {
                            edit.axis[a] = sc->items[static_cast<size_t>(a)].number;
                            any = true;
                        }
                    }
                }
            }
            vector_of({"rotate", "rotation"}, edit.turn);
            vector_of({"move", "offset", "translate"}, edit.shift);
            number_of({"length", "x"}, edit.axis[0]);
            number_of({"width", "y"}, edit.axis[1]);
            number_of({"depth", "z"}, edit.axis[2]);

            if (const pbzjson::JsonValue* ch = v.find("children"))
            {
                NpcBodyRig::Spread spread = NpcBodyRig::Spread::Chain;
                if (ch->kind == pbzjson::JsonValue::Kind::String && NpcBodyRig::SpreadFromChildren(ch->text, spread))
                {
                    edit.spread = spread;
                }
            }
            if (!any)
            {
                ++skipped;
                continue;
            }
            edit.clamp();
            out[NpcBodyRig::FoldCase(bone)] = edit;
        }

        message = std::to_string(out.size()) + " 根骨";
        if (skipped > 0)
        {
            message += "，" + std::to_string(skipped) + " 项读不懂已跳过";
        }
        return true;
    }

    bool Save(const std::string& name, const Book& book, std::string& message)
    {
        const std::string clean = CleanName(name);
        if (clean.empty())
        {
            message = "先输入一个预设名字";
            return false;
        }
        if (Folder().empty())
        {
            message = "找不到 dll 所在目录，预设没法落盘";
            return false;
        }
        CreateDirectoryW(Folder().c_str(), nullptr);
        const std::wstring target = file_for(clean);
        const std::wstring temp = target + L".tmp";
        {
            std::ofstream out(temp.c_str(), std::ios::binary | std::ios::trunc);
            if (!out)
            {
                message = "写不进 " + ToNarrow(temp);
                return false;
            }
            const std::string body = Serialize(book);
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!out)
            {
                message = "写到 " + ToNarrow(temp) + " 时失败";
                return false;
            }
        }
        if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(temp.c_str());
            message = "替换不了 " + ToNarrow(target);
            return false;
        }
        DeleteFileW(legacy_file_for(clean).c_str());
        message = "已保存「" + clean + "」";
        return true;
    }

    bool Load(const std::string& name, Book& out, std::string& message)
    {
        const std::string clean = CleanName(name);
        if (clean.empty() || Folder().empty())
        {
            message = "预设名不能用";
            return false;
        }
        std::wstring path = file_for(clean);
        std::string text;
        if (!read_file(path, text))
        {
            path = legacy_file_for(clean);
            if (!read_file(path, text))
            {
                message = "找不到预设「" + name + "」";
                return false;
            }
        }
        std::string detail;
        if (!Parse(text, out, detail))
        {
            message = "「" + clean + "」读不了：" + detail;
            return false;
        }
        // 预设里的骨名换成真实骨骼的下标，找不到的那些直接丢掉（别人的预设可能来自别的模型）。
        message = "已载入「" + clean + "」：" + detail;
        return true;
    }

    bool Remove(const std::string& name, std::string& message)
    {
        const std::string clean = CleanName(name);
        if (clean.empty())
        {
            message = "预设名不能用";
            return false;
        }
        const bool json_gone = DeleteFileW(file_for(clean).c_str()) != 0;
        const bool legacy_gone = DeleteFileW(legacy_file_for(clean).c_str()) != 0;
        if (!json_gone && !legacy_gone)
        {
            message = "删不掉「" + name + "」";
            return false;
        }
        message = "已删除「" + clean + "」";
        return true;
    }
} // namespace NpcBodyPreset
