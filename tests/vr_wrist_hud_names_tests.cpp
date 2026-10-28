#include "../L4D2VR/vr_wrist_hud_names.h"
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace l4d2vr_wrist_hud;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Stand-in for GDI: ASCII is 0.6em wide, every other code point 1em, plus the 2px outline.
static int FakeMeasure(std::string_view text, int fontPx)
{
    int w = 0;
    for (std::size_t i = 0; i < text.size();)
    {
        const std::size_t n = Utf8SequenceLength(text, i);
        w += ((unsigned char)text[i] < 0x80) ? (fontPx * 6) / 10 : fontPx;
        i += n;
    }
    return text.empty() ? 0 : w + 2;
}

static std::string Join(const std::vector<std::string>& lines, const char* sep)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i > 0)
            out += sep;
        out += lines[i];
    }
    return out;
}

static bool IsWholeUtf8(const std::string& s)
{
    if (s.empty())
        return true;
    if (((unsigned char)s[0] & 0xC0) == 0x80)
        return false;
    return Utf8SafePrefixLength(s, s.size()) == s.size();
}

static std::string Repeat(const char* piece, int count)
{
    std::string out;
    for (int i = 0; i < count; ++i)
        out += piece;
    return out;
}

static void TestUtf8Sequences()
{
    CHECK(Utf8SequenceLength("A", 0) == 1);
    CHECK(Utf8SequenceLength("\xC3\xA9", 0) == 2);          // e-acute
    CHECK(Utf8SequenceLength("\xE4\xB8\xAD", 0) == 3);      // CJK
    CHECK(Utf8SequenceLength("\xF0\x9F\x98\x80", 0) == 4);  // emoji
    CHECK(Utf8SequenceLength(std::string_view("\xE4\xB8", 2), 0) == 1); // cut off
    CHECK(Utf8SequenceLength("\x80", 0) == 1);              // stray continuation
    CHECK(Utf8SequenceLength("\xC0\x80", 0) == 1);          // overlong lead
    CHECK(Utf8SequenceLength("", 0) == 0);

    const std::string mixed = "ab\xE4\xB8\xAD"; // "ab" + one CJK char
    CHECK(Utf8SafePrefixLength(mixed, 4) == 2);
    CHECK(Utf8SafePrefixLength(mixed, 5) == 5);
    CHECK(Utf8SafePrefixLength(mixed, 99) == 5);
    CHECK(Utf8SafePrefixLength(mixed, 0) == 0);
}

static void TestBotNamesStayCompact()
{
    const std::vector<std::string> names{ "ZOEY", "FRANCIS", "ROCHELLE" };
    const TeamPanelGeometry g{};
    const TeamPanelLayout layout = LayoutTeamPanel(names, g, FakeMeasure);
    CHECK(!layout.expanded);
    CHECK(layout.texHeight == g.baseTexHeight);
    CHECK(layout.rows.size() == names.size());
    for (std::size_t i = 0; i < layout.rows.size(); ++i)
    {
        const TeamRowLayout& row = layout.rows[i];
        CHECK(row.fit.mode == NameMode::Inline);
        CHECK(row.fit.lines.size() == 1);
        CHECK(row.fit.lines[0] == names[i]);
        CHECK(row.fit.fontPx >= g.minFontPx && row.fit.fontPx <= g.maxFontPx);
        CHECK(FakeMeasure(row.fit.lines[0], row.fit.fontPx) <= row.nameMaxW);
        CHECK(row.nameX + row.nameMaxW <= row.barX);
        CHECK(row.barX == TeamBarX(g));
    }
    // ROCHELLE does not fit at 12px beside the bar, so it shrinks rather than moving.
    CHECK(layout.rows[2].fit.fontPx < g.maxFontPx);
}

static void TestShrinkComesBeforeGrowing()
{
    const NameFitLimits limits{ 50, 200, 12, 9 };
    // 7 ASCII chars: 51px at 12px, 44px at 11px -> stays inline at 11px.
    const NameFit fit = FitName("FRANCIS", limits, FakeMeasure);
    CHECK(fit.mode == NameMode::Inline);
    CHECK(fit.fontPx == 11);
}

static void TestLongSteamNameGrowsPanel()
{
    const std::string longName = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"; // 32 chars
    const std::vector<std::string> names{ "ZOEY", longName, "LOUIS" };
    const TeamPanelGeometry g{};
    const TeamPanelLayout layout = LayoutTeamPanel(names, g, FakeMeasure);
    CHECK(layout.expanded);
    CHECK(layout.texHeight > g.baseTexHeight);
    CHECK(layout.texHeight <= g.maxTexHeight);
    CHECK(layout.texHeight % g.heightStep == 0);
    CHECK(layout.rows.size() == 3);

    // Short names get their full size back in the wide rows.
    CHECK(layout.rows[0].fit.mode == NameMode::Inline);
    CHECK(layout.rows[0].fit.fontPx == g.maxFontPx);

    // The long one is complete, never cut.
    const TeamRowLayout& longRow = layout.rows[1];
    CHECK(Join(longRow.fit.lines, "") == longName);
    CHECK(longRow.fit.mode != NameMode::Inline || FakeMeasure(longName, longRow.fit.fontPx) <= longRow.nameMaxW);

    int prevBottom = g.expandedTop - 1;
    for (const TeamRowLayout& row : layout.rows)
    {
        CHECK(row.barX == TeamBarX(g));             // bars stay in one column
        CHECK(row.nameY > prevBottom - 1);          // rows do not overlap
        CHECK(row.barY >= prevBottom);
        if (row.fit.mode != NameMode::Inline)
            CHECK(row.nameY + (int)row.fit.lines.size() * row.lineStep <= row.barY); // name above bar
        for (const std::string& line : row.fit.lines)
            CHECK(FakeMeasure(line, row.fit.fontPx) <= row.nameMaxW);
        prevBottom = row.barY + g.barH;
    }
    CHECK(prevBottom <= layout.texHeight - 8); // inside the background frame
}

static void TestWideUtf8NameWrapsWithoutSplitting()
{
    const std::string cjk = Repeat("\xE4\xB8\xAD", 32); // 32 CJK chars, 96 bytes
    const std::vector<std::string> names{ cjk };
    const TeamPanelGeometry g{};
    const TeamPanelLayout layout = LayoutTeamPanel(names, g, FakeMeasure);
    CHECK(layout.expanded);
    CHECK(layout.rows.size() == 1);
    const TeamRowLayout& row = layout.rows[0];
    CHECK(row.fit.mode == NameMode::Wrapped);
    CHECK(row.fit.fontPx == g.minFontPx);
    CHECK(row.fit.lines.size() == 2);
    CHECK(Join(row.fit.lines, "") == cjk);
    for (const std::string& line : row.fit.lines)
    {
        CHECK(IsWholeUtf8(line));
        CHECK(FakeMeasure(line, row.fit.fontPx) <= row.nameMaxW);
    }
}

static void TestWrapPrefersSpaces()
{
    const std::string text = "THE QUICK BROWN FOX JUMPS";
    const std::vector<std::string> lines = WrapUtf8(text, 60, 10, FakeMeasure);
    CHECK(lines.size() >= 2);
    CHECK(Join(lines, " ") == text);
    for (const std::string& line : lines)
    {
        CHECK(!line.empty());
        CHECK(line.front() != ' ' && line.back() != ' ');
        CHECK(FakeMeasure(line, 10) <= 60);
    }
}

static void TestWrapNeverDropsText()
{
    // A glyph wider than the line still gets its own line.
    const std::vector<std::string> narrow = WrapUtf8("\xE4\xB8\xAD\xE4\xB8\xAD", 5, 12, FakeMeasure);
    CHECK(narrow.size() == 2);
    CHECK(Join(narrow, "") == "\xE4\xB8\xAD\xE4\xB8\xAD");

    // Malformed bytes are carried through one at a time.
    const std::string junk = "AB\xFF\xFE" "CD\xE4\xB8";
    const std::vector<std::string> junkLines = WrapUtf8(junk, 20, 10, FakeMeasure);
    CHECK(Join(junkLines, "") == junk);

    CHECK(WrapUtf8("", 100, 10, FakeMeasure).empty());
    CHECK(WrapUtf8("   ", 100, 10, FakeMeasure).empty());
}

static void TestEmptyListKeepsBasePanel()
{
    const TeamPanelGeometry g{};
    const TeamPanelLayout layout = LayoutTeamPanel({}, g, FakeMeasure);
    CHECK(!layout.expanded);
    CHECK(layout.rows.empty());
    CHECK(layout.texHeight == g.baseTexHeight);
}

static void TestExtraHeight()
{
    CHECK(ExtraPanelHeightMeters(0.1f, 256, 128, 128) == 0.0f);
    CHECK(ExtraPanelHeightMeters(0.1f, 256, 100, 128) == 0.0f);
    const float extra = ExtraPanelHeightMeters(0.1f, 256, 192, 128);
    CHECK(extra > 0.0249f && extra < 0.0251f);
}

int main()
{
    TestUtf8Sequences();
    TestBotNamesStayCompact();
    TestShrinkComesBeforeGrowing();
    TestLongSteamNameGrowsPanel();
    TestWideUtf8NameWrapsWithoutSplitting();
    TestWrapPrefersSpaces();
    TestWrapNeverDropsText();
    TestEmptyListKeepsBasePanel();
    TestExtraHeight();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vr_wrist_hud_names: all tests passed\n");
    return 0;
}
