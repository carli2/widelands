push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "hebrews_spinning_mill",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("hebrews_building", "Spinning Mill"),
   icon = dirname .. "menu.png",
   size = "small",

   buildcost = {
      clay = 4,
      granite = 1,
   },
   return_on_dismantle = {
      granite = 1
   },

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 34, 74 },
      },
      working = {
         basename = "idle", -- TODO(GunChleoc): No animation yet.
         hotspot = { 34, 74 },
      }
   },

   aihints = {
      prohibited_till = 210
   },

   working_positions = {
      hebrews_carrier = 1
   },

   inputs = {
      { name = "wool", amount = 6 }
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start spinning gold because ...
         descname = _("spinning"),
         actions = {
            "return=skipped unless economy needs yarn",
            "consume=wool",
            "sleep=duration:15s",
            "playsound=sound/atlanteans/goldspin priority:50% allow_multiple",
            "animate=working duration:15s",
            "produce=yarn",
            "produce=yarn"
         }
      },
   },
}

pop_textdomain()
