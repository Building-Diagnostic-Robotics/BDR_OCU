# Fields2Cover Decomposition Guide

## Overview

Decomposition is a powerful feature in Fields2Cover v2.0 that splits concave fields (or fields with obstacles) into multiple convex sub-fields. This enables better coverage path planning for complex geometries.

## When to Use Decomposition

Decomposition is particularly useful for:

1. **Concave Fields**: Fields with indentations or "C-shaped" geometries
2. **Fields with Obstacles**: Fields containing no-go zones or obstacles
3. **Complex Boundaries**: Fields with irregular, non-convex shapes
4. **Multi-Region Fields**: Fields that naturally consist of multiple separate areas

## How It Works

### Detection

The system automatically detects if a field is concave by:
- Checking if the polygon has interior angles > 180 degrees
- Detecting if the polygon has holes (obstacles)
- Comparing polygon area to its convex hull area

### Decomposition Methods

#### 1. Boustrophedon Decomposition (`DC_Boustrophedon`)

- Creates strips parallel to a chosen direction
- Good for fields with linear obstacles or indentations
- Produces rectangular-like sub-regions
- **Best for**: Fields with parallel obstacles or linear features

#### 2. Trapezoidal Decomposition (`DC_Trapezoidal`)

- Creates trapezoidal regions
- More flexible than Boustrophedon
- Can handle more complex geometries
- **Best for**: General concave fields with irregular shapes

### Processing Flow

```
Field (Concave)
    ↓
Decomposition Detection
    ↓
Split into Convex Sub-fields
    ↓
Process Each Sub-field:
  - Generate headlands
  - Generate swaths
  - Combine swaths
    ↓
Generate Unified Route
    ↓
Generate Unified Path
```

## Usage in GUI

1. **Enable Decomposition:**
   - Check "Use decomposition (for concave fields)" checkbox
   - Located in the "Fields2Cover & Coverage" section

2. **Select Method:**
   - Choose "Boustrophedon" or "Trapezoidal" from dropdown
   - Default: Boustrophedon

3. **Generate Coverage:**
   - Click "Generate Swaths", "Generate Route", or "Generate Path"
   - Decomposition is automatically applied if field is concave

## Usage in Code

```python
from f2c_pipeline import generate_coverage
import fields2cover as f2c
from shapely.geometry import Polygon

# Create field
field = f2c.Field(...)
poly = Polygon(...)

# Configure with decomposition
cfg = {
    "swath_width": 1.0,
    "headland_width": 1.0,
    "turn_radius": 0.5,
    "use_decomposition": True,  # Enable decomposition
    "decomposition_type": "boustrophedon",  # or "trapezoidal"
    # ... other config options
}

# Generate coverage
swaths, route, path = generate_coverage(field, poly, cfg)
```

## Performance Considerations

### Advantages:
- ✅ Better coverage for concave fields
- ✅ More optimal swath patterns
- ✅ Handles obstacles naturally
- ✅ Produces shorter routes in many cases

### Trade-offs:
- ⚠️ Slightly slower (processes multiple sub-fields)
- ⚠️ May produce more swaths (but better organized)
- ⚠️ Requires F2C v2.0+ with decomposition support

### When NOT to Use:
- Simple convex fields (adds unnecessary overhead)
- Very small fields (decomposition overhead not worth it)
- Fields that are already well-handled without decomposition

## Technical Details

### Concavity Detection

The system uses multiple heuristics:

1. **Interior Holes**: If polygon has `interiors`, it's considered concave
2. **Convex Hull Comparison**: If `area < convex_hull_area * 0.95`, field is concave
3. **Manual Override**: User can force decomposition via config

### Sub-field Processing

Each decomposed sub-field is processed independently:
- Headlands generated per sub-field
- Swaths generated per sub-field
- All swaths combined into single collection
- Route planning considers all swaths together
- Path planning creates unified smooth path

### Error Handling

- If decomposition fails, falls back to normal processing
- Warnings issued but processing continues
- Graceful degradation if F2C version doesn't support decomposition

## Examples

### Example 1: C-Shaped Field

```
Before Decomposition:
┌─────────────┐
│             │
│  ┌───┐      │
│  │   │      │  ← Concave region
│  └───┘      │
│             │
└─────────────┘

After Boustrophedon Decomposition:
┌─────────────┐
│  Sub-field 1│
│  ┌───┐      │
│  │   │      │
│  └───┘      │
│  Sub-field 2│
└─────────────┘
```

### Example 2: Field with Obstacle

```
Before:
┌─────────────┐
│             │
│  ┌───┐      │  ← Obstacle
│  │ X │      │
│  └───┘      │
│             │
└─────────────┘

After:
┌─────────────┐
│  Sub-field 1│
│  ┌───┐      │
│  │ X │      │  ← Obstacle handled as hole
│  └───┘      │
│  Sub-field 2│
└─────────────┘
```

## Troubleshooting

### Decomposition Not Working

1. **Check F2C Version**: Requires Fields2Cover v2.0+
   ```python
   import fields2cover as f2c
   print(hasattr(f2c, 'DC_Boustrophedon'))  # Should be True
   ```

2. **Check Field Geometry**: Field must be concave or have obstacles
   ```python
   from f2c_pipeline import _is_field_concave
   is_concave = _is_field_concave(polygon)
   ```

3. **Check Configuration**: Ensure `use_decomposition=True` in config

### Performance Issues

- For very large fields, decomposition may be slower
- Consider disabling for simple convex fields
- Trapezoidal decomposition may be slower than Boustrophedon

### Unexpected Results

- Decomposition may produce more swaths than expected
- This is normal - each sub-field generates its own swaths
- Route planning combines them optimally

## Best Practices

1. **Enable for Complex Fields**: Use decomposition for concave fields or fields with obstacles
2. **Choose Appropriate Method**: 
   - Boustrophedon for linear features
   - Trapezoidal for irregular shapes
3. **Test Both Methods**: Compare results to find best fit
4. **Monitor Performance**: Check if decomposition improves route quality
5. **Disable for Simple Fields**: Don't use for convex fields (unnecessary overhead)

## References

- Fields2Cover Documentation: https://fields2cover.github.io/
- Decomposition Tutorial: Part 7 of F2C tutorials
- F2C v2.0 Release Notes: Decomposition algorithms

