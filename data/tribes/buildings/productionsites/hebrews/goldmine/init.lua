push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_goldmine",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Gold Mine"),
   icon = dirname .. "menu.png",
   size = "mine",

   buildcost = {
      clay = 4,
      granite = 2
   },
   return_on_dismantle = {
      clay = 2,
      granite = 1
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 66, 88 },
      },
      empty = {
         hotspot = { 66, 88 },
      },
      working = {
         hotspot = { 66, 88 },
      },
   },

   aihints = {
      prohibited_till = 1200
   },

   working_positions = {
      hebrews_miner = 1
   },

   inputs = {
      { name = "bread_hebrews", amount = 6 }
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start mining gold because ...
         descname = _("mining gold"),
         actions = {
            "return=skipped unless economy needs gold_ore",
            "consume=bread_hebrews",
            "sleep=duration:45s400ms",
            "animate=working duration:20s",
            "mine=resource_gold radius:2 yield:33.33% when_empty:5% experience_on_fail:17%",
            "produce=gold_ore"
         }
      },
   },
   out_of_resource_notification = {
      -- Translators: Short for "Out of ..." for a resource
      title = _("No Gold"),
      heading = _("Main Gold Vein Exhausted"),
      message =
         pgettext("hebrews_building", "This gold mine's main vein is exhausted. Expect strongly diminished returns on investment. You should consider dismantling or destroying it."),
   },
}

pop_textdomain()
