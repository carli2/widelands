push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_trainingsite_type {
   name = "hebrews_trainingcamp",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Training Camp"),
   icon = dirname .. "menu.png",
   size = "big",

   buildcost = {
      log = 2,
   },
   return_on_dismantle = {
      log = 1,
   },

   animation_directory = dirname,
   spritesheets = {
      idle = {
         frames = 1,
         columns = 1,
         rows = 1,
         hotspot = { 87, 113 }
      }
   },

   aihints = {
      prohibited_till = 1500,
      very_weak_ai_limit = 0,
      weak_ai_limit = 1
   },

   working_positions = {
      hebrews_donkey = 1
   },

   inputs = {
      { name = "fish", amount = 6 },
      { name = "meat", amount = 6 },
      { name = "barbarians_bread", amount = 10 },
   },

   programs = {
      sleep = {
         -- TRANSLATORS: Completed/Skipped/Did not start sleeping because ...
         descname = _("sleeping"),
         actions = {
            "sleep=duration:5s",
            "return=skipped",
         }
      },
      upgrade_soldier_attack_0 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier attack from level 0 to level 1"),
         actions = {
            "checksoldier=soldier:attack level:0", -- Fails when aren't any soldier of level 0 attack
            "return=failed unless site has fish,meat",
            "sleep=duration:30s",
            "checksoldier=soldier:attack level:0", -- Because the soldier can be expelled by the player
            "consume=fish,meat",
            "train=soldier:attack level:1"
         }
      },
      upgrade_soldier_attack_1 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier attack from level 1 to level 2"),
         actions = {
            "checksoldier=soldier:attack level:1",
            "return=failed unless site has fish,meat",
            "sleep=duration:30s",
            "checksoldier=soldier:attack level:1",
            "consume=fish,meat",
            "train=soldier:attack level:2"
         }
      },
      upgrade_soldier_attack_2 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier attack from level 2 to level 3"),
         actions = {
            "checksoldier=soldier:attack level:2",
            "return=failed unless site has fish,meat:2",
            "sleep=duration:30s",
            "checksoldier=soldier:attack level:2",
            "consume=fish,meat:2",
            "train=soldier:attack level:3"
         }
      },
      upgrade_soldier_attack_3 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier attack from level 3 to level 4"),
         actions = {
            "checksoldier=soldier:attack level:3",
            "return=failed unless site has fish,meat",
            "sleep=duration:30s",
            "checksoldier=soldier:attack level:3",
            "consume=fish,meat",
            "train=soldier:attack level:4"
         }
      },
      upgrade_soldier_health_0 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier health from level 0 to level 1"),
         actions = {
            "checksoldier=soldier:health level:0",
            "return=failed unless site has fish,meat",
            "sleep=duration:30s",
            "checksoldier=soldier:health level:0",
            "consume=fish,meat",
            "train=soldier:health level:1"
         }
      },
      upgrade_soldier_health_1 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier health from level 1 to level 2"),
         actions = {
            "checksoldier=soldier:health level:1",
            "return=failed unless site has fish,meat",
            "sleep=duration:30s",
            "checksoldier=soldier:health level:1",
            "consume=fish,meat",
            "train=soldier:health level:2"
         }
      },
      upgrade_soldier_health_2 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier health from level 2 to level 3"),
         actions = {
            "checksoldier=soldier:health level:2",
            "return=failed unless site has fish,meat:2",
            "sleep=duration:30s",
            "checksoldier=soldier:health level:2",
            "consume=fish,meat:2",
            "train=soldier:health level:3"
         }
      },
      upgrade_soldier_health_3 = {
         -- TRANSLATORS: Completed/Skipped/Did not start upgrading ... because ...
         descname = pgettext("empire_building", "upgrading soldier health from level 3 to level 4"),
         actions = {
            "checksoldier=soldier:health level:3",
            "return=failed unless site has fish,meat:2",
            "sleep=duration:30s",
            "checksoldier=soldier:health level:3",
            "consume=fish,meat:2",
            "train=soldier:health level:4"
         }
      },
   },

   soldier_capacity = 12,
   trainer_patience = 24,

   messages = {
      -- TRANSLATORS: Empire training site tooltip when it has no soldiers assigned
      no_soldier = pgettext("empire_building", "No soldier to train!"),
      -- TRANSLATORS: Empire training site tooltip when none of the present soldiers match the current training program
      no_soldier_for_level = pgettext("empire_building", "No soldier found for this training level!"),
   },
}

pop_textdomain()
