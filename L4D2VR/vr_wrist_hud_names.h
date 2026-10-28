#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Name layout for the left wrist HUD teammate list. Names are never cut:
// each one first shrinks to fit beside its health bar, then moves onto its
// own line above the bar (the panel grows taller), and only as a last resort
// wraps onto more lines. Text width comes from a caller-supplied measure
// function, int(std::string_view utf8, int fontPx), so this stays std-only.
namespace l4d2vr_wrist_hud
{
    // Byte length of the UTF-8 sequence starting at s[i]. Malformed or cut-off
    // sequences count as one byte so callers always make progress.
    inline std::size_t Utf8SequenceLength(std::string_view s, std::size_t i)
    {
        if (i >= s.size())
            return 0;
        const unsigned char lead = (unsigned char)s[i];
        std::size_t n = 1;
        if (lead >= 0xC2 && lead <= 0xDF) n = 2;
        else if ((lead & 0xF0) == 0xE0) n = 3;
        else if (lead >= 0xF0 && lead <= 0xF4) n = 4;
        if (n == 1 || i + n > s.size())
            return 1;
        for (std::size_t j = 1; j < n; ++j)
        {
            if (((unsigned char)s[i + j] & 0xC0) != 0x80)
                return 1;
        }
        return n;
    }

    // Longest prefix of at most maxBytes that does not split a UTF-8 sequence.
    inline std::size_t Utf8SafePrefixLength(std::string_view s, std::size_t maxBytes)
    {
        std::size_t i = 0;
        while (i < s.size())
        {
            const std::size_t n = Utf8SequenceLength(s, i);
            if (i + n > maxBytes)
                break;
            i += n;
        }
        return i;
    }

    enum class NameMode
    {
        Inline,  // name and bar on one line
        Stacked, // name on its own line above the bar
        Wrapped  // name over several lines above the bar
    };

    struct NameFit
    {
        NameMode mode = NameMode::Inline;
        int fontPx = 0;
        std::vector<std::string> lines;
    };

    struct NameFitLimits
    {
        int inlineWidthPx = 0; // room beside the bar
        int fullWidthPx = 0;   // room on a line of its own
        int maxFontPx = 12;
        int minFontPx = 9;     // smallest size that stays readable on the wrist
    };

    // Greedy line breaking at fontPx: break at the last space that fits,
    // otherwise between code points. A single code point wider than the line
    // still gets a line of its own, so nothing is ever dropped.
    template <typename Measure>
    std::vector<std::string> WrapUtf8(std::string_view text, int widthPx, int fontPx, Measure&& measure)
    {
        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (pos < text.size())
        {
            while (pos < text.size() && text[pos] == ' ')
                ++pos;
            if (pos >= text.size())
                break;

            std::size_t end = pos;
            std::size_t spaceBreak = std::string_view::npos;
            while (end < text.size())
            {
                const std::size_t next = end + Utf8SequenceLength(text, end);
                if (measure(text.substr(pos, next - pos), fontPx) > widthPx)
                    break;
                if (text[end] == ' ')
                    spaceBreak = end;
                end = next;
            }

            if (end < text.size())
            {
                if (end == pos)
                    end = pos + Utf8SequenceLength(text, pos);
                else if (spaceBreak != std::string_view::npos && spaceBreak > pos && text[end] != ' ')
                    end = spaceBreak;
            }

            std::size_t lineEnd = end;
            while (lineEnd > pos && text[lineEnd - 1] == ' ')
                --lineEnd;
            lines.emplace_back(text.substr(pos, lineEnd - pos));
            pos = end;
        }
        return lines;
    }

    template <typename Measure>
    NameFit FitName(std::string_view name, const NameFitLimits& limits, Measure&& measure)
    {
        NameFit fit;
        const int maxPx = (limits.maxFontPx > limits.minFontPx) ? limits.maxFontPx : limits.minFontPx;
        const int minPx = limits.minFontPx;

        for (int px = maxPx; px >= minPx; --px)
        {
            if (measure(name, px) <= limits.inlineWidthPx)
            {
                fit.mode = NameMode::Inline;
                fit.fontPx = px;
                fit.lines.emplace_back(name);
                return fit;
            }
        }
        for (int px = maxPx; px >= minPx; --px)
        {
            if (measure(name, px) <= limits.fullWidthPx)
            {
                fit.mode = NameMode::Stacked;
                fit.fontPx = px;
                fit.lines.emplace_back(name);
                return fit;
            }
        }
        fit.mode = NameMode::Wrapped;
        fit.fontPx = minPx;
        fit.lines = WrapUtf8(name, limits.fullWidthPx, minPx, measure);
        if (fit.lines.size() == 1)
            fit.mode = NameMode::Stacked;
        return fit;
    }

    // Texture-space geometry of the wrist panel (pixels). The defaults match
    // the 256x128 panel: compact rows sit in the right column beside the big
    // health number; expanded rows run full width under the item row.
    struct TeamPanelGeometry
    {
        int texWidth = 256;
        int baseTexHeight = 128;
        int maxTexHeight = 512;
        int heightStep = 8;     // round grown heights to limit texture churn

        int barW = 62;
        int barH = 10;
        int rightPad = 10;      // bar's distance from the right edge
        int nameBarGap = 6;

        int compactNameX = 124;
        int compactTop = 18;
        int compactStride = 24;

        int expandedLeft = 18;
        int expandedTop = 114;
        int rowGap = 6;
        int lineGap = 2;
        int bottomPad = 14;

        int maxFontPx = 12;
        int minFontPx = 9;
    };

    struct TeamRowLayout
    {
        NameFit fit;
        int nameX = 0;
        int nameY = 0;
        int nameMaxW = 0;
        int lineStep = 0;
        int barX = 0;
        int barY = 0;
    };

    struct TeamPanelLayout
    {
        bool expanded = false;
        int texHeight = 128;
        std::vector<TeamRowLayout> rows;
    };

    inline int TeamBarX(const TeamPanelGeometry& g)
    {
        return g.texWidth - g.rightPad - g.barW;
    }

    template <typename Measure>
    TeamPanelLayout LayoutTeamPanel(const std::vector<std::string>& names, const TeamPanelGeometry& g, Measure&& measure)
    {
        TeamPanelLayout out;
        out.texHeight = g.baseTexHeight;
        const int barX = TeamBarX(g);

        // 1) Compact: every name fits beside its bar in the right column.
        const int compactW = barX - g.compactNameX - g.nameBarGap;
        bool compact = true;
        std::vector<NameFit> fits;
        fits.reserve(names.size());
        for (const std::string& name : names)
        {
            NameFitLimits limits{ compactW, compactW, g.maxFontPx, g.minFontPx };
            NameFit fit = FitName(name, limits, measure);
            if (fit.mode != NameMode::Inline)
            {
                compact = false;
                break;
            }
            fits.push_back(std::move(fit));
        }
        if (compact)
        {
            for (std::size_t i = 0; i < fits.size(); ++i)
            {
                TeamRowLayout row;
                row.fit = std::move(fits[i]);
                row.barX = barX;
                row.barY = g.compactTop + (int)i * g.compactStride;
                row.nameX = g.compactNameX;
                row.nameMaxW = compactW;
                row.nameY = row.barY + (g.barH - row.fit.fontPx) / 2;
                row.lineStep = row.fit.fontPx + g.lineGap;
                out.rows.push_back(std::move(row));
            }
            return out;
        }

        // 2) Expanded: full-width rows under the main stats; the panel grows.
        out.expanded = true;
        const int inlineW = barX - g.expandedLeft - g.nameBarGap;
        const int fullW = g.texWidth - g.expandedLeft - g.rightPad;
        int y = g.expandedTop;
        for (const std::string& name : names)
        {
            TeamRowLayout row;
            row.fit = FitName(name, NameFitLimits{ inlineW, fullW, g.maxFontPx, g.minFontPx }, measure);
            row.barX = barX;
            row.nameX = g.expandedLeft;
            row.lineStep = row.fit.fontPx + g.lineGap;
            int rowH = 0;
            if (row.fit.mode == NameMode::Inline)
            {
                rowH = (row.fit.fontPx > g.barH) ? row.fit.fontPx : g.barH;
                row.nameMaxW = inlineW;
                row.nameY = y + (rowH - row.fit.fontPx) / 2;
                row.barY = y + (rowH - g.barH) / 2;
            }
            else
            {
                const int textH = (int)row.fit.lines.size() * row.lineStep;
                row.nameMaxW = fullW;
                row.nameY = y;
                row.barY = y + textH;
                rowH = textH + g.barH;
            }
            out.rows.push_back(std::move(row));
            y += rowH + g.rowGap;
        }

        int h = y - g.rowGap + g.bottomPad;
        if (g.heightStep > 1)
            h = ((h + g.heightStep - 1) / g.heightStep) * g.heightStep;
        if (h < g.baseTexHeight)
            h = g.baseTexHeight;
        if (h > g.maxTexHeight)
            h = g.maxTexHeight;
        out.texHeight = h;
        return out;
    }

    // Extra panel height in meters beyond the base layout, for an overlay of
    // widthMeters across texWidth pixels with square texels.
    inline float ExtraPanelHeightMeters(float widthMeters, int texWidth, int texHeight, int baseTexHeight)
    {
        if (texWidth <= 0 || texHeight <= baseTexHeight)
            return 0.0f;
        return widthMeters * (float)(texHeight - baseTexHeight) / (float)texWidth;
    }
}
