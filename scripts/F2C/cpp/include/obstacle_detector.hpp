/**
 * @file obstacle_detector.hpp
 * @brief Automatic obstacle detection from a 3D point cloud + driven path.
 *
 * This is a C++ port of `scripts/test_scripts/test_obstacle_detection.py`.
 * It uses the driven path to sample "footprint ground" points, fits a plane
 * with RANSAC, extracts obstacle candidates above/below the ground band,
 * removes outliers, clusters with DBSCAN, merges nearby clusters, and then
 * polygonizes each merged group.
 *
 * This port tracks the evolving Python reference implementation, including:
 * - grid-based polygonization with optional coarser contour extraction
 * - conservative smoothing (rolling-disk closing) with optional hole preservation
 * - micro-obstacle preservation (tiny but dense)
 */

#pragma once

#include "coverage_pipeline.hpp"

#include <string>
#include <vector>

namespace f2c_cpp {

enum class ObstaclePolygonMode {
    Auto,
    Hull,
    Grid,
};

struct ObstacleDetectionParams {
    // Robot dimensions (metres) - defaults match the Python script.
    double robot_length_m = 0.31;
    double robot_width_m = 0.28;
    double footprint_margin_m = 0.10;

    // Ground detection
    double ground_z_max = 0.0;
    int ransac_iters = 300;
    double ransac_thresh_m = 0.03;
    double ground_band_m = 0.05;

    // Obstacle extraction
    double obstacle_z_max = 0.30;
    double trough_depth_m = 0.05;
    int outlier_k = 20;
    double outlier_std = 1.5;

    // Clustering
    double cluster_eps_m = 0.07;
    int cluster_min_pts = 10;

    // Polygonization / chaining (defaults match current Python script)
    ObstaclePolygonMode polygon_mode = ObstaclePolygonMode::Grid;
    double grid_cell_m = 0.09;
    double contour_cell_m = -1.0;    // if < 0 => 2 * grid_cell_m
    double inflate_radius_m = 0.0;  // default: disabled
    double smooth_radius_m = -1.0;  // if < 0 => 2 * grid_cell_m (kept for parity)

    // Post-process smoothing (rolling-disk closing)
    double geom_smooth_radius_m = 1.0;
    int geom_smooth_segs = 16;  // (Shapely-only in Python; kept for parity)
    bool preserve_holes = true;
    double preserve_holes_min_area_m2 = 0.5;

    // AUTO mode heuristic
    double hollow_ratio_thresh = 0.35;
    double min_contour_area_m2 = 1e-4;

    // Cluster merging (metres)
    double merge_distance_m = 0.50;

    // Micro obstacles (tiny but dense) - enabled by default in Python
    bool micro_enable = true;
    double micro_max_span_m = 0.01;
    double micro_min_size_m = 0.01;
    double micro_margin_m = 0.002;
    int micro_min_pts = 22;
    double micro_min_density_pts_per_m2 = 200000.0;
    double micro_noise_eps_m = 0.025;
};

struct ObstacleDetectionStats {
    size_t input_points = 0;
    size_t roi_points = 0;
    size_t path_poses = 0;
    size_t footprint_ground_points = 0;
    size_t ground_points_band = 0;
    size_t raw_obstacle_candidates = 0;
    size_t obstacle_points_after_outlier = 0;
    int clusters_found = 0;
    int groups_merged = 0;
    int obstacle_shapes = 0;
    int total_holes = 0;

    // Plane: n·p + d = 0
    double plane_nx = 0.0;
    double plane_ny = 0.0;
    double plane_nz = 1.0;
    double plane_d = 0.0;
};

struct ObstacleDetectionResult {
    bool success = false;
    std::string error_message;
    std::vector<Obstacle2D> obstacles;
    ObstacleDetectionStats stats;
};

using ObstacleCancelCallback = std::function<bool()>;

/**
 * @brief Set a cooperative cancellation callback for long obstacle-detection runs.
 */
void setObstacleCancelCallback(ObstacleCancelCallback callback);

/**
 * @brief Detect obstacles using AUTO mode (hull by default, grid-with-holes when hollow).
 *
 * @param cloud Point cloud currently loaded in the UI (map frame).
 * @param driven_path Driven odometry trail poses (x,y,heading). Used to sample footprint ground.
 * @param roi_or_boundary Optional polygon to restrict detection (legacy; UI may choose to post-filter instead).
 * @param params Algorithm parameters (defaults match Python script).
 */
ObstacleDetectionResult detectObstaclesAuto(
    const PointCloudPtr& cloud,
    const std::vector<PathState>& driven_path,
    const Polygon2D* roi_or_boundary,
    const ObstacleDetectionParams& params = {});

}  // namespace f2c_cpp

