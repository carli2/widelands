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
#include "logic/map_objects/tribes/militarysite.h"
#include "logic/map_objects/tribes/productionsite.h"
#include "logic/map_objects/tribes/tribe_descr.h"
#include "logic/map_objects/tribes/warehouse.h"
#include "logic/mapregion.h"
#include "logic/path.h"
#include "logic/player.h"

namespace AI {

// =====================================================================
// Stock level utilities
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

void PlannerAI::update_ware_pressures(const Time& gametime) {
	++pi_tick_count_;
	const size_t nr_wares = wares.size();
	std::vector<int32_t> raw_A(nr_wares, 0);  // Factor A: capacity PID
	std::vector<int32_t> raw_B(nr_wares, 0);  // Factor B: demand signals
	std::vector<int32_t> raw_total(nr_wares, 0);

	// PID parameters from shared weights (recomputed in update_building_pressures)
	const int32_t N_ticks = weights_.N_ticks;
	const int32_t P_weight = weights_.P_weight;
	const int32_t effective_D = std::max<int32_t>(1, D_permille_ * N_ticks / 1000);

	// === Meta-PID tick (before all other PIDs, using previous cycle's data) ===
	//
	// Meta-D: idle-vs-scarcity balance → D_permille.
	//   Error = (scarce wares) - (idle buildings).
	//   Positive → more scarcity than idleness → increase D.
	//   Negative → more idleness than scarcity → decrease D.
	meta_D_.error = cached_scarce_ware_count_ - cached_idle_count_;
	meta_D_.tick(
		P_weight, I_permille_, effective_D, weights_.leak_num, weights_.leak_den);
	D_permille_ = std::clamp(1000 + meta_D_.outputControl / P_weight, 100, 2000);
	const int32_t effective_D_updated = std::max<int32_t>(1, D_permille_ * N_ticks / 1000);

	// Meta-I: stock velocity → I_permille.
	//   Positive velocity (stocks growing) → increase I (more integral memory).
	//   Negative velocity (stocks shrinking) → decrease I (more reactive).
	const int32_t nr_wares_i = std::max<int32_t>(1, static_cast<int32_t>(nr_wares));
	meta_I_.error = cached_stock_velocity_ / nr_wares_i;
	meta_I_.tick(
		P_weight, I_permille_, effective_D_updated, weights_.leak_num, weights_.leak_den);
	I_permille_ = std::clamp(250 + meta_I_.outputControl / P_weight, 1, 500);

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
	int32_t stock_velocity_total = 0;  // accumulated for meta-I feedback

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
		// Depletion velocity: project stock decline forward by N_ticks.
		// If stock is falling, the effective stock is lower than actual,
		// making the PID respond BEFORE the stock reaches zero.
		//
		// Example: stock=18, last=20, decline=2, N_ticks=5
		//   effective_stock = 18 - 2*5 = 8  (predicts stock in 5 ticks)
		//   vs raw stock=18 which looks comfortable
		int32_t effective_stock = static_cast<int32_t>(stock);
		if (w < ware_stock_last_tick_.size() && ware_stock_last_tick_[w] > stock) {
			const int32_t decline = static_cast<int32_t>(ware_stock_last_tick_[w]) -
			                        static_cast<int32_t>(stock);
			effective_stock = std::max<int32_t>(0,
			   static_cast<int32_t>(stock) - decline * weights_.N_ticks);
		}
		// Accumulate stock velocity for meta-I feedback (before snapshot update)
		if (w < ware_stock_last_tick_.size()) {
			stock_velocity_total += static_cast<int32_t>(stock) -
			   static_cast<int32_t>(ware_stock_last_tick_[w]);
		}
		// Update snapshot for next tick
		if (ware_stock_last_tick_.size() <= w) {
			ware_stock_last_tick_.resize(w + 1, stock);
		}
		ware_stock_last_tick_[w] = stock;

		// Unit: [count] = [count] - [count] - [count]
		wp.error = consumption - effective_stock - production_capacity;

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
			if (cm_pending_total > 0 && effective_stock < cm_pending_total) {
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

		// STEP 3: PID tick (I/D weights tuned by meta-PIDs)
		// Unit flow:
		//   wp.error [count] (accumulated above)
		//   wp.ipart [count] (accumulated across ticks, anti-windup decay)
		//   outputControl = P_weight × error + I_permille × ipart / 1000
		//                   + effective_D × (error - lastError)
		//                 = [raw_pid]
		//   raw_A[w] = max(0, outputControl) [raw_pid]
		//
		// Leaky integrator in tick() handles anti-windup automatically.
		// Steady-state integral ≈ error × N_ticks (memory window).
		// I_permille ∈ [1, 500], effective_D = D_permille × N_ticks / 1000.
		wp.tick(P_weight, I_permille_, effective_D_updated, weights_.leak_num, weights_.leak_den);
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
		//
		// PID-based measurement: for each input without a producer, the
		// gate factor is the ratio of the building's accumulated demand
		// (integral) to the input ware's scarcity (PID output):
		//
		//   factor_i = demand / (demand + scarcity)
		//
		// This creates a proper feedback loop:
		//   - demand = building_pressure_ integral (grows as ware shortage
		//     persists, bounded by anti-windup). This is the "patience"
		//     signal: the longer we've wanted this building, the more the
		//     gate opens.
		//   - scarcity = ware_pressure_ output (PID measurement of how
		//     scarce the input ware is). High scarcity = gate harder to
		//     open. Low scarcity (stock filling) = gate opens easily.
		//
		// Multiple inputs: PRODUCT of per-input factors. Each missing
		// input multiplicatively closes the gate. A building missing
		// 2 inputs is blocked much harder than one missing 1.
		//
		// No magic numbers. The floor is 1/(1+N_wares) ≈ 0.7% for
		// Atlanteans — derived from the PID normalization budget, not
		// an arbitrary constant.
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
			// Building's accumulated demand: previous tick's integral.
			// Early game (no integral yet): factor ≈ 0 → gate closed.
			// After many ticks of demand: integral grows → gate opens.
			const int32_t demand =
			   (bi < building_pressure_.size()) ?
			   std::max<int32_t>(0, building_pressure_[bi].integral()) : 0;

			int64_t combined = 1000;
			for (const auto& input : bo.inputs) {
				if (static_cast<size_t>(input) >= nr_wares) {
					continue;
				}
				int32_t input_factor;
				if (ware_has_producer[input]) {
					// Producer built or under construction → fully open.
					input_factor = 1000;
				} else {
					// No producer: gate based on demand vs scarcity.
					const int32_t scarcity =
					   (static_cast<size_t>(input) < ware_pressure_.size()) ?
					   std::max<int32_t>(1, ware_pressure_[input].outputControl) : 1;
					// factor = demand / (demand + scarcity), scaled to 0..1000
					input_factor = static_cast<int32_t>(
					   std::min<int64_t>(1000,
					      static_cast<int64_t>(demand) * 1000 /
					      (static_cast<int64_t>(demand) + scarcity)));
				}
				combined = combined * input_factor / 1000;
			}
			building_supply_score_[bi] = static_cast<int32_t>(combined);
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
		   "P%u WARE SCARCITY (tick %u, %zu wares, I=%d D=%d idle=%d scarce=%d vel=%d):\n",
		   static_cast<unsigned>(player_number()), pi_tick_count_,
		   sorted_wp.size(), I_permille_, D_permille_,
		   cached_idle_count_, cached_scarce_ware_count_, cached_stock_velocity_);
		for (size_t i = 0; i < std::min<size_t>(8, sorted_wp.size()); ++i) {
			const size_t w = sorted_wp[i].second;
			const auto wi = static_cast<Widelands::DescriptionIndex>(w);
			verb_log_info_time(gametime,
			   "  P%u WP#%zu %s: score=%d (A=%d B=%d err=%d int=%d) stock=%u\n",
			   static_cast<unsigned>(player_number()),
			   i + 1, tribe_->get_ware_descr(wi)->name().c_str(),
			   ware_pressure_[w].outputControl, raw_A[w], raw_B[w],
			   ware_pressure_[w].error,
			   ware_pressure_[w].integral(), calculate_stocklevel(wi));
		}
	}

	// Cache feedback signals for meta-PIDs (used next cycle, one-tick delay).
	cached_stock_velocity_ = stock_velocity_total;
	cached_scarce_ware_count_ = 0;
	for (const auto& wp : ware_pressure_) {
		if (wp.outputControl > 0) {
			++cached_scarce_ware_count_;
		}
	}
	cached_idle_count_ = 0;
	for (const auto& site : productionsites) {
		if (site.site != nullptr && site.site->is_stopped()) {
			++cached_idle_count_;
		}
	}
	for (const auto& mine : mines_) {
		if (mine.site != nullptr && mine.site->is_stopped()) {
			++cached_idle_count_;
		}
	}

	// Sync PID state to persistent data for savegame persistence
	persistent_data->pi_tick_count = pi_tick_count_;
	persistent_data->ware_pressure_integrals.resize(nr_wares);
	persistent_data->ware_pressure_last_errors.resize(nr_wares);
	for (size_t w = 0; w < nr_wares; ++w) {
		ware_pressure_[w].save_state(
		   persistent_data->ware_pressure_integrals[w],
		   persistent_data->ware_pressure_last_errors[w]);
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
		//   1. Drive Pass 2 enhancement decisions
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
		//   = P_weight × error + I_permille × ipart / 1000 + D × Δerror
		//   Note: [raw_pid] here means "P-scaled budget" — dimensionally
		//   [budget × dimensionless] = [budget], but the magnitude is
		//   amplified by P_weight (up to 100×). Uses int64 + clamp in tick().
		const int32_t bp_effective_D = std::max<int32_t>(1, D_permille_ * N_ticks / 1000);
		bp.tick(P_weight, I_permille_, bp_effective_D, weights_.leak_num, weights_.leak_den);
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

		// --- 5. Worker blocked ---
		// Workers that can't be produced (missing tool producers)
		// are a reason NOT to build. The error signal feeds the
		// D-term, providing immediate response when worker tools
		// become producible (error drops → D fires negative).
		if (bo.type == BuildingObserver::Type::kProductionsite ||
		    bo.type == BuildingObserver::Type::kMine) {
			for (const auto& worker_idx : bo.positions) {
				auto it = worker_producible_.find(worker_idx);
				if (it != worker_producible_.end() && !it->second) {
					cv.error += avg_wp;
					break;
				}
			}
		}

		// Not buildable → no contra needed (PRO is already 0)
		if (!bo.buildable(*player_) &&
		    bo.desc->enhanced_from() == Widelands::INVALID_INDEX) {
			cv.error = 0;
		}

		// PID tick (same parameters as PRO)
		const int32_t cv_effective_D = std::max<int32_t>(1, D_permille_ * N_ticks / 1000);
		cv.tick(P_weight, I_permille_, cv_effective_D, weights_.leak_num, weights_.leak_den);
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
		building_pressure_[bi].save_state(
		   persistent_data->building_pressure_integrals[bi],
		   persistent_data->building_pressure_last_errors[bi]);
	}
	persistent_data->building_prevention_integrals.resize(buildings_.size());
	persistent_data->building_prevention_last_errors.resize(buildings_.size());
	for (size_t bi = 0; bi < buildings_.size(); ++bi) {
		building_prevention_[bi].save_state(
		   persistent_data->building_prevention_integrals[bi],
		   persistent_data->building_prevention_last_errors[bi]);
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
	update_military_gate(gametime);

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
			update_military_gate(gametime);
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
		const size_t mine_spot_count = std::count_if(
		   buildable_fields.begin(), buildable_fields.end(),
		   [](const UniversalBuildableField* bf) { return bf->is_mine_spot; });
		verb_log_info_time(gametime,
		   "P%u BUILD DECISIONS (fields=%zu mine_spots=%zu spots=%d constr=%u econ=%u):\n",
		   static_cast<unsigned>(player_number()), buildable_fields.size(),
		   mine_spot_count, spots_, numof_psites_in_constr, economy_size);
		for (size_t i = 0; i < std::min<size_t>(8, sorted_bp.size()); ++i) {
			const size_t bi = sorted_bp[i].second;
			const BuildingObserver& bo = buildings_[bi];
			const int32_t pro = building_pressure_[bi].outputControl;
			const int32_t contra = (bi < building_prevention_.size()) ?
			   building_prevention_[bi].outputControl : 0;
			const int32_t supply =
			   (bi < building_supply_score_.size()) ? building_supply_score_[bi] : 1000;
			verb_log_info_time(gametime,
			   "  P%u BP#%zu %s: eff=%d (pro=%d contra=%d) supply=%d built=%u constr=%u\n",
			   static_cast<unsigned>(player_number()),
			   i + 1, bo.name, pro - contra, pro, contra, supply,
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
							// Supply gate handles chain ordering — a
							// weaponsmithy with no input producers gets
							// a low supply factor that drops it below
							// basic buildings.
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

			// Input chain readiness: hard gate by supply chain completeness.
			// Supply chain gate: scale score by supply readiness.
			//
			// Uses the PID-based building_supply_score_ computed in
			// update_ware_pressures(). The factor is derived from:
			//   demand_integral / (demand_integral + ware_scarcity)
			// No magic numbers — the gate opens as the building's PID
			// integral accumulates demand over time, and closes when
			// input wares are scarce (high ware_pressure output).
			if ((bo.type == BuildingObserver::Type::kProductionsite ||
			     bo.type == BuildingObserver::Type::kMine) &&
			    bi < building_supply_score_.size() && !bo.inputs.empty()) {
				bo.add_new_building_score =
				   bo.add_new_building_score * building_supply_score_[bi] / 1000;
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

	// Military gate: PID-controlled cost/benefit ratio.
	// Positive outputControl = expansion healthy, build easier.
	// Negative outputControl = economy strained, build harder.
	// Convert to a divisor in [1, ...] range:
	//   gate_divisor = max(1, 1 - outputControl / avg_wp)
	// Positive output → divisor = 1 (no penalty)
	// Output = -avg_wp → divisor = 2 (half efficiency)
	// Output = -3*avg_wp → divisor = 4 (quarter efficiency)
	const int32_t avg_wp_gate =
	   wares.empty() ? 1 :
	   kNormalizationBudget / static_cast<int32_t>(wares.size());
	const int32_t gate_divisor = std::max<int32_t>(1,
	   1 - military_gate_.outputControl / std::max<int32_t>(1, avg_wp_gate));

	// Precompute which wares have a built producer (for conservation premium).
	// Wares without a built producer are treated as non-renewable even if
	// inherently renewable — HQ stock is finite regardless.
	std::vector<bool> ware_has_built_producer(wares.size(), false);
	for (const BuildingObserver& bo : buildings_) {
		if (bo.type != BuildingObserver::Type::kProductionsite &&
		    bo.type != BuildingObserver::Type::kMine) {
			continue;
		}
		if (bo.cnt_built == 0 && bo.cnt_under_construction == 0) {
			continue;
		}
		for (const auto& output : bo.ware_outputs) {
			if (static_cast<size_t>(output) < ware_has_built_producer.size()) {
				ware_has_built_producer[output] = true;
			}
		}
	}

	// Conservation factor for non-renewable military CM (permille 0..1000).
	// In peacetime (no enemy pressure), non-renewable wares get full premium.
	// In wartime (high enemy pressure), premium drops — big buildings are
	// justified for defense. This prevents barbarians burning 8 granite on
	// castles during peaceful expansion, but allows it when under attack.
	int32_t conservation_factor = 1000;
	{
		int32_t enemy_pressure = 0;
		int32_t total_pressure = 0;
		for (size_t i = 0; i < expansion_targets_.size(); ++i) {
			const int32_t p = std::max<int32_t>(0, expansion_targets_[i].outputControl);
			total_pressure += p;
			if (i > 0) {
				enemy_pressure += p;
			}
		}
		if (total_pressure > 0) {
			conservation_factor = (total_pressure - enemy_pressure) * 1000 / total_pressure;
		}
	}

	// Dynamic threshold: base × gate_divisor
	// When gate says "expand": divisor=1, threshold=base (~83K)
	// When gate says "conserve": divisor=4, threshold=4×base (~332K)
	const int32_t military_min_threshold =
	   wares.empty() ? 0 :
	   kNormalizationBudget / (4 * static_cast<int32_t>(wares.size())) * gate_divisor;
	int32_t military_priority = military_min_threshold;
	Widelands::Coords military_coords;

	// --- Phase 2: Pre-select best building types per category (O(m)) ---
	//
	// Economy: sorted top-10 candidates by PID score.
	// If the top candidate can't find a valid field (wrong size class,
	// no resources nearby, forest blocking all spots), fall through to
	// the next candidate. This prevents a single unplaceable building
	// from blocking the entire economy pipeline (e.g., a medium sawmill
	// at BP#1 blocking all building when only small spots are available
	// on a heavily forested map).
	constexpr size_t kMaxEconomyCandidates = 10;
	std::vector<BuildingObserver*> economy_candidates;
	for (BuildingObserver& bo : buildings_) {
		if (bo.type == BuildingObserver::Type::kMilitarysite) {
			continue;
		}
		if (bo.new_building != BuildingNecessity::kNeeded) {
			continue;
		}
		if (!bo.buildable(*player_)) {
			continue;
		}
		if (bo.add_new_building_score <= 0) {
			continue;
		}
		if (bo.cnt_under_construction >= 2) {
			continue;
		}
		economy_candidates.push_back(&bo);
	}
	std::sort(economy_candidates.begin(), economy_candidates.end(),
	   [](const BuildingObserver* a, const BuildingObserver* b) {
	       return a->add_new_building_score > b->add_new_building_score;
	   });
	if (economy_candidates.size() > kMaxEconomyCandidates) {
		economy_candidates.resize(kMaxEconomyCandidates);
	}

	// Military: collect all viable types (typically 3-5).
	// Must iterate per field for integral accumulation, but the
	// inner loop is tiny compared to the full buildings_ vector.
	// Pre-compute material cost and time factor for each candidate.
	struct MilitaryCandidateInfo {
		BuildingObserver* bo;
		int64_t material_cost;
		int32_t garrison_size;
		int32_t time_factor;
		int32_t prio_cap;
	};
	std::vector<MilitaryCandidateInfo> military_candidates;
	{
		const int32_t avg_wp_mil =
		   wares.empty() ? 1 :
		   kNormalizationBudget / static_cast<int32_t>(wares.size());
		int32_t soldier_cost_factor = 1;
		if (soldier_status_ == SoldiersStatus::kBadShortage) {
			soldier_cost_factor = 3;
		} else if (soldier_status_ == SoldiersStatus::kShortage) {
			soldier_cost_factor = 2;
		}
		for (BuildingObserver& bo : buildings_) {
			if (bo.type != BuildingObserver::Type::kMilitarysite) {
				continue;
			}
			if (!bo.buildable(*player_)) {
				continue;
			}
			if (bo.add_new_building_score <= 0) {
				continue;
			}
			if (bo.new_building == BuildingNecessity::kForbidden) {
				continue;
			}
			int64_t mat_cost = 0;
			int32_t total_wares = 0;
			for (const auto& [ware_idx, amount] : bo.desc->buildcost()) {
				int32_t ware_cost = 1;
				if (static_cast<size_t>(ware_idx) < ware_pressure_.size()) {
					ware_cost = std::max<int32_t>(1,
					   ware_pressure_[ware_idx].outputControl);
				}
				mat_cost += static_cast<int64_t>(ware_cost) * amount;
				total_wares += static_cast<int32_t>(amount);
				if (static_cast<size_t>(ware_idx) < ware_inherently_renewable_.size() &&
				    (!ware_inherently_renewable_[ware_idx] ||
				     !ware_has_built_producer[ware_idx])) {
					mat_cost += static_cast<int64_t>(amount) *
					   avg_wp_gate * conservation_factor / 1000;
				}
			}
			const Widelands::MilitarySiteDescr* ms_desc =
			   dynamic_cast<const Widelands::MilitarySiteDescr*>(bo.desc);
			const int32_t garrison = ms_desc != nullptr ?
			   static_cast<int32_t>(ms_desc->get_max_number_of_soldiers()) : 1;
			mat_cost += static_cast<int64_t>(garrison) *
			   soldier_cost_factor * avg_wp_mil;
			mat_cost = std::max<int64_t>(1, mat_cost);
			const int32_t tfactor = std::max<int32_t>(1,
			   1 + (total_wares + garrison) / 2);
			military_candidates.push_back(
			   {&bo, mat_cost, garrison, tfactor,
			    bo.add_new_building_score * 4});
		}
	}

	// Pre-compute: does any military building exist? (for border reservation)
	bool any_military_exists = false;
	for (const BuildingObserver& mbo : buildings_) {
		if (mbo.type == BuildingObserver::Type::kMilitarysite &&
		    (mbo.cnt_built + mbo.cnt_under_construction) > 0) {
			any_military_exists = true;
			break;
		}
	}

	// --- Phase 3a: Military integral accumulation (single field walk) ---
	// Military integrals are field-persistent state that must be updated
	// exactly once per construct_building() call, independent of economy.
	for (UniversalBuildableField* const bf : buildable_fields) {
		if (bf->field_info_expiration < gametime) {
			continue;
		}
		if (blocked_fields.is_blocked(bf->coords)) {
			continue;
		}
		const uint16_t field_caps = player_->get_buildcaps(bf->coords);
		const int32_t maxsize = field_caps & Widelands::BUILDCAPS_SIZEMASK;

		// === Military: integral accumulation (3-5 types per field) ===
		if (!military_candidates.empty() && maxsize > 0) {
			// Per-field integral management
			bool mil_frozen = false;
			if (bf->military_in_constr_nearby > 0) {
				bf->military_integral_ = std::min(bf->military_integral_ / 4, 0);
				mil_frozen = true;
			} else if (bf->military_score_ <= 0 && !bf->near_border) {
				bf->military_integral_ = bf->military_integral_ * 3 / 4;
				mil_frozen = true;
			}

			if (!mil_frozen) {
				const int32_t avg_wp_mil =
				   wares.empty() ? 1 :
				   kNormalizationBudget / static_cast<int32_t>(wares.size());
				int32_t best_efficiency = 0;
				BuildingObserver* best_mil_type = nullptr;

				for (const auto& mc : military_candidates) {
					if (mc.bo->desc->get_size() > maxsize) {
						continue;
					}
					const int32_t conquer_r = static_cast<int32_t>(
					   mc.bo->desc->get_conquers());
					const int32_t field_gain = bf->military_score_ * conquer_r;
					const int64_t land_value =
					   static_cast<int64_t>(field_gain) *
					   std::max<int32_t>(1, land_value_per_field_);
					const int64_t total_cost =
					   mc.material_cost * mc.time_factor;
					const int32_t raw_efficiency = static_cast<int32_t>(
					   std::min<int64_t>(static_cast<int64_t>(mc.prio_cap),
					      land_value * avg_wp_mil / std::max<int64_t>(1, total_cost)));
					const int32_t efficiency = raw_efficiency / gate_divisor;

					if (best_mil_type == nullptr || efficiency > best_efficiency) {
						best_mil_type = mc.bo;
						best_efficiency = efficiency;
					}
				}

				if (best_mil_type != nullptr) {
					bf->military_integral_ =
					   bf->military_integral_ * 7 / 8 + best_efficiency;
					const int32_t mil_prio = bf->military_integral_;

					if (mil_prio > military_priority) {
						best_military = best_mil_type;
						military_priority = mil_prio;
						military_coords = bf->coords;
					}
				}
			}
		}
	}

	// --- Phase 3b: Economy placement (fallback chain) ---
	// Try each candidate in priority order. The first candidate that
	// finds a valid field wins. This prevents a single unplaceable
	// building (e.g., medium sawmill when only small spots exist on a
	// forested map) from blocking the entire economy pipeline.
	for (BuildingObserver* economy_candidate : economy_candidates) {
		for (UniversalBuildableField* const bf : buildable_fields) {
			if (bf->field_info_expiration < gametime) {
				continue;
			}
			if (blocked_fields.is_blocked(bf->coords)) {
				continue;
			}
			const uint16_t field_caps = player_->get_buildcaps(bf->coords);
			const int32_t maxsize = field_caps & Widelands::BUILDCAPS_SIZEMASK;
			const bool field_is_mine_spot = (field_caps & Widelands::BUILDCAPS_MINE) != 0;

			// Border reservation: fields near the border without military
			// coverage are reserved for military expansion. Economy buildings
			// should not take spots that military buildings need to claim land.
			// Only apply when at least one military building exists (early
			// game needs economy on border spots before first military).
			// Mine spots are exempt: military can't be built on them.
			if (any_military_exists && !field_is_mine_spot &&
			    bf->near_border && bf->own_military_presence == 0) {
				continue;
			}

			// Field compatibility: mine buildings need mine spots,
			// non-mine buildings need flat spots with sufficient size.
			const bool candidate_is_mine = economy_candidate->desc->get_ismine();
			const bool field_compatible =
			   candidate_is_mine ? field_is_mine_spot :
			   (!field_is_mine_spot && economy_candidate->desc->get_size() <= maxsize);
			if (field_compatible) do {  // breakable block for early exits
				BuildingObserver& bo = *economy_candidate;
				int32_t prio = bo.add_new_building_score;

				// Lookup field counts for this building type
				uint8_t number_of_supporters_nearby = 0;
				if (bf->supporters_nearby.count(bo.name) > 0) {
					number_of_supporters_nearby = bf->supporters_nearby.at(bo.desc->name());
				}
				uint8_t number_of_supported_producers_nearby = 0;
				{
					auto sit = bf->supported_producers_nearby.find(bo.id);
					if (sit != bf->supported_producers_nearby.end()) {
						number_of_supported_producers_nearby = sit->second;
					}
				}
				uint8_t number_of_same_nearby = 0;
				{
					auto sit = bf->buildings_nearby.find(bo.id);
					if (sit != bf->buildings_nearby.end()) {
						number_of_same_nearby = sit->second;
					}
				}

				// Category-specific resource scoring
				if (bo.desc->get_ismine() && bo.mines != Widelands::INVALID_INDEX) {
					// Mine: resource amount in radius 2, competition divisor.
					if (bf->coords.field->get_resources() != bo.mines) {
						break;
					}
					Widelands::MapRegion<Widelands::Area<Widelands::FCoords>> mr(
					   map, Widelands::Area<Widelands::FCoords>(bf->coords, 2));
					int32_t resource_score = 0;
					do {
						if (bo.mines == mr.location().field->get_resources()) {
							bool already_mined = false;
							if (const Widelands::BaseImmovable* imm =
							       mr.location().field->get_immovable()) {
								if (imm->descr().type() == Widelands::MapObjectType::BUILDING ||
								    imm->descr().type() ==
								       Widelands::MapObjectType::CONSTRUCTIONSITE) {
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
					resource_score /= (1 + bf->same_type_mines_nearby);
					// Input supply gate
					if (!bo.inputs.empty()) {
						int32_t max_input_pressure = 0;
						for (const auto& input : bo.inputs) {
							if (static_cast<size_t>(input) < ware_pressure_.size()) {
								max_input_pressure = std::max(max_input_pressure,
								   ware_pressure_[input].outputControl);
							}
						}
						const int32_t avg_wp_mine =
						   wares.empty() ? 1 :
						   kNormalizationBudget / static_cast<int32_t>(wares.size());
						if (max_input_pressure > avg_wp_mine * 2) {
							resource_score = resource_score * avg_wp_mine * 2 /
							   std::max<int32_t>(1, max_input_pressure);
						}
					}
					if (resource_score <= 0) {
						break;
					}
					prio += resource_score;

				} else if (bo.is(BuildingAttribute::kWell)) {
					if (bf->ground_water < 2) {
						break;
					}
					prio += bf->ground_water * std::max<int32_t>(1, prio / 10);
					if (number_of_same_nearby > 2) {
						break;
					}

				} else if (bo.is(BuildingAttribute::kFisher)) {
					if (bf->fish_nearby <= 15) {
						break;
					}
					prio += bf->fish_nearby * std::max<int32_t>(1, prio / 20);
					prio -= number_of_same_nearby * std::max<int32_t>(1, prio / 4);
					prio += number_of_supporters_nearby * std::max<int32_t>(1, prio / 5);

				} else if (bo.is_resource_harvester) {
					bool any_below_threshold = false;
					int32_t resource_bonus = 0;
					bool has_collected_immovable = false;
					for (const auto& rt : bo.resource_targets) {
						auto rit = bf->resource_count_by_attribute.find(rt.attribute_id);
						const uint16_t count = (rit != bf->resource_count_by_attribute.end()) ?
						   rit->second : 0;
						// For collected resources (rocks, trees), subtract resources
						// already covered by existing same-type buildings. Each existing
						// building "claims" ~saturation units from the shared area.
						// This prevents 4 quarries on the same rock pile: 50 rocks
						// minus 3×20 saturation = -10 effective → field rejected.
						int32_t effective = static_cast<int32_t>(count);
						if (rt.is_collected && number_of_same_nearby > 0) {
							effective -= static_cast<int32_t>(number_of_same_nearby) *
							   static_cast<int32_t>(rt.saturation);
						}
						if (effective < static_cast<int32_t>(rt.threshold)) {
							any_below_threshold = true;
							break;
						}
						const int32_t sat = std::max<int32_t>(1, rt.saturation);
						const int32_t per_unit = std::max<int32_t>(1, prio / sat);
						if (rt.is_collected) {
							has_collected_immovable =
							   has_collected_immovable ||
							   (rt.kind == ResourceSearchTarget::Kind::kImmovableAttribute);
						}
						resource_bonus += effective * per_unit;
					}
					if (any_below_threshold) {
						break;
					}
					prio += resource_bonus;
					prio += number_of_supporters_nearby * std::max<int32_t>(1, prio / 5);
					if (has_collected_immovable) {
						prio -= bf->space_consumers_nearby *
						   std::max<int32_t>(1, tribe_has_ranger_ ? prio / 10 : prio / 2);
					}

				} else if (bo.is(BuildingAttribute::kRanger)) {
					prio += number_of_supported_producers_nearby * std::max<int32_t>(1, prio / 5);
					prio += number_of_same_nearby * std::max<int32_t>(1, prio / 20);
					prio -= bf->water_nearby * std::max<int32_t>(1, prio / 100);
					prio -= bf->space_consumers_nearby * std::max<int32_t>(1, prio / 10);
					if (bf->portspace_nearby == ExtendedBool::kTrue ||
					    bf->unowned_portspace_vicinity_nearby > 0) {
						prio -= prio * 3;
					}
					const uint8_t rocks_nearby =
					   bf->immovables_by_attribute_nearby.count(BuildingAttribute::kNeedsRocks) > 0 ?
					      bf->immovables_by_attribute_nearby.at(BuildingAttribute::kNeedsRocks) : 0;
					prio -= rocks_nearby * std::max<int32_t>(1, prio / 30);

				} else if (bo.is(BuildingAttribute::kNeedsCoast)) {
					if (bf->water_nearby <= 0) {
						break;
					}
					prio += bf->water_nearby * std::max<int32_t>(1, prio / 20);

				} else if (!bo.supported_producers.empty() &&
				           bo.is(BuildingAttribute::kSupportingProducer)) {
					prio += number_of_supported_producers_nearby * std::max<int32_t>(1, prio / 5);
					prio -= number_of_same_nearby * std::max<int32_t>(1, prio / 4);
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
					if (bo.is(BuildingAttribute::kSpaceConsumer) &&
					    bf->unowned_portspace_vicinity_nearby > 0) {
						prio -= prio * 3;
					}
				}

				// Mark resource-dependent building found a viable field.
				// Space consumer placement
				if (bo.is(BuildingAttribute::kSpaceConsumer) &&
				    !bo.is(BuildingAttribute::kRanger) &&
				    !bo.is(BuildingAttribute::kSupportingProducer)) {
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
					if (bf->space_consumers_nearby > 2) {
						prio /= bf->space_consumers_nearby;
					}
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

				// Generic production site spread-out penalty
				if (bo.type == BuildingObserver::Type::kProductionsite &&
				    !bo.is_resource_harvester &&
				    !bo.is(BuildingAttribute::kRanger) &&
				    !bo.is(BuildingAttribute::kFisher)) {
					const int32_t nr_wares_local = std::max<int32_t>(
					   1, static_cast<int32_t>(wares.size()));
					prio -= bf->space_consumers_nearby *
					   std::max<int32_t>(1, prio * 2 / nr_wares_local);
					prio -= bf->own_non_military_nearby *
					   std::max<int32_t>(1, prio / nr_wares_local);
				}

				// Big spot waste penalty
				{
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

				// Port spot protection
				if (!bo.is(BuildingAttribute::kPort) &&
				    bf->portspace_nearby == ExtendedBool::kTrue) {
					prio -= prio * 5;
				}

				// Border reservation
				// Resource harvesters, fishers, and wells must be placed where
				// their resources are — even if that's near the border. Workers
				// can harvest trees/rocks/critters on unowned land, so border
				// spots with resources are the BEST placement, not the worst.
				// The military hard-skip still prevents placement at completely
				// undefended borders.
				const bool needs_specific_location =
				   bo.is_resource_harvester || bo.is(BuildingAttribute::kFisher) ||
				   bo.is(BuildingAttribute::kWell) || bo.is(BuildingAttribute::kNeedsCoast);
				if (any_military_exists &&
				    bf->own_military_presence == 0 && bf->military_in_constr_nearby == 0 &&
				    (bf->near_border || bf->unowned_land_nearby > 3)) {
					break;  // Hard skip: no military coverage
				}
				if (!needs_specific_location) {
					if (bf->unowned_land_nearby > 0 && !expansion_targets_.empty()) {
						const int32_t exp_pressure =
						   std::max<int32_t>(0, expansion_targets_[0].outputControl);
						prio -= static_cast<int32_t>(
						   static_cast<int64_t>(bf->unowned_land_nearby) *
						   prio * exp_pressure / (kNormalizationBudget + 1));
					}
					if (bf->enemy_nearby) {
						prio -= prio;
					}
				}

				if (prio > economy_priority) {
					best_economy = &bo;
					economy_priority = prio;
					economy_coords = bf->coords;
				}
			} while (false);

		}  // end field walk for this candidate

		if (best_economy != nullptr) {
			break;  // Found a placement — stop fallback chain
		}

		// No integral reset needed for resource harvesters that found no
		// fields. The leaky integrator converges to a bounded steady state
		// (~8× error), and the fallback chain moves past unplaceable
		// candidates automatically.
	}  // end economy_candidates fallback loop


	// Pass 2: Enhancement candidates (upgrading existing buildings)
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

		// No integral reset needed: the leaky integrator in tick() handles
		// winddown automatically. On next tick, error drops (cnt_built increased)
		// and the D-term fires negative for immediate priority reduction.

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

		for (UniversalBuildableField* const nbf : buildable_fields) {
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

	// Anti-windup is handled by the leaky integrator built into tick().
	// The 7/8 leak converges the integral to ~8× steady-state error,
	// preventing unbounded growth for unplaceable buildings.
	// Worker-blocked buildings get CONTRA pressure from building_prevention_
	// (see update_building_pressures), which naturally reduces their
	// effective score through the normal PID mechanism.

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

	// --- Upgrade execution (pending upgrades from construct_building Pass 2) ---
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
	// The upgrade decision was made in construct_building() Pass 2
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
		// Two cases:
		//  1) Non-renewable (quarry: rocks are finite, mine: ore depletes).
		//     When resources are gone, the building will NEVER produce
		//     again → overwhelming dismantle signal (10× avg_pressure)
		//     overrides all counter-forces.
		//  2) Renewable (woodcutter: trees planted by ranger).
		//     Resources CAN regenerate if a supporter exists/is built.
		//     No spike here — let the low-productivity factor (+1× per
		//     visit) build gradually via the leaky integrator. This gives
		//     the supporter coupling time to build a forester, and gives
		//     the forester time to plant trees. If resources stay at 0
		//     for many integrator cycles, the score eventually crosses 0
		//     and the building gets dismantled naturally.
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
			if (has_any_collected && all_collected_exhausted &&
			    !site.bo->requires_supporters) {
				// Non-renewable: overwhelming spike to force dismantle.
				delta += weights_.resource_exhausted_penalty;
			}
			// Renewable resources with supporters: no spike.
			// The low-productivity factor already accumulates +avg_pressure
			// per visit at 0% stats, building the integrator slowly.
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

	// Handle pending upgrade (decision was made in construct_building Pass 2)
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

	// Mine upgrades are handled by construct_building() Pass 2 (PI-driven).
	// If we reach here, the mine has resources but no upgrade decision yet.
	return false;
}


// =====================================================================
// Player statistics
// =====================================================================

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
				   std::max(max_output_integral, ware_pressure_[output].integral());
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
						   std::max(max_output_integral, ware_pressure_[ware].integral());
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


}  // namespace AI
