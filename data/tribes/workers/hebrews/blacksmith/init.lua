push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_worker_type {
   name = "hebrews_blacksmith",
   -- TRANSLATORS: This is a worker name used in lists of workers
   descname = pgettext("hebrews_worker", "Blacksmith"),
   animation_directory = dirname,
   icon = dirname .. "menu.png",
   vision_range = 2,

   buildcost = {
      hebrews_carrier = 1,
      hammer = 1
   },

   animations = {
      idle = {
         hotspot = { 4, 20 }
      }
   },
   spritesheets = {
      walk = {
         fps = 10,
         frames = 10,
         rows = 4,
         columns = 3,
         directional = true,
         hotspot = { 9, 20 }
      },
      walkload = {
         fps = 10,
         frames = 10,
         rows = 4,
         columns = 3,
         directional = true,
         hotspot = { 7, 20 }
      }
   }
}

pop_textdomain()
