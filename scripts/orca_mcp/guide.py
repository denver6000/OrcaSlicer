"""Plain-language knowledge for the OrcaSlicer MCP server: what the important settings and
"advanced" features mean, and a review of the active presets that flags values worth a look.

Written for people new to tuning. Keys are OrcaSlicer setting keys (describe_settings has the
slicer's own tooltip for each). Notes marked P1S are about Bambu Lab enclosed CoreXY printers.
Numbers are typical starting points, not rules: calibration on the user's printer wins.
"""

from __future__ import annotations

import re
from typing import Any

# --------------------------------------------------------------------------- explanations

TOPICS: dict[str, dict[str, Any]] = {
    "pressure_advance": {
        "title": "Pressure advance (Bambu calls it the K factor / flow dynamics)",
        "aliases": ["pa", "k factor", "k value", "linear advance", "flow dynamics", "adaptive pressure advance"],
        "plain": (
            "Molten plastic in the nozzle behaves like toothpaste in a tube: when the extruder starts "
            "pushing, plastic comes out a moment late, and when it stops, plastic keeps oozing for a "
            "moment. At a corner the print head slows down and speeds up again, so without correction "
            "corners get too much plastic (bulges) and the lines after them get too little. Pressure "
            "advance pushes a little extra while speeding up and eases off while slowing down, so the "
            "line stays the same width all the way round. The number (K) is how strongly it corrects."
        ),
        "signs_too_low": "Bulging or blobby corners, a thick start and thin end of lines, seams that bulge.",
        "signs_too_high": "Rounded or thin corners, gaps right after corners, thin starts of lines, extruder clicking.",
        "typical": "Direct-drive printers like the P1S: roughly 0.01 to 0.08 for a 0.4 mm nozzle. It depends on the filament, so measure it per filament.",
        "how_to_set": "calibrate(test='pressure_advance'), print it, pick the line or corner that looks most even, then set pressure_advance to that value with enable_pressure_advance on, in the filament preset.",
        "keys": ["enable_pressure_advance", "pressure_advance", "adaptive_pressure_advance"],
        "p1s": "When enable_pressure_advance is off, OrcaSlicer leaves K to the printer, which uses its own value (Bambu's default is about 0.02, or what its flow dynamics calibration stored). Turning it on writes your K into the G-code for that filament. Leave adaptive_pressure_advance off until the basic value is calibrated.",
        "calibrate": "pressure_advance",
    },
    "flow_ratio": {
        "title": "Flow ratio (extrusion multiplier)",
        "aliases": ["flow rate", "extrusion multiplier", "flow", "over extrusion", "under extrusion"],
        "plain": (
            "The slicer works out how much filament each line needs. Flow ratio scales that amount: 1.0 "
            "is exactly the calculation, 0.95 is 5 % less. Every filament (and every spool brand) melts "
            "and packs slightly differently, so this corrects for it."
        ),
        "signs_too_low": "Gaps between lines on top surfaces, see-through or weak walls, rough top layers with grooves.",
        "signs_too_high": "Ridges or a rough, shiny-bumpy top surface, blobs, parts slightly too big, nozzle dragging through the top layer.",
        "typical": "0.92 to 1.02 for most filaments; Bambu's own PLA profiles are about 0.98.",
        "how_to_set": "calibrate(test='flow_rate', pass_number=1), print, pick the smoothest top square, then pass 2 for the fine step. New filament_flow_ratio = old x (100 + block value) / 100.",
        "keys": ["filament_flow_ratio", "print_flow_ratio", "top_solid_infill_flow_ratio"],
        "p1s": "Calibrate flow before pressure advance: PA is measured on lines whose width depends on the flow ratio.",
        "calibrate": "flow_rate",
    },
    "max_volumetric_speed": {
        "title": "Max volumetric speed (how fast the hotend can melt plastic)",
        "aliases": ["mvs", "volumetric", "flow limit", "melt rate", "speed limit"],
        "plain": (
            "A hotend can only melt so much plastic per second, like a kettle that can only boil so "
            "much water. That amount is measured in cubic millimetres per second (mm3/s): line width x "
            "layer height x speed. When a line would need more, OrcaSlicer slows that line down. So "
            "this setting, not the speed numbers, often decides how fast a print really goes."
        ),
        "signs_too_high": "Under-extrusion on fast sections (infill), weak or rough infill, clicking extruder, gaps in fast walls.",
        "signs_too_low": "Prints are slower than they need to be; nothing looks wrong.",
        "typical": "P1S standard hotend, 0.4 mm nozzle: PLA about 15-21, PETG about 10-16, ABS/ASA about 15-20, soft TPU about 3-4 mm3/s. High-flow (HF) filaments go higher.",
        "how_to_set": "calibrate(test='max_volumetric_speed'), find the height where the tower starts to look weak or gappy, read the flow there, and set filament_max_volumetric_speed about 10-20 % below it.",
        "keys": ["filament_max_volumetric_speed"],
        "p1s": "Hotter printing melts faster, so the limit rises a little with nozzle temperature.",
        "calibrate": "max_volumetric_speed",
    },
    "temperature": {
        "title": "Nozzle temperature",
        "aliases": ["nozzle temp", "hotend temperature", "temp tower", "print temperature"],
        "plain": (
            "How hot the plastic is melted. Hotter plastic flows easier and layers bond better, but it "
            "strings and droops more. Cooler gives sharper details and cleaner bridges but weaker layers "
            "and a lower speed limit. The spool label gives a range; the best value inside it depends on "
            "your printer and speed."
        ),
        "signs_too_high": "Stringing, droopy overhangs and bridges, glossy blobs, soft small details.",
        "signs_too_low": "Weak layers that split, rough or matte under-extruded surfaces, extruder clicking at speed.",
        "typical": "PLA 200-220, PETG 230-250, ABS/ASA 250-270, TPU 220-235 (degC). Fast printing needs the hotter end.",
        "how_to_set": "calibrate(test='temperature') prints a tower with one block per 5 degC; pick the block with the best overhang, bridge and surface, then set nozzle_temperature (and usually nozzle_temperature_initial_layer about 5 degC higher).",
        "keys": ["nozzle_temperature", "nozzle_temperature_initial_layer", "nozzle_temperature_range_low", "nozzle_temperature_range_high"],
        "calibrate": "temperature",
    },
    "bed_and_plates": {
        "title": "Bed temperature and build plates",
        "aliases": ["bed temp", "plate", "textured pei", "cool plate", "engineering plate", "smooth pei", "high temp plate", "glue", "adhesion", "bed adhesion", "not sticking"],
        "plain": (
            "The heated bed keeps the first layer soft and stuck down. Each plate type has its own "
            "temperature in the filament preset, because the surfaces grip differently. The single "
            "biggest fix for parts not sticking is a clean plate: wash it with dish soap and warm water "
            "(skin oil from fingers ruins adhesion), rinse, and dry."
        ),
        "signs_too_low": "Corners lift, the first layer peels off, prints come loose.",
        "signs_too_high": "Elephant foot (bottom layers bulge out), PLA softening and clogging in an enclosed printer.",
        "typical": "Textured PEI: PLA 55-65, PETG 70-80, ABS/ASA 90-100, TPU 30-40 (degC). A preset value of 0 for a plate means the filament is not meant for that plate.",
        "how_to_set": "Pick the plate in the slicer's plate settings so the right temperature key is used; adjust textured_plate_temp (or the key for your plate) in the filament preset.",
        "keys": ["curr_bed_type", "textured_plate_temp", "textured_plate_temp_initial_layer", "hot_plate_temp", "cool_plate_temp", "eng_plate_temp", "supertack_plate_temp"],
        "p1s": "The P1S ships with a Textured PEI plate. PETG sticks so hard to smooth PEI that it can tear the surface: use the textured side, or glue stick as a release layer. The Cool Plate needs glue.",
    },
    "first_layer": {
        "title": "First layer",
        "aliases": ["initial layer", "elephant foot", "squish", "z offset", "first layer height", "bottom layer"],
        "plain": (
            "Everything sits on the first layer, so it is printed slower, a bit thicker and hotter, and "
            "pressed slightly into the plate. Good first layer lines are flat, touch each other, and have "
            "no gaps or ridges. Elephant foot is the bottom few layers bulging out from being pressed "
            "down while the bed keeps them soft."
        ),
        "signs_too_low": "Lines that are round and separate (not squished), first layer peels off.",
        "signs_too_high": "Rough, ridged or transparent-thin first layer, elephant foot, nozzle scraping the plate.",
        "typical": "First layer height 0.2 mm for a 0.4 mm nozzle; first layer speed 30-50 mm/s; elephant foot compensation about 0.1-0.15 mm.",
        "how_to_set": "initial_layer_print_height, initial_layer_speed, initial_layer_line_width, elefant_foot_compensation (process); close_fan_the_first_x_layers (filament) keeps the fan off at the start.",
        "keys": ["initial_layer_print_height", "initial_layer_speed", "initial_layer_infill_speed", "initial_layer_line_width", "elefant_foot_compensation", "close_fan_the_first_x_layers"],
        "p1s": "The P1S measures the bed itself (auto bed leveling at the start of a print), so a bad first layer is usually a dirty plate or wrong plate temperature before it is a leveling problem. z_offset in the printer preset should normally stay 0.",
    },
    "cooling": {
        "title": "Cooling fans",
        "aliases": ["fan", "part cooling", "aux fan", "chamber fan", "min layer time", "slow down for cooling", "overhang fan"],
        "plain": (
            "Fans blow on the fresh plastic so it sets before the next layer lands on it. More cooling "
            "gives sharper overhangs and bridges; too much makes layers bond poorly and makes ABS-type "
            "plastics warp and crack. For small layers the printer also slows down so each layer has "
            "time to cool (minimum layer time)."
        ),
        "signs_too_low": "Droopy overhangs and bridges, melted-looking small tips, curled edges.",
        "signs_too_high": "Weak layers that split, warping and cracks on ABS/ASA, poor first layer if the fan runs early.",
        "typical": "PLA: 100 % after the first layers. PETG: about 30-60 %. ABS/ASA: 0-30 %, more only for overhangs and bridges. TPU: about 50-100 %.",
        "how_to_set": "fan_min_speed / fan_max_speed with fan_cooling_layer_time (how short a layer must be for full fan), slow_down_layer_time (minimum layer time), overhang_fan_speed and enable_overhang_bridge_fan, additional_cooling_fan_speed (aux fan).",
        "keys": ["fan_min_speed", "fan_max_speed", "fan_cooling_layer_time", "slow_down_layer_time", "slow_down_min_speed", "close_fan_the_first_x_layers", "enable_overhang_bridge_fan", "overhang_fan_speed", "overhang_fan_threshold", "additional_cooling_fan_speed"],
        "p1s": "The P1S has a part fan on the tool head, an auxiliary side fan (additional_cooling_fan_speed) and a chamber fan. Keep the aux fan low or off for ABS/ASA.",
    },
    "retraction": {
        "title": "Retraction, z-hop and wipe",
        "aliases": ["retract", "stringing", "z hop", "zhop", "wipe", "travel", "oozing", "strings", "hairs"],
        "plain": (
            "When the nozzle moves to a new place without printing, melted plastic can ooze out and leave "
            "strings. Retraction pulls the filament back a little first to relieve the pressure. Z-hop lifts "
            "the nozzle during the move so it does not drag over the print. Wipe moves the nozzle along "
            "the line it just printed before travelling, leaving the ooze there."
        ),
        "signs_too_low": "Strings and hairs between parts, blobs where travel starts.",
        "signs_too_high": "Gaps or thin spots after travels, clogs and ground filament from pulling soft plastic too far back.",
        "typical": "Direct drive (P1S): 0.4-1.0 mm at 30-40 mm/s; TPU less. More than 2 mm risks clogs. Z-hop 0.2-0.4 mm.",
        "how_to_set": "calibrate(test='retraction') finds the shortest length that stops strings. Set it per filament with filament_retraction_length (overrides the printer's retraction_length). Wet filament strings no matter what: dry it first.",
        "keys": ["retraction_length", "retraction_speed", "deretraction_speed", "filament_retraction_length", "z_hop", "z_hop_types", "retract_lift_above", "wipe", "wipe_distance", "reduce_crossing_wall"],
        "p1s": "z_hop_types 'Auto Lift' or 'Spiral Lift' avoid most of the stringing that a straight lift can cause. reduce_crossing_wall makes travels go around walls instead of over them, which hides strings.",
        "calibrate": "retraction",
    },
    "layer_height_and_width": {
        "title": "Layer height and line width",
        "aliases": ["layer height", "line width", "extrusion width", "resolution", "quality", "variable layer height", "adaptive layer"],
        "plain": (
            "Layer height is how thick each slice is. Thinner layers look smoother on curves and slopes "
            "but take longer. Line width is how wide each line is laid; slightly wider than the nozzle "
            "(about 105-110 %) bonds better. A good rule: layer height at most about 75 % of the nozzle "
            "diameter."
        ),
        "signs_too_high": "Visible stair-stepping on slopes, weak layers if over ~0.3 mm on a 0.4 mm nozzle.",
        "typical": "0.4 mm nozzle: 0.12 (fine), 0.16 (good), 0.20 (standard), 0.24-0.28 (draft). Line width about 0.42-0.45.",
        "how_to_set": "layer_height and line_width in the process preset. For a part that is flat in some places and curved in others, the slicer's variable layer height tool varies it by height.",
        "keys": ["layer_height", "line_width", "outer_wall_line_width", "inner_wall_line_width", "sparse_infill_line_width", "top_surface_line_width"],
    },
    "walls": {
        "title": "Walls (perimeters)",
        "aliases": ["perimeters", "wall loops", "shell", "arachne", "classic", "wall order", "inner outer", "wall generator", "strength"],
        "plain": (
            "Walls are the outlines printed around each layer. They carry most of a part's strength, more "
            "than infill does. The wall generator decides how thin areas are filled: Arachne varies the "
            "line width to fill small gaps; Classic keeps widths constant. Wall order decides whether the "
            "visible outer wall is printed first (more accurate size) or last (better overhangs)."
        ),
        "typical": "2 walls for looks, 3-4 for strong parts. Arachne for most prints. Inner/outer order by default; outer/inner for parts that must fit precisely.",
        "keys": ["wall_loops", "wall_generator", "wall_sequence", "precise_outer_wall", "only_one_wall_top", "detect_thin_wall"],
        "how_to_set": "wall_loops for strength; wall_sequence 'outer wall/inner wall' for dimensional accuracy; 'inner-outer-inner wall' is a compromise.",
    },
    "top_bottom": {
        "title": "Top and bottom layers",
        "aliases": ["top layers", "bottom layers", "shells", "pillowing", "top surface", "monotonic", "solid layers"],
        "plain": (
            "Solid layers close the top and bottom of a part. The first top layers sag a little into the "
            "gaps of the infill below; enough layers on top hide that. Too few gives 'pillowing': bumps "
            "or holes in the top surface. The top pattern 'monotonic' lays every line in the same "
            "direction for an even shine."
        ),
        "signs_too_low": "Pillowing, see-through or bumpy top surface.",
        "typical": "Top about 0.8-1.0 mm thick (4-5 layers at 0.2), bottom about 0.6 mm (3 layers).",
        "keys": ["top_shell_layers", "top_shell_thickness", "bottom_shell_layers", "top_surface_pattern", "top_surface_speed", "top_solid_infill_flow_ratio"],
        "how_to_set": "top_shell_layers / top_shell_thickness (the larger wins), top_surface_pattern 'monotonic' or 'monotonicline'. For very smooth tops, see 'ironing'.",
    },
    "infill": {
        "title": "Infill",
        "aliases": ["sparse infill", "gyroid", "grid", "density", "cubic", "lightning", "pattern"],
        "plain": (
            "The inside of a part is printed as a light pattern instead of solid plastic. More density "
            "adds weight and some strength, but walls matter more. Gyroid is strong in every direction "
            "and quiet to print; grid is fast but the nozzle crosses its own lines; lightning only "
            "supports the top surface and is for display parts."
        ),
        "typical": "10-20 % for most parts, 25-40 % for functional parts. Gyroid or crosshatch for strength, lightning for looks only.",
        "keys": ["sparse_infill_density", "sparse_infill_pattern", "sparse_infill_speed", "infill_wall_overlap"],
        "how_to_set": "sparse_infill_density and sparse_infill_pattern in the process preset; they can also be set per object or per height range with object_settings.",
    },
    "seams": {
        "title": "Seams (where each layer starts and ends)",
        "aliases": ["seam", "z seam", "zits", "scarf", "scarf seam", "blob", "seam position"],
        "plain": (
            "Each layer's outer wall has to start and end somewhere, and that point leaves a small mark. "
            "Lined up on every layer, the marks form a visible seam line. Seam position chooses where: "
            "aligned or back (one clean line you can hide at the back), nearest, or random (no line but "
            "small zits all over). A scarf seam ramps the start and end over each other so the mark "
            "almost disappears."
        ),
        "signs_too_high": "Visible seam line or zits; a gap at the seam means seam_gap is too large or pressure advance is off.",
        "keys": ["seam_position", "seam_gap", "seam_slope_type", "wipe_on_loops", "staggered_inner_seams"],
        "how_to_set": "seam_position 'aligned' or 'back'; paint seams where you want them (paint kind 'seam'); seam_slope_type 'external' for a scarf seam. A calibrated pressure advance makes every seam smaller.",
    },
    "supports": {
        "title": "Supports",
        "aliases": ["support", "tree support", "overhang", "support interface", "support gap", "z distance", "support removal"],
        "plain": (
            "Plastic cannot be printed in mid-air. Steep overhangs (more than about 45-60 degrees from "
            "vertical) need something under them. Supports are thrown-away structures that hold them up. "
            "Tree supports grow from a few points and touch less of the part; normal supports are solid "
            "blocks that are easier for flat areas. The gap between support and part decides how easily "
            "it breaks away versus how clean the underside is."
        ),
        "signs_too_low": "Droopy or failed overhangs.",
        "signs_too_high": "Supports hard to remove, scarred surfaces.",
        "typical": "support_type 'tree(auto)', threshold 30-45 degrees, top Z gap = one layer height (0.2) for PLA.",
        "keys": ["enable_support", "support_type", "support_threshold_angle", "support_on_build_plate_only", "support_top_z_distance", "support_interface_top_layers", "support_object_xy_distance"],
        "how_to_set": "First try a better orientation (analyze_orientation). Then enable_support, or paint/enforce supports only where needed (paint kind 'support', add_shape support_enforcer); block them elsewhere.",
    },
    "bridges_and_overhangs": {
        "title": "Bridges and overhangs",
        "aliases": ["bridge", "bridging", "overhang", "sagging", "curling", "droop"],
        "plain": (
            "A bridge is a line stretched between two supports in mid-air; an overhang leans out over "
            "the layer below. Both print best slower, cooler and with strong fan, and bridges with "
            "slightly less plastic so the line pulls tight instead of drooping."
        ),
        "keys": ["bridge_flow", "bridge_speed", "thick_bridges", "enable_overhang_speed", "overhang_1_4_speed", "overhang_3_4_speed", "enable_overhang_bridge_fan", "overhang_fan_speed", "slowdown_for_curled_perimeters"],
        "typical": "bridge_flow 0.85-1.0, bridge_speed 25-50 mm/s, overhang fan 100 % for PLA.",
        "how_to_set": "Lower bridge_flow a little and keep thick_bridges off for neater bridges; keep enable_overhang_speed and slowdown_for_curled_perimeters on.",
    },
    "speed_and_acceleration": {
        "title": "Speed and acceleration",
        "aliases": ["speed", "acceleration", "accel", "jerk", "fast", "quality", "print time"],
        "plain": (
            "Speed is how fast the head moves along a line; acceleration is how quickly it gets up to "
            "that speed. On small parts the head rarely reaches full speed, so acceleration matters as "
            "much as speed. The outer wall is the visible one: printing it slower and with less "
            "acceleration gives the best surface. Remember the max volumetric speed caps every line."
        ),
        "typical": "P1S outer wall 150-200 mm/s with 5000 mm/s2 for good looks; inner walls and infill can go much faster.",
        "keys": ["outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed", "travel_speed", "default_acceleration", "outer_wall_acceleration", "top_surface_acceleration", "inner_wall_acceleration"],
        "how_to_set": "Lower outer_wall_speed and outer_wall_acceleration for looks; raise inner and infill speeds for time (the flow limit decides if that helps).",
    },
    "vibration_and_vfa": {
        "title": "Ringing, ghosting and vertical fine artifacts (VFA)",
        "aliases": ["ringing", "ghosting", "vfa", "vertical lines", "input shaping", "vibration", "echo", "ripples"],
        "plain": (
            "Fast moves shake the printer a little. Ringing (ghosting) is ripples that echo after a corner "
            "or an embossed letter. Bambu printers measure their own vibration and cancel it (vibration "
            "compensation, an optional step at print start). Vertical fine artifacts are fine evenly "
            "spaced vertical lines on walls that appear only at certain speeds, from the motors."
        ),
        "how_to_set": "Put the printer on a solid surface. Run the printer's vibration calibration. Lower outer_wall_acceleration. For VFA, calibrate(test='vfa') and choose an outer_wall_speed away from the bad bands.",
        "keys": ["outer_wall_speed", "outer_wall_acceleration", "default_acceleration"],
        "calibrate": "vfa",
    },
    "adhesion_aids": {
        "title": "Brim, skirt and raft",
        "aliases": ["brim", "mouse ears", "raft", "skirt", "warping", "lifting corners"],
        "plain": (
            "A brim is a flat ring of extra lines around the bottom of a part, stuck to it, so more area "
            "holds the part down; you snap it off afterwards. Brim ears are small discs only at corners. "
            "A raft is a whole platform under the part (rarely needed on PEI). A skirt is a loop that "
            "does not touch the part, only to prime the nozzle."
        ),
        "typical": "brim_type 'auto_brim' is fine for PLA; use 'outer_only' with 5-8 mm width for ABS/ASA, tall thin parts or small contact areas.",
        "keys": ["brim_type", "brim_width", "brim_object_gap", "raft_layers", "skirt_loops"],
        "how_to_set": "brim_type and brim_width in the process preset, or per object with object_settings; brim_object_gap 0.1 mm makes it easier to peel off.",
    },
    "ironing": {
        "title": "Ironing",
        "aliases": ["iron", "smooth top", "glossy top"],
        "plain": (
            "After the top layer, the hot nozzle passes over it again with a tiny bit of plastic, "
            "melting the surface flat like an iron. It makes flat tops very smooth but adds time and "
            "only helps flat, upward-facing surfaces."
        ),
        "keys": ["ironing_type", "ironing_flow", "ironing_spacing", "ironing_speed"],
        "typical": "ironing_type 'topmost' (only the very top), flow 10-15 %, spacing 0.1-0.15 mm, speed 30-50 mm/s.",
        "how_to_set": "Turn on per object with object_settings so other parts stay fast.",
    },
    "dimensional_accuracy": {
        "title": "Making parts the right size (holes, fits, shrink)",
        "aliases": ["tolerance", "fit", "holes too small", "hole compensation", "shrinkage", "accuracy", "dimensions"],
        "plain": (
            "Printed holes usually come out slightly small and outer sizes slightly large, because the "
            "plastic spreads. OrcaSlicer can grow holes and shrink outlines by a set amount. ABS and ASA "
            "also shrink as they cool, which the shrinkage setting scales up for."
        ),
        "keys": ["xy_hole_compensation", "xy_contour_compensation", "elefant_foot_compensation", "filament_shrink", "wall_sequence", "hole_to_polyhole"],
        "typical": "xy_hole_compensation 0.05-0.15 mm. filament_shrink about 99.4-99.6 % for ABS/ASA.",
        "how_to_set": "Measure a test part with calipers first; get the flow ratio right before compensating. wall_sequence 'outer wall/inner wall' also improves size accuracy.",
    },
    "wet_filament": {
        "title": "Wet filament and drying",
        "aliases": ["moisture", "drying", "dryer", "popping", "bubbles", "humid", "hygroscopic"],
        "plain": (
            "Most filaments slowly absorb water from the air. When wet plastic is melted, the water boils: "
            "you hear popping, see steam or bubbles, and get strings, rough or fuzzy surfaces and weak "
            "parts. No setting fixes this; drying the spool does."
        ),
        "typical": "PLA 50 degC 6-8 h, PETG 65 degC 6-8 h, ABS/ASA 80 degC 4-6 h, TPU 55 degC 6-8 h, nylon (PA) 80 degC 8-12 h. Store with desiccant.",
        "keys": ["filament_type"],
        "p1s": "The AMS keeps spools in a box with desiccant, but it does not dry them. PETG, TPU and nylon need drying most often.",
    },
    "enclosure_and_heat_creep": {
        "title": "The enclosure: door and lid open or closed (heat creep)",
        "aliases": ["enclosure", "door", "lid", "chamber", "heat creep", "clog", "clogs", "jam", "open door"],
        "plain": (
            "An enclosed printer keeps the air warm. That is what ABS and ASA need to avoid warping and "
            "cracking. But PLA softens at about 55-60 degC: in a warm chamber it can soften too early in "
            "the cold part of the hotend and jam it (heat creep), usually mid-print."
        ),
        "how_to_set": "PLA, PETG and TPU: print with the door open (and the top lid propped open with a hot bed or long prints). ABS, ASA, PC, nylon: door and lid closed, let the bed preheat the chamber.",
        "keys": ["chamber_temperature", "textured_plate_temp", "activate_air_filtration"],
        "p1s": "The P1S has no chamber heater; the bed warms the chamber. ABS/ASA give off fumes: keep the chamber closed and the room ventilated.",
    },
    "abrasive_filaments": {
        "title": "Abrasive filaments (carbon fiber, glass fiber, glow-in-the-dark)",
        "aliases": ["cf", "gf", "carbon fiber", "glass fiber", "abrasive", "hardened nozzle", "nozzle wear", "glow"],
        "plain": (
            "Filaments with carbon fiber (CF), glass fiber (GF) or glow powder act like sandpaper and "
            "wear a soft nozzle out within a few hundred grams, which ruins accuracy and flow."
        ),
        "keys": ["nozzle_type", "nozzle_hrc", "required_nozzle_HRC"],
        "p1s": "The P1S ships with a stainless steel nozzle. Fit a hardened steel nozzle before printing CF/GF filaments, then set nozzle_type in the printer preset to match.",
    },
    "presets": {
        "title": "How presets fit together",
        "aliases": ["preset", "profile", "system preset", "user preset", "inherits", "save preset", "per object settings"],
        "plain": (
            "Settings live in three presets: printer (the machine and nozzle), filament (the plastic: "
            "temperatures, flow, cooling, retraction) and process (how the part is sliced: layers, "
            "walls, infill, speeds, supports). System presets come with the slicer and cannot be "
            "changed; your own presets inherit from one and store only what you changed. Process "
            "settings can also be overridden per object, per part or per height range."
        ),
        "how_to_set": "Edit a value (it shows as modified), then save it as your own preset. preset_changes lists edits and differences from the system preset. Save filament tuning in its own filament preset per spool brand.",
        "keys": [],
    },
    "calibration_order": {
        "title": "Calibrating a new filament: what to do in which order",
        "aliases": ["calibration", "calibrate", "tuning", "dial in", "new filament", "tune"],
        "plain": (
            "Each test depends on the ones before it, so the order matters: 1) temperature, 2) flow "
            "ratio (coarse, then fine), 3) pressure advance, 4) retraction, 5) max volumetric speed, and "
            "optionally 6) VFA. Start from the closest system preset (for example Generic PETG), and "
            "save the results as a new filament preset named after the spool."
        ),
        "how_to_set": "The tune_filament prompt walks through it; calibrate() sets up each test.",
        "keys": [],
    },
}


def _norm(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", s.lower()).strip()


def find_topic(query: str) -> list[str]:
    """Topic ids matching a word or phrase, best first."""
    q = _norm(query)
    if not q:
        return []
    scored: list[tuple[int, str]] = []
    for tid, t in TOPICS.items():
        names = [_norm(tid), _norm(t["title"])] + [_norm(a) for a in t.get("aliases", [])]
        score = 0
        if q in names:
            score = 100
        elif any(q in n or (n and n in q) for n in names):
            score = 50
        else:
            words = set(q.split())
            text = " ".join(names + [_norm(t["plain"])] + [_norm(k) for k in t.get("keys", [])])
            score = sum(3 if w in names else 1 for w in words if w in text)
        if q.replace(" ", "_") in t.get("keys", []):
            score = max(score, 80)
        if score:
            scored.append((score, tid))
    scored.sort(key=lambda x: -x[0])
    return [tid for _, tid in scored]


def topic_list() -> list[dict[str, str]]:
    return [{"topic": tid, "title": t["title"]} for tid, t in TOPICS.items()]


# --------------------------------------------------------------------------- settings review

# Rough per-material ranges for a 0.4 mm standard hotend. The preset's own nozzle range wins when set.
MATERIALS: dict[str, dict[str, Any]] = {
    "PLA": {"nozzle": (190, 235), "bed_textured": (45, 65), "fan_max_min": 70, "mvs_max": 24, "enclosure": "open"},
    "PETG": {"nozzle": (220, 260), "bed_textured": (65, 85), "fan_min_max": 85, "mvs_max": 20, "enclosure": "open"},
    "PCTG": {"nozzle": (230, 270), "bed_textured": (65, 85), "fan_min_max": 85, "mvs_max": 20, "enclosure": "open"},
    "ABS": {"nozzle": (240, 280), "bed_textured": (85, 110), "fan_max_max": 60, "mvs_max": 24, "enclosure": "closed", "brim": True},
    "ASA": {"nozzle": (240, 280), "bed_textured": (85, 110), "fan_max_max": 60, "mvs_max": 24, "enclosure": "closed", "brim": True},
    "TPU": {"nozzle": (200, 250), "bed_textured": (25, 50), "mvs_max": 6, "retract_max": 1.5, "enclosure": "open"},
    "PC": {"nozzle": (250, 300), "bed_textured": (90, 110), "fan_max_max": 60, "mvs_max": 20, "enclosure": "closed", "brim": True},
    "PA": {"nozzle": (250, 300), "bed_textured": (70, 110), "fan_max_max": 60, "mvs_max": 20, "enclosure": "closed", "brim": True},
}

PLATE_KEYS = {
    "Cool Plate": "cool_plate_temp",
    "Engineering Plate": "eng_plate_temp",
    "High Temp Plate": "hot_plate_temp",
    "Textured PEI Plate": "textured_plate_temp",
    "Supertack Plate": "supertack_plate_temp",
    "Textured Cool Plate": "textured_cool_plate_temp",
}

SPEED_ROLES = [
    ("outer wall", "outer_wall_speed", "outer_wall_line_width"),
    ("inner wall", "inner_wall_speed", "inner_wall_line_width"),
    ("sparse infill", "sparse_infill_speed", "sparse_infill_line_width"),
    ("internal solid infill", "internal_solid_infill_speed", "internal_solid_infill_line_width"),
    ("top surface", "top_surface_speed", "top_surface_line_width"),
]

REVIEW_KEYS = sorted({
    "filament_type", "filament_settings_id", "printer_model", "printer_settings_id", "print_settings_id",
    "nozzle_diameter", "nozzle_type", "nozzle_hrc", "required_nozzle_HRC", "curr_bed_type",
    "nozzle_temperature", "nozzle_temperature_initial_layer", "nozzle_temperature_range_low", "nozzle_temperature_range_high",
    *PLATE_KEYS.values(), "filament_flow_ratio", "enable_pressure_advance", "pressure_advance", "adaptive_pressure_advance",
    "filament_max_volumetric_speed", "fan_min_speed", "fan_max_speed", "close_fan_the_first_x_layers",
    "additional_cooling_fan_speed", "retraction_length", "filament_retraction_length", "layer_height",
    "initial_layer_print_height", "initial_layer_speed", "line_width", "wall_loops", "top_shell_layers",
    "sparse_infill_density", "brim_type", "elefant_foot_compensation", "enable_support",
    *[k for _, s, w in SPEED_ROLES for k in (s, w)],
})


def _slot(value: str | None, slot: int) -> str | None:
    """One filament slot's entry from a comma/semicolon list (or the value itself)."""
    if value is None:
        return None
    if value.startswith('"'):  # quoted strings: "a";"b"
        parts = [p.strip('"') for p in value.split('";"')]
    else:
        parts = [p.strip() for p in re.split(r"[;,]", value)]
    return parts[slot] if slot < len(parts) else parts[0]


def _num(value: str | None) -> float | None:
    if value is None or value == "" or value == "nil":
        return None
    try:
        return float(value.rstrip("%"))
    except ValueError:
        return None


def _width(value: str | None, nozzle: float, default: float) -> float:
    if not value:
        return default
    v = _num(value)
    if v is None or v == 0:
        return default
    return nozzle * v / 100 if value.endswith("%") else v


def material_of(filament_type: str, preset_name: str) -> tuple[str | None, bool]:
    """(base material key in MATERIALS, abrasive?) from the filament type and preset name."""
    t = (filament_type or "").upper()
    name = (preset_name or "").upper()
    abrasive = bool(re.search(r"\b(CF|GF)\b|-CF|-GF|CF\b|GF\b|GLOW", t + " " + name))
    for base in ("PCTG", "PETG", "PET", "PLA", "ABS", "ASA", "TPU", "PC", "PA"):
        if t.startswith(base) or re.search(rf"\b{base}\b", t):
            return ("PETG" if base == "PET" else base), abrasive
    return None, abrasive


def review(values: dict[str, str], slot: int = 0) -> dict[str, Any]:
    """Findings about the active presets for one filament slot."""
    v = {k: _slot(values.get(k), slot) if k not in ("printer_model", "curr_bed_type", "print_settings_id", "printer_settings_id") else values.get(k)
         for k in REVIEW_KEYS}
    findings: list[dict[str, Any]] = []

    def add(level: str, topic: str, title: str, detail: str, keys: list[str], suggestion: str | None = None) -> None:
        f = {"level": level, "topic": topic, "title": title, "detail": detail, "current": {k: v.get(k) for k in keys}}
        if suggestion:
            f["suggestion"] = suggestion
        findings.append(f)

    preset = v.get("filament_settings_id") or ""
    mat_key, abrasive = material_of(v.get("filament_type") or "", preset)
    mat = MATERIALS.get(mat_key or "", {})
    high_flow = bool(re.search(r"\bHF\b|HIGH ?FLOW|HIGH ?SPEED|\bHS\b", preset.upper()))
    nozzle = _num(_slot(values.get("nozzle_diameter"), 0)) or 0.4
    model = (values.get("printer_model") or "")
    enclosed = bool(re.search(r"P1S|X1|P2S|H2", model))

    # Nozzle temperature
    temp = _num(v.get("nozzle_temperature"))
    lo, hi = _num(v.get("nozzle_temperature_range_low")), _num(v.get("nozzle_temperature_range_high"))
    if temp is not None:
        rng = (lo, hi) if lo and hi and hi > lo else mat.get("nozzle")
        if rng and not (rng[0] <= temp <= rng[1]):
            add("warn", "temperature", "Nozzle temperature outside the filament's range",
                f"{temp:g} degC is outside {rng[0]:g}-{rng[1]:g} degC.", ["nozzle_temperature"],
                "Check the spool label; a temperature tower (calibrate test 'temperature') finds the best value.")

    # Bed temperature for the selected plate
    plate = values.get("curr_bed_type")
    plate_key = PLATE_KEYS.get(plate or "")
    if plate_key:
        bed = _num(v.get(plate_key))
        if bed == 0:
            add("error", "bed_and_plates", f"This filament is not meant for the {plate}",
                f"The filament preset sets {plate_key} = 0, meaning it should not be printed on this plate.", [plate_key, "curr_bed_type"],
                "Pick another plate (textured PEI suits most filaments), or set a temperature if you know it works.")
        elif bed is not None and plate_key == "textured_plate_temp" and mat.get("bed_textured"):
            blo, bhi = mat["bed_textured"]
            if not (blo <= bed <= bhi):
                add("warn", "bed_and_plates", "Bed temperature unusual for this material",
                    f"{bed:g} degC on {plate}; {mat_key} usually uses {blo}-{bhi} degC.", [plate_key])
        if enclosed and mat.get("enclosure") == "open" and bed is not None and bed >= 55:
            add("info", "enclosure_and_heat_creep", "Print with the door open",
                f"{mat_key} with a {bed:g} degC bed in an enclosed printer can soften in the hotend and clog (heat creep).",
                [plate_key], "Open the front door (and prop the top lid open on long prints).")
    if enclosed and mat.get("enclosure") == "closed":
        add("info", "enclosure_and_heat_creep", "Keep the enclosure closed",
            f"{mat_key} warps and cracks in drafts: door and lid closed, let the bed warm the chamber first.", [])

    # Flow ratio
    flow = _num(v.get("filament_flow_ratio"))
    if flow is not None and not (0.85 <= flow <= 1.1):
        add("warn", "flow_ratio", "Unusual flow ratio", f"filament_flow_ratio is {flow:g}; most filaments land between 0.92 and 1.02.",
            ["filament_flow_ratio"], "Run the flow rate calibration (pass 1 then 2) instead of guessing.")

    # Pressure advance
    pa_on = v.get("enable_pressure_advance") in ("1", "true")
    pa = _num(v.get("pressure_advance"))
    if pa_on and (pa is None or pa <= 0):
        add("warn", "pressure_advance", "Pressure advance on with K = 0", "Enabling it with 0 turns the correction off.",
            ["enable_pressure_advance", "pressure_advance"], "Calibrate it, or turn enable_pressure_advance off to let the printer use its own value.")
    elif pa_on and pa is not None and pa > 0.15:
        add("warn", "pressure_advance", "Pressure advance very high for a direct-drive printer",
            f"K = {pa:g}; direct-drive printers usually need 0.01-0.08.", ["pressure_advance"], "Re-run the pressure advance calibration.")
    elif not pa_on:
        add("info", "pressure_advance", "Pressure advance comes from the printer",
            "enable_pressure_advance is off, so the printer uses its own K for this filament. That is fine; calibrate and enable it if corners bulge or seams are messy.",
            ["enable_pressure_advance", "pressure_advance"])

    # Max volumetric speed and what it means for the speeds
    mvs = _num(v.get("filament_max_volumetric_speed"))
    if mvs is not None and mat.get("mvs_max") and not high_flow and mvs > mat["mvs_max"]:
        add("warn", "max_volumetric_speed", "Max volumetric speed high for this material",
            f"{mvs:g} mm3/s; {mat_key} on a standard 0.4 mm hotend usually manages up to about {mat['mvs_max']} mm3/s.",
            ["filament_max_volumetric_speed"], "Expect under-extrusion on fast infill; the max volumetric speed test finds the real limit.")
    layer = _num(v.get("layer_height")) or 0.2
    default_w = _width(v.get("line_width"), nozzle, nozzle * 1.05)
    if mvs:
        capped = []
        for role, skey, wkey in SPEED_ROLES:
            speed = _num(v.get(skey))
            w = _width(v.get(wkey), nozzle, default_w)
            if speed and w and layer:
                real = mvs / (w * layer)
                if real < speed * 0.95:
                    capped.append(f"{role} {speed:g} -> about {real:.0f} mm/s")
        if capped:
            add("info", "max_volumetric_speed", "Some speeds are capped by the flow limit",
                f"At {layer:g} mm layers with {mvs:g} mm3/s, these run slower than set: " + "; ".join(capped) + ".",
                ["filament_max_volumetric_speed", "layer_height"],
                "Raising those speeds will not make the print faster; a higher flow limit (if the hotend manages it) or thinner layers would.")

    # Cooling
    fmax, fmin = _num(v.get("fan_max_speed")), _num(v.get("fan_min_speed"))
    if mat.get("fan_max_min") and fmax is not None and fmax < mat["fan_max_min"]:
        add("warn", "cooling", "Little cooling for this material", f"fan_max_speed {fmax:g} %; {mat_key} prints best with strong cooling.",
            ["fan_max_speed", "fan_min_speed"], "Raise fan_max_speed toward 100 % for sharper overhangs and bridges.")
    if mat.get("fan_min_max") and fmin is not None and fmin > mat["fan_min_max"]:
        add("info", "cooling", "Strong cooling weakens this material's layers", f"fan_min_speed {fmin:g} %; {mat_key} bonds better with moderate fan.",
            ["fan_min_speed", "fan_max_speed"], "Try 30-60 % unless overhangs need more.")
    if mat.get("fan_max_max") and fmax is not None and fmax > mat["fan_max_max"]:
        add("warn", "cooling", "Too much cooling for this material", f"fan_max_speed {fmax:g} %; {mat_key} warps and cracks with strong cooling.",
            ["fan_max_speed", "additional_cooling_fan_speed"], "Keep fans low (0-30 %), raising them only for overhangs and bridges.")
    if _num(v.get("close_fan_the_first_x_layers")) == 0:
        add("info", "first_layer", "Fan runs from the first layer", "Cooling the first layer can hurt adhesion.",
            ["close_fan_the_first_x_layers"], "1-3 layers without fan is typical.")

    # Retraction
    retract = _num(v.get("filament_retraction_length"))
    retract = retract if retract is not None else _num(_slot(values.get("retraction_length"), 0))
    if retract is not None:
        rmax = mat.get("retract_max", 2.0)
        if retract > rmax:
            add("warn", "retraction", "Long retraction", f"{retract:g} mm; more than {rmax:g} mm on a direct-drive printer risks clogs and ground filament.",
                ["filament_retraction_length", "retraction_length"], "Use the retraction test to find the shortest length that stops strings.")

    # Layers and walls
    if layer > nozzle * 0.75:
        add("warn", "layer_height_and_width", "Layer height over 75 % of the nozzle", f"{layer:g} mm layers on a {nozzle:g} mm nozzle bond poorly.",
            ["layer_height"], f"Stay at or below {nozzle * 0.75:.2f} mm.")
    first = _num(v.get("initial_layer_print_height"))
    if first is not None and (first > nozzle * 0.75 or first < nozzle * 0.3):
        add("warn", "first_layer", "Unusual first layer height", f"{first:g} mm on a {nozzle:g} mm nozzle.", ["initial_layer_print_height"],
            f"About {nozzle / 2:.2f} mm is the usual choice.")
    fspeed = _num(v.get("initial_layer_speed"))
    if fspeed is not None and fspeed > 80:
        add("info", "first_layer", "Fast first layer", f"{fspeed:g} mm/s; slower first layers stick better.", ["initial_layer_speed"], "30-60 mm/s.")
    walls = _num(v.get("wall_loops"))
    if walls is not None and walls < 2:
        add("warn", "walls", "Only one wall", "Single walls are weak and show infill through the surface.", ["wall_loops"], "2 walls minimum, 3-4 for strength.")
    tops = _num(v.get("top_shell_layers"))
    dens = _num(v.get("sparse_infill_density"))
    if tops is not None and tops < 3:
        add("warn", "top_bottom", "Few top layers", f"{tops:g} top layers can leave holes or pillowing.", ["top_shell_layers"], "4-5 layers at 0.2 mm.")
    elif tops is not None and dens is not None and dens < 10 and tops < 5:
        add("info", "top_bottom", "Low infill under thin tops", f"{dens:g} % infill with {tops:g} top layers may pillow.",
            ["sparse_infill_density", "top_shell_layers"], "Add a top layer or two, or raise infill.")

    # Brim for warping materials
    if mat.get("brim") and v.get("brim_type") == "no_brim":
        add("info", "adhesion_aids", "No brim on a warp-prone material", f"{mat_key} corners tend to lift.", ["brim_type"], "brim_type 'outer_only', 5-8 mm.")

    # Abrasive filament and nozzle
    req, have = _num(v.get("required_nozzle_HRC")), _num(_slot(values.get("nozzle_hrc"), 0))
    ntype = _slot(values.get("nozzle_type"), 0) or ""
    # nozzle_hrc 0 and nozzle_type "undefine" mean the printer preset does not say; don't guess then.
    soft_nozzle = ntype in ("stainless_steel", "brass")
    if (req and have and req > have) or (abrasive and soft_nozzle):
        add("warn", "abrasive_filaments", "Abrasive filament on a soft nozzle",
            f"Filament looks abrasive (CF/GF/glow) and the printer preset says nozzle_type '{ntype}'.", ["nozzle_type", "nozzle_hrc", "required_nozzle_HRC"],
            "Fit a hardened steel nozzle and set nozzle_type in the printer preset; a stainless nozzle wears out fast.")

    order = {"error": 0, "warn": 1, "info": 2}
    findings.sort(key=lambda f: order[f["level"]])
    return {
        "filament_slot": slot,
        "filament_preset": preset,
        "material": mat_key or v.get("filament_type"),
        "printer": model,
        "process_preset": values.get("print_settings_id"),
        "plate": plate,
        "findings": findings,
        "summary": {lvl: sum(f["level"] == lvl for f in findings) for lvl in ("error", "warn", "info")},
        "note": "Rules of thumb for a 0.4 mm standard hotend; calibration results on this printer win. explain(topic) gives the background for each finding.",
    }
