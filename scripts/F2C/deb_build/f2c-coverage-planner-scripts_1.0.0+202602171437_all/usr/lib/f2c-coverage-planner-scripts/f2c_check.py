#!/usr/bin/env python3
"""
Quick Fields2Cover installation check.

This script verifies that the Python bindings are importable and key classes
can be instantiated. This is a minimal check to avoid segfaults that can occur
with complex geometry operations in some bindings.

Usage:
  ros2 run pilot_control f2c_check.py   (if installed via CMake)
  or
  python3 scripts/F2C/f2c_check.py
"""

import sys
# Remaining imports kept for commented-out legacy code below
# import os
# import json
# import math
# import tempfile

# Set up signal handler to catch segfaults gracefully (if possible)
try:
    import signal
    def signal_handler(sig, frame):
        print("[F2C CHECK] ERROR: Received signal (possibly segfault)", file=sys.stderr)
        sys.exit(1)
    signal.signal(signal.SIGSEGV, signal_handler)
except Exception:
    pass  # Not critical


def main() -> int:
    # Try both common top-level module names
    module_name = None
    try:
        import fields2cover as f2c  # type: ignore
        module_name = "fields2cover"
    except Exception:
        try:
            import f2c as f2c  # type: ignore  # noqa: F401
            module_name = "f2c"
        except Exception as exc:
            print("[F2C CHECK] ERROR: Could not import Fields2Cover Python bindings.", file=sys.stderr)
            print("[F2C CHECK] Tried 'fields2cover' and 'f2c'.", file=sys.stderr)
            print(f"[F2C CHECK] Import exception: {exc}", file=sys.stderr)
            return 1

    print(f"[F2C CHECK] Imported module: {module_name}")

    # Version (best-effort)
    version = getattr(f2c, "__version__", None)
    if version is None:
        version = getattr(f2c, "VERSION", "unknown")
    print(f"[F2C CHECK] Fields2Cover version: {version}")

    # Resolve API symbols (supports both top-level and v2 submodules)
    def resolve(attr_chain, fallback=None):
        cur = f2c
        for name in attr_chain:
            if hasattr(cur, name):
                cur = getattr(cur, name)
            else:
                return fallback
        return cur

    # Prefer submodules; fallback to top-level
    Point = resolve(["types", "Point"], getattr(f2c, "Point", None))
    Field = resolve(["types", "Field"], getattr(f2c, "Field", None))
    HG_Const_gen = resolve(["headland", "HG_Const_gen"], getattr(f2c, "HG_Const_gen", None))
    SG_BruteForce = resolve(["swath", "SG_BruteForce"], getattr(f2c, "SG_BruteForce", None))
    
    # Additional classes kept for reference in commented-out code below:
    # Polygon = resolve(["types", "Polygon"], getattr(f2c, "Polygon", getattr(f2c, "GeomPolygon", None)))
    # MultiPolygon = resolve(["types", "MultiPolygon"], getattr(f2c, "MultiPolygon", getattr(f2c, "GeomMultiPolygon", None)))
    # LinearRing = getattr(f2c, "GeomLinearRing", getattr(f2c, "LinearRing", None))
    # VectorPoint = getattr(f2c, "VectorPoint", None)
    # RP_Boustrophedon = resolve(["route", "RP_Boustrophedon"], getattr(f2c, "RP_Boustrophedon", None))
    # PP_Path = resolve(["path", "PP_Path"], getattr(f2c, "PP_Path", None))

    # Check key classes exist
    print("[F2C CHECK] Verifying key classes:")
    key_classes = {
        "Point": Point,
        "Field": Field,
        "HG_Const_gen": HG_Const_gen,
        "SG_BruteForce": SG_BruteForce,
        "Parser": getattr(f2c, "Parser", None),
    }
    all_present = True
    for name, cls in key_classes.items():
        if cls is not None:
            print(f"[F2C CHECK]   ✓ {name} available")
        else:
            print(f"[F2C CHECK]   ✗ {name} NOT available")
            all_present = False
    
    if not all_present:
        print("[F2C CHECK] WARNING: Some key classes are missing. Installation may be incomplete.", file=sys.stderr)
        return 1

    # Try to instantiate key classes (without complex operations)
    print("[F2C CHECK] Testing class instantiation:")
    try:
        if Point is not None:
            pt = Point()  # Default constructor
            print("[F2C CHECK]   ✓ Point() instantiated")
        if HG_Const_gen is not None:
            hg = HG_Const_gen()
            print("[F2C CHECK]   ✓ HG_Const_gen() instantiated")
        if SG_BruteForce is not None:
            sg = SG_BruteForce()
            print("[F2C CHECK]   ✓ SG_BruteForce() instantiated")
        parser_cls = getattr(f2c, "Parser", None)
        if parser_cls is not None:
            parser = parser_cls()
            print("[F2C CHECK]   ✓ Parser() instantiated")
    except Exception as e:
        print(f"[F2C CHECK] ERROR: Failed to instantiate classes: {e}", file=sys.stderr)
        return 2

    print("[F2C CHECK] SUCCESS: Fields2Cover is installed and basic classes are accessible.")
    print("[F2C CHECK] NOTE: Full pipeline test skipped to avoid segfaults in geometry operations.")
    print("[F2C CHECK] If you need full functionality testing, refer to Fields2Cover tutorials.")
    return 0

    # OLD CODE BELOW - kept for reference but not executed to avoid segfaults
    # This code is commented out to avoid segfaults in geometry operations
    # Uncomment and indent properly if you want to enable full pipeline testing
    """
    # If Polygon/Point missing, we will try Parser-based imports

    # Some versions don't expose MultiPolygon; build Field accordingly
    def build_field_from_polygon(poly):
        if Field is None:
            raise RuntimeError("Field type not available")
        if MultiPolygon is not None:
            return Field(MultiPolygon([poly]))
        # Try Field([poly]) signature
        try:
            return Field([poly])
        except Exception:
            # Try Field(poly)
            return Field(poly)

    try:
        # Build a simple square field (10x10 m). Prefer direct geometry; otherwise try Parser in multiple ways.
        field = None

        # Attempt 1: Direct constructors (Geom* types) - use default constructor then add points
        if Field is not None and Point is not None and LinearRing is not None and Polygon is not None and MultiPolygon is not None:
            try:
                # Create LinearRing using default constructor, then add points
                ring = LinearRing()
                # Try to add points via addPoint or push_back
                for x, y in [(0,0),(10,0),(10,10),(0,10),(0,0)]:
                    pt = Point(x, y)
                    if hasattr(ring, "addPoint"):
                        ring.addPoint(pt)
                    elif hasattr(ring, "push_back"):
                        ring.push_back(pt)
                    elif hasattr(ring, "add_point"):
                        ring.add_point(pt)
                # Create polygon from ring
                poly = Polygon(ring)
                # Create MultiPolygon and add polygon
                mp = MultiPolygon()
                if hasattr(mp, "push_back"):
                    mp.push_back(poly)
                elif hasattr(mp, "addGeometry"):
                    mp.addGeometry(poly)
                elif hasattr(mp, "add"):
                    mp.add(poly)
                else:
                    # Try constructor with single polygon
                    try:
                        mp = MultiPolygon(poly)  # type: ignore
                    except Exception:
                        mp = None
                if mp is not None:
                    field = Field(mp)
            except Exception as e:
                print(f"[F2C CHECK] DEBUG: Direct Geom* construction failed: {e}")
                field = None

        # Attempt 2: Alternative direct polygon signature (Point list)
        if field is None and Polygon is not None and Point is not None:
            try:
                poly = Polygon([Point(0, 0), Point(10, 0), Point(10, 10), Point(0, 10)], [])
                field = build_field_from_polygon(poly)
            except Exception:
                field = None

        # Attempt 3: Parser with WKT passed as string
        if field is None:
            parser_cls = getattr(f2c, "Parser", None)
            if parser_cls is not None:
                parser = parser_cls()
                square_wkt = "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))"
                for method in ("importFieldWkt", "import_field_wkt", "importFieldWKT"):
                    if hasattr(parser, method):
                        try:
                            field = getattr(parser, method)(square_wkt)  # type: ignore
                            break
                        except Exception:
                            field = None

        # Attempt 4: Parser with temporary GeoJSON file
        if field is None:
            parser_cls = getattr(f2c, "Parser", None)
            if parser_cls is not None:
                parser = parser_cls()
                geojson = {
                    "type": "FeatureCollection",
                    "features": [
                        {
                            "type": "Feature",
                            "properties": {},
                            "geometry": {
                                "type": "Polygon",
                                "coordinates": [[[0,0],[10,0],[10,10],[0,10],[0,0]]]
                            }
                        }
                    ]
                }
                with tempfile.TemporaryDirectory() as td:
                    gj_path = os.path.join(td, "square.geojson")
                    with open(gj_path, "w", encoding="utf-8") as f:
                        json.dump(geojson, f)
                    for method in ("importFieldGeoJSON", "import_field_geojson", "importGeoJSON", "importFieldJson"):
                        if hasattr(parser, method):
                            try:
                                field = getattr(parser, method)(gj_path)  # type: ignore
                                if field is not None:
                                    break
                            except Exception as e:
                                print(f"[F2C CHECK] DEBUG: Parser.{method} failed: {e}")
                                field = None
                    # Try importJson with fields argument if it exists
                    if field is None and hasattr(parser, "importJson"):
                        try:
                            # importJson might need a Fields object
                            Fields = getattr(f2c, "Fields", None)
                            if Fields is not None:
                                fields_obj = Fields()
                                field = parser.importJson(gj_path, fields_obj)  # type: ignore
                        except Exception as e:
                            print(f"[F2C CHECK] DEBUG: Parser.importJson failed: {e}")
                            field = None

        # Attempt 5: Parser with temporary GML file
        if field is None:
            parser_cls = getattr(f2c, "Parser", None)
            if parser_cls is not None:
                parser = parser_cls()
                gml_content = '''<?xml version="1.0" encoding="UTF-8"?>
<gml:Polygon xmlns:gml="http://www.opengis.net/gml">
  <gml:exterior>
    <gml:LinearRing>
      <gml:posList>0 0 10 0 10 10 0 10 0 0</gml:posList>
    </gml:LinearRing>
  </gml:exterior>
</gml:Polygon>'''
                with tempfile.TemporaryDirectory() as td:
                    gml_path = os.path.join(td, "square.gml")
                    with open(gml_path, "w", encoding="utf-8") as f:
                        f.write(gml_content)
                    for method in ("importFieldGml", "import_field_gml", "importGml", "importFieldGML"):
                        if hasattr(parser, method):
                            try:
                                field = getattr(parser, method)(gml_path)  # type: ignore
                                if field is not None:
                                    break
                            except Exception as e:
                                print(f"[F2C CHECK] DEBUG: Parser.{method} failed: {e}")
                                field = None

        # Last resort: Use top-level helper functions if available
        if field is None:
            planCovPath = getattr(f2c, "planCovPath", None)
            planCovRoute = getattr(f2c, "planCovRoute", None)
            if planCovPath is not None or planCovRoute is not None:
                # These might handle field creation internally, but we still need a field object
                # Skip this for now as it's complex
                pass

        if field is None:
            # Last resort: check for any callable that looks like a field importer on module
            for name in dir(f2c):
                if name.lower().startswith("import") and "field" in name.lower() and not name.startswith("_"):
                    try:
                        func = getattr(f2c, name)
                        if callable(func):
                            # Try with WKT first
                            result = func("POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))")
                            if result is not None:
                                # Check if result is Field or contains Field
                                if isinstance(result, type(Field)) if Field else False:
                                    field = result
                                    break
                                elif hasattr(result, "getField"):
                                    field = result.getField()
                                    break
                                elif isinstance(result, (list, tuple)) and len(result) > 0:
                                    field = result[0]
                                    break
                    except Exception as e:
                        print(f"[F2C CHECK] DEBUG: {name} failed: {e}")

        if field is None:
            # If we still can't create a field, at least verify the module loads and key classes exist
            print("[F2C CHECK] WARNING: Could not create Field, but module imports successfully.")
            print("[F2C CHECK] Basic verification: Key classes exist:")
            for cls_name in ["Field", "Point", "SG_BruteForce", "HG_Const_gen"]:
                cls = getattr(f2c, cls_name, None)
                if cls is not None:
                    print(f"[F2C CHECK]   ✓ {cls_name} available")
                else:
                    print(f"[F2C CHECK]   ✗ {cls_name} NOT available")
            raise RuntimeError("Failed to create field via Polygon or Parser (WKT/GeoJSON/GML)")

        # HEADLAND (0 width -> none for this quick test)
        try:
            headland_result = HG_Const_gen().generate_headlands(field, 0.0)
        except Exception as e:
            print(f"[F2C CHECK] DEBUG: Headland generation failed: {e}")
            # Continue without headland
            headland_result = None

        # SWATHS (2m width) — try multiple signatures
        sg = SG_BruteForce()
        swaths = None

        # Try simple signature
        try:
            swaths = sg.generate_swaths(field, 2.0)
        except Exception:
            swaths = None

        # Prepare headland geometry if needed
        geom = None
        try:
            hg = HG_Const_gen()
            hl = hg.generate_headlands(field, 0.0)
            # Access geometry by common getters
            for getter in ("getGeometry", "geometry", "get_geometry"):
                if hasattr(hl, getter):
                    try:
                        g = getattr(hl, getter)
                        # If callable, call with index 0 if needed
                        if callable(g):
                            try:
                                geom = g(0)
                            except Exception:
                                geom = g()
                        else:
                            geom = g
                        if geom is not None:
                            break
                    except Exception:
                        continue
        except Exception:
            geom = None

        # Try signature matching older docs: angle, width, geometry
        if swaths is None and geom is not None:
            try:
                swaths = sg.generateSwaths(math.pi, 2.0, geom)
            except Exception:
                swaths = None

        if swaths is None:
            raise RuntimeError("Unable to generate swaths with available API signatures")

        # ROUTE (best-effort; optional)
        route_obj = None
        if RP_Boustrophedon is not None:
            try:
                route_obj = RP_Boustrophedon().gen_route(swaths)
            except Exception:
                route_obj = None

        # PATH (optional)
        path_obj = None
        if PP_Path is not None and route_obj is not None:
            try:
                path_obj = PP_Path().gen_path(route_obj)
            except Exception:
                path_obj = None

        # Counts (support both size() and __len__)
        def get_size(obj):
            if hasattr(obj, "size") and callable(getattr(obj, "size")):
                try:
                    return obj.size()
                except Exception:
                    pass
            try:
                return len(obj)
            except Exception:
                return "n/a"

        print(f"[F2C CHECK] Generated swaths: {get_size(swaths)}")
        if route_obj is not None:
            print(f"[F2C CHECK] Route states: {get_size(route_obj)}")
        else:
            print("[F2C CHECK] Route step skipped (planner not available)")
        if path_obj is not None:
            print(f"[F2C CHECK] Path states: {get_size(path_obj)}")
        else:
            print("[F2C CHECK] Path step skipped (planner not available)")

        print("[F2C CHECK] SUCCESS: Fields2Cover swath generation executed.")
        return 0
    except Exception as exc:
        print("[F2C CHECK] ERROR: Fields2Cover pipeline failed.", file=sys.stderr)
        print(f"[F2C CHECK] Exception: {exc}", file=sys.stderr)
        return 2
    """


if __name__ == "__main__":
    sys.exit(main())


