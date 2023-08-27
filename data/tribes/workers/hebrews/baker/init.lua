push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_worker_type {
   name = "hebrews_baker",
   -- TRANSLATORS: This is a worker name used in lists of workers
   descname = pgettext("hebrews_worker", "Baker"),
   animation_directory = dirname,
   icon = dirname .. "menu.png",
   vision_range = 2,

   buildcost = {
      hebrews_carrier = 1,
   },

   animations = {
      idle = {
         hotspot = { 5, 26 }
      },
   },

   spritesheets = {
      walk = {
         fps = 10,
         frames = 20,
         rows = 5,
         columns = 4,
         directional = true,
         hotspot = { 14, 26 }
      },
      walkload = {
         fps = 10,
         frames = 20,
         rows = 5,
         columns = 4,
         directional = true,
         hotspot = { 8, 26 }
      },
   },
}

pop_textdomain()
