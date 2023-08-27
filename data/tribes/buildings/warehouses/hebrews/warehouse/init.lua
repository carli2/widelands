push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_warehouse_type {
   name = "hebrews_warehouse",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Warehouse"),
   icon = dirname .. "menu.png",
   size = "medium",

   buildcost = {
      log = 2,
   },
   return_on_dismantle = {
      log = 1,
   },

   animation_directory = dirname,
   spritesheets = {
      idle = {
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 58, 58 }
      }
   },

   aihints = {},

   heal_per_second = 170,
}

pop_textdomain()
