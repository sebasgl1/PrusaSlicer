// Brick Layers: interlocking perimeters for FFF prints.
//
// C++ port of "Brick Layers" by Everson Siqueira (Geek Detour),
// https://github.com/GeekDetour/BrickLayers (GPL-3.0-or-later), based on
// bricklayers.py v0.2.1-10-g6409588. The port follows the original algorithm
// line by line, so its output matches the Python script byte for byte.
//
// Copyright (C) 2025 Everson Siqueira, Geek Detour (original algorithm)
// This file is part of a PrusaSlicer fork and is distributed under the
// GNU Affero General Public License v3 (as permitted by GPLv3 section 13).
//
// The transformation converts the rectangular arrangement of perimeter beads
// into a hexagonal one: every other inner perimeter loop is deferred and
// printed half a layer higher, interlocking consecutive layers.
//
// This header has no dependencies outside the C++ standard library, so the
// algorithm can be built and tested on its own (see tests/bricklayers).

#ifndef slic3r_GCode_BrickLayers_hpp_
#define slic3r_GCode_BrickLayers_hpp_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r::GCode::BrickLayers {

struct Params
{
    // Extrusion multiplier applied to all inner perimeters that get reprocessed.
    double extrusion_multiplier = 1.05;
    // First layer (1-based, counted by ;LAYER_CHANGE markers) to apply Brick Layers to.
    int start_at_layer = 3;
    // Layers that are left untouched.
    std::vector<int> layers_to_ignore;
    // Prepend a ";==" comment block describing the settings used.
    bool enable_header = false;
    std::vector<std::pair<std::string, std::string>> header_info;
};

// Formats a double exactly like Python's repr()/str() (shortest round-trip).
std::string py_repr(double value);

// Streaming G-code transformer. Feed it text in arbitrary chunks; transformed
// G-code is appended to `out` one layer at a time. Call finish() at the end.
class Processor
{
public:
    explicit Processor(Params params);
    ~Processor();
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    void feed(std::string_view chunk, std::string& out);
    void finish(std::string& out);

    // Convenience: transform a whole G-code text at once.
    static std::string process(std::string_view gcode, const Params& params);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string           m_partial_line;
    bool                  m_finished = false;
};

} // namespace Slic3r::GCode::BrickLayers

#endif // slic3r_GCode_BrickLayers_hpp_
