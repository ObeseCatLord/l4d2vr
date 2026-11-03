#pragma once
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "vr_hand_mirror.h"

// The model's own LEFT hand for an item held in the physical left hand.
//
// vr_hand_mirror.h solves the arms in a mirror world and reflects every bone by
// M, which leaves the RIGHT arm's mesh mirrored onto the left controller (drawn
// with the winding flipped): a right hand model used as a left hand. The
// survivor arms have a real left arm, so the reflected pose is moved onto it:
// every bone b goes to its left/right counterpart b' (L <-> R by name, centre
// bones to themselves) as
//     out[b'] = mirrored[b] * F_b,   F_b = inverse(Mrest * rest[b]) * rest[b'],
// where rest[] are the model-space bind poses and Mrest the reflection across
// the model's own left/right symmetry plane (found from the paired bones). For
// the bind pose placed anywhere (mirrored[b] = M W rest[b] with M = W Mrest W^-1)
// this gives exactly W rest[b']; in general it puts each counterpart bone on the
// mirror image of its partner with a proper (det +1) frame, so the mesh is drawn
// normally: the left arm holds the item and the right arm is the free hand.
//
// std-only so the algebra is unit-tested offline.
namespace l4d2vr_left_hand
{
    using Mat34 = l4d2vr_hand_mirror::Mat34;

    inline std::string Lower(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    inline bool IsSep(char c) { return c == '_' || c == '.' || c == ' ' || c == '-'; }

    // -1 left, +1 right, 0 centre / unknown, from a Source bone name
    // ("ValveBiped.Bip01_L_Hand", "ValveBiped.L_Wrist", "..._Right").
    inline int SideOfName(const std::string& name)
    {
        const std::string n = Lower(name);
        if (n.find("left") != std::string::npos)
            return -1;
        if (n.find("right") != std::string::npos)
            return 1;
        for (size_t i = 0; i < n.size(); ++i)
        {
            if (n[i] != 'l' && n[i] != 'r')
                continue;
            const bool before = i == 0 || IsSep(n[i - 1]);
            const bool after = i + 1 == n.size() || IsSep(n[i + 1]);
            if (before && after)
                return n[i] == 'l' ? -1 : 1;
        }
        return 0;
    }

    // The lower-case name of the other side's bone ("" for a centre bone).
    inline std::string CounterpartName(const std::string& name)
    {
        std::string n = Lower(name);
        size_t at = n.find("left");
        if (at != std::string::npos)
            return n.replace(at, 4, "right");
        at = n.find("right");
        if (at != std::string::npos)
            return n.replace(at, 5, "left");
        for (size_t i = 0; i < n.size(); ++i)
        {
            if (n[i] != 'l' && n[i] != 'r')
                continue;
            const bool before = i == 0 || IsSep(n[i - 1]);
            const bool after = i + 1 == n.size() || IsSep(n[i + 1]);
            if (before && after)
            {
                n[i] = n[i] == 'l' ? 'r' : 'l';
                return n;
            }
        }
        return std::string();
    }

    // Inverse of a rigid transform whose 3x3 part is orthonormal (rotation or
    // reflection): transpose and back-rotated translation.
    inline Mat34 InverseOrthonormal(const Mat34& a)
    {
        Mat34 out{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                out.m[r][c] = a.m[c][r];
        for (int r = 0; r < 3; ++r)
            out.m[r][3] = -(out.m[r][0] * a.m[0][3] + out.m[r][1] * a.m[1][3] + out.m[r][2] * a.m[2][3]);
        return out;
    }

    // Model-space bind poses from parent-relative ones (parents come first in
    // Source's bone order; a bad parent index makes the bone a root).
    inline std::vector<Mat34> ComposeModelSpace(const std::vector<Mat34>& local, const std::vector<int>& parents)
    {
        std::vector<Mat34> model(local.size());
        for (size_t b = 0; b < local.size(); ++b)
        {
            const int p = b < parents.size() ? parents[b] : -1;
            model[b] = (p >= 0 && static_cast<size_t>(p) < b) ? l4d2vr_hand_mirror::Mul(model[static_cast<size_t>(p)], local[b])
                                                              : local[b];
        }
        return model;
    }

    struct Remap
    {
        bool valid = false;
        int pairs = 0;
        std::vector<int> counterpart;   // b -> b' (b itself for centre / unpaired bones)
        std::vector<Mat34> flip;        // F_b
        Mat34 restMirror{};             // Mrest
    };

    // Builds the remap from the bone names and model-space bind poses. Needs
    // at least `minPairs` left/right pairs whose bind positions are mirror
    // images across one plane (within `toleranceUnits`).
    inline bool BuildRemap(const std::vector<std::string>& names, const std::vector<Mat34>& restModel, Remap& out,
        int minPairs = 3, float toleranceUnits = 0.75f)
    {
        out = Remap{};
        const size_t n = names.size();
        if (n == 0 || restModel.size() != n)
            return false;
        std::unordered_map<std::string, int> byName;
        for (size_t b = 0; b < n; ++b)
            byName.emplace(Lower(names[b]), static_cast<int>(b));

        out.counterpart.assign(n, -1);
        double normal[3] = { 0.0, 0.0, 0.0 };
        double mid[3] = { 0.0, 0.0, 0.0 };
        int pairs = 0;
        for (size_t b = 0; b < n; ++b)
        {
            out.counterpart[b] = static_cast<int>(b);
            if (SideOfName(names[b]) != 1)
                continue;
            const auto found = byName.find(CounterpartName(names[b]));
            if (found == byName.end() || found->second == static_cast<int>(b))
                continue;
            const Mat34& r = restModel[b];
            const Mat34& l = restModel[static_cast<size_t>(found->second)];
            const double d[3] = { r.m[0][3] - l.m[0][3], r.m[1][3] - l.m[1][3], r.m[2][3] - l.m[2][3] };
            const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 0.5)
                continue;   // both sides at one point (a parked helper): no plane information
            for (int i = 0; i < 3; ++i)
            {
                normal[i] += d[i] / len;
                mid[i] += 0.5 * (r.m[i][3] + l.m[i][3]);
            }
            ++pairs;
        }
        // Counterparts both ways (left bones get theirs from the right pass).
        for (size_t b = 0; b < n; ++b)
        {
            const int side = SideOfName(names[b]);
            if (side == 0)
                continue;
            const auto found = byName.find(CounterpartName(names[b]));
            if (found != byName.end())
                out.counterpart[b] = found->second;
        }
        // Only symmetric pairings (b -> c and c -> b); anything else maps to itself.
        for (size_t b = 0; b < n; ++b)
        {
            const int c = out.counterpart[b];
            if (c != static_cast<int>(b) && out.counterpart[static_cast<size_t>(c)] != static_cast<int>(b))
                out.counterpart[b] = static_cast<int>(b);
        }
        if (pairs < minPairs)
            return false;
        const float point[3] = { static_cast<float>(mid[0] / pairs), static_cast<float>(mid[1] / pairs),
            static_cast<float>(mid[2] / pairs) };
        const float nrm[3] = { static_cast<float>(normal[0]), static_cast<float>(normal[1]), static_cast<float>(normal[2]) };
        if (!l4d2vr_hand_mirror::Reflection(point, nrm, out.restMirror))
            return false;

        // The pairs must really be mirror images across that plane.
        for (size_t b = 0; b < n; ++b)
        {
            const int cp = out.counterpart[b];
            if (cp == static_cast<int>(b) || SideOfName(names[b]) != 1)
                continue;
            const float p[3] = { restModel[b].m[0][3], restModel[b].m[1][3], restModel[b].m[2][3] };
            float q[3]{};
            l4d2vr_hand_mirror::TransformPoint(out.restMirror, p, q);
            const Mat34& l = restModel[static_cast<size_t>(cp)];
            const float dx = q[0] - l.m[0][3], dy = q[1] - l.m[1][3], dz = q[2] - l.m[2][3];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > toleranceUnits)
                return false;
        }

        out.flip.resize(n);
        for (size_t b = 0; b < n; ++b)
        {
            const Mat34 mirroredRest = l4d2vr_hand_mirror::Mul(out.restMirror, restModel[b]);
            out.flip[b] = l4d2vr_hand_mirror::Mul(InverseOrthonormal(mirroredRest),
                restModel[static_cast<size_t>(out.counterpart[b])]);
        }
        out.pairs = pairs;
        out.valid = true;
        return true;
    }

    // mirrored[] = every bone reflected by a world mirror (det -1 frames);
    // out[] = proper frames with each pose moved onto the counterpart bone.
    inline void Apply(const Remap& remap, const Mat34* mirrored, Mat34* out, int numBones)
    {
        if (!remap.valid || !mirrored || !out || numBones <= 0 ||
            static_cast<size_t>(numBones) > remap.counterpart.size())
            return;
        for (int b = 0; b < numBones; ++b)
        {
            const int cp = remap.counterpart[static_cast<size_t>(b)];
            if (cp < 0 || cp >= numBones)
                continue;
            out[cp] = l4d2vr_hand_mirror::Mul(mirrored[b], remap.flip[static_cast<size_t>(b)]);
        }
    }
}
