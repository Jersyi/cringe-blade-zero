#include "fonts.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <imgui.h>

#include <string>

namespace uuepbs::ui
{
    namespace
    {
        std::string system_font_path(const wchar_t* file)
        {
            wchar_t windir[MAX_PATH]{};
            if (GetWindowsDirectoryW(windir, MAX_PATH) == 0)
            {
                return {};
            }
            const std::wstring path = std::wstring(windir) + L"\\Fonts\\" + file;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                return {};
            }
            const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (n <= 0)
            {
                return {};
            }
            std::string utf8(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), n, nullptr, nullptr);
            utf8.resize(std::char_traits<char>::length(utf8.c_str()));
            return utf8;
        }
    } // namespace

    bool merge_cjk_fallback(float size_pixels)
    {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        if (atlas == nullptr || atlas->Fonts.Size == 0)
        {
            return false;
        }
        // Preferred order: Microsoft YaHei UI, YaHei, SimHei, SimSun (all ship with Windows in
        // the Chinese language pack; msyh.ttc also exists on most non-Chinese installs).
        static const wchar_t* const candidates[] = {L"msyh.ttc", L"msyhbd.ttc", L"simhei.ttf", L"simsun.ttc"};
        for (const wchar_t* file : candidates)
        {
            const std::string path = system_font_path(file);
            if (path.empty())
            {
                continue;
            }
            ImFontConfig cfg;
            cfg.MergeMode = true; // fallback glyphs for the font added just before this call
            cfg.FontNo = 0;       // first face of a .ttc collection
            cfg.PixelSnapH = true;
            if (atlas->AddFontFromFileTTF(path.c_str(), size_pixels, &cfg) != nullptr)
            {
                return true;
            }
        }
        return false;
    }
} // namespace uuepbs::ui
