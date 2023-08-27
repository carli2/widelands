push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_weaponsmithy",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Weapon Smithy"),
   icon = dirname .. "menu.png",
   size = "medium",

   buildcost = {
      clay = 6,
      granite = 2,
   },
   return_on_dismantle = {
      granite = 1,
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 56, 67 },
      },
      working = {
         basename = "idle", -- TODO(GunChleoc): No animation yet.
         hotspot = { 56, 67 },
      }
   },

   aihints = {
      prohibited_till = 1400
   },

   working_positions = {
      hebrews_blacksmith = 1
   },

   inputs = {
      { name = "fur", amount = 8 },
      { name = "granite", amount = 8 },
      { name = "iron", amount = 8 },
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("working"),
         actions = {
            -- time total: 54 + 4 * 81 = 378 sec
            "call=produce_slingshot",
            "call=produce_dagger",
            "call=produce_sword_short",
         }
      },
      produce_slingshot = {
         -- TRANSLATORS: Completed/Skipped/Did not start forging a light trident because ...
         descname = _("forging a slingshot"),
         actions = {
            -- time: 20.4 + 21 + 9 + 3.6 = 54 sec
            "return=skipped unless economy needs slingshot",
            "consume=fur granite",
            "sleep=duration:20s400ms",
            "playsound=sound/smiths/smith priority:50% allow_multiple",
            "animate=working duration:21s",
            "playsound=sound/smiths/sharpening priority:90%",
            "sleep=duration:9s",
            "produce=slingshot"
         }
      },
      produce_dagger = {
         -- TRANSLATORS: Completed/Skipped/Did not start forging a long trident because ...
         descname = _("forging a dagger"),
         actions = {
            -- time: 32.4 + 36 + 9 + 3.6 = 81 sec
            "return=skipped unless economy needs dagger",
            "consume=fur iron",
            "sleep=duration:32s400ms",
            "playsound=sound/smiths/smith priority:50% allow_multiple",
            "animate=working duration:36s",
            "playsound=sound/smiths/sharpening priority:90%",
            "sleep=duration:9s",
            "produce=dagger"
         }
      },
      produce_sword_short = {
         -- TRANSLATORS: Completed/Skipped/Did not start forging a steel trident because ...
         descname = _("forging a short sword"),
         actions = {
            -- time: 32.4 + 36 + 9 + 3.6 = 81 sec
            "return=skipped unless economy needs sword_short",
            "consume=fur iron:2",
            "sleep=duration:32s400ms",
            "playsound=sound/smiths/smith priority:50% allow_multiple",
            "animate=working duration:36s",
            "playsound=sound/smiths/sharpening priority:90%",
            "sleep=duration:9s",
            "produce=sword_short"
         }
      },
   },
}

pop_textdomain()
