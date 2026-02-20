/*
 * Copyright (C) 2026 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "ai/planner_ai.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "ai/ai_hints.h"
#include "base/log.h"
#include "base/macros.h"
#include "base/wexception.h"
#include "economy/flag.h"
#include "economy/portdock.h"
#include "economy/road.h"
#include "economy/ware_priority.h"
#include "economy/wares_queue.h"
#include "logic/game.h"
#include "logic/map.h"
#include "logic/map_objects/descriptions.h"
#include "logic/map_objects/findnode.h"
#include "logic/map_objects/immovable.h"
#include "logic/map_objects/tribes/constructionsite.h"
#include "logic/map_objects/tribes/market.h"
#include "logic/map_objects/tribes/militarysite.h"
#include "logic/map_objects/tribes/productionsite.h"
#include "logic/map_objects/tribes/ship.h"
#include "logic/map_objects/tribes/trainingsite.h"
#include "logic/map_objects/tribes/tribe_descr.h"
#include "logic/map_objects/tribes/warehouse.h"
#include "logic/mapregion.h"
#include "logic/path.h"
#include "logic/player.h"

namespace AI {

PlannerAI::EasyImpl PlannerAI::easy_impl;
PlannerAI::NormalImpl PlannerAI::normal_impl;
PlannerAI::HardImpl PlannerAI::hard_impl;

// =====================================================================
// Constructor / Destructor
// =====================================================================

PlannerAI::PlannerAI(Widelands::Game& ggame,
                     Widelands::PlayerNumber const pid,
                     int32_t desired_lead)
   : ComputerPlayer(ggame, pid), desired_lead_(desired_lead), next_ai_think_(1) {

	field_possession_subscriber_ = Notifications::subscribe<Widelands::NoteFieldPossession>(
	   [this](const Widelands::NoteFieldPossession& note) {
		   if (note.player != player_) {
			   return;
		   }
		   if (note.ownership == Widelands::NoteFieldPossession::Ownership::GAINED) {
			   unusable_fields.push_back(note.fc);
		   }
	   });

	immovable_subscriber_ = Notifications::subscribe<Widelands::NoteImmovable>(
	   [this](const Widelands::NoteImmovable& note) {
		   if (player_ == nullptr) {
			   return;
		   }
		   if (note.pi->owner().player_number() != player_->player_number()) {
			   return;
		   }
		   if (note.ownership == Widelands::NoteImmovable::Ownership::GAINED) {
			   gain_immovable(*note.pi);
		   } else {
			   lose_immovable(*note.pi);
		   }
	   });

	outofresource_subscriber_ =
	   Notifications::subscribe<Widelands::NoteProductionSiteOutOfResources>(
	      [this](const Widelands::NoteProductionSiteOutOfResources& note) {
		      if (note.ps->owner().player_number() != player_->player_number()) {
			      return;
		      }
		      for (ProductionSiteObserver& mine : mines_) {
			      if (mine.site == note.ps) {
				      if (mine.no_resources_since > game().get_gametime()) {
					      mine.no_resources_since = game().get_gametime();
				      }
				      break;
			      }
		      }
	      });

	soldiertrained_subscriber_ = Notifications::subscribe<Widelands::NoteTrainingSiteSoldierTrained>(
	   [this](const Widelands::NoteTrainingSiteSoldierTrained& note) {
		   if (note.ts->owner().player_number() != player_->player_number()) {
			   return;
		   }
		   soldier_trained(*note.ts);
	   });

	shipnotes_subscriber_ =
	   Notifications::subscribe<Widelands::NoteShip>([this](const Widelands::NoteShip& note) {
		   if (player_ == nullptr) {
			   return;
		   }
		   if (note.ship->get_owner()->player_number() != player_->player_number()) {
			   return;
		   }

		   switch (note.action) {
		   case Widelands::NoteShip::Action::kGained:
			   gain_ship(*note.ship, NewShip::kBuilt);
			   break;

		   case Widelands::NoteShip::Action::kLost:
			   for (std::deque<ShipObserver>::iterator i = allships.begin(); i != allships.end();
			        ++i) {
				   if (i->ship == note.ship) {
					   ShipObserver& so = *i;
					   if (so.guarding) {
						   assert(so.ship->get_ship_type() == Widelands::ShipType::kWarship);
						   Widelands::PortDock* dest = so.ship->get_destination_port(game());
						   for (PortSiteObserver& pso : portsites) {
							   if (dest == pso.site->get_portdock()) {
								   assert(pso.ships_assigned > 0);
								   --pso.ships_assigned;
								   break;
							   }
						   }
					   }
					   allships.erase(i);
					   break;
				   }
			   }
			   break;

		   case Widelands::NoteShip::Action::kWaitingForCommand:
			   for (ShipObserver& observer : allships) {
				   if (observer.ship == note.ship) {
					   observer.waiting_for_command_ = true;
					   break;
				   }
			   }
			   break;

		   case Widelands::NoteShip::Action::kDestinationChanged:
			   if (note.ship->get_ship_type() == Widelands::ShipType::kWarship &&
			       note.ship->get_destination_port(game()) == nullptr) {
				   for (ShipObserver& observer : allships) {
					   if (observer.ship == note.ship) {
						   observer.waiting_for_command_ = true;
						   break;
					   }
				   }
			   }
			   break;

		   default:
			   break;
		   }
	   });
}

PlannerAI::~PlannerAI() {
	while (!buildable_fields.empty()) {
		delete buildable_fields.back();
		buildable_fields.pop_back();
	}
	while (!economies.empty()) {
		delete economies.back();
		economies.pop_back();
	}
}

// =====================================================================
// Initialization
// =====================================================================

void PlannerAI::late_initialization() {
	player_ = game().get_player(player_number());
	tribe_ = &player_->tribe();
	const Time& gametime = game().get_gametime();

	verb_log_info_time(gametime, "PlannerAI(%d): initializing\n", player_number());

	// Initialize wares
	wares.resize(game().descriptions().nr_wares());
	ware_pressure_.resize(game().descriptions().nr_wares());

	// Persistent data
	persistent_data = player_->get_mutable_ai_persistent_state();
	if (!persistent_data->initialized) {
		persistent_data->initialize();
	}

	// Build BuildingObserver array
	const Widelands::DescriptionIndex& nr_buildings = game().descriptions().nr_buildings();
	for (Widelands::DescriptionIndex building_index = 0; building_index < nr_buildings;
	     ++building_index) {
		const Widelands::BuildingDescr& bld = *tribe_->get_building_descr(building_index);
		if (!tribe_->has_building(building_index) &&
		    bld.type() != Widelands::MapObjectType::MILITARYSITE) {
			continue;
		}

		const std::string& building_name = bld.name();
		const BuildingHints& bh = bld.hints();
		buildings_.resize(buildings_.size() + 1);
		BuildingObserver& bo = buildings_.back();
		bo.name = building_name.c_str();
		bo.id = building_index;
		bo.desc = &bld;
		bo.type = BuildingObserver::Type::kBoring;
		bo.cnt_built = 0;
		bo.cnt_under_construction = 0;
		bo.cnt_target = 1;
		bo.cnt_limit_by_aimode = std::numeric_limits<int32_t>::max();
		bo.cnt_upgrade_pending = 0;
		bo.stocklevel_count = 0;
		bo.stocklevel_time = Time(0);
		bo.last_dismantle_time = Time(0);
		bo.construction_decision_time = Time(0);
		bo.last_building_built = Time();
		bo.build_material_shortage = false;
		bo.current_stats = 0;
		bo.unoccupied_count = 0;
		bo.unconnected_count = 0;
		bo.new_building_overdue = 0;
		bo.add_new_building_score = 0;
		bo.mines = Widelands::INVALID_INDEX;
		bo.initial_preciousness = 0;
		bo.max_preciousness = 0;
		bo.max_needed_preciousness = 0;
		bo.expansion_type = false;
		bo.fighting_type = false;
		bo.mountain_conqueror = false;
		bo.prohibited_till = Time(0);
		bo.forced_after = Time(0);
		bo.max_trainingsites_proportion = 100;
		bo.requires_supporters = false;
		bo.substitutes_count = 0;
		bo.basic_amount = 0;

		if (bld.is_buildable()) {
			bo.set_is(BuildingAttribute::kBuildable);
		}
		if (bld.needs_seafaring()) {
			bo.set_is(BuildingAttribute::kNeedsSeafaring);
		}
		if (bld.get_isport()) {
			bo.set_is(BuildingAttribute::kPort);
		}
		if (bh.needs_water()) {
			bo.set_is(BuildingAttribute::kNeedsCoast);
		}
		if (bh.is_space_consumer()) {
			bo.set_is(BuildingAttribute::kSpaceConsumer);
		}

		// No AI hints: cnt_limit_by_aimode stays at max (unlimited).
		// The PI system decides building counts organically.

		// Production sites and mines
		if (bld.type() == Widelands::MapObjectType::PRODUCTIONSITE) {
			const Widelands::ProductionSiteDescr& prod =
			   dynamic_cast<const Widelands::ProductionSiteDescr&>(bld);
			bo.type = bld.get_ismine() ? BuildingObserver::Type::kMine :
			                             BuildingObserver::Type::kProductionsite;

			for (const auto& temp_input : prod.input_wares()) {
				bo.inputs.push_back(temp_input.first);
			}
			for (const Widelands::DescriptionIndex& temp_output : prod.output_ware_types()) {
				bo.ware_outputs.push_back(temp_output);
				wares.at(temp_output).producers.push_back(bo.id);
			}

			// Worker outputs: detect barracks (produces soldiers) and recruitment sites
			if (!prod.output_worker_types().empty()) {
				for (const Widelands::DescriptionIndex& temp_output : prod.output_worker_types()) {
					if (temp_output == tribe_->soldier()) {
						bo.set_is(BuildingAttribute::kBarracks);
					}
				}
				if (!bo.is(BuildingAttribute::kBarracks) && bo.ware_outputs.empty()) {
					bo.set_is(BuildingAttribute::kRecruitment);
				}
			}

			for (const auto& temp_position : prod.working_positions()) {
				for (uint8_t i = 0; i < temp_position.second; i++) {
					bo.positions.push_back(temp_position.first);
				}
			}

			// Mine resource type
			if (bo.type == BuildingObserver::Type::kMine) {
				iron_resource_id = game().descriptions().resource_index("resource_iron");
				const auto& collected_resources = prod.collected_resources();
				const auto& first_resource_it = collected_resources.begin();
				if (first_resource_it == collected_resources.end()) {
					bo.mines = Widelands::INVALID_INDEX;
				} else {
					bo.mines = game().descriptions().resource_index(first_resource_it->first);
				}
			}

			// Detect building attributes from collected immovables
			// (buildings that destroy map objects, e.g. woodcutter fells trees)
			if (prod.input_wares().empty() && !prod.output_ware_types().empty() &&
			    prod.created_immovables().empty() && !prod.collected_immovables().empty()) {
				for (const auto& attribute : prod.collected_attributes()) {
					if (attribute.second == Widelands::MapObjectDescr::get_attribute_id("rocks")) {
						bo.set_is(BuildingAttribute::kNeedsRocks);
						break;
					}
					if (attribute.second ==
					       Widelands::MapObjectDescr::get_attribute_id("tree") ||
					    attribute.second ==
					       Widelands::MapObjectDescr::get_attribute_id("normal_tree") ||
					    attribute.second ==
					       Widelands::MapObjectDescr::get_attribute_id("tree_balsa")) {
						bo.set_is(BuildingAttribute::kLumberjack);
						break;
					}
				}
			}

			if (!prod.collected_bobs().empty()) {
				bo.set_is(BuildingAttribute::kHunter);
			}
			if (prod.collected_resources().count("resource_fish") == 1) {
				bo.set_is(BuildingAttribute::kFisher);
			}
			if (prod.input_wares().empty()) {
				for (Widelands::DescriptionIndex ware_index : prod.output_ware_types()) {
					if (tribe_->get_ware_descr(ware_index)->name() == "water" &&
					    prod.collected_resources().count("resource_water") == 1) {
						bo.set_is(BuildingAttribute::kWell);
					}
				}
			}
			// Derive generic resource_targets from worker programs.
			// These are used for placement scoring of all resource harvesters.
			{
				// Compute threshold/saturation from workarea radius.
				uint32_t max_wa = 6;
				if (!prod.workarea_info().empty()) {
					max_wa = prod.workarea_info().rbegin()->first;
				}
				const uint8_t saturation =
				   std::max<uint8_t>(3, max_wa * max_wa * 6 / 10);
				const uint8_t threshold =
				   std::max<uint8_t>(1, saturation / 5);

				// From collected_attributes (worker destroys the object)
				for (const auto& [type, attr_id] : prod.collected_attributes()) {
					if (type == Widelands::MapObjectType::IMMOVABLE) {
						bo.resource_targets.push_back({
						   ResourceSearchTarget::Kind::kImmovableAttribute,
						   attr_id, true /*collected*/, threshold, saturation});
					} else if (type == Widelands::MapObjectType::BOB) {
						bo.resource_targets.push_back({
						   ResourceSearchTarget::Kind::kBobAttribute,
						   attr_id, true, threshold, saturation});
					}
				}
				// From needed_attributes (worker visits but doesn't destroy)
				for (const auto& [type, attr_id] : prod.needed_attributes()) {
					if (type == Widelands::MapObjectType::IMMOVABLE) {
						bool already = false;
						for (const auto& rt : bo.resource_targets) {
							if (rt.attribute_id == attr_id) {
								already = true;
								break;
							}
						}
						if (!already) {
							bo.resource_targets.push_back({
							   ResourceSearchTarget::Kind::kImmovableAttribute,
							   attr_id, false /*needed only*/, threshold, saturation});
						}
					} else if (type == Widelands::MapObjectType::BOB) {
						bool already = false;
						for (const auto& rt : bo.resource_targets) {
							if (rt.attribute_id == attr_id) {
								already = true;
								break;
							}
						}
						if (!already) {
							bo.resource_targets.push_back({
							   ResourceSearchTarget::Kind::kBobAttribute,
							   attr_id, false, threshold, saturation});
						}
					}
				}
				bo.is_resource_harvester = !bo.resource_targets.empty();
			}

			if (bh.is_shipyard()) {
				bo.set_is(BuildingAttribute::kShipyard);
			}
			if (bh.supports_seafaring()) {
				bo.set_is(BuildingAttribute::kSupportsSeafaring);
			}
			// requires_supporters derived in second pass below

			// Ranger detection
			if (!prod.supported_productionsites().empty()) {
				for (const BuildingObserver& other_bo : buildings_) {
					if (other_bo.is(BuildingAttribute::kLumberjack)) {
						const Widelands::ProductionSiteDescr* other_prod =
						   dynamic_cast<const Widelands::ProductionSiteDescr*>(other_bo.desc);
						for (const std::string& candidate : prod.supported_productionsites()) {
							if (other_prod->name() == candidate) {
								bo.set_is(BuildingAttribute::kRanger);
								break;
							}
						}
					}
				}
			}

			// Populate supported_producers map: maps DescriptionIndex of the
			// SUPPORTED building to its ProductionSiteDescr*.
			// E.g. for ranger: maps woodcutter index → woodcutter descr.
			// For gamekeeper: maps hunter index → hunter descr.
			// For fish breeder: maps fisher index → fisher descr.
			bo.supported_producers.clear();
			for (const std::string& supported_name : prod.supported_productionsites()) {
				Widelands::DescriptionIndex si = tribe_->building_index(supported_name);
				if (si != Widelands::INVALID_INDEX) {
					bo.supported_producers.insert(std::make_pair(
					   si, dynamic_cast<const Widelands::ProductionSiteDescr*>(
					           tribe_->get_building_descr(si))));
				}
			}
			// Supporting producer: has ware outputs AND supports other buildings
			// (e.g. gamekeeper produces meat AND supports hunters,
			//  fish breeder produces fish AND supports fishers)
			if (!bo.ware_outputs.empty() && !prod.supported_productionsites().empty()) {
				bo.set_is(BuildingAttribute::kSupportingProducer);
			}
			continue;
		}

		if (bld.type() == Widelands::MapObjectType::MILITARYSITE) {
			bo.type = BuildingObserver::Type::kMilitarysite;
			continue;
		}
		if (bld.type() == Widelands::MapObjectType::WAREHOUSE) {
			bo.type = BuildingObserver::Type::kWarehouse;
			continue;
		}
		if (bld.type() == Widelands::MapObjectType::TRAININGSITE) {
			bo.type = BuildingObserver::Type::kTrainingsite;
			const Widelands::TrainingSiteDescr& train =
			   dynamic_cast<const Widelands::TrainingSiteDescr&>(bld);
			for (const auto& temp_input : train.input_wares()) {
				bo.inputs.push_back(temp_input.first);
			}
			continue;
		}
		if (bld.type() == Widelands::MapObjectType::MARKET) {
			bo.type = BuildingObserver::Type::kMarket;
			continue;
		}
		if (bld.type() == Widelands::MapObjectType::CONSTRUCTIONSITE ||
		    bld.type() == Widelands::MapObjectType::DISMANTLESITE) {
			if (bld.type() == Widelands::MapObjectType::CONSTRUCTIONSITE) {
				bo.type = BuildingObserver::Type::kConstructionsite;
			}
			continue;
		}
	}

	// Precompute smallest military garrison size for overlap normalization.
	smallest_garrison_ = 100;  // start high, minimize
	for (const BuildingObserver& bo : buildings_) {
		if (bo.type == BuildingObserver::Type::kMilitarysite) {
			if (const auto* ms_desc = dynamic_cast<const Widelands::MilitarySiteDescr*>(bo.desc)) {
				smallest_garrison_ = std::min(smallest_garrison_,
				   static_cast<int32_t>(ms_desc->get_max_number_of_soldiers()));
			}
		}
	}
	smallest_garrison_ = std::max<int32_t>(1, smallest_garrison_);

	// Compute ware renewability via iterative fixed-point.
	// A ware is renewable if ANY non-depleting producer (not a mine, not a
	// quarry) has all its inputs renewable. This is tribe-agnostic:
	//   Barbarian granite: only from quarry (kNeedsRocks) + mine → non-renewable
	//   Hebrew granite: also from brick_kiln (clay+branch) → renewable
	//   Atlantean spidercloth: from weaving_mill (spider_silk) → renewable
	ware_inherently_renewable_.resize(wares.size(), false);
	{
		bool changed = true;
		while (changed) {
			changed = false;
			for (const BuildingObserver& bo : buildings_) {
				if (bo.type != BuildingObserver::Type::kProductionsite) {
					continue;
				}
				// Skip depleting producers: mines extract finite underground
				// resources, quarries (kNeedsRocks) destroy finite surface rocks.
				if (bo.is(BuildingAttribute::kNeedsRocks)) {
					continue;
				}
				// Check if all inputs are renewable
				bool all_inputs_renewable = true;
				for (const auto& input : bo.inputs) {
					if (static_cast<size_t>(input) < ware_inherently_renewable_.size() &&
					    !ware_inherently_renewable_[input]) {
						all_inputs_renewable = false;
						break;
					}
				}
				if (!all_inputs_renewable) {
					continue;
				}
				// All inputs renewable → outputs are renewable
				for (const auto& output : bo.ware_outputs) {
					if (static_cast<size_t>(output) < ware_inherently_renewable_.size() &&
					    !ware_inherently_renewable_[output]) {
						ware_inherently_renewable_[output] = true;
						changed = true;
					}
				}
			}
		}
		// Log results
		int renewable_count = 0;
		for (size_t i = 0; i < ware_inherently_renewable_.size(); ++i) {
			if (ware_inherently_renewable_[i]) {
				++renewable_count;
			}
		}
		verb_log_info_time(gametime,
		   "PlannerAI(%d): %d/%zu wares are inherently renewable\n",
		   player_number(), renewable_count, ware_inherently_renewable_.size());
	}

	// Basic economy status
	// Derive requires_supporters from game data: if any building lists
	// this one in its supported_producers map, it requires supporters.
	// E.g. woodcutter appears in ranger's supported_producers → requires_supporters.
	for (BuildingObserver& bo : buildings_) {
		for (const BuildingObserver& other : buildings_) {
			if (other.supported_producers.count(bo.id) > 0) {
				bo.requires_supporters = true;
				break;
			}
		}
	}

	// Collect all attribute IDs that any resource harvester cares about.
	// Used to filter the field scan (only count attributes someone needs).
	for (const auto& bo : buildings_) {
		for (const auto& rt : bo.resource_targets) {
			interesting_resource_attributes_.insert(rt.attribute_id);
		}
	}

	basic_economy_established = true;

	update_player_stat(gametime);

	// Initialize mines_per_type
	for (const BuildingObserver& bo : buildings_) {
		if (bo.type == BuildingObserver::Type::kMine && bo.mines != Widelands::INVALID_INDEX) {
			if (mines_per_type.count(bo.mines) == 0) {
				mines_per_type[bo.mines] = MineTypesObserver();
			}
			if (bo.mines == iron_resource_id) {
				mines_per_type[bo.mines].is_critical = true;
			}
		}
	}

	// Initialize wood policy for rangers and detect if tribe has any ranger
	tribe_has_ranger_ = false;
	for (BuildingObserver& bo : buildings_) {
		if (bo.is(BuildingAttribute::kRanger)) {
			wood_policy_[bo.id] = WoodPolicy::kAllowRangers;
			tribe_has_ranger_ = true;
		}
	}

	// Initialize building pressure vector (one per BuildingObserver)
	building_pressure_.resize(buildings_.size());

	// Initialize expansion targets (0 = unowned, 1..N = player numbers)
	expansion_targets_.resize(game().map().get_nrplayers() + 1);

	// Restore PI state from persistent data (savegame load).
	// Size mismatches are handled gracefully: use min(saved, current),
	// extra entries from new wares/buildings get zero-initialized.
	if (persistent_data->pi_tick_count > 0) {
		pi_tick_count_ = persistent_data->pi_tick_count;

		const size_t n_wp = std::min(
		   persistent_data->ware_pressure_integrals.size(), ware_pressure_.size());
		for (size_t i = 0; i < n_wp; ++i) {
			ware_pressure_[i].ipart = persistent_data->ware_pressure_integrals[i];
		}

		const size_t n_bp = std::min(
		   persistent_data->building_pressure_integrals.size(), building_pressure_.size());
		for (size_t i = 0; i < n_bp; ++i) {
			building_pressure_[i].ipart = persistent_data->building_pressure_integrals[i];
		}

		const size_t n_exp = std::min(
		   persistent_data->expansion_integrals.size(), expansion_targets_.size());
		for (size_t i = 0; i < n_exp; ++i) {
			expansion_targets_[i].ipart = persistent_data->expansion_integrals[i];
		}

		building_prevention_.resize(buildings_.size());
		const size_t n_prev = std::min(
		   persistent_data->building_prevention_integrals.size(),
		   building_prevention_.size());
		for (size_t i = 0; i < n_prev; ++i) {
			building_prevention_[i].ipart =
			   persistent_data->building_prevention_integrals[i];
		}

		// Restore lastError for D-term continuity
		{
			const size_t n = std::min(
			   persistent_data->ware_pressure_last_errors.size(), ware_pressure_.size());
			for (size_t i = 0; i < n; ++i) {
				ware_pressure_[i].lastError = persistent_data->ware_pressure_last_errors[i];
			}
		}
		{
			const size_t n = std::min(
			   persistent_data->building_pressure_last_errors.size(), building_pressure_.size());
			for (size_t i = 0; i < n; ++i) {
				building_pressure_[i].lastError = persistent_data->building_pressure_last_errors[i];
			}
		}
		{
			const size_t n = std::min(
			   persistent_data->building_prevention_last_errors.size(), building_prevention_.size());
			for (size_t i = 0; i < n; ++i) {
				building_prevention_[i].lastError = persistent_data->building_prevention_last_errors[i];
			}
		}
		{
			const size_t n = std::min(
			   persistent_data->expansion_last_errors.size(), expansion_targets_.size());
			for (size_t i = 0; i < n; ++i) {
				expansion_targets_[i].lastError = persistent_data->expansion_last_errors[i];
			}
		}

		verb_log_info_time(gametime,
		   "PlannerAI(%d): restored PID state from savegame (tick=%u, "
		   "%" PRIuS "/%" PRIuS " wares, %" PRIuS "/%" PRIuS " buildings, "
		   "%" PRIuS "/%" PRIuS " expansion targets)\n",
		   player_number(), pi_tick_count_,
		   n_wp, ware_pressure_.size(),
		   n_bp, building_pressure_.size(),
		   n_exp, expansion_targets_.size());
	}

	// Scan entire map for owned fields and existing buildings
	const Widelands::Map& map = game().map();
	std::set<Widelands::OPtr<Widelands::PlayerImmovable>> found_immovables;
	for (int16_t y = 0; y < map.get_height(); ++y) {
		for (int16_t x = 0; x < map.get_width(); ++x) {
			Widelands::FCoords f = map.get_fcoords(Widelands::Coords(x, y));
			if (f.field->get_owned_by() != player_number()) {
				continue;
			}
			unusable_fields.push_back(f);
			if (upcast(Widelands::PlayerImmovable, imm, f.field->get_immovable())) {
				if (&imm->owner() == player_ && (found_immovables.count(imm) == 0u)) {
					found_immovables.insert(imm);
					gain_immovable(*imm);
				}
			}
		}
	}

	// Block space consumers vicinity (when reloading a game)
	for (const ProductionSiteObserver& ps_obs : productionsites) {
		if (ps_obs.bo->is(BuildingAttribute::kSpaceConsumer) &&
		    !ps_obs.bo->is(BuildingAttribute::kRanger)) {
			Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
			   map, Widelands::Area<Widelands::FCoords>(
			           map.get_fcoords(ps_obs.site->get_position()), 5));
			do {
				blocked_fields.add(mr.location(), gametime + Duration(30 * 60 * 1000));
			} while (mr.advance(map));
		}
	}

	verb_log_info_time(gametime, "PlannerAI(%d): map scan found %" PRIuS " immovables, "
	         "%" PRIuS " warehousesites, %" PRIuS " productionsites, %" PRIuS " mines, "
	         "%" PRIuS " militarysites\n",
	         player_number(), found_immovables.size(),
	         warehousesites.size(), productionsites.size(), mines_.size(), militarysites.size());

	// Scan for ships (bobs on water fields)
	// Ships are found during the map scan above via NoteShip notifications,
	// but we also need to scan for ships that were already on the map
	{
		std::set<Widelands::Ship*> found_ships;
		for (int16_t y = 0; y < map.get_height(); ++y) {
			for (int16_t x = 0; x < map.get_width(); ++x) {
				Widelands::FCoords f = map.get_fcoords(Widelands::Coords(x, y));
				for (Widelands::Bob* bob = f.field->get_first_bob(); bob != nullptr;
				     bob = bob->get_next_on_field()) {
					if (upcast(Widelands::Ship, ship, bob)) {
						if (ship->get_owner() == player_ && (found_ships.count(ship) == 0u)) {
							found_ships.insert(ship);
							gain_ship(*ship, NewShip::kFoundOnLoad);
						}
					}
				}
			}
		}
	}

	// Initialize scheduler
	tasks_.clear();
	tasks_.push_back({gametime + Duration(1000), TaskId::kFieldCheck});
	tasks_.push_back({gametime + Duration(2000), TaskId::kConstructBuilding});
	tasks_.push_back({gametime + Duration(3000), TaskId::kRoadCheck});
	tasks_.push_back({gametime + Duration(4000), TaskId::kCheckEconomies});
	tasks_.push_back({gametime + Duration(5000), TaskId::kCheckProductionsites});
	tasks_.push_back({gametime + Duration(6000), TaskId::kCheckMines});
	tasks_.push_back({gametime + Duration(7000), TaskId::kCheckMilitarysites});
	tasks_.push_back({gametime + Duration(8000), TaskId::kCheckTrainingsites});
	tasks_.push_back({gametime + Duration(9000), TaskId::kCountMilitaryVacant});
	tasks_.push_back({gametime + Duration(10000), TaskId::kUpdateStats});
	tasks_.push_back({gametime + Duration(11000), TaskId::kUpdateProductionStats});
	tasks_.push_back({gametime + Duration(12000), TaskId::kCheckEnemySites});
	tasks_.push_back({gametime + Duration(15000), TaskId::kSetRangersPolicy});
	tasks_.push_back({gametime + Duration(20000), TaskId::kMarineDecisions});
	tasks_.push_back({gametime + Duration(25000), TaskId::kCheckShips});
	tasks_.push_back({gametime + Duration(90000), TaskId::kDiplomacy});
	tasks_.push_back({gametime + Duration(15 * 60 * 1000), TaskId::kReviewWareTargets});

	initialized_ = true;
}

// =====================================================================
// Main think() loop
// =====================================================================

void PlannerAI::think() {
	const Time& gametime = game().get_gametime();

	if (next_ai_think_ > gametime) {
		return;
	}

	if (!initialized_) {
		// Delay initialization past gametime 0 to let scenario scripts load
		if (gametime.get() == 0) {
			return;
		}
		verb_log_info_time(gametime, "PlannerAI(%d): late_initialization starting at t=%u\n",
		         player_number(), gametime.get());
		late_initialization();
		verb_log_info_time(gametime, "PlannerAI(%d): initialized with %" PRIuS " buildings, "
		         "%" PRIuS " wares, %" PRIuS " warehousesites\n",
		         player_number(), buildings_.size(), wares.size(),
		         warehousesites.size());
	}

	next_ai_think_ = gametime + Duration(500);

	// Run due tasks
	for (Task& task : tasks_) {
		if (task.due_time > gametime) {
			continue;
		}
		switch (task.id) {
		case TaskId::kFieldCheck:
			update_all_not_buildable_fields(gametime);
			update_all_buildable_fields(gametime);
			task.due_time = gametime + Duration(5000);
			break;

		case TaskId::kConstructBuilding:
			construct_building(gametime);
			task.due_time = gametime + Duration(4000);
			break;

		case TaskId::kRoadCheck:
			improve_roads(gametime);
			task.due_time = gametime + Duration(3000);
			break;

		case TaskId::kCheckEconomies:
			check_economies();
			task.due_time = gametime + Duration(8000);
			break;

		case TaskId::kCheckProductionsites: {
			const int32_t ps_to_check = std::min<int32_t>(productionsites.size(), 5);
			for (int32_t j = 0; j < ps_to_check; ++j) {
				if (check_productionsites(gametime)) {
					break;
				}
			}
			task.due_time = gametime + Duration(15000);
		} break;

		case TaskId::kCheckMines: {
			const int32_t mines_to_check = std::min<int32_t>(mines_.size(), 5);
			for (int32_t j = 0; j < mines_to_check; ++j) {
				if (check_mines_(gametime)) {
					break;
				}
			}
			task.due_time = gametime + Duration(15000);
		} break;

		case TaskId::kCheckMilitarysites:
			check_militarysites(gametime);
			task.due_time = gametime + Duration(15000);
			break;

		case TaskId::kCheckTrainingsites:
			check_trainingsites(gametime);
			task.due_time = gametime + Duration(30000);
			break;

		case TaskId::kCheckEnemySites:
			check_enemy_sites(gametime);
			task.due_time = gametime + Duration(19000);
			break;

		case TaskId::kCountMilitaryVacant:
			count_military_vacant_positions();
			task.due_time = gametime + Duration(25000);
			break;

		case TaskId::kUpdateStats:
			update_player_stat(gametime);
			task.due_time = gametime + Duration(15000);
			break;

		case TaskId::kDiplomacy:
			diplomacy_actions(gametime);
			task.due_time = gametime + kDiplomacyInterval + Duration(RNG::static_rand(30) * 1000);
			break;

		case TaskId::kMarineDecisions: {
			const uint8_t wait_mul = marine_main_decisions(gametime) ? 1 : 10;
			task.due_time = gametime + Duration(30000) * wait_mul;
		} break;

		case TaskId::kCheckShips: {
			const uint8_t wait_mul = check_ships(gametime) ? 1 : 10;
			task.due_time = gametime + Duration(5000) * wait_mul;
		} break;

		case TaskId::kSetRangersPolicy:
			set_rangers_policy(gametime);
			task.due_time = gametime + Duration(60000);
			break;

		case TaskId::kUpdateProductionStats:
			update_production_stats();
			task.due_time = gametime + Duration(10000);
			break;

		case TaskId::kReviewWareTargets:
			review_ware_targets();
			task.due_time = gametime + Duration(15 * 60 * 1000);
			break;
		}
	}
}

// =====================================================================
// Field management
// =====================================================================

void PlannerAI::update_all_not_buildable_fields(const Time& gametime) {
	int32_t const pn = player_number();
	uint32_t maxchecks = unusable_fields.size();
	if (maxchecks > 5) {
		maxchecks = std::min<uint32_t>(5 + (unusable_fields.size() - 5) / 10, 400);
	}
	uint32_t checked_fields = 0;

	while ((maxchecks--) != 0u) {
		if (unusable_fields.empty()) {
			break;
		}
		if (unusable_fields.front().field->get_owned_by() != pn) {
			unusable_fields.pop_front();
			continue;
		}
		if ((player_->get_buildcaps(unusable_fields.front()) & Widelands::BUILDCAPS_SIZEMASK) != 0) {
			buildable_fields.push_back(new UniversalBuildableField(unusable_fields.front()));
			unusable_fields.pop_front();
			if (20 > checked_fields++) {
				update_buildable_field(*buildable_fields.back());
				buildable_fields.back()->field_info_expiration = gametime + kFieldInfoExpiration;
			}
			continue;
		}
		if ((player_->get_buildcaps(unusable_fields.front()) & Widelands::BUILDCAPS_MINE) != 0) {
			buildable_fields.push_back(new UniversalBuildableField(unusable_fields.front()));
			buildable_fields.back()->is_mine_spot = true;
			unusable_fields.pop_front();
			if (20 > checked_fields++) {
				update_buildable_field(*buildable_fields.back());
				buildable_fields.back()->field_info_expiration = gametime + kMineFieldInfoExpiration;
			}
			continue;
		}
		unusable_fields.push_back(unusable_fields.front());
		unusable_fields.pop_front();
	}
}

void PlannerAI::update_all_buildable_fields(const Time& gametime) {
	if (buildable_fields.empty()) {
		return;
	}
	uint16_t updated = 0;
	spots_ = 0;
	trees_on_territory_ = 0;
	rocks_on_territory_ = 0;
	for (auto& [k, v] : resource_on_territory_) {
		v = 0;
	}

	for (auto it = buildable_fields.begin(); it != buildable_fields.end();) {
		UniversalBuildableField* bf = *it;

		const uint16_t caps = player_->get_buildcaps(bf->coords);
		const bool has_buildcap = bf->is_mine_spot
		   ? ((caps & Widelands::BUILDCAPS_MINE) != 0)
		   : ((caps & Widelands::BUILDCAPS_SIZEMASK) != 0);
		if (!has_buildcap || bf->coords.field->get_owned_by() != player_number()) {
			delete bf;
			it = buildable_fields.erase(it);
			continue;
		}
		++spots_;
		// Accumulate obstacle counts across all owned fields
		if (bf->immovables_by_attribute_nearby.count(BuildingAttribute::kLumberjack) > 0) {
			trees_on_territory_ +=
			   bf->immovables_by_attribute_nearby.at(BuildingAttribute::kLumberjack);
		}
		if (bf->immovables_by_attribute_nearby.count(BuildingAttribute::kNeedsRocks) > 0) {
			rocks_on_territory_ +=
			   bf->immovables_by_attribute_nearby.at(BuildingAttribute::kNeedsRocks);
		}
		// Accumulate generic resource counts for supporter depletion prediction
		for (const auto& [attr_id, count] : bf->resource_count_by_attribute) {
			resource_on_territory_[attr_id] += count;
		}

		if (bf->field_info_expiration <= gametime && updated < 30) {
			update_buildable_field(*bf);
			bf->field_info_expiration = gametime + kFieldInfoExpiration;
			++updated;
		}
		++it;
	}
}

void PlannerAI::update_buildable_field(UniversalBuildableField& field) {
	const Widelands::Map& map = game().map();

	// Reset all counters
	field.unowned_land_nearby = 0;
	field.enemy_owned_land_nearby = 0;
	field.near_border = false;
	field.enemy_nearby = false;
	field.water_nearby = 0;
	field.fish_nearby = 0;
	field.critters_nearby = 0;
	field.ground_water = 0;
	field.space_consumers_nearby = 0;
	field.rangers_nearby = 0;
	field.own_military_presence = 0;
	field.enemy_military_presence = 0;
	field.enemy_military_sites = 0;
	field.unowned_mines_spots_nearby = 0;
	field.portspace_nearby = ExtendedBool::kUnset;
	field.own_non_military_nearby = 0;
	field.military_in_constr_nearby = 0;
	for (auto& pair : field.resource_count_by_attribute) {
		pair.second = 0;
	}

	// Collect military construction sites nearby to discount their future conquest.
	// Two-pass: first collect construction site positions + radii, then during
	// unowned-land counting, skip fields that will be conquered when they finish.
	struct FutureConquest {
		Widelands::Coords pos;
		uint32_t conquer_radius;
	};
	static std::vector<FutureConquest> future_conquests;
	future_conquests.clear();
	for (auto& pair : field.immovables_by_attribute_nearby) {
		pair.second = 0;
	}
	for (auto& pair : field.immovables_by_name_nearby) {
		pair.second = 0;
	}
	// Supporter tracking (ported from DefaultAI)
	field.consumers_nearby.clear();
	field.consumers_nearby.resize(wares.size());
	field.producers_nearby.clear();
	field.producers_nearby.resize(wares.size());
	field.supported_producers_nearby.clear();
	field.buildings_nearby.clear();
	field.supporters_nearby.clear();

	Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
	   map, Widelands::Area<Widelands::FCoords>(field.coords, 6));
	do {
		const Widelands::FCoords loc = mr.location();
		const uint16_t dist = map.calc_distance(field.coords, loc);

		// Ownership
		const Widelands::PlayerNumber owner = loc.field->get_owned_by();
		if (owner == 0) {
			// Only count usable fields: building spots or mine spots.
			// Water, lava, and other impassable terrain has no value.
			const uint16_t terrain_caps = loc.field->nodecaps();
			if ((terrain_caps & (Widelands::BUILDCAPS_SIZEMASK | Widelands::BUILDCAPS_MINE)) != 0) {
				++field.unowned_land_nearby;
			}
			if (dist <= 3) {
				field.near_border = true;
			}
			// Mineable unowned spots
			if ((terrain_caps & Widelands::BUILDCAPS_MINE) != 0) {
				++field.unowned_mines_spots_nearby;
			}
		} else if (owner != player_number() &&
		           !player_statistics.players_in_same_team(player_number(), owner)) {
			// Enemy land: only count usable terrain (not water/lava).
			const uint16_t terrain_caps = loc.field->nodecaps();
			if ((terrain_caps & (Widelands::BUILDCAPS_SIZEMASK | Widelands::BUILDCAPS_MINE)) != 0) {
				++field.enemy_owned_land_nearby;
			}
			field.enemy_nearby = true;
		}

		// Terrain features (within radius 4)
		if (dist <= 4) {
			if ((loc.field->nodecaps() & Widelands::MOVECAPS_SWIM) != 0) {
				++field.water_nearby;
			}
			// Ground water for wells
			field.ground_water = std::max(field.ground_water, loc.field->get_resources_amount());
		}

		// Immovables: trees, rocks, etc.
		// Weight by proximity: closer trees/rocks are more valuable
		// (the building can actually reach them). dist 0-2: +3, dist 3-4: +2, dist 5-6: +1
		if (const Widelands::BaseImmovable* imm = loc.field->get_immovable()) {
			const uint8_t proximity_weight = (dist <= 2) ? 3 : (dist <= 4) ? 2 : 1;
			if (imm->has_attribute(Widelands::MapObjectDescr::get_attribute_id("tree")) ||
			    imm->has_attribute(Widelands::MapObjectDescr::get_attribute_id("normal_tree"))) {
				field.immovables_by_attribute_nearby[BuildingAttribute::kLumberjack] +=
				   proximity_weight;
			}
			if (imm->has_attribute(Widelands::MapObjectDescr::get_attribute_id("rocks"))) {
				field.immovables_by_attribute_nearby[BuildingAttribute::kNeedsRocks] +=
				   proximity_weight;
			}
			// Generic resource attribute counting for resource harvesters
			for (uint32_t attr : imm->descr().attributes()) {
				if (interesting_resource_attributes_.count(attr) > 0) {
					field.resource_count_by_attribute[attr] += proximity_weight;
				}
			}
			if (upcast(Widelands::Building const, bld, imm)) {
				if (bld->owner().player_number() == player_number()) {
					// Military construction sites: track for future conquest discount
					if (upcast(Widelands::ConstructionSite const, cs, bld)) {
						const Widelands::BuildingDescr& target = cs->building();
						if (target.type() == Widelands::MapObjectType::MILITARYSITE) {
							++field.military_in_constr_nearby;
							future_conquests.push_back(
							   {loc, target.get_conquers()});
						}
					}

					for (const BuildingObserver& bo : buildings_) {
						if (strcmp(bo.name, bld->descr().name().c_str()) == 0) {
							if (bo.is(BuildingAttribute::kSpaceConsumer) &&
							    !bo.is(BuildingAttribute::kRanger) && dist < 8) {
								++field.space_consumers_nearby;
							}
							if (bo.is(BuildingAttribute::kRanger)) {
								++field.rangers_nearby;
							}
							// Supporter tracking (like DefaultAI::consider_productionsite_influence)
							if (bo.type == BuildingObserver::Type::kProductionsite ||
							    bo.type == BuildingObserver::Type::kMine) {
								for (const auto& inp : bo.inputs) {
									if (static_cast<size_t>(inp) < field.consumers_nearby.size()) {
										++field.consumers_nearby[inp];
									}
								}
								for (const auto& outp : bo.ware_outputs) {
									if (static_cast<size_t>(outp) < field.producers_nearby.size()) {
										++field.producers_nearby[outp];
									}
								}
								if (const auto* prodsite =
								       dynamic_cast<const Widelands::ProductionSiteDescr*>(bo.desc)) {
									for (const auto& supported : prodsite->supported_productionsites()) {
										++field.supporters_nearby[supported];
									}
									for (const auto& supporter : prodsite->supported_by_productionsites()) {
										Widelands::DescriptionIndex si =
										   tribe_->building_index(supporter);
										++field.supported_producers_nearby[si];
									}
								}
								++field.buildings_nearby[bo.id];
							}
							if (bo.type == BuildingObserver::Type::kMilitarysite) {
								if (upcast(Widelands::MilitarySite const, ms, bld)) {
									field.own_military_presence +=
									   ms->soldier_control()->stationed_soldiers().size();
								}
							} else {
								++field.own_non_military_nearby;
							}
							break;
						}
					}
				}
			}
		}

		// Bobs: fish and critters
		for (Widelands::Bob* bob = loc.field->get_first_bob(); bob != nullptr;
		     bob = bob->get_next_on_field()) {
			if (bob->descr().type() == Widelands::MapObjectType::CRITTER) {
				++field.critters_nearby;
			}
			// Generic bob attribute counting for resource harvesters
			for (uint32_t attr : bob->descr().attributes()) {
				if (interesting_resource_attributes_.count(attr) > 0) {
					field.resource_count_by_attribute[attr] += 1;
				}
			}
		}

		// Fish (from resource overlay)
		if (loc.field->get_resources() ==
		       game().descriptions().resource_index("resource_fish") &&
		    loc.field->get_resources_amount() > 0) {
			++field.fish_nearby;
		}
	} while (mr.advance(map));

	// Port space check
	if (player_->get_buildcaps(field.coords) & Widelands::BUILDCAPS_PORT) {
		field.portspace_nearby = ExtendedBool::kTrue;
	} else {
		field.portspace_nearby = ExtendedBool::kFalse;
	}

	// Future conquest discount: subtract unowned fields that military
	// construction sites will conquer when they finish. This prevents
	// the AI from building a SECOND military building to "gain" land
	// that's already being conquered by the first one.
	//
	// For each construction site, estimate how many of the unowned_land_nearby
	// fields fall within its conquer radius. This is the overlap between
	// our scan area (radius 6) and the construction site's conquer area.
	// We re-scan the overlapping region and count actual unowned fields.
	if (!future_conquests.empty() && field.unowned_land_nearby > 0) {
		uint16_t already_claimed = 0;
		for (const FutureConquest& fc : future_conquests) {
			// Only discount fields that are both: within our scan radius (6)
			// AND within the construction site's conquer radius.
			const uint32_t effective_radius = std::min<uint32_t>(fc.conquer_radius, 6);
			Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> fcr(
			   map, Widelands::Area<Widelands::FCoords>(
			      map.get_fcoords(fc.pos), effective_radius));
			do {
				const Widelands::FCoords floc = fcr.location();
				// Count unowned USABLE fields within BOTH our scan area and their conquer area
				if (floc.field->get_owned_by() == 0 &&
				    (floc.field->nodecaps() &
				       (Widelands::BUILDCAPS_SIZEMASK | Widelands::BUILDCAPS_MINE)) != 0 &&
				    map.calc_distance(field.coords, floc) <= 6) {
					++already_claimed;
				}
			} while (fcr.advance(map));
		}
		field.unowned_land_nearby = (already_claimed >= field.unowned_land_nearby) ?
		   0 : field.unowned_land_nearby - already_claimed;
	}

	// Military score: weight by actual NEW land gain.
	// Now unowned_land_nearby already excludes fields being conquered by
	// construction sites, so it represents genuinely new territory.
	// Enemy land is more valuable (offensive expansion) → weight by 2.
	// Defensive bonus: when enemies are nearby, adjacent military buildings
	// provide redundancy (if one burns, the other holds the territory).
	// This is a soft bonus proportional to enemy pressure, not a hard if.
	const int32_t old_score = field.military_score_;
	field.military_score_ = 0;
	if (field.near_border && field.unowned_land_nearby > 0) {
		field.military_score_ +=
		   static_cast<int16_t>(field.unowned_land_nearby);
	}
	if (field.enemy_nearby && field.enemy_owned_land_nearby > 0) {
		field.military_score_ +=
		   static_cast<int16_t>(field.enemy_owned_land_nearby) * 2;
	}
	// Defensive redundancy bonus: when enemies are close, having a
	// second military building nearby is valuable even if it doesn't
	// gain NEW land — it prevents total land loss if one building falls.
	// Bonus = enemy_presence × overlap_coverage / max_presence.
	// At high enemy presence with good overlap: significant bonus.
	// At zero enemy presence: zero bonus.
	// This replaces the old overlap_divisor penalty for the defensive case.
	if (field.enemy_military_presence > 0 && field.own_military_presence > 0) {
		// Each friendly soldier providing redundant coverage adds defense value.
		// Scale by enemy threat: more enemies = more value in redundancy.
		const int32_t defense_bonus =
		   std::min<int32_t>(field.own_military_presence, field.enemy_military_presence);
		field.military_score_ += defense_bonus;
	}
	// Overlap penalty for non-defensive case: when NO enemy is near,
	// overlapping military presence means wasted soldiers and materials.
	// Divide by (1 + own_presence) to discount redundant coverage.
	if (field.enemy_military_presence == 0 && !field.enemy_nearby &&
	    field.own_military_presence > 0) {
		// Normalize soldier count by smallest garrison to get approximate building count.
		// Without this, tribes with 3-soldier buildings get 3× more penalty per building
		// than tribes with 1-soldier buildings, causing permanent expansion stall.
		const int32_t normalized_presence =
		   static_cast<int32_t>(field.own_military_presence) / smallest_garrison_;
		field.military_score_ /=
		   (1 + normalized_presence +
		    static_cast<int16_t>(field.military_in_constr_nearby));
	}
	// Per-spot integral: decay faster when military_score drops.
	// If the score decreased (another building built nearby, land conquered),
	// halve the integral immediately so the "plan" adapts quickly.
	// If score is zero (inland, no border), reset integral entirely.
	if (field.military_score_ <= 0) {
		field.military_integral_ = 0;
	} else if (field.military_score_ < old_score) {
		field.military_integral_ = field.military_integral_ / 2;
	}

	// Mine-spot specific: count nearby mines and same-resource mine fields.
	if (field.is_mine_spot) {
		field.mines_nearby = 0;
		field.same_mine_fields_nearby = 0;
		field.same_type_mines_nearby = 0;
		Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr_mine(
		   map, Widelands::Area<Widelands::FCoords>(field.coords, 4));
		do {
			const Widelands::FCoords loc = mr_mine.location();
			if (loc == field.coords) {
				continue;
			}
			if ((player_->get_buildcaps(loc) & Widelands::BUILDCAPS_MINE) != 0 &&
			    loc.field->get_resources() == field.coords.field->get_resources()) {
				++field.same_mine_fields_nearby;
			}
			if (const Widelands::BaseImmovable* imm = loc.field->get_immovable()) {
				if (upcast(Widelands::Building const, bld, imm)) {
					if (bld->descr().get_ismine() &&
					    loc.field->get_resources() == field.coords.field->get_resources()) {
						++field.same_type_mines_nearby;
					}
				}
			}
		} while (mr_mine.advance(map));

		// Check if preferred (flag/road nearby for road connection)
		field.preferred = false;
		Widelands::FCoords br = map.br_n(field.coords);
		if (br.field->get_immovable() != nullptr &&
		    br.field->get_immovable()->descr().type() >= Widelands::MapObjectType::FLAG) {
			field.preferred = true;
		}
	}
}

// =====================================================================
// Building tracking
// =====================================================================

void PlannerAI::gain_immovable(Widelands::PlayerImmovable& pi) {
	if (upcast(Widelands::Building, building, &pi)) {
		gain_building(*building);
	} else if (upcast(Widelands::Flag const, flag, &pi)) {
		new_flags.push_back(flag);
	} else if (upcast(Widelands::Road const, road, &pi)) {
		roads.push_front(road);
	}
}

void PlannerAI::lose_immovable(const Widelands::PlayerImmovable& pi) {
	if (upcast(Widelands::Building const, building, &pi)) {
		lose_building(*building);
	} else if (upcast(Widelands::Flag const, flag, &pi)) {
		for (EconomyObserver* eco_obs : economies) {
			for (auto it = eco_obs->flags.begin(); it != eco_obs->flags.end(); ++it) {
				if (*it == flag) {
					eco_obs->flags.erase(it);
					return;
				}
			}
		}
		for (auto it = new_flags.begin(); it != new_flags.end(); ++it) {
			if (*it == flag) {
				new_flags.erase(it);
				return;
			}
		}
	} else if (upcast(Widelands::Road const, road, &pi)) {
		for (auto it = roads.begin(); it != roads.end(); ++it) {
			if (*it == road) {
				roads.erase(it);
				return;
			}
		}
	}
}

void PlannerAI::gain_building(Widelands::Building& b) {
	BuildingObserver& bo = get_building_observer(b.descr().name().c_str());
	const Time& gametime = game().get_gametime();

	if (bo.type == BuildingObserver::Type::kConstructionsite) {
		BuildingObserver& target_bo = get_building_observer(
		   dynamic_cast<const Widelands::ConstructionSite&>(b).building().name().c_str());
		++target_bo.cnt_under_construction;
		if (target_bo.type == BuildingObserver::Type::kProductionsite) {
			++numof_psites_in_constr;
		}
		if (target_bo.type == BuildingObserver::Type::kWarehouse) {
			++numof_warehouses_in_const_;
		}
	} else {
		++bo.cnt_built;
		bo.last_building_built = gametime;

		if (bo.type == BuildingObserver::Type::kProductionsite) {
			productionsites.emplace_back();
			productionsites.back().site = &dynamic_cast<Widelands::ProductionSite&>(b);
			productionsites.back().bo = &bo;
			productionsites.back().built_time = gametime;
			productionsites.back().unoccupied_till = gametime;
			++bo.unoccupied_count;

		} else if (bo.type == BuildingObserver::Type::kMine) {
			mines_.emplace_back();
			mines_.back().site = &dynamic_cast<Widelands::ProductionSite&>(b);
			mines_.back().bo = &bo;
			mines_.back().built_time = gametime;
			++bo.unoccupied_count;

		} else if (bo.type == BuildingObserver::Type::kMilitarysite) {
			militarysites.emplace_back();
			militarysites.back().site = &dynamic_cast<Widelands::MilitarySite&>(b);
			militarysites.back().bo = &bo;
			militarysites.back().understaffed = 0;
			militarysites.back().built_time = gametime;
			militarysites.back().last_change = Time(0);

		} else if (bo.type == BuildingObserver::Type::kTrainingsite) {
			trainingsites.emplace_back();
			trainingsites.back().site = &dynamic_cast<Widelands::TrainingSite&>(b);
			trainingsites.back().bo = &bo;

		} else if (bo.type == BuildingObserver::Type::kWarehouse) {
			++numof_warehouses_;
			warehousesites.emplace_back();
			warehousesites.back().site = &dynamic_cast<Widelands::Warehouse&>(b);
			warehousesites.back().bo = &bo;

			if (bo.is(BuildingAttribute::kPort)) {
				portsites.emplace_back();
				portsites.back().site = &dynamic_cast<Widelands::Warehouse&>(b);
				portsites.back().bo = &bo;
				portsites.back().ships_assigned = 0;
			}
		}

		if (bo.is(BuildingAttribute::kShipyard)) {
			shipyardsites.emplace_back();
			shipyardsites.back().site = &dynamic_cast<Widelands::ProductionSite&>(b);
			shipyardsites.back().bo = &bo;
			shipyardsites.back().built_time = gametime;
		}
	}
}

void PlannerAI::lose_building(const Widelands::Building& b) {
	BuildingObserver& bo = get_building_observer(b.descr().name().c_str());

	if (bo.type == BuildingObserver::Type::kConstructionsite) {
		BuildingObserver& target_bo = get_building_observer(
		   dynamic_cast<const Widelands::ConstructionSite&>(b).building().name().c_str());
		if (target_bo.cnt_under_construction > 0) {
			--target_bo.cnt_under_construction;
		}
		if (target_bo.type == BuildingObserver::Type::kProductionsite &&
		    numof_psites_in_constr > 0) {
			--numof_psites_in_constr;
		}
		if (target_bo.type == BuildingObserver::Type::kWarehouse &&
		    numof_warehouses_in_const_ > 0) {
			--numof_warehouses_in_const_;
		}
	} else {
		if (bo.cnt_built > 0) {
			--bo.cnt_built;
		}

		if (bo.type == BuildingObserver::Type::kProductionsite) {
			for (auto it = productionsites.begin(); it != productionsites.end(); ++it) {
				if (it->site == &dynamic_cast<const Widelands::ProductionSite&>(b)) {
					productionsites.erase(it);
					break;
				}
			}
		} else if (bo.type == BuildingObserver::Type::kMine) {
			for (auto it = mines_.begin(); it != mines_.end(); ++it) {
				if (it->site == &dynamic_cast<const Widelands::ProductionSite&>(b)) {
					mines_.erase(it);
					break;
				}
			}
		} else if (bo.type == BuildingObserver::Type::kMilitarysite) {
			for (auto it = militarysites.begin(); it != militarysites.end(); ++it) {
				if (it->site == &dynamic_cast<const Widelands::MilitarySite&>(b)) {
					militarysites.erase(it);
					break;
				}
			}
		} else if (bo.type == BuildingObserver::Type::kTrainingsite) {
			for (auto it = trainingsites.begin(); it != trainingsites.end(); ++it) {
				if (it->site == &dynamic_cast<const Widelands::TrainingSite&>(b)) {
					trainingsites.erase(it);
					break;
				}
			}
		} else if (bo.type == BuildingObserver::Type::kWarehouse) {
			if (numof_warehouses_ > 0) {
				--numof_warehouses_;
			}
			for (auto it = warehousesites.begin(); it != warehousesites.end(); ++it) {
				if (it->site == &dynamic_cast<const Widelands::Warehouse&>(b)) {
					warehousesites.erase(it);
					break;
				}
			}
			if (bo.is(BuildingAttribute::kPort)) {
				for (auto it = portsites.begin(); it != portsites.end(); ++it) {
					if (it->site == &b) {
						portsites.erase(it);
						break;
					}
				}
			}
		}

		if (bo.is(BuildingAttribute::kShipyard)) {
			for (auto it = shipyardsites.begin(); it != shipyardsites.end(); ++it) {
				if (it->site == &dynamic_cast<const Widelands::ProductionSite&>(b)) {
					shipyardsites.erase(it);
					break;
				}
			}
		}
	}
}

// =====================================================================
// Three-Circle PID Pressure System (Power-Iteration)
// =====================================================================
//
// === Priority Inheritance Contract (Root Objective) ===
//
// Every decision the AI makes is derived from the game's root objective
// through a chain of priority inheritance. EVERY constraint can be
// expressed as a priority inheritance on subtasks:
//
//   Game Goal (points, military victory, territorial control, ...)
//   ├─ To beat a player: destroy their warehouses
//   │  ├─ Can't attack directly → attack the owning military building
//   │  ├─ Don't see enemy yet → EXPAND TERRITORY (Circle 3)
//   │  ├─ See enemy but no free soldiers → RAMP UP PRODUCTION (Circle 1+2)
//   │  └─ Have soldiers but enemy is stronger → TRAIN soldiers (military split)
//   ├─ To score points: produce valuable wares
//   │  └─ Need specific ware → build the producing building (Circle 2)
//   │     ├─ Building needs input wares → those wares get pressure (Circle 1)
//   │     │  └─ Input wares need their own producers → chain propagates (PI)
//   │     ├─ Building needs workers → worker ware gets pressure (not yet implemented)
//   │     └─ Building needs territory → feeds expansion pressure (Circle 3)
//   ├─ Building material sparse in ~100 ticks → PREEMPTIVE PRODUCTION
//   │  └─ CM demand-pull from Circle 2 building pressure
//   └─ Resources deplete without renewal → PROACTIVE SUPPORTER COUPLING
//      └─ Woodcutter exists → ranger gets pressure BEFORE trees run out
//         (same: fisher→fishbreeder, hunter→gamekeeper)
//
// Decisions at each level:
//   - "Which task next?" → building, dismantling, or attacking
//   - "Which building?" → Circle 2 selects the highest-pressure type
//   - "Which ware?" → Circle 1 selects the most urgent ware
//   - "Where to expand?" → Circle 3 selects the target direction
//   - "Where to place?" → field scoring with expansion penalty at border
//
// All factors are self-normalizing: expressed as multiples of avg_wp
// (= kNormalizationBudget / nr_wares). This ensures factors from
// different subsystems compete on the same scale without magic constants.
// The relative comparison between factors determines the decision.
//
// Number calibration: each circle normalizes to kNormalizationBudget (10M).
// After normalization, the most-wanted component in each circle has a score
// close to kNormalizationBudget (10M) if it dominates, or close to
// avg_wp (= 10M / nr_wares ≈ 333k for 30 wares) if demand is spread evenly.
// This means:
//   Circle 1: the most-wanted ware has score ≈ 333k (evenly spread) to 10M
//             (single ware dominates). Typical hot ware: 500k-2M.
//   Circle 2: the most-wanted building has score ≈ 10M / nr_building_types.
//             Typical hot building: 500k-3M.
//   Circle 3: unowned land vs enemies share 10M. With 2 enemies at
//             equal pressure: each gets ≈ 3.3M.
// When comparing factors across circles (e.g. military pressure from
// Circle 3 feeding Circle 2), the conversion factor must account for
// these ranges. The `4 * avg_wp / kNormalizationBudget` factor for
// military buildings converts Circle 3 scores to Circle 2 units.
//
// Decision thresholds ("WHEN is action A more beneficial than action B?"):
//   Building pressure A > B → build A first. This is implicit via
//   normalization: the highest-pressure building wins construct_building().
//   Build vs. not build → when any building pressure > 0 after subtracting
//     construction overload and material scarcity penalties.
//   Dismantle vs. keep → dismantle_score accumulates positive (dismantle)
//     or negative (keep). Positive needs multiple avg_pressure-sized ticks.
//   Expand vs. stay → Circle 3 base_expansion = 30 ensures constant demand.
//   Attack vs. defend → sum of enemy expansion pressures drives attacks.
//
// Magic number policy:
//   Every numeric constant MUST be documented with:
//   (a) what it controls,
//   (b) why that specific value was chosen,
//   (c) how it was derived or calibrated.
//   Constants derived from avg_wp are self-normalizing and preferred.
//   Constants that are ratios (e.g. 99/100 decay) are scale-independent.
//   Remaining constants must cite calibration source (playtesting, math).
//
// === Dimensional Analysis Contract ===
//
// On each mathematical operation, the exact UNIT of every value must be
// known and documented. Additions are only valid between same-unit values.
// Multiplications yield a product unit. Divisions yield a quotient unit.
// Every intermediate result must have a traceable unit derivation.
//
// UNIT DICTIONARY (all units used in this system):
//
//   [count]     Dimensionless integer: number of buildings, wares, workers.
//               Example: consumption = 5, production_capacity = 3, stock = 12.
//
//   [budget]    Share of kNormalizationBudget (10,000,000).
//               Range: [0, 10M] after normalization.
//               Example: ware_pressure_[w].outputControl ∈ [0, 10M].
//               Example: building_pressure_[bi].outputControl ∈ [0, 10M].
//
//   [budget/ware] = avg_wp = kNormalizationBudget / nr_wares.
//               The "fair share" of one ware type in the total budget.
//               For 30 wares: 333,333. Used as the universal threshold.
//
//   [raw_pid]   Pre-normalization PID output (dimensionless, unbounded).
//               = P_weight × [count] + 1 × accumulated_[count] + N × Δ[count].
//               Range: typically [-1G, +1G] (fits int32_t up to ~50 ticks).
//               Danger: ipart accumulates [count] per tick with no decay in
//               the PID tick itself (decay happens outside via anti-windup).
//               After 100 ticks at error=10M: ipart ≈ 1G. P_weight=100 ×
//               error=10M = 1G. Sum ≈ 2G → FITS int32_t (max 2.1G) but
//               is near the limit.
//
//   [weight]    Softmax polynomial weight (integer, typically [1, 221]).
//               = 1 + x + x²/2 where x ∈ [0, 20].
//               Used only during normalization, then discarded.
//
//   [ratio]     Dimensionless fraction, often in [0, 1000] permille.
//               Example: cm_ratio_[bi] ∈ [0, 1000] = affordability ratio.
//               Example: space_scarcity_ratio ∈ [0, 1000].
//
//   [score]     Dismantle score: accumulated [budget/ware]-sized deltas.
//               Leaky integrator with decay (2N-1)/(2N) per visit.
//               Range: clamped to [-nr_wares × avg_wp, +nr_wares × avg_wp].
//               Positive → dismantle. Negative → keep.
//
// UNIT FLOW THROUGH THE SYSTEM:
//
//   Circle 1 (Ware Pressure):
//     error signal:    consumption[count] - stock[count] - capacity[count]
//                      + cm_anticipated[count] + tool_demand[count]
//                      → wp.error [count]
//     PID tick:        outputControl = P×error + I×ipart + D×Δerror [raw_pid]
//     raw_A[w]:        max(0, outputControl) [raw_pid]
//     raw_B[w]:        demand-pull from building pressure [raw_pid]
//                      (building_pressure_.outputControl[raw_pid] / n_inputs)
//     Linear norm:     raw_A → [0, 10M budget], raw_B → [0, 10M budget]
//     Combined:        raw_total = norm_A + norm_B [0, 20M budget]
//     Softmax norm:    → ware_pressure_[w].outputControl [budget]
//
//   Circle 2 (Building Pressure):
//     error signal:    max(ware_pressure_[output].outputControl) [budget]
//                      + supporter_demand [budget]
//                      + depletion_pressure [budget]
//                      + structural_floor [budget]
//                      × cm_ratio [ratio/1000]
//                      → bp.error [budget]
//     PID tick:        → outputControl [raw_pid]
//     raw_total[bi]:   outputControl [raw_pid]
//     Linear norm:     → building_pressure_[bi].outputControl [budget]
//
//   Circle 3 (Expansion Pressure):
//     Same pattern: [count] → PID → [raw_pid] → norm → [budget]
//
//   Dismantle score:
//     avg_pressure:    weights_.avg_wp [budget/ware]
//     delta:           accumulated factors, each in [budget/ware] units
//     decay:           score × (2N-1)/(2N) [score]
//     accumulate:      score += delta [score]
//     threshold:       score > 0 → dismantle
//
// OVERFLOW ANALYSIS (why softmax normalization is needed):
//
//   Without normalization: PID ipart grows unboundedly.
//   After 200 ticks at error=5 (modest deficit): ipart = 1000.
//   P_weight = 20 (economy of 100 buildings).
//   outputControl = 20×5 + 1000 + 10×2 = 1120. Still tiny.
//
//   With ware_pressure feeding into building_pressure:
//   bp.error = ware_pressure_.outputControl ∈ [0, 10M].
//   After 50 ticks at bp.error = 5M: bp.ipart = 250M.
//   P = 20, D = 10: outputControl = 20×5M + 250M + 10×1M = 360M.
//   Still fits int32 (max 2.1G), but sum over ~80 buildings for
//   normalization: sum ≈ 80 × 200M = 16G → needs int64 for sum.
//   (Code uses int64_t S at line 2893. ✓)
//
//   The softmax step in Circle 1 SHARPENS the budget allocation:
//   a 2:1 raw ratio becomes ~3.6:1 after softmax (at scale=20).
//   This is needed to prevent "average buildup" where 30 wares
//   each get ~333K and the AI builds everything equally slowly.
//   The softmax focuses budget on the most scarce ware first.
//   Circle 2 uses LINEAR normalization only (no softmax) because
//   building types are fewer and demand is naturally more peaked.
//
//   Verdict: the softmax normalization IS needed for Circle 1.
//   Without it, the AI fails to prioritize and spreads too thin.
//
// Priority algebra — modeling AND and OR conditions:
//   ADDITION models OR: if a building produces BOTH axes AND nets, the
//   building's demand is the SUM of both output demands. Even if nets
//   are not needed (score 0), axes being needed still gives full score.
//
//   MULTIPLICATION models AND: if we need gold AND our gold mine runs
//   at 0% productivity, multiply demand × (productivity/100) = 0.
//   The building is needed AND it must be productive for the score to
//   be nonzero. Both conditions must hold.
//
//   INTERPOLATION via (1 - ratio): if gold mine works at 40%, the
//   factor (1 - 0.40) = 0.60 quantifies how much improvement is still
//   needed. This feeds into sub-scores for improving productivity.
//
//   PROPORTIONAL SPLITS: a sub-score distributes to its causes in
//   proportion to their contribution. E.g. if mine productivity is low
//   because 1/3 of mines lack resources and 2/3 lack bread: allocate
//   1/3 of improvement score to dismantle/rebuild, 2/3 to bread supply.
//   If bread has 4/5 queued and wine has 0/5: split the 2/3 supply
//   score as 1/6 bread, 5/6 wine (proportional to missing items).
//

// Positive example: expansion balancing & cost ratios
//   The expansion pressure system (Circle 3) demonstrates the correct
//   approach: figure out the EXACT cost ratio between competing options
//   rather than using heuristic thresholds. When deciding "train vs.
//   recruit", compute the marginal value of training (Δsurvival ×
//   land_protected × land_value) against the opportunity cost (input
//   ware pressures × consumption). The PI system normalizes everything
//   to the same 10M budget, so these ratios are directly comparable.
//   This is the pattern to follow for ALL AI decisions.
//
// Circle 1: Ware Pressure
// Each ware gets an error signal from capacity deficits, plus a
// demand-pull from Circle 2's building pressures (one backward
// step per tick). The PI integral accumulates across ticks for
// deep-chain convergence. CM demand is driven at the building
// level, not injected independently — no double-counting.
//
// PI parameterization (derived from game mechanics):
//   N = sqrt(economy_size): estimated ticks for items to travel from
//       warehouse to the furthest production site. With economy_size
//       buildings arranged in a grid, the average path length is ~sqrt(E)
//       road segments. Each segment takes ~2 seconds for a carrier.
//       One PI tick = 4 seconds (construct_building interval).
//       So delivery takes sqrt(E)/2 ticks → round up to sqrt(E).
//
//   decay = 1 - 1/(2N): integral leaks at rate 1/(2N) per tick.
//       After 2N ticks of constant error, integral reaches ~63% of
//       steady state. This means: if the problem hasn't self-solved
//       in 2N ticks (= 2× delivery time), the integral term has
//       accumulated enough to equal the proportional term, triggering
//       stronger action.
//
//   P_weight = 2N: proportional weight equals 2× self-solution time.
//       At steady state, P × error = I_ss, ensuring a balanced PI
//       controller where both terms contribute equally when the
//       problem persists for the expected self-solution period.
//
//   "Underway" vs "stored": the stock level used for deficit
//       computation only counts items in warehouses. Items currently
//       being carried on roads are NOT counted. This naturally creates
//       a transient deficit when items are underway, causing the PI
//       system to slightly overshoot production — which is correct
//       behavior (keep production ahead of consumption).
//
// =====================================================================
// Utilities
// =====================================================================

BuildingObserver& PlannerAI::get_building_observer(Widelands::DescriptionIndex idx) {
	for (BuildingObserver& bo : buildings_) {
		if (bo.id == idx) {
			return bo;
		}
	}
	throw wexception("PlannerAI: no BuildingObserver for index %d", idx);
}

BuildingObserver& PlannerAI::get_building_observer(char const* name) {
	for (BuildingObserver& bo : buildings_) {
		if (strcmp(bo.name, name) == 0) {
			return bo;
		}
	}
	throw wexception("PlannerAI: no BuildingObserver for '%s'", name);
}

}  // namespace AI
