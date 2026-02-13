push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_yeshiva",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Yeshiva"),
   icon = dirname .. "menu.png",
   size = "big",

   buildcost = {
      log = 2,
   },
   return_on_dismantle = {
      log = 1,
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 70, 80 }
      },
   },

   aihints = {
      prohibited_till = 900,
      very_weak_ai_limit = 1,
      weak_ai_limit = 2
   },

   working_positions = {
      hebrews_rebbe = 1
   },

   inputs = {
      { name = "zizit", amount = 4 },
      { name = "tallit_katan", amount = 4 },
      { name = "tefilin", amount = 4 },
      { name = "tallit", amount = 4 },
      { name = "hebrews_carrier", amount = 4 },
      { name = "hebrews_talmid", amount = 4 },
      { name = "hebrews_talmid_chacham", amount = 4 },
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = pgettext("hebrews_building", "working"),
         actions = {
            "call=talmid",
            "call=talmid_chacham",
            "call=rebbe",
         }
      },
      talmid = {
         -- TRANSLATORS: Completed/Skipped/Did not start educating Talmid because ...
         descname = pgettext("hebrews_building", "educating Talmid"),
         actions = {
            "return=skipped unless economy needs hebrews_talmid",
            "consume=zizit tallit_katan hebrews_carrier",
            "sleep=duration:15s",
            "recruit=hebrews_talmid"
         }
      },
      talmid_chacham = {
         -- TRANSLATORS: Completed/Skipped/Did not start educating Talmid Chacham because ...
         descname = pgettext("hebrews_building", "educating Talmid Chacham"),
         actions = {
            "return=skipped unless economy needs hebrews_talmid_chacham",
            "consume=tefilin hebrews_talmid",
            "sleep=duration:15s",
            "recruit=hebrews_talmid_chacham"
         }
      },
      rebbe = {
         -- TRANSLATORS: Completed/Skipped/Did not start educating Rebbe because ...
         descname = pgettext("hebrews_building", "educating Rebbe"),
         actions = {
            "return=skipped unless economy needs hebrews_rebbe",
            "consume=tallit hebrews_talmid_chacham",
            "sleep=duration:15s",
            "recruit=hebrews_rebbe"
         }
      },
   }
}

pop_textdomain()
