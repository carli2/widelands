push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_warehouse_type {
   name = "hebrews_port",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Port"),
   icon = dirname .. "menu.png",
   size = "port",
   map_check = {"seafaring"},

   buildcost = {
      log = 12,
      -- TODO: blattgold etc.
   },
   return_on_dismantle = {
      log = 6,
   },

   animation_directory = dirname,
   spritesheets = {
      idle = {
         fps = 10,
         frames = 20,
         columns = 10,
         rows = 2,
         hotspot = { 87, 116 }
      },
      build = {
         frames = 4,
         columns = 4,
         rows = 1,
         hotspot = { 87, 116 }
      },
   },

   aihints = {
      prohibited_till = 1000
   },

   conquers = 5,
   heal_per_second = 170,
   max_garrison = 20,
}

pop_textdomain()
