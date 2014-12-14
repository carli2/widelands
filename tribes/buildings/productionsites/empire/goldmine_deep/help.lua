-- The Imperial Deep Gold Mine
include "scripting/formatting.lua"
set_textdomain("tribes")
include "tribes/scripting/format_help.lua"

return {
   func = function(building_description)
	return

	--Lore Section
	building_help_lore_string("empire", building_description, _[[Text needed]], _[[Text needed]]) ..

	--General Section
	building_help_general_string("empire", building_description,
		_"Digs gold ore out of the ground in mountain terrain.") ..

	--Dependencies
	building_help_dependencies_production("empire", building_description) ..

	--Workers Section
	building_help_crew_string("empire", building_description) ..

	--Building Section
	building_help_building_section("empire", building_description, "empire_goldmine", {"empire_goldmine"}) ..

	--Production Section
	building_help_production_section(_[[Calculation needed]])
  end
}
