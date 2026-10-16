# Architecture

```text
VIDEO
  |
  v
analysis resize (configurable, aspect preserved)
  |
  v
L1: MOG2 raw foreground
  |
  +-- 3x3 opening
  |
  +-------------------------------+
  |                               |
  v                               v
L2 repeat evidence            L3 TAWL rain/noise evidence
  |                               |
  +---------------+---------------+
                  |
                  v
      connected-component decision
                  |
      fully explained? -> ignore
      not fully explained -> keep whole component
                  |
                  v
      valid_ratio = valid_component_px / frame_area
                  |
        >= event_min_valid_ratio
                  |
                  v
                EVENT
```

## L2

Grid history learns repeated occupancy + re-entry.

Re-entry is stateful: after activity falls, the cell remains armed until a later
rise. L2 history is a finite sliding window, so old repeat regions are actually
forgotten.

Otsu builds the repeat-region prior. Current components use containment only:

```text
100% inside repeat region -> repetitive evidence
any pixel outside -> preserve whole component
```

No tracker is used.

## L3

TAWL still uses the accepted TA/W ratios and Location connectivity.

Temporal support is component-level rather than exact-pixel age. A component can
inherit age from touching/8-neighbor foreground in the previous frame, which
protects small moving objects without optical flow or object tracking.


## VALID mask
`full_VALID_motion.mp4` and `motion_XXXXXX_VALID.mp4` show the full connected components already counted by the unchanged event logic as valid motion. Adding these files does not change detection or event decisions.
