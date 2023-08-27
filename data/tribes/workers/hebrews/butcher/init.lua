push_textdomain("tribes")

local dirname = path.dirname (__file__)

wl.Descriptions():new_worker_type {
   name = "hebrews_butcher",
   -- TRANSLATORS: This is a worker name used in lists of workers
   descname = pgettext ("hebrews_worker", "Butcher"),
   animation_directory = dirname,
   icon = dirname .. "menu.png",
   vision_range = 2,

   buildcost = {
      hebrews_carrier = 1,
   },

   ware_hotspot = {0, 20},

   spritesheets = {
      walk = {
         fps = 15,
         frames = 10,
         columns = 5,
         rows = 2,
         directional = true,
         hotspot = {11, 24}
      },
   },
   animations = {
      idle = {
         hotspot = {8, 23}
      },
   },
}

pop_textdomain()
