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
	while (!mineable_fields.empty()) {
		delete mineable_fields.back();
		mineable_fields.pop_back();
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
			update_all_mineable_fields(gametime);
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
			buildable_fields.push_back(new BuildableField(unusable_fields.front()));
			unusable_fields.pop_front();
			if (20 > checked_fields++) {
				update_buildable_field(*buildable_fields.back());
				buildable_fields.back()->field_info_expiration = gametime + kFieldInfoExpiration;
			}
			continue;
		}
		if ((player_->get_buildcaps(unusable_fields.front()) & Widelands::BUILDCAPS_MINE) != 0) {
			mineable_fields.push_back(new MineableField(unusable_fields.front()));
			unusable_fields.pop_front();
			if (20 > checked_fields++) {
				update_mineable_field(*mineable_fields.back());
				mineable_fields.back()->field_info_expiration = gametime + kMineFieldInfoExpiration;
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
		BuildableField* bf = *it;
		const uint16_t build_caps =
		   player_->get_buildcaps(bf->coords) & Widelands::BUILDCAPS_SIZEMASK;

		if (build_caps == 0u || bf->coords.field->get_owned_by() != player_number()) {
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

void PlannerAI::update_all_mineable_fields(const Time& gametime) {
	if (mineable_fields.empty()) {
		return;
	}
	uint16_t updated = 0;

	for (auto it = mineable_fields.begin(); it != mineable_fields.end();) {
		MineableField* mf = *it;

		if ((player_->get_buildcaps(mf->coords) & Widelands::BUILDCAPS_MINE) == 0 ||
		    mf->coords.field->get_owned_by() != player_number()) {
			delete mf;
			it = mineable_fields.erase(it);
			continue;
		}

		if (mf->field_info_expiration <= gametime && updated < 20) {
			update_mineable_field(*mf);
			mf->field_info_expiration = gametime + kMineFieldInfoExpiration;
			++updated;
		}
		++it;
	}
}

void PlannerAI::update_buildable_field(BuildableField& field) {
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
		field.military_score_ /=
		   (1 + static_cast<int16_t>(field.own_military_presence) +
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
}

void PlannerAI::update_mineable_field(MineableField& field) {
	const Widelands::Map& map = game().map();

	field.mines_nearby = 0;
	field.same_mine_fields_nearby = 0;

	// Check for nearby mines and same-type fields
	Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
	   map, Widelands::Area<Widelands::FCoords>(field.coords, 4));
	do {
		if (mr.location() == field.coords) {
			continue;
		}
		if ((player_->get_buildcaps(mr.location()) & Widelands::BUILDCAPS_MINE) != 0 &&
		    mr.location().field->get_resources() == field.coords.field->get_resources()) {
			++field.same_mine_fields_nearby;
		}
	} while (mr.advance(map));

	// Check if preferred (flag/road nearby)
	field.preferred = false;
	Widelands::FCoords br = map.br_n(field.coords);
	if (br.field->get_immovable() != nullptr &&
	    br.field->get_immovable()->descr().type() >= Widelands::MapObjectType::FLAG) {
		field.preferred = true;
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
void PlannerAI::update_ware_pressures(const Time& gametime) {
	++pi_tick_count_;
	const size_t nr_wares = wares.size();
	std::vector<int32_t> raw_A(nr_wares, 0);  // Factor A: capacity PID
	std::vector<int32_t> raw_B(nr_wares, 0);  // Factor B: demand signals
	std::vector<int32_t> raw_total(nr_wares, 0);

	// PID parameters from shared weights (recomputed in update_building_pressures)
	const int32_t N_ticks = weights_.N_ticks;
	const int32_t P_weight = weights_.P_weight;

	// === Priority Conservation: consumer type count per ware ===
	//
	// Law of Priority Conservation: pressure is SPLIT, never duplicated.
	// When ware w is consumed as input by N building types, each building
	// type's backwards propagation is divided by N. This prevents leaf
	// wares (water, wood) that serve many consumers from inflating their
	// pressure relative to specialized wares (wool, cloth) that serve
	// only one chain.
	//
	// Without this: water (5 consumers) gets 5× more backwards pressure
	// than wool (1 consumer). After normalization, water dominates and
	// the AI builds wells/farms instead of completing production chains
	// toward the actual goal (soldiers, military buildings).
	//
	for (size_t w = 0; w < nr_wares; ++w) {
		PIDController& wp = ware_pressure_[w];
		const Widelands::DescriptionIndex wi = static_cast<Widelands::DescriptionIndex>(w);

		const uint32_t stock = calculate_stocklevel(wi);

		// CAPACITY-BASED error signal.
		// Unit: [count] (dimensionless building/ware count).
		// Formula: consumption[count] - stock[count] - production_capacity[count]
		// Positive = need more production capacity, negative = oversupplied.
		//
		// production_capacity[count] = number of built+constructing buildings
		//   producing this ware. Plans capacity, not actual output.
		// consumption[count] = number of built+constructing buildings
		//   consuming this ware as input.
		// stock[count] = number of ware units in warehouses.
		//
		// After normalization, only RELATIVE magnitudes between wares
		// matter: the ware with the worst capacity balance gets the most
		// pressure, regardless of absolute stock levels.
		int32_t production_capacity = 0;  // [count]
		int32_t consumption = 0;          // [count]
		for (const BuildingObserver& bo : buildings_) {
			if (bo.type != BuildingObserver::Type::kProductionsite &&
			    bo.type != BuildingObserver::Type::kMine) {
				continue;
			}
			for (const auto& output : bo.ware_outputs) {
				if (static_cast<size_t>(output) == w) {
					// Count built + under-construction as committed capacity.
					// A building under construction WILL produce soon —
					// ignoring it causes the AI to over-build producers.
					production_capacity += static_cast<int32_t>(bo.cnt_built) +
					   static_cast<int32_t>(bo.cnt_under_construction);
					break;
				}
			}
			for (const auto& input : bo.inputs) {
				if (static_cast<size_t>(input) == w) {
					consumption += static_cast<int32_t>(bo.cnt_built) +
					              static_cast<int32_t>(bo.cnt_under_construction);
					break;
				}
			}
		}
		// Unit: [count] = [count] - [count] - [count]
		wp.error = consumption - static_cast<int32_t>(stock) - production_capacity;

		// CM anticipation: for each CM ware, inject pressure proportional
		// to how much of it will be consumed by buildings under pressure.
		// Only fires when stock < threshold (= sum of all pending buildcosts).
		// Unit: cm_anticipated[count] = buildcost[count] × bp_out[budget] / budget[budget]
		//       = [count × budget / budget] = [count] ✓ (same unit as wp.error)
		{
			int32_t cm_anticipated = 0;  // [count]
			int32_t cm_pending_total = 0;
			for (size_t bi = 0; bi < buildings_.size(); ++bi) {
				const BuildingObserver& cm_bo = buildings_[bi];
				if (bi >= building_pressure_.size()) {
					continue;
				}
				const int32_t bp_out = building_pressure_[bi].outputControl;
				if (bp_out <= 0) {
					continue;
				}
				const auto& cost = cm_bo.desc->buildcost();
				auto it = cost.find(wi);
				if (it == cost.end()) {
					continue;
				}
				cm_pending_total += it->second;
				cm_anticipated += static_cast<int32_t>(
				   static_cast<int64_t>(it->second) * bp_out / kNormalizationBudget);
			}
			if (cm_pending_total > 0 &&
			    stock < static_cast<uint32_t>(cm_pending_total)) {
				wp.error += cm_anticipated;
			}
			// Bootstrap floor: if this ware is a CM with pending demand
			// but NO producer exists, inject structural demand.
			if (production_capacity == 0 && cm_pending_total > 0) {
				const int32_t avg_wp_cm = kNormalizationBudget /
				   std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
				wp.error = std::max(wp.error,
				   avg_wp_cm * cm_pending_total /
				      std::max<int32_t>(1, static_cast<int32_t>(nr_wares)));
			}
		}

		// Goal-based error injection (replaces basic economy boost).
		//
		// Instead of a hard-coded basic economy multiplier, we inject a
		// single top-level goal: "I want desired_lead_ soldier-strength
		// per hour more than the best enemy." This error propagates
		// through the PI chain naturally:
		//   goal error → military wares → production chain → CMs
		//
		// The error is computed once (outside the per-ware loop) and
		// injected into military output wares. The 3-part distribution
		// and chain propagation handle the rest — CM production gets
		// boosted by the BM loop's construction material injection.
		//
		// (Goal error computed below, after per-ware loop, as it needs
		// to identify which wares are military outputs.)

		// Worker tool demand: buildings need workers, workers need tools.
		// This closes the chain: building pressure → worker → tool → material.
		//
		// For each building type with cnt_under_construction > 0 or
		// unoccupied positions, look up its workers' tool costs and
		// inject demand on those tool wares. This ensures that when
		// the AI places a gold mine (needs miner), the miner's pick
		// gets ware pressure, which propagates to iron bar, then to
		// iron ore through the production chain propagation.
		//
		// Scale: tool demand = building_pressure × worker_tool_cost.
		// This competes naturally with other ware demands.
		{
			for (const BuildingObserver& bo : buildings_) {
				if (bo.type != BuildingObserver::Type::kProductionsite &&
				    bo.type != BuildingObserver::Type::kMine) {
					continue;
				}
				// Only inject tool demand for buildings that need more workers
				const int32_t worker_need =
				   static_cast<int32_t>(bo.cnt_under_construction) +
				   static_cast<int32_t>(bo.unoccupied_count);
				if (worker_need <= 0) {
					continue;
				}
				// Get building pressure for this building type
				int32_t bp = 0;
				for (size_t bi = 0; bi < buildings_.size(); ++bi) {
					if (&buildings_[bi] == &bo && bi < building_pressure_.size()) {
						bp = std::max<int32_t>(0, building_pressure_[bi].outputControl);
						break;
					}
				}
				if (bp <= 0) {
					continue;
				}
				// Look up workers and their tool costs
				const auto* prod_descr =
				   dynamic_cast<const Widelands::ProductionSiteDescr*>(bo.desc);
				if (prod_descr == nullptr) {
					continue;
				}
				for (const auto& [worker_idx, worker_count] :
				     prod_descr->working_positions()) {
					const Widelands::WorkerDescr* wd =
					   tribe_->get_worker_descr(worker_idx);
					if (wd == nullptr || !wd->is_buildable()) {
						continue;
					}
					// Worker's buildcost = tools needed to create this worker
					for (const auto& [tool_name, tool_amount] : wd->buildcost()) {
						const Widelands::DescriptionIndex tool_idx =
						   tribe_->ware_index(tool_name);
						if (tool_idx != Widelands::INVALID_INDEX &&
						    static_cast<size_t>(tool_idx) == w) {
							// Scale: building pressure × tool amount × worker need
							raw_B[w] += bp * static_cast<int32_t>(tool_amount) *
							   worker_need /
							   std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
						}
					}
				}
			}
		}

		// Training demand from military split (Phase 5).
		// training_pressure_ is in [0, kNormalizationBudget] range.
		// Convert to raw error scale by dividing by nr_wares (= avg_wp
		// in absolute terms). This makes training demand compete with
		// production ware deficits on the same scale.
		// At full training pressure (10M) and 30 wares: +333k per input
		// ware. After PI amplification (×100) and normalization, this
		// becomes a significant share of the budget.
		if (training_pressure_ > 0) {
			const int32_t training_inject = training_pressure_ /
			   std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
			for (const BuildingObserver& bo : buildings_) {
				if (bo.type != BuildingObserver::Type::kTrainingsite) {
					continue;
				}
				for (const auto& input : bo.inputs) {
					if (static_cast<size_t>(input) == w) {
						raw_B[w] += training_inject;
					}
				}
			}
		}

		// Recruiting demand from military split (Phase 5).
		// Same derivation as training demand.
		if (recruiting_pressure_ > 0) {
			const int32_t recruiting_inject = recruiting_pressure_ /
			   std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
			for (const BuildingObserver& bo : buildings_) {
				if (!bo.is(BuildingAttribute::kBarracks)) {
					continue;
				}
				for (const auto& input : bo.inputs) {
					if (static_cast<size_t>(input) == w) {
						raw_B[w] += recruiting_inject;
					}
				}
			}
		}

		// STEP 3: PID tick (parameters derived from economy size)
		// Unit flow:
		//   wp.error [count] (accumulated above)
		//   wp.ipart [count] (accumulated across ticks, anti-windup decay)
		//   outputControl = P_weight × error + 1 × ipart + N_ticks × (error - lastError)
		//                 = [count × 1] + [count] + [count × 1] = [raw_pid]
		//   raw_A[w] = max(0, outputControl) [raw_pid]
		//
		// Anti-windup: when capacity error ≤ 0 (oversupplied),
		// decay the integral faster (×7/8) to prevent windup.
		if (wp.error <= 0) {
			wp.ipart = wp.ipart * 7 / 8;
		}
		wp.tick(P_weight, 1, N_ticks);
		raw_A[w] = std::max<int32_t>(0, wp.outputControl);  // [raw_pid]
	}

	// === Military readiness: proactive CM demand (tick 11+) ===
	//
	// Military buildings are placed independently by the per-field integral
	// system. The economy must ALWAYS be prepared to supply construction
	// materials. Inject CM demand for ALL buildable military building types,
	// scaled by expansion pressure and weighted by efficiency (conquer
	// area / material cost). This ensures the economy pre-stocks materials
	// for whatever military building the placement system chooses.
	//
	// Skipped during warmup (ticks 1-10): bootstrap seed provides CM demand.
	if (pi_tick_count_ > 10) {
		const int32_t total_exp = expansion_targets_.empty() ? 0 :
		   std::max<int32_t>(0, expansion_targets_[0].outputControl);
		const int32_t mil_readiness =
		   total_exp / std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
		if (mil_readiness > 0) {
			for (const BuildingObserver& bo : buildings_) {
				if (bo.type != BuildingObserver::Type::kMilitarysite ||
				    !bo.buildable(*player_)) {
					continue;
				}
				// Weight by conquer area / material cost (efficiency).
				// Cheap buildings (sentry) get proportionally more CM demand
				// per ware unit than expensive ones (fortress).
				int32_t total_cost = 0;
				for (const auto& [ware_idx, amount] : bo.desc->buildcost()) {
					total_cost += amount;
				}
				const int32_t conquer = static_cast<int32_t>(bo.desc->get_conquers());
				const int32_t efficiency = conquer * conquer /
				   std::max<int32_t>(1, total_cost);
				for (const auto& [ware_idx, amount] : bo.desc->buildcost()) {
					if (static_cast<size_t>(ware_idx) < nr_wares) {
						raw_B[ware_idx] += mil_readiness *
						   static_cast<int32_t>(amount) * efficiency /
						   std::max<int32_t>(1, efficiency + 1);
					}
				}
			}
		}
	}

	// === Demand-Pull Model (replaces 3-Part Distribution) ===
	//
	// Core principle: demand attribution, not demand injection.
	//
	// The old 3-Part Distribution had 3 independent injection loops
	// (BM, Production, Military) plus 3-pass chain propagation, which
	// caused double-counting and leaf-node pressure inflation.
	//
	// The new model uses a SINGLE demand-pull pass from Circle 2:
	// for each building with positive building pressure (from the
	// PREVIOUS tick), propagate demand backwards to its input wares
	// and construction material wares. Deep chains converge via PI
	// integration across ticks (one level per tick), not by iterating
	// chain propagation within a single tick.
	//
	// CM demand is handled at the BUILDING level: a building under
	// construction injects CM demand proportional to its building
	// pressure. An unbuilt building with high pressure pre-stocks
	// materials. This prevents double-counting where CM wares like
	// planks got demand from both the BM loop and the production
	// loop independently.
	//
	// Conservation: demand is SPLIT across inputs (/ n_inputs),
	// not duplicated. A building consuming water+grain gives each
	// half the demand, not full demand to both.
	{
		const size_t n_bldgs = buildings_.size();

		// Precompute: does any built/constructing building produce each ware?
		std::vector<bool> ware_has_producer(nr_wares, false);
		for (const BuildingObserver& bo : buildings_) {
			if (bo.type != BuildingObserver::Type::kProductionsite &&
			    bo.type != BuildingObserver::Type::kMine) {
				continue;
			}
			if (bo.cnt_built + bo.cnt_under_construction == 0) {
				continue;
			}
			for (const auto& output : bo.ware_outputs) {
				if (static_cast<size_t>(output) < nr_wares) {
					ware_has_producer[output] = true;
				}
			}
		}

		// Count producers per ware (for CM injection dampening).
		// Used to dampen CM pre-stocking: wares with many producers
		// need less demand injection (already well-supplied).
		std::vector<int32_t> ware_producer_count(nr_wares, 0);
		for (const BuildingObserver& bo : buildings_) {
			if (bo.type != BuildingObserver::Type::kProductionsite &&
			    bo.type != BuildingObserver::Type::kMine) {
				continue;
			}
			const int32_t count = static_cast<int32_t>(
			   bo.cnt_built + bo.cnt_under_construction);
			if (count == 0) {
				continue;
			}
			for (const auto& output : bo.ware_outputs) {
				if (static_cast<size_t>(output) < nr_wares) {
					ware_producer_count[output] += count;
				}
			}
		}

		// Compute cm_ratio_ per building type (CM affordability + input chain readiness).
		// Used by Circle 2's CM-missing penalty and demand-pull dampening.
		cm_ratio_.resize(n_bldgs);
		for (size_t bi = 0; bi < n_bldgs; ++bi) {
			const BuildingObserver& bo = buildings_[bi];
			if (bo.cnt_built > 0 || bo.cnt_under_construction > 0) {
				cm_ratio_[bi] = 1000;
				continue;
			}
			int32_t cm_stock_ratio = 1000;
			const Widelands::Buildcost& cost = bo.desc->buildcost();
			if (!cost.empty()) {
				for (const auto& [ware_idx, amount] : cost) {
					const int64_t stock = static_cast<int64_t>(
					   calculate_total_stocklevel(ware_idx));
					const int32_t ratio = static_cast<int32_t>(
					   std::min<int64_t>(1000, stock * 1000 /
					      std::max<int64_t>(1, amount)));
					cm_stock_ratio = std::min(cm_stock_ratio, ratio);
				}
			}
			int32_t input_chain_ratio = 1000;
			if (!bo.inputs.empty() &&
			    (bo.type == BuildingObserver::Type::kProductionsite ||
			     bo.type == BuildingObserver::Type::kMine)) {
				int32_t producers_found = 0;
				for (const auto& input : bo.inputs) {
					if (static_cast<size_t>(input) < nr_wares &&
					    ware_has_producer[input]) {
						++producers_found;
					}
				}
				input_chain_ratio = producers_found * 1000 /
				   static_cast<int32_t>(bo.inputs.size());
			}
			cm_ratio_[bi] = std::min(cm_stock_ratio, input_chain_ratio);
		}

		// Compute building_supply_score_ (input chain readiness per building).
		// Used by construct_building() to gate buildings by supply chain.
		building_supply_score_.assign(n_bldgs, 0);
		for (size_t bi = 0; bi < n_bldgs; ++bi) {
			const BuildingObserver& bo = buildings_[bi];
			if (bo.type != BuildingObserver::Type::kProductionsite &&
			    bo.type != BuildingObserver::Type::kMine) {
				continue;
			}
			if (bo.inputs.empty() || bo.cnt_built > 0) {
				building_supply_score_[bi] = 1000;
				continue;
			}
			// Unbuilt: fraction of inputs that have built producers
			int32_t satisfied = 0;
			for (const auto& input : bo.inputs) {
				if (static_cast<size_t>(input) < nr_wares &&
				    ware_has_producer[input]) {
					++satisfied;
				}
			}
			building_supply_score_[bi] = satisfied * 1000 /
			   static_cast<int32_t>(bo.inputs.size());
		}

		// === Demand-pull: propagate building pressure to input wares ===
		//
		// Single pass using PREVIOUS tick's building_pressure_[].
		// Each building's demand is split evenly across its inputs.
		// Deep chains converge via PI integration: one level per tick,
		// with the integral accumulating across ticks for full depth.
		for (size_t bi = 0; bi < n_bldgs && bi < building_pressure_.size(); ++bi) {
			const int32_t bp = building_pressure_[bi].outputControl;
			if (bp <= 0) {
				continue;
			}
			const BuildingObserver& bo = buildings_[bi];

			// Production/mine: propagate to production inputs
			if ((bo.type == BuildingObserver::Type::kProductionsite ||
			     bo.type == BuildingObserver::Type::kMine) &&
			    !bo.inputs.empty()) {
				const int32_t n_inputs =
				   static_cast<int32_t>(bo.inputs.size());
				int32_t per_input = bp / n_inputs;
				// Dampen for unbuilt buildings by CM affordability.
				// If this building can't be built (CM missing), inject
				// less demand for its inputs. Floor at 10%.
				if (bo.cnt_built == 0 && bo.cnt_under_construction == 0 &&
				    bi < cm_ratio_.size()) {
					per_input = per_input *
					   std::max<int32_t>(100, cm_ratio_[bi]) / 1000;
				}
				for (const auto& input : bo.inputs) {
					if (static_cast<size_t>(input) < nr_wares) {
						raw_B[input] += per_input;
					}
				}
			}

			// Training site: propagate to training inputs
			if (bo.type == BuildingObserver::Type::kTrainingsite &&
			    !bo.inputs.empty()) {
				const int32_t n_inputs =
				   static_cast<int32_t>(bo.inputs.size());
				for (const auto& input : bo.inputs) {
					if (static_cast<size_t>(input) < nr_wares) {
						raw_B[input] += bp / n_inputs;
					}
				}
			}

			// CM demand for buildings under construction (urgent).
			// These need materials NOW. Inject proportional to building
			// pressure, scaled by how much CM is still missing.
			if (bo.cnt_under_construction > 0) {
				const Widelands::Buildcost& cost = bo.desc->buildcost();
				if (!cost.empty()) {
					int64_t total_cost_units = 0;
					for (const auto& [ware_idx, amount] : cost) {
						total_cost_units += amount;
					}
					if (total_cost_units > 0) {
						for (const auto& [ware_idx, amount] : cost) {
							if (static_cast<size_t>(ware_idx) >= nr_wares) {
								continue;
							}
							const int64_t stock = static_cast<int64_t>(
							   calculate_total_stocklevel(ware_idx));
							const int64_t needed = static_cast<int64_t>(amount) *
							   static_cast<int64_t>(bo.cnt_under_construction);
							if (stock < needed * 3) {
								raw_B[ware_idx] += static_cast<int32_t>(
								   static_cast<int64_t>(bp) *
								   amount / total_cost_units);
							}
						}
					}
				}
			}

			// CM pre-stocking for unbuilt buildings with demand.
			// If Circle 2 wants this building, start stockpiling its
			// construction materials so they're ready when placement
			// triggers construction.
			if (bo.cnt_built == 0 && bo.cnt_under_construction == 0) {
				const Widelands::Buildcost& cost = bo.desc->buildcost();
				if (!cost.empty()) {
					int64_t total_cost_units = 0;
					for (const auto& [ware_idx, amount] : cost) {
						total_cost_units += amount;
					}
					if (total_cost_units > 0) {
						for (const auto& [ware_idx, amount] : cost) {
							if (static_cast<size_t>(ware_idx) < nr_wares) {
								// Dampen by producer count: wares with many
								// producers need less pre-stocking demand.
								// 0 producers: full injection (bootstrapping).
								// 4 producers: 1/5 (already well-supplied).
								// This prevents CM wares like granite (needed by
								// every building) from accumulating 20× more
								// demand than production inputs, which causes
								// their producers to monopolize construction.
								raw_B[ware_idx] += static_cast<int32_t>(
								   static_cast<int64_t>(bp) *
								   amount / total_cost_units /
								   (1 + ware_producer_count[ware_idx]));
							}
						}
					}
				}
			}
		}

		// === Structural CM demand ===
		//
		// Two phases:
		// 1. Bootstrap seed (ticks 1-10): strong injection to kickstart
		//    the economy toward buildability (clay, branches, planks).
		// 2. Permanent floor (all ticks): minimum demand floor for CM
		//    wares that have NO producer. This prevents deep production
		//    chains from being permanently neglected after bootstrap.
		//
		// A cloth-based economy illustrates why the floor is needed:
		// cloth is CM for military buildings but has err=0 (no consumers
		// yet) and only 68K from demand-pull (vs. 4M for water). After
		// softmax normalization, cloth gets negligible budget → weaver
		// never built → shepherd never built → cloth chain dead forever.
		//
		// The floor ensures unproduced CM wares always have a minimum
		// signal proportional to how many building types need them.
		// This bootstraps neglected chains (cloth → wool → shepherd)
		// through PI integration across ticks, without dominating
		// established production (the floor is below avg_wp).
		{
			std::vector<int32_t> cm_structural(nr_wares, 0);
			for (const BuildingObserver& bo : buildings_) {
				if (!bo.buildable(*player_)) {
					continue;
				}
				for (const auto& [ware_idx, amount] : bo.desc->buildcost()) {
					if (static_cast<size_t>(ware_idx) < nr_wares) {
						cm_structural[ware_idx] += amount;
					}
				}
			}
			int32_t max_structural = 0;
			for (size_t w = 0; w < nr_wares; ++w) {
				max_structural = std::max(max_structural, cm_structural[w]);
			}

			if (max_structural > 0) {
				const int32_t seed_unit = kNormalizationBudget /
				   std::max<int32_t>(1, static_cast<int32_t>(nr_wares));

				if (pi_tick_count_ <= 10) {
					// Phase 1: Bootstrap — strong seed for all CM wares.
					for (size_t w = 0; w < nr_wares; ++w) {
						if (cm_structural[w] > 0) {
							raw_B[w] += cm_structural[w] * seed_unit / max_structural;
						}
					}
				}

				// Phase 2: Permanent floor for unproduced CM wares.
				// Only inject for wares that have NO built/constructing
				// producer — once a producer exists, the capacity formula
				// and demand-pull handle demand naturally.
				//
				// Floor = cm_structural[w] / max_structural × avg_wp.
				// For cloth (needed by ~5 building types, max=granite ~30):
				//   floor ≈ 5/30 × 322K ≈ 54K. Modest but persistent.
				// Through PI integration over 10+ ticks, this accumulates
				// to ~500K total pressure → weaver gets building pressure
				// → wool gets demand → shepherd gets building pressure.
				// The chain bootstraps itself one level per tick.
				const int32_t avg_wp_floor = kNormalizationBudget /
				   std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
				for (size_t w = 0; w < nr_wares; ++w) {
					if (cm_structural[w] > 0 && !ware_has_producer[w]) {
						const auto wi =
						   static_cast<Widelands::DescriptionIndex>(w);
						const uint32_t stock = calculate_stocklevel(wi);
						// CM with no producer: scale injection by
						// scarcity. cm_structural[w] = total units
						// needed across all building types using this
						// CM. At stock=0: full avg_pressure. At
						// stock >= need: revert to normal floor.
						//
						// This handles both deadlock (cloth stock=0,
						// can't build shepherd) and depletion (marble
						// stock=3, quarry depleted, rocks still exist
						// but no new quarry being built).
						const int32_t need = cm_structural[w];
						const int32_t shortfall = std::max<int32_t>(
						   0, need - static_cast<int32_t>(stock));
						if (shortfall > 0) {
							const int32_t boost =
							   avg_wp_floor * shortfall / need;
							raw_A[w] = std::max(raw_A[w], boost);
							raw_B[w] += boost;
						} else {
							// Enough stock: normal floor only.
							const int32_t floor =
							   cm_structural[w] * avg_wp_floor /
							   max_structural;
							if (raw_B[w] < floor) {
								raw_B[w] = floor;
							}
						}
					}
				}
			}
		}
	}

	// STEP 4b: Demand compression — cap per-ware Factor B.
	//
	// Demand-pull backwards propagation creates self-reinforcing feedback
	// loops: brick kiln needs clay → high building pressure → clay gets
	// massive demand → quarry building pressure → more quarries → more
	// clay → more bricks → more buildings → more brick kiln demand → ...
	//
	// Without compression, a single ware (clay) can monopolize 50-60% of
	// the entire Factor B budget, starving all other production chains
	// (cloth, wool, sheep) and causing permanent deadlocks.
	//
	// Cap: no single ware takes more than 4× its fair share (4/nr_wares)
	// of the total Factor B budget. For 32 wares: cap = 4 × 312K = 1.25M.
	// Clay goes from 7.6M → 1.25M. Cloth stays at 360K. Ratio drops from
	// 21:1 to 3.5:1, giving diversified production chains a chance.
	{
		const int32_t avg_b = kNormalizationBudget /
		   std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
		const int32_t cap_b = avg_b * 4;
		for (size_t w = 0; w < nr_wares; ++w) {
			if (raw_B[w] > cap_b) {
				raw_B[w] = cap_b;
			}
		}
	}

	// STEP 5a: Linear normalization of Factor A and Factor B independently.
	//
	// Each factor is normalized to kNormalizationBudget (10M) using linear
	// scaling: norm[w] = raw[w] * 10M / sum(raw). This ensures both factors
	// contribute equally to the final decision regardless of their absolute
	// magnitudes. Without this, demand signals (millions from backwards
	// propagation) overwhelm capacity signals (hundreds from production
	// balance), causing the AI to ignore capacity deficits.
	//
	// After normalization, both factors are on the same [0, 10M] scale.
	// Combined: raw_total = norm_A + norm_B, range [0, 20M].
	// The subsequent softmax normalization maps this to the final [0, 10M].
	{
		int64_t sum_A = 0;
		int64_t sum_B = 0;
		for (size_t w = 0; w < nr_wares; ++w) {
			sum_A += raw_A[w];
			sum_B += raw_B[w];
		}
		for (size_t w = 0; w < nr_wares; ++w) {
			const int32_t norm_A = (sum_A > 0) ?
			   static_cast<int32_t>(
			      static_cast<int64_t>(raw_A[w]) *
			      kNormalizationBudget / sum_A) : 0;
			const int32_t norm_B = (sum_B > 0) ?
			   static_cast<int32_t>(
			      static_cast<int64_t>(raw_B[w]) *
			      kNormalizationBudget / sum_B) : 0;
			raw_total[w] = norm_A + norm_B;
		}
	}

	// STEP 5b: Softmax normalization (polynomial exp approximation).
	//
	// weight(x) = 1 + x + x²/2  ≈  exp(x)  (2nd-order Taylor)
	//
	// Pointier than linear: the most scarce ware gets disproportionately
	// more budget — like an attention head. This prevents "average buildup"
	// where all wares progress equally slowly. Instead the AI focuses on
	// the most scarce ware, resolves it, then moves to the next.
	//
	// kSoftmaxScale controls the temperature (sharpness):
	//   raw_total is mapped to x ∈ [0, kSoftmaxScale].
	//   At scale=20: a 2:1 raw ratio yields ~3.6:1 weight ratio.
	//
	// Integer-safe: max weight = 1+20+200 = 221, max sum = 30×221 = 6630.
	// weight × 10M = 2.2×10⁹, fits int64 easily. No overflow possible.
	{
		constexpr int32_t kSoftmaxScale = 20;
		int32_t max_raw = 0;
		for (size_t w = 0; w < nr_wares; ++w) {
			max_raw = std::max(max_raw, raw_total[w]);
		}
		if (max_raw > 0) {
			std::vector<int32_t> weights(nr_wares, 0);
			int64_t S = 0;
			for (size_t w = 0; w < nr_wares; ++w) {
				if (raw_total[w] > 0) {
					const int32_t x = static_cast<int32_t>(
					   static_cast<int64_t>(raw_total[w]) * kSoftmaxScale / max_raw);
					weights[w] = 1 + x + x * x / 2;
					S += weights[w];
				}
			}
			if (S > 0) {
				for (size_t w = 0; w < nr_wares; ++w) {
					ware_pressure_[w].outputControl = (weights[w] > 0) ?
					   static_cast<int32_t>(
					      static_cast<int64_t>(weights[w]) *
					      kNormalizationBudget / S) :
					   0;
				}
			}
		}
	}

	// Debug: log top 8 ware pressures (scarcity ranking)
	{
		std::vector<std::pair<int32_t, size_t>> sorted_wp;
		for (size_t w = 0; w < nr_wares; ++w) {
			if (ware_pressure_[w].outputControl > 0) {
				sorted_wp.emplace_back(ware_pressure_[w].outputControl, w);
			}
		}
		std::sort(sorted_wp.begin(), sorted_wp.end(), std::greater<>());
		verb_log_info_time(gametime,
		   "P%u WARE SCARCITY (tick %u, %zu wares with pressure):\n",
		   static_cast<unsigned>(player_number()), pi_tick_count_,
		   sorted_wp.size());
		for (size_t i = 0; i < std::min<size_t>(8, sorted_wp.size()); ++i) {
			const size_t w = sorted_wp[i].second;
			const auto wi = static_cast<Widelands::DescriptionIndex>(w);
			verb_log_info_time(gametime,
			   "  P%u WP#%zu %s: score=%d (A=%d B=%d err=%d int=%d) stock=%u\n",
			   static_cast<unsigned>(player_number()),
			   i + 1, tribe_->get_ware_descr(wi)->name().c_str(),
			   ware_pressure_[w].outputControl, raw_A[w], raw_B[w],
			   ware_pressure_[w].error,
			   ware_pressure_[w].ipart, calculate_stocklevel(wi));
		}
	}

	// Sync PID state to persistent data for savegame persistence
	persistent_data->pi_tick_count = pi_tick_count_;
	persistent_data->ware_pressure_integrals.resize(nr_wares);
	persistent_data->ware_pressure_last_errors.resize(nr_wares);
	for (size_t w = 0; w < nr_wares; ++w) {
		persistent_data->ware_pressure_integrals[w] = ware_pressure_[w].ipart;
		persistent_data->ware_pressure_last_errors[w] = ware_pressure_[w].lastError;
	}
}

// Circle 2: Building Pressure
// Derives building demand from ware pressures (Circle 1) and
// expansion pressures (Circle 3).
void PlannerAI::update_building_pressures(const Time& /* gametime */) {
	if (building_pressure_.size() != buildings_.size()) {
		building_pressure_.resize(buildings_.size());
	}

	std::vector<int32_t> raw_total(buildings_.size(), 0);

	// Sum of all enemy expansion pressures for military demand
	int32_t total_enemy_expansion = 0;
	for (size_t t = 1; t < expansion_targets_.size(); ++t) {
		total_enemy_expansion += std::max<int32_t>(0, expansion_targets_[t].outputControl);
	}

	// === Recompute global weights from current game state ===
	const int32_t nr_wares_int = std::max<int32_t>(1,
	   static_cast<int32_t>(wares.size()));
	const int32_t economy_size =
	   static_cast<int32_t>(productionsites.size() + mines_.size());
	weights_.update(nr_wares_int, economy_size, spots_,
	   trees_on_territory_, rocks_on_territory_, kNormalizationBudget);

	const int32_t avg_wp = weights_.avg_wp;
	const int32_t N_ticks = weights_.N_ticks;
	const int32_t P_weight = weights_.P_weight;

	// Space scarcity: if we have few buildable fields relative to our
	// economy size, boost space-clearing buildings (clearing huts, quarries).
	// Desired: at least 1 buildable spot per 2 existing buildings.
	const int32_t desired_spots = economy_size / 2 + 5;
	const int32_t space_scarcity = std::max<int32_t>(0, desired_spots - spots_);

	// Precompute structural importance per building type.
	// For each production building, count how many OTHER building types
	// need its output wares (as construction material or production input).
	// This is used to give unbuilt buildings a minimum pressure floor
	// proportional to their structural importance in the economy,
	// breaking the chicken-and-egg deadlock where a ware has 0 consumers
	// so its producer gets 0 building pressure.
	std::vector<int32_t> structural_importance(buildings_.size(), 0);
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		const BuildingObserver& bo = buildings_[bi];
		if (bo.type != BuildingObserver::Type::kProductionsite &&
		    bo.type != BuildingObserver::Type::kMine) {
			continue;
		}
		for (const auto& output : bo.ware_outputs) {
			for (const BuildingObserver& other : buildings_) {
				// Needed as construction material?
				if (other.desc->buildcost().count(output) > 0) {
					++structural_importance[bi];
				}
				// Needed as production input?
				for (const auto& inp : other.inputs) {
					if (inp == output) {
						++structural_importance[bi];
						break;
					}
				}
			}
		}
	}

	// Precompute: for each ware, does a built/constructing producer exist?
	// Used for worker producibility check and CONTRA section below.
	const size_t nr_wares_sz = wares.size();
	std::vector<bool> ware_has_built_producer(nr_wares_sz, false);
	for (const BuildingObserver& whp_bo : buildings_) {
		if (whp_bo.type != BuildingObserver::Type::kProductionsite &&
		    whp_bo.type != BuildingObserver::Type::kMine) {
			continue;
		}
		if (whp_bo.cnt_built + whp_bo.cnt_under_construction == 0) {
			continue;
		}
		for (const auto& output : whp_bo.ware_outputs) {
			if (static_cast<size_t>(output) < nr_wares_sz) {
				ware_has_built_producer[output] = true;
			}
		}
	}

	// Precompute per-worker-type producibility.
	// A worker is "producible" if all its buildcost tools have built producers.
	// Used for counter-pressure dampening (Change 5a) and anti-windup (5b).
	worker_producible_.clear();
	for (const BuildingObserver& wp_bo : buildings_) {
		if (wp_bo.type != BuildingObserver::Type::kProductionsite &&
		    wp_bo.type != BuildingObserver::Type::kMine) {
			continue;
		}
		if (wp_bo.unoccupied_count == 0) {
			continue;
		}
		for (const auto& worker_idx : wp_bo.positions) {
			if (worker_producible_.count(worker_idx) > 0) {
				continue;  // Already checked this worker type
			}
			const Widelands::WorkerDescr* wd =
			   tribe_->get_worker_descr(worker_idx);
			if (wd == nullptr || !wd->is_buildable()) {
				worker_producible_[worker_idx] = false;
				continue;
			}
			bool producible = true;
			for (const auto& [tool_name, tool_amount] : wd->buildcost()) {
				Widelands::DescriptionIndex tool_idx = tribe_->ware_index(tool_name);
				if (tool_idx == Widelands::INVALID_INDEX ||
				    static_cast<size_t>(tool_idx) >= nr_wares_sz ||
				    !ware_has_built_producer[tool_idx]) {
					producible = false;
					break;
				}
			}
			worker_producible_[worker_idx] = producible;
		}
	}

	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		PIDController& bp = building_pressure_[bi];
		const BuildingObserver& bo = buildings_[bi];

		bp.error = 0;

		if (bo.type == BuildingObserver::Type::kProductionsite ||
		    bo.type == BuildingObserver::Type::kMine) {
			// Capacity-based: building pressure = output ware pressure.
			// Unit: bp.error [budget] (inherits from ware_pressure_.outputControl).
			// ware_pressure_[w].outputControl is normalized to [0, 10M budget].
			// So bp.error starts in [budget] units and stays there through
			// all subsequent additions (supporter demand, structural floor, etc).
			int32_t output_demand = 0;  // [budget]
			for (const auto& output : bo.ware_outputs) {
				output_demand = std::max(output_demand,
				   std::max<int32_t>(0, ware_pressure_[output].outputControl));
			}
			bp.error = output_demand;  // [budget]

			// Barracks: building pressure from recruiting pressure.
			// Barracks output soldiers (workers, not wares), so ware_outputs
			// is empty → output_demand = 0. Without this injection, barracks
			// get ZERO building pressure and are never built by the PI network.
			// The game goal → military_pressure_ → recruiting_pressure_ chain
			// only injects into barracks INPUT wares, not the building itself.
			if (bo.is(BuildingAttribute::kBarracks)) {
				const int32_t recruit_demand = static_cast<int32_t>(
				   static_cast<int64_t>(recruiting_pressure_) *
				   avg_wp / kNormalizationBudget);
				bp.error = std::max(bp.error, recruit_demand);
			}

			// === Proactive supporter coupling ===
			// Supporters (ranger, fishbreeder, gamekeeper) inherit pressure
			// from the buildings they sustain (woodcutter, fisher, hunter).
			//
			// Contract: supporter_demand = supported_output_pressure * ratio
			//   ratio = supported_count / max(1, supporter_count)
			//   When NO supporter exists: ratio = supported_count + 1
			//     → URGENTLY need the first supporter before resources deplete.
			//   When ratio = 1:1: demand = output pressure (balanced).
			//   Range: [0, (N+1) * max_ware_pressure]
			//
			// This ensures supporters get built BEFORE the supported building
			// runs out of resources (trees, fish, game animals). Without this,
			// woodcutters exhaust all trees before a ranger is even considered.
			if (!bo.supported_producers.empty()) {
				int32_t supporter_demand = 0;
				for (const auto& [sp_idx, sp_desc] : bo.supported_producers) {
					const BuildingObserver& sp_bo = get_building_observer(sp_desc->name().c_str());
					if (sp_bo.cnt_built > 0) {
						int32_t sp_pressure = 0;
						for (const auto& sp_out : sp_bo.ware_outputs) {
							sp_pressure = std::max(
							   sp_pressure, ware_pressure_[sp_out].outputControl);
						}
						const int32_t supported = sp_bo.cnt_built;
						const int32_t supporters =
						   bo.cnt_built + bo.cnt_under_construction;
						// When no supporter exists: multiply by (supported + 1)
						// to make the first supporter very urgent.
						// When supporters exist: ratio = supported / supporters.
						const int32_t effective_ratio =
						   (supporters == 0) ?
						      (supported + 1) :
						      std::max<int32_t>(1, supported / supporters);
						supporter_demand = std::max(
						   supporter_demand, sp_pressure * effective_ratio);
					}
				}
				bp.error = std::max(bp.error, supporter_demand);
			}

			// Predictive depletion pressure for supporters:
			// Watch the resource layer (territory counts), not the ware layer.
			// This fires BEFORE output ware pressure rises (anticipatory).
			// For each supported consumer: estimate net depletion rate
			// (consumers minus replenishment) and scarcity (how far below
			// saturation the territory resource count has fallen).
			// depletion_pressure = output_wp × net_rate × scarcity / saturation
			if (!bo.supported_producers.empty() && bo.is_resource_harvester) {
				int32_t depletion_pressure = 0;
				for (const auto& [sp_idx, sp_desc] : bo.supported_producers) {
					const BuildingObserver& sp_bo =
					   get_building_observer(sp_desc->name().c_str());
					if (sp_bo.cnt_built == 0) {
						continue;
					}
					// For each collected resource target of the consumer
					for (const auto& rt : sp_bo.resource_targets) {
						if (!rt.is_collected) {
							continue;
						}
						// Territory-wide resource count for this attribute
						int32_t territory_count = 0;
						auto rot_it = resource_on_territory_.find(rt.attribute_id);
						if (rot_it != resource_on_territory_.end()) {
							territory_count = rot_it->second;
						}

						const int32_t consumers = static_cast<int32_t>(sp_bo.cnt_built);
						const int32_t supporters_count =
						   static_cast<int32_t>(bo.cnt_built + bo.cnt_under_construction);
						const int32_t net_rate =
						   std::max<int32_t>(0, consumers - supporters_count);
						// Saturation: resource count where no pressure needed
						const int32_t sat = std::max<int32_t>(
						   1, static_cast<int32_t>(rt.saturation) * spots_ / 100);
						const int32_t scarcity =
						   std::max<int32_t>(0, sat - territory_count);

						// Max output ware pressure of the consumer
						int32_t sp_pressure = 0;
						for (const auto& sp_out : sp_bo.ware_outputs) {
							sp_pressure = std::max(
							   sp_pressure, ware_pressure_[sp_out].outputControl);
						}
						depletion_pressure = std::max(depletion_pressure,
						   sp_pressure * net_rate * scarcity / (sat + 1));
					}
				}
				bp.error = std::max(bp.error, depletion_pressure);
			}

			// Self-healing investment: if this building produces wares that
			// are ALSO its own construction material, building it is the
			// only way to break the deadlock. Example: shepherd needs cloth
			// to build, but IS the only cloth producer. Spending the last
			// cloth to build it produces more cloth → self-healing.
			//
			// Detect: for each output ware, check if it appears in the
			// building's own construction cost. If so, and we're the sole
			// or primary producer with output under pressure → big boost.
			if (bo.total_count() == 0 || bo.cnt_built == 0) {
				const Widelands::Buildcost& self_cost = bo.desc->buildcost();
				for (const auto& output : bo.ware_outputs) {
					if (self_cost.count(output) > 0 &&
					    static_cast<size_t>(output) < ware_pressure_.size() &&
					    ware_pressure_[output].outputControl > avg_wp) {
						// This building produces its own construction material
						// and that material is under above-average pressure.
						// Boost = output pressure (the scarcer the material,
						// the more critical it is to build this NOW).
						bp.error += ware_pressure_[output].outputControl;
					}
				}
			}

			// First instance: chain bootstrapping with structural floor.
			//
			// Problem: with the capacity formula, a ware that has NO
			// consumers yet gets 0 ware pressure → its producer gets 0
			// building pressure → never gets built (chicken-and-egg).
			// Example: cloth has 0 production consumers but IS a
			// construction material for many buildings.
			//
			// Fix: use structural_importance as a minimum floor.
			// structural_importance[bi] = how many building types need
			// this building's outputs (as CM or input). A weaving mill
			// producing cloth needed by 15 building types gets floor =
			// 15 × avg_wp / nr_wares = 0.5 × avg_wp. A niche building
			// gets a proportionally smaller floor.
			//
			// ×2 because the first producer is enabling — without it,
			// the entire downstream chain stalls.
			if (bo.total_count() == 0) {
				const int32_t struct_floor =
				   structural_importance[bi] * avg_wp / nr_wares_int;
				bp.error = std::max(bp.error,
				   std::max(output_demand, struct_floor)) * 2;
			}
			// Already under construction: divide by count+1.
			// Reason: each pending instance will satisfy some demand when done.
			// This is a natural diminishing-returns formula, not a magic number.
			if (bo.cnt_under_construction > 0) {
				bp.error /= (1 + static_cast<int32_t>(bo.cnt_under_construction));
			}
			// Mines: extra diminishing returns per total count.
			// Mines share finite resource deposits — each additional mine
			// of the same type depletes the deposit faster with sharply
			// diminishing output. At 2 mines: /2, at 3: /3, at 5: /5.
			if (bo.type == BuildingObserver::Type::kMine && bo.total_count() > 1) {
				bp.error /= static_cast<int32_t>(bo.total_count());
			}
			// Production sites: diminishing returns per built count.
			// Unlike mines (which deplete finite deposits → sharp 1/N),
			// production sites face market saturation: each additional
			// instance of the same type has diminishing marginal value.
			// Formula: /= (1 + floor(log2(cnt_built))). This gives:
			//   1 built: /1, 2: /2, 3-4: /3, 5-8: /4
			// Prevents the quarry-monopoly problem where one producer type
			// with high output demand (from CM inflation) keeps winning
			// every construction slot despite already having many instances.
			if (bo.type == BuildingObserver::Type::kProductionsite &&
			    bo.cnt_built > 1) {
				int32_t count = static_cast<int32_t>(bo.cnt_built);
				int32_t log2_divisor = 1;
				while (count > 1) {
					++log2_divisor;
					count >>= 1;
				}
				bp.error /= log2_divisor;
			}
			// Requires supporters but none exist → dampening by count.
			// E.g. don't build more woodcutters if there's no ranger yet.
			// Each unsupported instance is depleting resources faster, so
			// the more we have, the more we should wait for a supporter.
			// Divisor = cnt_built + 1: at 1 built, /2; at 3 built, /4.
			// This is proportional to the depletion risk, not a magic number.
			if (bo.requires_supporters && bo.cnt_built > 0) {
				bool has_any_supporter = false;
				for (const BuildingObserver& other : buildings_) {
					if (other.supported_producers.count(bo.id) > 0 &&
					    (other.cnt_built + other.cnt_under_construction) > 0) {
						has_any_supporter = true;
						break;
					}
				}
				if (!has_any_supporter) {
					bp.error /= static_cast<int32_t>(bo.cnt_built + 1);
				}
			}

			// Worker availability dampening: if this building type needs
			// workers that can't currently be produced (no tool chain),
			// dampen building pressure. This prevents unsolvable worker
			// demand from crowding out solvable problems. The PI integral
			// still accumulates (floor at 25%) so recovery is fast when
			// the chain is eventually built.
			if (bo.unoccupied_count > 0 && bo.cnt_built > 0) {
				bool any_worker_unproducible = false;
				for (const auto& worker_idx : bo.positions) {
					auto it = worker_producible_.find(worker_idx);
					if (it != worker_producible_.end() && !it->second) {
						any_worker_unproducible = true;
						break;
					}
				}
				if (any_worker_unproducible) {
					bp.error = bp.error / 4;
				}
			}

			// Obstacle clearing: density-based demand, self-normalizing.
			// Clearing demand = (obstacle_density) × avg_wp / nr_wares.
			// When density = nr_wares (one obstacle per ware type),
			// clearing demand = avg_wp → competes evenly with production.
			// This derives the threshold from the economy structure itself:
			// more ware types means a more complex economy that can
			// tolerate more obstacles before clearing becomes urgent.
			// Space scarcity amplifies: demand × (1 + scarcity/desired).
			if (bo.is(BuildingAttribute::kSpaceConsumer) &&
			    !bo.is(BuildingAttribute::kRanger) &&
			    !bo.is(BuildingAttribute::kLumberjack)) {
				int32_t clearing_demand = trees_on_territory_ / (spots_ + 1);
				clearing_demand = clearing_demand * avg_wp / nr_wares_int;
				if (bo.cnt_built > 0) {
					clearing_demand /= (1 + bo.cnt_built);
				}
				if (space_scarcity > 0) {
					clearing_demand = clearing_demand *
					   (desired_spots + space_scarcity) / desired_spots;
				}
				bp.error += clearing_demand;
			}
			if (bo.is(BuildingAttribute::kNeedsRocks)) {
				int32_t rock_demand = rocks_on_territory_ / (spots_ + 1);
				rock_demand = rock_demand * avg_wp / nr_wares_int;
				if (bo.cnt_built > 0) {
					rock_demand /= (1 + bo.cnt_built);
				}
				if (space_scarcity > 0) {
					rock_demand = rock_demand *
					   (desired_spots + space_scarcity) / desired_spots;
				}
				bp.error += rock_demand;
			}

		} else if (bo.type == BuildingObserver::Type::kMilitarysite) {
			// Contract: military demand is proportional to expansion pressure.
			//
			// The per-field integral in construct_building() already prevents
			// building on every border spot simultaneously (each field needs
			// sustained positive efficiency to trigger). So the building
			// pressure here just needs to be the RIGHT SIZE to keep military
			// buildings "allowed" and competing fairly with economy buildings.
			//
			// Anti-clustering: each building under construction or unoccupied
			// divides the demand (satisfaction divisor). This ensures the
			// integral doesn't accumulate when builds are already pending.
			//
			// The integral term itself provides smoothing across ticks.
			// No additional /N_ticks dampening needed — that caused the
			// military budget to be so tiny that expansion stalled.
			const int32_t unowned_pressure =
			   std::max<int32_t>(0, expansion_targets_[0].outputControl);
			const int32_t total_expansion = total_enemy_expansion + unowned_pressure;

			// Base demand: proportional to expansion need, in ware-pressure
			// units. At 100% expansion budget: demand = avg_wp (competing
			// evenly with economy buildings).
			const int32_t base_demand = static_cast<int32_t>(
			   static_cast<int64_t>(total_expansion) *
			   avg_wp / kNormalizationBudget);

			// Each military building under construction satisfies demand.
			// Subtract a proportional amount: at 1 in construction, demand
			// is halved. At 2, reduced to 1/3. This prevents clustering.
			const int32_t satisfaction = 1 + static_cast<int32_t>(bo.cnt_under_construction) +
			   static_cast<int32_t>(bo.unoccupied_count);
			bp.error = base_demand / satisfaction;

		} else if (bo.type == BuildingObserver::Type::kWarehouse) {
			// Contract: warehouse pressure = avg_wp when economy is large
			// enough (more production sites than warehouses can serve).
			// Threshold: nr_wares sites per warehouse (each ware type ~= 1 site).
			// Derived from economy structure: when we have more production
			// types than warehouse capacity, we need another warehouse.
			if (static_cast<int32_t>(productionsites.size() + mines_.size()) >
			    static_cast<int32_t>(numof_warehouses_) * nr_wares_int) {
				bp.error = avg_wp;
			}

		} else if (bo.type == BuildingObserver::Type::kTrainingsite) {
			// Contract: training pressure from military split,
			// scaled identically to military conversion above.
			bp.error = static_cast<int32_t>(
			   static_cast<int64_t>(training_pressure_) *
			   avg_wp / kNormalizationBudget);
			if (bo.cnt_under_construction > 0) {
				bp.error = 0;
			}
		}

		// Not buildable or limit reached → null (don't accumulate penalties).
		// Exception: enhancement buildings (enhanced_from != INVALID_INDEX)
		// can't be directly built but still need building pressure to:
		//   1. Drive Pass 3 enhancement decisions
		//   2. Propagate demand to predecessors
		const bool is_enhancement =
		   (bo.desc->enhanced_from() != Widelands::INVALID_INDEX);
		if ((!bo.buildable(*player_) && !is_enhancement) ||
		    bo.aimode_limit_status() != AiModeBuildings::kAnotherAllowed) {
			bp.error = 0;
		} else if (is_enhancement && !player_->is_building_type_allowed(bo.id)) {
			bp.error = 0;
		} else {
			// CM-missing penalty: scale building pressure by CM affordability.
			//
			// If the building's construction materials aren't in stock,
			// reduce its building pressure proportionally. This propagates
			// through the PI system: unaffordable buildings generate less
			// demand for their outputs AND less demand for their inputs,
			// naturally suppressing entire chains that can't be built.
			//
			// Floor at 100 (10%): the PI integral still accumulates slowly
			// so the building is ready when CM eventually becomes available.
			// Built/under-construction buildings have cm_ratio = 1000 (no penalty).
			if (bi < cm_ratio_.size() && cm_ratio_[bi] < 1000) {
				bp.error = bp.error * std::max<int32_t>(100, cm_ratio_[bi]) / 1000;
			}
		}

		// PID tick with economy-derived parameters (same as Circle 1).
		// Unit flow: bp.error [budget] → outputControl [raw_pid]
		//   = P_weight × error[budget] + 1 × ipart[budget] + N × Δerror[budget]
		//   Note: [raw_pid] here means "P-scaled budget" — dimensionally
		//   [budget × dimensionless] = [budget], but the magnitude is
		//   amplified by P_weight (up to 100×). Range: ~[-2G, +2G].
		bp.tick(P_weight, 1, N_ticks);
		raw_total[bi] = bp.outputControl;  // [raw_pid]
	}

	// Enhancement pressure propagation: if building bi is an enhancement
	// of building bj, bi's pressure adds to bj's pressure. This makes the
	// AI build more predecessors when enhancements are needed.
	// Example: master woodcutter (enhancement) needs woodcutter (predecessor).
	// If master_woodcutter has building pressure, that pressure should also
	// boost regular woodcutter's pressure → more woodcutters get built →
	// more candidates for future upgrades.
	//
	// Propagation happens BEFORE normalization so the predecessor gets a
	// naturally larger share of the 10M budget.
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		const BuildingObserver& bo = buildings_[bi];
		if (raw_total[bi] <= 0) {
			continue;
		}
		const Widelands::DescriptionIndex from = bo.desc->enhanced_from();
		if (from == Widelands::INVALID_INDEX) {
			continue;
		}
		// Find the predecessor's index in buildings_
		for (size_t bj = 0; bj < buildings_.size(); ++bj) {
			if (buildings_[bj].id == from) {
				// Add enhancement demand to predecessor.
				// Scale by how many predecessors exist: if we already have
				// many predecessors, the demand for MORE is lower.
				// At 0 predecessors: full propagation (need one to upgrade).
				// At 1: half (one candidate exists). At 2: third.
				const int32_t pred_count = static_cast<int32_t>(
				   buildings_[bj].cnt_built + buildings_[bj].cnt_under_construction);
				raw_total[bj] += raw_total[bi] / (1 + pred_count);
				break;
			}
		}
	}

	// Linear normalization to kNormalizationBudget.
	// Unit: raw_total[bi] [raw_pid] → building_pressure_[bi].outputControl [budget]
	// S uses int64_t because sum of ~80 buildings × ~200M each = ~16G > int32 max.
	// After: outputControl = raw_total × 10M / S ∈ [0, 10M] [budget].
	int64_t S = 0;
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		if (raw_total[bi] > 0) {
			S += raw_total[bi];
		}
	}
	if (S > 0) {
		for (size_t bi = 0; bi < buildings_.size(); ++bi) {
			building_pressure_[bi].outputControl =
			   (raw_total[bi] > 0) ?
			      static_cast<int32_t>(
			         static_cast<int64_t>(raw_total[bi]) * kNormalizationBudget / S) :
			      0;
		}
	}

	// ========== CONTRA: Building Prevention (why NOT to build) ==========
	//
	// Dual-PID: PRO says "build this", CONTRA says "don't build this".
	// Build decision = PRO.outputControl - CONTRA.outputControl.
	//
	// CONTRA accumulates:
	//   1. Missing input chain: each production input without a producer
	//      → avg_wp penalty. Weaponsmithy without mine = massive contra.
	//   2. Non-renewable CM: building consumes CM wares that have no
	//      producer → depletes finite HQ stock.
	//   3. Already building: under-construction count → don't pile up.
	//   4. Economy too young for military: military with tiny economy.
	//
	// CONTRA is NOT normalized to a budget. It uses the same avg_wp
	// scale as PRO so subtraction is meaningful. When everything is
	// fine, CONTRA = 0. Only active problems generate contra pressure.
	if (building_prevention_.size() != buildings_.size()) {
		building_prevention_.resize(buildings_.size());
	}

	// ware_has_built_producer was precomputed above (before the PRO loop).
	const size_t contra_nr_wares = nr_wares_sz;

	// Precompute transitive production chains for unproduced CM wares.
	// For each CM ware without a built producer, BFS backwards through the
	// production graph to find all wares transitively upstream.
	// Buildings producing upstream wares are "chain-essential" — building
	// them is an investment toward producing the scarce CM, not a waste.
	//
	// Example: cloth has no producer.
	//   cloth ← weaving_mill(yarn) ← spinning_mill(wool) ← shepherd
	//   cm_upstream_of[cloth] = {cloth, yarn, wool}
	//   → shepherd produces wool ∈ set → exempt from cloth CM contra
	//   → spinning_mill produces yarn ∈ set → exempt from cloth CM contra
	//   → weaving_mill produces cloth ∈ set → exempt (also self-healing)
	std::vector<std::vector<bool>> cm_upstream_of(contra_nr_wares);
	for (size_t w = 0; w < contra_nr_wares; ++w) {
		if (ware_has_built_producer[w]) {
			continue;
		}
		const auto wi = static_cast<Widelands::DescriptionIndex>(w);
		// Check if this ware is used as construction material
		bool is_cm = false;
		for (const BuildingObserver& cm_bo : buildings_) {
			if (cm_bo.desc->buildcost().count(wi) > 0) {
				is_cm = true;
				break;
			}
		}
		if (!is_cm) {
			continue;
		}
		// BFS: find all wares transitively needed to produce wi
		cm_upstream_of[w].assign(contra_nr_wares, false);
		cm_upstream_of[w][w] = true;  // The CM ware itself
		std::vector<Widelands::DescriptionIndex> frontier;
		frontier.push_back(wi);
		while (!frontier.empty()) {
			std::vector<Widelands::DescriptionIndex> next_frontier;
			for (const auto& target : frontier) {
				for (const BuildingObserver& prod_bo : buildings_) {
					bool produces = false;
					for (const auto& out : prod_bo.ware_outputs) {
						if (out == target) {
							produces = true;
							break;
						}
					}
					if (!produces) {
						continue;
					}
					for (const auto& inp : prod_bo.inputs) {
						if (static_cast<size_t>(inp) < contra_nr_wares &&
						    !cm_upstream_of[w][inp]) {
							cm_upstream_of[w][inp] = true;
							next_frontier.push_back(inp);
						}
					}
				}
			}
			frontier = std::move(next_frontier);
		}
	}

	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		PIDController& cv = building_prevention_[bi];
		const BuildingObserver& bo = buildings_[bi];

		cv.error = 0;

		// Already built or under construction → minimal contra
		// (committed investments shouldn't accumulate prevention)
		if (bo.cnt_built > 0) {
			// No contra for existing buildings.
		} else if (bo.type == BuildingObserver::Type::kProductionsite ||
		           bo.type == BuildingObserver::Type::kMine) {
			// --- 1. Missing input chain ---
			// Each input ware without a producer → big contra.
			// This prevents: weaponsmithy without mine, bakery without farm,
			// spinning_mill without shepherds, etc.
			for (const auto& input : bo.inputs) {
				if (static_cast<size_t>(input) < contra_nr_wares &&
				    !ware_has_built_producer[input]) {
					cv.error += avg_wp;
				}
			}

			// --- 2. Non-renewable CM consumption ---
			// Building consumes CM wares (clay, branch, etc.) that have no
			// producer. HQ stock is all we have. Penalty proportional to
			// how much of the finite stock this building would consume.
			//
			// Exception: chain-essential buildings. If this building produces
			// a ware that is transitively upstream of the scarce CM ware,
			// building it is an INVESTMENT toward CM production, not waste.
			// Example: shepherd needs cloth (CM) but produces wool → wool
			// is upstream of cloth → shepherd exempt from cloth contra.
			const Widelands::Buildcost& cost = bo.desc->buildcost();
			for (const auto& [ware_idx, amount] : cost) {
				if (static_cast<size_t>(ware_idx) < contra_nr_wares &&
				    !ware_has_built_producer[ware_idx]) {
					// Chain-essential exemption: skip contra if this building
					// produces a ware needed to eventually produce this CM.
					if (!cm_upstream_of[ware_idx].empty()) {
						bool chain_essential = false;
						for (const auto& output : bo.ware_outputs) {
							if (static_cast<size_t>(output) < contra_nr_wares &&
							    cm_upstream_of[ware_idx][output]) {
								chain_essential = true;
								break;
							}
						}
						if (chain_essential) {
							continue;
						}
					}
					// Finite stock: penalty scales with amount consumed.
					// At amount=3 of stock=50: penalty = avg_wp * 3/50 = small.
					// At amount=5 of stock=5: penalty = avg_wp * 5/5 = avg_wp.
					const int64_t stock = static_cast<int64_t>(
					   calculate_total_stocklevel(ware_idx));
					if (stock > 0) {
						cv.error += static_cast<int32_t>(
						   static_cast<int64_t>(avg_wp) * amount /
						   std::max<int64_t>(1, stock));
					} else {
						// Zero stock, no producer: massive contra.
						cv.error += avg_wp * 2;
					}
				}
			}

			// --- 3. Already building ---
			if (bo.cnt_under_construction > 0) {
				cv.error += avg_wp * static_cast<int32_t>(bo.cnt_under_construction);
			}

		} else if (bo.type == BuildingObserver::Type::kMilitarysite) {
			// --- 4. Economy too young for military ---
			// Military buildings consume CM (clay, cloth, etc.).
			// Before economy has CM producers, military = waste.
			// Contra proportional to how many CM wares lack producers.
			const Widelands::Buildcost& cost = bo.desc->buildcost();
			int32_t missing_cm_producers = 0;
			for (const auto& [ware_idx, amount] : cost) {
				if (static_cast<size_t>(ware_idx) < contra_nr_wares &&
				    !ware_has_built_producer[ware_idx]) {
					++missing_cm_producers;
				}
			}
			if (missing_cm_producers > 0) {
				// Each missing CM producer = half avg_wp contra.
				// A tent needing clay+cloth with neither produced → avg_wp contra.
				cv.error += avg_wp * missing_cm_producers / 2;
			}

			// Already building military → contra to prevent clustering
			if (bo.cnt_under_construction > 0) {
				cv.error += avg_wp * static_cast<int32_t>(bo.cnt_under_construction);
			}

		} else if (bo.type == BuildingObserver::Type::kTrainingsite) {
			// Training sites: contra if their inputs lack producers
			for (const auto& input : bo.inputs) {
				if (static_cast<size_t>(input) < contra_nr_wares &&
				    !ware_has_built_producer[input]) {
					cv.error += avg_wp;
				}
			}
			if (bo.cnt_under_construction > 0) {
				cv.error += avg_wp * 2;
			}
		}

		// Not buildable → no contra needed (PRO is already 0)
		if (!bo.buildable(*player_) &&
		    bo.desc->enhanced_from() == Widelands::INVALID_INDEX) {
			cv.error = 0;
		}

		// PID tick (same parameters as PRO)
		cv.tick(P_weight, 1, N_ticks);
		// CONTRA total is never negative (no "anti-prevention")
		if (cv.outputControl < 0) {
			cv.outputControl = 0;
		}
	}

	// Normalize CONTRA to kNormalizationBudget (same scale as PRO).
	//
	// Without this, CONTRA uses absolute avg_wp units while PRO is
	// normalized to 10M. In large economies (N_ticks=10, P_weight=20),
	// CONTRA for 1 missing input reaches ~13M steady state while
	// PRO is capped at 10M total budget. This makes CONTRA
	// disproportionately strong, permanently blocking buildings
	// with even minor issues.
	//
	// With normalization: the total CONTRA budget equals the total
	// PRO budget (10M). A building with the MOST contra problems
	// absorbs the largest share. When problems are concentrated
	// (1 building, 3 missing inputs), that building is strongly
	// blocked. When problems are distributed (many buildings,
	// 1 issue each), each gets moderate blocking.
	{
		int64_t contra_sum = 0;
		for (size_t bi = 0; bi < buildings_.size(); ++bi) {
			if (building_prevention_[bi].outputControl > 0) {
				contra_sum += building_prevention_[bi].outputControl;
			}
		}
		if (contra_sum > 0) {
			for (size_t bi = 0; bi < buildings_.size(); ++bi) {
				if (building_prevention_[bi].outputControl > 0) {
					building_prevention_[bi].outputControl = static_cast<int32_t>(
					   static_cast<int64_t>(building_prevention_[bi].outputControl) *
					   kNormalizationBudget / contra_sum);
				}
			}
		}
	}

	// --- Construction site ware priority (per-site, per-slot) ---
	// Each construction site gets per-ware priority based on stock scarcity.
	// Scarce CM wares → kHigh (64× kNormal) so economy routes them to
	// construction first, before production buildings consume them.
	// No global throttle needed — the economy handles distribution.
	const Widelands::Map& cs_map = game().map();

	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		BuildingObserver& bo = buildings_[bi];
		if (bo.cnt_under_construction == 0) {
			continue;
		}

		// Find actual construction sites of this type on the map
		const auto& stats = player_->get_building_statistics(bo.id);
		for (const auto& bs : stats) {
			if (!bs.is_constructionsite) {
				continue;
			}
			Widelands::BaseImmovable* imm = cs_map[bs.pos].get_immovable();
			if (imm == nullptr) {
				continue;
			}
			auto* cs = dynamic_cast<Widelands::Building*>(imm);
			if (cs == nullptr || cs->owner().player_number() != player_->player_number()) {
				continue;
			}

			// Set priority per input slot based on CM scarcity.
			// Each ware gets its own priority: scarce wares get kHigh
			// so the economy routes them to construction first.
			// Adequate wares stay kNormal for balanced distribution.
			const Widelands::Buildcost& cost = bo.desc->buildcost();
			for (const auto& [ware_idx, amount] : cost) {
				const uint32_t stock =
				   calculate_total_stocklevel(ware_idx);
				// Scarce: stock can't cover even this one site's need.
				// kHigh = 64× kNormal → economy delivers here first.
				const Widelands::WarePriority desired =
				   (stock < static_cast<uint32_t>(amount) * 2) ?
				      Widelands::WarePriority::kHigh :
				      Widelands::WarePriority::kNormal;
				if (cs->get_priority(Widelands::wwWARE, ware_idx, 0) != desired) {
					game().send_player_set_ware_priority(
					   *cs, Widelands::wwWARE, ware_idx, desired);
				}
			}
		}
	}

	// Sync building PID state to persistent data
	persistent_data->building_pressure_integrals.resize(buildings_.size());
	persistent_data->building_pressure_last_errors.resize(buildings_.size());
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		persistent_data->building_pressure_integrals[bi] = building_pressure_[bi].ipart;
		persistent_data->building_pressure_last_errors[bi] = building_pressure_[bi].lastError;
	}
	persistent_data->building_prevention_integrals.resize(buildings_.size());
	persistent_data->building_prevention_last_errors.resize(buildings_.size());
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		persistent_data->building_prevention_integrals[bi] = building_prevention_[bi].ipart;
		persistent_data->building_prevention_last_errors[bi] = building_prevention_[bi].lastError;
	}
}

// Circle 3: Expansion Pressure
// Determines where to expand: unowned land vs each enemy player.
void PlannerAI::update_expansion_pressures(const Time& /* gametime */) {
	if (expansion_targets_.empty()) {
		return;
	}

	const uint32_t our_power = player_statistics.get_player_power(player_number());
	const uint32_t our_land = player_statistics.get_player_land(player_number());

	// Win condition modifiers
	const std::string& wc = game().get_win_condition_displayname();
	const bool wc_eliminate = (wc == "Autocrat" || wc == "HQ Hunter");
	const bool wc_territorial = (wc == "Territorial Lord" || wc == "Territorial Time");

	std::vector<int32_t> raw_total(expansion_targets_.size(), 0);

	// === Land value estimation ===
	// What is one conquered field worth in ware-pressure units?
	// Each field could potentially support a production building.
	// Value = max output ware pressure across all building types,
	// divided by the building's work area (how many fields it needs).
	//
	// Example: a woodcutter works a ~50 field radius, output log pressure
	// is 300k. Per-field value = 300k/50 = 6k. A farm works ~30 fields,
	// output grain pressure = 400k → per-field = 13k. Take max = 13k.
	//
	// This converts territory from "field count" to "ware production
	// potential", making it directly comparable with building costs.
	{
		int32_t best_per_field = 0;
		for (size_t bi = 0; bi < buildings_.size() && bi < building_pressure_.size(); ++bi) {
			const BuildingObserver& bo = buildings_[bi];
			if (bo.type != BuildingObserver::Type::kProductionsite &&
			    bo.type != BuildingObserver::Type::kMine) {
				continue;
			}
			// Get total output ware pressure for this building
			int32_t output_pressure = 0;
			for (const auto& output : bo.ware_outputs) {
				if (static_cast<size_t>(output) < ware_pressure_.size()) {
					output_pressure += std::max<int32_t>(0, ware_pressure_[output].outputControl);
				}
			}
			if (output_pressure <= 0) {
				continue;
			}
			// Work area: approximate field count ≈ π × radius².
			// Production sites: radius ~5 (area ~78). Mines: ~1 field.
			// Use desc->workarea_info() if available, otherwise default.
			int32_t work_area_fields = 1;
			const auto& wa = bo.desc->workarea_info();
			if (!wa.empty()) {
				// Largest workarea radius
				const uint32_t radius = wa.rbegin()->first;
				work_area_fields = std::max<int32_t>(1,
				   static_cast<int32_t>(radius * radius * 3));  // ~π ≈ 3
			} else if (bo.desc->get_ismine()) {
				work_area_fields = 6;  // mine works ~6 fields
			} else {
				work_area_fields = 50;  // default production site
			}
			const int32_t per_field = output_pressure / work_area_fields;
			if (per_field > best_per_field) {
				best_per_field = per_field;
			}
		}
		// Bootstrap floor: at game start, no production buildings exist
		// so output_pressure = 0 for all buildings → best_per_field = 0.
		// With land_value_per_field_ = 0, military efficiency = 0 and
		// the per-spot military integral only decays, never accumulates.
		// This prevents ANY military building from ever being placed.
		//
		// Floor = avg_wp / 100: a modest seed that lets military
		// integrals start accumulating from tick 1. Once real production
		// exists, the computed value quickly exceeds this floor.
		// For 32 wares: avg_wp = 312K, floor = 3.1K per field.
		const int32_t avg_wp_land = kNormalizationBudget /
		   std::max<int32_t>(1, static_cast<int32_t>(wares.size()));
		land_value_per_field_ = std::max(best_per_field, avg_wp_land / 100);
	}

	// Index 0: Unowned land
	//
	// Priority inheritance: expansion pressure inherits from what we
	// want to BUILD on conquered land. Two components:
	//   1. Mine field demand: sum of building pressures for mine types
	//      we can't build yet (no mineable fields of that type), plus
	//      a base offset per mine type for long-term resource security.
	//   2. Normal field demand: sum of building pressures for production
	//      sites we need but lack space for, plus a base offset for
	//      discovery (finding new resources, port spaces, etc.).
	//
	// This creates a direct flow: "I need gold → gold mine has high
	// building pressure → expansion pressure is high → military
	// buildings get priority → we expand → we find gold fields."
	{
		const int32_t nr_wares_exp = std::max<int32_t>(1,
		   static_cast<int32_t>(wares.size()));
		const int32_t avg_wp =
		   kNormalizationBudget / nr_wares_exp;

		// Mine field demand: inherit pressure from mine buildings we
		// can't build because we lack mineable fields.
		// For each mine type where we have zero mineable fields OR zero
		// built mines: add the building pressure of that mine type.
		// Also add a base offset (avg_wp) per mine type for long-term
		// resource security — even if we don't need gold NOW, we want
		// access to gold fields for LATER.
		int32_t mine_expansion = 0;
		for (size_t bi = 0; bi < buildings_.size() && bi < building_pressure_.size(); ++bi) {
			const BuildingObserver& bo = buildings_[bi];
			if (bo.type != BuildingObserver::Type::kMine) {
				continue;
			}
			// Base offset: always want access to mine resources (avg_wp)
			mine_expansion += avg_wp;
			// Inherited demand: if this mine has building pressure, inherit it
			if (building_pressure_[bi].outputControl > 0) {
				mine_expansion += building_pressure_[bi].outputControl;
			}
			// Extra urgency when we have NO mineable fields of this type
			if (bo.mines != Widelands::INVALID_INDEX) {
				bool have_fields = false;
				for (const MineableField* mf : mineable_fields) {
					if (mf->coords.field->get_resources() == bo.mines) {
						have_fields = true;
						break;
					}
				}
				if (!have_fields) {
					// Can't build this mine at all — double the inherited pressure
					mine_expansion += std::max(avg_wp,
					   building_pressure_[bi].outputControl);
				}
			}
		}

		// Normal field demand: inherit from production sites that need
		// more building spots. When spots_ is low relative to economy
		// size, we need more land for general building.
		const int32_t economy_sz =
		   static_cast<int32_t>(productionsites.size() + mines_.size());
		int32_t normal_expansion = 0;
		// Base offset: always want to discover more land (avg_wp).
		// New land reveals resources, port spaces, and building options.
		normal_expansion += avg_wp;
		// Space pressure: when buildable spots are scarce relative to
		// economy size, expansion becomes urgent for building room.
		if (spots_ < economy_sz) {
			normal_expansion += avg_wp *
			   std::min<int32_t>(nr_wares_exp, economy_sz - spots_) / nr_wares_exp;
		}

		// Port space discovery bonus
		bool portspace_visible = false;
		for (const BuildableField* bf : buildable_fields) {
			if (bf->portspace_nearby == ExtendedBool::kTrue) {
				portspace_visible = true;
				break;
			}
		}
		if (portspace_visible) {
			normal_expansion += avg_wp;
		}

		// Win condition bonus
		if (wc_territorial) {
			normal_expansion += avg_wp;
		}

		// Total unowned land pressure = mine + normal demands
		expansion_targets_[0].error = mine_expansion + normal_expansion;

		// Per enemy player
		// Inherit pressure from enemy threat: military power gap and
		// land gap. Bully weight scales the per-enemy pressure.
		// bully_neutral = avg_wp (same scale as other pressures).
		const int32_t bully_neutral = std::max<int32_t>(1, avg_wp);
		for (Widelands::PlayerNumber pn = 1;
		     pn <= static_cast<Widelands::PlayerNumber>(expansion_targets_.size() - 1);
		     ++pn) {
			if (pn >= expansion_targets_.size()) {
				break;
			}
			if (!player_statistics.get_is_enemy(pn)) {
				expansion_targets_[pn].error = 0;
				continue;
			}

			int32_t ep = 0;
			const uint32_t enemy_power = player_statistics.get_player_power(pn);
			const uint32_t enemy_land = player_statistics.get_player_land(pn);

			// Power deficit: each unit of power gap adds pressure.
			// Scale by avg_wp so it competes with other expansion reasons.
			if (enemy_power > our_power) {
				ep += static_cast<int32_t>(
				   static_cast<int64_t>(enemy_power - our_power) *
				   avg_wp / std::max<uint32_t>(1, our_power + 1));
			}
			if (enemy_land > our_land) {
				ep += static_cast<int32_t>(
				   static_cast<int64_t>(enemy_land - our_land) *
				   avg_wp / std::max<uint32_t>(1, our_land + 1));
			}

			// Win condition bonuses
			if (wc_eliminate) {
				ep += avg_wp;
			}
			if (wc_territorial && enemy_land > our_land) {
				ep += static_cast<int32_t>(
				   static_cast<int64_t>(enemy_land - our_land) *
				   avg_wp / std::max<uint32_t>(1, our_land + 1));
			}

			ep = ep * get_bully_weight(pn) / bully_neutral;

			expansion_targets_[pn].error = ep;
		}

		// Inherit enemy pressure to unowned land expansion.
		// Statistics tell us total enemy land, but we can only reach
		// enemies we border. To defeat an unseen enemy we must first
		// expand through unowned land to find them. So all enemy
		// pressure also feeds unowned land expansion (index 0).
		// Normalization handles the relative proportions.
		for (Widelands::PlayerNumber pn = 1;
		     pn < static_cast<Widelands::PlayerNumber>(expansion_targets_.size());
		     ++pn) {
			if (expansion_targets_[pn].error > 0) {
				expansion_targets_[0].error += expansion_targets_[pn].error;
			}
		}
	}

	// === Goal-based error injection (win-condition-aware) ===
	//
	// This is the TOP-LEVEL GOAL that drives the entire economy.
	// The chain: goal error → expansion pressure → military_pressure_
	// → training/recruiting split → ware pressures → building pressures.
	// By injecting here (not into individual wares), the full PI chain
	// handles building prioritization correctly: training sites get
	// building pressure alongside their input-producing buildings.
	{
		const std::string& wc_goal = game().get_win_condition_displayname();
		const bool wc_economy_goal =
		   (wc_goal == "Collectors" || wc_goal == "Wood Gnome" || wc_goal == "Artifacts");
		const bool wc_hq_hunter = (wc_goal == "HQ Hunter");
		const bool wc_terr_goal =
		   (wc_goal == "Territorial Lord" || wc_goal == "Territorial Time");

		const uint32_t our_power_g = player_statistics.get_player_power(player_number());
		const uint32_t our_land_g = player_statistics.get_player_land(player_number());
		const int32_t nr_wares_g = std::max<int32_t>(1,
		   static_cast<int32_t>(wares.size()));
		const int32_t avg_wp_g = kNormalizationBudget / nr_wares_g;

		// Military goal: desired_lead + (best_enemy_strength - our_strength)
		// Positive = behind desired trajectory → push harder.
		// Zero = goal met → PI stalls, economy coasts.
		int32_t goal_error = 0;
		if (!wc_economy_goal) {
			const uint32_t best_enemy_strength =
			   player_statistics.get_enemies_max_power();
			goal_error = std::max<int32_t>(0,
			   desired_lead_ +
			   static_cast<int32_t>(best_enemy_strength) -
			   static_cast<int32_t>(our_power_g));

			// Growth rate penalty: if enemy is growing faster, extra urgency.
			const uint32_t our_old_strength =
			   player_statistics.get_old60_player_power(player_number());
			const int32_t our_growth =
			   static_cast<int32_t>(our_power_g) -
			   static_cast<int32_t>(our_old_strength);
			const int32_t enemy_growth_est =
			   static_cast<int32_t>(best_enemy_strength) -
			   static_cast<int32_t>(
			      std::min(best_enemy_strength, our_old_strength));
			if (enemy_growth_est > our_growth) {
				goal_error += (enemy_growth_est - our_growth) / 2;
			}
		}

		// Territorial mode: also count land deficit.
		if (wc_terr_goal) {
			const uint32_t best_enemy_land =
			   player_statistics.get_enemies_max_land();
			const int32_t land_goal = std::max<int32_t>(0,
			   desired_lead_ * 100 +
			   static_cast<int32_t>(best_enemy_land) -
			   static_cast<int32_t>(our_land_g));
			goal_error = std::max(goal_error, land_goal / 5);
		}

		// HQ Hunter: extra urgency — the goal is to DESTROY enemy HQs,
		// not just match strength. Push aggressive expansion toward
		// enemy warehouses. Attack scoring already prioritizes warehouses
		// (all-in attack), this amplifies the military buildup.
		if (wc_hq_hunter) {
			goal_error = goal_error * 3 / 2;
		}

		// Economy mode: no military goal, just expansion for building room.
		if (wc_economy_goal) {
			goal_error = std::max<int32_t>(1, std::abs(desired_lead_));
		}

		// Ensure minimum push at game start (both strengths = 0,
		// desired_lead > 0 for Normal/Hard → goal_error > 0).
		goal_error = std::max<int32_t>(goal_error,
		   std::max<int32_t>(0, desired_lead_));

		// Inject: add goal error to expansion targets.
		if (goal_error > 0) {
			const int32_t scaled_goal = goal_error * avg_wp_g;

			// Unowned land: always push expansion
			expansion_targets_[0].error += scaled_goal;

			// Per-enemy: add goal pressure so military builds toward them.
			for (Widelands::PlayerNumber pn = 1;
			     pn < static_cast<Widelands::PlayerNumber>(expansion_targets_.size());
			     ++pn) {
				if (player_statistics.get_is_enemy(pn)) {
					expansion_targets_[pn].error += scaled_goal / 2;
				}
			}
		}
	}

	// PID tick per target
	// Use same economy-derived parameters as Circle 1 & 2.
	const int32_t economy_sz_pi = std::max<int32_t>(1,
	   static_cast<int32_t>(productionsites.size() + mines_.size()));
	int32_t N_ticks_exp = 2;
	{
		int32_t temp = economy_sz_pi;
		while (N_ticks_exp * N_ticks_exp < temp) {
			++N_ticks_exp;
		}
	}
	N_ticks_exp = std::min<int32_t>(N_ticks_exp, 50);
	const int32_t P_weight_exp = 2 * N_ticks_exp;

	for (size_t t = 0; t < expansion_targets_.size(); ++t) {
		PIDController& et = expansion_targets_[t];
		et.tick(P_weight_exp, 1, N_ticks_exp);
		raw_total[t] = et.outputControl;
	}

	// Normalization to kNormalizationBudget
	int64_t S = 0;
	for (size_t t = 0; t < expansion_targets_.size(); ++t) {
		if (raw_total[t] > 0) {
			S += raw_total[t];
		}
	}
	if (S > 0) {
		for (size_t t = 0; t < expansion_targets_.size(); ++t) {
			expansion_targets_[t].outputControl =
			   (raw_total[t] > 0) ?
			      static_cast<int32_t>(
			         static_cast<int64_t>(raw_total[t]) * kNormalizationBudget / S) :
			      0;
		}
	}

	// Compute military_pressure_ from ALL expansion pressures.
	// Unowned land (index 0) also needs soldiers to garrison military
	// buildings that conquer that land, so it contributes too.
	military_pressure_ = 0;
	for (size_t t = 0; t < expansion_targets_.size(); ++t) {
		military_pressure_ += std::max<int32_t>(0, expansion_targets_[t].outputControl);
	}

	// Sync expansion PID state to persistent data
	persistent_data->expansion_integrals.resize(expansion_targets_.size());
	persistent_data->expansion_last_errors.resize(expansion_targets_.size());
	for (size_t t = 0; t < expansion_targets_.size(); ++t) {
		persistent_data->expansion_integrals[t] = expansion_targets_[t].ipart;
		persistent_data->expansion_last_errors[t] = expansion_targets_[t].lastError;
	}
}

// Military PID: Training vs Recruiting Split (ROI-based)
//
// Computes the EXACT cost ratio between training and recruiting:
//
//   training_value  = Δstrength × land_defended × land_value
//   training_cost   = Σ(input_ware_pressure × consumption_per_cycle)
//   recruiting_value = slots_to_fill × rookie_strength × land_value
//   recruiting_cost  = barracks_input_ware_pressure
//
// The split is proportional to value/cost ratios, ensuring the AI
// invests where the ROI is highest. When wares are scarce (high
// pressure), training becomes expensive → recruit instead. When
// land is vast and soldiers are weak, training ROI rises.
//
void PlannerAI::update_military_split() {
	if (military_pressure_ <= 0) {
		training_pressure_ = 0;
		recruiting_pressure_ = 0;
		return;
	}

	// --- Soldier stats: compute marginal training value ---
	// Δstrength per training level = how much stronger a soldier gets.
	// Computed from the SoldierDescr's per-level increments.
	// Combat power formula (from calculate_strength):
	//   power = attack × health / (defense_gap × evade_gap)
	// where defense_gap = 100 - defense%, evade_gap = 100 - evade%.
	//
	// A level-0 soldier has base stats. Each training cycle upgrades
	// one attribute by one level. The marginal value is the derivative
	// of the power function with respect to that attribute.
	//
	// Simplified: average Δpower across all trainable attributes,
	// expressed as a fraction of base power (dimensionless multiplier).
	// A multiplier of 0.5 means training adds 50% to base power.

	const Widelands::SoldierDescr* soldier_descr = nullptr;
	{
		const Widelands::DescriptionIndex soldier_idx = tribe_->soldier();
		if (soldier_idx != Widelands::INVALID_INDEX) {
			soldier_descr = dynamic_cast<const Widelands::SoldierDescr*>(
			   tribe_->get_worker_descr(soldier_idx));
		}
	}

	// Base (level-0) combat power components
	int32_t base_attack = 1;
	int32_t base_health = 1;
	int32_t base_defense_gap = 100;
	int32_t base_evade_gap = 100;
	int32_t max_total_levels = 0;

	// Per-level increments
	int32_t attack_incr = 0;
	int32_t health_incr = 0;
	int32_t defense_incr = 0;
	int32_t evade_incr = 0;

	if (soldier_descr != nullptr) {
		base_attack = static_cast<int32_t>(
		   soldier_descr->get_base_min_attack() +
		   (soldier_descr->get_base_max_attack() -
		      soldier_descr->get_base_min_attack()) / 2);
		base_health = static_cast<int32_t>(soldier_descr->get_base_health());
		base_defense_gap = std::max<int32_t>(1,
		   100 - static_cast<int32_t>(soldier_descr->get_base_defense()));
		base_evade_gap = std::max<int32_t>(1,
		   100 - static_cast<int32_t>(soldier_descr->get_base_evade()));

		attack_incr = static_cast<int32_t>(soldier_descr->get_attack_incr_per_level());
		health_incr = static_cast<int32_t>(soldier_descr->get_health_incr_per_level());
		defense_incr = static_cast<int32_t>(soldier_descr->get_defense_incr_per_level());
		evade_incr = static_cast<int32_t>(soldier_descr->get_evade_incr_per_level());

		max_total_levels = static_cast<int32_t>(soldier_descr->get_max_total_level());
	}

	// Base power (level-0 soldier): attack × health / (def_gap × evade_gap)
	// Scale to integer: multiply by 1000 for precision.
	const int64_t base_power = static_cast<int64_t>(base_attack) *
	   base_health * 1000 / (base_defense_gap * base_evade_gap);

	// Marginal power gain per training level (averaged across attributes).
	// Each attribute's marginal gain:
	//   Δattack_power = attack_incr × health / (def_gap × evade_gap)
	//   Δhealth_power = attack × health_incr / (def_gap × evade_gap)
	//   Δdefense_power = attack × health × defense_incr / (def_gap² × evade_gap)
	//   Δevade_power = attack × health × evade_incr / (def_gap × evade_gap²)
	// Average across the 4 trainable attributes:
	int64_t marginal_power = 0;
	int32_t trainable_attrs = 0;
	if (attack_incr > 0 && soldier_descr != nullptr &&
	    soldier_descr->get_max_attack_level() > 0) {
		marginal_power += static_cast<int64_t>(attack_incr) *
		   base_health * 1000 / (base_defense_gap * base_evade_gap);
		++trainable_attrs;
	}
	if (health_incr > 0 && soldier_descr != nullptr &&
	    soldier_descr->get_max_health_level() > 0) {
		marginal_power += static_cast<int64_t>(base_attack) *
		   health_incr * 1000 / (base_defense_gap * base_evade_gap);
		++trainable_attrs;
	}
	if (defense_incr > 0 && soldier_descr != nullptr &&
	    soldier_descr->get_max_defense_level() > 0) {
		marginal_power += static_cast<int64_t>(base_attack) *
		   base_health * defense_incr * 1000 /
		   (static_cast<int64_t>(base_defense_gap) * base_defense_gap * base_evade_gap);
		++trainable_attrs;
	}
	if (evade_incr > 0 && soldier_descr != nullptr &&
	    soldier_descr->get_max_evade_level() > 0) {
		marginal_power += static_cast<int64_t>(base_attack) *
		   base_health * evade_incr * 1000 /
		   (static_cast<int64_t>(base_defense_gap) * base_evade_gap * base_evade_gap);
		++trainable_attrs;
	}
	if (trainable_attrs > 0) {
		marginal_power /= trainable_attrs;
	}

	// --- Training ROI ---
	// Marginal training value = Δpower / base_power × land_defended × land_value
	// land_defended = avg garrison size × conquer area
	// Simplified: use total military capacity × land_value_per_field_
	int32_t total_mil_capacity = 0;
	for (const MilitarySiteObserver& mso : militarysites) {
		total_mil_capacity += mso.site->soldier_control()->soldier_capacity();
	}

	// Training value: how much is one level-up worth?
	// = (Δpower / base_power) × soldiers_that_benefit × land_value
	// Soldiers that benefit: all soldiers at below max level.
	// Conservative estimate: total capacity (all could benefit).
	const int32_t training_value = (base_power > 0) ?
	   static_cast<int32_t>(
	      marginal_power * std::max<int32_t>(1, total_mil_capacity) *
	      std::max<int32_t>(1, land_value_per_field_) /
	      std::max<int64_t>(1, base_power)) :
	   0;

	// --- Training cost: sum of input ware pressures for training sites ---
	// Each training cycle consumes specific wares (weapons, armor, food).
	// The opportunity cost = those wares' pressure (what else could
	// they be used for). Higher ware pressure → more expensive to train.
	int32_t training_cost = 0;
	for (const BuildingObserver& bo : buildings_) {
		if (bo.type != BuildingObserver::Type::kTrainingsite) {
			continue;
		}
		for (const auto& input : bo.inputs) {
			if (static_cast<size_t>(input) < ware_pressure_.size()) {
				training_cost += ware_pressure_[input].outputControl;
			}
		}
	}
	// Normalize: divide by nr_wares to get cost in avg_wp units
	const int32_t nr_wares_ms = std::max<int32_t>(1,
	   static_cast<int32_t>(wares.size()));
	training_cost /= nr_wares_ms;

	// --- Recruiting value ---
	// Each new soldier fills a vacant slot, defending land immediately.
	// Value = slots_to_fill × base_power_contribution × land_value
	int32_t vacant_positions = 0;
	int32_t garrison_deficit = 0;
	int32_t stock_surplus = 0;

	for (const MilitarySiteObserver& mso : militarysites) {
		const int32_t cap = mso.site->soldier_control()->soldier_capacity();
		const int32_t present = mso.site->soldier_control()->present_soldiers().size();
		if (cap > present) {
			vacant_positions += cap - present;
		}
	}

	for (const WarehouseSiteObserver& wh : warehousesites) {
		const uint32_t wh_associated = wh.site->soldier_control()->associated_soldiers().size();
		if (wh_associated < wh.site->get_desired_soldier_count()) {
			garrison_deficit += wh.site->get_desired_soldier_count() - wh_associated;
		} else {
			stock_surplus += wh_associated - wh.site->get_desired_soldier_count();
		}
	}

	// Baseline recruiting: always maintain at least 1 per military
	// capacity in reserve. Soldiers are needed BEFORE enemies appear.
	int32_t base_recruiting = 1;
	for (const MilitarySiteObserver& mso : militarysites) {
		base_recruiting += mso.site->soldier_control()->soldier_capacity();
	}
	base_recruiting = std::max<int32_t>(1, base_recruiting - stock_surplus);

	const int32_t slots_needed = std::max<int32_t>(
	   base_recruiting, vacant_positions + garrison_deficit);

	// Recruiting value = slots × land_value (each soldier defends land).
	// More slots empty = higher value per recruit.
	const int32_t recruiting_value = slots_needed *
	   std::max<int32_t>(1, land_value_per_field_);

	// Recruiting cost: barracks input ware pressures.
	int32_t recruiting_cost = 0;
	for (const BuildingObserver& bo : buildings_) {
		if (!bo.is(BuildingAttribute::kBarracks)) {
			continue;
		}
		for (const auto& input : bo.inputs) {
			if (static_cast<size_t>(input) < ware_pressure_.size()) {
				recruiting_cost += ware_pressure_[input].outputControl;
			}
		}
	}
	recruiting_cost /= nr_wares_ms;

	// --- ROI-based split ---
	// training_roi = training_value / max(1, training_cost)
	// recruiting_roi = recruiting_value / max(1, recruiting_cost)
	// Split proportional to ROI.
	//
	// When training ROI >> recruiting ROI: most pressure goes to training
	// (soldiers are weak, wares are plentiful, land is at risk).
	// When recruiting ROI >> training ROI: most pressure goes to recruiting
	// (many empty slots, or training wares are very scarce).
	//
	// Scale by max_total_levels: a tribe with 16 trainable levels
	// benefits more from training than one with 4 levels.
	const int32_t scaled_training_value = training_value *
	   std::max<int32_t>(1, max_total_levels) / 4;

	const int64_t training_roi = static_cast<int64_t>(scaled_training_value) *
	   1000 / std::max<int32_t>(1, training_cost);
	const int64_t recruiting_roi = static_cast<int64_t>(recruiting_value) *
	   1000 / std::max<int32_t>(1, recruiting_cost);

	const int64_t total_roi = training_roi + recruiting_roi + 1;

	training_pressure_ = static_cast<int32_t>(
	   static_cast<int64_t>(military_pressure_) * training_roi / total_roi);
	recruiting_pressure_ = military_pressure_ - training_pressure_;

	// Safety: both must be non-negative
	if (training_pressure_ < 0) {
		training_pressure_ = 0;
		recruiting_pressure_ = military_pressure_;
	}
	if (recruiting_pressure_ < 0) {
		recruiting_pressure_ = 0;
		training_pressure_ = military_pressure_;
	}
}

// Bully weight interface
void PlannerAI::set_bully_weight(Widelands::PlayerNumber pn, int32_t weight) {
	bully_weights_[pn] = weight;
}

int32_t PlannerAI::get_bully_weight(Widelands::PlayerNumber pn) const {
	auto it = bully_weights_.find(pn);
	return (it != bully_weights_.end()) ? it->second : 50;
}

// Production statistics aggregation
void PlannerAI::update_production_stats() {
	for (BuildingObserver& bo : buildings_) {
		if (bo.type != BuildingObserver::Type::kProductionsite &&
		    bo.type != BuildingObserver::Type::kMine) {
			continue;
		}
		bo.current_stats = 0;
		bo.unoccupied_count = 0;
		bo.unconnected_count = 0;
	}

	for (const ProductionSiteObserver& pso : productionsites) {
		if (pso.bo == nullptr || pso.site == nullptr) {
			continue;
		}
		pso.bo->current_stats += pso.site->get_statistics_percent();
		if (!pso.site->can_start_working()) {
			++pso.bo->unoccupied_count;
		}
		if (pso.site->get_economy(Widelands::wwWORKER)->warehouses().empty()) {
			++pso.bo->unconnected_count;
		}
	}
	for (const ProductionSiteObserver& mso : mines_) {
		if (mso.bo == nullptr || mso.site == nullptr) {
			continue;
		}
		mso.bo->current_stats += mso.site->get_statistics_percent();
		if (!mso.site->can_start_working()) {
			++mso.bo->unoccupied_count;
		}
		if (mso.site->get_economy(Widelands::wwWORKER)->warehouses().empty()) {
			++mso.bo->unconnected_count;
		}
	}

	for (BuildingObserver& bo : buildings_) {
		if (bo.type != BuildingObserver::Type::kProductionsite &&
		    bo.type != BuildingObserver::Type::kMine) {
			continue;
		}
		const int32_t working =
		   std::max<int32_t>(1, static_cast<int32_t>(bo.cnt_built) -
		                           static_cast<int32_t>(bo.unconnected_count));
		if (working > 0) {
			bo.current_stats /= working;
		}
	}
}

// Ware target scaling — placeholder for future implementation
// The actual API to set ware targets needs to be identified.
void PlannerAI::review_ware_targets() {
	// Currently a no-op. Economy-level ware targets are managed by the game
	// engine defaults. Future: scale targets based on economy size.
}

// =====================================================================
// Construction loop
// =====================================================================

bool PlannerAI::construct_building(const Time& gametime) {
	if (buildable_fields.empty()) {
		return false;
	}

	const Widelands::Map& map = game().map();

	// Remove expired blocked fields
	blocked_fields.remove_expired(gametime);

	// --- Run all three PI circles ---
	update_expansion_pressures(gametime);
	update_military_split();
	update_ware_pressures(gametime);
	update_building_pressures(gametime);

	verb_log_info_time(gametime,
	   "P%u PID: tick=%u mil=%d train=%d recruit=%d land_val=%d\n",
	   static_cast<unsigned>(player_number()), pi_tick_count_,
	   military_pressure_, training_pressure_, recruiting_pressure_,
	   land_value_per_field_);

	// First call: run PI circles 10 times to converge before building.
	// Each iteration propagates chain pressure 2 levels deeper.
	// After 10 iterations, an 8-deep production chain is fully propagated
	// and the system has a stable priority ranking for all building types.
	// The warmup ticks (1-10) use equal building seed instead of game goal.
	if (pi_tick_count_ <= 1) {
		verb_log_info_time(gametime,
		   "P%u: running 9 additional PI warmup iterations\n",
		   static_cast<unsigned>(player_number()));
		for (int i = 0; i < 9; ++i) {
			update_expansion_pressures(gametime);
			update_military_split();
			update_ware_pressures(gametime);
			update_building_pressures(gametime);
		}
		// pi_tick_count_ is now 10, next normal call makes it 11
	}

	// Construction limit for economy buildings (military is independent)
	const uint32_t economy_size = productionsites.size() + mines_.size();
	uint32_t max_construction = economy_size / 5 + 2;

	// Worker scarcity throttle: if many existing buildings lack workers,
	// building more only makes things worse. Each unoccupied building
	// needs a worker that needs tools that need materials — adding more
	// buildings amplifies the scarcity. Reduce construction limit by
	// the number of unoccupied buildings beyond a threshold.
	// Threshold = 2 (some unoccupied is normal during transitions).
	{
		uint32_t total_unoccupied = 0;
		for (const BuildingObserver& bo : buildings_) {
			if (bo.type == BuildingObserver::Type::kProductionsite ||
			    bo.type == BuildingObserver::Type::kMine) {
				total_unoccupied += bo.unoccupied_count;
			}
		}
		if (total_unoccupied > 2 && max_construction > 1) {
			max_construction = std::max<uint32_t>(1,
			   max_construction - (total_unoccupied - 2));
		}
	}

	// Debug: log top 8 building decisions (sorted by effective = PRO - CONTRA)
	{
		std::vector<std::pair<int32_t, size_t>> sorted_bp;
		for (size_t bi = 0; bi < buildings_.size() && bi < building_pressure_.size(); ++bi) {
			const int32_t pro = building_pressure_[bi].outputControl;
			const int32_t contra = (bi < building_prevention_.size()) ?
			   building_prevention_[bi].outputControl : 0;
			const int32_t effective = pro - contra;
			if (pro > 0) {
				sorted_bp.emplace_back(effective, bi);
			}
		}
		std::sort(sorted_bp.begin(), sorted_bp.end(), std::greater<>());
		verb_log_info_time(gametime,
		   "P%u BUILD DECISIONS (fields=%zu mines=%zu spots=%d constr=%u econ=%u):\n",
		   static_cast<unsigned>(player_number()), buildable_fields.size(),
		   mineable_fields.size(), spots_, numof_psites_in_constr, economy_size);
		for (size_t i = 0; i < std::min<size_t>(8, sorted_bp.size()); ++i) {
			const size_t bi = sorted_bp[i].second;
			const BuildingObserver& bo = buildings_[bi];
			const int32_t pro = building_pressure_[bi].outputControl;
			const int32_t contra = (bi < building_prevention_.size()) ?
			   building_prevention_[bi].outputControl : 0;
			verb_log_info_time(gametime,
			   "  P%u BP#%zu %s: eff=%d (pro=%d contra=%d) built=%u constr=%u\n",
			   static_cast<unsigned>(player_number()),
			   i + 1, bo.name, pro - contra, pro, contra,
			   bo.cnt_built, bo.cnt_under_construction);
		}
	}

	// --- Transfer Circle 2 pressures to BuildingObservers ---
	// Effective score = PRO - CONTRA. Negative → don't build.
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		BuildingObserver& bo = buildings_[bi];
		const int32_t pro =
		   (bi < building_pressure_.size()) ? building_pressure_[bi].outputControl : 0;
		const int32_t contra =
		   (bi < building_prevention_.size()) ? building_prevention_[bi].outputControl : 0;
		const int32_t effective = pro - contra;

		bo.add_new_building_score = std::max<int32_t>(0, effective);

		if (effective > 0 && bo.buildable(*player_) &&
		    bo.aimode_limit_status() == AiModeBuildings::kAnotherAllowed) {
			bo.new_building = BuildingNecessity::kNeeded;

			// Material availability gate.
			//
			// Production/mine buildings: HARD GATE. Don't start construction
			// unless stock covers the full buildcost. Starting a building
			// you can't finish drains CM from other sites and deadlocks
			// the economy. Wait until CM reproduction fills the stock.
			//
			// Exception: first instance (total_count == 0) keeps a 25%
			// floor for bootstrap ("need wood to build woodcutter").
			//
			// Military buildings: SOFT GATE with 25% floor. They must
			// always be considered for placement so the per-field integral
			// can accumulate. CM will arrive during construction.
			const Widelands::Buildcost& cost = bo.desc->buildcost();
			if (!cost.empty()) {
				Widelands::Quantity total_needed = 0;
				Widelands::Quantity total_available = 0;
				for (const auto& item : cost) {
					total_needed += item.second;
					const uint32_t stock = calculate_stocklevel(item.first);
					total_available += std::min<uint32_t>(stock, item.second);
				}
				if (total_needed > 0) {
					if (bo.type == BuildingObserver::Type::kMilitarysite) {
						// Military: soft gate, floor at 25%.
						const int32_t numerator_mil =
						   1 + 3 * static_cast<int32_t>(total_available) /
						         static_cast<int32_t>(total_needed);
						bo.add_new_building_score =
						   bo.add_new_building_score * numerator_mil / 4;
					} else if (total_available < total_needed) {
						// Production/mine: stock doesn't cover buildcost.
						if (bo.total_count() == 0) {
							// Bootstrap: first instance, allow at 25%.
							// Supply_score handles chain ordering — a
							// weaponsmithy with no input producers gets
							// supply=1% which drops it below basic buildings.
							const int32_t numerator =
							   1 + 3 * static_cast<int32_t>(total_available) /
							         static_cast<int32_t>(total_needed);
							bo.add_new_building_score =
							   bo.add_new_building_score * numerator / 4;
						} else {
							// Duplicate: hard gate. Don't build a 2nd
							// instance unless stock covers full buildcost.
							bo.add_new_building_score = 0;
						}
					}
					// else: full stock available, score unchanged.
				}
				bo.build_material_shortage = (total_available < total_needed);
			}

			// Input chain readiness: scale by supply score from 3-part distribution.
			// A building whose production inputs have no producers yet (weaponsmithy
			// without iron smelter) gets supply_score near 0 → score drops to near 0.
			// A building with satisfied inputs gets supply_score = 1000 → full score.
			// This prevents building deep-chain buildings before their suppliers exist.
			// Floor at 1/10 (100/1000): even with 0 supply, don't completely zero out
			// to allow the PI system to keep accumulating pressure for when suppliers
			// are eventually built.
			if ((bo.type == BuildingObserver::Type::kProductionsite ||
			     bo.type == BuildingObserver::Type::kMine) &&
			    bi < building_supply_score_.size() && !bo.inputs.empty()) {
				// Floor of 10 (1%): buildings with NO input producers are
				// nearly blocked. This enforces chain build order:
				// shepherds → spinning_mill → weaving_mill, not backwards.
				// 1% lets the integral accumulate slowly so the building
				// is eventually picked once suppliers appear.
				const int32_t supply = std::max<int32_t>(10, building_supply_score_[bi]);
				bo.add_new_building_score =
				   bo.add_new_building_score * supply / 1000;
			}

			// Respect construction interval for production sites
			if ((bo.type == BuildingObserver::Type::kProductionsite ||
			     bo.type == BuildingObserver::Type::kMine) &&
			    gametime - bo.construction_decision_time < kBuildingMinInterval &&
			    !bo.is_resource_harvester) {
				bo.new_building = BuildingNecessity::kForbidden;
			}
		} else {
			bo.new_building = BuildingNecessity::kNotNeeded;
			bo.add_new_building_score = 0;
		}
	}

	// --- Spot size value estimation ---
	//
	// Big building spots are scarce and can host more valuable buildings.
	// Placing a small building on a big spot wastes the potential for a
	// medium or big building that could produce more value.
	//
	// Compute per-size max building pressure:
	//   small_max = max pressure of any needed small building
	//   medium_max = max pressure of any needed medium building
	//   big_max = max pressure of any needed big building
	//
	// Cumulative spot value: a big spot can host ANY size, so its value
	// is the sum: small_max + medium_max + big_max.
	// A small building on a big spot wastes (medium_max + big_max).
	//
	// This is analogous to land_value_per_field_ for military: a big
	// spot has strategic value from the buildings it COULD support.
	int32_t max_pressure_by_size[4] = {0, 0, 0, 0};  // index = size (1..3)
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		const BuildingObserver& bo = buildings_[bi];
		if (bo.type == BuildingObserver::Type::kMilitarysite) {
			continue;
		}
		if (bo.desc->get_ismine()) {
			continue;  // mines have separate spots
		}
		if (bo.add_new_building_score <= 0) {
			continue;
		}
		const int32_t sz = static_cast<int32_t>(bo.desc->get_size());
		if (sz >= 1 && sz <= 3) {
			max_pressure_by_size[sz] =
			   std::max(max_pressure_by_size[sz], bo.add_new_building_score);
		}
	}
	// Cumulative: spot_value[N] = sum of best pressures for sizes 1..N
	int32_t spot_value[4] = {0, 0, 0, 0};
	spot_value[1] = max_pressure_by_size[1];
	spot_value[2] = spot_value[1] + max_pressure_by_size[2];
	spot_value[3] = spot_value[2] + max_pressure_by_size[3];

	// --- Two independent construction chains ---
	// Chain 1: Economy (production, mine, warehouse, training)
	// Chain 2: Military (military buildings)
	// Each chain picks its own winner independently.
	// This avoids the dilemma "economy or military next?" — both happen.
	BuildingObserver* best_economy = nullptr;
	int32_t economy_priority = 0;
	Widelands::Coords economy_coords;

	BuildingObserver* best_military = nullptr;
	// Minimum threshold for military placement. The integral must
	// accumulate over several ticks before triggering a build.
	// This prevents the one-tick spike when a nearby construction
	// finishes: military_in_constr_nearby drops to 0, one tick of
	// positive efficiency makes integral > 0, and without a threshold
	// that's enough to place a redundant building immediately.
	// avg_wp/4 ≈ 83K (for 30 wares) requires ~2-3 ticks of sustained
	// positive efficiency before the spot qualifies.
	const int32_t military_min_threshold =
	   wares.empty() ? 0 :
	   kNormalizationBudget / (4 * static_cast<int32_t>(wares.size()));
	int32_t military_priority = military_min_threshold;
	Widelands::Coords military_coords;

	// Track which harvester building types found at least one field
	// with sufficient resources during the placement loop. Harvesters
	// that find NO suitable field get a stronger ipart penalty in the
	// anti-windup section (ipart/2 instead of 7/8) to quickly back off
	// and let other buildings compete.
	std::vector<bool> harvester_has_resource_field(buildings_.size(), false);

	// Pass 1: buildable fields (non-mine buildings)
	for (BuildableField* const bf : buildable_fields) {
		if (bf->field_info_expiration < gametime) {
			continue;
		}
		if (blocked_fields.is_blocked(bf->coords)) {
			continue;
		}

		const int32_t maxsize = player_->get_buildcaps(bf->coords) & Widelands::BUILDCAPS_SIZEMASK;

		for (BuildingObserver& bo : buildings_) {
			if (!bo.buildable(*player_)) {
				continue;
			}
			if (bo.desc->get_ismine()) {
				continue;
			}
			if (bo.new_building == BuildingNecessity::kNotNeeded ||
			    bo.new_building == BuildingNecessity::kForbidden) {
				continue;
			}
			if (bo.desc->get_size() > maxsize) {
				continue;
			}
			if (bo.add_new_building_score <= 0) {
				continue;
			}
			if (bo.cnt_under_construction >= 2 &&
			    bo.type != BuildingObserver::Type::kMilitarysite) {
				continue;
			}

			int32_t prio = bo.add_new_building_score;

			// Supporter/same-building counts for placement
			uint8_t number_of_supporters_nearby = 0;
			if (bf->supporters_nearby.count(bo.name) > 0) {
				number_of_supporters_nearby = bf->supporters_nearby.at(bo.desc->name());
			}
			uint8_t number_of_supported_producers_nearby = 0;
			{
				auto it = bf->supported_producers_nearby.find(bo.id);
				if (it != bf->supported_producers_nearby.end()) {
					number_of_supported_producers_nearby = it->second;
				}
			}
			uint8_t number_of_same_nearby = 0;
			{
				auto it = bf->buildings_nearby.find(bo.id);
				if (it != bf->buildings_nearby.end()) {
					number_of_same_nearby = it->second;
				}
			}

			// === Resource-based field scoring ===
			//
			// All placement bonuses/penalties are expressed as fractions of
			// `prio` (the building pressure from Circle 2). This ensures
			// placement scoring scales with how urgently the building is
			// needed — no absolute magic numbers.
			//
			// Resource scoring: each nearby resource unit adds prio/N,
			// where N is the typical resource count in a good field.
			// A perfect field (N resources) doubles prio. A bare field
			// adds nothing. This makes placement a tiebreaker within
			// the priority class, not an override.
			//
			// Competition penalty: each nearby same-type building subtracts
			// prio/4 — at 4 competitors, the field is effectively blocked
			// (prio reduced to 0 from competition alone).
			//
			// Supporter bonus: each nearby supporter adds prio/5 (20% boost).
			// A field with 3 supporters gets +60% — strong pull toward the
			// supported area.
			//
			// Port protection: -prio × 3 near port spaces. This means
			// the building would need 4× its normal priority to overcome
			// the penalty — effectively forbidden without being a hard gate.

			if (bo.is(BuildingAttribute::kWell)) {
				if (bf->ground_water < 2) {
					continue;
				}
				// Resource bonus: more groundwater = better well placement.
				// Typical good groundwater: ~10. So prio/10 per unit.
				prio += bf->ground_water * std::max<int32_t>(1, prio / 10);
				if (number_of_same_nearby > 2) {
					continue;
				}

			} else if (bo.is(BuildingAttribute::kFisher)) {
				if (bf->fish_nearby <= 15) {
					continue;
				}
				// Resource bonus: prio/20 per fish. 20 fish = double prio.
				prio += bf->fish_nearby * std::max<int32_t>(1, prio / 20);
				// Competition: prio/4 per competitor (fish deplete).
				prio -= number_of_same_nearby * std::max<int32_t>(1, prio / 4);
				// Supporter bonus: prio/5 per fishbreeder (renewable).
				prio += number_of_supporters_nearby * std::max<int32_t>(1, prio / 5);

			} else if (bo.is_resource_harvester) {
				// Generic resource harvester placement (woodcutter, quarry,
				// hunter, branch collector, and any modded building whose
				// worker uses findobject to find map objects by attribute).
				//
				// For each resource target, look up the weighted count
				// from the field scan. If below threshold → skip field.
				// Resource bonus = count * prio / saturation.
				// Collected resources: competition penalty (1 + same_nearby).
				// Non-collected (just visited): milder competition.
				bool any_below_threshold = false;
				int32_t resource_bonus = 0;
				bool has_collected_immovable = false;
				for (const auto& rt : bo.resource_targets) {
					auto it = bf->resource_count_by_attribute.find(rt.attribute_id);
					const uint16_t count = (it != bf->resource_count_by_attribute.end()) ?
					   it->second : 0;
					if (count < rt.threshold) {
						any_below_threshold = true;
						break;
					}
					// Resource bonus: count * prio / saturation.
					// At saturation resources, bonus = prio (doubles score).
					const int32_t sat = std::max<int32_t>(1, rt.saturation);
					int32_t per_unit = std::max<int32_t>(1, prio / sat);
					if (rt.is_collected) {
						// Collected resources: divide by competition.
						per_unit = per_unit / (1 + number_of_same_nearby);
						has_collected_immovable =
						   has_collected_immovable ||
						   (rt.kind == ResourceSearchTarget::Kind::kImmovableAttribute);
					}
					resource_bonus += count * per_unit;
				}
				if (any_below_threshold) {
					continue;
				}
				prio += resource_bonus;
				// Supporter bonus: prio/5 per supporter nearby.
				prio += number_of_supporters_nearby * std::max<int32_t>(1, prio / 5);
				// Space consumer penalty for buildings that harvest immovables
				// (space consumers destroy immovables the harvester needs).
				if (has_collected_immovable) {
					prio -= bf->space_consumers_nearby *
					   std::max<int32_t>(1, tribe_has_ranger_ ? prio / 10 : prio / 2);
				}

			} else if (bo.is(BuildingAttribute::kRanger)) {
				// Supporter coupling: each nearby woodcutter adds prio/5.
				prio += number_of_supported_producers_nearby * std::max<int32_t>(1, prio / 5);
				// Rangers can cluster (forests overlap beneficially).
				prio += number_of_same_nearby * std::max<int32_t>(1, prio / 20);
				// Water penalty: can't plant on water tiles. prio/100 per tile.
				prio -= bf->water_nearby * std::max<int32_t>(1, prio / 100);
				// Space consumer penalty: they destroy planted trees.
				prio -= bf->space_consumers_nearby * std::max<int32_t>(1, prio / 10);
				// Port protection: -3× prio near port spaces.
				if (bf->portspace_nearby == ExtendedBool::kTrue ||
				    bf->unowned_portspace_vicinity_nearby > 0) {
					prio -= prio * 3;
				}
				// Rock penalty: rocks occupy tiles where trees could grow.
				const uint8_t rocks_nearby =
				   bf->immovables_by_attribute_nearby.count(BuildingAttribute::kNeedsRocks) > 0 ?
				      bf->immovables_by_attribute_nearby.at(BuildingAttribute::kNeedsRocks) : 0;
				prio -= rocks_nearby * std::max<int32_t>(1, prio / 30);

			} else if (bo.is(BuildingAttribute::kNeedsCoast)) {
				if (bf->water_nearby <= 0) {
					continue;
				}
				// Resource bonus: prio/20 per water tile.
				prio += bf->water_nearby * std::max<int32_t>(1, prio / 20);

			} else if (!bo.supported_producers.empty() &&
			           bo.is(BuildingAttribute::kSupportingProducer)) {
				// Supporting producers (gamekeepers, fish breeders):
				// prio/5 per nearby supported building.
				prio += number_of_supported_producers_nearby * std::max<int32_t>(1, prio / 5);
				// Competition: prio/4 per same nearby.
				prio -= number_of_same_nearby * std::max<int32_t>(1, prio / 4);
				// Trees and rangers block their work area.
				const uint8_t trees_nearby = std::max(
				   bf->immovables_by_attribute_nearby.count(BuildingAttribute::kLumberjack) > 0 ?
				      bf->immovables_by_attribute_nearby.at(BuildingAttribute::kLumberjack) : 0,
				   bf->immovables_by_attribute_nearby.count(BuildingAttribute::kRanger) > 0 ?
				      bf->immovables_by_attribute_nearby.at(BuildingAttribute::kRanger) : 0);
				prio -= trees_nearby * std::max<int32_t>(1, prio / 30);
				prio -= bf->rangers_nearby * std::max<int32_t>(1, prio / 10);
				if (bf->enemy_nearby) {
					prio -= prio / 5;
				}
				// Port protection.
				if (bo.is(BuildingAttribute::kSpaceConsumer) &&
				    bf->unowned_portspace_vicinity_nearby > 0) {
					prio -= prio * 3;
				}
			}

			// Mark harvesters that passed the resource threshold on this field.
			// If a harvester reaches this point, it found at least one field
			// with sufficient resources.
			if (bo.is_resource_harvester || bo.is(BuildingAttribute::kFisher)) {
				harvester_has_resource_field[&bo - buildings_.data()] = true;
			}

			// Space consumer placement (for non-ranger, non-specific)
			if (bo.is(BuildingAttribute::kSpaceConsumer) &&
			    !bo.is(BuildingAttribute::kRanger) &&
			    !bo.is(BuildingAttribute::kSupportingProducer)) {
				// Clearing bonus: proportional to obstacle count and space scarcity.
				// At no scarcity: prio/15 per tree (comparable to woodcutter).
				// At scarcity: prio/15 + scarcity per tree → scales up.
				const uint8_t trees_here =
				   bf->immovables_by_attribute_nearby.count(BuildingAttribute::kLumberjack) > 0 ?
				      bf->immovables_by_attribute_nearby.at(BuildingAttribute::kLumberjack) : 0;
				const int32_t clear_economy_sz =
				   static_cast<int32_t>(productionsites.size() + mines_.size());
				const int32_t clear_desired = clear_economy_sz / 2 + 5;
				const int32_t clear_scarcity =
				   std::max<int32_t>(0, clear_desired - spots_);
				const int32_t per_tree_clear =
				   std::max<int32_t>(1, prio / 15) + clear_scarcity;
				prio += trees_here * per_tree_clear;

				// Diminishing returns with multiple space consumers nearby.
				if (bf->space_consumers_nearby > 2) {
					prio /= bf->space_consumers_nearby;
				}
				// Near lumberjack: clearing destroys trees lumberjack needs.
				// With ranger: prio/5 per nearby lumberjack (recoverable).
				// Without ranger: prio per lumberjack (permanent damage).
				for (const auto& [bid, count] : bf->buildings_nearby) {
					for (const BuildingObserver& nearby_bo : buildings_) {
						if (nearby_bo.id == bid && nearby_bo.is(BuildingAttribute::kLumberjack)) {
							prio -= count * std::max<int32_t>(1,
							   tribe_has_ranger_ ? prio / 5 : prio);
							break;
						}
					}
				}
			}

			// Generic production sites: spread-out penalty.
			// Space consumers (farms, etc.) compete for land. Each space consumer
			// occupies an area roughly equal to one building's work area, so at
			// nr_wares_int/2 space consumers, half the local capacity is used and
			// the penalty equals prio → effective priority halved.
			// Non-military buildings cause road congestion. At nr_wares_int
			// neighbors, the area is a full production cluster (one building per
			// ware type) and the penalty equals prio.
			if (bo.type == BuildingObserver::Type::kProductionsite &&
			    !bo.is_resource_harvester &&
			    !bo.is(BuildingAttribute::kRanger) &&
			    !bo.is(BuildingAttribute::kFisher)) {
				const int32_t nr_wares_local = std::max<int32_t>(
				   1, static_cast<int32_t>(wares.size()));
				// Space consumer density: at nr_wares/2 consumers, penalty = prio.
				prio -= bf->space_consumers_nearby *
				   std::max<int32_t>(1, prio * 2 / nr_wares_local);
				// General density: at nr_wares neighbors, penalty = prio.
				prio -= bf->own_non_military_nearby *
				   std::max<int32_t>(1, prio / nr_wares_local);
			}

			// === Big spot waste penalty ===
			//
			// A small building on a big spot wastes the potential for a
			// higher-value building. Penalize proportional to the wasted
			// value: prio × (spot_value[maxsize] - spot_value[bld_size])
			//                / spot_value[maxsize].
			// Example: if big buildings are urgently needed, placing a
			// small house on a big spot loses up to 2/3 of its priority.
			// If no big buildings are needed, no penalty (spot_value[3] ==
			// spot_value[1] when medium_max and big_max are 0).
			if (bo.type != BuildingObserver::Type::kMilitarysite &&
			    !bo.desc->get_ismine()) {
				const int32_t bld_size = static_cast<int32_t>(bo.desc->get_size());
				if (maxsize > bld_size && bld_size >= 1 && bld_size <= 3 &&
				    maxsize <= 3 && spot_value[maxsize] > 0) {
					const int32_t wasted =
					   spot_value[maxsize] - spot_value[bld_size];
					if (wasted > 0) {
						prio -= prio * wasted / spot_value[maxsize];
					}
				}
			}

			// === Port spot protection ===
			// Port-capable spots are rare and extremely valuable for
			// seafaring expansion. A farm or branch harvester on a port
			// spot wastes a strategic resource that can't be recovered
			// (dismantling loses materials and time). Penalty = prio × 5
			// → the building needs 6× normal priority to overcome this.
			// Port buildings themselves are exempt.
			if (bo.type != BuildingObserver::Type::kMilitarysite &&
			    !bo.is(BuildingAttribute::kPort) &&
			    bf->portspace_nearby == ExtendedBool::kTrue) {
				prio -= prio * 5;
			}

			// === Border reservation (priority inheritance from expansion goal) ===
			// Contract: border spots serve the expansion goal. Non-military
			// buildings at the border compete against the expansion need:
			//
			// 1. Hard skip: no military coverage at all → spot is reserved
			//    for the first military building to claim this frontier.
			//    Also hard skip spots with significant unowned land nearby:
			//    these are frontier spots needed for military expansion.
			// 2. Soft penalty: unowned land nearby → penalize proportional
			//    to expansion pressure AND unowned land count. Each unowned
			//    field nearby is land a military building could conquer.
			//    Penalty = unowned_count × expansion_pressure × prio / budget.
			//    At 20 unowned fields and full expansion: penalty ≈ 2× prio.
			//
			// The penalty derives from the root objective: expansion pressure
			// (Circle 3) flows through to placement decisions here.
			if (bo.type != BuildingObserver::Type::kMilitarysite) {
				// Only reserve border spots when military buildings exist
				// SOMEWHERE. At game start with no military, all spots must
				// be available for economy — otherwise deadlock: economy
				// blocked by "no coverage", military by "no materials".
				bool any_military_exists = false;
				for (const BuildingObserver& mbo : buildings_) {
					if (mbo.type == BuildingObserver::Type::kMilitarysite &&
					    (mbo.cnt_built + mbo.cnt_under_construction) > 0) {
						any_military_exists = true;
						break;
					}
				}
				if (any_military_exists &&
				    bf->own_military_presence == 0 && bf->military_in_constr_nearby == 0 &&
				    (bf->near_border || bf->unowned_land_nearby > 3)) {
					continue;  // Hard skip: no military coverage
				}
				// Strong penalty: unowned land nearby means this spot is
				// at the frontier. Economy buildings here steal military
				// expansion potential. Scale with BOTH unowned count and
				// expansion pressure: more unowned land = more valuable
				// frontier, higher expansion pressure = more urgency.
				if (bf->unowned_land_nearby > 0 && !expansion_targets_.empty()) {
					const int32_t exp_pressure =
					   std::max<int32_t>(0, expansion_targets_[0].outputControl);
					// Penalty per unowned field = prio × exp_pressure / budget.
					// At full budget: each field costs prio/10M × 10M = prio.
					// At 10 fields: penalty = 10 × prio = 10× prio → skip.
					// This effectively reserves frontier spots for military.
					prio -= static_cast<int32_t>(
					   static_cast<int64_t>(bf->unowned_land_nearby) *
					   prio * exp_pressure / (kNormalizationBudget + 1));
				}
				// Enemy land nearby: even stronger reservation.
				// Military buildings here attack/defend — economy buildings don't.
				if (bf->enemy_nearby) {
					prio -= prio;  // halve effective priority
				}
			}

			// Military buildings: per-spot PI with land value / cost scoring.
			//
			// Each border field accumulates its own military integral.
			// The error signal is the field's efficiency (land_value / cost).
			// When the integral reaches a threshold, the spot triggers a build.
			//
			// If circumstances change (another building finishes nearby,
			// reducing this spot's military_score_), the error drops and
			// the integral decays → the plan naturally adapts. This prevents
			// building on ALL border spots simultaneously.
			//
			// Land gain is valued in ware-pressure units using
			// land_value_per_field_. Cost includes materials at their
			// current ware pressure plus garrison soldiers.
			if (bo.type == BuildingObserver::Type::kMilitarysite) {
				// Freeze integral while a military construction site is nearby.
				// The construction site is already claiming the overlapping land.
				// Don't let the integral accumulate — just decay it.
				// Once the construction finishes, update_buildable_field will
				// recompute military_score_ (now reflecting conquered land),
				// and the integral can start accumulating again if needed.
				// This ensures competing spots don't go positive until the
				// first building is actually finished and conquering.
				if (bf->military_in_constr_nearby > 0) {
					// Slam integral hard: nearby construction already claims
					// the overlapping land. 7/8 decay was too slow — a spot
					// with integral 100K only dropped to 50K after 5 ticks,
					// still enough to trigger a redundant tent placement.
					// Divide by 4 per tick: after 2 ticks, integral is <10%
					// of original — effectively prevents cluster building.
					// Clamp to ≤0: when the construction finishes,
					// military_in_constr_nearby drops to 0 and the integral
					// starts accumulating again. Without the clamp, a small
					// positive residual could trigger an immediate second
					// placement in the very next tick.
					bf->military_integral_ = std::min(bf->military_integral_ / 4, 0);
					continue;
				}
				if (bf->military_score_ <= 0 && !bf->near_border) {
					// Spot has no land gain → decay integral toward zero.
					bf->military_integral_ = bf->military_integral_ * 3 / 4;
					continue;
				}
				// Land gain in ware-pressure units:
				const int32_t conquer_r = static_cast<int32_t>(bo.desc->get_conquers());
				const int32_t field_gain = bf->military_score_ * conquer_r;
				const int64_t land_value =
				   static_cast<int64_t>(field_gain) *
				   std::max<int32_t>(1, land_value_per_field_);

				// === Total cost: material + time-to-conquer ===
				//
				// A military building doesn't conquer land instantly.
				// Before it starts gaining territory:
				//   1. Each ware must be carried from warehouse (N ticks each)
				//   2. All wares delivered sequentially (total_wares × N ticks)
				//   3. Construction time after delivery (proportional to size)
				//   4. Soldiers must walk to garrison
				//
				// A sentry (2 planks): 2×N + small build time → conquers fast.
				// A tower (6 planks + 4 stones): 10×N + long build → slow.
				//
				// Efficiency = land_value / (material_cost × time_factor).
				// time_factor = 1 + total_wares_needed / 2.
				// This means: a 2-ware sentry has time_factor = 2 (fast).
				// A 10-ware tower has time_factor = 6 (3× slower per unit value).
				// The tower only wins when its conquer area is large enough
				// to overcome the 3× time penalty — i.e., when a sentry
				// CAN'T reach the land (too far from border).
				//
				// Material cost: each ware weighted by its scarcity (ware pressure).
				int64_t material_cost = 0;
				int32_t total_wares_needed = 0;
				for (const auto& [ware_idx, amount] : bo.desc->buildcost()) {
					int32_t ware_cost = 1;
					if (static_cast<size_t>(ware_idx) < ware_pressure_.size()) {
						ware_cost = std::max<int32_t>(1,
						   ware_pressure_[ware_idx].outputControl);
					}
					material_cost += static_cast<int64_t>(ware_cost) * amount;
					total_wares_needed += static_cast<int32_t>(amount);
				}
				// Garrison cost:
				const Widelands::MilitarySiteDescr* ms_desc =
				   dynamic_cast<const Widelands::MilitarySiteDescr*>(bo.desc);
				const int32_t garrison_size = ms_desc != nullptr ?
				   static_cast<int32_t>(ms_desc->get_max_number_of_soldiers()) : 1;
				const int32_t avg_wp_mil =
				   wares.empty() ? 1 :
				   kNormalizationBudget / static_cast<int32_t>(wares.size());
				int32_t soldier_cost_factor = 1;
				if (soldier_status_ == SoldiersStatus::kBadShortage) {
					soldier_cost_factor = 3;
				} else if (soldier_status_ == SoldiersStatus::kShortage) {
					soldier_cost_factor = 2;
				}
				material_cost += static_cast<int64_t>(garrison_size) *
				   soldier_cost_factor * avg_wp_mil;
				material_cost = std::max<int64_t>(1, material_cost);

				// Time factor: how many ticks until this building conquers.
				// Each ware = ~1 delivery trip. Soldiers also take time.
				// time_factor = 1 + (total_wares + garrison) / 2
				// Sentry (2 wares, 1 soldier): 1 + 3/2 = 2.5
				// Tower (10 wares, 6 soldiers): 1 + 16/2 = 9
				// → Tower is 3.6× slower to become productive.
				const int32_t time_factor = std::max<int32_t>(1,
				   1 + (total_wares_needed + garrison_size) / 2);

				// Efficiency = land_value × avg_wp / (material_cost × time_factor)
				// The avg_wp scaling prevents integer division truncation:
				// without it, land_value < total_cost → efficiency = 0 always
				// (e.g., 1.1M / 1.8M = 0 in integer math). With avg_wp:
				// 1.1M × 333K / 1.8M = 200K → meaningful accumulation.
				const int64_t total_cost = material_cost * time_factor;
				const int32_t efficiency = static_cast<int32_t>(
				   std::min<int64_t>(static_cast<int64_t>(prio) * 4,
				      land_value * avg_wp_mil / std::max<int64_t>(1, total_cost)));

				// Per-spot integral accumulation (NO proportional term).
				// Priority = integral ONLY. The spot must accumulate over
				// multiple ticks before it triggers a build. This is the
				// key throttle: one tick of high efficiency is not enough.
				//
				// Decay: 7/8 per tick (slow leak). After ~8 ticks of
				// sustained positive efficiency, integral reaches ~8× error.
				// A newly slammed-negative integral needs ~8 ticks at the
				// same efficiency to recover to zero, then more to go positive.
				bf->military_integral_ =
				   bf->military_integral_ * 7 / 8 + efficiency;

				// Priority = integral only. No P term means: the FIRST
				// tick a spot becomes valuable, prio ≈ efficiency (small).
				// After 4 ticks: prio ≈ 4 × efficiency. After 8: ~8×.
				// This naturally spaces out military buildings.
				prio = bf->military_integral_;
			}

			// Route to the appropriate chain
			if (bo.type == BuildingObserver::Type::kMilitarysite) {
				if (prio > military_priority) {
					best_military = &bo;
					military_priority = prio;
					military_coords = bf->coords;
				}
			} else {
				if (prio > economy_priority) {
					best_economy = &bo;
					economy_priority = prio;
					economy_coords = bf->coords;
				}
			}
		}
	}

	// Pass 2: mineable fields (mines + mine-sized buildings)
	for (MineableField* const mf : mineable_fields) {
		if (mf->field_info_expiration <= gametime) {
			continue;
		}
		if (blocked_fields.is_blocked(mf->coords)) {
			continue;
		}

		for (BuildingObserver& bo : buildings_) {
			if (!bo.buildable(*player_)) {
				continue;
			}
			if (!bo.desc->get_ismine()) {
				continue;
			}
			if (bo.new_building == BuildingNecessity::kNotNeeded ||
			    bo.new_building == BuildingNecessity::kForbidden) {
				continue;
			}
			if (bo.add_new_building_score <= 0) {
				continue;
			}

			int32_t prio = bo.add_new_building_score;

			if (bo.mines != Widelands::INVALID_INDEX) {
				// Real mine: must match resource
				if (mf->coords.field->get_resources() != bo.mines) {
					continue;
				}
				// Score by nearby resources, excluding fields already
				// being mined by an existing mine (built or construction).
				// A resource field with a mine on it is "claimed" — building
				// another mine nearby to access the same resources is wasteful.
				Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
				   map, Widelands::Area<Widelands::FCoords>(mf->coords, 2));
				int32_t resource_score = 0;
				do {
					if (bo.mines == mr.location().field->get_resources()) {
						// Check if this resource field already has a mine on it
						bool already_mined = false;
						if (const Widelands::BaseImmovable* imm =
						       mr.location().field->get_immovable()) {
							if (imm->descr().type() == Widelands::MapObjectType::BUILDING ||
							    imm->descr().type() == Widelands::MapObjectType::CONSTRUCTIONSITE) {
								if (upcast(Widelands::Building const, bld, imm)) {
									if (bld->descr().get_ismine()) {
										already_mined = true;
									}
								}
							}
						}
						if (!already_mined) {
							resource_score += mr.location().field->get_resources_amount();
						}
					}
				} while (mr.advance(map));
				resource_score /= 10;
				if (resource_score <= 0) {
					continue;
				}
				prio += resource_score;
			} else {
				// Mine-sized non-mine: avoid wasting ore fields
				if (mf->coords.field->get_resources_amount() > 0) {
					prio -= 10;
				}
			}

			// Military mine buildings: add military score
			if (bo.type == BuildingObserver::Type::kMilitarysite) {
				// Mine-sized military buildings are handled here
				prio += mf->same_mine_fields_nearby;
			}

			// Route to the appropriate chain
			if (bo.type == BuildingObserver::Type::kMilitarysite) {
				if (prio > military_priority) {
					best_military = &bo;
					military_priority = prio;
					military_coords = mf->coords;
				}
			} else {
				if (prio > economy_priority) {
					best_economy = &bo;
					economy_priority = prio;
					economy_coords = mf->coords;
				}
			}
		}
	}

	// Pass 3: Enhancement candidates (upgrading existing buildings)
	//
	// For each building type that is an enhancement (has enhanced_from),
	// scan existing buildings of the predecessor type as upgrade candidates.
	// The "building spot" is the predecessor building itself.
	//
	// Placement scoring: the predecessor's location is evaluated using the
	// ENHANCEMENT's resource rules (trees for master woodcutter, etc.).
	// This ensures the best-located predecessor gets upgraded first.
	//
	// Competes in the economy chain: an enhancement candidate is compared
	// against economy_priority from Pass 1/2. If upgrading is more valuable
	// than building something new, the upgrade wins.
	BuildingObserver* best_enhance_bo = nullptr;
	Widelands::Building* best_enhance_site = nullptr;
	int32_t enhance_priority = 0;

	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		BuildingObserver& en_bo = buildings_[bi];
		if (en_bo.desc->enhanced_from() == Widelands::INVALID_INDEX) {
			continue;  // Not an enhancement
		}
		if (!player_->is_building_type_allowed(en_bo.id)) {
			continue;
		}
		if (en_bo.aimode_limit_status() != AiModeBuildings::kAnotherAllowed) {
			continue;
		}
		const int32_t bp_total =
		   (bi < building_pressure_.size()) ? building_pressure_[bi].outputControl : 0;
		if (bp_total <= 0) {
			continue;  // No demand for this enhancement
		}
		// Don't enhance if one is already under construction or pending
		if (en_bo.cnt_under_construction > 0 || en_bo.cnt_upgrade_pending > 0) {
			continue;
		}

		// Enhancement cost: apply the same material availability scaling.
		// Use enhancement_cost, not buildcost (enhancement has its own cost).
		int32_t en_score = bp_total;
		const Widelands::Buildcost& en_cost = en_bo.desc->enhancement_cost();
		if (!en_cost.empty()) {
			Widelands::Quantity total_needed = 0;
			Widelands::Quantity total_available = 0;
			for (const auto& item : en_cost) {
				total_needed += item.second;
				const uint32_t stock = calculate_stocklevel(item.first);
				total_available += std::min<uint32_t>(stock, item.second);
			}
			if (total_needed > 0) {
				// Same floor as economy (1/4): enhancements can be queued.
				const int32_t numerator =
				   1 + 3 * static_cast<int32_t>(total_available) /
				         static_cast<int32_t>(total_needed);
				en_score = en_score * numerator / 4;
			}
		}
		if (en_score <= 0) {
			continue;
		}

		// Find predecessor building type
		const Widelands::DescriptionIndex pred_idx = en_bo.desc->enhanced_from();
		BuildingObserver* pred_bo = nullptr;
		for (BuildingObserver& pbo : buildings_) {
			if (pbo.id == pred_idx) {
				pred_bo = &pbo;
				break;
			}
		}
		if (pred_bo == nullptr || pred_bo->cnt_built == 0) {
			continue;  // No predecessor buildings exist to upgrade
		}

		// For kUpgradeExtends: keep at least one predecessor (don't upgrade ALL)
		if (en_bo.is(BuildingAttribute::kUpgradeExtends) && pred_bo->cnt_built <= 1) {
			// Only upgrade the last one if we're building a replacement
			if (pred_bo->cnt_under_construction == 0) {
				continue;
			}
		}

		// Scan existing production sites of the predecessor type.
		// Score each by the ENHANCEMENT's resource needs at that location.
		for (ProductionSiteObserver& pso : productionsites) {
			if (pso.bo != pred_bo || pso.site == nullptr) {
				continue;
			}
			if (pso.upgrade_pending || pso.dismantle_pending_since.is_valid()) {
				continue;
			}
			// Must be connected to warehouse
			if (pso.site->get_economy(Widelands::wwWORKER)->warehouses().empty()) {
				continue;
			}
			// Worker availability: enhancement requires specific workers
			if (!pso.site->has_workers(en_bo.id, game())) {
				continue;
			}

			int32_t site_score = en_score;

			// Resource scoring at this location: evaluate the enhancement's
			// attributes at the predecessor's position. Same rules as Pass 1.
			const Widelands::Coords site_pos = pso.site->get_position();
			Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
			   map, Widelands::Area<Widelands::FCoords>(
			      map.get_fcoords(site_pos), 6));
			uint8_t trees_nearby = 0;
			uint8_t rocks_nearby = 0;
			uint8_t supporters_nearby = 0;
			uint8_t same_type_nearby = 0;
			do {
				const Widelands::FCoords loc = mr.location();
				if (const Widelands::BaseImmovable* imm = loc.field->get_immovable()) {
					if (imm->has_attribute(
					       Widelands::MapObjectDescr::get_attribute_id("tree")) ||
					    imm->has_attribute(
					       Widelands::MapObjectDescr::get_attribute_id("normal_tree"))) {
						++trees_nearby;
					}
					if (imm->has_attribute(
					       Widelands::MapObjectDescr::get_attribute_id("rocks"))) {
						++rocks_nearby;
					}
					// Check for supporters and same-type buildings
					if (upcast(Widelands::Building const, bld, imm)) {
						if (bld->owner().player_number() == player_number()) {
							if (const auto* prodsite =
							       dynamic_cast<const Widelands::ProductionSiteDescr*>(
							          &bld->descr())) {
								for (const auto& supported :
								     prodsite->supported_productionsites()) {
									if (supported == en_bo.desc->name()) {
										++supporters_nearby;
									}
								}
							}
							if (&bld->descr() == en_bo.desc) {
								++same_type_nearby;
							}
						}
					}
				}
			} while (mr.advance(map));

			// Apply resource bonuses/penalties (same logic as Pass 1)
			if (en_bo.is(BuildingAttribute::kLumberjack)) {
				site_score += trees_nearby * std::max<int32_t>(1,
				   site_score / 15 / (1 + same_type_nearby));
				site_score += supporters_nearby * std::max<int32_t>(1, site_score / 5);
			} else if (en_bo.is(BuildingAttribute::kNeedsRocks)) {
				site_score += rocks_nearby * std::max<int32_t>(1, site_score / 10);
				site_score -= same_type_nearby * std::max<int32_t>(1, site_score / 4);
			} else {
				// Generic: supporter bonus, competition penalty
				site_score += supporters_nearby * std::max<int32_t>(1, site_score / 5);
				site_score -= same_type_nearby * std::max<int32_t>(1, site_score / 4);
			}

			if (site_score > enhance_priority) {
				enhance_priority = site_score;
				best_enhance_bo = &en_bo;
				best_enhance_site = pso.site;
			}
		}

		// Also scan mines for mine enhancements
		for (ProductionSiteObserver& mso : mines_) {
			if (mso.bo != pred_bo || mso.site == nullptr) {
				continue;
			}
			if (mso.upgrade_pending || mso.dismantle_pending_since.is_valid()) {
				continue;
			}
			if (mso.site->get_economy(Widelands::wwWORKER)->warehouses().empty()) {
				continue;
			}
			if (!mso.site->has_workers(en_bo.id, game())) {
				continue;
			}

			int32_t site_score = en_score;
			// Mine enhancements: prefer mines with remaining resources
			const Widelands::FCoords mine_pos =
			   map.get_fcoords(mso.site->get_position());
			if (mine_pos.field->get_resources_amount() > 0) {
				site_score += mine_pos.field->get_resources_amount();
			} else {
				// Depleted mine: lower priority for enhancement
				site_score /= 2;
			}

			if (site_score > enhance_priority) {
				enhance_priority = site_score;
				best_enhance_bo = &en_bo;
				best_enhance_site = mso.site;
			}
		}
	}

	// Enhancement competes with economy chain: if enhancing is better
	// than building new, the enhancement wins.
	if (best_enhance_bo != nullptr && best_enhance_site != nullptr &&
	    enhance_priority > economy_priority) {
		verb_log_dbg_time(gametime,
		   "PlannerAI %u enhance: %s → %s (prio %d)\n",
		   static_cast<unsigned>(player_number()),
		   best_enhance_site->descr().name().c_str(),
		   best_enhance_bo->desc->name().c_str(), enhance_priority);

		// Set inputs to zero and mark as pending (drains inputs first)
		for (ProductionSiteObserver& pso : productionsites) {
			if (pso.site == best_enhance_site) {
				set_inputs_to_zero(pso);
				pso.upgrade_pending = true;
				++pso.bo->cnt_upgrade_pending;
				best_enhance_bo->construction_decision_time = gametime;
				break;
			}
		}
		for (ProductionSiteObserver& mso : mines_) {
			if (mso.site == best_enhance_site) {
				set_inputs_to_zero(mso);
				mso.upgrade_pending = true;
				++mso.bo->cnt_upgrade_pending;
				best_enhance_bo->construction_decision_time = gametime;
				break;
			}
		}
	}

	// --- Build the winners (two independent chains) ---
	bool built_something = false;

	// Chain 1: Economy building (only if construction limit not exceeded)
	if (best_economy != nullptr && numof_psites_in_constr <= max_construction) {
		verb_log_info_time(gametime, "P%u >>> BUILD %s at %d,%d (score %d)\n",
		                  static_cast<unsigned>(player_number()),
		                  best_economy->desc->name().c_str(),
		                  economy_coords.x, economy_coords.y, economy_priority);

		game().send_player_build_building(player_number(), economy_coords, best_economy->id);
		blocked_fields.add(economy_coords, gametime + Duration(2 * 60 * 1000));
		best_economy->new_building_overdue = 0;
		best_economy->construction_decision_time = gametime;

		// Reset building pressure integral: we just built one, so the
		// accumulated demand is SATISFIED. Without this reset, the PI
		// integral retains stale demand and might trigger a second build
		// of the same type before the game processes the first one.
		for (size_t bi = 0; bi < buildings_.size(); ++bi) {
			if (&buildings_[bi] == best_economy && bi < building_pressure_.size()) {
				building_pressure_[bi].ipart = 0;
				break;
			}
		}

		if (best_economy->is(BuildingAttribute::kSpaceConsumer)) {
			// Territory-proportional blocking: scale radius with territory size.
			// The fixed radius-5 (covering ~91 hex cells) is appropriate for
			// large territories (200+ fields) but catastrophic for small ones.
			// At 59 fields, radius 5 blocks the ENTIRE territory for 30 minutes,
			// causing the Atlantean stall: forester wins every 30-min cycle,
			// re-blocks everything, economy frozen at econ=6 indefinitely.
			//
			// Formula: sqrt(fields) / 4, clamped to [1, 5].
			//   59 fields → radius 2 (~19 hex cells, 32% of territory)
			//   100 fields → radius 3 (~37 hex cells, 37% of territory)
			//   200 fields → radius 4 (~61 hex cells, 30% of territory)
			//   500 fields → radius 5 (capped, ~91 hex cells, 18%)
			// This blocks roughly 30% of the territory regardless of size,
			// leaving room for other buildings during the 30-min window.
			const int32_t n_fields =
			   static_cast<int32_t>(buildable_fields.size());
			int32_t sc_block_radius = 1;
			{
				int32_t temp = n_fields / 16;  // sqrt(n)/4 squared = n/16
				while (sc_block_radius * sc_block_radius < temp) {
					++sc_block_radius;
				}
			}
			sc_block_radius = std::min<int32_t>(5,
			   std::max<int32_t>(1, sc_block_radius));
			Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
			   map,
			   Widelands::Area<Widelands::FCoords>(
			      map.get_fcoords(economy_coords), sc_block_radius));
			do {
				blocked_fields.add(mr.location(), gametime + Duration(30 * 60 * 1000));
			} while (mr.advance(map));
		}

		// Resource-based buildings (quarry, lumberjack, fisher, hunter, mine):
		// Block the work area so a second building of the same type
		// can't be placed at an overlapping spot in the next few ticks.
		// These buildings compete for finite or slow-renewing resources.
		// Block duration = 50 seconds (enough for the field scan to
		// update and show the construction site).
		if (best_economy->is_resource_harvester ||
		    best_economy->is(BuildingAttribute::kFisher) ||
		    best_economy->is(BuildingAttribute::kWell) ||
		    best_economy->desc->get_ismine()) {
			// Mines: block radius 4 (mine work area overlap).
			// Surface buildings: use their work area radius.
			const uint32_t block_radius = best_economy->desc->get_ismine() ? 4 :
			   (best_economy->desc->workarea_info().empty() ?
			      6 : best_economy->desc->workarea_info().rbegin()->first);
			Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
			   map,
			   Widelands::Area<Widelands::FCoords>(
			      map.get_fcoords(economy_coords), block_radius));
			do {
				blocked_fields.add(mr.location(), gametime + Duration(50 * 1000));
			} while (mr.advance(map));
		}
		built_something = true;
	}

	// Chain 2: Military building (independent of economy construction limit)
	if (best_military != nullptr) {
		verb_log_info_time(gametime, "P%u >>> MILITARY %s at %d,%d (score %d)\n",
		                  static_cast<unsigned>(player_number()),
		                  best_military->desc->name().c_str(),
		                  military_coords.x, military_coords.y, military_priority);

		game().send_player_build_building(player_number(), military_coords, best_military->id);
		blocked_fields.add(military_coords, gametime + Duration(2 * 60 * 1000));
		best_military->new_building_overdue = 0;
		best_military->construction_decision_time = gametime;

		// === Slam nearby military integrals negative ===
		//
		// We just decided to build here. Nearby fields whose potential
		// conquer area OVERLAPS with this building's conquer area should
		// have their integral reset to zero. The construction freeze
		// (military_in_constr_nearby > 0) will prevent accumulation
		// until the building finishes. The slam provides immediate
		// suppression until the next field scan detects the construction
		// site and sets military_in_constr_nearby.
		//
		// Only affect fields within conquer_r_new + conquer_r_smallest.
		// Fields on the opposite side of the HQ (no overlap) keep their
		// integral untouched — they compete for DIFFERENT land.
		const int32_t conquer_r_built = static_cast<int32_t>(
		   best_military->desc->get_conquers());
		// Find smallest military conquer radius for overlap calculation.
		int32_t smallest_conquer_r = conquer_r_built;
		for (const BuildingObserver& mbo : buildings_) {
			if (mbo.type == BuildingObserver::Type::kMilitarysite &&
			    mbo.buildable(*player_)) {
				const int32_t cr = static_cast<int32_t>(mbo.desc->get_conquers());
				if (cr > 0 && cr < smallest_conquer_r) {
					smallest_conquer_r = cr;
				}
			}
		}
		const int32_t overlap_radius = conquer_r_built + smallest_conquer_r;

		for (BuildableField* const nbf : buildable_fields) {
			const uint16_t dist =
			   map.calc_distance(military_coords, nbf->coords);
			// Only slam fields whose conquer area would actually overlap
			// with the new building's conquer area. Two circles overlap
			// when dist < r1 + r2. Beyond that: different land, no slam.
			if (dist <= static_cast<uint16_t>(overlap_radius)) {
				// Reset to zero: the construction freeze will prevent
				// re-accumulation until the building finishes.
				nbf->military_integral_ = 0;
			}
		}

		Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
		   map,
		   Widelands::Area<Widelands::FCoords>(map.get_fcoords(military_coords), 6));
		do {
			blocked_fields.add(mr.location(), gametime + Duration(25 * 1000));
		} while (mr.advance(map));

		built_something = true;
	}

	// === Integral anti-windup for unplaceable buildings ===
	//
	// Classic PI anti-windup: when a building type is kNeeded (positive
	// building pressure) but wasn't placed this tick, decay its integral.
	// Without this, the integral grows unboundedly when the building
	// CAN'T be placed (all fields blocked by space consumer, frontier
	// penalty killing priority on every field, etc.). The accumulated
	// integral creates permanent monopoly — when fields finally unblock,
	// the inflated integral makes the building unbeatable for many ticks.
	//
	// The Atlantean stall illustrates: foresters_house accumulates 6.3M
	// pressure while blocked for 30 minutes. When fields unblock, the
	// forester's integral is so high that no other building can compete.
	// With anti-windup: the integral decays by 1/8 per tick during the
	// stall, naturally allowing lower-priority buildings to catch up.
	//
	// When a building IS placed, its integral is already reset to 0
	// (line ~4553), so the anti-windup doesn't interfere with normal
	// construction flow.
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		BuildingObserver& bo = buildings_[bi];
		if (bi >= building_pressure_.size()) {
			continue;
		}
		// Only apply to buildings that were wanted but not placed.
		// A building that was placed this tick has its integral reset above.
		// A building that wasn't wanted (kNotNeeded) should keep accumulating
		// normally in update_building_pressures() — the anti-windup only
		// targets the specific failure mode of "wanted but unplaceable."
		if (bo.new_building == BuildingNecessity::kNeeded &&
		    (bo.type == BuildingObserver::Type::kProductionsite ||
		     bo.type == BuildingObserver::Type::kMine) &&
		    &bo != best_economy) {
			// Harvesters with no resource fields: reset ipart to 0.
			// The harvester won the PID competition but can't build
			// anywhere — all fields lack trees/rocks/fish/critters.
			// With N resource-starved harvesters and a weaker penalty
			// (e.g. /2), the pipeline stalls for N ticks as each
			// harvester cycles through #1 in turn. Resetting to 0
			// clears ALL of them in a single tick — the next tick,
			// buildable buildings with accumulated ipart win immediately.
			// When resources reappear (trees grow, fish breed), normal
			// PID error accumulation rebuilds pressure from scratch.
			if (!harvester_has_resource_field[bi] &&
			    (bo.is_resource_harvester || bo.is(BuildingAttribute::kFisher))) {
				building_pressure_[bi].ipart = 0;
			} else {
				// Standard anti-windup: 7/8 per tick. After 8 stalled
				// ticks, integral is at ~35% of its peak. After 16: ~12%.
				// Fast enough to prevent monopoly, slow enough that
				// momentary placement failures don't erase valid demand.
				building_pressure_[bi].ipart =
				   building_pressure_[bi].ipart * 7 / 8;
			}
		}

		// Extra anti-windup for worker-blocked buildings:
		// when workers can't be produced, the integral accumulates
		// pressure that can never be satisfied. Apply additional
		// 3/4 decay on top of the standard 7/8 to prevent this.
		if (bo.unoccupied_count > 0 &&
		    (bo.type == BuildingObserver::Type::kProductionsite ||
		     bo.type == BuildingObserver::Type::kMine)) {
			bool worker_blocked = false;
			for (const auto& widx : bo.positions) {
				auto it = worker_producible_.find(widx);
				if (it != worker_producible_.end() && !it->second) {
					worker_blocked = true;
					break;
				}
			}
			if (worker_blocked) {
				building_pressure_[bi].ipart =
				   building_pressure_[bi].ipart * 3 / 4;
			}
		}
	}

	return built_something;
}

// =====================================================================
// Road building
// =====================================================================

bool PlannerAI::improve_roads(const Time& gametime) {

	// 1. Dead end removal: remove flags with no building and only one road
	{
		const uint16_t stepping = std::max<uint16_t>(1, roads.size() / 25 + 1);
		for (uint16_t i = 0; i < roads.size(); i += stepping) {
			const Widelands::Flag& startflag = roads[i]->get_flag(Widelands::RoadBase::FlagStart);
			const Widelands::Flag& endflag = roads[i]->get_flag(Widelands::RoadBase::FlagEnd);
			if (startflag.get_building() == nullptr && startflag.is_dead_end()) {
				game().send_player_bulldoze(*const_cast<Widelands::Flag*>(&startflag));
				return true;
			}
			if (endflag.get_building() == nullptr && endflag.is_dead_end()) {
				game().send_player_bulldoze(*const_cast<Widelands::Flag*>(&endflag));
				return true;
			}
		}
	}

	// 2. Road splitting: split roads > 3 segments by placing a flag in the middle
	if (!roads.empty()) {
		const Widelands::Map& map = game().map();
		const Widelands::Path& path = roads.front()->get_path();
		if (path.get_nsteps() > 3 && spots_ > 0) {
			Widelands::CoordPath cp(map, path);
			Widelands::CoordPath::StepVector::size_type i = cp.get_nsteps() - 1;
			Widelands::CoordPath::StepVector::size_type j = 1;
			for (; i >= j; --i, ++j) {
				{
					const Widelands::Coords c = cp.get_coords().at(i);
					if ((map[c].nodecaps() & Widelands::BUILDCAPS_FLAG) != 0) {
						game().send_player_build_flag(player_number(), c);
						return true;
					}
				}
				{
					const Widelands::Coords c = cp.get_coords().at(j);
					if ((map[c].nodecaps() & Widelands::BUILDCAPS_FLAG) != 0) {
						game().send_player_build_flag(player_number(), c);
						return true;
					}
				}
			}
		}
		// Rotate roads
		roads.push_back(roads.front());
		roads.pop_front();
	}

	// 3. Process new flags: connect to road network
	uint16_t max_attempts = 5;
	while (!new_flags.empty() && max_attempts > 0) {
		const Widelands::Flag* flag = new_flags.front();
		new_flags.pop_front();

		if (flag->get_economy(Widelands::wwWORKER) == nullptr) {
			continue;
		}

		if (flag->nr_of_roads() < 1) {
			connect_flag_to_road_network(*flag, gametime);
		}

		// Track in economy
		bool found_economy = false;
		for (EconomyObserver* eco : economies) {
			if (&eco->economy == flag->get_economy(Widelands::wwWORKER)) {
				eco->flags.push_back(flag);
				found_economy = true;
				break;
			}
		}
		if (!found_economy) {
			EconomyObserver* new_eco = new EconomyObserver(*flag->get_economy(Widelands::wwWORKER));
			new_eco->flags.push_back(flag);
			economies.push_back(new_eco);
		}
		--max_attempts;
	}

	// 4. Shortcut roads: try to improve connectivity for existing flags
	if (!economies.empty()) {
		if (economies.size() >= 2) {
			economies.push_back(economies.front());
			economies.pop_front();
		}
		EconomyObserver* eco = economies.front();
		if (!eco->flags.empty()) {
			if (eco->flags.size() > 1) {
				eco->flags.push_back(eco->flags.front());
				eco->flags.pop_front();
			}
			const Widelands::Flag& flag = *eco->flags.front();

			// Dead end flag removal
			if (flag.is_dead_end() && flag.current_wares() == 0 &&
			    flag.get_building() == nullptr) {
				game().send_player_bulldoze(*const_cast<Widelands::Flag*>(&flag));
				eco->flags.pop_front();
				return true;
			}

			// Probability-based shortcut attempt
			uint16_t probability_score = 0;
			if (flag.nr_of_roadbases() == 1) {
				probability_score += 20;
			}
			if (flag.get_building() != nullptr) {
				BuildingObserver& bo = get_building_observer(
				   flag.get_building()->descr().name().c_str());
				if (bo.type == BuildingObserver::Type::kWarehouse && flag.nr_of_roadbases() <= 3) {
					probability_score += 20;
				}
			}
			probability_score += flag.current_wares() * 5;
			const bool needs_warehouse =
			   flag.get_economy(Widelands::wwWORKER)->warehouses().empty();
			if (needs_warehouse) {
				probability_score += 500;
			}

			if (RNG::static_rand(200) < probability_score) {
				connect_flag_to_road_network(flag, gametime);
			}
		}
	}

	return false;
}

bool PlannerAI::connect_flag_to_road_network(
   const Widelands::Flag& flag, const Time& gametime) {
	const Widelands::Map& map = game().map();
	CheckStepRoadAI check(player_, Widelands::MOVECAPS_WALK, true);

	uint16_t checkradius = 14;

	// Unconnected economy: increase radius and set grace time for dismantling
	const bool needs_warehouse =
	   flag.get_economy(Widelands::wwWORKER)->warehouses().empty();
	if (needs_warehouse && flag.get_building() != nullptr) {
		checkradius += 2;
		// Occupied military sites get extra radius
		if (upcast(Widelands::MilitarySite, militb, flag.get_building())) {
			if (!militb->soldier_control()->stationed_soldiers().empty()) {
				checkradius += 4;
			}
		}
	}

	// Use FlagCandidates to score and select the best target
	FlagCandidates flag_candidates(0);

	// Find all reachable flags/roads within radius
	FindNodeWithFlagOrRoad functor;
	std::vector<Widelands::Coords> reachable;
	map.find_reachable_fields(
	   game(),
	   Widelands::Area<Widelands::FCoords>(map.get_fcoords(flag.get_position()), checkradius),
	   &reachable, check, functor);

	for (const Widelands::Coords& reachable_coords : reachable) {
		if (reachable_coords == flag.get_position()) {
			continue;
		}

		Widelands::BaseImmovable* this_immovable = map[reachable_coords].get_immovable();
		if (upcast(Widelands::PlayerImmovable const, player_immovable, this_immovable)) {
			// If it's a road, place a flag there
			if (this_immovable->descr().type() == Widelands::MapObjectType::ROAD) {
				game().send_player_build_flag(player_number(), reachable_coords);
			}

			if (this_immovable->descr().type() != Widelands::MapObjectType::FLAG) {
				continue;
			}

			// Only connect to flags whose economy has a warehouse
			if (player_immovable->economy(Widelands::wwWORKER).warehouses().empty()) {
				continue;
			}

			const bool is_different_economy =
			   (player_immovable->get_economy(Widelands::wwWORKER) !=
			    flag.get_economy(Widelands::wwWORKER));
			const uint16_t air_distance =
			   map.calc_distance(flag.get_position(), reachable_coords);

			const uint32_t reachable_coords_hash = reachable_coords.hash();
			if (!flag_candidates.has_candidate(reachable_coords_hash)) {
				flag_candidates.add_flag(
				   reachable_coords_hash, is_different_economy,
				   0,  // warehouse distance not tracked in PlannerAI
				   air_distance);
			}
		}
	}

	// Walk over road network to collect actual road distances
	std::map<uint32_t, NearFlag> nearflags;
	nearflags[flag.get_position().hash()] = NearFlag(&flag, 0);
	collect_nearflags(nearflags, flag, checkradius);

	for (auto& nf_walk : nearflags) {
		const uint32_t nf_hash = nf_walk.second.flag->get_position().hash();
		if (flag_candidates.has_candidate(nf_hash)) {
			flag_candidates.set_cur_road_distance(nf_hash, nf_walk.second.current_road_distance);
		}
	}

	// Score candidate flags by finding actual paths
	flag_candidates.sort_by_air_distance();
	uint32_t possible_roads_count = 0;
	for (const auto& flag_candidate : flag_candidates.flags()) {
		if (possible_roads_count > 10) {
			break;
		}
		const Widelands::Coords coords =
		   Widelands::Coords::unhash(flag_candidate.coords_hash);
		Widelands::Path path;
		const int32_t pathcost =
		   map.findpath(flag.get_position(), coords, 0, path, check);
		if (pathcost >= 0) {
			flag_candidates.set_road_possible(flag_candidate.coords_hash, path.get_nsteps());
			++possible_roads_count;
		}
	}

	// Pick the best candidate and build the road
	flag_candidates.sort();
	FlagCandidates::Candidate* winner = flag_candidates.get_winner(needs_warehouse ? 50 : 25);
	if (winner != nullptr) {
		const Widelands::Coords target_coords =
		   Widelands::Coords::unhash(winner->coords_hash);
		// Path must be heap-allocated: CmdBuildRoad stores a pointer
		// and serializes it asynchronously in another thread
		Widelands::Path& path = *new Widelands::Path();
		const int32_t pathcost =
		   map.findpath(flag.get_position(), target_coords, 0, path, check);
		if (pathcost >= 0) {
			game().send_player_build_road(player_number(), path);
			return true;
		}
		delete &path;
	}

	// Failed to connect — try to clear blocking trees on the path
	// to the nearest connected flag, so workers can remove them and
	// the road can be built on the next attempt.
	if (needs_warehouse && !flag_candidates.flags().empty()) {
		// Find the nearest connected flag by air distance
		Widelands::Coords nearest_connected = Widelands::Coords::null();
		uint32_t best_dist = std::numeric_limits<uint32_t>::max();
		for (const auto& fc : flag_candidates.flags()) {
			const Widelands::Coords c = Widelands::Coords::unhash(fc.coords_hash);
			const uint32_t d = map.calc_distance(flag.get_position(), c);
			if (d < best_dist) {
				best_dist = d;
				nearest_connected = c;
			}
		}

		if (nearest_connected != Widelands::Coords::null()) {
			// Walk from the disconnected flag toward the nearest connected flag.
			// Mark any trees/immovables on the path for removal so woodcutters
			// clear a road corridor.
			const uint32_t tree_attr =
			   Widelands::MapObjectDescr::get_attribute_id("tree");
			const uint32_t normal_tree_attr =
			   Widelands::MapObjectDescr::get_attribute_id("normal_tree");
			uint16_t trees_marked = 0;
			const uint16_t mark_radius = 2;  // mark trees within 2 fields of the path

			// Walk step by step toward the target using the closest-neighbor heuristic
			Widelands::FCoords walker = map.get_fcoords(flag.get_position());
			for (uint16_t step = 0; step < best_dist + mark_radius && step < 30; ++step) {
				// Scan the area around the current walker position
				Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> scan(
				   map, Widelands::Area<Widelands::FCoords>(walker, mark_radius));
				do {
					if (scan.location().field->get_owned_by() != player_number()) {
						continue;
					}
					Widelands::BaseImmovable* imm = scan.location().field->get_immovable();
					if (imm == nullptr || imm->get_size() < Widelands::BaseImmovable::SMALL) {
						continue;
					}
					// Only mark non-player immovables (trees, rocks etc.), not buildings/flags
					if (imm->descr().type() >= Widelands::MapObjectType::FLAG) {
						continue;
					}
					if (imm->has_attribute(tree_attr) || imm->has_attribute(normal_tree_attr)) {
						if (upcast(Widelands::Immovable, tree_imm, imm)) {
							game().send_player_mark_object_for_removal(
							   player_number(), *tree_imm, true);
							++trees_marked;
						}
					}
				} while (scan.advance(map));

				// Step toward the target: pick the neighbor closest to the target
				uint32_t closest = map.calc_distance(walker, nearest_connected);
				if (closest == 0) {
					break;
				}
				Widelands::FCoords best_next = walker;
				for (Widelands::Direction d = Widelands::FIRST_DIRECTION;
				     d <= Widelands::LAST_DIRECTION; ++d) {
					const Widelands::FCoords nb = map.get_neighbour(walker, d);
					const uint32_t nd = map.calc_distance(nb, nearest_connected);
					if (nd < closest) {
						closest = nd;
						best_next = nb;
					}
				}
				if (best_next == walker) {
					break;  // stuck, no progress
				}
				walker = best_next;
			}

			if (trees_marked > 0) {
				verb_log_dbg_time(gametime,
				   "PlannerAI %u: marked %u trees for removal near flag %d,%d "
				   "to clear path toward %d,%d\n",
				   static_cast<unsigned>(player_number()), trees_marked,
				   flag.get_position().x, flag.get_position().y,
				   nearest_connected.x, nearest_connected.y);
				return false;  // Don't block — trees are being cleared
			}
		}

		// No trees to clear but still can't connect — block the area
		const Duration block_time(2 * 60 * 1000);
		Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
		   map,
		   Widelands::Area<Widelands::FCoords>(map.get_fcoords(flag.get_position()), checkradius));
		do {
			if (mr.location().field->get_owned_by() == player_number()) {
				blocked_fields.add(mr.location(), gametime + block_time);
			}
		} while (mr.advance(map));
	}

	return false;
}

void PlannerAI::collect_nearflags(
   std::map<uint32_t, NearFlag>& nearflags,
   const Widelands::Flag& flag,
   uint16_t checkradius) {
	const Widelands::Map& map = game().map();

	// Add the starting flag
	nearflags[flag.get_position().hash()] = NearFlag(&flag, 0);

	// Walk over road network collecting reachable flags
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& nf_pair : nearflags) {
			if (!nf_pair.second.to_be_checked) {
				continue;
			}
			nf_pair.second.to_be_checked = false;
			const uint32_t start_field = nf_pair.first;

			// Iterate over all roads from this flag
			for (uint8_t dir = Widelands::WalkingDir::FIRST_DIRECTION;
			     dir <= Widelands::WalkingDir::LAST_DIRECTION; ++dir) {
				Widelands::Road* const road = nearflags[start_field].flag->get_road(dir);
				if (road == nullptr) {
					continue;
				}

				Widelands::Flag* endflag = &road->get_flag(Widelands::RoadBase::FlagStart);
				if (endflag == nearflags[start_field].flag) {
					endflag = &road->get_flag(Widelands::RoadBase::FlagEnd);
				}

				const uint32_t endflag_hash = endflag->get_position().hash();
				const int32_t dist =
				   map.calc_distance(flag.get_position(), endflag->get_position());

				if (dist > checkradius + 2) {
					continue;
				}

				if (nearflags.count(endflag_hash) == 0) {
					nearflags[endflag_hash] =
					   NearFlag(endflag, nearflags[start_field].current_road_distance +
					                        road->get_path().get_nsteps());
					changed = true;
				} else {
					if (nearflags[endflag_hash].current_road_distance >
					    nearflags[start_field].current_road_distance +
					       road->get_path().get_nsteps()) {
						nearflags[endflag_hash].current_road_distance =
						   nearflags[start_field].current_road_distance +
						   road->get_path().get_nsteps();
						nearflags[endflag_hash].to_be_checked = true;
						changed = true;
					}
				}
			}
		}
	}
}

// =====================================================================
// Economy management
// =====================================================================

bool PlannerAI::check_economies() {
	// Process new flags into economies
	while (!new_flags.empty()) {
		const Widelands::Flag& flag = *new_flags.front();
		new_flags.pop_front();

		bool found = false;
		for (EconomyObserver* eco : economies) {
			if (&eco->economy == flag.get_economy(Widelands::wwWORKER)) {
				eco->flags.push_back(&flag);
				found = true;
				break;
			}
		}
		if (!found) {
			EconomyObserver* new_eco = new EconomyObserver(*flag.get_economy(Widelands::wwWORKER));
			new_eco->flags.push_back(&flag);
			economies.push_back(new_eco);
		}
	}

	// Clean up empty economies
	for (auto it = economies.begin(); it != economies.end();) {
		EconomyObserver* eco = *it;
		// Remove flags that are no longer in this economy
		for (auto fit = eco->flags.begin(); fit != eco->flags.end();) {
			if ((*fit)->get_economy(Widelands::wwWORKER) != &eco->economy) {
				fit = eco->flags.erase(fit);
			} else {
				++fit;
			}
		}
		if (eco->flags.empty()) {
			delete eco;
			it = economies.erase(it);
		} else {
			++it;
		}
	}

	return true;
}

// =====================================================================
// Production site management (Layer 5: Input Queue Priorities)
// =====================================================================

bool PlannerAI::check_productionsites(const Time& gametime) {
	if (productionsites.empty()) {
		return false;
	}

	// Rotate: move front to back, then check the new front
	productionsites.push_back(productionsites.front());
	productionsites.pop_front();
	ProductionSiteObserver& site = productionsites.front();

	if (site.site == nullptr || site.bo == nullptr) {
		return false;
	}

	// Track occupancy
	if (!site.site->can_start_working()) {
		site.unoccupied_till = gametime;
	}

	const bool connected_to_wh = !site.site->get_economy(Widelands::wwWORKER)->warehouses().empty();

	// --- Worker eviction: release experienced workers needed elsewhere ---
	for (uint8_t i = 0; i < site.site->descr().nr_working_positions(); i++) {
		const Widelands::Worker* cw = site.site->working_positions()->at(i).worker.get(game());
		if (cw != nullptr) {
			Widelands::DescriptionIndex current_worker = cw->descr().worker_index();
			if (current_worker != site.bo->positions.at(i)) {
				game().send_player_evict_worker(
				   *site.site->working_positions()->at(i).worker.get(game()));
				return true;
			}
		}
	}

	// --- Upgrade execution (pending upgrades from construct_building Pass 3) ---
	const Widelands::DescriptionIndex enhancement = site.site->descr().enhancement();

	// Handle input queues set to 0 unexpectedly (e.g. after game load)
	if (!site.upgrade_pending) {
		bool resetting_wares = false;
		for (const auto& queue : site.site->inputqueues()) {
			if (queue->get_max_fill() == 0) {
				resetting_wares = true;
				game().send_player_set_input_max_fill(
				   *site.site, queue->get_index(), queue->get_type(), queue->get_max_size());
			}
		}
		if (resetting_wares) {
			return true;
		}
	}

	// Site is pending upgrade: wait for inputs to drain, then enhance.
	// The upgrade decision was made in construct_building() Pass 3
	// using the PI building pressure system.
	if (site.upgrade_pending) {
		if (site.bo->construction_decision_time + Duration(4 * 60 * 1000) > gametime &&
		    !set_inputs_to_zero(site)) {
			return false;
		}
		assert(enhancement != Widelands::INVALID_INDEX);
		game().send_player_enhance_building(*site.site, enhancement, true);
		return true;
	}

	// --- Barracks management ---
	if (site.bo->is(BuildingAttribute::kBarracks)) {
		// Unconnected barracks: try to reconnect flag, or bulldoze after 3min
		if (!connected_to_wh) {
			if ((gametime - site.built_time) > Duration(3 * 60 * 1000)) {
				game().send_player_bulldoze(*site.site);
				return true;
			}
			// Try reconnecting
			if (site.site->base_flag().nr_of_roads() < 1) {
				connect_flag_to_road_network(site.site->base_flag(), gametime);
			}
			return false;
		}

		if (site.bo->total_count() > 1) {
			game().send_player_dismantle(*site.site, true);
			return true;
		}

		// Limit barracks input fill to 4
		for (const auto& queue : site.site->inputqueues()) {
			if (queue->get_max_fill() > 4) {
				game().send_player_set_input_max_fill(
				   *site.site, queue->get_index(), queue->get_type(), 4);
			}
		}

		// Start/stop based on soldier needs.
		// Score = soldier_status enum value (kFull=0, kEnough=1, kShortage=3,
		// kBadShortage=6). Threshold = kEnough (= 1). Barracks runs when
		// soldier shortage exceeds "enough". Training sites without trainers
		// reduce the threshold — no point recruiting if we can't train.
		int16_t barracks_score = static_cast<int16_t>(soldier_status_);
		barracks_score -= static_cast<int16_t>(SoldiersStatus::kEnough);
		if (ts_without_trainers_ > 0) {
			barracks_score -= static_cast<int16_t>(SoldiersStatus::kShortage);
		}

		if (site.site->is_stopped() && barracks_score >= 0) {
			game().send_player_start_stop_building(*site.site);
		}
		if (!site.site->is_stopped() && barracks_score < 0) {
			game().send_player_start_stop_building(*site.site);
		}
		return false;
	}

	// --- Rangers management ---
	if (site.bo->is(BuildingAttribute::kRanger)) {
		if (wood_policy_.count(site.bo->id) > 0 &&
		    (wood_policy_.at(site.bo->id) == WoodPolicy::kStopRangers ||
		     wood_policy_.at(site.bo->id) == WoodPolicy::kDismantleRangers) &&
		    !site.site->is_stopped()) {
			game().send_player_start_stop_building(*site.site);
		}
		if (wood_policy_.count(site.bo->id) > 0 &&
		    wood_policy_.at(site.bo->id) == WoodPolicy::kAllowRangers &&
		    site.site->is_stopped()) {
			game().send_player_start_stop_building(*site.site);
		}
	}

	// --- Set input queue priorities based on ware pressure ---
	if (!site.bo->inputs.empty() && !site.bo->ware_outputs.empty()) {
		int32_t max_output_pressure = 0;
		for (const auto& output : site.bo->ware_outputs) {
			if (static_cast<size_t>(output) < ware_pressure_.size()) {
				max_output_pressure = std::max(max_output_pressure, ware_pressure_[output].outputControl);
			}
		}

		for (Widelands::InputQueue* queue : site.site->inputqueues()) {
			if (queue->get_type() != Widelands::wwWARE) {
				continue;
			}
			const Widelands::DescriptionIndex ware_idx = queue->get_index();
			if (static_cast<size_t>(ware_idx) >= ware_pressure_.size()) {
				continue;
			}

			const int32_t input_pressure = ware_pressure_[ware_idx].outputControl;
			// Threshold = avg ware pressure. A ware at avg pressure is
			// "normally important". Above avg = high priority; below = low.
			// Derived from Circle 1 normalization — no magic number.
			const int32_t avg_wp_q = wares.empty() ? 1 :
			   kNormalizationBudget / static_cast<int32_t>(wares.size());
			Widelands::WarePriority desired = Widelands::WarePriority::kNormal;

			if (max_output_pressure > avg_wp_q && input_pressure < avg_wp_q) {
				desired = Widelands::WarePriority::kHigh;
			} else if (max_output_pressure < avg_wp_q && input_pressure > avg_wp_q) {
				desired = Widelands::WarePriority::kLow;
			}

			if (site.site->get_priority(Widelands::wwWARE, ware_idx, 0) != desired) {
				game().send_player_set_ware_priority(
				   *site.site, Widelands::wwWARE, ware_idx, desired);
			}
		}

		// --- Production site pause/resume based on ware pressure ---
		// Stop production when outputs are not needed but inputs are
		// scarce. This prevents a building from consuming resources
		// that are more urgently needed elsewhere (e.g., a smithy
		// consuming coal when the smelter desperately needs it).
		//
		// Logic: pause when ALL outputs are below avg pressure AND
		// at least one input is above 2× avg pressure (crisis level).
		// Resume when any output rises back above avg pressure.
		// Never pause the last/only instance of a building type.
		// Never pause buildings on the basic economy list.
		if (!site.bo->is(BuildingAttribute::kBarracks) &&
		    !site.bo->is(BuildingAttribute::kShipyard) &&
		    !site.bo->is(BuildingAttribute::kRanger) &&
		    site.bo->cnt_built > 1 &&
		    !site.bo->ware_outputs.empty() &&
		    !site.bo->inputs.empty()) {
			const int32_t avg_wp_pause = wares.empty() ? 1 :
			   kNormalizationBudget / static_cast<int32_t>(wares.size());

			bool any_output_needed = false;
			for (const auto& output : site.bo->ware_outputs) {
				if (static_cast<size_t>(output) < ware_pressure_.size() &&
				    ware_pressure_[output].outputControl >= avg_wp_pause) {
					any_output_needed = true;
					break;
				}
			}

			bool input_in_crisis = false;
			for (const auto& input : site.bo->inputs) {
				if (static_cast<size_t>(input) < ware_pressure_.size() &&
				    ware_pressure_[input].outputControl > avg_wp_pause * 2) {
					input_in_crisis = true;
					break;
				}
			}

			if (!any_output_needed && input_in_crisis && !site.site->is_stopped()) {
				game().send_player_start_stop_building(*site.site);
			} else if (any_output_needed && site.site->is_stopped()) {
				game().send_player_start_stop_building(*site.site);
			}
		}
	}

	// ========== Unified Dismantle Score (leaky integrator) ==========
	//
	// === Priority Inheritance Contract ===
	// Every dismantle factor is derived from the root objective:
	//   game goal → need wares/soldiers → need buildings → keep or dismantle?
	// A building is worth keeping if its output serves the goal chain.
	// A building should be dismantled if its output is no longer needed
	// AND the resources it occupies serve the goal better elsewhere.
	//
	// All factors are expressed in [budget/ware] units (= avg_wp).
	// Unit: delta [budget/ware], site.dismantle_score [score].
	//   delta is accumulated from factors each sized as multiples of avg_wp.
	//   score decays per visit: score × (2N-1)/(2N), then score += delta.
	//   Steady state at constant delta: score ≈ 2N × delta.
	//   Threshold: score > 0 → dismantle. Negative = keep.
	//   Clamped: [-nr_wares × avg_wp, +nr_wares × avg_wp].
	//
	// Barracks and shipyards are excluded (managed separately above).
	if (!site.bo->is(BuildingAttribute::kBarracks) &&
	    !site.bo->is(BuildingAttribute::kShipyard) &&
	    (gametime - site.built_time) > Duration(3 * 60 * 1000)) {

		// Leaky integrator: economy-derived decay (same as PI circles)
		const int32_t N_dis = weights_.N_ticks;
		const int32_t dd_den = 2 * N_dis;
		const int32_t dd_num = dd_den - 1;
		site.dismantle_score = site.dismantle_score * dd_num / dd_den;  // [score]

		// Base unit for all dismantle factors: avg ware pressure [budget/ware]
		const int32_t avg_pressure = weights_.avg_wp;  // [budget/ware]

		// Number of output wares this building produces (for scaling).
		const int32_t nr_outputs = std::max<int32_t>(1,
		   static_cast<int32_t>(site.bo->ware_outputs.size()));

		int32_t delta = 0;  // [budget/ware] (accumulated from factors below)

		// --- Global offset: baseline resistance to dismantling ---
		// Requires positive factors worth > offset to trigger.
		// Derived in weights_.update() from avg_wp and clearing density.
		const int32_t nr_wares_d = std::max<int32_t>(1,
		   static_cast<int32_t>(wares.size()));
		delta -= weights_.dismantle_offset;

		// --- Factor: unconnected to warehouse ---
		// A building not connected to a warehouse is completely useless:
		// it can't receive inputs or deliver outputs. Strong dismantle
		// signal overwhelms the global offset.
		if (!connected_to_wh) {
			delta += weights_.unconnected_penalty;
		}

		// --- Factor: output ware pressure ---
		// Derived from Circle 1: output at avg_pressure → neutral (delta 0).
		// Output above avg → keep (negative delta). Output below → dismantle.
		// Clamp: [-avg, +avg]. No magic — avg_pressure IS the threshold.
		int32_t max_output_pressure = 0;
		for (const auto& output : site.bo->ware_outputs) {
			if (static_cast<size_t>(output) < ware_pressure_.size()) {
				max_output_pressure =
				   std::max(max_output_pressure, ware_pressure_[output].outputControl);
			}
		}
		delta += std::clamp(avg_pressure - max_output_pressure,
		                    -avg_pressure, avg_pressure);

		// --- Factor: low productivity ---
		// At 0% stats: +avg_pressure per visit (building is dead weight).
		// Scaled linearly: at 50% → half pressure. At 100% → nothing.
		// Only triggers after building has had time to receive workers.
		const uint32_t stats = site.site->get_statistics_percent();
		if (site.site->can_start_working() && stats < 100 &&
		    (gametime - site.built_time) > Duration(5 * 60 * 1000)) {
			delta += avg_pressure *
			   static_cast<int32_t>(100 - stats) / 100;
		}

		// --- Factor: unoccupied excess (worker scarcity response) ---
		// When this specific building has no worker AND there are other
		// buildings of the same type that DO have workers, this building
		// is excess — it was built before workers were available.
		// Dismantling it frees the spot and recovers some materials.
		// The signal is strong: 2× avg_pressure per excess count.
		// This is the proactive response to the "300 logs but 20
		// unoccupied woodcutters" problem — dismantle the excess.
		if (!site.site->can_start_working() &&
		    site.bo->unoccupied_count > 0 && site.bo->cnt_built > 1 &&
		    (gametime - site.built_time) > Duration(5 * 60 * 1000)) {
			delta += avg_pressure * 2 *
			   static_cast<int32_t>(site.bo->unoccupied_count) /
			   static_cast<int32_t>(site.bo->cnt_built);
		}

		// --- Factor: only/last producer ---
		// Losing the last producer of a ware type means the entire
		// downstream chain stalls. Protection = avg_pressure × nr_outputs:
		// each output ware the building produces is worth protecting.
		// For a building producing 1 ware: -avg_pressure (= offset cancel).
		// For a building producing 3 wares: -3×avg_pressure.
		if (site.bo->cnt_built <= 1) {
			delta -= avg_pressure * nr_outputs;
		}

		// --- Factor: resource depletion for finite-resource buildings ---
		// Quarries depend on rocks — a finite resource. When rocks are
		// gone, the quarry produces nothing and should be dismantled to
		// free the spot and recover materials. Scan the work area for
		// rocks; if none remain, inject an overwhelming dismantle signal.
		// This must override ALL counter-forces (output pressure, last-
		// producer protection, global offset) because a quarry with no
		// rocks will NEVER produce again. The signal (+10× avg_pressure)
		// exceeds: offset(1×) + last_producer(1×) + output_pressure(1×)
		// + clearing_potential + any other factor combined.
		// Similarly handles other finite-resource buildings (fishers with
		// no fish, etc.) through their respective resource checks.
		// Generic resource depletion: scan work area for the building's
		// collected resource targets. If ALL collected resources are
		// exhausted (count = 0), this building can never produce again.
		// Inject overwhelming dismantle signal.
		if (site.bo->is_resource_harvester) {
			bool all_collected_exhausted = true;
			bool has_any_collected = false;
			const Widelands::Map& map = game().map();
			const Widelands::FCoords site_fc =
			   map.get_fcoords(site.site->get_position());

			for (const auto& rt : site.bo->resource_targets) {
				if (!rt.is_collected) {
					continue;
				}
				has_any_collected = true;

				uint16_t work_radius = 6;
				if (!site.bo->desc->workarea_info().empty()) {
					work_radius = site.bo->desc->workarea_info().rbegin()->first;
				}

				uint16_t count = 0;
				Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
				   map, Widelands::Area<Widelands::FCoords>(site_fc, work_radius));
				do {
					if (rt.kind == ResourceSearchTarget::Kind::kImmovableAttribute) {
						if (const Widelands::BaseImmovable* imm =
						       mr.location().field->get_immovable()) {
							for (uint32_t attr : imm->descr().attributes()) {
								if (attr == rt.attribute_id) {
									++count;
									break;
								}
							}
						}
					} else if (rt.kind == ResourceSearchTarget::Kind::kBobAttribute) {
						for (Widelands::Bob* bob =
						        mr.location().field->get_first_bob();
						     bob != nullptr; bob = bob->get_next_on_field()) {
							for (uint32_t attr : bob->descr().attributes()) {
								if (attr == rt.attribute_id) {
									++count;
									break;
								}
							}
						}
					}
				} while (mr.advance(map));

				if (count > 0) {
					all_collected_exhausted = false;
					break;
				}
			}
			if (has_any_collected && all_collected_exhausted) {
				delta += weights_.resource_exhausted_penalty;
			}
		}
		// kNeedsRocks fallback for buildings not classified as resource_harvester
		if (!site.bo->is_resource_harvester &&
		    site.bo->is(BuildingAttribute::kNeedsRocks)) {
			const Widelands::Map& map = game().map();
			const Widelands::FCoords site_fc =
			   map.get_fcoords(site.site->get_position());
			uint16_t work_radius = 6;
			if (!site.bo->desc->workarea_info().empty()) {
				work_radius = site.bo->desc->workarea_info().rbegin()->first;
			}
			uint8_t rocks_count = 0;
			Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
			   map, Widelands::Area<Widelands::FCoords>(site_fc, work_radius));
			do {
				if (const Widelands::BaseImmovable* imm =
				       mr.location().field->get_immovable()) {
					if (imm->has_attribute(
					       Widelands::MapObjectDescr::get_attribute_id("rocks"))) {
						++rocks_count;
					}
				}
			} while (mr.advance(map));
			if (rocks_count == 0) {
				delta += weights_.resource_exhausted_penalty;
			}
		}
		if (site.bo->is(BuildingAttribute::kFisher)) {
			if (site.site->get_statistics_percent() == 0 &&
			    (gametime - site.built_time) > Duration(8 * 60 * 1000)) {
				delta += weights_.unconnected_penalty;
			}
		}

		// --- Factor: ranger dismantle policy ---
		// Explicit policy override. Must overcome global offset (avg_pressure)
		// plus last-producer protection (avg_pressure × nr_outputs).
		// Override = offset + protection + avg_pressure (margin).
		if (site.bo->is(BuildingAttribute::kRanger) &&
		    wood_policy_.count(site.bo->id) > 0 &&
		    wood_policy_.at(site.bo->id) == WoodPolicy::kDismantleRangers &&
		    site.bo->cnt_built > site.bo->cnt_target) {
			delta += avg_pressure * (nr_outputs + 2);
		}

		// --- Factor: build cost vs dismantle payback (loop exit) ---
		//
		// When production chains form loops (need cloth → build shepherd
		// → need cloth), every circuit of the loop amplifies ware
		// pressure. This factor provides the EXIT from the loop:
		// dismantling a building that returns a scarce ware breaks the
		// deadlock by providing the material needed to build the
		// producer.
		//
		// Returns are valued HIGHER than losses because:
		//   - Returns solve an URGENT problem (the deadlock)
		//   - Losses are a sunk cost (materials already spent)
		//   - The building wasn't producing anyway (output low)
		//
		// Scale: returned wares contribute wp² / avg_pressure (quadratic
		// in scarcity). At avg pressure: linear (normal). At 3× avg:
		// 9× multiplier. This makes the dismantle signal grow FASTER
		// than the loop pressure, ensuring convergence.
		// Lost wares contribute linearly: wp / avg_pressure (sunk cost
		// doesn't become more sunk with higher pressure).
		{
			const Widelands::Buildcost& cost = site.bo->desc->buildcost();
			const Widelands::Buildcost& returns = site.bo->desc->returns_on_dismantle();

			// Losses: wares consumed by the dismantle (cost - returns)
			// Unit: lost[count] × wp[budget] / avg_pressure[budget/ware]
			//      = [count × budget × ware / budget] = [count × ware]
			//      ≈ [budget/ware] when lost ≈ 1 and wp ≈ avg_pressure.
			// This is dimensionally correct: the result scales with both
			// the quantity of wares lost AND their relative scarcity.
			for (const auto& [ware_idx, amount] : cost) {
				if (static_cast<size_t>(ware_idx) >= ware_pressure_.size()) {
					continue;
				}
				const int32_t wp = ware_pressure_[ware_idx].outputControl;  // [budget]
				const uint8_t returned =
				   returns.count(ware_idx) > 0 ? returns.at(ware_idx) : 0;
				const int32_t lost = static_cast<int32_t>(amount) - returned;  // [count]
				if (lost > 0) {
					// Unit: [count] × [budget] / [budget/ware] = [count×ware]
					// At lost=1, wp=avg: result = 1. Competes with avg_pressure offsets.
					delta -= lost * wp / (avg_pressure + 1);
				}
			}

			// Gains: wares recovered from dismantle (loop exit mechanism)
			//
			// The dismantle-vs-build tradeoff in exact ratios:
			//
			//   BUILD: costs C wares, produces L wares over lifetime.
			//     Amortized cost = C / L (near 0 for long-lived buildings).
			//     Lifetime L ≈ kAmortizationTicks (25+) production cycles.
			//
			//   DISMANTLE: recovers R wares ONCE. Future production = 0.
			//
			// When returned wares ENABLE building a blocked producer:
			//   dismantle_value = R / C × L × wp
			//     R/C = fraction of construction cost we provide
			//     L = lifetime production of the unlocked building
			//     wp = ware pressure (how urgently it's needed)
			//   This can be very high: recovering 1/2 of construction
			//   cost × 25 lifetime cycles × high pressure = significant.
			//
			// When returned wares DON'T unlock anything:
			//   dismantle_value = R × wp / avg (just one-time relief)
			//   This is small: one item at normal scarcity = 1 unit.
			//
			// The ratio naturally converges: the loop pushes wp higher
			// each tick, which makes the "unlocks producer" path more
			// attractive, until dismantling occurs and breaks the loop.
			constexpr int32_t kLifetimeCycles = 25;

			for (const auto& [ware_idx, amount] : returns) {
				if (static_cast<size_t>(ware_idx) >= ware_pressure_.size()) {
					continue;
				}
				const int32_t wp = ware_pressure_[ware_idx].outputControl;

				// Check if this returned ware would unlock a blocked producer
				int32_t best_unlock_value = 0;
				for (const BuildingObserver& pbo : buildings_) {
					if (pbo.type != BuildingObserver::Type::kProductionsite &&
					    pbo.type != BuildingObserver::Type::kMine) {
						continue;
					}
					if (!pbo.build_material_shortage || pbo.add_new_building_score <= 0) {
						continue;
					}
					if (pbo.desc->buildcost().count(ware_idx) == 0) {
						continue;
					}
					const uint32_t needed = pbo.desc->buildcost().at(ware_idx);
					const uint32_t have = calculate_stocklevel(ware_idx);
					if (have >= needed) {
						continue;  // Not the bottleneck
					}
					// Compute unlock value:
					// R/C × lifetime × ware_pressure / avg_pressure
					const int32_t total_cost = std::max<int32_t>(1,
					   static_cast<int32_t>(pbo.desc->buildcost().total()));
					const int32_t unlock = static_cast<int32_t>(
					   static_cast<int64_t>(amount) * kLifetimeCycles *
					   wp / (static_cast<int64_t>(total_cost) * (avg_pressure + 1)));
					best_unlock_value = std::max(best_unlock_value, unlock);
				}

				if (best_unlock_value > 0) {
					delta += best_unlock_value;
				} else {
					// No deadlock: one-time relief, linear value
					delta += static_cast<int32_t>(amount) * wp / (avg_pressure + 1);
				}
			}
		}

		site.dismantle_score += delta;
		// Clamp: prevent overflow. Limit = nr_wares × avg_pressure (= budget).
		// The score can never exceed the total ware pressure budget.
		const int32_t clamp_limit = nr_wares_d * avg_pressure;
		site.dismantle_score = std::clamp(site.dismantle_score, -clamp_limit, clamp_limit);
	}

	// ========== Dismantle Decision: highest score > 0 across all sites ==========
	ProductionSiteObserver* worst_site = nullptr;
	int32_t worst_score = 0;
	for (auto& ps : productionsites) {
		if (ps.site == nullptr || ps.bo == nullptr) {
			continue;
		}
		if (ps.upgrade_pending || ps.dismantle_pending_since.is_valid()) {
			continue;
		}
		// Don't dismantle same type too frequently
		if (gametime < ps.bo->last_dismantle_time + Duration(3 * 60 * 1000)) {
			continue;
		}
		if (ps.dismantle_score > worst_score) {
			worst_score = ps.dismantle_score;
			worst_site = &ps;
		}
	}

	if (worst_site != nullptr) {
		verb_log_dbg_time(gametime,
		   "PlannerAI %u: dismantling %s (score %" PRId32 ")\n",
		   player_number(), worst_site->bo->name, worst_score);
		worst_site->bo->last_dismantle_time = gametime;
		worst_site->dismantle_score = 0;
		const bool wh_connected =
		   !worst_site->site->get_economy(Widelands::wwWORKER)->warehouses().empty();
		if (wh_connected) {
			game().send_player_dismantle(*worst_site->site, true);
		} else {
			game().send_player_bulldoze(*worst_site->site);
		}
		return true;
	}

	return false;
}

// =====================================================================
// Mine management
// =====================================================================

bool PlannerAI::check_mines_(const Time& gametime) {
	if (mines_.empty()) {
		return false;
	}

	// Rotate
	mines_.push_back(mines_.front());
	mines_.pop_front();
	ProductionSiteObserver& site = mines_.front();

	if (site.site == nullptr || site.bo == nullptr) {
		return false;
	}

	const bool connected_to_wh = !site.site->get_economy(Widelands::wwWORKER)->warehouses().empty();

	// Handle pending dismantling
	if (site.dismantle_pending_since.is_valid()) {
		assert(site.dismantle_pending_since <= gametime);
		if (set_inputs_to_zero(site) ||
		    site.dismantle_pending_since + Duration(5 * 60 * 1000) < gametime) {
			if (connected_to_wh) {
				game().send_player_dismantle(*site.site, true);
			} else {
				game().send_player_bulldoze(*site.site);
			}
			return true;
		}
		if (site.dismantle_pending_since + Duration(3 * 60 * 1000) < gametime) {
			stop_site(site);
		}
		return false;
	}

	// Set inputs based on working status
	if (site.site->can_start_working()) {
		set_inputs_to_max(site);
	} else {
		set_inputs_to_zero(site);
	}

	// Worker eviction
	for (uint8_t i = 0; i < site.site->descr().nr_working_positions(); i++) {
		const Widelands::Worker* cw = site.site->working_positions()->at(i).worker.get(game());
		if (cw != nullptr) {
			Widelands::DescriptionIndex current_worker = cw->descr().worker_index();
			if (current_worker != site.bo->positions.at(i)) {
				game().send_player_evict_worker(
				   *site.site->working_positions()->at(i).worker.get(game()));
				return true;
			}
		}
	}

	// Is this a critical mine (only one of its resource type)?
	bool single_critical = false;
	if (site.bo->mines != Widelands::INVALID_INDEX &&
	    mines_per_type.count(site.bo->mines) > 0 &&
	    (site.bo->mines == iron_resource_id) &&
	    mines_per_type[site.bo->mines].finished == 1) {
		single_critical = true;
	}

	// Dismantle mines missing workers for a long time
	if (!single_critical && site.built_time + Duration(10 * 60 * 1000) < gametime &&
	    !site.site->can_start_working() &&
	    site.bo->mines != Widelands::INVALID_INDEX &&
	    mines_per_type.count(site.bo->mines) > 0 &&
	    mines_per_type[site.bo->mines].total_count() > 2) {
		initiate_dismantling(site, gametime);
		return false;
	}

	if (gametime < Time(10 * 60 * 1000)) {
		return false;
	}

	// If mine is working, nothing to do
	if (site.no_resources_since.is_invalid() ||
	    gametime < site.no_resources_since + Duration(5 * 60 * 1000)) {
		return false;
	}

	// Non-mining mine buildings: ignore out-of-resources
	if (site.bo->mines == Widelands::INVALID_INDEX) {
		return false;
	}

	// Check for enhancement
	const Widelands::DescriptionIndex enhancement = site.site->descr().enhancement();
	const bool has_upgrade =
	   (enhancement != Widelands::INVALID_INDEX &&
	    player_->is_building_type_allowed(enhancement));

	// Handle pending upgrade (decision was made in construct_building Pass 3)
	if (site.upgrade_pending) {
		if (site.bo->construction_decision_time + Duration(4 * 60 * 1000) > gametime &&
		    !set_inputs_to_zero(site)) {
			return false;
		}
		if (has_upgrade) {
			game().send_player_enhance_building(*site.site, enhancement, true);
		}
		return true;
	}

	// Dismantle if no upgrade possible or resources gone too long
	bool forcing_upgrade = false;
	if (has_upgrade && site.bo->mines != Widelands::INVALID_INDEX &&
	    mines_per_type.count(site.bo->mines) > 0 &&
	    mines_per_type[site.bo->mines].total_count() <= 1) {
		forcing_upgrade = true;
	}
	if (!has_upgrade ||
	    (site.no_resources_since + Duration(30 * 60 * 1000) < gametime && !forcing_upgrade)) {
		initiate_dismantling(site, gametime);
		return true;
	}

	// Mine upgrades are handled by construct_building() Pass 3 (PI-driven).
	// If we reach here, the mine has resources but no upgrade decision yet.
	return false;
}


// =====================================================================
// Player statistics
// =====================================================================

void PlannerAI::update_player_stat(const Time& gametime) {
	if (player_statistics.get_update_time() > Time(0) &&
	    player_statistics.get_update_time() + Duration(15 * 1000) > gametime) {
		return;
	}
	player_statistics.set_update_time(gametime);

	const Widelands::PlayerNumber pn = player_number();
	const Widelands::PlayerNumber nr_players = game().map().get_nrplayers();
	const Widelands::Game::GeneralStatsVector& genstats = game().get_general_statistics();
	const Widelands::Player* me = game().get_player(pn);
	const bool me_def = me->is_defeated();

	// Gather our own stats for diplomacy calculations
	const uint32_t vsize = genstats.at(pn - 1).miltary_strength.size();
	uint32_t me_strength = 0;
	uint32_t me_land = 0;
	uint32_t me_cass = 0;
	uint32_t me_buildings = 0;
	if (vsize > 0 && !me_def) {
		me_strength = genstats.at(pn - 1).miltary_strength.back();
		me_land = genstats.at(pn - 1).land_size.back();
		me_cass = genstats.at(pn - 1).nr_casualties.back();
		me_buildings = genstats.at(pn - 1).nr_buildings.back();
	}

	for (Widelands::PlayerNumber j = 1; j <= nr_players; ++j) {
		const Widelands::Player* this_player = game().get_player(j);
		if (this_player == nullptr) {
			player_statistics.remove_stat(j);
			continue;
		}
		const bool player_def = this_player->is_defeated();

		if (player_def || me_def) {
			player_statistics.add(pn, j, me->team_number(), this_player->team_number(), 0, 0, 0, 0,
			                      0, 0, 0, -20, 0, player_def);
			continue;
		}

		try {
			const uint32_t jvsize = genstats.at(j - 1).miltary_strength.size();
			uint32_t cur_strength = 0;
			uint32_t cur_land = 0;
			uint32_t old_strength = 0;
			uint32_t old60_strength = 0;
			uint32_t old_land = 0;
			uint32_t old60_land = 0;
			uint32_t cass = 0;
			uint32_t buildings = 0;
			if (jvsize > 0) {
				cur_strength = genstats.at(j - 1).miltary_strength.back();
				cur_land = genstats.at(j - 1).land_size.back();
				cass = genstats.at(j - 1).nr_casualties.back();
				buildings = genstats.at(j - 1).nr_buildings.back();

				if (jvsize > 21) {
					old_strength = genstats.at(j - 1).miltary_strength[jvsize - 20];
					old_land = genstats.at(j - 1).land_size[jvsize - 20];
				} else {
					old_strength = genstats.at(j - 1).miltary_strength[0];
					old_land = genstats.at(j - 1).land_size[0];
				}
				if (jvsize > 91) {
					old60_strength = genstats.at(j - 1).miltary_strength[jvsize - 90];
					old60_land = genstats.at(j - 1).land_size[jvsize - 90];
				} else {
					old60_strength = genstats.at(j - 1).miltary_strength[0];
					old60_land = genstats.at(j - 1).land_size[0];
				}
			}

			// Calculate diplomacy score using simple heuristics
			int32_t diplo_score = 0;
			if (game().diplomacy_allowed() && gametime > Time(30000) && pn != j) {
				// Stronger players are more desirable allies
				if (cur_strength > me_strength) {
					diplo_score += 5;
				} else {
					diplo_score -= 2;
				}
				if (cur_strength > 2 * me_strength) {
					diplo_score += 5;
				}
				// Growing players are desirable
				if (cur_strength > old_strength) {
					diplo_score += 3;
				} else {
					diplo_score -= 3;
				}
				if (cur_strength > old60_strength) {
					++diplo_score;
				} else {
					diplo_score -= 5;
				}
				// Land matters
				if (cur_land > me_land) {
					diplo_score += 2;
				} else {
					--diplo_score;
				}
				// Combined strength: together we could be the strongest
				if (cur_strength + me_strength > player_statistics.get_max_power() &&
				    cur_strength < player_statistics.get_max_power() &&
				    me_strength < player_statistics.get_max_power()) {
					diplo_score += 8;
				}
				// Already the strongest: no need for allies
				if (me_strength >= player_statistics.get_max_power()) {
					diplo_score -= 8;
				}
				// Unaligned players are slightly preferred
				if (this_player->team_number() == 0) {
					diplo_score += 4;
				} else {
					--diplo_score;
				}
				// Same team bonus
				if (this_player->team_number() == me->team_number() &&
				    me->team_number() != 0) {
					diplo_score += 5;
				}
				// Don't ally with the already strongest
				if (cur_strength >= player_statistics.get_max_power()) {
					diplo_score -= 10;
				}
				// Large teams are less desirable
				if (player_statistics.members_in_team(this_player->team_number()) >=
				    nr_players / 2) {
					diplo_score -= 4;
				}
				// Early game: be cautious
				if (gametime < Time(30 * 60 * 1000)) {
					diplo_score -= 5;
				}
				// Our casualties vs theirs
				if (me_cass > cass) {
					diplo_score += 2;
				} else {
					--diplo_score;
				}
				// Economy matters
				if (me_buildings < buildings) {
					diplo_score += 3;
				} else {
					--diplo_score;
				}
			}

			player_statistics.add(pn, j, me->team_number(), this_player->team_number(),
			                      cur_strength, old_strength, old60_strength, cass, cur_land,
			                      old_land, old60_land, diplo_score, buildings, player_def);
		} catch (const std::out_of_range&) {
			// Statistics not yet available
		}
	}
	player_statistics.recalculate_team_power();
}

// =====================================================================
// Utilities
// =====================================================================

uint32_t PlannerAI::calculate_stocklevel(Widelands::DescriptionIndex wt) const {
	uint32_t count = 0;
	for (const WarehouseSiteObserver& obs : warehousesites) {
		count += obs.site->get_wares().stock(wt);
	}
	return count;
}

uint32_t PlannerAI::calculate_total_stocklevel(Widelands::DescriptionIndex wt) const {
	// Count ALL wares of this type: warehouse stock + in transit + in queues.
	// Uses Economy's total tracking which includes everything.
	uint32_t count = 0;
	for (const auto& [serial, economy] : player_->economies()) {
		if (economy->type() == Widelands::wwWARE) {
			count += economy->get_wares_or_workers().stock(wt);
		}
	}
	return count;
}

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

bool PlannerAI::set_inputs_to_zero(const ProductionSiteObserver& site) {
	uint16_t remaining_wares = 0;
	for (const auto& queue : site.site->inputqueues()) {
		remaining_wares += queue->get_filled();
		if (queue->get_max_fill() > 0) {
			game().send_player_set_input_max_fill(
			   *site.site, queue->get_index(), queue->get_type(), 0);
		}
	}
	return remaining_wares == 0;
}

void PlannerAI::set_inputs_to_max(const ProductionSiteObserver& site) {
	for (const auto& queue : site.site->inputqueues()) {
		if (queue->get_max_fill() < queue->get_max_size()) {
			game().send_player_set_input_max_fill(
			   *site.site, queue->get_index(), queue->get_type(), queue->get_max_size());
		}
	}
}

void PlannerAI::stop_site(const ProductionSiteObserver& site) {
	if (!site.site->is_stopped()) {
		game().send_player_start_stop_building(*site.site);
	}
}

void PlannerAI::initiate_dismantling(ProductionSiteObserver& site, const Time& gametime) {
	site.dismantle_pending_since = gametime;
	set_inputs_to_zero(site);
	site.bo->construction_decision_time = gametime;
}

// =====================================================================
// Rangers policy
// =====================================================================

void PlannerAI::set_rangers_policy(const Time& /* gametime */) {
	// Use the PI system's ware pressure as the signal for ranger policy.
	// The ware pressure already integrates stock levels, production rates,
	// and demand — it IS the authoritative answer to "do we need more wood?"
	//
	// Policy: kAllowRangers when any ranger output has positive ware pressure
	//         kStopRangers when ALL ranger outputs have zero/negative pressure
	//         kDismantleRangers when pressure has been zero for a sustained period
	//         (tracked via the integral term being near zero)
	for (BuildingObserver& bo : buildings_) {
		if (!bo.is(BuildingAttribute::kRanger)) {
			continue;
		}
		if (wood_policy_.count(bo.id) == 0) {
			wood_policy_[bo.id] = WoodPolicy::kAllowRangers;
		}

		// Query Circle 1 ware pressure for ranger outputs.
		// The lumberjack's output (log/trunk) pressure tells us if wood is needed.
		// The ranger plants trees → lumberjack cuts → output ware.
		// So we check supported buildings' outputs (= the ware chain endpoint).
		int32_t max_output_pressure = 0;
		int32_t max_output_integral = 0;
		for (const auto& output : bo.ware_outputs) {
			if (static_cast<size_t>(output) < ware_pressure_.size()) {
				max_output_pressure =
				   std::max(max_output_pressure, ware_pressure_[output].outputControl);
				max_output_integral =
				   std::max(max_output_integral, ware_pressure_[output].ipart);
			}
		}
		for (const auto& supported : bo.supported_producers) {
			const Widelands::ProductionSiteDescr* prod = supported.second;
			if (prod != nullptr) {
				for (const auto& ware : prod->output_ware_types()) {
					if (static_cast<size_t>(ware) < ware_pressure_.size()) {
						max_output_pressure =
						   std::max(max_output_pressure, ware_pressure_[ware].outputControl);
						max_output_integral =
						   std::max(max_output_integral, ware_pressure_[ware].ipart);
					}
				}
			}
		}

		if (max_output_pressure > 0) {
			// Wood is needed — allow rangers
			wood_policy_[bo.id] = WoodPolicy::kAllowRangers;
		} else if (max_output_integral <= 0) {
			// No pressure AND integral has decayed to zero → sustained oversupply.
			// The integral at 99/100 decay rate halves in ~70 ticks (~5min).
			// If it reached zero, wood hasn't been needed for several minutes.
			wood_policy_[bo.id] = WoodPolicy::kDismantleRangers;
		} else {
			// Pressure is zero but integral still positive → recently needed,
			// may become needed again soon. Stop but don't dismantle.
			wood_policy_[bo.id] = WoodPolicy::kStopRangers;
		}
	}
}


// =====================================================================
// Diplomacy
// =====================================================================

void PlannerAI::diplomacy_actions(const Time& gametime) {
	if (!game().diplomacy_allowed()) {
		return;
	}

	const Widelands::PlayerNumber mypn = player_number();
	const Widelands::Player* me = game().get_player(mypn);
	const Widelands::TeamNumber mytn = me->team_number();
	const bool me_def = me->is_defeated();
	const bool me_alone = player_statistics.get_is_alone(mypn);

	constexpr Widelands::DiplomacyAction kNoAction =
	   static_cast<Widelands::DiplomacyAction>(std::numeric_limits<uint8_t>::max());

	int32_t plan_priority = 0;
	Widelands::DiplomacyAction planned_action = kNoAction;
	Widelands::PlayerNumber planned_opn = 0;

	// If defeated or last in team, leave team
	if (me->team_number() != 0 && (me_alone || me_def)) {
		planned_action = Widelands::DiplomacyAction::kLeaveTeam;
		plan_priority = me_alone ? 0 : std::numeric_limits<int32_t>::max();
	}

	const int32_t my_team_score =
	   me_alone ? 0 : player_statistics.get_team_average_score(mytn, mypn);

	// Check if current team is disadvantageous
	if (planned_action == kNoAction && !me_alone && my_team_score < 0 &&
	    (my_team_score < -10 || RNG::static_rand(8) == 0)) {
		planned_action = Widelands::DiplomacyAction::kLeaveTeam;
		plan_priority = -my_team_score;
	}

	// Check for undesirable teammates
	if (planned_action == kNoAction && player_statistics.get_worst_ally_score() < -15) {
		const uint8_t my_team_size = player_statistics.members_in_team(mytn);
		const int32_t team_vs_worst =
		   (my_team_size < 3 ? 0 : my_team_score) + player_statistics.get_worst_ally_score();
		if (team_vs_worst <= 0 || RNG::static_rand(my_team_size * 2) == 0) {
			planned_action = Widelands::DiplomacyAction::kLeaveTeam;
			plan_priority = std::max(0, -team_vs_worst);
		}
	}

	// Process pending diplomacy requests
	for (const Widelands::Game::PendingDiplomacyAction& pda : game().pending_diplomacy_actions()) {
		if (pda.other != mypn) {
			continue;
		}

		const int32_t diploscore = player_statistics.get_diplo_score(pda.sender);
		const Widelands::TeamNumber other_tn = player_statistics.get_team_number(pda.sender);
		int32_t priority = 0;
		const bool plan_to_leave = planned_action == Widelands::DiplomacyAction::kLeaveTeam;

		bool accept =
		   !me_def && player_statistics.members_in_team(
		                 pda.action == Widelands::DiplomacyAction::kInvite ? other_tn : mytn) <
		                 player_statistics.players_active() - 1;

		accept =
		   accept && (diploscore >= std::max(my_team_score, 25) ||
		              (diploscore > std::max(my_team_score / 2, 15) && RNG::static_rand(2) == 0));

		if (pda.action == Widelands::DiplomacyAction::kJoin && accept) {
			priority = diploscore / std::max<uint8_t>(player_statistics.members_in_team(mytn), 1);
		}

		if (pda.action == Widelands::DiplomacyAction::kInvite && accept) {
			const bool other_alone = player_statistics.get_is_alone(pda.sender);
			const int32_t ots = other_alone ? diploscore - RNG::static_rand(10) :
			                                  player_statistics.get_team_average_score(other_tn);
			const int32_t my_effective_ts = plan_to_leave ? 0 : my_team_score;
			accept = ots > my_effective_ts;
			priority = ots - my_effective_ts;
			if (accept && plan_to_leave) {
				plan_priority = 0;
			}
		}

		accept = accept && (planned_action == kNoAction || plan_priority < priority);

		if (!accept) {
			game().send_player_diplomacy(pda.other,
			                             (pda.action == Widelands::DiplomacyAction::kInvite ?
			                                 Widelands::DiplomacyAction::kRefuseInvite :
			                                 Widelands::DiplomacyAction::kRefuseJoin),
			                             pda.sender);
		} else {
			if (planned_action != kNoAction && planned_action != Widelands::DiplomacyAction::kLeaveTeam) {
				game().send_player_diplomacy(
				   mypn,
				   (planned_action == Widelands::DiplomacyAction::kAcceptInvite ?
				       Widelands::DiplomacyAction::kRefuseInvite :
				       Widelands::DiplomacyAction::kRefuseJoin),
				   planned_opn);
			}
			planned_action = pda.action == Widelands::DiplomacyAction::kInvite ?
			                    Widelands::DiplomacyAction::kAcceptInvite :
			                    Widelands::DiplomacyAction::kAcceptJoin;
			planned_opn = pda.sender;
			plan_priority = priority;
		}
	}

	// Execute planned action
	if (planned_action != kNoAction) {
		game().send_player_diplomacy(mypn, planned_action, planned_opn);
		return;
	}

	// Proactively invite/join strong players
	for (Widelands::PlayerNumber opn = 1; opn <= game().map().get_nrplayers(); ++opn) {
		const Widelands::Player* other_player = game().get_player(opn);
		if (other_player == nullptr || opn == mypn ||
		    player_statistics.player_diplo_requested_lately(opn, gametime)) {
			continue;
		}
		if (player_statistics.get_diplo_score(opn) >= 35) {
			player_statistics.join_or_invite(opn, game(), gametime);
		}
	}
}

}  // namespace AI
