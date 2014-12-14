dirname = path.dirname(__file__)

tribes:new_productionsite_type {
   name = "empire_marblemine",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = _"Marble Mine",
   size = "mine",
   enhancement = "empire_marblemine_deep",

   buildcost = {
		log = 4,
		planks = 2
	},
	return_on_dismantle = {
		log = 2,
		planks = 1
	},

	-- #TRANSLATORS: Helptext for a building: Marble Mine
   helptext = "", -- NOCOM(GunChleoc): See what we can shift over from help.lua here

   animations = {
		idle = {
			pictures = { dirname .. "idle_\\d+.png" },
			hotspot = { 49, 49 },
		},
		working = {
			pictures = { dirname .. "working_\\d+.png" },
			hotspot = { 49, 49 },
			fps = 10
		},
		empty = {
			pictures = { dirname .. "empty_\\d+.png" },
			hotspot = { 49, 49 },
		},
	},

   aihints = {
		mines = "granite",
		mines_percent = 50,
		prohibited_till = 450
   },

	working_positions = {
		empire_miner = 1
	},

   inputs = {
		ration = 6,
		wine = 6
	},
   outputs = {
		"marble",
		"granite"
   },

	programs = {
		work = {
			-- TRANSLATORS: Completed/Skipped/Did not start working because ...
			descname = _"working",
			actions = {
				"call=mine_marble",
				"call=mine_granite",
				"return=skipped"
			}
		},
		mine_marble = {
			-- TRANSLATORS: Completed/Skipped/Did not start mining marble because ...
			descname = _"mining marble",
			actions = {
				"sleep=20000",
				"return=skipped unless economy needs marble or economy needs granite",
				"consume=wine ration",
				"animate=working 20000",
				"mine=granite 2 50 5 17",
				"produce=marble:2",
				"animate=working 20000",
				"mine=granite 2 50 5 17",
				"produce=marble granite"
			}
		},
		mine_granite = {
			-- TRANSLATORS: Completed/Skipped/Did not start mining granite because ...
			descname = _"mining granite",
			actions = {
				"sleep=20000",
				"return=skipped unless economy needs marble or economy needs granite",
				"consume=ration wine",
				"animate=working 20000",
				"mine=granite 2 50 5 17",
				"produce=granite marble",
				"animate=working 20000",
				"mine=granite 2 50 5 17",
				"produce=granite:2"
			}
		},
	},
	out_of_resource_notification = {
		title = _"Main Marble Vein Exhausted",
		message =
			_"This marble mine’s main vein is exhausted. Expect strongly diminished returns on investment." .. " " ..
			-- TRANSLATORS: "it" is a mine.
			_"You should consider enhancing, dismantling or destroying it.",
		delay_attempts = 0
	},
}
