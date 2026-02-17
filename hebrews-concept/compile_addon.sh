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
OUTPUT_DIR="$SCRIPT_DIR/${ADDON_NAME}.wad"
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
sync_safe=false
MANIFEST

# ---------------------------------------------------------------------------
# 2. Copy tribe initialization files
#    These contain the tribe definition (init.lua, units.lua, starting
#    conditions, flag/frontier/bridge images, road textures, etc.)
# ---------------------------------------------------------------------------

echo "Copying tribe initialization files..."
mkdir -p "$OUTPUT_DIR/tribes/initialization/hebrews"
cp -r "$TRIBES_DIR/initialization/hebrews/"* "$OUTPUT_DIR/tribes/initialization/hebrews/"

# Patch init.lua to use the add-on textdomain instead of "tribes"
sed -i 's/push_textdomain("tribes")/push_textdomain("hebrews_tribe.wad", true)/' \
    "$OUTPUT_DIR/tribes/initialization/hebrews/init.lua"

# Patch units.lua to use the add-on textdomain instead of "tribes_encyclopedia"
sed -i 's/push_textdomain("tribes_encyclopedia")/push_textdomain("hebrews_tribe.wad", true)/' \
    "$OUTPUT_DIR/tribes/initialization/hebrews/units.lua"

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

# Hebrew resource indicators (hebrews_resi_*)
# These may be defined inline or in separate directories
for resi in hebrews_resi_none hebrews_resi_iron_1 hebrews_resi_iron_2 \
            hebrews_resi_gold_1 hebrews_resi_gold_2 \
            hebrews_resi_stones_1 hebrews_resi_stones_2 \
            hebrews_resi_water; do
    if [ -d "$TRIBES_DIR/immovables/$resi" ]; then
        cp -r "$TRIBES_DIR/immovables/$resi" "$OUTPUT_DIR/tribes/immovables/"
    fi
done

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
# 10. Create register.lua
#     This file registers all Hebrew tribe descriptions with the engine.
#     It is called when the add-on is loaded.
# ---------------------------------------------------------------------------

echo "Writing register.lua..."
cat > "$OUTPUT_DIR/register.lua" << 'REGISTER_LUA'
-- register.lua for the Hebrew Tribe add-on
-- This file registers all tribe descriptions (buildings, wares, workers,
-- immovables) with the Widelands engine when the add-on is loaded.

-- Collect all register.lua files from the tribes/ subdirectory tree.
-- Each register.lua returns a table of description names to register.

-- Helper: recursively find and execute register.lua files
local function collect_registrations(basepath)
   local result = {}
   -- The engine's include mechanism handles path resolution
   -- We register each component type separately
   return result
end

-- Register the tribe itself
return {
   -- The order matters: wares and workers must be registered before
   -- buildings that reference them.

   -- Wares
   "tribes/wares/branch/register.lua",
   "tribes/wares/log/register.lua",
   "tribes/wares/granite/register.lua",
   "tribes/wares/clay/register.lua",
   "tribes/wares/water/register.lua",
   "tribes/wares/wheat/register.lua",
   "tribes/wares/wheat_grains/register.lua",
   "tribes/wares/flour/register.lua",
   "tribes/wares/bread_hebrews/register.lua",
   "tribes/wares/fish/register.lua",
   "tribes/wares/meat/register.lua",
   "tribes/wares/olives/register.lua",
   "tribes/wares/olive_oil/register.lua",
   "tribes/wares/grape/register.lua",
   "tribes/wares/wine/register.lua",
   "tribes/wares/copper_ore/register.lua",
   "tribes/wares/copper/register.lua",
   "tribes/wares/gold_ore/register.lua",
   "tribes/wares/gold_leaf/register.lua",
   "tribes/wares/menorah/register.lua",
   "tribes/wares/sheep/register.lua",
   "tribes/wares/wool/register.lua",
   "tribes/wares/fur/register.lua",
   "tribes/wares/yarn/register.lua",
   "tribes/wares/cloth/register.lua",
   "tribes/wares/pick/register.lua",
   "tribes/wares/hammer/register.lua",
   "tribes/wares/fishing_rod/register.lua",
   "tribes/wares/zizit/register.lua",
   "tribes/wares/tallit_katan/register.lua",
   "tribes/wares/tefilin/register.lua",
   "tribes/wares/tallit/register.lua",
   "tribes/wares/tunic/register.lua",
   "tribes/wares/slingshot/register.lua",
   "tribes/wares/dagger/register.lua",

   -- Workers
   "tribes/workers/hebrews/carrier/register.lua",
   "tribes/workers/hebrews/ferry/register.lua",
   "tribes/workers/hebrews/donkey/register.lua",
   "tribes/workers/hebrews/builder/register.lua",
   "tribes/workers/hebrews/stonemason/register.lua",
   "tribes/workers/hebrews/branch_collector/register.lua",
   "tribes/workers/hebrews/miner/register.lua",
   "tribes/workers/hebrews/geologist/register.lua",
   "tribes/workers/hebrews/scout/register.lua",
   "tribes/workers/hebrews/shipwright/register.lua",
   "tribes/workers/hebrews/fisher/register.lua",
   "tribes/workers/hebrews/farmer/register.lua",
   "tribes/workers/hebrews/shepherd/register.lua",
   "tribes/workers/hebrews/smelter/register.lua",
   "tribes/workers/hebrews/talmid/register.lua",
   "tribes/workers/hebrews/talmid_chacham/register.lua",
   "tribes/workers/hebrews/recruit/register.lua",
   "tribes/workers/hebrews/soldier/register.lua",

   -- Ships
   "tribes/ships/hebrews/register.lua",

   -- Immovables
   "tribes/immovables/shipconstruction_hebrews/register.lua",
   "tribes/immovables/grapevine/tiny/register.lua",
   "tribes/immovables/grapevine/small/register.lua",
   "tribes/immovables/grapevine/medium/register.lua",
   "tribes/immovables/grapevine/ripe/register.lua",
   "tribes/immovables/pond/dry/register.lua",

   -- Buildings: Warehouses
   "tribes/buildings/warehouses/hebrews/headquarters/register.lua",
   "tribes/buildings/warehouses/hebrews/headquarters_tent/register.lua",
   "tribes/buildings/warehouses/hebrews/warehouse/register.lua",
   "tribes/buildings/warehouses/hebrews/port/register.lua",

   -- Buildings: Production sites
   "tribes/buildings/productionsites/hebrews/branch_collectors_hut/register.lua",
   "tribes/buildings/productionsites/hebrews/fishers_hut/register.lua",
   "tribes/buildings/productionsites/hebrews/shepherds/register.lua",
   "tribes/buildings/productionsites/hebrews/well/register.lua",
   "tribes/buildings/productionsites/hebrews/quarry/register.lua",
   "tribes/buildings/productionsites/hebrews/clay_pit/register.lua",
   "tribes/buildings/productionsites/hebrews/brick_kiln/register.lua",
   "tribes/buildings/productionsites/hebrews/clearing_tent/register.lua",
   "tribes/buildings/productionsites/hebrews/spinning_mill/register.lua",
   "tribes/buildings/productionsites/hebrews/zizijot_makers_hut/register.lua",
   "tribes/buildings/productionsites/hebrews/scouts_house/register.lua",
   "tribes/buildings/productionsites/hebrews/mill/register.lua",
   "tribes/buildings/productionsites/hebrews/bakery/register.lua",
   "tribes/buildings/productionsites/hebrews/butchery/register.lua",
   "tribes/buildings/productionsites/hebrews/weaving_mill/register.lua",
   "tribes/buildings/productionsites/hebrews/winery/register.lua",
   "tribes/buildings/productionsites/hebrews/dressmakery/register.lua",
   "tribes/buildings/productionsites/hebrews/clay_furnace/register.lua",
   "tribes/buildings/productionsites/hebrews/gold_beater/register.lua",
   "tribes/buildings/productionsites/hebrews/workshop/register.lua",
   "tribes/buildings/productionsites/hebrews/sofers_workshop/register.lua",
   "tribes/buildings/productionsites/hebrews/weaponsmithy/register.lua",
   "tribes/buildings/productionsites/hebrews/donkeyfarm/register.lua",
   "tribes/buildings/productionsites/hebrews/machane/register.lua",
   "tribes/buildings/productionsites/hebrews/farm/register.lua",
   "tribes/buildings/productionsites/hebrews/oliveplant/register.lua",
   "tribes/buildings/productionsites/hebrews/yeshiva/register.lua",
   "tribes/buildings/productionsites/hebrews/solomons_harbour/register.lua",
   "tribes/buildings/productionsites/hebrews/coppermine/register.lua",
   "tribes/buildings/productionsites/hebrews/goldmine/register.lua",
   "tribes/buildings/productionsites/hebrews/goldmine_deep/register.lua",
   "tribes/buildings/productionsites/hebrews/granitemine/register.lua",
   "tribes/buildings/productionsites/hebrews/threshing_floor/register.lua",
   "tribes/buildings/productionsites/hebrews/vineyard/register.lua",
   "tribes/buildings/productionsites/hebrews/shipyard/register.lua",

   -- Buildings: Training sites
   "tribes/buildings/trainingsites/hebrews/trainingcamp/register.lua",

   -- Buildings: Military sites
   "tribes/buildings/militarysites/hebrews/tent_small/register.lua",
   "tribes/buildings/militarysites/hebrews/massada/register.lua",

   -- Buildings: Markets
   "tribes/buildings/markets/hebrews/market/register.lua",

   -- Tribe initialization (must be last)
   "tribes/initialization/hebrews/init.lua",
}
REGISTER_LUA

# ---------------------------------------------------------------------------
# 11. Create init.lua
#     This is the entry point for the add-on, executed by the engine.
# ---------------------------------------------------------------------------

echo "Writing init.lua..."
cat > "$OUTPUT_DIR/init.lua" << 'INIT_LUA'
-- init.lua for the Hebrew Tribe add-on
-- This file is the entry point executed when the add-on is loaded by the
-- Widelands engine. It sets up the textdomain and includes the tribe
-- initialization files.

push_textdomain("hebrews_tribe.wad", true)

-- Include the tribe initialization
-- The engine will handle loading based on register.lua
include("addons/hebrews_tribe.wad/tribes/initialization/hebrews/init.lua")

pop_textdomain()
INIT_LUA

# ---------------------------------------------------------------------------
# 12. Create icon.png
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
# 13. Patch textdomains in building/worker/ware init.lua files
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
# 14. Summary
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
              tribes/immovables tribes/initialization tribes/scripting locale; do
    if [ -d "$OUTPUT_DIR/$subdir" ]; then
        count=$(find "$OUTPUT_DIR/$subdir" -type f | wc -l)
        size=$(du -sh "$OUTPUT_DIR/$subdir" | cut -f1)
        printf "    %-30s %4d files  (%s)\n" "$subdir" "$count" "$size"
    fi
done
echo ""

echo "To install, copy the .wad directory to your Widelands addons folder:"
echo "  cp -r $OUTPUT_DIR ~/.widelands/addons/"
echo ""
echo "Done."
