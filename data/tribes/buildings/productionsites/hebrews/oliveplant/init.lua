push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_oliveplant",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Olive Plant"),
   icon = dirname .. "menu.png",
   size = "big",

   buildcost = {
      clay = 5,
      granite = 2,
   },
   return_on_dismantle = {
      clay = 1,
      granite = 2
   },

   animation_directory = dirname,
   spritesheets = {
      idle = {
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 89, 82 },
      },
      working = {
         basename = "idle", -- TODO(GunChleoc): No animation yet.
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 89, 82 },
      },
   },

   aihints = {
      prohibited_till = 380,
      very_weak_ai_limit = 1,
      weak_ai_limit = 3
   },

   working_positions = {
      hebrews_carrier = 1,
      hebrews_donkey = 1,
   },

   inputs = {
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start breeding sheep because ...
         descname = _("planting olives"),
         actions = {
            "return=skipped unless economy needs olives",
            "sleep=duration:35s",
            "animate=working duration:50s",
            "produce=olives"
         }
      },
   },
}

pop_textdomain()
