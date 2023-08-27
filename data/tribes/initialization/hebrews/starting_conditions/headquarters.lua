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
	 granite = 20,
	 clay = 20,
         log = 5,
	 pick = 5,
	 felling_ax = 2,
	 sheep2 = 5,
      },
      workers = {
	 hebrews_donkey = 5,
         hebrews_builder = 5,
	 hebrews_stonemason = 1,
	 hebrews_claydigger = 1,
	 hebrews_geologist = 1
      },
      soldiers = {
         [{0,0,0,0}] = 45,
      }
   })
end
}

pop_textdomain()
return init
