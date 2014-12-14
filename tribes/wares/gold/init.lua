dirname = path.dirname(__file__)

tribes:new_ware_type{
   name = "gold",
   -- TRANSLATORS: This is a ware name used in lists of wares
   descname = _"Gold",
   -- TRANSLATORS: mass description, e.g. 'The economy needs ...'
   genericname = _"gold",
   default_target_quantity = {
		atlanteans = 20,
		barbarians = 20,
		empire = 20
	},
   preciousness = {
		atlanteans = 2,
		barbarians = 2,
		empire = 2
	},
   helptext = {
		-- TRANSLATORS: Helptext for a ware: Gold
		default = _"Gold is the most valuable of all metals, and it is obtained out of gold ore in a smelting works.",
		-- TRANSLATORS: Helptext for a ware: Gold
		atlanteans = _"Gold is used by the armor smithy, the weapon smithy and the gold spinning mill.",
		-- TRANSLATORS: Helptext for a ware: Gold
		barbarians = _"Only very important things are embellished with gold. It is used as a precious building material and to produce different axes (in the war mill) and different parts of armor (in the helm smithy).",
		-- TRANSLATORS: Helptext for a ware: Gold
		empire = _"Armor and weapons are embellished with gold in the armor smithy and the weapon smithy."
   },
   animations = {
      idle = {
         pictures = { dirname .. "idle.png" },
         hotspot = { 4, 10 },
      },
   }
}
