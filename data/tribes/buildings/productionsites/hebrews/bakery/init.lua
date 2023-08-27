push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_bakery",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("barbarians_building", "Bakery"),
   icon = dirname .. "menu.png",
   size = "medium",

   buildcost = {
      log = 2,
   },
   return_on_dismantle = {
      log = 1,
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 41, 58 },
      },
      unoccupied = {
         hotspot = { 41, 58 },
      },
      working = {
         basename = "idle", -- TODO(GunChleoc): No animation yet.
         hotspot = { 41, 58 },
      },
   },

   aihints = {
      prohibited_till = 500
   },

   working_positions = {
      hebrews_baker = 1
   },

   inputs = {
      { name = "water", amount = 6 },
      { name = "flour", amount = 6 },
      { name = "olive_oil", amount = 6 }
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start baking bread because ...
         descname = pgettext("hebrews_building", "baking bread"),
         actions = {
            -- time total: 20.8 + 2 * (20 + 3.6) = 68 sec
            "return=skipped unless economy needs barbarians_bread",
            "consume=water:3 flour:3 olive_oil:1",
            "sleep=duration:20s800ms",
            "animate=working duration:20s",
            "produce=barbarians_bread",
            "animate=working duration:20s",
            "produce=barbarians_bread",
            "animate=working duration:20s",
            "produce=barbarians_bread",
            "animate=working duration:20s",
            "produce=barbarians_bread"
         }
      },
   },
}

pop_textdomain()
