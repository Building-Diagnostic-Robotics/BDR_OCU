#!/bin/bash
# Create Debian package for BDR Coverage Planner

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
PACKAGE_NAME="bdr-coverage-planner"
ARCH="amd64"

# Version composition (CMake project semver + build number + git short SHA).
# Build number = git commit count; gives dpkg a monotonically increasing
# upstream version so OTA `dpkg -i` always treats a new push as an upgrade.
# Override any piece via env (CI uses BDR_VERSION_SHA / BDR_BUILD_NUMBER).
SEMVER="${BDR_VERSION_SEMVER:-1.0.0}"
GIT_SHA_FALLBACK="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_COUNT_FALLBACK="$(git -C "$SCRIPT_DIR" rev-list --count HEAD 2>/dev/null || echo 0)"
SHORT_SHA="${BDR_VERSION_SHA:-$GIT_SHA_FALLBACK}"
BUILD_NUMBER="${BDR_BUILD_NUMBER:-$GIT_COUNT_FALLBACK}"

if [ "$SHORT_SHA" = "unknown" ] || [ -z "$SHORT_SHA" ]; then
    VERSION="$SEMVER"
else
    VERSION="${SEMVER}-${BUILD_NUMBER}+${SHORT_SHA}"
fi

DEB_DIR="${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}"
DEB_FILE="${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"

echo "=== Creating Debian Package for BDR Coverage Planner ==="

# Check if executables exist
if [ ! -f "${BUILD_DIR}/bdr_coverage_planner" ]; then
    echo "Error: Executable not found at ${BUILD_DIR}/bdr_coverage_planner"
    echo "Please build the application first: ./build.sh"
    exit 1
fi
if [ ! -f "${BUILD_DIR}/bdr-update-runner" ]; then
    echo "Error: OTA runner not found at ${BUILD_DIR}/bdr-update-runner"
    echo "Please build the application first: ./build.sh"
    exit 1
fi

# Clean previous package
rm -rf "$DEB_DIR"
rm -f "$DEB_FILE"

# Create Debian package directory structure
echo "Creating package structure..."
mkdir -p "$DEB_DIR/DEBIAN"
mkdir -p "$DEB_DIR/usr/bin"
mkdir -p "$DEB_DIR/usr/lib/bdr-coverage-planner"
mkdir -p "$DEB_DIR/usr/share/applications"
mkdir -p "$DEB_DIR/usr/share/icons/hicolor/256x256/apps"
mkdir -p "$DEB_DIR/usr/share/icons/hicolor/scalable/apps"
mkdir -p "$DEB_DIR/usr/share/doc/bdr-coverage-planner"
mkdir -p "$DEB_DIR/usr/share/bdr-coverage-planner"

# Create control file
echo "Creating control file..."
cat > "$DEB_DIR/DEBIAN/control" << EOF
Package: bdr-coverage-planner
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: libqt5core5a (>= 5.9.5), libqt5widgets5 (>= 5.9.5), libqt5gui5 (>= 5.9.5), libc6 (>= 2.27), libstdc++6 (>= 6.0), libgcc-s1 (>= 3.0), ros-humble-rclcpp, ros-humble-std-msgs, ros-humble-std-srvs, ros-humble-geometry-msgs, ros-humble-sensor-msgs, ros-humble-nav-msgs, ros-humble-tf2, ros-humble-tf2-geometry-msgs, ros-humble-rmw-cyclonedds-cpp
Maintainer: Andrew Dave <andrew.dave@bdx-robotics.com>
Description: BDR Operator Control Unit (OCU) for autonomous roof scanning
 Operator GUI for "Roofus," an autonomous mobile robot that performs
 building roof scanning. Provides setup, calibration, dashboard, exploration
 and coverage-planning views, and ships an in-app OTA update pipeline.
 Proprietary software of Building Diagnostic Robotics (BDR).
EOF

# Create postinst script
echo "Creating postinst script..."
cat > "$DEB_DIR/DEBIAN/postinst" << 'EOF'
#!/bin/bash
set -e

# Update desktop database
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi

# Update icon cache
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

# Set executable permissions
chmod +x /usr/bin/bdr_coverage_planner
chmod +x /usr/bin/bdr_coverage_planner_launcher
chmod +x /usr/bin/bdr-update-runner
chmod +x /usr/bin/bdr-apply-update

# Phase 8 OTA sudoers drop-in. The wrapper /usr/bin/bdr-apply-update is
# the only binary granted NOPASSWD via this drop-in. Validation via
# `visudo -c` BEFORE we let the file land in /etc/sudoers.d/ — a broken
# sudoers file there can lock the operator out of `sudo` entirely, so we
# stage in /tmp first, validate, then atomically move into place.
SUDOERS_SRC="/usr/share/bdr-coverage-planner/sudoers/bdr-coverage-planner"
SUDOERS_DST="/etc/sudoers.d/bdr-coverage-planner"
if [ -f "$SUDOERS_SRC" ]; then
    SUDOERS_TMP="$(mktemp /tmp/bdr-coverage-planner.sudoers.XXXXXX)"
    cp "$SUDOERS_SRC" "$SUDOERS_TMP"
    chmod 0440 "$SUDOERS_TMP"
    chown root:root "$SUDOERS_TMP"
    if visudo -c -f "$SUDOERS_TMP" >/dev/null 2>&1; then
        mv "$SUDOERS_TMP" "$SUDOERS_DST"
        chmod 0440 "$SUDOERS_DST"
        chown root:root "$SUDOERS_DST"
        echo "[bdr-coverage-planner] OTA sudoers drop-in installed at $SUDOERS_DST"
    else
        rm -f "$SUDOERS_TMP"
        echo "[bdr-coverage-planner] WARNING: sudoers drop-in failed visudo -c; OTA installs will require an interactive sudo password."
    fi
fi

echo "BDR Coverage Planner has been installed successfully!"
echo "You can find it in your applications menu or run 'bdr_coverage_planner_launcher' from the terminal."
EOF
chmod +x "$DEB_DIR/DEBIAN/postinst"

# Create prerm script
echo "Creating prerm script..."
cat > "$DEB_DIR/DEBIAN/prerm" << 'EOF'
#!/bin/bash
set -e

# Remove desktop database entry
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi

# Phase 8 OTA sudoers drop-in cleanup. We remove the file regardless of
# uninstall reason ("remove" or "purge") because leaving a sudoers entry
# pointing at a now-missing wrapper is harmless but misleading. The
# wrapper itself is owned by this package so dpkg removes it before
# prerm runs (actually dpkg removes after prerm — no matter, the file
# still gets deleted either way).
SUDOERS_DST="/etc/sudoers.d/bdr-coverage-planner"
if [ -f "$SUDOERS_DST" ]; then
    rm -f "$SUDOERS_DST"
    echo "[bdr-coverage-planner] removed OTA sudoers drop-in $SUDOERS_DST"
fi
EOF
chmod +x "$DEB_DIR/DEBIAN/prerm"

# Copy executables
echo "Copying executables..."
cp "${BUILD_DIR}/bdr_coverage_planner" "$DEB_DIR/usr/bin/"
# bdr-update-runner sits next to the OCU; the OCU spawns it via its
# applicationDirPath() during the OTA "Install Now" handoff. Phase 7
# locked Q1=A: runner consumes OCU args via CLI flags.
cp "${BUILD_DIR}/bdr-update-runner" "$DEB_DIR/usr/bin/"

# Phase 8: privileged installer wrapper (invoked by the runner via
# `sudo -n /usr/bin/bdr-apply-update install <deb>`). Source lives in
# cpp/scripts/.
SCRIPTS_DIR="$(cd "$(dirname "$0")" && pwd)/scripts"
if [ ! -f "${SCRIPTS_DIR}/bdr-apply-update" ]; then
    echo "Error: OTA installer wrapper missing at ${SCRIPTS_DIR}/bdr-apply-update"
    exit 1
fi
cp "${SCRIPTS_DIR}/bdr-apply-update" "$DEB_DIR/usr/bin/"

# Phase 8: stage the sudoers drop-in source under /usr/share/. The
# postinst validates it via `visudo -c` before atomically moving it to
# /etc/sudoers.d/bdr-coverage-planner. Validating in /tmp first prevents
# a corrupt drop-in from breaking sudo system-wide.
if [ ! -f "${SCRIPTS_DIR}/bdr-coverage-planner.sudoers" ]; then
    echo "Error: sudoers source missing at ${SCRIPTS_DIR}/bdr-coverage-planner.sudoers"
    exit 1
fi
mkdir -p "$DEB_DIR/usr/share/bdr-coverage-planner/sudoers"
cp "${SCRIPTS_DIR}/bdr-coverage-planner.sudoers" \
   "$DEB_DIR/usr/share/bdr-coverage-planner/sudoers/bdr-coverage-planner"

# Create launcher script
echo "Creating launcher script..."
cat > "$DEB_DIR/usr/bin/bdr_coverage_planner_launcher" << 'EOF'
#!/bin/bash
# BDR Coverage Planner Launcher with ROS2 Support

# Source ROS2 environment (try different distributions)
if [ -f "/opt/ros/humble/setup.bash" ]; then
    source /opt/ros/humble/setup.bash
    ROS_DISTRO="humble"
elif [ -f "/opt/ros/foxy/setup.bash" ]; then
    source /opt/ros/foxy/setup.bash
    ROS_DISTRO="foxy"
elif [ -f "/opt/ros/galactic/setup.bash" ]; then
    source /opt/ros/galactic/setup.bash
    ROS_DISTRO="galactic"
elif [ -f "/opt/ros/noetic/setup.bash" ]; then
    source /opt/ros/noetic/setup.bash
    ROS_DISTRO="noetic"
else
    echo "Warning: No ROS2 installation found. Waypoint publishing will not work."
    echo "Please install ROS2 (Humble, Foxy, or Galactic recommended) to enable waypoint publishing features."
fi

# Set basic environment
export DISPLAY=${DISPLAY:-:0}

# CycloneDDS config (loopback-only for local node communication)
# Cross-network communication is handled by Zenoh bridge DDS (managed by laptop_teleop.launch.py)
DDS_CONFIG="$HOME/cyclone_loopback.xml"

# Set CYCLONEDDS_URI if config file exists
if [ -f "$DDS_CONFIG" ]; then
    export CYCLONEDDS_URI="$DDS_CONFIG"
    echo "Using CycloneDDS config (loopback): $DDS_CONFIG"
else
    echo "Warning: CycloneDDS config not found at $DDS_CONFIG"
    echo "Local node communication may not work correctly."
    echo "Expected loopback-only config at: $DDS_CONFIG"
fi

# Check if Zenoh bridge is running (managed by laptop_teleop.launch.py)
if pgrep -x zenohd > /dev/null 2>&1; then
    echo "Zenoh bridge DDS: running (robot communication available)"
else
    echo "Note: Zenoh bridge DDS not running - robot communication unavailable"
    echo "Start with: ros2 launch pilot_control laptop_teleop.launch.py"
fi

# CRITICAL: Set RMW implementation to CycloneDDS (required for communication)
# Without this, ROS2 defaults to FastRTPS which cannot communicate with CycloneDDS nodes
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

# Set ROS domain ID (default 0, matches most robot configurations)
export ROS_DOMAIN_ID=0

# Add bundled library path first (highest priority)
export LD_LIBRARY_PATH="/usr/lib/bdr-coverage-planner:$LD_LIBRARY_PATH"

# Add system ROS paths if available (fallback)
if [ -n "$ROS_DISTRO" ] && [ -d "/opt/ros/$ROS_DISTRO/lib" ]; then
    export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/opt/ros/$ROS_DISTRO/lib:/opt/ros/$ROS_DISTRO/lib/x86_64-linux-gnu:/opt/ros/$ROS_DISTRO/opt/ortools_vendor/lib"
fi

# Add other common library paths
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/usr/local/lib"

# Set Qt platform plugin path - try system Qt first, fallback to bundled
if [ -d "/usr/lib/x86_64-linux-gnu/qt5/plugins/platforms" ]; then
    export QT_QPA_PLATFORM_PLUGIN_PATH="/usr/lib/x86_64-linux-gnu/qt5/plugins/platforms:$QT_QPA_PLATFORM_PLUGIN_PATH"
elif [ -d "/usr/lib/qt5/plugins/platforms" ]; then
    export QT_QPA_PLATFORM_PLUGIN_PATH="/usr/lib/qt5/plugins/platforms:$QT_QPA_PLATFORM_PLUGIN_PATH"
else
    # Fallback to bundled plugins
    export QT_QPA_PLATFORM_PLUGIN_PATH="/usr/lib/bdr-coverage-planner/qt5/plugins/platforms:$QT_QPA_PLATFORM_PLUGIN_PATH"
fi

# Set Qt to use xcb platform (standard Linux desktop)
export QT_QPA_PLATFORM=xcb

# Launch the application
exec "/usr/bin/bdr_coverage_planner" "$@"
EOF
chmod +x "$DEB_DIR/usr/bin/bdr_coverage_planner_launcher"

# Copy required Qt libraries (only the ones not commonly available)
echo "Copying required libraries..."
QT_LIBS=$(ldd "${BUILD_DIR}/bdr_coverage_planner" | grep -E "(Qt5Core|Qt5Widgets|Qt5Gui)" | awk '{print $3}')
for lib in $QT_LIBS; do
    if [ -f "$lib" ]; then
        # Only copy if it's not in standard system locations
        if [[ "$lib" == /usr/local/* ]] || [[ "$lib" == /opt/* ]]; then
            cp "$lib" "$DEB_DIR/usr/lib/bdr-coverage-planner/"
        fi
    fi
done

# Note: Using system Qt platform plugins instead of bundling them
# This avoids conflicts and reduces package size
# The launcher script will find and use system Qt plugins

# Copy Fields2Cover and related libraries (required for functionality)
echo "Copying Fields2Cover libraries..."
if [ -d "/home/avenblake/pilot_ws/install/fields2cover/lib" ]; then
    cp -r /home/avenblake/pilot_ws/install/fields2cover/lib/* "$DEB_DIR/usr/lib/bdr-coverage-planner/" 2>/dev/null || true
fi

# Copy OR-Tools library
echo "Copying OR-Tools library..."
if [ -f "/opt/ros/humble/opt/ortools_vendor/lib/libortools.so.9" ]; then
    cp "/opt/ros/humble/opt/ortools_vendor/lib/libortools.so.9" "$DEB_DIR/usr/lib/bdr-coverage-planner/"
fi

# Bundle the vendored odrive_can interface libs.
#
# odrive_can lives in vendor/odrive_can/ and is built by colcon into
# _ros_ws/install/odrive_can/lib/ (CI does this in the "Build vendored
# ROS2 interface packages" step; locally a developer who's run colcon
# in pilot_ws or BDR_CP will have it under their own _ros_ws/install).
# It's NOT a /opt/ros/humble package, so without explicit bundling the
# OCU fails to load with:
#   error while loading shared libraries:
#   libodrive_can__rosidl_typesupport_cpp.so: cannot open shared object
# This was invisible from a terminal launch on machines with a colcon
# overlay sourced via .bashrc, but desktop-entry launches don't source
# .bashrc and surface the failure.
echo "Copying vendored odrive_can interface libraries..."
# CI uses `colcon build --merge-install`, which flattens all package libs
# into install/lib/. Local developer workflows usually use the default
# (per-package) layout install/<pkg>/lib/. We try both.
ODRIVE_CAN_LIB_CANDIDATES=(
    "${SCRIPT_DIR}/../_ros_ws/install/lib"
    "${SCRIPT_DIR}/../_ros_ws/install/odrive_can/lib"
    "${HOME}/pilot_ws/install/lib"
    "${HOME}/pilot_ws/install/odrive_can/lib"
    "${HOME}/BDR_CP/_ros_ws/install/lib"
    "${HOME}/BDR_CP/_ros_ws/install/odrive_can/lib"
)
for ODRIVE_CAN_LIB_DIR in "${ODRIVE_CAN_LIB_CANDIDATES[@]}"; do
    if [ -d "$ODRIVE_CAN_LIB_DIR" ] && \
       ls "$ODRIVE_CAN_LIB_DIR"/libodrive_can*.so* >/dev/null 2>&1; then
        cp -P "$ODRIVE_CAN_LIB_DIR"/libodrive_can*.so* \
              "$DEB_DIR/usr/lib/bdr-coverage-planner/"
        echo "  bundled odrive_can libs from: $ODRIVE_CAN_LIB_DIR"
        break
    fi
done
if ! ls "$DEB_DIR/usr/lib/bdr-coverage-planner/"libodrive_can*.so* >/dev/null 2>&1; then
    echo "Error: failed to bundle odrive_can libs; OCU will fail to launch on clean systems."
    echo "       Looked under: ${ODRIVE_CAN_LIB_CANDIDATES[*]}"
    exit 1
fi

# Copy other required libraries (only non-standard ones)
REQUIRED_LIBS=$(ldd "${BUILD_DIR}/bdr_coverage_planner" | grep -v "=>" | grep -v "linux-vdso" | grep -v "ld-linux" | awk '{print $1}')
for lib in $REQUIRED_LIBS; do
    LIB_PATH=$(ldd "${BUILD_DIR}/bdr_coverage_planner" | grep "$lib" | awk '{print $3}')
    if [ -f "$LIB_PATH" ] && [ "$LIB_PATH" != "" ]; then
        # Only copy non-standard libraries
        if [[ "$LIB_PATH" == /usr/local/* ]] || [[ "$LIB_PATH" == /opt/* ]] || [[ "$LIB_PATH" == /home/avenblake/pilot_ws/* ]]; then
            cp "$LIB_PATH" "$DEB_DIR/usr/lib/bdr-coverage-planner/"
        fi
    fi
done

# Create desktop file
echo "Creating desktop entry..."
cat > "$DEB_DIR/usr/share/applications/bdr_coverage_planner.desktop" << EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=BDR Coverage Planner
Comment=Coverage path planning GUI (Fields2Cover)
Exec=/usr/bin/bdr_coverage_planner_launcher
Icon=bdr_coverage_planner
Terminal=false
Categories=Development;Engineering;
StartupWMClass=bdr_coverage_planner
EOF
chmod +x "$DEB_DIR/usr/share/applications/bdr_coverage_planner.desktop"

# Create icon
echo "Installing application icon..."
ASSET_PNG="${SCRIPT_DIR}/assets/bdr_logo.png"
ASSET_SVG="${SCRIPT_DIR}/assets/bdr_logo.svg"
ICON_PNG_DEST="$DEB_DIR/usr/share/icons/hicolor/256x256/apps/bdr_coverage_planner.png"
ICON_SVG_DEST="$DEB_DIR/usr/share/icons/hicolor/scalable/apps/bdr_coverage_planner.svg"

# Prefer PNG for consistent, non-distorted rendering. If PNG is missing, render from SVG.
if [ -f "$ASSET_PNG" ]; then
    cp "$ASSET_PNG" "$ICON_PNG_DEST"
elif [ -f "$ASSET_SVG" ] && command -v convert >/dev/null 2>&1; then
    # Trim extra whitespace, then resize uniformly, then pad to square.
    convert -background none -density 384 "$ASSET_SVG" -trim +repage -resize 256x256 -gravity center -extent 256x256 \
        "$ICON_PNG_DEST" 2>/dev/null || true
fi

# Final fallback: generate a simple text icon if rendering failed.
if [ ! -f "$ICON_PNG_DEST" ]; then
    if command -v convert >/dev/null 2>&1; then
        convert -size 256x256 xc:"#111111" -fill white -pointsize 64 -gravity center -annotate +0+0 "BDR" \
            "$ICON_PNG_DEST" 2>/dev/null || true
    fi
    if [ ! -f "$ICON_PNG_DEST" ]; then
        cat > "$ICON_SVG_DEST" << 'EOF'
<svg width="256" height="256" xmlns="http://www.w3.org/2000/svg">
  <rect width="256" height="256" fill="#111111"/>
  <text x="128" y="148" font-family="Arial" font-size="64" fill="white" text-anchor="middle">BDR</text>
</svg>
EOF
    fi
fi

# Create copyright file
echo "Creating copyright file..."
cat > "$DEB_DIR/usr/share/doc/bdr-coverage-planner/copyright" << EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: bdr-coverage-planner
Upstream-Contact: Andrew Dave <andrew.dave@bdx-robotics.com>
Source: https://github.com/Building-Diagnostic-Robotics/BDR_OCU

Files: *
Copyright: $(date +%Y) Building Diagnostic Robotics. All rights reserved.
License: Proprietary
 This software is proprietary to Building Diagnostic Robotics (BDR).
 .
 All rights reserved. Unauthorized reproduction, distribution, or
 modification of this software, in whole or in part, is strictly
 prohibited. This software is licensed solely for use within BDR
 operations. No license, express or implied, is granted to any third
 party.
 .
 For licensing inquiries, contact: andrew.dave@bdx-robotics.com
EOF

# Create changelog
echo "Creating changelog..."
cat > "$DEB_DIR/usr/share/doc/bdr-coverage-planner/changelog.Debian" << EOF
bdr-coverage-planner ($VERSION) unstable; urgency=medium

  * Initial release

 -- Andrew Dave <andrew.dave@bdx-robotics.com>  $(date -R)
EOF
gzip "$DEB_DIR/usr/share/doc/bdr-coverage-planner/changelog.Debian"

# Set permissions
echo "Setting permissions..."
find "$DEB_DIR" -type d -exec chmod 755 {} \;
find "$DEB_DIR" -type f -exec chmod 644 {} \;
chmod +x "$DEB_DIR/usr/bin/"*
chmod 755 "$DEB_DIR/DEBIAN/postinst"
chmod 755 "$DEB_DIR/DEBIAN/prerm"

# Build the package
echo "Building Debian package..."
dpkg-deb --root-owner-group --build "$DEB_DIR" "$DEB_FILE"

echo "=== Debian Package Created Successfully ==="
echo "Package: $DEB_FILE"
echo "Size: $(du -h "$DEB_FILE" | cut -f1)"
echo ""
echo "To install:"
echo "  sudo dpkg -i $DEB_FILE"
echo "  sudo apt-get install -f  # Install any missing dependencies"
echo ""
echo "To test installation:"
echo "  bdr_coverage_planner_launcher"
echo ""
echo "Package contents:"
dpkg-deb -c "$DEB_FILE" 2>/dev/null | head -10
