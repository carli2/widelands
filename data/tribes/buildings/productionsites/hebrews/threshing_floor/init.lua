push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_threshing_floor",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Threshing Floor"),
   icon = dirname .. "menu.png",
   size = "medium",

   buildcost = {
      clay = 2,
      granite = 2
   },
   return_on_dismantle = {
      granite = 2,
   },

   animation_directory = dirname,
   spritesheets = {
      idle = {
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 44, 90 },
      },
      build = {
         frames = 2,
         columns = 2,
         rows = 1,
         hotspot = { 44, 90 },
      },
      unoccupied = {
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 44, 90 },
      },
      working = {
         fps = 15,
         frames = 19,
         columns = 10,
         rows = 2,
         hotspot = { 44, 90 },
      },
   },

   aihints = {
      prohibited_till = 540
   },

   working_positions = {
      hebrews_carrier = 3,
   },

   inputs = {
      { name = "wheat", amount = 6 }
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start grinding wheat because ...
         descname = _("threshing wheat"),
         actions = {
            "return=skipped unless economy needs wheat_grains",
            "consume=wheat",
            "sleep=duration:5s",
            "animate=working duration:20s",
            "produce=wheat_grains"
         }
      },
   },
}

pop_textdomain()
