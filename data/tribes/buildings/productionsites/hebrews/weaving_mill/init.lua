push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_weaving_mill",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("empire_building", "Weaving Mill"),
   icon = dirname .. "menu.png",
   size = "medium",

   buildcost = {
      clay = 6,
      granite = 3,
   },
   return_on_dismantle = {
      granite = 2
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 65, 62 },
      },
      unoccupied = {
         hotspot = { 65, 62 },
      },
   },

   spritesheets = {
      build = {
         frames = 4,
         rows = 2,
         columns = 2,
         hotspot = { 65, 62 }
      },
      working = {
         fps = 5,
         frames = 11,
         rows = 4,
         columns = 3,
         hotspot = { 65, 62 }
      },
   },

   aihints = {
      prohibited_till = 420
   },

   working_positions = {
      hebrews_carrier = 2
   },

   inputs = {
      { name = "yarn", amount = 12 }
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start weaving because ...
         descname = _("weaving"),
         actions = {
            "return=skipped unless economy needs cloth",
            "consume=yarn:6",
            "sleep=duration:20s",
            "playsound=sound/mill/weaving priority:90%",
            "animate=working duration:15s", -- Unsure of balancing CW
            "sleep=duration:5s",
            "produce=cloth"
         }
      },
   },
}

pop_textdomain()
