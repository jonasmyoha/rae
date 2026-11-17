#!/usr/bin/env python3
"""Scale the UI lengths of .raescene scenes and themes by a factor.

Rae's UI unit is the logical point (docs/ui-coordinate-and-responsive-layout.md). A scene or a
theme authored at another scale (3 units per point, a 1080-wide Pixi canvas) is converted with
`--factor 0.333333` (or `--divide 3`): every length (a position, a size, a padding, a gap, a radius,
a stroke, a row height, a text size, a theme's space / radius / padding tokens, a text shadow's
offset and softness) is multiplied, and nothing else (counts, ratios, angles, colors, times,
opacities, 3D values). A frame's `when` is in points already and is kept, and so are a sprite's
`nineSlice` insets (pixels of the source image) and a SafeArea's `apply` flags. A scene's own
`unitScale` key is removed: every scene is in points.

A number the tool does not know is reported, not changed, so a component added later is never
scaled or skipped silently: add it to LENGTHS (scaled) or KEPT (not a length).

    python3 tools/scale-ui-lengths.py --divide 3 examples/106_mobile_ui/assets/scenes/*.raescene
    python3 tools/scale-ui-lengths.py --divide 3 --check scenes/   # report only, write nothing
"""
import argparse
import json
import os
import sys

# Component -> the fields that are lengths; a dotted field is a nested object's field.
LENGTHS = {
    "Rect": {"x", "y", "w", "h"},
    "Padding": {"l", "t", "r", "b"},
    "Margin": {"l", "t", "r", "b"},
    "SafeArea": {"extra.l", "extra.t", "extra.r", "extra.b"},
    "Offset": {"x", "y", "delta.x", "delta.y"},
    "CornerRadius": {"radius"},
    "Shape": {"radius", "strokeWidth", "strokeDash", "strokeGap"},
    "MaskShape": {"radius"},
    "HitArea": {"radius"},
    "BackdropBlur": {"radius"},
    "Shadow": {"blur", "offset.x", "offset.y"},
    "TextShadow": {"softness", "offset.x", "offset.y"},
    "Layout": {"gap", "rowGap", "columnGap"},
    "ListView": {"itemHeight", "itemWidth", "itemGap"},
    "ScrollRoot": {"endInset", "fixedFloor"},
    "ScrollState": {"y"},
    "SheetPanel": {"rowHeight"},
    "BottomSheet": {"rowHeight"},
    "StatTable": {"rowHeight", "headerHeight"},
    "Timeline": {"rowHeight", "laneGap"},
    "FlameGraph": {"rowHeight"},
    "Histogram": {"barGap"},
    "Constraints": {"minW", "maxW", "minH", "maxH"},
    "Size": {"w.min", "w.max", "w.value", "h.min", "h.max", "h.value"},
    "DialogOptions": {"maxWidth"},
    "FocusStyle": {"strokeWidth"},
    "Appear": {"x", "y"},
    "Disappear": {"x", "y"},
    "StateStyle": {"offset.x", "offset.y", "offsetSelected.x", "offsetSelected.y"},
    "Wires": {"width", "minScreenWidth", "hitTolerance"},
    "PlaneItem": {"x", "y", "w", "h"},
    "Camera2D": {"offsetX", "offsetY"},
    "Text": {"styleOverride.size"},
}

# Component -> numeric fields that are not lengths (kept as they are, not reported).
KEPT = {
    "Rect": set(), "Aspect": {"ratio"}, "Opacity": {"value"}, "GradientFill": {"angle"},
    "HoverScale": {"current", "hoverScale", "pressedScale", "restScale", "speed", "target"},
    "Layout": {"columns"}, "ListView": {"overscanRows", "visibleItemCount"},
    "DataDependency": {"observedRevision"}, "RefreshRegion": {"observedRevision"},
    "SafeArea": {"apply.l", "apply.t", "apply.r", "apply.b"},
    "Shadow": {"layers", "opacity", "hoverOpacity"},
    "Sprite": {"nineSlice.l", "nineSlice.t", "nineSlice.r", "nineSlice.b", "tileScale.x", "tileScale.y"},
    "TransformFx": {"alpha", "rotation", "scaleX", "scaleY", "pivot.x", "pivot.y", "anchor.x", "anchor.y"},
    "Camera2D": {"zoom"}, "LayerRoot": {"order"}, "OnClick": {"maxDelayMs"},
    "ValueSlider": {"min", "max"}, "ValueText": {"decimals", "scale"},
    "AnimFrames": {"fps", "frameCount", "frames"}, "Carousel": {
        "autoAdvanceSeconds", "previewScale", "swipeThreshold", "transitionDuration", "travelFactor"},
    "WobbleFx": {"amp", "freq", "secondAmp", "secondFreq"},
    "SmokeFx": {"driftX", "driftY", "fadeMs", "jitter", "lifetimeMs", "scaleEnd", "scaleStart",
                "spawnIntervalMs"},
    "TouchStick": {"zoneShare"}, "OpticalAlign": {"bias.x", "bias.y"},
    "DataRequest": {"aroundAfter", "aroundBefore", "initialLimit", "pageLimit"},
}

# Keys whose numbers are never lengths wherever they appear (colors, 3D, flags).
COLOR_FIELDS = {"r", "g", "b", "a"}
THREE_D = {"Camera3d", "Light3d", "Material3d", "SdfPrimitive", "Transform3d", "Transform3D"}


def scaleNumber(value, factor):
    scaled = value * factor
    rounded = round(scaled, 4)
    if rounded == int(rounded):
        return int(rounded) if isinstance(value, int) else float(int(rounded))
    return rounded


class Scaler:
    def __init__(self, factor, path):
        self.factor = factor
        self.path = path
        self.changed = 0
        self.unknown = []

    def component(self, name, body):
        lengths = LENGTHS.get(name, set())
        kept = KEPT.get(name, set())
        if name in THREE_D:
            return body
        return self.fields(name, body, "", lengths, kept)

    def fields(self, name, body, prefix, lengths, kept):
        if not isinstance(body, dict):
            return body
        for key, value in body.items():
            field = prefix + key
            if isinstance(value, dict):
                if key in ("color", "fill", "stroke", "tint", "from", "to") and set(value) <= COLOR_FIELDS:
                    continue
                body[key] = self.fields(name, value, field + ".", lengths, kept)
            elif isinstance(value, bool) or not isinstance(value, (int, float)):
                continue
            elif field in lengths:
                body[key] = scaleNumber(value, self.factor)
                self.changed += 1
            elif field not in kept:
                self.unknown.append(f"{name}.{field} = {value}")
        return body

    def node(self, node):
        if isinstance(node, dict):
            for key, value in list(node.items()):
                if key in LENGTHS or key in KEPT or (isinstance(value, dict) and key[:1].isupper()):
                    node[key] = self.component(key, value)
                elif key in ("children", "nodes"):
                    self.nodes(value)
        return node

    def nodes(self, value):
        if isinstance(value, list):
            for child in value:
                self.node(child)
        elif isinstance(value, dict):
            for child in value.values():
                self.node(child)

    def theme(self, scene):
        for style in (scene.get("text") or {}).values():
            if isinstance(style, dict):
                for key in ("size", "tracking"):
                    if isinstance(style.get(key), (int, float)):
                        style[key] = scaleNumber(style[key], self.factor)
                        self.changed += 1
        for section in ("space", "radius"):
            tokens = scene.get(section) or {}
            for key, value in tokens.items():
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    tokens[key] = scaleNumber(value, self.factor)
                    self.changed += 1
        for insets in (scene.get("padding") or {}).values():
            if isinstance(insets, dict):
                for key in ("l", "t", "r", "b"):
                    if isinstance(insets.get(key), (int, float)):
                        insets[key] = scaleNumber(insets[key], self.factor)
                        self.changed += 1
        for shadow in (scene.get("shadow") or {}).values():
            if isinstance(shadow, dict):
                for key in ("offsetX", "offsetY", "softness"):
                    if isinstance(shadow.get(key), (int, float)):
                        shadow[key] = scaleNumber(shadow[key], self.factor)
                        self.changed += 1

    def scene(self, scene):
        if "unitScale" in scene:
            del scene["unitScale"]
            self.changed += 1
        self.theme(scene)
        for key in ("root", "nodes"):
            if key in scene:
                if key == "root":
                    self.node(scene[key])
                else:
                    self.nodes(scene[key])
        for bundle in (scene.get("bundles") or {}).values():
            self.node(bundle)
        for state in (scene.get("states") or {}).values():
            if isinstance(state, dict):
                self.nodes(state.get("nodes") or {})
        for frame in (scene.get("frames") or {}).values():
            if isinstance(frame, dict):
                self.nodes(frame.get("nodes") or {})
        return scene


def numberSpans(text):
    """The (start, end) of every number token in a JSON text, in order (strings skipped)."""
    spans = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif c == "-" or c.isdigit():
            start = i
            i += 1
            while i < len(text) and (text[i].isdigit() or text[i] in ".eE+-"):
                i += 1
            spans.append((start, i))
        else:
            i += 1
    return spans


def numbersInOrder(value, out):
    if isinstance(value, dict):
        for item in value.values():
            numbersInOrder(item, out)
    elif isinstance(value, list):
        for item in value:
            numbersInOrder(item, out)
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        out.append(value)
    return out


def formatNumber(value, original):
    if isinstance(value, float) and value == int(value) and "." in original:
        return f"{int(value)}.0"
    return repr(value) if isinstance(value, float) else str(value)


def rewrite(text, scene):
    """The original text with only its numbers replaced by the scene's (its layout kept)."""
    import re
    text = re.sub(r'\n[ \t]*"unitScale":[ \t]*[0-9.]+,', "", text)
    text = re.sub(r',\n[ \t]*"unitScale":[ \t]*[0-9.]+(\n)', r"\1", text)
    spans = numberSpans(text)
    values = numbersInOrder(scene, [])
    if len(spans) != len(values):
        raise SystemExit(f"number count changed ({len(spans)} in the text, {len(values)} after)")
    out = []
    last = 0
    for (start, end), value in zip(spans, values):
        original = text[start:end]
        out.append(text[last:start])
        same = float(original) == float(value)
        out.append(original if same else formatNumber(value, original))
        last = end
    out.append(text[last:])
    return "".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("paths", nargs="+")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--factor", type=float)
    group.add_argument("--divide", type=float)
    parser.add_argument("--check", action="store_true", help="report only, write nothing")
    args = parser.parse_args()
    factor = args.factor if args.factor is not None else 1.0 / args.divide

    files = []
    for path in args.paths:
        if os.path.isdir(path):
            for folder, _, names in os.walk(path):
                files += [os.path.join(folder, n) for n in sorted(names) if n.endswith(".raescene")]
        else:
            files.append(path)
    unknownTotal = 0
    for path in files:
        text = open(path).read()
        scene = json.loads(text)
        scaler = Scaler(factor, path)
        scaler.scene(scene)
        for line in scaler.unknown:
            print(f"{path}: not scaled, unknown: {line}")
        unknownTotal += len(scaler.unknown)
        if not args.check and scaler.changed:
            open(path, "w").write(rewrite(text, scene))
        print(f"{path}: {scaler.changed} values {'to scale' if args.check else 'scaled'}")
    return 1 if unknownTotal else 0


if __name__ == "__main__":
    sys.exit(main())
