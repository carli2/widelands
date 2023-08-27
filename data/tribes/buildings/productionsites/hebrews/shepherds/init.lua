push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_shepherds",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Shepherds Fire"),
   icon = dirname .. "menu.png",
   size = "small",

   buildcost = {
      clay = 1,
   },
   return_on_dismantle = {
   },

   animation_directory = dirname,
   animations = {
      unoccupied = {
         hotspot = { 44, 43 },
      },
   },

   spritesheets = {
      build = {
         frames = 4,
         rows = 2,
         columns = 2,
         hotspot = { 44, 43 }
      },
      idle = {
         frames = 20,
         rows = 5,
         columns = 4,
         hotspot = { 44, 43 }
      },
   },

   aihints = {
      prohibited_till = 510,
      basic_amount = 1
   },

   working_positions = {
      hebrews_shepherd = 3
   },

   inputs = {
      { name = "water", amount = 4 },
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("working"),
         actions = {
            -- "return=skipped" causes 10 sec delay
            -- time total: 3 * 58 + 10 = 184 sec
            "call=release",
            "call=sheer",
            "call=sheer",
            "call=sheer",
            "call=hunt",
            "return=skipped"
         }
      },
      release = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("releasing sheep"),
         actions = {
            -- time total: 53 sec
            "return=skipped when not economy needs wool and not economy needs meat",
            "return=skipped unless site has water",
            "consume=water",
            "callworker=release",
            "sleep=duration:53s"
         }
      },
      hunt = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("hunting"),
         actions = {
            -- time total: 53 sec
            "return=skipped when not economy needs meat",
            "callworker=hunt",
            "sleep=duration:53s"
         }
      },
      sheer = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("sheering"),
         actions = {
            -- time total: 53 sec
            "return=skipped when not economy needs wool",
            "callworker=sheer",
            "sleep=duration:53s"
         }
      },
   },
}

pop_textdomain()
