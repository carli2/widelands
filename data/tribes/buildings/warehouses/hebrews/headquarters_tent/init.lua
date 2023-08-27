push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_warehouse_type {
   name = "hebrews_headquarters_tent",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Tabernacle"),
   icon = dirname .. "menu.png",
   size = "big",
   destructible = false,

   animation_directory = dirname,
   spritesheets = {
      idle = {
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 100, 115 },
      },
   },

   aihints = {},

   heal_per_second = 170,
   conquers = 9,
   max_garrison = 20,
}

pop_textdomain()
