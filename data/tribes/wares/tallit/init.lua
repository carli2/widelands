push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_ware_type {
   name = "tallit",
   -- TRANSLATORS: This is a ware name used in lists of wares
   descname = pgettext("ware", "Tallit"),
   icon = dirname .. "menu.png",

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 5, 10 },
      },
   }
}

pop_textdomain()
