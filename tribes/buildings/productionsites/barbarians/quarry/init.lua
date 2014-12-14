dirname = path.dirname(__file__)

tribes:new_productionsite_type {
   name = "barbarians_quarry",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = _"Quarry",
   size = "small",

   buildcost = {
		log = 4
	},
	return_on_dismantle = {
		log = 2
	},

	-- #TRANSLATORS: Helptext for a building: Quarry
   helptext = "", -- NOCOM(GunChleoc): See what we can shift over from help.lua here

   animations = {
		idle = {
			pictures = { dirname .. "idle_\\d+.png" },
			hotspot = { 45, 40 },
		},
		build = {
			pictures = { dirname .. "build_\\d+.png" },
			hotspot = { 44, 36 },
		},
	},

   aihints = {
		forced_after = 0,
		stoneproducer = true
   },

	working_positions = {
		barbarians_stonemason = 1
	},

   outputs = {
		"granite"
   },

	programs = {
		work = {
			-- TRANSLATORS: Completed/Skipped/Did not start quarrying granite because ...
			descname = _"quarrying granite",
			actions = {
			  -- This order is on purpose so that the productivity
			  -- drops fast once all rocks are gone.
				"worker=cut_granite",
				"sleep=25000"
			}
		},
	},
	out_of_resource_notification = {
		title = _"Out of Rocks",
		message = _"The stonemason working at this quarry can’t find any rocks in his working radius.",
		delay_attempts = 0
	},
}
