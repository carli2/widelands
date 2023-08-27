-- =======================================================================
--             Headquarters starting conditions for Barbarians
-- =======================================================================

include "scripting/infrastructure.lua"

push_textdomain("tribes")

init = {
   -- TRANSLATORS: This is the name of a starting condition
   descname = _("Headquarters"),
   -- TRANSLATORS: This is the tooltip for the "Headquarters" starting condition
   tooltip = _("Start the game with your headquarters only"),
   func = function(player, shared_in_start)

   local sf = wl.Game().map.player_slots[player.number].starting_field
   if shared_in_start then
      sf = shared_in_start
   else
      player:allow_workers("all")
   end

   hq = prefilled_buildings(player, { "hebrews_headquarters", sf.x, sf.y,
      wares = {
	 granite = 30,
	 clay = 50,
         log = 5,
	 pick = 5,
	 felling_ax = 2,
	 sheep2 = 5,
	 water = 15,
	 hammer = 8,
	 iron = 10,
      },
      workers = {
	 hebrews_donkey = 5,
         hebrews_builder = 5,
	 hebrews_stonemason = 1,
	 hebrews_claydigger = 1,
	 hebrews_geologist = 1,
	 hebrews_fisher = 3,
	 hebrews_blacksmith = 1,
      },
      soldiers = {
         [{0,0,0,0}] = 45,
      }
   })
end
}

pop_textdomain()
return init
