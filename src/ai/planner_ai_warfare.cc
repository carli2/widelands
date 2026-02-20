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

#include <algorithm>
#include <cstdlib>

#include "ai/planner_ai.h"
#include "base/log.h"
#include "economy/wares_queue.h"
#include "logic/game.h"
#include "logic/map.h"
#include "logic/map_objects/descriptions.h"
#include "logic/map_objects/findimmovable.h"
#include "logic/map_objects/tribes/militarysite.h"
#include "logic/map_objects/tribes/productionsite.h"
#include "logic/map_objects/tribes/tribe_descr.h"
#include "logic/map_objects/tribes/warehouse.h"
#include "logic/player.h"

namespace AI {

// Soldier training value: how expensive was this soldier to train?
// A rookie (all level 0) costs 1 unit. Each training level adds 1 unit.
// This approximates the ware cost of training (weapons, armor, food).
static int32_t soldier_training_value(const Widelands::Soldier* s) {
	return 1 + static_cast<int32_t>(s->get_attack_level()) +
	       static_cast<int32_t>(s->get_defense_level()) +
	       static_cast<int32_t>(s->get_evade_level()) +
	       static_cast<int32_t>(s->get_health_level());
}

// Per-soldier combat power: effective fighting strength considering
// current health. A soldier at 10% HP has only 10% of their full power.
static float soldier_combat_power(const Widelands::Soldier* s) {
	const Widelands::SoldierDescr& d = s->descr();
	const float attack = static_cast<float>(
	   d.get_base_min_attack() +
	   (d.get_base_max_attack() - d.get_base_min_attack()) / 2 +
	   d.get_attack_incr_per_level() * s->get_attack_level());
	const float health = static_cast<float>(s->get_current_health());
	const float defense = static_cast<float>(
	   100 - d.get_base_defense() - d.get_defense_incr_per_level() * s->get_defense_level());
	const float evade = static_cast<float>(
	   100 - d.get_base_evade() - d.get_evade_incr_per_level() / 100 * s->get_evade_level());
	// Avoid division by zero
	return (attack * health) / (std::max(1.0f, defense) * std::max(1.0f, evade));
}

// Fight prediction result.
struct FightPrediction {
	int32_t our_casualties{0};       // soldiers we lose
	int32_t our_value_lost{0};       // sum of training value of our dead
	int32_t enemy_casualties{0};     // soldiers enemy loses
	int32_t enemy_value_lost{0};     // sum of training value of enemy dead
	int32_t our_survivors_hp_pct{0}; // avg HP% of our surviving soldiers
};

// Predict fight outcome between our attackers and enemy defenders.
// Simulates sequential 1v1 combat (strongest vs strongest).
// Each winner continues to next fight with reduced HP.
static FightPrediction predict_fight(
   const std::vector<Widelands::Soldier*>& our_soldiers,
   const std::vector<Widelands::Soldier*>& enemy_soldiers) {

	FightPrediction result;
	if (our_soldiers.empty()) {
		result.enemy_casualties = 0;
		return result;
	}

	// Build combat data for each soldier
	struct CombatUnit {
		float power;           // current combat power (decreases with HP)
		float hp_fraction;     // current HP as fraction of max [0..1]
		float max_hp;          // max possible HP
		int32_t value;         // training value (buildup cost)
	};

	std::vector<CombatUnit> ours;
	ours.reserve(our_soldiers.size());
	for (const auto* s : our_soldiers) {
		CombatUnit u;
		u.power = soldier_combat_power(s);
		u.max_hp = static_cast<float>(s->get_max_health());
		u.hp_fraction = (u.max_hp > 0) ?
		   static_cast<float>(s->get_current_health()) / u.max_hp : 0.0f;
		u.value = soldier_training_value(s);
		ours.push_back(u);
	}
	std::vector<CombatUnit> theirs;
	theirs.reserve(enemy_soldiers.size());
	for (const auto* s : enemy_soldiers) {
		CombatUnit u;
		u.power = soldier_combat_power(s);
		u.max_hp = static_cast<float>(s->get_max_health());
		u.hp_fraction = (u.max_hp > 0) ?
		   static_cast<float>(s->get_current_health()) / u.max_hp : 0.0f;
		u.value = soldier_training_value(s);
		theirs.push_back(u);
	}

	// Sort: strongest first (they fight first in Widelands)
	std::sort(ours.begin(), ours.end(),
	   [](const CombatUnit& a, const CombatUnit& b) { return a.power > b.power; });
	std::sort(theirs.begin(), theirs.end(),
	   [](const CombatUnit& a, const CombatUnit& b) { return a.power > b.power; });

	// Simulate sequential 1v1 fights
	size_t our_idx = 0;
	size_t their_idx = 0;
	while (our_idx < ours.size() && their_idx < theirs.size()) {
		CombatUnit& a = ours[our_idx];
		CombatUnit& d = theirs[their_idx];

		// Effective power = base power × HP fraction
		// A 10% HP soldier has only 10% of their combat effectiveness.
		const float a_eff = a.power * a.hp_fraction;
		const float d_eff = d.power * d.hp_fraction;
		const float total = a_eff + d_eff;

		if (total <= 0) {
			// Both dead, skip
			++our_idx;
			++their_idx;
			continue;
		}

		if (a_eff >= d_eff) {
			// Attacker wins: defender dies, attacker loses HP
			// HP loss proportional to defender's power relative to attacker's
			const float damage_ratio = d_eff / (a_eff + 1.0f);
			a.hp_fraction *= (1.0f - damage_ratio);
			result.enemy_casualties++;
			result.enemy_value_lost += d.value;
			++their_idx;

			// If winner is below 10% HP, they're effectively dead too
			if (a.hp_fraction < 0.10f) {
				result.our_casualties++;
				result.our_value_lost += a.value;
				++our_idx;
			}
		} else {
			// Defender wins: attacker dies, defender loses HP
			const float damage_ratio = a_eff / (d_eff + 1.0f);
			d.hp_fraction *= (1.0f - damage_ratio);
			result.our_casualties++;
			result.our_value_lost += a.value;
			++our_idx;

			if (d.hp_fraction < 0.10f) {
				result.enemy_casualties++;
				result.enemy_value_lost += d.value;
				++their_idx;
			}
		}
	}

	// Average HP of our survivors
	int32_t survivor_count = 0;
	float total_hp_pct = 0;
	for (size_t i = our_idx; i < ours.size(); ++i) {
		if (ours[i].hp_fraction >= 0.10f) {
			total_hp_pct += ours[i].hp_fraction * 100.0f;
			++survivor_count;
		} else {
			result.our_casualties++;
			result.our_value_lost += ours[i].value;
		}
	}
	// Count remaining enemies as enemy survivors (not killed)
	// Our soldiers that already fought are counted above
	for (size_t i = 0; i < our_idx && i < ours.size(); ++i) {
		if (ours[i].hp_fraction >= 0.10f) {
			total_hp_pct += ours[i].hp_fraction * 100.0f;
			++survivor_count;
		}
	}
	result.our_survivors_hp_pct = (survivor_count > 0) ?
	   static_cast<int32_t>(total_hp_pct / static_cast<float>(survivor_count)) : 0;

	return result;
}

// Scan military sites for nearby enemy buildings, evaluate attack targets,
// and execute attacks using cost/benefit scoring.
bool PlannerAI::check_enemy_sites(const Time& gametime) {

	const Widelands::Map& map = game().map();
	Widelands::PlayerNumber const pn = player_number();

	const uint32_t my_power = player_statistics.get_modified_player_power(pn);

	// First we scan vicinity of our military sites to discover new enemy sites.
	// Militarysites rotate (see check_militarysites()).
	int32_t i = 0;
	for (const MilitarySiteObserver& mso : militarysites) {
		++i;
		if (i % 4 == 0) {
			continue;
		}
		if (i > 20) {
			continue;
		}

		Widelands::MilitarySite* ms = mso.site;
		uint32_t const vision = ms->descr().vision_range();
		Widelands::FCoords f = map.get_fcoords(ms->get_position());

		// Get list of immovables around this military site
		static std::vector<Widelands::ImmovableFound> immovables;
		immovables.clear();
		immovables.reserve(40);
		map.find_immovables(
		   game(), Widelands::Area<Widelands::FCoords>(f, (vision + 3 < 13) ? 13 : vision + 3),
		   &immovables, Widelands::FindImmovableAttackTarget());

		for (const Widelands::ImmovableFound& imm_found : immovables) {
			if (upcast(Widelands::MilitarySite const, bld, imm_found.object)) {
				const Widelands::PlayerNumber opn = bld->owner().player_number();
				if (player_statistics.get_is_enemy(opn)) {
					assert(opn != pn);
					player_statistics.set_last_time_seen(gametime, opn);
					if (enemy_sites.count(bld->get_position().hash()) == 0) {
						enemy_sites[bld->get_position().hash()] = EnemySiteObserver();
					} else {
						enemy_sites[bld->get_position().hash()].last_time_seen = gametime;
					}
				}
			}
			if (upcast(Widelands::Warehouse const, wh, imm_found.object)) {
				const Widelands::PlayerNumber opn = wh->owner().player_number();
				if (player_statistics.get_is_enemy(opn)) {
					assert(opn != pn);
					player_statistics.set_last_time_seen(gametime, opn);
					if (enemy_sites.count(wh->get_position().hash()) == 0) {
						enemy_sites[wh->get_position().hash()] = EnemySiteObserver();
					} else {
						enemy_sites[wh->get_position().hash()].last_time_seen = gametime;
					}
				}
			}
		}
	}

	// Now evaluate targets
	Widelands::Serial best_target = Widelands::kInvalidSerial;
	int32_t best_score = 0;
	uint32_t count = 0;
	// Sites that were either conquered or destroyed
	static std::vector<uint32_t> disappeared_sites;
	disappeared_sites.clear();
	disappeared_sites.reserve(6);

	// Removing sites we saw too long ago
	for (const auto& observer : enemy_sites) {
		if (observer.second.last_time_seen + Duration(20 * 60 * 1000) < gametime) {
			disappeared_sites.push_back(observer.first);
		}
	}
	while (!disappeared_sites.empty()) {
		enemy_sites.erase(disappeared_sites.back());
		disappeared_sites.pop_back();
	}

	for (auto& observer : enemy_sites) {
		assert(observer.second.last_time_attacked <= gametime);
		// Do not attack too soon
		if (Duration(std::min<uint32_t>(observer.second.attack_counter, 10) * 20 * 1000) >
		    (gametime - observer.second.last_time_attacked)) {
			continue;
		}

		++count;
		// We test max 12 sites and prefer ones tested more than 1 min ago
		if (((observer.second.last_tested + enemysites_check_delay_ * 1000) > gametime &&
		     count > 4) ||
		    count > 12) {
			continue;
		}

		observer.second.last_tested = gametime;
		// Resetting some values
		uint16_t enemy_military_presence_in_region_ = 0;
		uint16_t enemy_military_sites_in_region_ = 0;
		uint8_t defenders_strength = 0;
		bool is_warehouse = false;
		bool is_attackable = false;
		const bool is_visible = player_->is_seeing(
		   Widelands::Map::get_index(Widelands::Coords::unhash(observer.first), map.get_width()));
		uint16_t owner_number = 100;

		// Testing if we can attack the building
		Widelands::FCoords f = map.get_fcoords(Widelands::Coords::unhash(observer.first));
		Widelands::Flag* flag = nullptr;

		// Collect defenders and attackers (kept alive for fight prediction)
		std::vector<Widelands::Soldier*> target_defenders;
		int32_t target_build_cost = 0;  // building materials of target

		if (upcast(Widelands::MilitarySite, bld, f.field->get_immovable())) {
			if (player_->is_hostile(bld->owner())) {
				target_defenders = bld->soldier_control()->present_soldiers();
				defenders_strength = calculate_strength(target_defenders);
				for (const auto& item : bld->descr().buildcost()) {
					target_build_cost += item.second;
				}
				flag = &bld->base_flag();
				if (is_visible && bld->attack_target()->can_be_attacked()) {
					is_attackable = true;
				}
				owner_number = bld->owner().player_number();
			}
		}
		if (upcast(Widelands::Warehouse, wh, f.field->get_immovable())) {
			if (player_->is_hostile(wh->owner())) {
				target_defenders = wh->soldier_control()->present_soldiers();
				defenders_strength = calculate_strength(target_defenders);
				flag = &wh->base_flag();
				is_warehouse = true;
				if (is_visible && wh->attack_target()->can_be_attacked()) {
					is_attackable = true;
				}
				owner_number = wh->owner().player_number();
			}
		}

		// If flag is defined it is a good target
		if (flag != nullptr) {

			// Site is still there but not visible for us
			if (!is_visible) {
				if (observer.second.last_time_seen + Duration(20 * 60 * 1000) < gametime) {
					verb_log_dbg_time(
					   gametime, "site %u not visible for more than 20 minutes\n", observer.first);
					disappeared_sites.push_back(observer.first);
				}
				continue;
			}

			// Updating info on mines nearby if needed
			if (observer.second.mines_nearby == ExtendedBool::kUnset) {
				FindNodeMineable find_mines_spots_nearby(game(), f.field->get_resources());
				const int32_t minescount = map.find_fields(
				   game(), Widelands::Area<Widelands::FCoords>(f, 6), nullptr, find_mines_spots_nearby);
				if (minescount > 0) {
					observer.second.mines_nearby = ExtendedBool::kTrue;
				} else {
					observer.second.mines_nearby = ExtendedBool::kFalse;
				}
			}

			observer.second.is_warehouse = is_warehouse;

			// Collect our attackers (kept alive for fight prediction)
			std::vector<Widelands::Soldier*> our_attackers;
			if (is_attackable) {
				player_->find_attack_soldiers(*flag, &our_attackers);
				if (our_attackers.empty()) {
					observer.second.attack_soldiers_strength = 0;
				} else {
					int32_t strength = calculate_strength(our_attackers);
					observer.second.attack_soldiers_strength = strength;
					assert(strength >= 0);
					observer.second.attack_soldiers_competency =
					   strength * 10 / static_cast<int32_t>(our_attackers.size());
				}
			} else {
				observer.second.attack_soldiers_strength = 0;
			}

			observer.second.defenders_strength = defenders_strength;

			observer.second.score = 0;
			const uint16_t enemys_power = player_statistics.get_modified_player_power(owner_number);
			if (observer.second.attack_soldiers_strength > 0 &&
			    !player_statistics.players_in_same_team(pn, owner_number)) {

				// Collect ALL enemy defenders: target building + reinforcements
				// from nearby military buildings. In Widelands, nearby garrisons
				// send soldiers to help defend, so we fight them ALL.
				std::vector<Widelands::Soldier*> all_defenders = target_defenders;

				static std::vector<Widelands::ImmovableFound> immovables;
				immovables.reserve(50);
				immovables.clear();
				static std::set<uint32_t> unique_serials;
				unique_serials.clear();
				map.find_immovables(game(),
				                    Widelands::Area<Widelands::FCoords>(
				                       map.get_fcoords(Widelands::Coords::unhash(observer.first)), 10),
				                    &immovables);
				for (const Widelands::ImmovableFound& im_found : immovables) {
					const Widelands::BaseImmovable& base_immovable = *im_found.object;

					if (!unique_serials.insert(base_immovable.serial()).second) {
						continue;
					}

					if (upcast(Widelands::Building const, building, &base_immovable)) {
						const Widelands::PlayerNumber bpn = building->owner().player_number();
						if (player_statistics.get_is_enemy(bpn)) {
							assert(!player_statistics.players_in_same_team(bpn, pn));
							// Collect reinforcement soldiers from nearby buildings.
							// Each nearby military building sends all but 1 soldier.
							if (upcast(Widelands::MilitarySite const, militarysite, building)) {
								auto reinforcements =
								   militarysite->soldier_control()->present_soldiers();
								// Keep 1 for defense of that building
								if (!reinforcements.empty()) {
									reinforcements.erase(reinforcements.begin());
								}
								all_defenders.insert(all_defenders.end(),
								   reinforcements.begin(), reinforcements.end());
								++enemy_military_sites_in_region_;
							}
							if (upcast(Widelands::Warehouse const, warehouse, building)) {
								auto reinforcements =
								   warehouse->soldier_control()->present_soldiers();
								all_defenders.insert(all_defenders.end(),
								   reinforcements.begin(), reinforcements.end());
								++enemy_military_sites_in_region_;
							}
						}
					}
				}
				enemy_military_presence_in_region_ =
				   static_cast<uint16_t>(all_defenders.size() - target_defenders.size());
				observer.second.enemy_military_presence_in_region = enemy_military_presence_in_region_;
				observer.second.enemy_military_sites_in_region = enemy_military_sites_in_region_;

				// --- Cost/benefit attack scoring with fight prediction ---
				//
				// Predict the actual fight: which soldiers die on each side,
				// what is each soldier's training buildup cost, what do we
				// lose vs what does the enemy lose. Considers current HP
				// (a 10% HP enemy is nearly dead = easy to beat).
				//
				// Score = (enemy_losses + territory_gain) - our_losses
				// All values in "score points".

				const int32_t nr_wares = std::max<int32_t>(1, static_cast<int32_t>(wares.size()));

				// Run fight prediction with actual soldier data
				const FightPrediction fp = predict_fight(our_attackers, all_defenders);

				// === OUR COST (what we lose) ===
				// Each dead soldier costs their training value (buildup cost).
				// Survivors with low HP cost proportionally (time to heal).
				// Survivor health damage: 100% HP = no cost, 0% = full cost.
				int32_t our_cost = fp.our_value_lost;
				// Survivors' HP loss: each % lost = fraction of a training unit
				const int32_t survivors = static_cast<int32_t>(our_attackers.size()) -
				   fp.our_casualties;
				if (survivors > 0 && fp.our_survivors_hp_pct < 100) {
					our_cost += survivors * (100 - fp.our_survivors_hp_pct) / 100;
				}

				// === ENEMY LOSS (what they lose) ===
				// Soldier casualties × training value (from fight prediction)
				int32_t enemy_loss = fp.enemy_value_lost;

				// Building materials lost by enemy, weighted by THEIR scarcity.
				// Each lost ware is worth its scarcity score (0..100).
				// Military building: enemy loses the build cost wares.
				// Warehouse: enemy loses stored items (estimate: nr_wares items
				// at average scarcity).
				if (is_warehouse) {
					// Warehouse: estimate stored items at average scarcity
					int32_t avg_scarcity = 0;
					const auto& scarcity = get_enemy_ware_scarcity(owner_number);
					for (const auto& [wi, sc] : scarcity) {
						avg_scarcity += sc;
					}
					avg_scarcity /= std::max<int32_t>(1, static_cast<int32_t>(scarcity.size()));
					enemy_loss += nr_wares * avg_scarcity / 20;
				} else if (!is_warehouse && f.field->get_immovable() != nullptr) {
					// Military building: enemy loses build cost wares at their scarcity
					if (upcast(Widelands::Building const, target_bld,
					           f.field->get_immovable())) {
						for (const auto& [ware_idx, qty] : target_bld->descr().buildcost()) {
							enemy_loss += enemy_ware_value(owner_number, ware_idx) *
							   static_cast<int32_t>(qty) / 20;
						}
					}
				}

				// === OUR GAINS (what we win beyond enemy losses) ===
				// Dismantle materials from conquered building (~50% recovery)
				// Weighted by OUR scarcity (how much we value those materials)
				int32_t our_gains = 0;
				if (!is_warehouse && f.field->get_immovable() != nullptr) {
					if (upcast(Widelands::Building const, target_bld,
					           f.field->get_immovable())) {
						for (const auto& [ware_idx, qty] : target_bld->descr().buildcost()) {
							// We recover ~50% of build materials
							our_gains += static_cast<int32_t>(qty) / 2;
						}
					}
				}
				// Won soldiers: enemy garrison survivors may join us
				// (approximation: we gain defenders_count - enemy_casualties soldiers)
				const int32_t won_soldiers = std::max<int32_t>(0,
				   static_cast<int32_t>(target_defenders.size()) - fp.enemy_casualties);
				our_gains += won_soldiers;

				// === TERRITORY VALUE (in ware-pressure units) ===
				// Each conquered field has a production value estimated
				// from the most valuable building that could use it.
				// Zero-sum: we GAIN that value AND the enemy LOSES it.
				// So territory value = 2 × field_count × per_field_value.
				int32_t conquer_area = 0;
				if (!is_warehouse) {
					if (upcast(Widelands::MilitarySite const, target_ms,
					           f.field->get_immovable())) {
						const int32_t cr = static_cast<int32_t>(
						   target_ms->descr().get_conquers());
						conquer_area = cr * cr;
					}
				}
				// Land value: each field × land_value_per_field_ (ware units).
				// Divide by a scaling factor so it fits the score range.
				// Zero-sum factor of 2: our gain + their loss.
				const int32_t land_val_scale = std::max<int32_t>(1,
				   kNormalizationBudget / std::max<int32_t>(1, nr_wares));
				int32_t territory_value = static_cast<int32_t>(
				   static_cast<int64_t>(2 * conquer_area) *
				   land_value_per_field_ / land_val_scale);
				// Mine fields nearby: extra value (non-renewable resources)
				if (observer.second.mines_nearby == ExtendedBool::kTrue) {
					territory_value += territory_value / 2;
				}
				// Expansion priority for this enemy (from Circle 3)
				if (owner_number < expansion_targets_.size()) {
					territory_value += static_cast<int32_t>(
					   static_cast<int64_t>(expansion_targets_[owner_number].outputControl) *
					   (conquer_area + 1) / kNormalizationBudget);
				}

				// === NET SCORE ===
				// ALWAYS account the casualty exchange — even if we lose,
				// killing expensive enemy soldiers while losing cheap recruits
				// can be profitable. The fight is a value exchange.
				//
				// If we WIN: full benefit (enemy loss + territory + our gains - cost)
				// If we LOSE: only casualty exchange (enemy_loss - our_cost)
				//   Territory and building gains are NOT counted because we
				//   won't hold the building if we lose.
				int32_t net;
				if (fp.our_casualties < static_cast<int32_t>(our_attackers.size())) {
					// We have survivors → we likely win (or draw)
					net = (enemy_loss + our_gains + territory_value) - our_cost;
				} else {
					// Total loss: only casualty exchange matters
					net = enemy_loss - our_cost;
				}

				// HQ Hunter bonus: warehouses are the PRIMARY target.
				// Destroying an enemy HQ wins the game, so massively
				// boost the score for warehouse attacks.
				const std::string& wc_atk = game().get_win_condition_displayname();
				if (is_warehouse && wc_atk == "HQ Hunter") {
					net += 50;  // huge bonus — worth sacrificing soldiers
				}

				// Global power check: if globally weaker, penalize
				int32_t power_penalty = 0;
				if (enemys_power > 0 && my_power * 100 / enemys_power < 80) {
					power_penalty = std::max<int32_t>(0, net) / 2;
				}

				int16_t score = static_cast<int16_t>(std::clamp<int32_t>(
				   net - power_penalty, -100, 100));

				// Recently attacked cooldown
				if (observer.second.last_time_attacked + Duration(2 * 60 * 1000) > gametime) {
					score -= static_cast<int16_t>(
					   std::min<uint32_t>(observer.second.attack_counter, 5));
				}

				// Per-target attack integral: accumulate when profitable,
				// decay when not. The integral models "sustained interest"
				// — only attack after considering it for multiple ticks.
				// Decay: 3/4 per tick (fast forgetting for bad targets).
				// Accumulation: add score when positive.
				if (score > 0) {
					observer.second.attack_integral =
					   observer.second.attack_integral * 3 / 4 + score;
				} else {
					// Negative or zero: decay the integral
					observer.second.attack_integral =
					   observer.second.attack_integral * 3 / 4 + score;
					// Clamp to prevent deeply negative integrals that take
					// forever to recover from
					observer.second.attack_integral = std::max<int32_t>(
					   -200, observer.second.attack_integral);
				}
				observer.second.score = score;
			}

			// Attack threshold: only consider targets whose integral
			// has accumulated enough sustained interest. A single positive
			// tick (score=50) gives integral=50. After 4 ticks of +50:
			// integral ≈ 50*(1 + 3/4 + 9/16 + 27/64) ≈ 145.
			// Threshold of 100 means: ~2-3 ticks of sustained interest.
			if (observer.second.attack_integral > 100) {
				assert(is_visible);
				if (observer.second.attack_integral > best_score) {
					best_score = observer.second.attack_integral;
					best_target = observer.first;
				}
			}

		} else {  // No flag = site does not exist anymore
			disappeared_sites.push_back(observer.first);
		}
	}

	while (!disappeared_sites.empty()) {
		enemy_sites.erase(disappeared_sites.back());
		disappeared_sites.pop_back();
	}

	// Modifying enemysites_check_delay_ based on count of enemy sites
	if (enemy_sites.size() >= 13 && enemysites_check_delay_ < Duration(180)) {
		enemysites_check_delay_ += Duration(3);
	}
	if (enemy_sites.size() < 10 && enemysites_check_delay_ > Duration(30)) {
		enemysites_check_delay_ -= Duration(2);
	}

	// If no valid target found
	if (best_target == Widelands::kInvalidSerial) {
		return false;
	}

	assert(enemy_sites.count(best_target) > 0);

	// Attacking
	Widelands::FCoords f = map.get_fcoords(Widelands::Coords::unhash(best_target));

	Widelands::Flag* flag = nullptr;
	if (upcast(Widelands::MilitarySite, bld, f.field->get_immovable())) {
		flag = &bld->base_flag();
	} else if (upcast(Widelands::Warehouse, Wh, f.field->get_immovable())) {
		flag = &Wh->base_flag();
	} else {
		return false;  // should not happen
	}

	// How many attack soldiers can we send?
	std::vector<Widelands::Soldier*> soldiers;
	int32_t attackers = player_->find_attack_soldiers(*flag, &soldiers);
	assert(attackers < 500);

	// Attack group size: derived from target's defender strength.
	// Send enough to win convincingly (2× defender strength) but keep reserves.
	// Minimum: defender_strength + 2 (enough to overwhelm + margin).
	// Maximum: all available if target is a warehouse (high value).
	const int32_t def_strength = enemy_sites[best_target].defenders_strength;
	const bool target_is_wh = enemy_sites[best_target].is_warehouse;
	int32_t attack_group = std::max<int32_t>(3, def_strength * 2 + 2);
	if (target_is_wh) {
		attack_group = attackers;  // all-in for warehouses
	}
	if (attackers > attack_group) {
		attackers = attack_group;
	}

	assert(attackers < 500);

	if (attackers <= 0) {
		return false;
	}

	std::vector<Widelands::Serial> attacking_soldiers;
	const Widelands::SoldierDescr& descr = soldiers.front()->descr();
	int a = 0;  // counter of chosen soldiers
	int b = 0;  // counter of attempts to choose
	while (a < attackers && b < static_cast<int32_t>(soldiers.size())) {
		// Only healthy soldiers are chosen - require at least 70% health
		uint32_t maxhealth = ((descr.get_base_health() +
		                       descr.get_health_incr_per_level() * soldiers[b]->get_health_level()) *
		                      70 / 100);
		if (soldiers[b]->get_current_health() > maxhealth) {
			attacking_soldiers.push_back(soldiers[b]->serial());
			++a;
		}
		++b;
	}
	verb_log_info_time(
	   gametime,
	   "%2u: attacking site at %3dx%3d, score %3d, with %2d soldiers, attacking %2u times, after "
	   "%5u seconds\n",
	   static_cast<unsigned>(player_number()), flag->get_position().x, flag->get_position().y,
	   best_score, a, enemy_sites[best_target].attack_counter + 1,
	   (gametime - enemy_sites[best_target].last_time_attacked).get() / 1000);

	game().send_player_attack(*flag, player_number(), attacking_soldiers, true);
	assert(player_->is_seeing(
	   Widelands::Map::get_index(flag->get_building()->get_position(), map.get_width())));
	attackers_count_ += attackers;
	enemy_sites[best_target].last_time_attacked = gametime;
	++enemy_sites[best_target].attack_counter;

	last_attack_time_ = gametime;
	persistent_data->last_attacked_player = flag->owner().player_number();

	return true;
}

// Count vacant soldier positions across military sites, training sites, and warehouses.
// Sets soldier_status_ based on vacant positions vs garrison count.
void PlannerAI::count_military_vacant_positions() {
	int32_t vacant_mil_positions_ = 0;
	int32_t understaffed_ = 0;
	int32_t on_stock_ = 0;

	// Count all soldiers too
	int32_t soldiers_counted = 0;

	for (TrainingSiteObserver tso : trainingsites) {
		vacant_mil_positions_ +=
		   5 * std::min<int32_t>((tso.site->soldier_control()->soldier_capacity() -
		                          tso.site->soldier_control()->associated_soldiers().size()),
		                         2);
		soldiers_counted += tso.site->soldier_control()->associated_soldiers().size();
	}
	for (const MilitarySiteObserver& mso : militarysites) {
		vacant_mil_positions_ += mso.site->soldier_control()->soldier_capacity() -
		                         mso.site->soldier_control()->associated_soldiers().size();
		understaffed_ += mso.understaffed;
		soldiers_counted += mso.site->soldier_control()->associated_soldiers().size();
	}

	int32_t garrisons_count = militarysites.size();

	// Also available in warehouses
	for (auto wh : warehousesites) {
		if (wh.bo->is(BuildingAttribute::kPort)) {
			assert(wh.site->soldier_control()->max_soldier_capacity() > 0);
			++garrisons_count;
			assert(wh.site->get_desired_soldier_count() <= kPortDefaultGarrison * 3);
			understaffed_ += kPortDefaultGarrison * 3 - wh.site->get_desired_soldier_count();
		}

		uint32_t wh_associated = wh.site->soldier_control()->associated_soldiers().size();
		if (wh_associated < wh.site->get_desired_soldier_count()) {
			vacant_mil_positions_ += wh.site->get_desired_soldier_count() - wh_associated;
		} else {
			on_stock_ += wh_associated - wh.site->get_desired_soldier_count();
		}
		soldiers_counted += wh_associated;
	}

	// Unassociated soldiers on the roads are counted as stock.
	int32_t total_soldiers = player_->count_soldiers();
	if (soldiers_counted < total_soldiers) {
		int32_t remaining_soldiers = total_soldiers - soldiers_counted;
		verb_log_dbg_time(game().get_gametime(),
		                  "AI %u: soldiers: total: %d, associated: %d, difference: %d",
		                  static_cast<unsigned>(player_number()), total_soldiers, soldiers_counted,
		                  remaining_soldiers);
		on_stock_ += remaining_soldiers;
	} else if (soldiers_counted > total_soldiers) {
		log_warn_time(game().get_gametime(),
		              "AI %u: soldiers: total: %d, associated: %d, unexpected: %d",
		              static_cast<unsigned>(player_number()), total_soldiers, soldiers_counted,
		              soldiers_counted - total_soldiers);
	}

	vacant_mil_positions_ += understaffed_;

	// May become negative, but that doesn't seem to be a problem
	vacant_mil_positions_ -= on_stock_;

	if (vacant_mil_positions_ <= 1 || on_stock_ > 4) {
		soldier_status_ = SoldiersStatus::kFull;
	} else if (vacant_mil_positions_ * 4 <= garrisons_count || on_stock_ > 2) {
		soldier_status_ = SoldiersStatus::kEnough;
	} else if (vacant_mil_positions_ > garrisons_count) {
		soldier_status_ = SoldiersStatus::kBadShortage;
	} else {
		soldier_status_ = SoldiersStatus::kShortage;
	}

	assert(soldier_status_ == SoldiersStatus::kFull || soldier_status_ == SoldiersStatus::kEnough ||
	       soldier_status_ == SoldiersStatus::kShortage ||
	       soldier_status_ == SoldiersStatus::kBadShortage);
}

// Manage training sites: capacity, input queues, upgrades.
// Uses pressure-based logic instead of neural network scoring.
bool PlannerAI::check_trainingsites(const Time& gametime) {

	if (trainingsites.empty()) {
		return false;
	}

	// Patterns to identify weapons and armors for input queue management
	static const std::vector<std::string> armors_and_weapons = {
	   "ax",      "armor",  "boots",  "garment", "helm",  "padded", "sword",
	   "trident", "tabard", "shield", "mask",    "spear", "warrior"};

	trainingsites.push_back(trainingsites.front());
	trainingsites.pop_front();

	Widelands::TrainingSite* ts = trainingsites.front().site;
	TrainingSiteObserver& tso = trainingsites.front();

	const Widelands::DescriptionIndex enhancement = ts->descr().enhancement();

	if (enhancement != Widelands::INVALID_INDEX && ts_without_trainers_ == 0 &&
	    ts_finished_count_ > 1 && ts_in_const_count_ == 0) {

		// Consider enhancing this training site.
		// Check that:
		// 1. Building is allowed
		// 2. AI limit is not exceeded
		// 3. We have enough material to construct it
		BuildingObserver& en_bo =
		   get_building_observer(tribe_->get_building_descr(enhancement)->name().c_str());
		uint16_t current_proportion =
		   en_bo.total_count() * 100 / (ts_finished_count_ + ts_in_const_count_);
		en_bo.build_material_shortage = false;
		uint8_t shortage_counter = 0;
		// Check we have enough critical material on stock
		for (uint32_t m = 0; m < en_bo.critical_building_material.size(); ++m) {
			Widelands::DescriptionIndex wt(
			   static_cast<size_t>(en_bo.critical_building_material.at(m)));
			if (calculate_stocklevel(wt) <= 2) {
				shortage_counter++;
				en_bo.build_material_shortage = true;
			}
		}
		if (player_->is_building_type_allowed(enhancement) &&
		    en_bo.aimode_limit_status() == AiModeBuildings::kAnotherAllowed &&
		    en_bo.max_trainingsites_proportion > current_proportion && shortage_counter < 2) {
			game().send_player_enhance_building(*tso.site, enhancement, true);
		}
	}

	// Changing capacity to 0 - this will happen only once
	if (tso.site->soldier_control()->soldier_capacity() > 1) {
		game().send_player_change_soldier_capacity(
		   *ts, -1 * tso.site->soldier_control()->soldier_capacity());
		return true;
	}

	// Reducing ware queues for armors and weapons to max_fill 1
	for (Widelands::InputQueue* queue : tso.site->inputqueues()) {

		if (queue->get_type() != Widelands::wwWARE) {
			continue;
		}

		// Already decreased
		if (queue->get_max_fill() <= 1) {
			continue;
		}

		// Modify max_fill of armors and weapons
		for (const std::string& pattern : armors_and_weapons) {
			if (tribe_->get_ware_descr(queue->get_index())->name().find(pattern) !=
			    std::string::npos) {
				if (queue->get_max_fill() > 1) {
					game().send_player_set_input_max_fill(*ts, queue->get_index(), Widelands::wwWARE, 1);
					continue;
				}
			}
		}
	}

	// If soldier capacity is 0, check if the site is supplied enough to increase to 1
	if (tso.site->soldier_control()->soldier_capacity() == 0) {

		// Check substitute wares first
		int32_t filled = 0;
		int32_t shortage = 0;
		bool inputs_are_substitutes = false;
		for (Widelands::InputQueue* queue : tso.site->inputqueues()) {
			if (queue->get_type() != Widelands::wwWARE) {
				continue;
			}
			if (tso.bo->substitute_inputs.count(queue->get_index()) > 0) {
				inputs_are_substitutes = true;
				filled += queue->get_filled();
			}
		}
		if (filled < 5 && inputs_are_substitutes) {
			shortage += 5 - filled;
		}

		// Check non-substitutes
		for (Widelands::InputQueue* queue : tso.site->inputqueues()) {
			if (queue->get_type() != Widelands::wwWARE) {
				continue;
			}
			if (tso.bo->substitute_inputs.count(queue->get_index()) == 0) {
				const uint32_t required_amount =
				   (queue->get_max_fill() < 5) ? queue->get_max_fill() : 5;
				if (queue->get_filled() < required_amount) {
					shortage += required_amount - queue->get_filled();
				}
			}
		}

		// Pressure-based training decision:
		// Train if shortage is acceptable AND (military pressure is high OR enemy seen lately)
		if (shortage <= 3) {
			bool should_train = false;

			if (training_pressure_ > 0) {
				should_train = true;
			} else if (player_statistics.any_enemy_seen_lately(gametime)) {
				should_train = true;
			} else if (shortage == 0) {
				// Fully stocked? Train anyway, even without pressure
				should_train = true;
			} else if (shortage <= 1 &&
			           player_statistics.get_player_power(player_number()) <
			              player_statistics.get_visible_enemies_power(gametime)) {
				// We're weaker than enemies - train if nearly stocked
				should_train = true;
			}

			if (should_train) {
				game().send_player_change_soldier_capacity(*ts, 1);
			}
		}
	}

	ts_without_trainers_ = 0;
	for (const TrainingSiteObserver& observer : trainingsites) {
		if (!observer.site->can_start_working()) {
			++ts_without_trainers_;
		}
	}
	return true;
}

// Manage garrison levels and dismantle unneeded military sites.
// Uses pressure-based scoring instead of neural network weights.
bool PlannerAI::check_militarysites(const Time& gametime) {

	// Only usable if we own at least one military site
	if (militarysites.empty()) {
		return false;
	}

	bool changed = false;
	Widelands::MilitarySite* ms = militarysites.front().site;

	// Don't do anything if last change was too recent
	if (militarysites.front().last_change + Duration(2 * 60 * 1000) > gametime) {
		militarysites.push_back(militarysites.front());
		militarysites.pop_front();
		return false;
	}

	Widelands::FCoords f = game().map().get_fcoords(ms->get_position());

	UniversalBuildableField bf(f);
	update_buildable_field(bf);

	Widelands::Quantity const total_capacity = ms->soldier_control()->max_soldier_capacity();
	Widelands::Quantity const current_target = ms->soldier_control()->soldier_capacity();
	Widelands::Quantity const current_soldiers = ms->soldier_control()->present_soldiers().size();

	// Target occupancy based on soldier status
	Widelands::Quantity target_occupancy = total_capacity;
	if (soldier_status_ == SoldiersStatus::kBadShortage) {
		target_occupancy = total_capacity / 3 + 1;
	} else if (soldier_status_ == SoldiersStatus::kShortage) {
		target_occupancy = total_capacity * 2 / 3 + 1;
	}

	militarysites.front().understaffed = 0;

	// Can this site be dismantled?
	const bool can_be_dismantled =
	   military_last_dismantle_ + Duration(30 * 1000) < gametime &&
	   (bf.own_military_presence - current_soldiers > 0 || bf.military_unstationed > 2) &&
	   militarysites.front().built_time + Duration(10 * 60 * 1000) < gametime &&
	   bf.military_loneliness < 800;

	// --- Cost/benefit dismantle scoring ---
	//
	// GAIN of dismantling: freed soldiers (can garrison frontier or attack).
	//   Each freed soldier is worth: military_pressure / total_soldiers.
	//   Also: freed building materials from dismantle (partial recovery).
	//
	// COST of dismantling: lost military coverage.
	//   Enemy land + unowned land nearby = territory at risk.
	//   Enemy military presence nearby = direct threat.
	//   Rebuild cost if we need to re-expand later.
	//
	// Score > 0 means gain > cost → dismantle is worthwhile.

	const int32_t total_soldiers_now = std::max<int32_t>(1, player_->count_soldiers());

	// GAIN: freed soldiers × their value to us.
	// soldier_value = how much each soldier matters = pressure per soldier.
	// When soldiers are scarce (high pressure, few soldiers), gain is high.
	const int32_t soldier_value =
	   military_pressure_ / total_soldiers_now;
	const int32_t freed_soldiers = static_cast<int32_t>(current_soldiers);
	int32_t dismantle_gain = freed_soldiers * soldier_value;

	// Redundancy bonus: other military sites cover this area.
	// Each overlapping soldier reduces the strategic cost of dismantling.
	// own_military_presence includes THIS building's soldiers, so subtract them.
	const int32_t overlap = std::max<int32_t>(0,
	   static_cast<int32_t>(bf.own_military_presence) - freed_soldiers);
	dismantle_gain += overlap * soldier_value / 2;

	// Vacancy cost: unfilled positions during soldier shortage.
	//
	// When no free soldiers exist, empty military buildings are a
	// liability: they request soldiers from the global pool that
	// can never be satisfied, tying up logistics and blocking the
	// spot from economy use. The dismantle decision should slowly
	// build up for these buildings.
	//
	// Each vacant position adds soldier_value to the gain (scaled
	// by shortage severity). The integral's leaky accumulator
	// (3/4 decay per visit) ensures this builds up slowly:
	// at net=+50 it takes ~5 visits (~6 min with 10 military sites)
	// to cross the threshold (150).
	//
	// Interior empties dismantle first (low cost offsets gain quickly).
	// Frontier empties dismantle only after prolonged shortage
	// (high cost from territory protection slows accumulation).
	// If soldiers become available (status improves), vacancy gain
	// vanishes → integral decays → building is kept.
	if (soldier_status_ == SoldiersStatus::kBadShortage ||
	    soldier_status_ == SoldiersStatus::kShortage) {
		const int32_t vacant = (current_target > current_soldiers) ?
		   static_cast<int32_t>(current_target - current_soldiers) : 0;
		if (vacant > 0) {
			// kBadShortage: full value per vacant position.
			// kShortage: half value (less urgent).
			const int32_t shortage_factor =
			   (soldier_status_ == SoldiersStatus::kBadShortage) ? 1 : 2;
			dismantle_gain += vacant * soldier_value / shortage_factor;
		}
	}

	// COST: coverage loss = what this building protects, in ware units.
	// Territory at risk is valued by its production potential.
	int32_t dismantle_cost = 0;

	// Territory at risk in ware-pressure units:
	// Each protected field × land_value_per_field_ / scaling.
	// Unowned land: we'd lose the ability to build there.
	// Enemy land: we'd lose the defensive buffer.
	const int32_t land_scale = std::max<int32_t>(1,
	   kNormalizationBudget / std::max<int32_t>(1, static_cast<int32_t>(wares.size())));
	dismantle_cost += static_cast<int32_t>(
	   static_cast<int64_t>(bf.unowned_land_nearby) *
	   land_value_per_field_ / land_scale);
	dismantle_cost += static_cast<int32_t>(
	   static_cast<int64_t>(bf.enemy_owned_land_nearby) *
	   land_value_per_field_ * 2 / land_scale);  // enemy land: 2× (offensive value)

	// Enemy military threat: each enemy soldier nearby makes dismantling risky.
	dismantle_cost += bf.enemy_military_presence * soldier_value;

	// Rebuild cost: larger buildings cost more to rebuild if needed later.
	// Proportional to capacity = building size.
	dismantle_cost += static_cast<int32_t>(total_capacity) * soldier_value / 2;

	// Port space protection: port spaces are uniquely valuable and rare.
	if (bf.portspace_nearby == ExtendedBool::kTrue) {
		dismantle_cost += soldier_value * static_cast<int32_t>(total_capacity);
	}

	// Per-site dismantle integral: accumulates when gain > cost,
	// decays when cost > gain. Only dismantle after sustained positive
	// scoring over multiple ticks. This prevents impulsive dismantling
	// of buildings that briefly seem redundant.
	//
	// The error signal is (gain - cost), clamped to prevent runaway.
	// Decay: 3/4 per tick. After ~4 ticks of consistent positive net,
	// the integral crosses the threshold.
	//
	// Over-militarization: inland buildings with no enemy nearby slowly
	// accumulate dismantle pressure because gain > cost (freed soldiers
	// have value, no territory is at risk). But if enemy approaches,
	// cost jumps up and the integral decays → building is kept.
	const int32_t dismantle_net = std::clamp(dismantle_gain - dismantle_cost, -200, 200);
	militarysites.front().dismantle_integral =
	   militarysites.front().dismantle_integral * 3 / 4 + dismantle_net;
	// Clamp integral to prevent excessive accumulation
	militarysites.front().dismantle_integral = std::clamp(
	   militarysites.front().dismantle_integral, -500, 500);

	// Threshold: dismantle when integral > 150 (roughly 3+ ticks of gain > cost)
	const bool should_be_dismantled = militarysites.front().dismantle_integral > 150;

	if (bf.enemy_accessible_ && !should_be_dismantled) {

		assert(total_capacity >= target_occupancy);

		militarysites.front().understaffed = total_capacity - target_occupancy;

		if (current_target < target_occupancy) {
			game().send_player_change_soldier_capacity(*ms, 1);
			changed = true;
		}
		if (current_target > target_occupancy) {
			game().send_player_change_soldier_capacity(*ms, -1);
			changed = true;
		}
		// At border: prefer heroes
		if (ms->get_soldier_preference() == Widelands::SoldierPreference::kRookies) {
			game().send_player_set_soldier_preference(*ms, Widelands::SoldierPreference::kHeroes);
			changed = true;
		}
	} else if (should_be_dismantled && can_be_dismantled) {
		changed = true;
		if ((ms->get_playercaps() & Widelands::Building::PCap_Dismantle) != 0u) {
			game().send_player_dismantle(*ms, true);
			military_last_dismantle_ = game().get_gametime();
		} else {
			game().send_player_bulldoze(*ms);
			military_last_dismantle_ = game().get_gametime();
		}
	} else {
		// Interior: reduce garrison, prefer rookies
		if (current_target > 1) {
			game().send_player_change_soldier_capacity(*ms, -1);
			changed = true;
		}
		if (ms->get_soldier_preference() == Widelands::SoldierPreference::kHeroes) {
			game().send_player_set_soldier_preference(*ms, Widelands::SoldierPreference::kRookies);
			changed = true;
		}
	}
	if (changed) {
		militarysites.front().last_change = gametime;
	}

	// Reorder: move front to back
	militarysites.push_back(militarysites.front());
	militarysites.pop_front();
	return changed;
}

// Calculate combined strength of a group of soldiers.
// Pure math - ported directly from DefaultAI.
int32_t PlannerAI::calculate_strength(const std::vector<Widelands::Soldier*>& soldiers) {
	if (soldiers.empty()) {
		return 0;
	}

	float health = 0;
	float attack = 0;
	float defense = 0;
	float evade = 0;
	float final_strength = 0;

	const Widelands::SoldierDescr& descr = soldiers.front()->descr();

	for (Widelands::Soldier* soldier : soldiers) {
		health = soldier->get_current_health();
		attack = (descr.get_base_max_attack() - descr.get_base_min_attack()) / 2.f +
		         descr.get_base_min_attack() +
		         descr.get_attack_incr_per_level() * soldier->get_attack_level();
		defense = 100 - descr.get_base_defense() -
		          descr.get_defense_incr_per_level() * soldier->get_defense_level();
		evade = 100 - descr.get_base_evade() -
		        descr.get_evade_incr_per_level() / 100.f * soldier->get_evade_level();
		final_strength += (attack * health) / (defense * evade);
	}

	assert(final_strength >= 0);
	assert(final_strength <=
	       soldiers.size() * (descr.get_base_max_attack() * descr.get_base_health() +
	                          descr.get_max_attack_level() * descr.get_attack_incr_per_level() +
	                          descr.get_max_health_level() * descr.get_health_incr_per_level()));

	// Divide by approximate strength of one unpromoted soldier
	const uint16_t average_unpromoted_strength =
	   (descr.get_base_min_attack() +
	    (descr.get_base_max_attack() - descr.get_base_min_attack()) / 2) *
	   descr.get_base_health() / (100 - descr.get_base_defense()) / (100 - descr.get_base_evade());
	return static_cast<int32_t>(final_strength / average_unpromoted_strength);
}

// Callback when a soldier finishes training. Set capacity to 0
// so the AI waits until the training site is restocked.
void PlannerAI::soldier_trained(const Widelands::TrainingSite& site) {

	const Time& gametime = game().get_gametime();

	for (TrainingSiteObserver& trainingsite_obs : trainingsites) {
		if (trainingsite_obs.site == &site) {
			soldier_trained_log.push(gametime, trainingsite_obs.bo->id);
			if (trainingsite_obs.site->soldier_control()->soldier_capacity() > 0) {
				game().send_player_change_soldier_capacity(
				   *trainingsite_obs.site,
				   -1 * trainingsite_obs.site->soldier_control()->soldier_capacity());
			}
			return;
		}
	}

	verb_log_warn_time(gametime, "AI %u: soldier_trained(): trainingsite not found\n",
	                   static_cast<unsigned>(player_number()));
}

// =====================================================================
// Enemy ware scarcity prediction
// =====================================================================
//
// Build a scarcity table for each enemy tribe. The table maps each ware
// to a scarcity score (0..100) representing how expensive/scarce it is.
//
// Base costs:
//   Finite resources (gold_ore, iron_ore, granite, marble, etc.) = 80
//   "Cheap" renewable resources (log, water, fish, etc.) = 20
//
// Produced items: discover all recipes that produce this ware, compute
// each recipe's cost as sum(input_costs) / output_count, then choose
// the cheapest recipe. E.g.:
//   coal: charcoal kiln (6 × log@20 = 120/6 = 20) vs coal mine (80 base
//         + bread inputs), cheapest wins.
//
// Uses fixed-point iteration: repeat until all costs converge.

const std::map<Widelands::DescriptionIndex, int32_t>&
PlannerAI::get_enemy_ware_scarcity(Widelands::PlayerNumber enemy_pn) {
	const Widelands::Player* enemy_player = game().get_player(enemy_pn);
	if (enemy_player == nullptr) {
		// Return empty map for invalid player
		static const std::map<Widelands::DescriptionIndex, int32_t> empty;
		return empty;
	}

	const Widelands::TribeDescr& tribe = enemy_player->tribe();
	const std::string& tribe_name = tribe.name();

	// Check cache
	auto it = tribe_scarcity_cache_.find(tribe_name);
	if (it != tribe_scarcity_cache_.end()) {
		return it->second;
	}

	// Build the scarcity table for this tribe
	std::map<Widelands::DescriptionIndex, int32_t>& scarcity =
	   tribe_scarcity_cache_[tribe_name];

	const Widelands::Descriptions& descriptions = game().descriptions();

	// Step 1: Classify base resources.
	// Finite resources from mines = 80 (gold_ore, iron_ore, granite, etc.)
	// Renewable resources = 20 (log, water, fish, wheat, etc.)
	// Unknown/unproduceable = 50 (default)
	for (const Widelands::DescriptionIndex wi : tribe.wares()) {
		scarcity[wi] = 50;  // default: moderate scarcity
	}

	// Identify finite resources: wares produced ONLY by mines.
	// Identify renewable resources: wares produced by non-mine buildings
	// that have no inputs (primary producers like woodcutter, well, fisher).
	std::map<Widelands::DescriptionIndex, bool> produced_by_mine;
	std::map<Widelands::DescriptionIndex, bool> produced_by_primary;

	for (const Widelands::DescriptionIndex bi : tribe.buildings()) {
		const Widelands::BuildingDescr* bdesc = descriptions.get_building_descr(bi);
		if (bdesc == nullptr) {
			continue;
		}
		const auto* psdesc =
		   dynamic_cast<const Widelands::ProductionSiteDescr*>(bdesc);
		if (psdesc == nullptr) {
			continue;
		}

		const bool is_mine = bdesc->get_ismine();
		const bool has_inputs = !psdesc->input_wares().empty();

		for (const Widelands::DescriptionIndex output : psdesc->output_ware_types()) {
			if (is_mine) {
				produced_by_mine[output] = true;
			}
			if (!is_mine && !has_inputs) {
				produced_by_primary[output] = true;
			}
		}
	}

	// Assign base costs
	for (auto& [wi, cost] : scarcity) {
		if (produced_by_mine.count(wi) > 0 && produced_by_primary.count(wi) == 0) {
			// Only produced by mines = finite resource
			cost = 80;
		} else if (produced_by_primary.count(wi) > 0) {
			// Produced by primary producer (no inputs) = cheap/renewable
			cost = 20;
		}
		// Otherwise stays at 50 (will be resolved by recipe iteration)
	}

	// Step 2: Iterate to resolve produced wares.
	// For each ware that can be produced, find all recipes.
	// Recipe cost = sum of input costs (weighted by quantity).
	// Choose the cheapest recipe.
	// Repeat until convergence (deep chains need multiple iterations).
	for (int iteration = 0; iteration < 10; ++iteration) {
		bool changed = false;

		for (const Widelands::DescriptionIndex bi : tribe.buildings()) {
			const Widelands::BuildingDescr* bdesc = descriptions.get_building_descr(bi);
			if (bdesc == nullptr) {
				continue;
			}
			const auto* psdesc =
			   dynamic_cast<const Widelands::ProductionSiteDescr*>(bdesc);
			if (psdesc == nullptr) {
				continue;
			}

			const auto& inputs = psdesc->input_wares();
			if (inputs.empty()) {
				continue;  // primary producer, already handled
			}

			// Compute recipe cost: sum of (input_cost × quantity)
			int32_t recipe_cost = 0;
			for (const auto& [input_idx, qty] : inputs) {
				auto input_it = scarcity.find(input_idx);
				if (input_it != scarcity.end()) {
					recipe_cost += input_it->second * static_cast<int32_t>(qty);
				} else {
					// Unknown input — use default
					recipe_cost += 50 * static_cast<int32_t>(qty);
				}
			}

			// Mine penalty: finite resource base cost on top of inputs.
			// The mine depletes a non-renewable resource.
			if (bdesc->get_ismine()) {
				recipe_cost += 80;
			}

			// Divide by number of outputs to get per-ware cost
			const int32_t nr_outputs = std::max<int32_t>(1,
			   static_cast<int32_t>(psdesc->output_ware_types().size()));
			recipe_cost /= nr_outputs;

			// Clamp to [0..100]
			recipe_cost = std::clamp(recipe_cost, 0, 100);

			// Update each output: take minimum across all recipes
			for (const Widelands::DescriptionIndex output : psdesc->output_ware_types()) {
				auto out_it = scarcity.find(output);
				if (out_it != scarcity.end()) {
					if (recipe_cost < out_it->second) {
						out_it->second = recipe_cost;
						changed = true;
					}
				}
			}
		}

		if (!changed) {
			break;  // converged
		}
	}

	return scarcity;
}

// Get the scarcity value of a specific ware for an enemy player.
// Returns 0..100 where 100 = extremely scarce/valuable.
int32_t PlannerAI::enemy_ware_value(
   Widelands::PlayerNumber enemy_pn, Widelands::DescriptionIndex ware) {
	const auto& table = get_enemy_ware_scarcity(enemy_pn);
	auto it = table.find(ware);
	if (it != table.end()) {
		return it->second;
	}
	return 50;  // default: moderate value
}


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

// Military gate PID: controls military expansion cost/benefit ratio.
// Positive outputControl = expansion healthy, build easier.
// Negative outputControl = economy strained, build harder.
void PlannerAI::update_military_gate(const Time& /* gametime */) {
	const int32_t avg_wp = weights_.avg_wp > 0 ? weights_.avg_wp : 1;
	const int32_t P_weight = weights_.P_weight;
	const int32_t N_ticks = weights_.N_ticks;

	military_gate_.error = 0;

	// Signal 1: Expansion urgency.
	// When expansion_targets_[0] is high, unowned land is plentiful
	// and the AI should expand. Capped at P_weight to prevent windup.
	if (!expansion_targets_.empty()) {
		military_gate_.error += std::min<int32_t>(P_weight,
		   expansion_targets_[0].outputControl / avg_wp);
	}

	// Signal 2: CM strain from military constructions.
	// For each in-construction military building, sum
	// (ware_pressure × buildcost_amount) as the opportunity cost.
	// Castles (8 wares) contribute 4× more strain than sentries (2 wares).
	int64_t cm_strain = 0;
	for (const BuildingObserver& bo : buildings_) {
		if (bo.type != BuildingObserver::Type::kMilitarysite ||
		    bo.cnt_under_construction == 0) {
			continue;
		}
		for (uint32_t i = 0; i < bo.cnt_under_construction; ++i) {
			for (const auto& [ware_idx, amount] : bo.desc->buildcost()) {
				if (static_cast<size_t>(ware_idx) < ware_pressure_.size()) {
					cm_strain += static_cast<int64_t>(
					   ware_pressure_[ware_idx].outputControl) * amount;
				}
			}
		}
	}
	// Normalize to [count] by dividing by avg_wp²
	military_gate_.error -= static_cast<int32_t>(
	   std::min<int64_t>(static_cast<int64_t>(P_weight) * 2,
	      cm_strain / std::max<int64_t>(1, static_cast<int64_t>(avg_wp) * avg_wp)));

	military_gate_.tick(P_weight, 1, N_ticks);
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
// Circle 3: Expansion Pressure
// =====================================================================

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
				for (const UniversalBuildableField* bf : buildable_fields) {
					if (bf->is_mine_spot &&
					    bf->coords.field->get_resources() == bo.mines) {
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
		for (const UniversalBuildableField* bf : buildable_fields) {
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
