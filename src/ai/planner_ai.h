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

#ifndef WL_AI_PLANNER_AI_H
#define WL_AI_PLANNER_AI_H

#include <memory>
#include <set>

#include "ai/ai_help_structs.h"
#include "ai/computer_player.h"
#include "base/i18n.h"
#include "economy/economy.h"
#include "logic/map_objects/immovable.h"
#include "logic/map_objects/tribes/ship.h"
#include "logic/map_objects/tribes/soldier.h"
#include "logic/map_objects/tribes/trainingsite.h"

namespace Widelands {
struct Road;
}  // namespace Widelands

namespace AI {

/// =====================================================================
/// PlannerAI — Target Architecture Overview
/// =====================================================================
///
/// Data Structures (flat, no recursion):
///
///   1. Wares       — one PID controller per ware type.
///                    Tracks preciousness / scarcity of each ware.
///
///   2. Building Types — one PID controller per building type.
///                    Dual PID: PRO (should build) vs CONTRA (should not).
///                    effective_score = PRO - CONTRA.
///
///   3. Building Spots — one unified list (UniversalBuildableField).
///                    Flat terrain AND mine spots in the same deque.
///                    Each spot carries pre-computed field factors
///                    (resources nearby, military presence, etc.).
///                    Only needed for placement — NOT for the build decision.
///
///   4. Existing Buildings — one entry per placed building.
///                    With PID controller for dismantle decisions.
///                    (productionsites, mines_, militarysites, etc.)
///
///   5. Military Actions — union of possible actions:
///                    { building_spot → build_military,
///                      enemy_building → attack,
///                      own_military   → dismantle }
///                    Each action has a PID-controlled worth score.
///
/// Two loops per think() — each propagates one layer, no recursion:
///
///   Loop 1: Economy
///     a) PID-control ware preciousness      (update_ware_pressures)
///     b) PID-control building type scores    (update_building_pressures)
///     c) Pick the building type with highest effective_score
///     d) Walk all building spots, find best location for that type
///     e) Build 1 building (or nothing if score < 0)
///     f) Pick the existing building with highest dismantle score
///     g) Dismantle 1 building (or nothing if score < 0)
///     h) Reprioritize ware inputs on existing production buildings
///        (shift input priorities based on ware preciousness from 1a)
///     i) Stop/start buildings based on supply conditions
///        (stop starving buildings to free workers, restart when inputs
///        become available again)
///
///   Loop 2: Military
///     a) Maintain the list of possible military actions
///        (visible enemy buildings, valid border spots, own garrisons)
///     b) PID-control each action's worth
///        (land gain, construction cost, enemy destruction value, etc.)
///     c) Select the single best action and execute it
///        (or do nothing if best score < 0)
///     d) Garrison management: adjust soldier capacity per site
///        (hero/rookie preference, shortage response)
///
/// The system is completely tribe-agnostic — it reads building configs
/// at runtime.
///
/// STATUS vs current implementation:
///
///   1a) update_ware_pressures        — IMPLEMENTED
///   1b) update_building_pressures    — IMPLEMENTED
///   1c) Pick building type first     — GAP: currently merged field×building
///       loop in construct_building() scores all types at every spot.
///       Target: pick type first, THEN find best spot for that type.
///   1d) Walk spots for that type     — (see 1c)
///   1e) Build 1 building             — IMPLEMENTED (construct_building)
///   1f) Unified dismantle PID        — PARTIAL: production sites have
///       leaky-integrator dismantle_score (11 factors).  Mines use
///       timeout paths.  Military uses separate dismantle_integral.
///       Target: single scoring pass across all building types.
///   1g) Dismantle 1 building         — PARTIAL (scattered across
///       check_productionsites, check_mines_, check_militarysites)
///   1h) Ware input reprioritization  — IMPLEMENTED in
///       check_productionsites (set_inputs_to_zero / set_inputs_to_max)
///   1i) Stop/start buildings         — IMPLEMENTED in
///       check_productionsites (stop_site / initiate_dismantling)
///
///   2a) Maintain military actions    — PARTIAL: enemy_sites map tracks
///       visible targets; buildable_fields tracks border spots;
///       militarysites tracks own garrisons.  Not a single unified list.
///   2b) PID-control per action       — PARTIAL: per-field military
///       integral for build, per-target attack_integral for attack,
///       per-site dismantle_integral for military dismantle.
///       Each has its own decay rate and threshold.
///   2c) Select best action           — GAP: build/attack/dismantle are
///       evaluated in separate functions at different scheduler intervals.
///       Target: single pass, single best action.
///   2d) Garrison management          — IMPLEMENTED in
///       check_militarysites (capacity adjustment, hero/rookie preference)
/// =====================================================================
struct PlannerAI : ComputerPlayer {

	// desired_lead: goal-based error parameter.
	//   Negative = OK being behind (easy), 0 = match enemies, positive = push ahead (hard).
	//   Units: soldier-strength-per-hour lead over the best enemy.
	PlannerAI(Widelands::Game&, Widelands::PlayerNumber, int32_t desired_lead);
	~PlannerAI() override;
	void think() override;

	/// Easy Planner: OK being behind, builds slowly, stalls early.
	struct EasyImpl : public ComputerPlayer::Implementation {
		EasyImpl()
		   : Implementation(
		        "planner_easy",
		        /** TRANSLATORS: This is the name of an AI used in the game setup screens */
		        gettext_noop("Planner AI (Easy)"),
		        "images/ai/ai_weak.png",
		        Implementation::Type::kDefault) {
		}
		ComputerPlayer* instantiate(Widelands::Game& game,
		                            Widelands::PlayerNumber const p) const override {
			return new PlannerAI(game, p, -5);
		}
	};

	/// Normal Planner: wants to be slightly ahead, matches player pace.
	struct NormalImpl : public ComputerPlayer::Implementation {
		NormalImpl()
		   : Implementation(
		        "planner",
		        /** TRANSLATORS: This is the name of an AI used in the game setup screens */
		        gettext_noop("Planner AI"),
		        "images/ai/ai_normal.png",
		        Implementation::Type::kDefault) {
		}
		ComputerPlayer* instantiate(Widelands::Game& game,
		                            Widelands::PlayerNumber const p) const override {
			return new PlannerAI(game, p, 2);
		}
	};

	/// Hard Planner: massive lead desired, never stalls, always pushing.
	struct HardImpl : public ComputerPlayer::Implementation {
		HardImpl()
		   : Implementation(
		        "planner_hard",
		        /** TRANSLATORS: This is the name of an AI used in the game setup screens */
		        gettext_noop("Planner AI (Hard)"),
		        "images/ai/ai_normal.png",
		        Implementation::Type::kDefault) {
		}
		ComputerPlayer* instantiate(Widelands::Game& game,
		                            Widelands::PlayerNumber const p) const override {
			return new PlannerAI(game, p, 20);
		}
	};

	static EasyImpl easy_impl;
	static NormalImpl normal_impl;
	static HardImpl hard_impl;

	// --- Enums ---
	enum class SoldiersStatus : uint8_t { kFull = 0, kEnough = 1, kShortage = 3, kBadShortage = 6 };
	enum class NewShip : uint8_t { kBuilt, kFoundOnLoad };
	enum class WoodPolicy : uint8_t { kDismantleRangers, kStopRangers, kAllowRangers };

private:
	// --- Constants ---
	static constexpr int32_t kNormalizationBudget = 10'000'000;
	static constexpr Duration kFieldInfoExpiration{14 * 1000};
	static constexpr Duration kMineFieldInfoExpiration{20 * 1000};
	static constexpr Duration kBuildingMinInterval{25 * 1000};
	static constexpr Duration kCampaignDuration{15 * 60 * 1000};
	static constexpr Duration kDiplomacyInterval{90 * 1000};
	static constexpr Widelands::Serial kNoShip = Widelands::kInvalidSerial;
	static constexpr Duration kExpeditionMaxDuration{210 * 60 * 1000};
	static constexpr Widelands::Quantity kPortsPerTradeShip = 3;
	static constexpr Widelands::Quantity kWarshipsPerPort = 2;
	static constexpr Widelands::Quantity kPortDefaultGarrison = 5;

	// --- Core state ---
	Widelands::Player* player_{nullptr};
	Widelands::TribeDescr const* tribe_{nullptr};
	Widelands::Player::AiPersistentState* persistent_data{nullptr};
	bool initialized_{false};
	bool tribe_has_ranger_{false};

	// Goal-based error: desired soldier-strength-per-hour lead over best enemy.
	// Negative = easy (OK being behind), positive = hard (push ahead).
	// When our climb rate matches desired_lead + enemy rate, error → 0 and PI stalls.
	int32_t desired_lead_{2};

	// --- Initialization ---
	void late_initialization();

	// --- Field management ---
	void update_all_not_buildable_fields(const Time&);
	void update_all_buildable_fields(const Time&);
	void update_buildable_field(UniversalBuildableField&);

	// --- Building tracking ---
	void gain_immovable(Widelands::PlayerImmovable&);
	void lose_immovable(const Widelands::PlayerImmovable&);
	void gain_building(Widelands::Building&);
	void lose_building(const Widelands::Building&);

	// ========== PID Controller (shared by all three circles) ==========
	//
	// Standard PID controller with accumulation pattern:
	//   Phase 1: error = 0; error += signal_a; error += signal_b; ...
	//   Phase 2: tick(P, I, D)
	//   Phase 3: read outputControl (pre-normalization = [raw_pid])
	//   Phase 4: normalization → outputControl becomes [budget]
	//
	// Unit contract:
	//   error:         Circle 1: [count] (building/ware imbalance)
	//                  Circle 2: [budget] (from ware_pressure_.outputControl)
	//                  Circle 3: [count] (land/territory imbalance)
	//   ipart:         Same unit as error (accumulated across ticks)
	//   lastError:     Same unit as error
	//   outputControl: [raw_pid] after tick() (= P×error + I×ipart + D×Δerror)
	//                  [budget] after normalization (= raw × 10M / sum)
	//
	// Overflow budget (int32_t max = 2,147,483,647):
	//   Circle 1: error ∈ [-100, +100] (count). ipart after 200 ticks: ~20K.
	//     P=100 × 100 + 20K + 50 × 50 = 32.5K. Safe.
	//   Circle 2: error ∈ [0, 10M] (budget). ipart after 50 ticks: ~500M.
	//     P=100 × 10M + 500M + 50 × 5M = 1.75G. TIGHT but fits int32.
	//     At N=50 (economy of 2500 buildings): P=100 × 10M = 1G,
	//     ipart after 50 ticks at 10M = 500M. Sum = 1.55G. Fits.
	//   Sum for normalization uses int64_t (see update_building_pressures).
	//
	// Per think(), at most one tick() per controller instance.
	// The D-term (derivative) detects rate-of-change in error signals,
	// enabling anticipatory supply chain building: bakery placed →
	// flour consumption rises → D-term spikes → mill gets immediate
	// pressure instead of waiting for integral to accumulate.
	struct PIDController {
		int32_t lastError{0};      // previous tick's error (for D-term)
		int32_t error{0};          // accumulated error signal (reset before each tick)
		int32_t ipart{0};          // integral accumulator
		int32_t outputControl{0};  // readable output after tick()

		/// Compute PID output and advance state.
		/// Call once per think, after all error signals are accumulated.
		void tick(int32_t p_weight, int32_t i_weight, int32_t d_weight) {
			outputControl = p_weight * error + i_weight * ipart +
			   d_weight * (error - lastError);
			lastError = error;
			ipart += error;
		}
	};

	// ========== Circle 1: Ware Pressure (Power-Iteration) ==========
	std::vector<PIDController> ware_pressure_;
	std::vector<uint32_t> ware_stock_last_tick_;  // stock snapshot from previous PID tick

	void update_ware_pressures(const Time& gametime);

	// ========== Circle 2: Building Pressure (Power-Iteration) ==========
	//
	// Dual-PID per building type: PRO (should build) vs CONTRA (should NOT build).
	// PRO = "how much does the economy need this building?"
	// CONTRA = "why should we NOT build this building right now?"
	//
	// Build decision: effective_score = PRO.outputControl - CONTRA.outputControl
	// Positive → consider building. Highest wins.
	// Negative → skip. The contra reasons outweigh the demand.
	//
	// PRO accumulates: output ware demand, military/expansion need, etc.
	// CONTRA accumulates: missing input chains, non-renewable CM consumption,
	//   overcapacity, economy too young for military, etc.
	std::vector<PIDController> building_pressure_;   // PRO signal
	std::vector<PIDController> building_prevention_;  // CONTRA signal

	void update_building_pressures(const Time& gametime);

	// ========== Circle 3: Expansion Pressure (Power-Iteration) ==========
	// Vector components: index 0 = unowned land, 1..N = player 1..N
	std::vector<PIDController> expansion_targets_;

	// Bully weights (interface for future UI)
	std::map<Widelands::PlayerNumber, int32_t> bully_weights_;
	void set_bully_weight(Widelands::PlayerNumber pn, int32_t weight);
	int32_t get_bully_weight(Widelands::PlayerNumber pn) const;

	void update_expansion_pressures(const Time& gametime);

	// ========== Military Gate PID ==========
	// Controls military expansion cost/benefit ratio based on
	// expansion urgency vs construction material strain.
	PIDController military_gate_;
	int32_t smallest_garrison_{1};  // smallest military building's max_soldiers
	void update_military_gate(const Time& gametime);

	// ========== Military PI: Training vs Recruiting Split ==========
	int32_t military_pressure_{0};
	int32_t training_pressure_{0};
	int32_t recruiting_pressure_{0};

	void update_military_split();

	// --- Construction ---
	bool construct_building(const Time&);

	// --- Road building ---
	bool improve_roads(const Time&);
	bool connect_flag_to_road_network(const Widelands::Flag&, const Time&);
	void collect_nearflags(std::map<uint32_t, NearFlag>&, const Widelands::Flag&, uint16_t);

	// --- Economy & production management ---
	bool check_economies();
	bool check_productionsites(const Time&);
	bool check_mines_(const Time&);
	void update_production_stats();
	void review_ware_targets();
	bool set_inputs_to_zero(const ProductionSiteObserver&);
	void set_inputs_to_max(const ProductionSiteObserver&);
	void stop_site(const ProductionSiteObserver&);
	void initiate_dismantling(ProductionSiteObserver&, const Time&);
	void set_rangers_policy(const Time&);

	// --- Military (planner_ai_warfare.cc) ---
	bool check_militarysites(const Time&);
	bool check_enemy_sites(const Time&);
	void count_military_vacant_positions();
	bool check_trainingsites(const Time&);
	int32_t calculate_strength(const std::vector<Widelands::Soldier*>&);
	void soldier_trained(const Widelands::TrainingSite&);

	// Enemy ware scarcity prediction: per-tribe production cost table.
	// Maps tribe name → (ware DescriptionIndex → scarcity score 0..100).
	// Computed once per tribe from production chain analysis.
	// Finite resources (gold, iron, rocks) = 80, renewable (wood, water) = 20,
	// produced items = cheapest recipe cost.
	std::map<std::string, std::map<Widelands::DescriptionIndex, int32_t>> tribe_scarcity_cache_;
	const std::map<Widelands::DescriptionIndex, int32_t>&
	   get_enemy_ware_scarcity(Widelands::PlayerNumber enemy_pn);
	int32_t enemy_ware_value(Widelands::PlayerNumber enemy_pn, Widelands::DescriptionIndex ware);

	// --- Diplomacy ---
	void diplomacy_actions(const Time&);

	// --- Seafaring (planner_ai_seafaring.cc) ---
	void gain_ship(Widelands::Ship&, NewShip);
	bool marine_main_decisions(const Time&);
	void evaluate_fleet();
	void manage_shipyards();
	void manage_ports();
	bool check_ships(const Time&);
	void check_ship_in_expedition(ShipObserver&, const Time&);
	void expedition_management(ShipObserver&);
	void warship_management(ShipObserver&);
	bool attempt_escape(ShipObserver&);
	uint8_t spot_scoring(Widelands::Coords);
	Widelands::IslandExploreDirection randomExploreDirection();
	Widelands::ShipFleet* get_main_fleet();
	bool other_player_accessible(uint32_t max_distance,
	                             uint32_t* tested_fields,
	                             uint16_t* mineable_fields_count,
	                             const Widelands::Coords& starting_spot);

	// --- Player statistics ---
	void update_player_stat(const Time&);

	// --- Utilities ---
	uint32_t calculate_stocklevel(Widelands::DescriptionIndex) const;
	uint32_t calculate_total_stocklevel(Widelands::DescriptionIndex) const;
	BuildingObserver& get_building_observer(Widelands::DescriptionIndex);
	BuildingObserver& get_building_observer(char const*);

	// Per-ware renewability: true if the ware can be produced indefinitely
	// through a chain of non-depleting buildings (not mines, not quarries).
	// Computed once at late_initialization() via iterative fixed-point.
	// Used for conservation premium on military construction materials.
	std::vector<bool> ware_inherently_renewable_;

	// 3-Part Distribution: per building type, the fraction of wantedness
	// that stays as "build this building" score (0..1000 = 0%..100%).
	// Computed in update_ware_pressures(), used in update_building_pressures().
	// Satisfied needs → building score, missing needs → ware pressure.
	std::vector<int32_t> building_supply_score_;

	// CM affordability ratio per building type (0..1000 = 0%..100%).
	// Computed in update_ware_pressures(). 1000 = fully affordable or already
	// built. 0 = no CM stock at all. Used to dampen demand propagation
	// through unaffordable buildings (the "CM-missing penalty").
	std::vector<int32_t> cm_ratio_;

	// Per-worker-type producibility, precomputed in update_building_pressures().
	// A worker is "producible" if all its buildcost tools have built producers.
	// Used for counter-pressure dampening and anti-windup of worker-blocked buildings.
	std::map<Widelands::DescriptionIndex, bool> worker_producible_;

	// Land value estimation: average production value of one conquered field
	// in "ware pressure units". Computes max(production_value) across all
	// building types that could use a generic field, amortized over the
	// building's work area. Cached per call to update_expansion_pressures().
	int32_t land_value_per_field_{0};

	// --- Scheduler ---
	enum class TaskId : uint8_t {
		kFieldCheck,
		kConstructBuilding,
		kRoadCheck,
		kCheckEconomies,
		kCheckProductionsites,
		kCheckMines,
		kCheckMilitarysites,
		kCheckTrainingsites,
		kCheckEnemySites,
		kCountMilitaryVacant,
		kUpdateStats,
		kDiplomacy,
		kMarineDecisions,
		kCheckShips,
		kSetRangersPolicy,
		kUpdateProductionStats,
		kReviewWareTargets,
	};
	struct Task {
		Time due_time;
		TaskId id;
	};
	std::vector<Task> tasks_;

	// --- Data collections ---
	std::vector<BuildingObserver> buildings_;
	std::deque<Widelands::FCoords> unusable_fields;
	std::deque<UniversalBuildableField*> buildable_fields;
	BlockedFields blocked_fields;

	std::deque<ProductionSiteObserver> productionsites;
	std::deque<ProductionSiteObserver> mines_;
	std::deque<ProductionSiteObserver> shipyardsites;
	std::deque<MilitarySiteObserver> militarysites;
	std::deque<WarehouseSiteObserver> warehousesites;
	std::deque<PortSiteObserver> portsites;
	std::deque<TrainingSiteObserver> trainingsites;
	std::deque<EconomyObserver*> economies;
	std::deque<Widelands::Flag const*> new_flags;
	std::deque<Widelands::Road const*> roads;

	std::vector<WareObserver> wares;
	PlayersStrengths player_statistics;

	// Enemy tracking
	std::map<uint32_t, EnemySiteObserver> enemy_sites;
	Duration enemysites_check_delay_{Duration(120)};

	// Mine type tracking
	std::map<int32_t, MineTypesObserver> mines_per_type;

	// Soldier tracking
	SoldiersStatus soldier_status_{SoldiersStatus::kFull};
	uint16_t attackers_count_{0U};
	EventTimeQueue soldier_trained_log;
	Time last_attack_time_{Time(0)};

	// Training site tracking
	int16_t ts_finished_count_{0};
	int16_t ts_in_const_count_{0};
	int16_t ts_without_trainers_{0};

	// Seafaring
	std::deque<ShipObserver> allships;
	Widelands::Serial expedition_ship_{kNoShip};
	std::unordered_set<uint32_t> expedition_visited_spots;
	uint16_t ports_count{0U};
	uint16_t ports_finished_count{0U};
	uint16_t expeditions_in_prep{0U};
	uint16_t expeditions_ready{0U};
	uint16_t expeditions_in_progress{0U};
	uint16_t warships_count{0U};
	uint16_t tradeships_count{0U};
	uint16_t fleet_target{1U};
	bool start_expedition{false};
	bool warship_needed{false};
	bool tradeship_refit_needed{false};

	// Wood policy
	std::map<Widelands::DescriptionIndex, WoodPolicy> wood_policy_;

	// Building counts
	uint32_t numof_psites_in_constr{0U};
	uint16_t numof_warehouses_{0U};
	uint16_t numof_warehouses_in_const_{0U};
	bool basic_economy_established{false};
	Time military_last_dismantle_{Time(0)};
	Time military_last_build_{Time(0)};

	// Time tracking
	Time next_ai_think_;
	Time last_road_dismantled_{Time(0)};

	int32_t spots_{0};
	int32_t trees_on_territory_{0};
	int32_t rocks_on_territory_{0};

	// Per-attribute territory-wide resource count (attr_id -> count).
	// Populated in update_all_buildable_fields() alongside trees/rocks counts.
	// Used for predictive supporter pressure (depletion anticipation).
	std::map<uint32_t, int32_t> resource_on_territory_;

	// Derived decision weights, recomputed each PID tick.
	AIWeights weights_;

	// Set of attribute IDs that any resource harvester building cares about.
	// Populated once during init, used to filter the field scan.
	std::set<uint32_t> interesting_resource_attributes_;

	// id of iron as resource to identify iron mines
	int32_t iron_resource_id{Widelands::INVALID_INDEX};

	// PID tick counter: how many times update_ware_pressures has been called.
	// Ticks 1-10: warmup phase (equal building seed, no game goal injection).
	// Ticks 11+: normal operation (game goal injection active).
	uint16_t pi_tick_count_{0};

	// Notification subscribers
	std::unique_ptr<Notifications::Subscriber<Widelands::NoteFieldPossession>>
	   field_possession_subscriber_;
	std::unique_ptr<Notifications::Subscriber<Widelands::NoteImmovable>> immovable_subscriber_;
	std::unique_ptr<Notifications::Subscriber<Widelands::NoteProductionSiteOutOfResources>>
	   outofresource_subscriber_;
	std::unique_ptr<Notifications::Subscriber<Widelands::NoteTrainingSiteSoldierTrained>>
	   soldiertrained_subscriber_;
	std::unique_ptr<Notifications::Subscriber<Widelands::NoteShip>> shipnotes_subscriber_;
};

}  // namespace AI
#endif  // end of include guard: WL_AI_PLANNER_AI_H
