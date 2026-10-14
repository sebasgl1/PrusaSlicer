// Standalone command line driver for the Brick Layers port.
// Mirrors the options of bricklayers.py that affect the output, so both
// implementations can be compared on the same input:
//
//   bricklayers_cli input.gcode output.gcode [-extrusionMultiplier 1.05]
//                   [-startAtLayer 3] [-ignoreLayers 5 6 ...] [-chunk N]
//
// -chunk N feeds the input in N-byte pieces to exercise the streaming path.

#include "libslic3r/GCode/BrickLayers.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace Slic3r::GCode::BrickLayers;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " input.gcode output.gcode [-extrusionMultiplier X] [-startAtLayer N] "
                     "[-ignoreLayers N...] [-chunk BYTES]\n";
        return 2;
    }
    Params params;
    size_t chunk = 0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-extrusionMultiplier" && i + 1 < argc)
            params.extrusion_multiplier = std::strtod(argv[++i], nullptr);
        else if (a == "-startAtLayer" && i + 1 < argc)
            params.start_at_layer = std::atoi(argv[++i]);
        else if (a == "-ignoreLayers")
            while (i + 1 < argc && argv[i + 1][0] != '-')
                params.layers_to_ignore.push_back(std::atoi(argv[++i]));
        else if (a == "-chunk" && i + 1 < argc)
            chunk = std::strtoul(argv[++i], nullptr, 10);
        else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string input = ss.str();

    const auto  t0 = std::chrono::steady_clock::now();
    std::string output;
    try {
        if (chunk == 0) {
            output = Processor::process(input, params);
        } else {
            Processor p(params);
            for (size_t pos = 0; pos < input.size(); pos += chunk)
                p.feed(std::string_view(input).substr(pos, chunk), output);
            p.finish(output);
        }
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::ofstream out(argv[2], std::ios::binary);
    out << output;
    std::cerr << "processed " << input.size() << " bytes in " << secs << " s\n";
    return 0;
}
