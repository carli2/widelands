push_textdomain("tribes")

local dirname = path.dirname (__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_dressmakery",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext ("hebrews_building", "Dressmakery"),
   icon = dirname .. "menu.png",
   size = "medium",

   buildcost = {
      clay = 5,
      granite = 2,
   },
   return_on_dismantle = {
      clay = 2,
   },

   animation_directory = dirname,
   animations = {
      idle = {hotspot = {43, 44}},
      unoccupied = {hotspot = {43, 44}},
   },
   spritesheets = {
      working = {
         hotspot = {43, 44},
         fps = 15,
         frames = 30,
         columns = 6,
         rows = 5
      }
   },

   aihints = {
      prohibited_till = 450,
   },

   working_positions = {
      hebrews_carrier = 1
   },

   inputs = {
      { name = "cloth", amount = 6 },
      { name = "zizit", amount = 8 },
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("working"),
         actions = {
            -- time total: 9 * 71 = 639 sec
            "call=produce_tunic",
            "call=produce_tallit_katan",
            "call=produce_tallit",
         },
      },
      produce_tunic = {
         -- TRANSLATORS: Completed/Skipped/Did not start forging a felling ax because ...
         descname = _("sewing a tunic"),
         actions = {
            -- time: 32.4 + 35 + 3.6 = 71 sec
            "return=skipped unless economy needs tunic",
            "consume=cloth:2",
            "sleep=duration:32s400ms",
            "animate=working duration:35s",
            "produce=tunic"
         },
      },
      produce_tallit_katan = {
         -- TRANSLATORS: Completed/Skipped/Did not start forging a felling ax because ...
         descname = _("sewing a tunic"),
         actions = {
            -- time: 32.4 + 35 + 3.6 = 71 sec
            "return=skipped unless economy needs tallit_katan",
            "consume=cloth:1 zizit:4",
            "sleep=duration:32s400ms",
            "animate=working duration:35s",
            "produce=tallit_katan"
         },
      },
      produce_tallit = {
         -- TRANSLATORS: Completed/Skipped/Did not start forging a felling ax because ...
         descname = _("sewing a tunic"),
         actions = {
            -- time: 32.4 + 35 + 3.6 = 71 sec
            "return=skipped unless economy needs tallit",
            "consume=cloth:2 zizit:4",
            "sleep=duration:32s400ms",
            "animate=working duration:35s",
            "produce=tallit"
         },
      },
   },
}

pop_textdomain()
