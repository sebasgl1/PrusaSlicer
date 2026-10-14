// Brick Layers: interlocking perimeters for FFF prints.
// C++ port of bricklayers.py by Everson Siqueira (Geek Detour).
// See BrickLayers.hpp for license and attribution.
//
// Porting notes
// -------------
// * The structure mirrors the Python classes (GCodeSimulator, GCodeFeature,
//   LoopNode, BrickLayersProcessor) so both can be compared side by side.
// * Python objects are reference types. Lines, loops and perimeter groups are
//   shared and mutated after being stored elsewhere (e.g. a travel line that is
//   already in the output buffer gets rewritten later), so they are held by
//   shared_ptr here to keep exactly the same aliasing behaviour.
// * Python's `None` is modelled with std::optional / nullptr. Where the script
//   interpolates a possibly-None value into an f-string, the literal "None" is
//   produced, as Python would. Where it emits a None line, nothing is output.
// * Number formatting and parsing are locale independent (to_chars/from_chars),
//   which matters inside a GUI application running in e.g. a Spanish locale.

#include "BrickLayers.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace Slic3r::GCode::BrickLayers {

// ---------------------------------------------------------------------------
// Python compatible string helpers
// ---------------------------------------------------------------------------

namespace {

bool is_py_space(char c)
{
    // ASCII subset of what Python's str.strip()/str.split() treat as whitespace.
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
           (c >= '\x1c' && c <= '\x1f');
}

std::string_view py_strip(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && is_py_space(s[b])) ++b;
    while (e > b && is_py_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::string_view> py_split(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_py_space(s[i])) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && !is_py_space(s[j])) ++j;
        out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Python float(): strict parse, surrounding whitespace allowed.
double py_float(std::string_view s)
{
    std::string_view t = py_strip(s);
    std::string_view num = t;
    if (!num.empty() && num.front() == '+') num.remove_prefix(1);
    double v = 0;
    if (!num.empty() && num.front() != '+') {
        auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), v);
        if (ec == std::errc() && ptr == num.data() + num.size())
            return v;
        if (ec == std::errc::result_out_of_range && ptr == num.data() + num.size()) {
            // Python returns inf / 0.0 instead of failing.
            return std::strtod(std::string(num).c_str(), nullptr);
        }
    }
    throw std::runtime_error("Brick Layers: could not convert string to float: '" + std::string(s) + "'");
}

// f"{v:.{prec}f}"
std::string fmt_fixed(double v, int prec)
{
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    char buf[512];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, prec);
    if (ec != std::errc()) throw std::runtime_error("Brick Layers: number formatting failed");
    return std::string(buf, ptr);
}

// f"{int(v)}"
std::string fmt_int(double v)
{
    if (!std::isfinite(v)) throw std::runtime_error("Brick Layers: cannot convert non-finite float to integer");
    double t = std::trunc(v);
    if (std::fabs(t) < 9.2e18) return std::to_string(static_cast<long long>(t));
    return fmt_fixed(t, 0);
}

} // namespace

std::string py_repr(double value)
{
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";

    // Shortest round-trip digits in scientific form: [-]d[.ddd]e[+-]XX
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::scientific);
    if (ec != std::errc()) throw std::runtime_error("Brick Layers: number formatting failed");
    std::string_view sci(buf, ptr - buf);

    bool negative = false;
    if (!sci.empty() && sci.front() == '-') {
        negative = true;
        sci.remove_prefix(1);
    }
    size_t epos = sci.find('e');
    std::string_view mantissa = sci.substr(0, epos);
    int exponent = 0;
    std::from_chars(sci.data() + epos + 1 + (sci[epos + 1] == '+' ? 1 : 0), sci.data() + sci.size(), exponent);

    std::string digits;
    for (char c : mantissa)
        if (c != '.') digits.push_back(c);
    // Strip trailing zeros (to_chars shortest never adds them, except for zero).
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    std::string out = negative ? "-" : "";
    if (exponent >= -4 && exponent < 16) {
        // Fixed notation, always with a decimal point.
        if (exponent >= 0) {
            const size_t int_len = size_t(exponent) + 1;
            if (digits.size() <= int_len) {
                out += digits;
                out.append(int_len - digits.size(), '0');
                out += ".0";
            } else {
                out += digits.substr(0, int_len);
                out += '.';
                out += digits.substr(int_len);
            }
        } else {
            out += "0.";
            out.append(size_t(-exponent - 1), '0');
            out += digits;
        }
    } else {
        // Scientific notation, Python style: 1e+16, 1.5e-05
        out += digits[0];
        if (digits.size() > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'e';
        out += exponent < 0 ? '-' : '+';
        int ae = std::abs(exponent);
        if (ae < 10) out += '0';
        out += std::to_string(ae);
    }
    return out;
}

namespace {

// "None" when unset, as Python's f-string interpolation would print it.
std::string py_str(const std::optional<std::string>& s) { return s ? *s : std::string("None"); }

// re.sub(r'<axis>[-+]?[0-9]*\.?[0-9]+', replacement, s)
std::string regex_sub_axis(const std::string& s, char axis, const std::string& replacement)
{
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string out;
    out.reserve(s.size() + 16);
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == axis) {
            size_t j = i + 1;
            if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
            size_t d1 = j;
            while (d1 < s.size() && is_digit(s[d1])) ++d1;
            size_t end = std::string::npos;
            if (d1 < s.size() && s[d1] == '.' && d1 + 1 < s.size() && is_digit(s[d1 + 1])) {
                size_t d2 = d1 + 1;
                while (d2 < s.size() && is_digit(s[d2])) ++d2;
                end = d2;
            } else if (d1 > j) {
                end = d1;
            }
            if (end != std::string::npos) {
                out += replacement;
                i = end;
                continue;
            }
        }
        out += s[i];
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct ObjectEntry
{
    std::string name;
};

struct GCodeState
{
    double x = 0, y = 0, z = 0, e = 0, f = 0, retracted = 0, width = 0;
    bool   absolute_positioning   = true;
    bool   relative_extrusion     = false;
    bool   is_moving              = false;
    bool   is_extruding           = false;
    bool   is_retracting          = false;
    bool   just_started_extruding = false;
    bool   just_stopped_extruding = false;
};

struct Point
{
    double x, y;
};

template<class A, class B> double distance_between_points(const A& s_from, const B& s_to)
{
    const double dx = s_to.x - s_from.x;
    const double dy = s_to.y - s_from.y;
    return std::sqrt(dx * dx + dy * dy);
}

template<class A, class B> Point point_along_line_forward(const A& p_from, const B& p_to, double desired_distance)
{
    const double dx          = p_to.x - p_from.x;
    const double dy          = p_to.y - p_from.y;
    const double full_length = std::sqrt(dx * dx + dy * dy);
    if (full_length <= 0.0)
        return Point{p_from.x, p_from.y};
    const double fraction = desired_distance / full_length;
    return Point{p_from.x + dx * fraction, p_from.y + dy * fraction};
}

template<class A, class B> Point point_along_line_backward(const A& from_pos, const B& to_pos, double distance)
{
    const double d0     = from_pos.x - to_pos.x;
    const double d1     = from_pos.y - to_pos.y;
    const double length = std::sqrt(std::pow(d0, 2.0) + std::pow(d1, 2.0));
    if (length < 1e-6)
        return Point{to_pos.x, to_pos.y};
    const double scale = distance / length;
    return Point{to_pos.x + d0 * scale, to_pos.y + d1 * scale};
}

struct GCodeLine
{
    // Unset gcode == a Python `None` line: it produces no output.
    std::optional<std::string> gcode;
    std::optional<GCodeState>  previous;
    std::optional<GCodeState>  current;
    const ObjectEntry*         object = nullptr;
    int                        looporder        = 0;
    int                        concentric_group = 0;

    void update_xy(double new_x, double new_y)
    {
        if (!gcode) return;
        std::string g = regex_sub_axis(*gcode, 'X', "X" + py_repr(new_x));
        gcode         = regex_sub_axis(g, 'Y', "Y" + py_repr(new_y));
    }
};

using LineP      = std::shared_ptr<GCodeLine>;
using Loop       = std::vector<LineP>;
using LoopP      = std::shared_ptr<Loop>;
using Perimeter  = std::vector<LoopP>;
using PerimeterP = std::shared_ptr<Perimeter>;

LineP from_gcode(std::string gcode)
{
    auto l   = std::make_shared<GCodeLine>();
    l->gcode = std::move(gcode);
    return l;
}

LineP from_gcode(const std::optional<std::string>& gcode)
{
    auto l   = std::make_shared<GCodeLine>();
    l->gcode = gcode;
    return l;
}

struct GCodeStateBBox
{
    double min_x = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();

    void compute(const GCodeState& state)
    {
        const double x = state.x, y = state.y;
        if (min_x == std::numeric_limits<double>::infinity()) {
            min_x = x - 0.1;
            max_x = x + 0.1;
            min_y = y - 0.1;
            max_y = y + 0.1;
        } else {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }

    bool contains(const GCodeStateBBox& o) const
    {
        return min_x <= o.min_x && max_x >= o.max_x && min_y <= o.min_y && max_y >= o.max_y;
    }
};

// ---------------------------------------------------------------------------
// GCodeSimulator
// ---------------------------------------------------------------------------

struct GCodeSimulator
{
    double x = 0, y = 0, z = 0, e = 0, f = 0, retracted = 0, width = 0;
    bool   absolute_positioning   = true;
    bool   relative_extrusion     = false;
    bool   is_moving              = false;
    bool   is_extruding           = false;
    bool   is_retracting          = false;
    bool   just_started_extruding = false;
    bool   just_stopped_extruding = false;
    bool   moved_in_xy            = false;
    double travel_speed           = 0;
    double wipe_speed             = 0;
    double retraction_speed       = 0;
    double detraction_speed       = 0;
    double retraction_length      = 0;

    std::optional<std::string> const_width;

    void parse_gcode_line(std::string_view rawline)
    {
        static constexpr std::string_view DEF_WIDTHS[] = {";WIDTH:", "; LINE_WIDTH: "};

        const std::string_view stripline = py_strip(rawline);
        if (stripline.empty())
            return;

        just_stopped_extruding = false;

        std::string_view line = stripline.substr(0, stripline.find(';'));

        if (!line.empty()) {
            const auto        parts   = py_split(line);
            const std::string_view command = parts.front();

            if (command == "G0" || command == "G1" || command == "G2" || command == "G3") {
                const bool old_extruding = is_extruding;

                moved_in_xy            = false;
                is_extruding           = false;
                is_retracting          = false;
                just_stopped_extruding = false;

                const bool abs_pos = absolute_positioning;
                const bool rel_ext = relative_extrusion;

                const double old_x = x, old_y = y, old_z = z, old_e = e;
                double new_x = x, new_y = y, new_z = z;
                double new_e = e, new_f = f;
                bool   has_x = false, has_y = false, has_z = false, has_f = false;

                for (size_t i = 1; i < parts.size(); ++i) {
                    const std::string_view arg  = parts[i];
                    const char             axis = arg[0];
                    if (axis == 'I' || axis == 'J')
                        continue;
                    const double val = py_float(arg.substr(1));
                    if (axis == 'X') {
                        new_x = abs_pos ? val : (x + val);
                        has_x = true;
                    } else if (axis == 'Y') {
                        new_y = abs_pos ? val : (y + val);
                        has_y = true;
                    } else if (axis == 'Z') {
                        new_z = abs_pos ? val : (z + val);
                        has_z = true;
                    } else if (axis == 'E') {
                        new_e = rel_ext ? (e + val) : val;
                    } else if (axis == 'F') {
                        new_f = val;
                        has_f = true;
                    }
                }

                const bool x_move               = new_x != old_x;
                const bool y_move               = new_y != old_y;
                const bool z_move               = new_z != old_z;
                const bool had_a_movement_change = x_move || y_move || z_move;

                moved_in_xy = x_move || y_move;

                const bool just_feed_rate = has_f && !(has_x || has_y || has_z);

                if (has_x || has_y || has_z) {
                    x = new_x;
                    y = new_y;
                    z = new_z;
                }
                e = new_e;
                f = new_f;

                bool         extruding  = false;
                bool         retracting = false;
                const double extruded   = new_e - old_e;
                if (extruded > 0) {
                    extruding    = true;
                    is_extruding = true;
                } else if (extruded < 0) {
                    retracting    = true;
                    is_retracting = true;
                }

                retracted += extruded;
                if (retracted + 0.0001 > 0)
                    retracted = 0.0;

                if (had_a_movement_change && new_e == old_e && travel_speed < new_f)
                    travel_speed = new_f;

                if (!had_a_movement_change && retracting && std::fabs(extruded) > retraction_length)
                    retraction_length = std::fabs(extruded);

                if (had_a_movement_change)
                    is_moving = true;
                else if (extruded != 0 && !had_a_movement_change)
                    is_moving = false;

                if (!just_feed_rate) {
                    just_started_extruding = extruding && !old_extruding && had_a_movement_change;
                    if (old_extruding && !is_extruding)
                        just_stopped_extruding = true;
                }
            } else if (command == "G90") {
                absolute_positioning = true;
            } else if (command == "G91") {
                absolute_positioning = false;
            } else if (command == "M82") {
                relative_extrusion = false;
            } else if (command == "M83") {
                relative_extrusion = true;
            } else if (command == "G92") {
                for (size_t i = 1; i < parts.size(); ++i) {
                    const std::string_view arg  = parts[i];
                    const char             axis = static_cast<char>(std::toupper(static_cast<unsigned char>(arg[0])));
                    if (axis == 'X' || axis == 'Y' || axis == 'Z' || axis == 'E') {
                        const double val = py_float(arg.substr(1));
                        switch (axis) {
                        case 'X': x = val; break;
                        case 'Y': y = val; break;
                        case 'Z': z = val; break;
                        default: e = val; break;
                        }
                    }
                }
                if (e == 0)
                    retracted = 0;
            }
        } else {
            for (std::string_view prefix : DEF_WIDTHS) {
                if (starts_with(stripline, prefix)) {
                    width = py_float(stripline.substr(prefix.size()));
                    if (!const_width)
                        const_width = std::string(prefix);
                    break;
                }
            }
        }
    }

    GCodeState get_state() const
    {
        GCodeState s;
        s.x = x; s.y = y; s.z = z; s.e = e; s.f = f;
        s.retracted              = retracted;
        s.width                  = width;
        s.absolute_positioning   = absolute_positioning;
        s.relative_extrusion     = relative_extrusion;
        s.is_moving              = is_moving;
        s.is_extruding           = is_extruding;
        s.is_retracting          = is_retracting;
        s.just_started_extruding = just_started_extruding;
        s.just_stopped_extruding = just_stopped_extruding;
        return s;
    }
};

// ---------------------------------------------------------------------------
// GCodeFeature
// ---------------------------------------------------------------------------

struct GCodeFeature
{
    static constexpr std::string_view SANE_INNERPERIMETER    = "internal_perimeter";
    static constexpr std::string_view SANE_OUTERPERIMETER    = "external_perimeter";
    static constexpr std::string_view SANE_OVERHANGPERIMETER = "overhang_perimeter";

    int                layer        = 0;
    double             z            = 0.0;
    double             height       = 0.0;
    bool               layer_change = false;
    const ObjectEntry* current_object = nullptr;
    std::string        current_type;
    std::string        last_type;
    bool               overhang_perimeter               = false;
    bool               internal_perimeter               = false;
    bool               external_perimeter               = false;
    bool               justgotinside_internal_perimeter = false;
    bool               justleft_internal_perimeter      = false;
    bool               justgotinside_external_perimeter = false;
    bool               just_changed_type                = false;
    bool               wiping                           = false;
    bool               wipe_willfinish                  = false;
    bool               wipe_justfinished                = true;
    bool               capture_height                   = true;

    // Detected once from the G-code flavour being parsed.
    std::optional<std::string> internal_perimeter_type;
    std::optional<std::string> external_perimeter_type;
    std::optional<std::string> const_wipe_start;
    std::optional<std::string> const_wipe_end;
    std::optional<std::string> const_printingobject_start;
    std::optional<std::string> const_printingobject_stop;
    std::optional<std::string> const_layer_change;
    std::optional<std::string> const_layer_height;
    std::optional<std::string> const_layer_z;

    std::unordered_map<std::string, std::unique_ptr<ObjectEntry>> object_registry;

    const ObjectEntry* entry_from_name(const std::string& name)
    {
        auto& slot = object_registry[name];
        if (!slot)
            slot = std::make_unique<ObjectEntry>(ObjectEntry{name});
        return slot.get();
    }

    void parse_gcode_line(std::string_view line)
    {
        static constexpr std::string_view DEF_TYPES[]            = {";TYPE:", "; FEATURE: "};
        static constexpr std::string_view DEF_INNERPERIMETERS[]  = {"Perimeter", "Inner wall"};
        static constexpr std::string_view DEF_OUTERPERIMETERS[]  = {"External perimeter", "Outer wall"};
        static constexpr std::string_view DEF_WIPESTARTS[]       = {";WIPE_START", "; WIPE_START"};
        static constexpr std::string_view DEF_WIPEENDS[]         = {";WIPE_END", "; WIPE_END"};
        static constexpr std::string_view DEF_LAYERCHANGES[]     = {";LAYER_CHANGE", "; CHANGE_LAYER"};
        static constexpr std::string_view DEF_LAYER_HEIGHTS[]    = {";HEIGHT:", "; LAYER_HEIGHT: "};
        static constexpr std::string_view DEF_LAYER_ZS[]         = {";Z:", "; Z_HEIGHT: "};
        static constexpr std::string_view DEF_START_OBJECTS[]    = {"; printing object ", "; start printing object, "};
        static constexpr std::string_view DEF_STOP_OBJECTS[]     = {"; stop printing object ", "; stop printing object, "};

        auto in = [](std::string_view s, const auto& set) {
            for (std::string_view v : set)
                if (s == v) return true;
            return false;
        };
        auto matching_prefix = [](std::string_view s, const auto& set) -> std::optional<std::string_view> {
            for (std::string_view p : set)
                if (starts_with(s, p)) return p;
            return std::nullopt;
        };

        const std::string_view strippedline = py_strip(line);

        const bool old_internal_perimeter = internal_perimeter;
        just_changed_type                 = false;
        justgotinside_internal_perimeter  = false;
        justgotinside_external_perimeter  = false;
        justleft_internal_perimeter       = false;
        wipe_justfinished                 = false;
        layer_change                      = false;

        if (wipe_willfinish) {
            wiping            = false;
            wipe_justfinished = true;
            wipe_willfinish   = false;
        }

        if (!line.empty() && line[0] != ';')
            return;

        if (auto prefix = matching_prefix(line, DEF_TYPES)) {
            just_changed_type = true;
            const std::string new_type(py_strip(line.substr(prefix->size())));

            if (in(new_type, DEF_INNERPERIMETERS)) {
                if (!internal_perimeter_type)
                    internal_perimeter_type = std::string(line);
                current_type = SANE_INNERPERIMETER;
                if (!internal_perimeter)
                    justgotinside_internal_perimeter = true;
                internal_perimeter = true;
                external_perimeter = false;
                overhang_perimeter = false;
            } else if (in(new_type, DEF_OUTERPERIMETERS)) {
                if (!external_perimeter_type)
                    external_perimeter_type = std::string(line);
                current_type = SANE_OUTERPERIMETER;
                if (!external_perimeter)
                    justgotinside_external_perimeter = true;
                external_perimeter = true;
                internal_perimeter = false;
                overhang_perimeter = false;
            } else if (new_type == "Overhang wall") {
                current_type       = SANE_OVERHANGPERIMETER;
                overhang_perimeter = true;
            } else {
                internal_perimeter = false;
                external_perimeter = false;
                overhang_perimeter = false;
                current_type       = new_type;
            }

            if (old_internal_perimeter && !internal_perimeter)
                justleft_internal_perimeter = true;

        } else if (in(strippedline, DEF_WIPESTARTS)) {
            wiping = true;
            if (!const_wipe_start)
                if (auto p = matching_prefix(strippedline, DEF_WIPESTARTS))
                    const_wipe_start = std::string(*p) + "\n";

        } else if (in(strippedline, DEF_WIPEENDS)) {
            wipe_willfinish = true;
            if (!const_wipe_end)
                if (auto p = matching_prefix(strippedline, DEF_WIPEENDS))
                    const_wipe_end = std::string(*p) + "\n";

        } else if (auto p = matching_prefix(strippedline, DEF_START_OBJECTS)) {
            const std::string object_name(strippedline.substr(p->size()));
            if (!const_printingobject_start)
                const_printingobject_start = std::string(*p);
            current_object = entry_from_name(object_name);

        } else if (!const_printingobject_stop && matching_prefix(strippedline, DEF_STOP_OBJECTS)) {
            const_printingobject_stop = std::string(*matching_prefix(strippedline, DEF_STOP_OBJECTS));

        } else if (in(strippedline, DEF_LAYERCHANGES)) {
            if (!const_layer_change)
                const_layer_change = std::string(line);
            layer_change       = true;
            internal_perimeter = false;
            external_perimeter = false;
            layer += 1;
            capture_height = true;

        } else if (auto pz = matching_prefix(strippedline, DEF_LAYER_ZS)) {
            z = py_float(strippedline.substr(pz->size()));
            if (!const_layer_z)
                const_layer_z = std::string(*pz);

        } else if (auto ph = matching_prefix(strippedline, DEF_LAYER_HEIGHTS)) {
            if (capture_height) {
                height         = py_float(strippedline.substr(ph->size()));
                capture_height = false;
                if (!const_layer_height)
                    const_layer_height = std::string(*ph);
            }
        }

        if (!just_changed_type)
            last_type = current_type;
    }
};

// ---------------------------------------------------------------------------
// Loop nesting (LoopNode / calculate_loop_depth / build_loop_tree)
// ---------------------------------------------------------------------------

struct LoopNode
{
    bool                   around_hole = false;
    int                    depth       = 0;
    const GCodeStateBBox*  boundingbox = nullptr;
    size_t                 order       = 0;
    LoopP                  looplines;
    std::vector<LoopNode*> kids;
};

struct LoopTree
{
    int concentric = 0;

    // Returns depth + 1 for loops around a hole, std::nullopt (Python None) otherwise.
    std::optional<int> propagate(LoopNode& node, std::vector<bool>& boolean_list, int depth, std::optional<int> myconcentric)
    {
        if (!myconcentric) {
            concentric += 1;
            myconcentric = concentric;
        }

        node.depth = depth;

        // Python: `kid_depth` is only bound when there are kids.
        std::optional<int> kid_depth;
        const size_t       total_kids = node.kids.size();
        if (total_kids == 1) {
            kid_depth = propagate(*node.kids[0], boolean_list, depth + 1, concentric);
        } else if (total_kids > 1) {
            for (LoopNode* kid : node.kids) {
                concentric += 1;
                kid_depth = propagate(*kid, boolean_list, depth + 1, concentric);
            }
        }

        if (node.around_hole) {
            if (total_kids == 0) {
                node.depth = 0;
                depth      = 0;
            } else if (kid_depth) {
                node.depth = *kid_depth;
                depth      = *kid_depth;
            }
        }

        if ((depth + 1) % 2)
            boolean_list[node.order] = true;

        for (const LineP& line : *node.looplines) {
            line->looporder        = depth;
            line->concentric_group = *myconcentric;
        }

        if (node.around_hole)
            return depth + 1;
        return std::nullopt;
    }

    static std::vector<LoopNode*> build_loop_tree(std::vector<LoopNode>& nodes, bool hole)
    {
        std::vector<LoopNode*> parents;
        LoopNode*              previous_node = nullptr;

        for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
            LoopNode& node = nodes[node_index];
            if (node_index != 0) {
                if (node.boundingbox->contains(*previous_node->boundingbox)) {
                    node.kids.push_back(previous_node);
                    if (hole) {
                        node.around_hole           = true;
                        previous_node->around_hole = true;
                    }
                    std::vector<LoopNode*> parents_to_remove;
                    for (LoopNode* parent : parents) {
                        if (node.boundingbox->contains(*parent->boundingbox)) {
                            node.kids.push_back(parent);
                            parents_to_remove.push_back(parent);
                        }
                    }
                    for (LoopNode* parent : parents_to_remove)
                        parents.erase(std::find(parents.begin(), parents.end(), parent));
                } else {
                    parents.push_back(previous_node);
                }
            }
            previous_node = &node;
        }
        parents.push_back(previous_node);
        return parents;
    }

    std::vector<bool> calculate_loop_depth(const Perimeter& group_perimeter)
    {
        std::vector<bool> moving_order(group_perimeter.size(), false);

        std::vector<GCodeStateBBox> bboxes(group_perimeter.size());
        std::vector<LoopNode>       nodes(group_perimeter.size());
        for (size_t loop_index = 0; loop_index < group_perimeter.size(); ++loop_index) {
            for (const LineP& pline : *group_perimeter[loop_index])
                if (pline->current->is_extruding)
                    bboxes[loop_index].compute(*pline->current);
            nodes[loop_index].order       = loop_index;
            nodes[loop_index].boundingbox = &bboxes[loop_index];
            nodes[loop_index].looplines   = group_perimeter[loop_index];
        }

        std::vector<LoopNode> nodes_reverse(nodes.rbegin(), nodes.rend());
        for (LoopNode& n : nodes_reverse) {
            n.kids.clear();
            n.around_hole = false;
            n.depth       = 0;
        }

        std::vector<LoopNode*> parents_direct  = build_loop_tree(nodes, false);
        std::vector<LoopNode*> parents_reverse = build_loop_tree(nodes_reverse, true);

        std::vector<LoopNode*> parents_merged;
        for (LoopNode* parent : parents_direct) {
            if (!parent->kids.empty()) {
                parents_merged.push_back(parent);
            } else {
                auto match = std::find_if(parents_reverse.begin(), parents_reverse.end(),
                                          [parent](const LoopNode* r) { return r->order == parent->order; });
                if (match != parents_reverse.end()) {
                    if (!(*match)->kids.empty())
                        parents_merged.push_back(*match);
                    else
                        parents_merged.push_back(parent);
                }
            }
        }

        concentric = 0;
        for (LoopNode* parent : parents_merged) {
            concentric += 1;
            propagate(*parent, moving_order, 0, std::nullopt);
        }
        return moving_order;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// BrickLayersProcessor
// ---------------------------------------------------------------------------

struct Processor::Impl
{
    explicit Impl(Params p) : params(std::move(p))
    {
        extrusion_global_multiplier  = params.extrusion_multiplier;
        start_at_layer               = params.start_at_layer;
        layers_to_ignore             = params.layers_to_ignore;
        extrusion_multiplier         = extrusion_global_multiplier;
        first_layer_multiplier       = extrusion_global_multiplier * 1.5;
        extrusion_multiplier_preview = ((extrusion_multiplier - 1) / 4) + 1;
        if (params.enable_header)
            gen_header_lines();
    }

    // --- settings (BrickLayersProcessor.__init__) ---
    Params           params;
    double           extrusion_global_multiplier = 1.05;
    int              start_at_layer              = 3;
    std::vector<int> layers_to_ignore;
    double           travel_threshold    = 1.5;
    double           wipe_distance       = 2.0;
    double           retract_before_wipe = 0.8;
    double           travel_zhop         = 0.4;
    double           retracted           = 0.0;
    LineP            last_noninternalperimeter_xy_line;

    // --- process_gcode() locals ---
    GCodeFeature   feature;
    GCodeSimulator simulator;
    GCodeState     current_state  = simulator.get_state();
    GCodeState     previous_state = current_state;
    LineP          special_accel_command;
    double         extrusion_multiplier         = 1.05;
    double         first_layer_multiplier       = 1.575;
    double         extrusion_multiplier_preview = 1.0125;

    std::vector<LineP>      buffer_lines;
    PerimeterP              group_perimeter = std::make_shared<Perimeter>();
    LoopP                   group_loop      = std::make_shared<Loop>();
    std::vector<PerimeterP> deffered_perimeters;
    std::vector<LoopP>      kept_loops;
    std::vector<LoopP>      temp_list;

    bool still_in_header                       = true;
    bool detect_speeds                         = true;
    bool knife_activated                       = false;
    bool layer_changed_during_internal_perimeter = false;

    LoopTree loop_tree;

    bool layer_is_processed(int layer) const
    {
        return layer >= start_at_layer &&
               std::find(layers_to_ignore.begin(), layers_to_ignore.end(), layer) == layers_to_ignore.end();
    }

    void gen_header_lines()
    {
        const std::string pf  = ";==";
        const std::string bar = "==============================";
        std::vector<std::string> t;
        t.push_back(bar);
        t.push_back(" BrickLayers Post-Processed");
        for (const auto& [key, value] : params.header_info)
            t.push_back(" " + key + ": " + value);
        t.push_back(bar);
        t.push_back("");
        for (const std::string& line : t)
            buffer_lines.push_back(from_gcode(line.empty() ? std::string("\n") : pf + line + "\n"));
    }

    static LineP new_line_from_multiplier(const LineP& myline, double multiplier)
    {
        double e_val = myline->current->e - myline->previous->e;
        e_val        = e_val * multiplier;

        std::string command;
        bool        replaced = false;
        for (std::string_view part : py_split(*myline->gcode)) {
            if (!command.empty())
                command += ' ';
            if (!replaced && starts_with(part, "E")) {
                command += "E" + fmt_fixed(e_val, 5);
                replaced = true;
            } else {
                command += part;
            }
        }
        myline->gcode = command + "\n";
        return myline;
    }

    // travel_to(target_state, simulator, feature, loop=None, start_state=None, z=None)
    void travel_to(std::vector<LineP>& gcodes, const GCodeState& target_state, const LoopP& loop,
                   const std::optional<GCodeState>& start_state, std::optional<double> z)
    {
        GCodeState start_pos;
        if (loop)
            start_pos = *loop->back()->current;
        else if (start_state)
            start_pos = *start_state;

        std::string move_z, hopping_z;
        if (z) {
            move_z    = " Z" + fmt_fixed(*z, 2);
            hopping_z = " Z" + fmt_fixed(*z + travel_zhop, 2);
        }

        if (distance_between_points(start_pos, target_state) < travel_threshold) {
            gcodes.push_back(from_gcode("G1 X" + py_repr(target_state.x) + " Y" + py_repr(target_state.y) + move_z + " F" +
                                        fmt_int(simulator.travel_speed) + " ; BRICK: Travel (no-retraction)\n"));
            gcodes.push_back(from_gcode("G1 F" + fmt_int(target_state.f) + " ; BRICK: Feed Rate (no wipe)\n"));
            return;
        }

        if (simulator.retraction_length > 0 && simulator.retraction_speed > 0) {
            double retraction = simulator.wipe_speed == 0 ? simulator.retraction_length
                                                          : simulator.retraction_length * retract_before_wipe;
            gcodes.push_back(from_gcode("G1 E-" + fmt_fixed(retraction, 2) + " F" + fmt_int(simulator.retraction_speed) +
                                        " ; BRICK: Retraction \n"));
            retracted = retracted + retraction;

            if (loop && simulator.wipe_speed > 0 && retract_before_wipe < 1)
                wipe(gcodes, *loop);
        }

        gcodes.push_back(from_gcode("G1 X" + py_repr(target_state.x) + " Y" + py_repr(target_state.y) + hopping_z + " F" +
                                    fmt_int(simulator.travel_speed) + " ; BRICK: Target Position\n"));
        if (z)
            gcodes.push_back(from_gcode("G1 Z" + fmt_fixed(*z, 2) + " ; BRICK: Target Position\n"));

        if (retracted > 0 && simulator.retraction_speed > 0) {
            gcodes.push_back(from_gcode("G1 E" + fmt_fixed(retracted, 2) + " F" + fmt_int(simulator.retraction_speed) +
                                        " ; BRICK: Unretract\n"));
            retracted = 0;
        }
    }

    void wipe(std::vector<LineP>& gcodes, const Loop& loop)
    {
        const double total_extrusion_to_retract = simulator.retraction_length * (1 - retract_before_wipe);
        const double extrusion_per_mm           = total_extrusion_to_retract / wipe_distance;

        const GCodeState start_pos = *loop.back()->current;
        const bool       forward   = distance_between_points(start_pos, *loop.front()->previous) < 1;

        double              traveled = 0.0;
        std::vector<Point>  moving_points;
        std::vector<double> moving_extrusions;

        auto visit = [&](const LineP& line) -> bool {
            if (!line->current->is_extruding)
                return true;
            const GCodeState& from_pos = forward ? *line->previous : *line->current;
            const GCodeState& to_pos   = forward ? *line->current : *line->previous;

            const double segment_length = distance_between_points(from_pos, to_pos);
            if (segment_length <= 1e-6)
                return true;

            if (traveled + segment_length >= wipe_distance) {
                const double needed_distance = wipe_distance - traveled;
                moving_points.push_back(forward ? point_along_line_forward(from_pos, to_pos, needed_distance)
                                                : point_along_line_backward(from_pos, to_pos, needed_distance));
                moving_extrusions.push_back(needed_distance * extrusion_per_mm);
                return false;
            }
            moving_points.push_back(Point{to_pos.x, to_pos.y});
            moving_extrusions.push_back(segment_length * extrusion_per_mm);
            traveled += segment_length;
            return true;
        };
        if (forward) {
            for (const LineP& line : loop)
                if (!visit(line)) break;
        } else {
            for (auto it = loop.rbegin(); it != loop.rend(); ++it)
                if (!visit(*it)) break;
        }

        gcodes.push_back(from_gcode(feature.const_wipe_start));
        gcodes.push_back(from_gcode("G1 F" + fmt_int(simulator.wipe_speed) + "\n"));
        for (size_t i = 0; i < moving_points.size(); ++i)
            gcodes.push_back(from_gcode("G1 X" + fmt_fixed(moving_points[i].x, 3) + " Y" + fmt_fixed(moving_points[i].y, 3) +
                                        " E-" + fmt_fixed(moving_extrusions[i], 5) + " ; BRICK: Wipe \n"));
        gcodes.push_back(from_gcode(feature.const_wipe_end));

        retracted = retracted + total_extrusion_to_retract;
    }

    void generate_deffered_perimeters(const LineP& myline, std::vector<PerimeterP>& deffered, double multiplier,
                                      std::vector<LineP>& buffer)
    {
        if (deffered.empty())
            return;

        const double      target_z           = feature.z + feature.height / 2;
        const std::string target_z_formated  = fmt_fixed(target_z, 2);
        const double      higher_z           = feature.z + feature.height + 0.2;
        const std::string higher_z_formated  = fmt_fixed(higher_z, 2);

        const ObjectEntry* current_object       = nullptr;
        LoopP              previous_loop;
        bool               should_create_moveup = false;
        LineP              deffered_line; // last line visited (Python loop variable)

        deffered.erase(std::remove_if(deffered.begin(), deffered.end(), [](const PerimeterP& p) { return p->empty(); }),
                       deffered.end());

        for (size_t perimeter_index = 0; perimeter_index < deffered.size(); ++perimeter_index) {
            Perimeter& deffered_perimeter = *deffered[perimeter_index];
            const bool is_first_perimeter = perimeter_index == 0;
            const bool is_last_perimeter  = perimeter_index == deffered.size() - 1;
            deffered_perimeter.erase(std::remove_if(deffered_perimeter.begin(), deffered_perimeter.end(),
                                                    [](const LoopP& l) { return l->empty(); }),
                                     deffered_perimeter.end());

            for (size_t loop_index = 0; loop_index < deffered_perimeter.size(); ++loop_index) {
                const LoopP& deffered_loop = deffered_perimeter[loop_index];
                const bool   is_first_loop = loop_index == 0;
                const bool   is_last_loop  = loop_index == deffered_perimeter.size() - 1;
                bool         new_loop      = true;

                for (size_t line_index = 0; line_index < deffered_loop->size(); ++line_index) {
                    deffered_line            = (*deffered_loop)[line_index];
                    const bool is_first_line = line_index == 0;
                    const bool is_last_line  = line_index == deffered_loop->size() - 1;

                    // In the beginning of the layer
                    if (is_first_perimeter && is_first_loop && is_first_line) {
                        LineP xy_line_to_adjust = last_noninternalperimeter_xy_line;
                        if (xy_line_to_adjust && !xy_line_to_adjust->current->is_extruding) {
                            xy_line_to_adjust->gcode = "G1 X" + py_repr(deffered_line->previous->x) + " Y" +
                                                       py_repr(deffered_line->previous->y) + " Z" + higher_z_formated + " F" +
                                                       fmt_int(simulator.travel_speed) + " ; BRICK: Travel Fix Up\n";
                            last_noninternalperimeter_xy_line = nullptr;
                        } else {
                            should_create_moveup = true;
                        }
                        if (feature.current_object)
                            buffer.push_back(from_gcode(py_str(feature.const_printingobject_stop) + feature.current_object->name + "\n"));
                        if (!deffered_line->current->relative_extrusion)
                            buffer.push_back(from_gcode(std::string("M83 ; BRICK: Change to Relative Extrusion\n")));
                        buffer.push_back(from_gcode(feature.const_layer_change));
                        buffer.push_back(from_gcode(py_str(feature.const_layer_z) + target_z_formated + "\n"));
                        buffer.push_back(from_gcode(";" + target_z_formated + "\n"));
                        buffer.push_back(from_gcode(py_str(feature.const_layer_height) + fmt_fixed(feature.height, 2) + "\n"));
                        if (should_create_moveup)
                            travel_to(buffer, *deffered_line->previous, nullptr, deffered_line->previous, higher_z);
                        buffer.push_back(from_gcode("G1 Z" + target_z_formated + " F" + fmt_int(simulator.travel_speed) +
                                                    " ; BRICK: Z-Hop Down\n"));
                        buffer.push_back(from_gcode(feature.internal_perimeter_type));
                        buffer.push_back(from_gcode(py_str(simulator.const_width) + fmt_fixed(deffered_line->current->width, 2) + "\n"));
                    }

                    if (current_object != deffered_line->object) {
                        if (current_object)
                            buffer.push_back(from_gcode(py_str(feature.const_printingobject_stop) + current_object->name + "\n"));
                        if (previous_loop)
                            travel_to(buffer, *deffered_line->previous, previous_loop, std::nullopt, target_z);
                        else
                            travel_to(buffer, *deffered_line->previous, nullptr, deffered_line->previous, target_z);
                        buffer.push_back(from_gcode(py_str(feature.const_printingobject_start) +
                                                    (deffered_line->object ? deffered_line->object->name : std::string("None")) + "\n"));
                        buffer.push_back(from_gcode("G1 F" + fmt_int(deffered_line->previous->f) + " ; BRICK: FeedRate\n"));
                        current_object = deffered_line->object;
                    } else if (new_loop && is_first_line) {
                        if (previous_loop)
                            travel_to(buffer, *deffered_line->previous, previous_loop, std::nullopt, target_z);
                        else
                            travel_to(buffer, *deffered_line->previous, nullptr, deffered_line->previous, target_z);
                        buffer.push_back(from_gcode("G1 F" + fmt_int(deffered_line->previous->f) + " ; BRICK: FeedRate\n"));
                    }

                    buffer.push_back(new_line_from_multiplier(deffered_line, multiplier));

                    // The last thing of the layer:
                    if (is_last_perimeter && is_last_loop && is_last_line && current_object)
                        buffer.push_back(from_gcode("; stop printing object " + current_object->name + "\n"));
                    if (is_last_perimeter && is_last_loop && is_last_line && !deffered_line->current->relative_extrusion) {
                        buffer.push_back(from_gcode(std::string("M82 ; BRICK: Return to Absolute Extrusion\n")));
                        buffer.push_back(from_gcode("G92 E" + py_repr(myline->previous->e) +
                                                    " ; BRICK: Resets the Extruder absolute position\n"));
                    }
                    new_loop = false;
                }
                previous_loop = deffered_loop;
            }
        }

        deffered.clear();

        buffer.push_back(from_gcode("G1 X" + py_repr(myline->previous->x) + " Y" + py_repr(myline->previous->y) + " F" +
                                    fmt_int(simulator.travel_speed) + " ; BRICK: Calculated to next coordinate\n"));
        // (The Python script raises UnboundLocalError here if no deferred line was visited.)
        if (deffered_line)
            buffer.push_back(from_gcode("G1 F" + fmt_int(deffered_line->previous->f) + " ; BRICK: Feed Rate\n"));
    }

    void flush(std::string& out)
    {
        for (const LineP& l : buffer_lines)
            if (l && l->gcode)
                out += *l->gcode;
        buffer_lines.clear();
    }

    static bool is_orca_preview_type(const std::string& t)
    {
        return t == "Internal Bridge" || t == "Ironing" || t == "Bridge" || t == "Sparse infill";
    }

    // One iteration of the main loop of process_gcode().
    void process_line(const std::string& line, std::string& out)
    {
        static constexpr std::string_view DEF_LAYER_HEIGHTS[] = {";HEIGHT:", "; LAYER_HEIGHT: "};

        if (still_in_header && starts_with(line, ";TYPE:Custom"))
            still_in_header = false;

        simulator.parse_gcode_line(line);
        current_state = simulator.get_state();

        feature.parse_gcode_line(line);

        LineP myline   = from_gcode(line);
        myline->object = feature.current_object;

        extrusion_multiplier = feature.layer == start_at_layer ? first_layer_multiplier : extrusion_global_multiplier;

        // Detecting speeds from the file
        if (detect_speeds) {
            if (simulator.retraction_speed == 0 && feature.wiping)
                simulator.retraction_speed = simulator.f;
            if (simulator.wipe_speed == 0 && feature.wipe_willfinish)
                simulator.wipe_speed = simulator.f;
            if (simulator.detraction_speed == 0 && simulator.retraction_speed > 0 && simulator.is_extruding)
                simulator.detraction_speed = simulator.f;
            if (simulator.wipe_speed > 0 && simulator.detraction_speed > 0 && simulator.retraction_speed > 0)
                detect_speeds = false;
        }

        if (layer_changed_during_internal_perimeter && simulator.is_extruding && simulator.is_moving) {
            feature.internal_perimeter               = true;
            layer_changed_during_internal_perimeter = false;
        }

        // Capture all the lines of an internal perimeter
        if (feature.internal_perimeter) {
            if (layer_is_processed(feature.layer)) {
                myline->previous = previous_state;
                myline->current  = current_state;
                myline->object   = feature.current_object;

                if (starts_with(*myline->gcode, "SET_VELOCITY_LIMIT ") || starts_with(*myline->gcode, "M204 ")) {
                    special_accel_command = myline;
                    return; // Python `continue`: previous_state is intentionally not updated.
                }

                if (!knife_activated && (feature.wiping || simulator.retracted < 0 || simulator.just_stopped_extruding))
                    knife_activated = true;
                else if (knife_activated && simulator.is_extruding && simulator.is_moving)
                    knife_activated = false;
                else if (knife_activated && !feature.wiping && starts_with(*myline->gcode, "G1 F"))
                    knife_activated = false;

                if (feature.justgotinside_internal_perimeter) {
                    group_loop      = std::make_shared<Loop>();
                    group_perimeter = std::make_shared<Perimeter>();
                    group_perimeter->push_back(group_loop);
                    deffered_perimeters.push_back(group_perimeter);
                } else if (knife_activated) {
                    if (!group_loop->empty()) {
                        group_loop = std::make_shared<Loop>();
                        group_perimeter->push_back(group_loop);
                    }
                } else {
                    group_loop->push_back(myline);
                }
            } else {
                buffer_lines.push_back(myline);
            }
        }

        if (feature.layer_change && feature.current_type == "internal_perimeter" && !deffered_perimeters.empty())
            layer_changed_during_internal_perimeter = true;

        // Processing the last buffered perimeter
        if (feature.justleft_internal_perimeter || layer_changed_during_internal_perimeter) {
            knife_activated  = false;
            myline->previous = previous_state;
            myline->current  = current_state;

            while (!group_perimeter->empty() && group_perimeter->back()->empty())
                group_perimeter->pop_back();

            if (!group_perimeter->empty()) {
                const std::vector<bool> moving_sequence = loop_tree.calculate_loop_depth(*group_perimeter);

                for (size_t pos = 0; pos < moving_sequence.size(); ++pos) {
                    if (moving_sequence[pos])
                        temp_list.push_back((*group_perimeter)[pos]);
                    else
                        kept_loops.push_back((*group_perimeter)[pos]);
                }
                *group_perimeter = temp_list;
                temp_list.clear();

                // Reinsert the loops that remain at the normal height
                if (!kept_loops.empty()) {
                    LoopP previous_loop;
                    LineP kept_line, calculated_line;

                    for (size_t loop_index = 0; loop_index < kept_loops.size(); ++loop_index) {
                        const LoopP& kept_loop     = kept_loops[loop_index];
                        const bool   is_first_loop = loop_index == 0;

                        for (size_t line_index = 0; line_index < kept_loop->size(); ++line_index) {
                            kept_line                = (*kept_loop)[line_index];
                            const bool is_first_line = line_index == 0;

                            if (is_first_loop && is_first_line) {
                                LineP xy_line_to_adjust = last_noninternalperimeter_xy_line;
                                if (xy_line_to_adjust && !xy_line_to_adjust->current->is_extruding) {
                                    xy_line_to_adjust->update_xy(kept_line->previous->x, kept_line->previous->y);
                                    last_noninternalperimeter_xy_line = nullptr;
                                }
                                buffer_lines.push_back(from_gcode(feature.internal_perimeter_type));
                                buffer_lines.push_back(from_gcode(py_str(feature.const_layer_height) + py_repr(feature.height) + "\n"));
                            }

                            if (is_first_loop && is_first_line && !kept_line->current->relative_extrusion)
                                buffer_lines.push_back(from_gcode(std::string("M83 ; BRICK: Change to Relative Extrusion\n")));

                            if (is_first_line) {
                                if (previous_loop)
                                    travel_to(buffer_lines, *kept_line->previous, previous_loop, std::nullopt, feature.z);
                                else
                                    travel_to(buffer_lines, *kept_line->previous, nullptr, kept_line->previous, feature.z);
                                buffer_lines.push_back(from_gcode("G1 F" + fmt_int(kept_line->previous->f) + " ; BRICK: Feed Rate\n"));
                            }

                            calculated_line = new_line_from_multiplier(kept_line, extrusion_multiplier);
                            buffer_lines.push_back(calculated_line);
                        }
                        previous_loop = kept_loop;
                    }

                    if (kept_line && !kept_line->current->relative_extrusion) {
                        buffer_lines.push_back(from_gcode(std::string("M82 ; BRICK: Return to Absolute Extrusion\n")));
                        buffer_lines.push_back(from_gcode("G92 E" + py_repr(myline->previous->e) +
                                                          " ; BRICK: Resets the Extruder absolute position\n"));
                    }
                    buffer_lines.push_back(from_gcode(py_str(simulator.const_width) + py_repr(myline->previous->width) + "\n"));
                    buffer_lines.push_back(from_gcode(py_str(feature.const_layer_height) + fmt_fixed(feature.height, 2) + "\n"));

                    kept_loops.clear();
                }
            }

            if (layer_is_processed(feature.layer)) {
                buffer_lines.push_back(from_gcode("G1 X" + py_repr(myline->previous->x) + " Y" + py_repr(myline->previous->y) + " F" +
                                                  fmt_int(simulator.travel_speed) + " ; BRICK: Calculated to next coordinate\n"));
                buffer_lines.push_back(from_gcode("G1 F" + fmt_int(myline->previous->f) + " ; BRICK: Feed Rate\n"));
            }

            if (special_accel_command) {
                buffer_lines.push_back(special_accel_command);
                special_accel_command = nullptr;
            }
        }

        // Layer change (or the final "Custom" block): emit the deferred loops and flush the layer
        if (feature.layer_change ||
            (feature.current_type == "Custom" && feature.just_changed_type && current_state.z > 0)) {
            myline->previous = previous_state;
            myline->current  = current_state;

            // Makes the very last layer flatter (no brick).
            if (feature.current_type == "Custom" && current_state.z > 0) {
                feature.z            = feature.z - feature.height / 2;
                extrusion_multiplier = extrusion_multiplier / 2;
            }

            generate_deffered_perimeters(myline, deffered_perimeters, extrusion_multiplier, buffer_lines);

            if (feature.current_type == "external_perimeter")
                buffer_lines.push_back(from_gcode(feature.external_perimeter_type));

            flush(out);
            deffered_perimeters.clear();
        }

        // All the other lines go straight to the buffer
        if (!feature.internal_perimeter) {
            buffer_lines.push_back(myline);
            if (simulator.moved_in_xy) {
                myline->current                   = current_state;
                last_noninternalperimeter_xy_line = myline;
            }
        }

        // OrcaSlicer / BambuStudio preview cosmetics
        if (is_orca_preview_type(feature.current_type)) {
            bool is_height_line = false;
            for (std::string_view p : DEF_LAYER_HEIGHTS)
                if (starts_with(line, p)) is_height_line = true;
            if (is_height_line)
                myline->gcode = py_str(feature.const_layer_height) + py_repr(feature.height) + "\n";
            if (feature.just_changed_type)
                buffer_lines.push_back(from_gcode(py_str(feature.const_layer_height) + py_repr(feature.height) + "\n"));
        }

        // Restore the width after an external perimeter begins (preview only).
        if (feature.justgotinside_external_perimeter && simulator.const_width)
            buffer_lines.push_back(from_gcode(*simulator.const_width + py_repr(current_state.width) + "\n"));

        previous_state = current_state;
    }

    void finish(std::string& out)
    {
        flush(out);
        kept_loops.clear();
        group_perimeter->clear();
        deffered_perimeters.clear();
    }
};

Processor::Processor(Params params) : m_impl(std::make_unique<Impl>(std::move(params))) {}

Processor::~Processor() = default;

void Processor::feed(std::string_view chunk, std::string& out)
{
    if (m_finished)
        throw std::logic_error("Brick Layers: feed() after finish()");
    size_t start = 0;
    while (start < chunk.size()) {
        const size_t nl = chunk.find('\n', start);
        if (nl == std::string_view::npos) {
            m_partial_line.append(chunk.substr(start));
            break;
        }
        m_partial_line.append(chunk.substr(start, nl + 1 - start));
        m_impl->process_line(m_partial_line, out);
        m_partial_line.clear();
        start = nl + 1;
    }
}

void Processor::finish(std::string& out)
{
    if (m_finished)
        return;
    if (!m_partial_line.empty()) {
        m_impl->process_line(m_partial_line, out);
        m_partial_line.clear();
    }
    m_impl->finish(out);
    m_finished = true;
}

std::string Processor::process(std::string_view gcode, const Params& params)
{
    Processor   p(params);
    std::string out;
    out.reserve(gcode.size() + gcode.size() / 8);
    p.feed(gcode, out);
    p.finish(out);
    return out;
}

} // namespace Slic3r::GCode::BrickLayers
