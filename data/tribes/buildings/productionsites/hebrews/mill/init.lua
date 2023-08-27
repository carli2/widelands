push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_mill",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Mill"),
   icon = dirname .. "menu.png",
   size = "medium",

   buildcost = {
      clay = 4,
      granite = 5,
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
      hebrews_carrier = 1,
      hebrews_donkey = 1,
   },

   inputs = {
      { name = "wheat_grains", amount = 6 },
      { name = "olives", amount = 6 },
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("working"),
         actions = {
            -- "return=skipped" causes 10 sec delay
            -- time total: 3 * 58 + 10 = 184 sec
            "call=grind_wheat",
            "call=grind_olives",
            "return=skipped"
         }
      },
      grind_wheat = {
         -- TRANSLATORS: Completed/Skipped/Did not start grinding wheat because ...
         descname = _("grinding wheat"),
         actions = {
            "return=skipped unless economy needs flour",
            "consume=wheat_grains",
            "sleep=duration:5s",
            "playsound=sound/mill/mill_turning priority:90% allow_multiple",
            "animate=working duration:30s",
            "produce=flour"
         }
      },
      grind_olives = {
         -- TRANSLATORS: Completed/Skipped/Did not start grinding wheat because ...
         descname = _("grinding olives"),
         actions = {
            "return=skipped unless economy needs olive_oil",
            "consume=olives:2",
            "sleep=duration:5s",
            "playsound=sound/mill/mill_turning priority:90% allow_multiple",
            "animate=working duration:30s",
            "produce=olive_oil"
         }
      },
   },
}

pop_textdomain()
