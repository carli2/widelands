#!/bin/bash
# =============================================================================
# compile_addon.sh - Package the Hebrew tribe as a Widelands add-on (.wad)
# =============================================================================
#
# This script collects all Hebrew tribe data files from the Widelands source
# tree and packages them into a .wad add-on directory that can be installed
# via Widelands' add-on manager.
#
# Usage: Run from the Widelands root directory (parent of hebrews-concept/)
#   ./hebrews-concept/compile_addon.sh
#
# Output: hebrews-concept/hebrews_tribe.wad/
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ADDON_NAME="hebrews_tribe"
ADDON_VERSION="0.2"
ADDON_AUTHOR="Carli"
ADDON_CATEGORY="tribes"
MIN_WL_VERSION="1.2"

# Paths (relative to Widelands root)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WL_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="$HOME/.widelands/addons/${ADDON_NAME}.wad"
FLATPAK_DIR="$HOME/.var/app/org.widelands.Widelands/.widelands/addons/${ADDON_NAME}.wad"
DATA_DIR="$WL_ROOT/data"
TRIBES_DIR="$DATA_DIR/tribes"

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------

if [ ! -d "$TRIBES_DIR/initialization/hebrews" ]; then
    echo "ERROR: Hebrew tribe data not found at $TRIBES_DIR/initialization/hebrews"
    echo "       Make sure you run this script from the Widelands root directory."
    exit 1
fi

# ---------------------------------------------------------------------------
# Clean previous output
# ---------------------------------------------------------------------------

if [ -d "$OUTPUT_DIR" ]; then
    echo "Removing previous build: $OUTPUT_DIR"
    rm -rf "$OUTPUT_DIR"
fi

echo "Creating add-on: $OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# ---------------------------------------------------------------------------
# 1. Create the 'addons' manifest file
# ---------------------------------------------------------------------------

echo "Writing addon manifest..."
cat > "$OUTPUT_DIR/addon" << 'MANIFEST'
[global]
name=_"Hebrew Tribe"
description=_"Adds the Hebrew tribe (Stamm der Hebräer) to Widelands. Features a unique economy based on clay, branches, and cloth with copper instead of iron."
author=Carli
version=0.2
category=tribes
requires=
min_wl_version=1.2
max_wl_version=
sync_safe=true
MANIFEST

# ---------------------------------------------------------------------------
# 2. Copy tribe initialization files
#    These contain the tribe definition (init.lua, units.lua, starting
#    conditions, flag/frontier/bridge images, road textures, etc.)
# ---------------------------------------------------------------------------

echo "Copying tribe initialization files..."
# The engine discovers addon tribes by scanning direct children of
# addons/<name>/tribes/ for init.lua.  So the tribe init must live at
# tribes/hebrews/init.lua  (NOT tribes/initialization/hebrews/init.lua).
mkdir -p "$OUTPUT_DIR/tribes/hebrews"
cp -r "$TRIBES_DIR/initialization/hebrews/"* "$OUTPUT_DIR/tribes/hebrews/"

# Patch init.lua to use the add-on textdomain instead of "tribes"
sed -i 's/push_textdomain("tribes")/push_textdomain("hebrews_tribe.wad", true)/' \
    "$OUTPUT_DIR/tribes/hebrews/init.lua"

# Add addon field to init.lua (required for addon tribes)
sed -i 's/name = "hebrews",/name = "hebrews",\n   addon = "hebrews_tribe.wad",/' \
    "$OUTPUT_DIR/tribes/hebrews/init.lua"

# Patch units.lua to use the add-on textdomain instead of "tribes_encyclopedia"
sed -i 's/push_textdomain("tribes_encyclopedia")/push_textdomain("hebrews_tribe.wad", true)/' \
    "$OUTPUT_DIR/tribes/hebrews/units.lua"

# ---------------------------------------------------------------------------
# 3. Copy buildings
#    Hebrew buildings are organized in subdirectories by building category:
#    productionsites, militarysites, trainingsites, warehouses, markets
# ---------------------------------------------------------------------------

echo "Copying buildings..."

# Production sites (bakery, branch_collectors_hut, brick_kiln, butchery, etc.)
if [ -d "$TRIBES_DIR/buildings/productionsites/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/buildings/productionsites/hebrews"
    cp -r "$TRIBES_DIR/buildings/productionsites/hebrews/"* \
        "$OUTPUT_DIR/tribes/buildings/productionsites/hebrews/"
fi

# Military sites (tent_small, massada)
if [ -d "$TRIBES_DIR/buildings/militarysites/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/buildings/militarysites/hebrews"
    cp -r "$TRIBES_DIR/buildings/militarysites/hebrews/"* \
        "$OUTPUT_DIR/tribes/buildings/militarysites/hebrews/"
fi

# Training sites (trainingcamp, yeshiva stub)
if [ -d "$TRIBES_DIR/buildings/trainingsites/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/buildings/trainingsites/hebrews"
    cp -r "$TRIBES_DIR/buildings/trainingsites/hebrews/"* \
        "$OUTPUT_DIR/tribes/buildings/trainingsites/hebrews/"
fi

# Warehouses (headquarters, headquarters_tent, warehouse, port)
if [ -d "$TRIBES_DIR/buildings/warehouses/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/buildings/warehouses/hebrews"
    cp -r "$TRIBES_DIR/buildings/warehouses/hebrews/"* \
        "$OUTPUT_DIR/tribes/buildings/warehouses/hebrews/"
fi

# Markets
if [ -d "$TRIBES_DIR/buildings/markets/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/buildings/markets/hebrews"
    cp -r "$TRIBES_DIR/buildings/markets/hebrews/"* \
        "$OUTPUT_DIR/tribes/buildings/markets/hebrews/"
fi

# ---------------------------------------------------------------------------
# 4. Copy workers
#    All Hebrew workers live under data/tribes/workers/hebrews/
# ---------------------------------------------------------------------------

echo "Copying workers..."
if [ -d "$TRIBES_DIR/workers/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/workers/hebrews"
    cp -r "$TRIBES_DIR/workers/hebrews/"* "$OUTPUT_DIR/tribes/workers/hebrews/"
fi

# ---------------------------------------------------------------------------
# 5. Copy wares
#    Some wares are Hebrew-exclusive, others are shared with other tribes.
#    We include ALL wares referenced in units.lua so the add-on is
#    self-contained. Shared wares (water, granite, etc.) are also needed
#    because the add-on must register them.
# ---------------------------------------------------------------------------

echo "Copying wares..."

# Complete list of wares from units.lua wares_order
HEBREW_WARES=(
    # Building Materials
    branch log granite clay
    # Food
    water wheat wheat_grains flour bread_hebrews fish meat
    olives olive_oil grape wine
    # Mining and Smelting
    copper_ore copper gold_ore gold_leaf menorah
    # Textile and Animal Products (sheep2 uses "sheep" directory if it exists)
    wool fur yarn cloth
    # Tools
    pick hammer fishing_rod
    # Religious Items
    zizit tallit_katan tefilin tallit
    # Weapons & Armor
    tunic slingshot dagger
)

mkdir -p "$OUTPUT_DIR/tribes/wares"
for ware in "${HEBREW_WARES[@]}"; do
    if [ -d "$TRIBES_DIR/wares/$ware" ]; then
        cp -r "$TRIBES_DIR/wares/$ware" "$OUTPUT_DIR/tribes/wares/"
    else
        echo "  WARNING: Ware directory not found: $ware"
    fi
done

# The "sheep2" ware uses the "sheep" directory in the source tree
# (the ware is registered as sheep2 in the init.lua but stored under sheep/)
if [ -d "$TRIBES_DIR/wares/sheep" ]; then
    cp -r "$TRIBES_DIR/wares/sheep" "$OUTPUT_DIR/tribes/wares/"
    echo "  Copied sheep ware (used as sheep2 in Hebrew economy)"
fi

# ---------------------------------------------------------------------------
# 6. Copy ships
#    Hebrew ship graphics (even though seafaring is a TODO, the ship
#    directory already exists with sprites)
# ---------------------------------------------------------------------------

echo "Copying ships..."
if [ -d "$TRIBES_DIR/ships/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/ships"
    cp -r "$TRIBES_DIR/ships/hebrews" "$OUTPUT_DIR/tribes/ships/"
fi

# ---------------------------------------------------------------------------
# 7. Copy immovables
#    Hebrew-specific immovables: resource indicators, shipconstruction,
#    crop fields (wheat, grape, olive), and pond_dry
#    We also include shared immovables referenced in units.lua
# ---------------------------------------------------------------------------

echo "Copying immovables..."
mkdir -p "$OUTPUT_DIR/tribes/immovables"

# Hebrew ship construction
if [ -d "$TRIBES_DIR/immovables/shipconstruction_hebrews" ]; then
    cp -r "$TRIBES_DIR/immovables/shipconstruction_hebrews" "$OUTPUT_DIR/tribes/immovables/"
fi

# Wheat fields (shared, but required by Hebrew farmer)
if [ -d "$TRIBES_DIR/immovables/wheatfield" ]; then
    cp -r "$TRIBES_DIR/immovables/wheatfield" "$OUTPUT_DIR/tribes/immovables/"
fi

# Grapevines (used by Hebrew vineyard)
if [ -d "$TRIBES_DIR/immovables/grapevine" ]; then
    cp -r "$TRIBES_DIR/immovables/grapevine" "$OUTPUT_DIR/tribes/immovables/"
fi

# Olive trees (used by Hebrew olive plantation)
if [ -d "$TRIBES_DIR/immovables/olivetree" ]; then
    cp -r "$TRIBES_DIR/immovables/olivetree" "$OUTPUT_DIR/tribes/immovables/"
fi

# Destroyed buildings and ashes (shared immovables)
for imm in ashes destroyed_building; do
    if [ -d "$TRIBES_DIR/immovables/$imm" ]; then
        cp -r "$TRIBES_DIR/immovables/$imm" "$OUTPUT_DIR/tribes/immovables/"
    fi
done

# Dead trees (referenced in units.lua for woodcutter interaction)
for imm in deadtree7; do
    if [ -d "$TRIBES_DIR/immovables/$imm" ]; then
        cp -r "$TRIBES_DIR/immovables/$imm" "$OUTPUT_DIR/tribes/immovables/"
    fi
done

# Amazon trees referenced in units.lua (balsa, ironwood, rubber variants)
for tree_type in balsa balsa_black balsa_desert balsa_winter \
                 ironwood ironwood_black ironwood_desert ironwood_winter \
                 rubber rubber_black rubber_desert rubber_winter; do
    tree_dir="${tree_type}_amazons_old"
    if [ -d "$TRIBES_DIR/immovables/$tree_dir" ]; then
        cp -r "$TRIBES_DIR/immovables/$tree_dir" "$OUTPUT_DIR/tribes/immovables/"
    fi
done

# Pond (clay pit byproduct: dry, growing, mature stages)
if [ -d "$TRIBES_DIR/immovables/pond" ]; then
    cp -r "$TRIBES_DIR/immovables/pond" "$OUTPUT_DIR/tribes/immovables/"
fi

# Hebrew resource indicators (stored under resi/hebrews/ in the source tree)
if [ -d "$TRIBES_DIR/immovables/resi/hebrews" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/immovables/resi/hebrews"
    cp -r "$TRIBES_DIR/immovables/resi/hebrews/"* "$OUTPUT_DIR/tribes/immovables/resi/hebrews/"
    echo "  Copied resource indicators from resi/hebrews/"
fi

# ---------------------------------------------------------------------------
# 8. Copy scripting helpers
#    The units.lua includes a shared scripting file for time formatting
# ---------------------------------------------------------------------------

echo "Copying scripting helpers..."
if [ -d "$TRIBES_DIR/scripting" ]; then
    mkdir -p "$OUTPUT_DIR/tribes/scripting"
    cp -r "$TRIBES_DIR/scripting/"* "$OUTPUT_DIR/tribes/scripting/"
fi

# ---------------------------------------------------------------------------
# 9. Copy German translations
#    Extract Hebrew-related entries from tribes and tribes_encyclopedia .po
#    files. For a proper add-on these would be in the add-on's own locale
#    directory, but for now we include the full .po files.
# ---------------------------------------------------------------------------

echo "Copying translations..."
TRANSLATIONS_DIR="$DATA_DIR/i18n/translations"

mkdir -p "$OUTPUT_DIR/locale/de/LC_MESSAGES"

# Copy tribes German translation (contains Hebrew building/ware/worker strings)
if [ -f "$TRANSLATIONS_DIR/tribes/de.po" ]; then
    cp "$TRANSLATIONS_DIR/tribes/de.po" "$OUTPUT_DIR/locale/de/LC_MESSAGES/hebrews_tribe.wad.po"
    echo "  Copied tribes/de.po"
fi

# Copy tribes_encyclopedia German translation
if [ -f "$TRANSLATIONS_DIR/tribes_encyclopedia/de.po" ]; then
    cp "$TRANSLATIONS_DIR/tribes_encyclopedia/de.po" "$OUTPUT_DIR/locale/de/LC_MESSAGES/hebrews_tribe_encyclopedia.po"
    echo "  Copied tribes_encyclopedia/de.po"
fi

# Try to compile .po to .mo if msgfmt is available
if command -v msgfmt &>/dev/null; then
    for pofile in "$OUTPUT_DIR/locale/de/LC_MESSAGES/"*.po; do
        mofile="${pofile%.po}.mo"
        msgfmt -o "$mofile" "$pofile" 2>/dev/null && echo "  Compiled: $(basename "$mofile")" || true
    done
fi

# ---------------------------------------------------------------------------
# 9b. Create tips.lua for game loading screen tips
# ---------------------------------------------------------------------------

echo "Creating tips.lua..."
cat > "$OUTPUT_DIR/tribes/hebrews/tips.lua" << 'TIPSEOF'
push_textdomain("hebrews_tribe.wad", true)
tips = {
   {
      text = _("Clay and branches are the main building materials of the Hebrews. Make sure to build clay pits and branch collector's huts early."),
      seconds = 5
   },
   {
      text = _("Bread is a staple food for the Hebrew economy. Build farms, threshing floors, mills, and bakeries to keep your workers fed."),
      seconds = 6
   },
   {
      text = _("The Hebrew tribe uses copper instead of iron. Ensure a steady supply of copper ore from your mines."),
      seconds = 5
   },
   {
      text = _("Cloth is essential for Hebrew construction. Set up a wool production chain: shepherds, spinning mills, and weaving mills."),
      seconds = 6
   },
}
pop_textdomain()
return tips
TIPSEOF

# ---------------------------------------------------------------------------
# 10. Add __skip_if_exists to all register.lua files
#     Add-on entities must define __skip_if_exists or __replace_if_exists
#     to tell the engine how to handle conflicts with already-registered
#     entities. The source files in data/tribes/ must NOT have this
#     attribute (the engine forbids it for built-in tribes), so we inject
#     it here during packaging.
# ---------------------------------------------------------------------------

echo "Injecting __skip_if_exists into register.lua files..."
INJECTED=0
while IFS= read -r -d '' regfile; do
    # Empty attribute tables:  = {}  →  = { "__skip_if_exists" }
    sed -i 's/= {}/= { "__skip_if_exists" }/' "$regfile"
    # Non-empty attribute tables (if not already patched):
    #   = { "resi"  →  = { "__skip_if_exists", "resi"
    sed -i '/__skip_if_exists/!s/= { "/= { "__skip_if_exists", "/' "$regfile"
    INJECTED=$((INJECTED + 1))
done < <(find "$OUTPUT_DIR/tribes" -name "register.lua" -print0 2>/dev/null)
echo "  Injected __skip_if_exists into $INJECTED register.lua files"

# ---------------------------------------------------------------------------
# 11. Create icon.png
#     Use one of the Hebrew building menu images as the add-on icon.
#     Resize to 64x64 if ImageMagick is available, otherwise just copy.
# ---------------------------------------------------------------------------

echo "Creating icon.png..."
ICON_SOURCE="$TRIBES_DIR/initialization/hebrews/images/icon.png"
if [ ! -f "$ICON_SOURCE" ]; then
    # Fallback: use the headquarters menu icon
    ICON_SOURCE="$TRIBES_DIR/buildings/warehouses/hebrews/headquarters/menu.png"
fi

if [ -f "$ICON_SOURCE" ]; then
    if command -v convert &>/dev/null; then
        convert "$ICON_SOURCE" -resize 64x64 "$OUTPUT_DIR/icon.png"
        echo "  Resized icon from: $(basename "$ICON_SOURCE")"
    elif command -v magick &>/dev/null; then
        magick "$ICON_SOURCE" -resize 64x64 "$OUTPUT_DIR/icon.png"
        echo "  Resized icon from: $(basename "$ICON_SOURCE")"
    else
        cp "$ICON_SOURCE" "$OUTPUT_DIR/icon.png"
        echo "  Copied icon (ImageMagick not found, no resize): $(basename "$ICON_SOURCE")"
    fi
else
    echo "  WARNING: No suitable icon source found!"
fi

# ---------------------------------------------------------------------------
# 12. Patch textdomains in building/worker/ware init.lua files
#     Replace push_textdomain("tribes") with the add-on textdomain
# ---------------------------------------------------------------------------

echo "Patching textdomains in Lua files..."
PATCHED=0
while IFS= read -r -d '' luafile; do
    if grep -q 'push_textdomain("tribes")' "$luafile" 2>/dev/null; then
        sed -i 's/push_textdomain("tribes")/push_textdomain("hebrews_tribe.wad", true)/' "$luafile"
        PATCHED=$((PATCHED + 1))
    fi
    if grep -q 'push_textdomain("tribes_encyclopedia")' "$luafile" 2>/dev/null; then
        sed -i 's/push_textdomain("tribes_encyclopedia")/push_textdomain("hebrews_tribe.wad", true)/' "$luafile"
        PATCHED=$((PATCHED + 1))
    fi
done < <(find "$OUTPUT_DIR/tribes" -name "*.lua" -print0 2>/dev/null)
echo "  Patched $PATCHED textdomain references"

# ---------------------------------------------------------------------------
# 13. Summary
# ---------------------------------------------------------------------------

echo ""
echo "============================================"
echo "  Hebrew Tribe Add-on Build Complete"
echo "============================================"
echo ""
echo "Output directory: $OUTPUT_DIR"
echo ""

# Count files and calculate size
FILE_COUNT=$(find "$OUTPUT_DIR" -type f | wc -l)
DIR_COUNT=$(find "$OUTPUT_DIR" -type d | wc -l)
TOTAL_SIZE=$(du -sh "$OUTPUT_DIR" | cut -f1)

echo "  Files:       $FILE_COUNT"
echo "  Directories: $DIR_COUNT"
echo "  Total size:  $TOTAL_SIZE"
echo ""

# Breakdown by category
echo "  Breakdown:"
for subdir in tribes/buildings tribes/workers tribes/wares tribes/ships \
              tribes/immovables tribes/hebrews tribes/scripting locale; do
    if [ -d "$OUTPUT_DIR/$subdir" ]; then
        count=$(find "$OUTPUT_DIR/$subdir" -type f | wc -l)
        size=$(du -sh "$OUTPUT_DIR/$subdir" | cut -f1)
        printf "    %-30s %4d files  (%s)\n" "$subdir" "$count" "$size"
    fi
done
echo ""

# ---------------------------------------------------------------------------
# 14. Deploy to Flatpak location (if applicable)
# ---------------------------------------------------------------------------

if [ -d "$HOME/.var/app/org.widelands.Widelands/.widelands" ]; then
    echo "Deploying to Flatpak location..."
    if [ -d "$FLATPAK_DIR" ]; then
        rm -rf "$FLATPAK_DIR"
    fi
    cp -r "$OUTPUT_DIR" "$FLATPAK_DIR"
    echo "  Deployed to: $FLATPAK_DIR"

    # Deploy translations to addons_i18n (where the engine actually looks)
    FLATPAK_I18N="$HOME/.var/app/org.widelands.Widelands/.widelands/addons_i18n/${ADDON_NAME}.wad"
    mkdir -p "$FLATPAK_I18N"
    if [ -f "$OUTPUT_DIR/locale/de/LC_MESSAGES/hebrews_tribe.wad.po" ]; then
        cp "$OUTPUT_DIR/locale/de/LC_MESSAGES/hebrews_tribe.wad.po" "$FLATPAK_I18N/de.po"
        echo "  Deployed German translations to: $FLATPAK_I18N/de.po"
    fi
fi

# Also deploy translations for native install
NATIVE_I18N="$HOME/.widelands/addons_i18n/${ADDON_NAME}.wad"
mkdir -p "$NATIVE_I18N"
if [ -f "$OUTPUT_DIR/locale/de/LC_MESSAGES/hebrews_tribe.wad.po" ]; then
    cp "$OUTPUT_DIR/locale/de/LC_MESSAGES/hebrews_tribe.wad.po" "$NATIVE_I18N/de.po"
    echo "  Deployed German translations to: $NATIVE_I18N/de.po"
fi

echo ""
echo "Installed to: $OUTPUT_DIR"
echo ""
echo "Done."
