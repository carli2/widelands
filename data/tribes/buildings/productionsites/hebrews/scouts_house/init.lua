push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_scouts_hut",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Scout’s House"),
   icon = dirname .. "menu.png",
   size = "mine",

   buildcost = {
      clay = 2,
      granite = 2,
      branch = 1,
   },
   return_on_dismantle = {
      granite = 1
   },

   animation_directory = dirname,

   spritesheets = {
      idle = {
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 40, 67 },
      },
   },

   aihints = {},

   working_positions = {
      hebrews_scout = 1
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start scouting because ...
         descname = _("scouting"),
         actions = {
            "sleep=duration:30s",
            "callworker=scout"
         }
      },
      targeted_scouting = {
         descname = _("scouting"),
         actions = {
            "callworker=targeted_scouting"
         }
      },
   },
}

pop_textdomain()
