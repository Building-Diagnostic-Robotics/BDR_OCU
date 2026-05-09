# BDR_OCU — BDR Coverage Planning Suite

C++ Qt/ROS2 desktop application (the Operator Control Unit) for "Roofus,"
an autonomous mobile robot that performs building roof scanning.

## Structure

- `cpp/` — C++ Qt/ROS2 desktop application (staged flow + planner code).
- `App resource/` — design / exported SVG assets used during UI development.
- `docs/OTA.md` — OTA update pipeline reference (architecture, marker
  states, field-test recipes).

## Build & run (from source)

From the repo root:

```bash
cd cpp
./build.sh
./build/bdr_coverage_planner
```

## Install on an OCU machine

Each push to `main` builds a Debian package and publishes it to the rolling
`latest` release on GitHub. To install on a target machine:

```bash
gh release download latest -R Building-Diagnostic-Robotics/BDR_OCU --pattern '*.deb'
sudo dpkg -i ./bdr-coverage-planner_*_amd64.deb
sudo apt -f install -y
```

After this first install, in-app OTA takes over — every subsequent release
appears as a banner on the dashboard.

If `gh` is unavailable, fetch the asset URL via the GitHub API and `wget` it:

```bash
DEB_URL=$(curl -s https://api.github.com/repos/Building-Diagnostic-Robotics/BDR_OCU/releases/tags/latest \
  | grep '"browser_download_url".*\.deb"' | cut -d'"' -f4)
wget "$DEB_URL"
sudo dpkg -i ./bdr-coverage-planner_*_amd64.deb
sudo apt -f install -y
```

For private-repo access, prepend `-H "Authorization: token <PAT>"` to
`curl` and `--header="Authorization: token <PAT>"` to `wget`.
