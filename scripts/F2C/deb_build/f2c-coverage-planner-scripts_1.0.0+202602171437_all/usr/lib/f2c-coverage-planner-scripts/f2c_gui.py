#!/usr/bin/env python3
"""
Interactive PySide6 GUI for the Fields2Cover coverage planner.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np
import open3d as o3d
from PySide6 import QtGui, QtWidgets
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QProgressBar,
    QPushButton,
    QRadioButton,
    QSizePolicy,
    QSpinBox,
    QSplitter,
    QStatusBar,
    QVBoxLayout,
    QWidget,
)
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.backends.backend_qt5agg import NavigationToolbar2QT
from matplotlib.figure import Figure
from shapely.geometry import Polygon

try:
    from . import f2c_pipeline as fp
except ImportError:  # Running as a standalone script
    import os
    import pathlib

    HERE = pathlib.Path(__file__).resolve().parent
    if str(HERE) not in sys.path:
        sys.path.append(str(HERE))
    import f2c_pipeline as fp  # type: ignore


@dataclass
class CoverageConfig:
    swath_width: float
    headland_width: float
    turn_radius: float
    auto_align: bool
    align_mode: str
    route_pattern: str = "boustro"
    path_planner: str = "dubins"
    corners_only: bool = False
    obj_function: str = "nswath_modified"  # Objective function for swath optimization
    start_point: Optional[Tuple[float, float]] = None  # Optional start point (x, y)
    end_point: Optional[Tuple[float, float]] = None  # Optional end point (x, y)
    use_decomposition: bool = False  # Enable decomposition for concave fields
    decomposition_type: str = "boustrophedon"  # "boustrophedon" or "trapezoidal"
    use_axial_turns: bool = False  # Use axial turns (zero turning radius) instead of Dubins curves


class CustomNavigationToolbar(NavigationToolbar2QT):
    def __init__(self, canvas, parent=None):
        super().__init__(canvas, parent)
        self.full_extent_provider = None

    def set_full_extent_provider(self, provider):
        self.full_extent_provider = provider

    def home(self, *args):
        if self.full_extent_provider:
            extent = self.full_extent_provider()
            if extent:
                ax = getattr(self.canvas, "ax", None)
                if ax is None and self.canvas.figure.axes:
                    ax = self.canvas.figure.axes[0]
                if ax is not None:
                    ax.set_xlim(extent[0], extent[1])
                    ax.set_ylim(extent[2], extent[3])
                    self.canvas.draw_idle()
                    return
        super().home(*args)


class MplCanvas(FigureCanvas):
    def __init__(self, parent: QWidget | None = None) -> None:
        self.fig = Figure()
        self.ax = self.fig.add_subplot(111)
        self.fig.patch.set_facecolor("#ffffff")
        self.ax.set_facecolor("#ffffff")
        self.ax.grid(color="#d0d0d0", linestyle="--", linewidth=0.5, alpha=0.8)
        for spine in self.ax.spines.values():
            spine.set_color("#333333")
        self.ax.tick_params(colors="#000000")
        self.ax.title.set_color("#000000")
        self.ax.yaxis.label.set_color("#000000")
        self.ax.xaxis.label.set_color("#000000")
        super().__init__(self.fig)
        self.setParent(parent)
        self.setFocusPolicy(Qt.StrongFocus)


class CoverageGUI(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Roof Coverage Planner")
        self._load_styles()
        self._icon_style = self.style()

        # State
        self.pcd_points: Optional[np.ndarray] = None
        self.filtered_points: Optional[np.ndarray] = None
        self.xy_2d: Optional[np.ndarray] = None
        self.poly: Optional[Polygon] = None
        self.field: Optional[fp.f2c.Field] = None  # type: ignore
        self.swaths = None
        self.route = None
        self.path = None
        self.loaded_file: Optional[str] = None
        self.roi_polygon: Optional[Polygon] = None

        # Matplotlib ROI/obstacle selection
        self._roi_points: List[Tuple[float, float]] = []
        self._roi_active = False
        self._mpl_cid = None
        self._mpl_motion_cid = None
        self._roi_markers = None
        self._roi_line_artist = None
        self._roi_fill_artist = None
        self._roi_preview_line = None
        self._selection_mode: Optional[str] = None  # "roi" or "obstacle"

        # Obstacles
        self.obstacles: List[Polygon] = []

        self._full_extent: Optional[Tuple[float, float, float, float]] = None
        self._needs_view_reset = True
        self._status_bar: Optional[QStatusBar] = None

        central = QWidget()
        main_layout = QHBoxLayout(central)
        splitter = QSplitter(Qt.Horizontal)
        splitter.setChildrenCollapsible(False)
        main_layout.addWidget(splitter)

        controls_scroll = QtWidgets.QScrollArea()
        controls_scroll.setWidgetResizable(True)
        controls_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        controls_scroll.setMinimumWidth(420)
        controls_scroll.setSizePolicy(QSizePolicy(QSizePolicy.Preferred, QSizePolicy.Expanding))
        controls_container = QWidget()
        controls_layout = QVBoxLayout(controls_container)
        controls_layout.setContentsMargins(12, 12, 12, 12)
        controls_layout.setSpacing(12)
        controls_scroll.setWidget(controls_container)
        splitter.addWidget(controls_scroll)

        plot_container = QWidget()
        plot_layout = QVBoxLayout(plot_container)
        plot_layout.setContentsMargins(0, 0, 0, 0)
        self.canvas = MplCanvas(self)
        self.canvas.mpl_connect("scroll_event", self._on_scroll_zoom)
        plot_layout.addWidget(self.canvas, 1)
        self.nav_toolbar = CustomNavigationToolbar(self.canvas, self)
        self.nav_toolbar.set_full_extent_provider(self._get_full_extent)
        plot_layout.addWidget(self.nav_toolbar)
        splitter.addWidget(plot_container)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)

        self._build_file_controls(controls_layout)
        self._build_height_controls(controls_layout)
        self._build_downsample_controls(controls_layout)
        self._build_hull_controls(controls_layout)
        self._build_simplify_controls(controls_layout)
        self._build_f2c_controls(controls_layout)
        self._build_export_controls(controls_layout)

        controls_layout.addStretch(1)

        self.setCentralWidget(central)
        self.resize(1500, 900)
        self._status_bar = QStatusBar()
        self.setStatusBar(self._status_bar)
        self._progress_bar = QProgressBar()
        self._progress_bar.setVisible(False)
        self._progress_bar.setRange(0, 100)
        self._progress_bar.setValue(0)
        self._progress_bar.setMaximumWidth(200)
        self._status_bar.addPermanentWidget(self._progress_bar)
        self.set_status("Ready")

    def set_status(self, text: str, timeout_ms: int = 0) -> None:
        if self._status_bar is not None:
            self._status_bar.showMessage(text, timeout_ms)
        else:
            print(f"[STATUS] {text}")

    def _progress_begin(self, text: str = "", determinate: bool = False) -> None:
        if self._progress_bar is None:
            return
        if determinate:
            self._progress_bar.setRange(0, 100)
            self._progress_bar.setValue(0)
        else:
            self._progress_bar.setRange(0, 0)
        self._progress_bar.setVisible(True)
        if text:
            self.set_status(text)

    def _progress_update(self, value: int, text: Optional[str] = None) -> None:
        if self._progress_bar is None:
            return
        if self._progress_bar.maximum() == 0:
            self._progress_bar.setRange(0, 100)
        self._progress_bar.setValue(max(0, min(100, value)))
        if text:
            self.set_status(text)

    def _progress_end(self, text: Optional[str] = None) -> None:
        if self._progress_bar is None:
            return
        self._progress_bar.setVisible(False)
        if text:
            self.set_status(text)

    def _load_styles(self) -> None:
        QtWidgets.QApplication.setStyle("Fusion")
        font = QtGui.QFont("Segoe UI", 10)
        QtWidgets.QApplication.instance().setFont(font)
        style_path = Path(__file__).resolve().parent / "assets" / "style.qss"
        if style_path.exists():
            with open(style_path, "r", encoding="utf-8") as f:
                QtWidgets.QApplication.instance().setStyleSheet(f.read())

    # ------------------------------------------------------------------ UI sections
    def _build_file_controls(self, layout: QVBoxLayout) -> None:
        box = QGroupBox("Point Cloud")
        v = QVBoxLayout(box)
        btn_load = QPushButton("Load PCD / PLY / XYZ")
        btn_load.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DialogOpenButton))
        btn_load.setToolTip("Load a point cloud file (.pcd, .ply, .xyz)")
        btn_load.clicked.connect(self.load_point_cloud)
        v.addWidget(btn_load)
        self.lbl_file = QLabel("No file loaded")
        v.addWidget(self.lbl_file)
        layout.addWidget(box)

    def _build_height_controls(self, layout: QVBoxLayout) -> None:
        box = QGroupBox("Height Cropping")
        v = QVBoxLayout(box)
        h = QHBoxLayout()
        h.addWidget(QLabel("Z-band ±"))
        self.spin_z_band = QDoubleSpinBox()
        self.spin_z_band.setRange(0.0, 10.0)
        self.spin_z_band.setSingleStep(0.05)
        self.spin_z_band.setValue(0.5)
        h.addWidget(self.spin_z_band)
        v.addLayout(h)
        btn_apply = QPushButton("Apply Height Crop & View 3D")
        btn_apply.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_ArrowDown))
        btn_apply.setToolTip("Filter points within the selected Z band and preview in 3D")
        btn_apply.clicked.connect(self.apply_height_crop)
        v.addWidget(btn_apply)
        layout.addWidget(box)

    def _build_downsample_controls(self, layout: QVBoxLayout) -> None:
        box = QGroupBox("Downsampling")
        v = QVBoxLayout(box)
        method_row = QHBoxLayout()
        method_row.addWidget(QLabel("Method"))
        self.combo_downsample = QtWidgets.QComboBox()
        self.combo_downsample.addItems(["None", "Random", "Voxel", "Statistical"])
        self.combo_downsample.currentTextChanged.connect(self._update_downsample_ui)
        method_row.addWidget(self.combo_downsample)
        v.addLayout(method_row)

        # Random params
        self.group_random = QGroupBox("Random settings")
        rand_layout = QHBoxLayout(self.group_random)
        rand_layout.addWidget(QLabel("Max points"))
        self.spin_max_points = QSpinBox()
        self.spin_max_points.setRange(100, 2_000_000)
        self.spin_max_points.setValue(50_000)
        rand_layout.addWidget(self.spin_max_points)
        v.addWidget(self.group_random)

        # Voxel params
        self.group_voxel = QGroupBox("Voxel grid settings")
        voxel_layout = QHBoxLayout(self.group_voxel)
        voxel_layout.addWidget(QLabel("Voxel size"))
        self.spin_voxel = QDoubleSpinBox()
        self.spin_voxel.setRange(0.001, 1.0)
        self.spin_voxel.setSingleStep(0.005)
        self.spin_voxel.setValue(0.05)
        voxel_layout.addWidget(self.spin_voxel)
        v.addWidget(self.group_voxel)

        # Statistical params
        self.group_stat = QGroupBox("Statistical outlier settings")
        stat_layout = QHBoxLayout(self.group_stat)
        stat_layout.addWidget(QLabel("Mean K"))
        self.spin_mean_k = QSpinBox()
        self.spin_mean_k.setRange(5, 1000)
        self.spin_mean_k.setValue(20)
        stat_layout.addWidget(self.spin_mean_k)
        stat_layout.addWidget(QLabel("Std Ratio"))
        self.spin_std_ratio = QDoubleSpinBox()
        self.spin_std_ratio.setRange(0.1, 5.0)
        self.spin_std_ratio.setSingleStep(0.1)
        self.spin_std_ratio.setValue(1.0)
        stat_layout.addWidget(self.spin_std_ratio)
        v.addWidget(self.group_stat)

        self._update_downsample_ui(self.combo_downsample.currentText())

        btn_down = QPushButton("Downsample")
        btn_down.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_BrowserReload))
        btn_down.setToolTip("Apply the selected downsampling method to the filtered cloud")
        btn_down.clicked.connect(self.apply_downsample)
        v.addWidget(btn_down)
        layout.addWidget(box)

    def _build_hull_controls(self, layout: QVBoxLayout) -> None:
        box = QGroupBox("2D Projection & Concave Hull")
        v = QVBoxLayout(box)
        
        # Method selection - only accurate methods
        h_method = QHBoxLayout()
        h_method.addWidget(QLabel("Method:"))
        self.combo_hull_method = QComboBox()
        self.combo_hull_method.addItem("AlphaShape (Most Accurate)", "alphashape")
        self.combo_hull_method.addItem("Delaunay (Fast & Accurate)", "delaunay")
        self.combo_hull_method.addItem("Grid (Fast, Less Accurate)", "grid")
        self.combo_hull_method.setCurrentIndex(0)  # Default to AlphaShape
        self.combo_hull_method.setToolTip(
            "AlphaShape: Most accurate for concave shapes, adjustable detail\n"
            "Delaunay: Fast and accurate for convex/mildly concave shapes\n"
            "Grid: Fastest but less accurate, adjustable grid size"
        )
        h_method.addWidget(self.combo_hull_method)
        v.addLayout(h_method)
        
        # Alpha/Grid Size parameter (adaptive based on method)
        h = QHBoxLayout()
        h.addWidget(QLabel("Parameter:"))
        self.spin_alpha = QDoubleSpinBox()
        self.spin_alpha.setRange(0.01, 10.0)
        self.spin_alpha.setSingleStep(0.1)
        self.spin_alpha.setValue(1.5)
        self.spin_alpha.setToolTip(
            "AlphaShape: Alpha parameter (0.1-0.5=detailed, 0.5-2.0=balanced, 2.0+=smooth)\n"
            "Grid: Grid cell size (smaller=more accurate but slower)"
        )
        h.addWidget(self.spin_alpha)
        v.addLayout(h)
        
        btn_proj = QPushButton("Project to 2D & Compute Hull")
        btn_proj.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_FileDialogDetailedView))
        btn_proj.setToolTip("Project filtered points to XY plane and compute boundary hull")
        btn_proj.clicked.connect(self.compute_hull)
        v.addWidget(btn_proj)
        layout.addWidget(box)

    def _build_simplify_controls(self, layout: QVBoxLayout) -> None:
        box = QGroupBox("Polygon Simplification")
        v = QVBoxLayout(box)
        h = QHBoxLayout()
        h.addWidget(QLabel("Tolerance"))
        self.spin_simplify = QDoubleSpinBox()
        self.spin_simplify.setRange(0.0, 5.0)
        self.spin_simplify.setSingleStep(0.05)
        self.spin_simplify.setValue(0.1)
        h.addWidget(self.spin_simplify)
        v.addLayout(h)
        btn_simplify = QPushButton("Simplify Polygon")
        btn_simplify.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_BrowserReload))
        btn_simplify.setToolTip("Apply Douglas–Peucker simplification to the hull")
        btn_simplify.clicked.connect(self.simplify_polygon)
        v.addWidget(btn_simplify)
        layout.addWidget(box)

    def _build_f2c_controls(self, layout: QVBoxLayout) -> None:
        box = QGroupBox("Fields2Cover & Coverage")
        v = QVBoxLayout(box)

        h1 = QHBoxLayout()
        h1.addWidget(QLabel("Swath width"))
        self.spin_swath = QDoubleSpinBox()
        self.spin_swath.setRange(0.05, 10.0)
        self.spin_swath.setSingleStep(0.05)
        self.spin_swath.setValue(1.0)
        h1.addWidget(self.spin_swath)
        v.addLayout(h1)

        h2 = QHBoxLayout()
        h2.addWidget(QLabel("Headland width"))
        self.spin_headland = QDoubleSpinBox()
        self.spin_headland.setRange(0.0, 10.0)
        self.spin_headland.setSingleStep(0.1)
        self.spin_headland.setValue(1.0)
        h2.addWidget(self.spin_headland)
        v.addLayout(h2)

        h3 = QHBoxLayout()
        h3.addWidget(QLabel("Turn radius"))
        self.spin_turn = QDoubleSpinBox()
        self.spin_turn.setRange(0.0, 20.0)
        self.spin_turn.setSingleStep(0.1)
        self.spin_turn.setValue(0.5)
        h3.addWidget(self.spin_turn)
        v.addLayout(h3)

        self.chk_auto_align = QCheckBox("Auto-align to building")
        v.addWidget(self.chk_auto_align)

        align_box = QHBoxLayout()
        self.radio_long = QRadioButton("Parallel (long edge)")
        self.radio_perp = QRadioButton("Perpendicular")
        self.radio_perp.setChecked(True)
        align_box.addWidget(self.radio_long)
        align_box.addWidget(self.radio_perp)
        v.addLayout(align_box)

        route_pattern_layout = QHBoxLayout()
        route_pattern_layout.addWidget(QLabel("Route pattern"))
        self.combo_route_pattern = QComboBox()
        self.combo_route_pattern.setToolTip("Select how swaths are ordered before route planning")
        self.combo_route_pattern.addItem("Boustrophedon", "boustro")
        if hasattr(fp.f2c, "RP_Snake"):
            self.combo_route_pattern.addItem("Snake", "snake")
        if hasattr(fp.f2c, "RP_Spiral"):
            self.combo_route_pattern.addItem("Spiral", "spiral")
        route_pattern_layout.addWidget(self.combo_route_pattern)
        v.addLayout(route_pattern_layout)

        planner_layout = QHBoxLayout()
        planner_layout.addWidget(QLabel("Path planner"))
        self.combo_path_planner = QComboBox()
        self.combo_path_planner.setToolTip("Choose the curve type for the smooth connecting path")
        
        # Add checkbox for axial turns (zero turning radius)
        self.chk_axial_turns = QCheckBox("Use axial turns (zero radius)")
        self.chk_axial_turns.setToolTip(
            "For robots that can turn in place. Uses swath corners with instantaneous heading changes.\n"
            "Ensures complete coverage without sacrificing swath ends for Dubins arcs."
        )
        v.addWidget(self.chk_axial_turns)

        def _add_planner(label: str, value: str, attr: Optional[str]) -> None:
            if attr is None or hasattr(fp.f2c, attr):
                self.combo_path_planner.addItem(label, value)

        _add_planner("Dubins curves", "dubins", "PP_DubinsCurves")
        _add_planner("Dubins curves (CC)", "dubins_cc", "PP_DubinsCurvesCC")
        _add_planner("Reeds–Shepp curves", "reeds", "PP_ReedsSheppCurves")
        _add_planner("Reeds–Shepp curves (HC)", "reeds_hc", "PP_ReedsSheppCurvesHC")
        self.combo_path_planner.addItem("Straight (no curves)", "none")
        planner_layout.addWidget(self.combo_path_planner)
        v.addLayout(planner_layout)

        # Decomposition controls (for concave fields)
        decomp_box = QHBoxLayout()
        self.chk_decomposition = QCheckBox("Use decomposition (for concave fields)")
        self.chk_decomposition.setToolTip("Split concave fields into convex sub-fields for better coverage")
        decomp_box.addWidget(self.chk_decomposition)
        v.addLayout(decomp_box)
        
        decomp_type_layout = QHBoxLayout()
        decomp_type_layout.addWidget(QLabel("Decomposition type"))
        self.combo_decomp_type = QComboBox()
        self.combo_decomp_type.setToolTip("Method for decomposing concave fields")
        self.combo_decomp_type.addItem("Boustrophedon", "boustrophedon")
        self.combo_decomp_type.addItem("Trapezoidal", "trapezoidal")
        decomp_type_layout.addWidget(self.combo_decomp_type)
        v.addLayout(decomp_type_layout)

        # ROI controls
        roi_box = QHBoxLayout()
        self.btn_roi = QPushButton("Select ROI")
        self.btn_roi.setCheckable(True)
        self.btn_roi.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DialogYesButton))
        self.btn_roi.setToolTip("Select a polygonal region of interest to restrict coverage")
        self.btn_roi.clicked.connect(self.toggle_roi_mode)
        roi_box.addWidget(self.btn_roi)
        self.btn_roi_clear = QPushButton("Clear ROI")
        self.btn_roi_clear.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DialogCancelButton))
        self.btn_roi_clear.setToolTip("Remove the current ROI")
        self.btn_roi_clear.clicked.connect(self.clear_roi)
        roi_box.addWidget(self.btn_roi_clear)
        v.addLayout(roi_box)
        roi_actions = QHBoxLayout()
        self.btn_roi_finish = QPushButton("Finish ROI")
        self.btn_roi_finish.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DialogApplyButton))
        self.btn_roi_finish.setToolTip("Close the current ROI polygon")
        self.btn_roi_finish.clicked.connect(lambda: self._stop_polygon_selection(finished=True))
        roi_actions.addWidget(self.btn_roi_finish)
        self.btn_roi_undo = QPushButton("Undo Point")
        self.btn_roi_undo.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_ArrowBack))
        self.btn_roi_undo.setToolTip("Remove the last point in the ROI/obstacle polygon")
        self.btn_roi_undo.clicked.connect(self.undo_polygon_point)
        roi_actions.addWidget(self.btn_roi_undo)
        v.addLayout(roi_actions)
        self.lbl_roi = QLabel("ROI: none")
        v.addWidget(self.lbl_roi)

        obstacle_box = QHBoxLayout()
        self.btn_obstacle = QPushButton("Add Obstacle")
        self.btn_obstacle.setCheckable(True)
        self.btn_obstacle.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_MessageBoxWarning))
        self.btn_obstacle.setToolTip("Draw a polygon that marks an obstacle / no-go zone")
        self.btn_obstacle.clicked.connect(self.toggle_obstacle_mode)
        obstacle_box.addWidget(self.btn_obstacle)
        self.btn_obstacle_clear = QPushButton("Clear Obstacles")
        self.btn_obstacle_clear.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_TrashIcon))
        self.btn_obstacle_clear.setToolTip("Remove all obstacle polygons")
        self.btn_obstacle_clear.clicked.connect(self.clear_obstacles)
        obstacle_box.addWidget(self.btn_obstacle_clear)
        v.addLayout(obstacle_box)
        self.lbl_obstacles = QLabel("Obstacles: 0")
        v.addWidget(self.lbl_obstacles)

        btn_field = QPushButton("Build F2C Field")
        btn_field.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DriveHDIcon))
        btn_field.setToolTip("Convert the current polygon (minus obstacles) into a Fields2Cover field")
        btn_field.clicked.connect(self.build_f2c_field)
        v.addWidget(btn_field)

        btn_swaths = QPushButton("Generate Swaths")
        btn_swaths.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_ArrowForward))
        btn_swaths.setToolTip("Generate coverage swaths for the current field")
        btn_swaths.clicked.connect(self.generate_swaths)
        v.addWidget(btn_swaths)

        btn_route = QPushButton("Generate Route")
        btn_route.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DialogApplyButton))
        btn_route.setToolTip("Generate an ordered route that avoids obstacles")
        btn_route.clicked.connect(self.generate_route)
        v.addWidget(btn_route)

        btn_clear_swaths = QPushButton("Clear Swaths/Path")
        btn_clear_swaths.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_TrashIcon))
        btn_clear_swaths.setToolTip("Remove all generated swaths and paths")
        btn_clear_swaths.clicked.connect(self.clear_swaths)
        v.addWidget(btn_clear_swaths)

        btn_path = QPushButton("Generate Path")
        btn_path.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_MediaPlay))
        btn_path.setToolTip("Generate a smooth Dubins path along the swaths")
        btn_path.clicked.connect(self.generate_path)
        v.addWidget(btn_path)

        layout.addWidget(box)

    def _build_export_controls(self, layout: QVBoxLayout) -> None:
        box = QGroupBox("Export")
        v = QVBoxLayout(box)
        btn_export_full = QPushButton("Export Path CSV")
        btn_export_full.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DialogSaveButton))
        btn_export_full.setToolTip("Save the generated path to CSV")
        btn_export_full.clicked.connect(self.export_path_csv)
        v.addWidget(btn_export_full)
        btn_export_corners = QPushButton("Export Swath Corners CSV")
        btn_export_corners.setIcon(self._icon_style.standardIcon(QtWidgets.QStyle.SP_DialogSaveButton))
        btn_export_corners.setToolTip("Save the start/end of each swath to CSV")
        btn_export_corners.clicked.connect(self.export_corners_csv)
        v.addWidget(btn_export_corners)
        layout.addWidget(box)

    # ------------------------------------------------------------------ Actions
    def load_point_cloud(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Select point cloud", "", "Point Clouds (*.pcd *.ply *.xyz)"
        )
        if not path:
            return
        self._progress_begin("Loading point cloud...")
        try:
            pts = fp.load_point_cloud(path)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Point Cloud", str(exc))
            self._progress_end("Failed to load point cloud")
            return
        self._progress_end()
        self._after_cloud_loaded(path, pts)

    def apply_height_crop(self) -> None:
        if self.pcd_points is None:
            return
        z_band = self.spin_z_band.value()
        self._progress_begin("Applying height crop...")
        try:
            pts = fp.filter_by_z_band(self.pcd_points, z_band)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Height Crop", str(exc))
            self._progress_end("Height crop failed")
            return
        self._progress_end()
        self._after_height_crop(pts, z_band)

    def _update_downsample_ui(self, method: str) -> None:
        method = method.lower()
        self.group_random.setVisible(method == "random")
        self.group_voxel.setVisible(method == "voxel")
        self.group_stat.setVisible(method == "statistical")

    def apply_downsample(self) -> None:
        if self.filtered_points is None:
            return
        method = self.combo_downsample.currentText().lower()
        if method == "none":
            self.set_status("Downsampling skipped (method=None)", 3000)
            return
        params = {}
        if method == "random":
            params["max_points"] = self.spin_max_points.value()
        elif method == "voxel":
            params["voxel_size"] = self.spin_voxel.value()
        elif method == "statistical":
            params["mean_k"] = self.spin_mean_k.value()
            params["std_ratio"] = self.spin_std_ratio.value()
        self._progress_begin(f"Applying {method} downsampling...")
        try:
            pts = fp.apply_downsample(self.filtered_points, method, params)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Downsampling", str(exc))
            self._progress_end("Downsampling failed")
            return
        self._progress_end()
        self._after_downsample(pts, method)

    def compute_hull(self) -> None:
        if self.filtered_points is None:
            return
        alpha = self.spin_alpha.value()
        method = self.combo_hull_method.currentData()
        # Note: Downsampling is already handled in the "Downsampling" section before this step
        xy = self.filtered_points[:, :2].copy()
        
        method_name = self.combo_hull_method.currentText()
        self._progress_begin(f"Computing hull using {method_name}...")
        try:
            poly = fp.compute_concave_hull(xy, alpha, method=method)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Hull Computation", str(exc))
            self._progress_end("Hull computation failed")
            return
        self._progress_end()
        self._after_hull(poly, xy)

    def simplify_polygon(self) -> None:
        if self.poly is None:
            return
        tol = self.spin_simplify.value()
        source_poly = self.poly
        self._progress_begin("Simplifying polygon...")
        try:
            poly = fp.simplify_polygon(source_poly, tol)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Simplify", str(exc))
            self._progress_end("Polygon simplification failed")
            return
        self._progress_end()
        self._after_simplify(poly)

    def build_f2c_field(self) -> None:
        effective_poly = self._effective_polygon()
        if effective_poly is None:
            QtWidgets.QMessageBox.warning(self, "F2C", "No usable polygon (check ROI/obstacles).")
            return
        self._progress_begin("Building F2C field...")
        try:
            field_obj = fp.polygon_to_f2c_field(effective_poly)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "F2C", str(exc))
            self._progress_end("Failed to build field")
            return
        self._progress_end()
        self._after_field_built(field_obj)

    def _current_config(self, corners_only: bool = False) -> CoverageConfig:
        align_mode = "perp" if self.radio_perp.isChecked() else "long"
        route_pattern = self.combo_route_pattern.currentData()
        if not route_pattern:
            route_pattern = "boustro"
        path_planner = self.combo_path_planner.currentData()
        if not path_planner:
            path_planner = "dubins"
        decomp_type = self.combo_decomp_type.currentData()
        if not decomp_type:
            decomp_type = "boustrophedon"
        
        return CoverageConfig(
            swath_width=self.spin_swath.value(),
            headland_width=self.spin_headland.value(),
            turn_radius=self.spin_turn.value(),
            auto_align=self.chk_auto_align.isChecked(),
            align_mode=align_mode,
            route_pattern=str(route_pattern),
            path_planner=str(path_planner),
            corners_only=corners_only,
            obj_function="nswath_modified",  # Use faster approximation by default
            start_point=None,  # Can be extended with GUI controls if needed
            end_point=None,  # Can be extended with GUI controls if needed
            use_decomposition=self.chk_decomposition.isChecked(),
            decomposition_type=str(decomp_type),
            use_axial_turns=self.chk_axial_turns.isChecked(),
        )

    def generate_swaths(self) -> None:
        self._generate(mode="swaths")

    def generate_route(self) -> None:
        self._generate(mode="route")

    def generate_path(self) -> None:
        self._generate(mode="path")

    def clear_swaths(self) -> None:
        if self.swaths is None and self.path is None and self.route is None:
            self.set_status("Nothing to clear", 2000)
            return
        self.swaths = None
        self.route = None
        self.path = None
        self.set_status("Swaths, route, and path cleared", 4000)
        self._needs_view_reset = True
        self.refresh_plot()

    def _generate(self, mode: str) -> None:
        if self.poly is None:
            QtWidgets.QMessageBox.warning(self, "Warning", "Compute the concave hull first.")
            return
        effective_poly = self._effective_polygon()
        if effective_poly is None:
            QtWidgets.QMessageBox.warning(self, "Warning", "No usable area after ROI/obstacles.")
            return
        
        # corners_only should only be True if we ONLY want swath corners (not implemented in GUI yet)
        # For now, always False - we want full generation
        corners_only = False
        cfg = self._current_config(corners_only)

        self._progress_begin("Generating coverage...", determinate=True)
        try:
            self._progress_update(15, "Building F2C field...")
            field_obj = fp.polygon_to_f2c_field(effective_poly)
            if mode == "path":
                step_text = "Generating swaths, route & path"
            elif mode == "route":
                step_text = "Generating swaths & route"
            else:
                step_text = "Generating swaths"
            self._progress_update(60, step_text + "...")
            swaths, route, path = fp.generate_coverage(
                field_obj,
                effective_poly,
                cfg.__dict__,
                roi_polygon=None,
                corners_only=corners_only,
            )
            # Filter results based on mode
            if mode == "swaths":
                # Only keep swaths
                route = None
                path = None
            elif mode == "route":
                # Keep swaths and route, but not path
                path = None
            # For "path" mode, keep everything
            self._progress_update(100)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "Coverage", str(exc))
            self._progress_end("Coverage generation failed")
            return
        self._progress_end()
        self._after_generate((field_obj, swaths, route, path), mode)

    def export_path_csv(self) -> None:
        if self.path is None:
            QtWidgets.QMessageBox.warning(self, "Export", "No path to export.")
            return
        self._progress_begin("Exporting path CSV...", determinate=True)
        path, _ = QFileDialog.getSaveFileName(self, "Save Path CSV", "", "CSV (*.csv)")
        if not path:
            self._progress_end("Path export canceled")
            return
        if not path.lower().endswith(".csv"):
            path += ".csv"
        try:
            # Save only x,y coordinates
            import csv
            with open(path, 'w', newline='') as csvfile:
                writer = csv.writer(csvfile)
                writer.writerow(['x', 'y'])  # Header
            for i in range(self.path.size()):
                    state = self.path.getState(i)  # Use getState instead of at
                    point = state.point
                    writer.writerow([f"{point.getX():.6f}", f"{point.getY():.6f}"])
            point_count = self.path.size()
            QtWidgets.QMessageBox.information(self, "Export", f"Saved {point_count} points.")
            self._progress_end(f"Saved path CSV ({point_count} points)")
        except Exception as e:
            QtWidgets.QMessageBox.critical(self, "Export Error", f"Failed to save path: {e}")
            self._progress_end("Path export failed")

    def export_corners_csv(self) -> None:
        if self.swaths is None:
            QtWidgets.QMessageBox.warning(self, "Export", "No swaths available.")
            return
        self._progress_begin("Exporting swath corners...", determinate=True)
        path, _ = QFileDialog.getSaveFileName(
            self, "Save Swath Corners CSV", "", "CSV (*.csv)"
        )
        if not path:
            self._progress_end("Corner export canceled")
            return
        if not path.lower().endswith(".csv"):
            path += ".csv"
        self._progress_update(50, "Writing CSV...")
        fp.save_swath_corners_to_csv(self.swaths, path)
        QtWidgets.QMessageBox.information(self, "Export", "Corners CSV saved.")
        self._progress_end("Swath corners CSV saved")

    # ------------------------------------------------------------------ ROI / Obstacles
    def toggle_roi_mode(self, checked: bool) -> None:
        if checked:
            if self.btn_obstacle.isChecked():
                self.btn_obstacle.setChecked(False)
            self._start_polygon_selection("roi")
        else:
            if self._selection_mode == "roi":
                self._stop_polygon_selection(finished=False)

    def toggle_obstacle_mode(self, checked: bool) -> None:
        if checked:
            if self.btn_roi.isChecked():
                self.btn_roi.setChecked(False)
            self._start_polygon_selection("obstacle")
        else:
            if self._selection_mode == "obstacle":
                self._stop_polygon_selection(finished=False)

    def _start_polygon_selection(self, mode: str) -> None:
        if self.poly is None:
            QtWidgets.QMessageBox.warning(self, "Selection", "Load the point cloud and compute the hull first.")
            if mode == "roi":
                self.btn_roi.setChecked(False)
            else:
                self.btn_obstacle.setChecked(False)
            return
        if self._selection_mode is not None:
            self._stop_polygon_selection(finished=False)
        if self._mpl_cid is None:
            self._mpl_cid = self.canvas.mpl_connect("button_press_event", self._on_canvas_click)
        if self._mpl_motion_cid is None:
            self._mpl_motion_cid = self.canvas.mpl_connect("motion_notify_event", self._on_canvas_move)
        self._selection_mode = mode
        self._roi_points = []
        self._roi_active = True
        if mode == "roi":
            self.lbl_roi.setText("ROI: selecting points (left-click, right-click to finish)")
        self.set_status(f"{mode.capitalize()} selection enabled")
        self._clear_roi_artists()
        self.canvas.draw_idle()

    def _stop_polygon_selection(self, finished: bool) -> None:
        if self._mpl_cid is not None:
            self.canvas.mpl_disconnect(self._mpl_cid)
            self._mpl_cid = None
        if self._mpl_motion_cid is not None:
            self.canvas.mpl_disconnect(self._mpl_motion_cid)
            self._mpl_motion_cid = None
        mode = self._selection_mode
        self._selection_mode = None
        self._roi_active = False
        if mode == "roi":
            self.btn_roi.setChecked(False)
        elif mode == "obstacle":
            self.btn_obstacle.setChecked(False)

        if finished and len(self._roi_points) >= 3:
            poly = Polygon(self._roi_points)
            if not poly.is_valid:
                QtWidgets.QMessageBox.warning(self, "Selection", "Polygon is invalid.")
                self.set_status("Selection invalid")
            else:
                if mode == "roi":
                    self.roi_polygon = poly
                    self._update_roi_label()
                    self._invalidate_field("ROI updated; field reset")
                elif mode == "obstacle":
                    self.obstacles.append(poly)
                    self.lbl_obstacles.setText(f"Obstacles: {len(self.obstacles)}")
                    self.set_status(f"Obstacle added (total {len(self.obstacles)})", 4000)
                    self._invalidate_field("Obstacle added; field reset")
        else:
            if mode == "roi":
                if self.roi_polygon is None:
                    self.lbl_roi.setText("ROI: none")
                    self.set_status("ROI cleared", 4000)
            elif mode == "obstacle":
                self.set_status("Obstacle selection canceled", 3000)

        self._roi_points = []
        self._clear_roi_artists()
        self.refresh_plot()

    def _on_canvas_click(self, event):
        if not self._roi_active or event.inaxes != self.canvas.ax:
            return
        if event.button == 1 and event.xdata is not None and event.ydata is not None:
            self._roi_points.append((event.xdata, event.ydata))
            self._update_selection_status()
            self._update_roi_artists()
        elif event.button == 3:
            self._stop_polygon_selection(finished=True)

    def _on_canvas_move(self, event):
        if not self._roi_active or event.inaxes != self.canvas.ax:
            return
        if event.xdata is None or event.ydata is None:
            self._update_roi_artists()
        else:
            self._update_roi_artists(cursor_pos=(event.xdata, event.ydata))

    def clear_roi(self) -> None:
        if self.roi_polygon is None:
            self.set_status("ROI already cleared", 2000)
            return
        self.roi_polygon = None
        self._roi_points = []
        self._update_roi_label()
        self.set_status("ROI cleared", 4000)
        self._invalidate_field("ROI cleared; field reset")
        self.refresh_plot()

    def clear_obstacles(self) -> None:
        if not self.obstacles:
            self.set_status("No obstacles to clear", 2000)
            return
        self.obstacles.clear()
        self.lbl_obstacles.setText("Obstacles: 0")
        self.set_status("Obstacles cleared", 4000)
        self._invalidate_field("Obstacles cleared; field reset")
        self.refresh_plot()

    def undo_polygon_point(self) -> None:
        if not self._roi_points:
            self.set_status("No points to undo", 2000)
            return
        self._roi_points.pop()
        self._update_selection_status()
        self._update_roi_artists()

    # ------------------------------------------------------------------ Plotting
    def refresh_plot(self) -> None:
        ax = self.canvas.ax
        ax.clear()
        if self.filtered_points is not None:
            xy = self.filtered_points[:, :2]
            ax.scatter(xy[:, 0], xy[:, 1], s=1, c="gray", alpha=0.4, label="Points")

        if self.poly is not None:
            x, y = self.poly.exterior.xy
            ax.plot(x, y, "k-", linewidth=2, label="Boundary")

        if self.roi_polygon is not None:
            rx, ry = self.roi_polygon.exterior.xy
            ax.plot(rx, ry, "g-", linewidth=2, label="ROI")

        if self.obstacles:
            for idx, obs in enumerate(self.obstacles):
                ox, oy = obs.exterior.xy
                label = "Obstacle" if idx == 0 else "_nolegend_"
                ax.plot(ox, oy, "r-", linewidth=2, label=label)
                ax.fill(ox, oy, color="red", alpha=0.2, label="_nolegend_")

        if self.swaths is not None:
            try:
                for i in range(self.swaths.size()):
                    swath = self.swaths.at(i)
                    start = swath.startPoint()
                    end = swath.endPoint()
                    ax.plot(
                        [start.getX(), end.getX()],
                        [start.getY(), end.getY()],
                        "b--",
                        alpha=0.6,
                    )
                if self.swaths.size() > 0:
                    ax.plot([], [], "b--", label="Swaths")
            except Exception:
                pass

        if self.route is not None:
            route_xs: List[float] = []
            route_ys: List[float] = []
            try:
                # Try to get full route by iterating through swaths in route
                # Route contains ordered swaths, so visualize each swath's start/end points
                if hasattr(self.route, "getVectorSwaths"):
                    try:
                        route_swaths = self.route.getVectorSwaths()
                        # Use the helper to iterate through swaths consistently
                        for swath in fp._iter_swaths(route_swaths):
                            try:
                                start = swath.startPoint()
                                end = swath.endPoint()
                                route_xs.extend([start.getX(), end.getX()])
                                route_ys.extend([start.getY(), end.getY()])
                            except Exception:
                                continue
                    except Exception:
                        pass
                
                # Fallback to asLineString if swath iteration fails
                if not route_xs:
                line = self.route.asLineString()
                count = line.size()
                for i in range(count):
                    route_xs.append(line.getX(i))
                    route_ys.append(line.getY(i))
            except Exception:
                route_xs = []
                route_ys = []
            if route_xs:
                ax.plot(route_xs, route_ys, color="#ff8c00", linewidth=1.5, label="Route", alpha=0.7)

        if self.path is not None:
            xs, ys = [], []
            try:
                for i in range(self.path.size()):
                    st = self.path.getState(i)
                    xs.append(st.point.getX())
                    ys.append(st.point.getY())
            except Exception:
                try:
                    arr = np.asarray(self.path)
                    xs, ys = arr[:, 0], arr[:, 1]
                except Exception:
                    xs, ys = [], []
            if xs:
                ax.plot(xs, ys, "r-", linewidth=1.5, label="Path")
                ax.plot(xs[0], ys[0], "go", label="Start")
                ax.plot(xs[-1], ys[-1], "rx", label="End")

        ax.set_aspect("equal", "box")
        ax.grid(True)
        ax.legend(loc="best")
        ax.set_title("2D Projection / Coverage")
        self._full_extent = self._compute_full_extent()
        self.nav_toolbar.set_full_extent_provider(self._get_full_extent)
        if self._needs_view_reset:
            self._apply_full_extent()
            self._needs_view_reset = False
        if self._roi_active and self._roi_points:
            self._update_roi_artists()
        else:
            self._clear_roi_artists()
            self.canvas.draw_idle()

    def _compute_full_extent(self) -> Optional[Tuple[float, float, float, float]]:
        xmin = ymin = float("inf")
        xmax = ymax = float("-inf")

        def update_bounds(bounds: Tuple[float, float, float, float]) -> None:
            nonlocal xmin, xmax, ymin, ymax
            bx0, by0, bx1, by1 = bounds
            xmin = min(xmin, bx0)
            ymin = min(ymin, by0)
            xmax = max(xmax, bx1)
            ymax = max(ymax, by1)

        if self.filtered_points is not None and len(self.filtered_points) > 0:
            xy = self.filtered_points[:, :2]
            update_bounds(
                (
                    float(np.min(xy[:, 0])),
                    float(np.min(xy[:, 1])),
                    float(np.max(xy[:, 0])),
                    float(np.max(xy[:, 1])),
                )
            )

        if self.poly is not None:
            update_bounds(self.poly.bounds)

        if self.roi_polygon is not None:
            update_bounds(self.roi_polygon.bounds)

        for obs in self.obstacles:
            update_bounds(obs.bounds)

        if self.swaths is not None:
            sw_minx = sw_miny = float("inf")
            sw_maxx = sw_maxy = float("-inf")
            for i in range(self.swaths.size()):
                sw = self.swaths.at(i)
                start_pt = getattr(sw, "startPoint", None)
                end_pt = getattr(sw, "endPoint", None)
                if start_pt is None or end_pt is None:
                    continue
                for p in (start_pt(), end_pt()):
                    sw_minx = min(sw_minx, p.getX())
                    sw_miny = min(sw_miny, p.getY())
                    sw_maxx = max(sw_maxx, p.getX())
                    sw_maxy = max(sw_maxy, p.getY())
            if sw_minx != float("inf"):
                update_bounds((sw_minx, sw_miny, sw_maxx, sw_maxy))

        if self.route is not None:
            try:
                line = self.route.asLineString()
                if line.size() > 0:
                    route_minx = route_miny = float("inf")
                    route_maxx = route_maxy = float("-inf")
                    for i in range(line.size()):
                        x_val = line.getX(i)
                        y_val = line.getY(i)
                        route_minx = min(route_minx, x_val)
                        route_miny = min(route_miny, y_val)
                        route_maxx = max(route_maxx, x_val)
                        route_maxy = max(route_maxy, y_val)
                    if route_minx != float("inf"):
                        update_bounds((route_minx, route_miny, route_maxx, route_maxy))
            except Exception:
                pass

        if self.path is not None and self.path.size() > 0:
            path_minx = path_miny = float("inf")
            path_maxx = path_maxy = float("-inf")
            for i in range(self.path.size()):
                st = self.path.getState(i)
                path_minx = min(path_minx, st.point.getX())
                path_miny = min(path_miny, st.point.getY())
                path_maxx = max(path_maxx, st.point.getX())
                path_maxy = max(path_maxy, st.point.getY())
            if path_minx != float("inf"):
                update_bounds((path_minx, path_miny, path_maxx, path_maxy))

        if xmin == float("inf") or xmax == float("-inf"):
            return None

        width = max(1e-6, xmax - xmin)
        height = max(1e-6, ymax - ymin)
        margin = 0.05 * max(width, height)
        return (xmin - margin, xmax + margin, ymin - margin, ymax + margin)

    def _get_full_extent(self) -> Optional[Tuple[float, float, float, float]]:
        return self._full_extent

    def _apply_full_extent(self) -> None:
        if not self._full_extent:
            return
        ax = getattr(self.canvas, "ax", None)
        if ax is None and self.canvas.figure.axes:
            ax = self.canvas.figure.axes[0]
        if ax is None:
            return
        ax.set_xlim(self._full_extent[0], self._full_extent[1])
        ax.set_ylim(self._full_extent[2], self._full_extent[3])
        self.canvas.draw_idle()

    def _on_scroll_zoom(self, event) -> None:
        if event.inaxes != self.canvas.ax or event.xdata is None or event.ydata is None:
            return
        base_scale = 1.2
        if event.button == "up":
            scale_factor = 1 / base_scale
        elif event.button == "down":
            scale_factor = base_scale
        else:
            return
        ax = event.inaxes
        cur_xlim = ax.get_xlim()
        cur_ylim = ax.get_ylim()
        if cur_xlim[0] == cur_xlim[1] or cur_ylim[0] == cur_ylim[1]:
            return
        xdata, ydata = event.xdata, event.ydata
        new_width = (cur_xlim[1] - cur_xlim[0]) * scale_factor
        new_height = (cur_ylim[1] - cur_ylim[0]) * scale_factor
        relx = (cur_xlim[1] - xdata) / (cur_xlim[1] - cur_xlim[0])
        rely = (cur_ylim[1] - ydata) / (cur_ylim[1] - cur_ylim[0])
        ax.set_xlim([xdata - new_width * (1 - relx), xdata + new_width * relx])
        ax.set_ylim([ydata - new_height * (1 - rely), ydata + new_height * rely])
        self.canvas.draw_idle()

    def _clear_roi_artists(self) -> None:
        for artist in (
            self._roi_markers,
            self._roi_line_artist,
            self._roi_preview_line,
        ):
            if artist is not None:
                try:
                    artist.remove()
                except ValueError:
                    pass
        if self._roi_fill_artist is not None:
            try:
                self._roi_fill_artist.remove()
            except ValueError:
                pass
        self._roi_markers = None
        self._roi_line_artist = None
        self._roi_fill_artist = None
        self._roi_preview_line = None

    def _update_selection_status(self) -> None:
        if not self._roi_points:
            if self._selection_mode == "roi":
                self.lbl_roi.setText("ROI: none")
            return
        area_text = ""
        if len(self._roi_points) >= 3:
            poly = Polygon(self._roi_points)
            if poly.is_valid:
                area_text = f" (~{poly.area:.2f} u²)"
        if self._selection_mode == "roi":
            self.lbl_roi.setText(f"ROI: {len(self._roi_points)} point(s){area_text}")
        elif self._selection_mode == "obstacle":
            self.set_status(f"Obstacle points: {len(self._roi_points)}{area_text}")

    def _update_roi_artists(self, cursor_pos: Optional[Tuple[float, float]] = None) -> None:
        if not self._roi_active:
            self._clear_roi_artists()
            self.canvas.draw_idle()
            return
        ax = self.canvas.ax
        self._clear_roi_artists()
        if not self._roi_points:
            self.canvas.draw_idle()
            return
        xs, ys = zip(*self._roi_points)
        self._roi_markers, = ax.plot(xs, ys, "mo", markersize=5, label="_nolegend_")
        if len(self._roi_points) > 1:
            self._roi_line_artist, = ax.plot(xs, ys, "m-", linewidth=1.5, label="_nolegend_")
        if len(self._roi_points) >= 3:
            closed_xs = list(xs) + [xs[0]]
            closed_ys = list(ys) + [ys[0]]
            fill = ax.fill(
                closed_xs, closed_ys, color="magenta", alpha=0.15, label="_nolegend_"
            )
            self._roi_fill_artist = fill[0] if fill else None
        if cursor_pos and self._roi_points:
            last = self._roi_points[-1]
            self._roi_preview_line, = ax.plot(
                [last[0], cursor_pos[0]],
                [last[1], cursor_pos[1]],
                "m--",
                alpha=0.6,
                label="_nolegend_",
            )
        self.canvas.draw_idle()

    def _after_cloud_loaded(self, path: str, pts: np.ndarray) -> None:
        self.loaded_file = path
        self.lbl_file.setText(path)
        self.pcd_points = pts
        self.filtered_points = pts.copy()
        self.xy_2d = None
        self.poly = None
        self.roi_polygon = None
        self._roi_points = []
        self.obstacles.clear()
        self._update_roi_label()
        self.lbl_obstacles.setText("Obstacles: 0")
        self.field = None
        self.swaths = None
        self.route = None
        self.path = None
        self.set_status("Point cloud loaded", 4000)
        self._needs_view_reset = True
        self._clear_roi_artists()
        self.refresh_plot()

    def _after_height_crop(self, pts: np.ndarray, z_band: float) -> None:
        self.filtered_points = pts
        cloud = o3d.geometry.PointCloud()
        cloud.points = o3d.utility.Vector3dVector(self.filtered_points)
        o3d.visualization.draw_geometries([cloud])
        self.set_status(f"Height crop ±{z_band:.2f} m applied", 4000)
        self._needs_view_reset = True
        self.refresh_plot()

    def _after_downsample(self, pts: np.ndarray, method: str) -> None:
        self.filtered_points = pts
        self.set_status(f"{method.capitalize()} downsampling complete", 4000)
        self._needs_view_reset = True
        self.refresh_plot()

    def _after_hull(self, poly: Polygon, xy: np.ndarray) -> None:
        self.poly = poly
        self.xy_2d = xy
        self.set_status("Hull computed", 4000)
        self._invalidate_field("Hull updated; field reset")
        self.refresh_plot()

    def _after_simplify(self, poly: Polygon) -> None:
        self.poly = poly
        self.set_status("Polygon simplified", 4000)
        self._invalidate_field("Polygon simplified; field reset")
        self.refresh_plot()

    def _after_field_built(self, field_obj) -> None:
        self.field = field_obj
        QtWidgets.QMessageBox.information(self, "F2C", "Field built successfully.")
        self.set_status("F2C field built", 4000)

    def _after_generate(self, result, mode: str) -> None:
        field_obj, swaths, route, path = result
        self.field = field_obj
        self.swaths = swaths
        self.route = route
        self.path = path
        if swaths is None:
            QtWidgets.QMessageBox.warning(self, "Coverage", "No swaths generated.")
            self.set_status("No swaths generated")
        else:
            if mode == "swaths":
                self.set_status("Swaths generated", 4000)
            elif mode == "route":
                if route is None:
                    QtWidgets.QMessageBox.warning(self, "Coverage", "Route generation failed.")
                    self.set_status("Route generation failed", 4000)
                else:
                    self.set_status("Route generated", 4000)
            else:
                if path is None:
                    QtWidgets.QMessageBox.warning(self, "Coverage", "Path generation failed.")
                    self.set_status("Path generation failed")
                else:
                    suffix = " (route-aware)" if route is not None else ""
                    self.set_status(f"Path generated{suffix}", 4000)
        self._needs_view_reset = True
        self.refresh_plot()

    def _update_roi_label(self) -> None:
        if self.roi_polygon is None:
            self.lbl_roi.setText("ROI: none")
            return
        verts = max(len(self.roi_polygon.exterior.coords) - 1, 0)
        self.lbl_roi.setText(f"ROI: {verts} vertices (~{self.roi_polygon.area:.2f} u²)")

    def _effective_polygon(self) -> Optional[Polygon]:
        base = self.roi_polygon if self.roi_polygon is not None else self.poly
        if base is None:
            return None
        result = base
        for obs in self.obstacles:
            try:
                result = result.difference(obs)
            except Exception:
                continue
            if result.is_empty:
                return None
        if result.is_empty:
            return None
        if result.geom_type == "MultiPolygon":
            result = max(result.geoms, key=lambda g: g.area)
        return result

    def _invalidate_field(self, message: Optional[str] = None) -> None:
        self.field = None
        self.swaths = None
        self.route = None
        self.path = None
        if message:
            self.set_status(message, 4000)
        self._needs_view_reset = True


def main() -> None:
    app = QApplication(sys.argv)
    gui = CoverageGUI()
    gui.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()


