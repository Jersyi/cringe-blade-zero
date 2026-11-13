// UUEPBS - CJK fallback font.
//
// ImGui's built-in font has no Chinese glyphs, and the skin fonts usually do not either, so
// Chinese UI text rendered with them shows up as boxes. This merges a system CJK font
// (Microsoft YaHei / SimHei / SimSun) into the font that was added last, which gives ImGui a
// fallback source for the missing glyphs (ImGui 1.92 rasterises glyphs on demand).
#pragma once

namespace uuepbs::ui
{
    // Adds the CJK fallback as a merged font source into the most recently added font.
    // `size_pixels` should match the font it merges into. Returns true when a usable font file
    // was found (false leaves the UI ASCII-only, which is harmless).
    bool merge_cjk_fallback(float size_pixels);
} // namespace uuepbs::ui
