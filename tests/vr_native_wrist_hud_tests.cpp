#include "../L4D2VR/vr_native_wrist_hud.h"
#include <cstdio>
#include <vector>

using namespace l4d2vr_native_wrist_hud;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Inside(const Rect& r, int w, int h)
{
    return r.x >= 0 && r.y >= 0 && r.Right() <= w && r.Bottom() <= h;
}

int main()
{
    // Screen -> texture mapping, including a texture that differs from the VGUI screen.
    {
        const Rect r = ScreenToTexture({ 100, 50, 30, 20 }, 1280, 720, 1280, 720);
        CHECK(r.x == 100 && r.y == 50 && r.w == 30 && r.h == 20);
        const Rect half = ScreenToTexture({ 100, 50, 30, 20 }, 1280, 720, 640, 360);
        CHECK(half.x == 50 && half.y == 25 && half.w == 15 && half.h == 10);
        const Rect clipped = ScreenToTexture({ 1270, 700, 40, 40 }, 1280, 720, 1280, 720);
        CHECK(clipped.w == 10 && clipped.h == 20);
        CHECK(ScreenToTexture({ 2000, 0, 10, 10 }, 1280, 720, 1280, 720).Empty());
        CHECK(Union(Rect{}, Rect{ 1, 2, 3, 4 }).w == 3);
        const Rect u = Union(Rect{ 0, 0, 10, 10 }, Rect{ 20, 5, 10, 10 });
        CHECK(u.x == 0 && u.y == 0 && u.w == 30 && u.h == 15);
    }

    // Squeezed root panel: a 1280-wide viewport painted into a 640-wide root.
    {
        const PanelMap m = PanelMap::Between(Rect{ 0, 0, 1280, 720 }, Rect{ 0, 0, 640, 720 });
        const Rect r = m.Apply(Rect{ 1040, 584, 240, 127 });
        CHECK(r.x == 520 && r.y == 584 && r.w == 120 && r.h == 127);
        const Rect same = PanelMap{}.Apply(Rect{ 5, 6, 7, 8 });
        CHECK(same.x == 5 && same.y == 6 && same.w == 7 && same.h == 8);
        const Element a{ Rect{ 10, 10, 20, 10 }, 40, 10 };
        const Element b{ Rect{ 40, 10, 10, 10 }, 20, 10 };
        const Element u = Union(a, b);
        CHECK(u.src.w == 40 && u.w == 80 && u.h == 10);
    }

    // Left wrist at 1280x720 (proportional scale 1.5): own block large on top,
    // three teammates below, each bar then name, smaller than the own block.
    {
        // The HUD is painted squeezed to half width: crops are half as wide as laid out.
        const Element own{ Rect{ 553, 630, 83, 63 }, 166, 63 };
        std::vector<Teammate> mates;
        for (int i = 0; i < 3; ++i)
            mates.push_back({ Element{ Rect{ 150 + i * 103, 600, 70, 20 }, 140, 20 }, Element{ Rect{ 140 + i * 103, 622, 90, 18 }, 180, 18 } });
        const Composition c = ComposeLeft(own, mates, kLeftTexW, kLeftTexH);
        CHECK(c.blits.size() == 7);
        CHECK(c.usedW > 0 && c.usedW <= kLeftTexW && c.usedH > 0 && c.usedH <= kLeftTexH);
        for (const Blit& b : c.blits)
            CHECK(Inside(b.dst, kLeftTexW, kLeftTexH) && !b.dst.Empty());
        // Own block first, spanning the width, scaled up.
        CHECK(c.blits[0].src.x == own.src.x && c.blits[0].dst.y < c.blits[1].dst.y);
        CHECK(c.blits[0].dst.w > own.w * 2);
        // The laid-out proportions are restored (2:1 squeeze undone).
        const double ownAspect = (double)c.blits[0].dst.w / c.blits[0].dst.h;
        CHECK(ownAspect > 166.0 / 63.0 * 0.97 && ownAspect < 166.0 / 63.0 * 1.03);
        // A teammate's bar is clearly smaller than the own block and sits above its name.
        const double ownScale = (double)c.blits[0].dst.w / own.w;
        const double mateScale = (double)c.blits[1].dst.w / mates[0].bar.w;
        CHECK(c.blits[1].src.w == mates[0].bar.src.w);
        CHECK(mateScale < ownScale * 0.7);
        CHECK(c.blits[1].src.y == mates[0].bar.src.y && c.blits[2].src.y == mates[0].name.src.y);
        CHECK(c.blits[2].dst.y >= c.blits[1].dst.Bottom());
        // Rows do not overlap.
        for (size_t i = 1; i + 1 < c.blits.size(); ++i)
            CHECK(c.blits[i + 1].dst.y >= c.blits[i].dst.Bottom());
    }

    // Many teammates shrink to fit the texture height instead of overflowing.
    {
        std::vector<Teammate> mates(12, Teammate{ Element{ Rect{ 0, 0, 140, 20 } }, Element{ Rect{ 0, 20, 180, 18 } } });
        const Composition c = ComposeLeft(Element{ Rect{ 0, 0, 166, 63 } }, mates, kLeftTexW, kLeftTexH);
        CHECK(c.blits.size() == 25);
        CHECK(c.usedH <= kLeftTexH);
        for (const Blit& b : c.blits)
            CHECK(Inside(b.dst, kLeftTexW, kLeftTexH));
    }

    // Only teammates (own panel hidden, e.g. dead) or nothing at all.
    {
        std::vector<Teammate> mates{ Teammate{ Element{ Rect{ 0, 0, 140, 20 } }, Element{} } };
        const Composition c = ComposeLeft(Element{}, mates, kLeftTexW, kLeftTexH);
        CHECK(c.blits.size() == 1 && c.blits[0].dst.w <= kLeftTexW);
        CHECK(ComposeLeft(Element{}, {}, kLeftTexW, kLeftTexH).Empty());
    }

    // Right wrist: the weapon panel keeps its aspect and fits the texture.
    {
        const Element panel{ Rect{ 566, 225, 75, 262 }, 150, 262 };
        const Composition c = ComposeRight(panel, kRightTexW, kRightTexH);
        CHECK(c.blits.size() == 1);
        const Rect& d = c.blits[0].dst;
        CHECK(Inside(d, kRightTexW, kRightTexH));
        const double aspectIn = (double)panel.h / panel.w; // laid-out aspect, not the squeezed crop
        const double aspectOut = (double)d.h / d.w;
        CHECK(aspectOut > aspectIn * 0.98 && aspectOut < aspectIn * 1.02);
        CHECK(ComposeRight(Element{}, kRightTexW, kRightTexH).Empty());
        CHECK(UsedHeightMeters(0.1f, c) > 0.1f);
    }

    // Weapon panel trimmed to its painted content.
    {
        const int w = 20, h = 40;
        std::vector<uint8_t> px((size_t)w * h * 4, 0);
        for (int y = 10; y < 30; ++y)
            for (int x = 12; x < 18; ++x)
                px[((size_t)y * w + x) * 4 + 3] = 255;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        CHECK(AlphaBounds(px.data(), w, h, w * 4, 8, 0, u0, v0, u1, v1));
        CHECK(u0 == 12.0f / w && v0 == 10.0f / h && u1 == 18.0f / w && v1 == 30.0f / h);
        std::vector<uint8_t> empty((size_t)w * h * 4, 0);
        CHECK(!AlphaBounds(empty.data(), w, h, w * 4, 8, 0, u0, v0, u1, v1));

        const Element panel{ Rect{ 566, 225, 75, 300 }, 150, 300 };
        const Element t = Trim(panel, 0.4f, 0.2f, 1.0f, 0.7f);
        CHECK(t.src.x == 566 + 30 && t.src.y == 225 + 60 && t.src.w == 45 && t.src.h == 150);
        CHECK(t.w == 90 && t.h == 150);
        CHECK(Trim(panel, 0.0f, 0.0f, 0.0f, 0.0f).src.w == panel.src.w);
    }

    if (g_failures == 0)
        std::printf("vr_native_wrist_hud_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
