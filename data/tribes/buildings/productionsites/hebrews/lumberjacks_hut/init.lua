push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_lumberjacks_hut",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Lumberjack’s Hut"),
   icon = dirname .. "menu.png",
   size = "small",

   buildcost = {
      clay = 3
   },
   return_on_dismantle = {
      clay = 2
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 43, 45 },
      },
   },

   aihints = {
      basic_amount = 1,
   },

   working_positions = {
      hebrews_lumberjack = 1
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start felling trees because ...
         descname = _("felling trees"),
         actions = {
            "callworker=harvest",
            "sleep=duration:20s"
         }
      },
   },
   out_of_resource_notification = {
      -- Translators: Short for "Out of ..." for a resource
      title = _("No Trees"),
      heading = _("Out of Trees"),
      message = pgettext("hebrews_building", "The lumberjack working at this lumberjack’s hut can’t find any trees in his work area. You should consider dismantling or destroying the building or building a ranger’s hut."),
      productivity_threshold = 60
   },
}

pop_textdomain()
