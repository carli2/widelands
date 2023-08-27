push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_quarry",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Quarry"),
   icon = dirname .. "menu.png",
   size = "small",

   buildcost = {
      clay = 4
   },
   return_on_dismantle = {
      clay = 2
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 45, 48 },
      },
      unoccupied = {
         hotspot = { 45, 48 },
      },
   },

   spritesheets = {
      build = {
         frames = 4,
         rows = 2,
         columns = 2,
         hotspot = { 45, 48 }
      },
   },

   aihints = {},

   working_positions = {
      hebrews_stonemason = 1,
      hebrews_donkey = 1,
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start quarrying granite because ...
         descname = _("quarrying granite"),
         actions = {
            "callworker=cut_granite",
            "sleep=duration:17s500ms"
         }
      },
   },
   out_of_resource_notification = {
      -- Translators: Short for "Out of ..." for a resource
      title = _("No Rocks"),
      heading = _("Out of Rocks"),
      message = pgettext("hebrews_building", "The stonemason working at this quarry can’t find any rocks in his work area."),
      productivity_threshold = 75
   },
}

pop_textdomain()
