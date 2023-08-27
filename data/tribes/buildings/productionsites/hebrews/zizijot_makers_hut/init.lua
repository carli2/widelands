push_textdomain("tribes")

local dirname = path.dirname (__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_zizijot_makers_hut",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext ("hebrews_building", "Zizijot Makers Hut"),
   icon = dirname .. "menu.png",
   size = "small",

   buildcost = {
      clay = 4,
   },
   return_on_dismantle = {
      clay = 2,
   },

   animation_directory = dirname,
   animations = {
      idle = {hotspot = {39, 46}},
      unoccupied = {hotspot = {39, 46}}
   },

   aihints = {},

   working_positions = {
      hebrews_carrier = 1
   },

   inputs = {
      { name = "yarn", amount = 8 },
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start forging a felling ax because ...
         descname = _("sewing a tunic"),
         actions = {
            -- time: 32.4 + 35 + 3.6 = 71 sec
            "return=skipped unless economy needs zizit",
            "consume=yarn:4",
            "sleep=duration:10s",
            -- TODO "animate=working duration:10s",
            "produce=zizit"
         },
      },

   },
}

pop_textdomain()
