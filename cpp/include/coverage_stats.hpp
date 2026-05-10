#pragma once

namespace f2c_cpp {

struct CoverageStats {
    double path_length_m = 0;
    double coverage_area_m2 = 0;
    double polygon_area_m2 = 0;
    double coverage_percent = 0;
    int num_swaths = 0;
    int num_turns = 0;
    int num_waypoints = 0;
    double estimated_time_min = 0;
    double overlap_percent = 0;

    bool isValid() const { return path_length_m > 0; }
};

}  // namespace f2c_cpp
