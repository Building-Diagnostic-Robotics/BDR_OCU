#!/bin/bash
# Create Debian package for BDR Coverage Planner (Raj system profile)
#
# Goals:
# - Keep the shared create_deb.sh unchanged for collaborators
# - Bundle non-standard runtime libs from this machine (primarily /usr/local/lib)
# - Rely on system ROS installation for /opt/ros/<distro> libs
#
# Usage:
#   ./create_deb_raj.sh
#   VERSION=1.0.1 ./create_deb_raj.sh
#   BUILD_DIR=./build ./create_deb_raj.sh
#
set -euo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"

PACKAGE_NAME="${PACKAGE_NAME:-bdr-coverage-planner}"
VERSION="${VERSION:-1.0.0}"
ARCH="${ARCH:-$(dpkg --print-architecture)}"

DEB_DIR="${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}"
DEB_FILE="${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"

APP_BIN="${BUILD_DIR}/bdr_coverage_planner"

MAINTAINER_NAME="${DEBFULLNAME:-${MAINTAINER_NAME:-Raj}}"
MAINTAINER_EMAIL="${DEBEMAIL:-${MAINTAINER_EMAIL:-raj@example.com}}"

echo "=== Creating Debian Package (${PACKAGE_NAME}) [Raj profile] ==="
echo "Build dir: ${BUILD_DIR}"
echo "Version  : ${VERSION}"
echo "Arch     : ${ARCH}"
echo

if [ ! -f "${APP_BIN}" ]; then
  echo "Error: Executable not found at ${APP_BIN}" >&2
  echo "Build first, e.g.: cmake --build \"${BUILD_DIR}\" -j\$(nproc)" >&2
  exit 1
fi

rm -rf "${DEB_DIR}"
rm -f "${DEB_FILE}"

echo "Creating package structure..."
mkdir -p "${DEB_DIR}/DEBIAN"
mkdir -p "${DEB_DIR}/usr/bin"
mkdir -p "${DEB_DIR}/usr/lib/bdr-coverage-planner"
mkdir -p "${DEB_DIR}/usr/share/applications"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/256x256/apps"
mkdir -p "${DEB_DIR}/usr/share/icons/hicolor/scalable/apps"
mkdir -p "${DEB_DIR}/usr/share/doc/bdr-coverage-planner"

echo "Creating control file..."
cat > "${DEB_DIR}/DEBIAN/control" <<EOF
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libqt5core5a (>= 5.9.5), libqt5widgets5 (>= 5.9.5), libqt5gui5 (>= 5.9.5), libc6 (>= 2.27), libstdc++6 (>= 6.0), libgcc-s1 (>= 3.0), ros-humble-rclcpp, ros-humble-std-msgs
Maintainer: ${MAINTAINER_NAME} <${MAINTAINER_EMAIL}>
Description: Coverage Path Planning GUI with ROS2 Integration
 BDR Coverage Planner: Fields2Cover-based coverage planning GUI with ROS2 integration.
EOF

echo "Creating postinst/prerm..."
cat > "${DEB_DIR}/DEBIAN/postinst" <<'EOF'
#!/bin/bash
set -e

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

chmod +x /usr/bin/bdr_coverage_planner
chmod +x /usr/bin/bdr_coverage_planner_launcher
EOF
chmod 755 "${DEB_DIR}/DEBIAN/postinst"

cat > "${DEB_DIR}/DEBIAN/prerm" <<'EOF'
#!/bin/bash
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
EOF
chmod 755 "${DEB_DIR}/DEBIAN/prerm"

echo "Copying executable..."
cp "${APP_BIN}" "${DEB_DIR}/usr/bin/"

echo "Creating launcher script..."
cat > "${DEB_DIR}/usr/bin/bdr_coverage_planner_launcher" <<'EOF'
#!/bin/bash
# BDR Coverage Planner Launcher (Raj profile)
set -e

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
  echo "Warning: No ROS installation found. ROS features will be unavailable." >&2
fi

export DISPLAY=${DISPLAY:-:0}
export RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-rmw_cyclonedds_cpp}
export ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-0}

# Ensure logs are writable even if ~/.ros has bad permissions
export ROS_LOG_DIR=${ROS_LOG_DIR:-/tmp/ros_logs}
mkdir -p "${ROS_LOG_DIR}" >/dev/null 2>&1 || true

# Prefer bundled libs shipped with this package
export LD_LIBRARY_PATH="/usr/lib/bdr-coverage-planner:${LD_LIBRARY_PATH:-}"

# Include /usr/local/lib on dev machines (Fields2Cover often installed there)
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/usr/local/lib"

if [ -n "${ROS_DISTRO:-}" ] && [ -d "/opt/ros/${ROS_DISTRO}/lib" ]; then
  export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/opt/ros/${ROS_DISTRO}/lib:/opt/ros/${ROS_DISTRO}/lib/x86_64-linux-gnu"
fi

# Qt platform plugins
if [ -d "/usr/lib/x86_64-linux-gnu/qt5/plugins/platforms" ]; then
  export QT_QPA_PLATFORM_PLUGIN_PATH="/usr/lib/x86_64-linux-gnu/qt5/plugins/platforms:${QT_QPA_PLATFORM_PLUGIN_PATH:-}"
elif [ -d "/usr/lib/qt5/plugins/platforms" ]; then
  export QT_QPA_PLATFORM_PLUGIN_PATH="/usr/lib/qt5/plugins/platforms:${QT_QPA_PLATFORM_PLUGIN_PATH:-}"
fi

export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}
exec /usr/bin/bdr_coverage_planner "$@"
EOF
chmod 755 "${DEB_DIR}/usr/bin/bdr_coverage_planner_launcher"

echo "Bundling non-standard shared libraries from this machine..."
#
# We bundle only libraries under /usr/local or /opt. Everything under /lib or /usr/lib
# is assumed to be provided by the target OS packages.
#
copy_if_nonstandard() {
  local p="$1"
  if [ -f "$p" ]; then
    case "$p" in
      /usr/local/*|/opt/*)
        cp -n "$p" "${DEB_DIR}/usr/lib/bdr-coverage-planner/" 2>/dev/null || true
        ;;
    esac
  fi
}

declare -A seen=()
queue=("${APP_BIN}")
while [ "${#queue[@]}" -gt 0 ]; do
  f="${queue[0]}"
  queue=("${queue[@]:1}")

  if [ -n "${seen[$f]+x}" ]; then
    continue
  fi
  seen["$f"]=1

  # Resolved paths: "... => /path/to/lib (0x...)"
  while read -r dep; do
    copy_if_nonstandard "$dep"
    # Recurse into copied libs to pull in their deps too
    case "$dep" in
      /usr/local/*|/opt/*)
        queue+=("$dep")
        ;;
    esac
  done < <(ldd "$f" 2>/dev/null | awk '/=> \// {print $3}' | sort -u)
done

echo "Creating desktop entry..."
cat > "${DEB_DIR}/usr/share/applications/bdr_coverage_planner.desktop" <<EOF
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

echo "Installing application icon..."
ASSET_PNG="${SCRIPT_DIR}/assets/bdr_logo.png"
ASSET_SVG="${SCRIPT_DIR}/assets/bdr_logo.svg"
ICON_PNG_DEST="${DEB_DIR}/usr/share/icons/hicolor/256x256/apps/bdr_coverage_planner.png"
ICON_SVG_DEST="${DEB_DIR}/usr/share/icons/hicolor/scalable/apps/bdr_coverage_planner.svg"

if [ -f "${ASSET_PNG}" ]; then
  cp "${ASSET_PNG}" "${ICON_PNG_DEST}"
elif [ -f "${ASSET_SVG}" ]; then
  cp "${ASSET_SVG}" "${ICON_SVG_DEST}"
fi

echo "Creating changelog/copyright..."
cat > "${DEB_DIR}/usr/share/doc/bdr-coverage-planner/changelog.Debian" <<EOF
${PACKAGE_NAME} (${VERSION}) unstable; urgency=medium

  * Local build

 -- ${MAINTAINER_NAME} <${MAINTAINER_EMAIL}>  $(date -R)
EOF
gzip "${DEB_DIR}/usr/share/doc/bdr-coverage-planner/changelog.Debian"

cat > "${DEB_DIR}/usr/share/doc/bdr-coverage-planner/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: ${PACKAGE_NAME}

Files: *
Copyright: $(date +%Y) ${MAINTAINER_NAME}
License: Proprietary
EOF

echo "Setting permissions..."
find "${DEB_DIR}" -type d -exec chmod 755 {} \;
find "${DEB_DIR}" -type f -exec chmod 644 {} \;
chmod +x "${DEB_DIR}/usr/bin/"*
chmod 755 "${DEB_DIR}/DEBIAN/postinst" "${DEB_DIR}/DEBIAN/prerm"

echo "Building Debian package..."
dpkg-deb --root-owner-group --build "${DEB_DIR}" "${DEB_FILE}"

echo
echo "=== Debian Package Created Successfully ==="
echo "Package: ${DEB_FILE}"
echo "Size: $(du -h "${DEB_FILE}" | cut -f1)"
echo
echo "To install:"
echo "  sudo dpkg -i ${DEB_FILE}"
echo "  sudo apt-get install -f"
