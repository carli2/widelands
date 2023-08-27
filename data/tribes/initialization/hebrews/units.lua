descriptions = wl.Descriptions() -- TODO(matthiakl): only for savegame compatibility with 1.0, do not use.

-- TODO:
-- Weinberg -> Wein
-- Dattelhain -> Dattelkuchen
-- Olivenhain -> Oliven
-- Ölmühle -> Olivenöl
-- Schäfer + Schärer -> Wolle, Fleisch
-- Wolle -> Garn
-- Garn -> Ziziot
-- Garn -> Stoff
-- Stoff -> Tallit
-- Eisen = Kupfererz
-- Smeltery -> Bronze
-- Gold -> Münze
-- Münze -> Zedernholz
-- Gold -> Blattgold
-- Weizen -> (Tenne) Weizenkörner
-- Weizenkörner -> Mehl
-- Mehl + Öl -> Fladenbrot
-- Lehm
-- Donkey
-- Leather
-- Leather -> Tephelin

image_dirname = path.dirname(__file__) .. "images/"

push_textdomain("tribes_encyclopedia")

-- For formatting time strings
include "tribes/scripting/help/time_strings.lua"

wl.Descriptions():new_tribe {
   name = "hebrews",
   animation_directory = image_dirname,
   animations = {
      frontier = { hotspot = {1, 19} },
      pinned_note = { hotspot = {18, 67} },
      bridge_normal_e = { hotspot = {-1, 13} },
      bridge_busy_e = { hotspot = {-1, 13} },
      bridge_normal_se = { hotspot = {8, 3} },
      bridge_busy_se = { hotspot = {8, 3} },
      bridge_normal_sw = { hotspot = {41, 3} },
      bridge_busy_sw = { hotspot = {41, 3} }
   },
   spritesheets = {
      flag = {
         fps = 10,
         frames = 16,
         columns = 8,
         rows = 2,
         hotspot = { 12, 40 }
      },
   },

   bridge_height = 8,

   collectors_points_table = {
      { ware = "gold", points = 3},
      { ware = "log", points = 4},
      -- TODO: 2-10 points for items
   },

   -- Image file paths for this tribe's road and waterway textures
   roads = {
      busy = {
         image_dirname .. "roadt_busy.png",
      },
      normal = {
         image_dirname .. "roadt_normal_00.png",
         image_dirname .. "roadt_normal_01.png",
         image_dirname .. "roadt_normal_02.png",
      },
      waterway = {
         image_dirname .. "waterway_0.png",
      },
   },

   resource_indicators = {
      [""] = {
         [0] = "empire_resi_none",
      },
      resource_coal = {
         [10] = "empire_resi_coal_1",
         [20] = "empire_resi_coal_2",
      },
      resource_iron = {
         [10] = "empire_resi_iron_1",
         [20] = "empire_resi_iron_2",
      },
      resource_gold = {
         [10] = "empire_resi_gold_1",
         [20] = "empire_resi_gold_2",
      },
      resource_stones = {
         [10] = "empire_resi_stones_1",
         [20] = "empire_resi_stones_2",
      },
      resource_water = {
         [100] = "empire_resi_water",
      },
   },

   -- Wares positions in wares windows.
   -- This also gives us the information which wares the tribe uses.
   -- Each subtable is a column in the wares windows.
   wares_order = {
      {
         -- Building Materials
         {
            name = "clay",
            default_target_quantity = 30,
            preciousness = 3,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Granite, part 1
                  pgettext("ware", "Clay is a basic building material."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Granite, part 2
                  pgettext("hebrews_ware", "The Hebrews produce clay by digging it from the earth.")
               }
            }
         },
         {
            name = "granite",
            default_target_quantity = 20,
            preciousness = 5,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Granite, part 1
                  pgettext("ware", "Granite is a basic building material."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Granite, part 2
                  pgettext("barbarians_ware", "The Barbarians produce granite blocks in quarries and granite mines.")
               }
            }
         },
         {
            name = "log",
            preciousness = 2,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Log, part 1
                  pgettext("ware", "Logs are an important basic building material. They are produced by felling trees."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Log, part 2
                  pgettext("barbarians_ware", "Barbarian lumberjacks fell the trees; rangers take care of the supply of trees. Logs are also used in the metal workshop to build basic tools, and in the charcoal kiln for the production of coal. The wood hardener refines logs into blackwood by hardening them with fire.")
               }
            }
         },
         {
            name = "cloth",
            default_target_quantity = 10,
            preciousness = 0,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Cloth
               purpose = pgettext("barbarians_ware", "Cloth is needed for Barbarian ships. It is produced out of reed.")
            }
         }
      },
      {
         -- Food
         {
            name = "fish",
            preciousness = 3,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Fish
               purpose = pgettext("barbarians_ware", "Besides pitta bread and meat, fish is also a foodstuff for the Barbarians. It is used in the taverns, inns and big inns and at the training sites (training camp and battle arena).")
            }
         },
         {
            name = "meat",
            preciousness = 3,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Meat, part 1
                  pgettext("ware", "Meat contains a lot of energy, and it is obtained from wild game taken by hunters."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Meat, part 2
                  pgettext("barbarians_ware", "Meat is used in the taverns, inns and big inns to prepare rations, snacks and meals for the miners. It is also consumed at the training sites (training camp and battle arena).")
               }
            }
         },
         {
            name = "water",
            preciousness = 8,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Water, part 1
                  pgettext("ware", "Water is the essence of life!"),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Water, part 2
                  pgettext("barbarians_ware", "Water is used in the bakery, the micro brewery and the brewery. The lime kiln and the cattle farm also need to be supplied with water.")
               }
            }
         },
         {
            name = "wheat",
            preciousness = 12,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Wheat, part 1
                  pgettext("ware", "Wheat is essential for survival."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Wheat, part 2
                  pgettext("barbarians_ware", "Wheat is produced by farms and consumed by bakeries, micro breweries and breweries. Cattle farms also need to be supplied with wheat.")
               }
            }
         },
         {
            name = "wheat_grains",
            preciousness = 12,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Wheat, part 1
                  pgettext("ware", "Wheat is essential for survival."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Wheat, part 2
                  pgettext("barbarians_ware", "Wheat is produced by farms and consumed by bakeries, micro breweries and breweries. Cattle farms also need to be supplied with wheat.")
               }
            }
         },
         {
            name = "flour",
            preciousness = 12,
            default_target_quantity = 10,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Wheat, part 1
                  pgettext("ware", "Wheat is essential for survival."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Wheat, part 2
                  pgettext("barbarians_ware", "Wheat is produced by farms and consumed by bakeries, micro breweries and breweries. Cattle farms also need to be supplied with wheat.")
               }
            }
         },
         {
            name = "barbarians_bread",
            default_target_quantity = 20,
            preciousness = 4,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Pitta Bread
               purpose = pgettext("barbarians_ware", "The Barbarian bakers are best in making this flat and tasty pitta bread. It is made out of wheat and water following a secret recipe. Pitta bread is used in the taverns, inns and big inns to prepare rations, snacks and meals. It is also consumed at training sites (training camp and battle arena).")
            }
         },
         {
            name = "grape",
            default_target_quantity = 20,
            preciousness = 4,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Pitta Bread
               purpose = pgettext("barbarians_ware", "The Barbarian bakers are best in making this flat and tasty pitta bread. It is made out of wheat and water following a secret recipe. Pitta bread is used in the taverns, inns and big inns to prepare rations, snacks and meals. It is also consumed at training sites (training camp and battle arena).")
            }
         },
         {
            name = "wine",
            default_target_quantity = 20,
            preciousness = 4,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Pitta Bread
               purpose = pgettext("barbarians_ware", "The Barbarian bakers are best in making this flat and tasty pitta bread. It is made out of wheat and water following a secret recipe. Pitta bread is used in the taverns, inns and big inns to prepare rations, snacks and meals. It is also consumed at training sites (training camp and battle arena).")
            }
         },
         {
            name = "olives",
            default_target_quantity = 20,
            preciousness = 4,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Pitta Bread
               purpose = pgettext("barbarians_ware", "The Barbarian bakers are best in making this flat and tasty pitta bread. It is made out of wheat and water following a secret recipe. Pitta bread is used in the taverns, inns and big inns to prepare rations, snacks and meals. It is also consumed at training sites (training camp and battle arena).")
            }
         },
         {
            name = "olive_oil",
            default_target_quantity = 20,
            preciousness = 4,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Pitta Bread
               purpose = pgettext("barbarians_ware", "The Barbarian bakers are best in making this flat and tasty pitta bread. It is made out of wheat and water following a secret recipe. Pitta bread is used in the taverns, inns and big inns to prepare rations, snacks and meals. It is also consumed at training sites (training camp and battle arena).")
            }
         },
      },
      {
         -- Mining
         {
            name = "coal",
            default_target_quantity = 20,
            preciousness = 20,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Coal, part 1
                  pgettext("ware", "Coal is mined in coal mines or produced out of logs by a charcoal kiln."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Coal, part 2
                  pgettext("barbarians_ware", "The fires of the Barbarians are usually fed with coal. Consumers are several buildings: lime kiln, smelting works, ax workshop, war mill, and helm smithy.")
               }
            }
         },
         {
            name = "iron_ore",
            default_target_quantity = 15,
            preciousness = 4,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Iron Ore, part 1
                  pgettext("default_ware", "Iron ore is mined in iron mines."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Iron Ore, part 2
                  pgettext("barbarians_ware", "It is smelted in a smelting works to retrieve the iron.")
               }
            }
         },
         {
            name = "iron",
            default_target_quantity = 20,
            preciousness = 4,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Iron, part 1
                  pgettext("ware", "Iron is smelted out of iron ores."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Iron, part 2
                  pgettext("barbarians_ware", "It is produced by the smelting works and used to produce weapons and tools in the metal workshop, ax workshop, war mill and helm smithy.")
               }
            }
         },
         {
            name = "gold_ore",
            default_target_quantity = 15,
            preciousness = 2,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Gold Ore, part 1
                  pgettext("ware", "Gold ore is mined in a gold mine."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Gold Ore, part 2
                  pgettext("barbarians_ware", "Smelted in a smelting works, it turns into gold which is used as a precious building material and to produce weapons and armor.")
               }
            }
         },
         {
            name = "gold",
            default_target_quantity = 20,
            preciousness = 2,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Gold, part 1
                  pgettext("ware", "Gold is the most valuable of all metals, and it is smelted out of gold ore."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Gold, part 2
                  pgettext("barbarians_ware", "Only very important things are embellished with gold. It is produced by the smelting works and used as a precious building material and to produce different axes (in the war mill) and different parts of armor (in the helm smithy).")
               }
            }
         }
      },
      {
			-- Middleware
			{
				name = "wool",
				default_target_quantity = 10,
				preciousness = 20,
				helptexts = {
					-- TRANSLATORS: Helptext for Hebrews ware: Wool
					purpose = pgettext("hebrews_ware", "Most textiles are made from wool")
				}
			},
			{
				name = "yarn",
				default_target_quantity = 10,
				preciousness = 20,
				helptexts = {
					-- TRANSLATORS: Helptext for Hebrews ware: Wool
					purpose = pgettext("hebrews_ware", "Most textiles are made from wool")
				}
			},
			{
				name = "fur",
				default_target_quantity = 10,
				preciousness = 20,
				helptexts = {
					-- TRANSLATORS: Helptext for Hebrews ware: Wool
					purpose = pgettext("hebrews_ware", "Most textiles are made from wool")
				}
			},
			{
				name = "sheep2",
				default_target_quantity = 10,
				preciousness = 20,
				helptexts = {
					-- TRANSLATORS: Helptext for Hebrews ware: Wool
					purpose = pgettext("hebrews_ware", "Most textiles are made from wool")
				}
			},
      },
      {
         -- Tools
         {
            name = "pick",
            default_target_quantity = 2,
            preciousness = 1,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian ware: Pick
               purpose = pgettext("barbarians_ware", "Picks are used by stonemasons and miners. They are produced in the metal workshop (but cease to be produced by the building if it is enhanced to an ax workshop and war mill).")
            }
         },
         {
            name = "felling_ax",
            default_target_quantity = 5,
            preciousness = 3,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Felling Ax, part 1
                  pgettext("ware", "The felling ax is the tool to chop down trees."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Felling Ax, part 2
                  pgettext("barbarians_ware", "Felling axes are used by lumberjacks and produced in the metal workshop (but cease to be produced by the building if it is enhanced to an ax workshop and war mill).")
               }
            }
         },
         {
            name = "hammer",
            default_target_quantity = 2,
            preciousness = 1,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Hammer, part 1
                  pgettext("ware", "The hammer is an essential tool."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Hammer, part 2
                  pgettext("barbarians_ware", "Geologists, builders, blacksmiths and helmsmiths all need a hammer. Make sure you’ve always got some in reserve! They are one of the basic tools produced at the metal workshop (but cease to be produced by the building if it is enhanced to an ax workshop and war mill).")
               }
            }
         },
         {
            name = "fishing_rod",
            default_target_quantity = 1,
            preciousness = 0,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Fishing Rod, part 1
                  pgettext("ware", "Fishing rods are needed by fishers to catch fish."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Fishing Rod, part 2
                  pgettext("barbarians_ware", "They are one of the basic tools produced in a metal workshop (but cease to be produced by the building if it is enhanced to an ax workshop and war mill).")
               }
            }
         },
      },
      {
         -- Weapons & Armor
         {
            name = "tunic",
            default_target_quantity = 5,
            preciousness = 0,
            helptexts = {
               purpose = {
                  -- TRANSLATORS: Helptext for a Barbarian ware: Fishing Rod, part 1
                  pgettext("ware", "Fishing rods are needed by fishers to catch fish."),
                  -- TRANSLATORS: Helptext for a Barbarian ware: Fishing Rod, part 2
                  pgettext("barbarians_ware", "They are one of the basic tools produced in a metal workshop (but cease to be produced by the building if it is enhanced to an ax workshop and war mill).")
               }
            }
			}
	 -- TODO: slingshot, ziziot, urim+turim
      }
   },

   -- Workers positions in workers windows.
   -- This also gives us the information which workers the tribe uses.
   -- Each subtable is a column in the workers windows.
   workers_order = {
      {
         -- Carriers
         {
            name = "hebrews_carrier",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Carrier
               purpose = pgettext("barbarians_worker", "Carries items along your roads.")
            }
         },
         {
            name = "hebrews_ferry",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Ferry
               purpose = pgettext("barbarians_worker", "Ships wares across narrow rivers.")
            }
         },
         {
            name = "hebrews_donkey",
            default_target_quantity = 10,
            preciousness = 2,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Ox
               purpose = pgettext("barbarians_worker", "Oxen help to carry items along busy roads. They are reared in a cattle farm.")
            }
         },
         {
            name = "hebrews_scout",
            helptexts = {
               -- TRANSLATORS: Helptext for an Empire worker: Scout
               purpose = pgettext("hebrews_worker", "Scouts like Scotty the scout scouting unscouted areas in a scouty fashion.")
            }
         },
      },
      {
         -- Building Materials
         {
            name = "hebrews_stonemason",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Stonemason
               purpose = pgettext("barbarians_worker", "Cuts raw pieces of granite out of rocks in the vicinity.")
            }
         },
         {
            name = "hebrews_lumberjack",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Lumberjack
               purpose = pgettext("barbarians_worker", "Fells trees.")
            }
         },
         {
            name = "hebrews_claydigger",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Carrier
               purpose = pgettext("hebrews_worker", "Digs out clay from dust.")
            }
         },
         {
            name = "hebrews_builder",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Builder
               purpose = pgettext("barbarians_worker", "Works at construction sites to raise new buildings.")
            }
         },
      },
      {
         -- Food
         {
            name = "hebrews_fisher",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Fisher
               purpose = pgettext("barbarians_worker", "Catches fish in the sea.")
            }
         },
         {
            name = "hebrews_farmer",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Farmer
               purpose = pgettext("barbarians_worker", "Plants fields.")
            }
         },
         {
            name = "hebrews_shepherd",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Builder
               purpose = pgettext("barbarians_worker", "Works at construction sites to raise new buildings.")
            }
         },
         {
            name = "hebrews_butcher",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Baker
               purpose = pgettext("barbarians_worker", "Bakes pitta bread for the miners, soldiers and scouts.")
            }
         },
         {
            name = "hebrews_baker",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Baker
               purpose = pgettext("barbarians_worker", "Bakes pitta bread for the miners, soldiers and scouts.")
            }
         },
      },
      {
         -- Mining
         {
            name = "hebrews_geologist",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Geologist
               purpose = pgettext("barbarians_worker", "Discovers resources for mining.")
            }
         },
         {
            name = "barbarians_miner",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Miner
               purpose = pgettext("barbarians_worker", "Works deep in the mines to obtain coal, iron, gold or granite.")
            }
         },
      },
      {
         -- Tools
         {
            name = "barbarians_blacksmith",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Blacksmith
               purpose = pgettext("barbarians_worker", "Produces weapons for soldiers and tools for workers.")
            }
         },
      },
      {
         -- Military
         {
            name = "barbarians_recruit",
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Recruit
               purpose = pgettext("barbarians_worker", "Eager to become a soldier and defend his tribe!")
            }
         },
         {
            name = "barbarians_soldier",
            default_target_quantity = 10,
            preciousness = 5,
            helptexts = {
               -- TRANSLATORS: Helptext for a Barbarian worker: Soldier
               purpose = pgettext("barbarians_worker", "Defend and Conquer!")
            }
         },
      }
   },

   immovables = {
      {
         name = "ashes",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Ashes
            purpose = _("The remains of a destroyed building.")
         }
      },
      {
         name = "destroyed_building",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Destroyed Building
            purpose = _("The remains of a destroyed building.")
         }
      },
      {
         name = "wheatfield_tiny",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Wheat field
            purpose = _("This field has just been planted.")
         }
      },
      {
         name = "wheatfield_small",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Wheat field
            purpose = _("This field is growing.")
         }
      },
      {
         name = "wheatfield_medium",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Wheat field
            purpose = _("This field is growing.")
         }
      },
      {
         name = "wheatfield_ripe",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Wheat field
            purpose = _("This field is ready for harvesting.")
         }
      },
      {
         name = "wheatfield_harvested",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Wheat field
            purpose = _("This field has been harvested.")
         }
      },
      {
         name = "barbarians_resi_coal_1",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Coal, part 1
               _("Coal veins contain coal that can be dug up by coal mines."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Coal part 2
               _("There is only a little bit of coal here.")
            }
         }
      },
      {
         name = "barbarians_resi_iron_1",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Iron, part 1
               _("Iron veins contain iron ore that can be dug up by iron mines."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Iron, part 2
               _("There is only a little bit of iron here.")
            }
         }
      },
      {
         name = "barbarians_resi_gold_1",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Gold, part 1
               _("Gold veins contain gold ore that can be dug up by gold mines."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Gold, part 2
               _("There is only a little bit of gold here.")
            }
         }
      },
      {
         name = "barbarians_resi_stones_1",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Stones, part 1
               _("Granite is a basic building material and can be dug up by a granite mine."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Stones, part 2
               _("There is only a little bit of granite here."),
            }
         }
      },
      {
         name = "barbarians_resi_coal_2",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Coal, part 1
               _("Coal veins contain coal that can be dug up by coal mines."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Coal, part 2
               _("There is a lot of coal here.")
            }
         }
      },
      {
         name = "barbarians_resi_iron_2",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Iron, part 1
               _("Iron veins contain iron ore that can be dug up by iron mines."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Iron, part 2
               _("There is a lot of iron here.")
            }
         }
      },
      {
         name = "barbarians_resi_gold_2",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Gold, part 1
               _("Gold veins contain gold ore that can be dug up by gold mines."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Gold, part 2
               _("There is a lot of gold here.")
            }
         }
      },
      {
         name = "barbarians_resi_stones_2",
         helptexts = {
            purpose = {
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Stones, part 1
               _("Granite is a basic building material and can be dug up by a granite mine."),
               -- TRANSLATORS: Helptext for a Barbarian resource indicator: Stones, part 2
               _("There is a lot of granite here.")
            }
         }
      },
      {
         name = "barbarians_shipconstruction",
         helptexts = {
            -- TRANSLATORS: Helptext for a Barbarian immovable: Ship Under Construction
            purpose = _("A ship is being constructed at this site.")
         }
      },
      -- non Barbarian immovables used by the woodcutter
      {
         name = "deadtree7",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Dead Tree
            purpose = _("The remains of an old tree.")
         }
      },
      {
         name = "balsa_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Balsa Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "balsa_black_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Balsa Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "balsa_desert_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Balsa Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "balsa_winter_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Blackroot Field
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "ironwood_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Balsa Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "ironwood_black_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Ironwood Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "ironwood_desert_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Ironwood Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "ironwood_winter_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Ironwood Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "rubber_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Rubber Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "rubber_black_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Rubber Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "rubber_desert_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Rubber Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
      {
         name = "rubber_winter_amazons_old",
         helptexts = {
            -- TRANSLATORS: Helptext for an Amazon immovable usable by the Barbarians: Rubber Tree
            purpose = _("This tree is only planted by the Amazon tribe but can be harvested for logs.")
         }
      },
   },

   -- The order here also determines the order in lists on screen.
   buildings = {
      -- Warehouses
      {
         name = "hebrews_headquarters",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian warehouse: Headquarters
            lore = pgettext("barbarians_building", "‘Steep like the slopes of Kal’mavrath, shiny like the most delicate armor and strong like our ancestors, that’s how the headquarters of Chat’Karuth presented itself to us.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian warehouse: Headquarters
            lore_author = pgettext("barbarians_building", "Ballad ‘The Battle of Kal’mavrath’ by Hakhor the Bard"),
            -- TRANSLATORS: Purpose helptext for a Barbarian warehouse: Headquarters
            purpose = pgettext("barbarians_building", "Accommodation for your people. Also stores your wares and tools."),
            -- TRANSLATORS: Note helptext for a Barbarian warehouse: Headquarters
            note = pgettext("barbarians_building", "The headquarters is your main building.")
         }
      },
      {
         name = "hebrews_headquarters_tent",
         helptexts = {
            -- TRANSLATORS: Purpose helptext for a Barbarian warehouse: Headquarters
            purpose = pgettext("barbarians_building", "Accommodation for your people. Also stores your wares and tools."),
            -- TRANSLATORS: Note helptext for a Barbarian warehouse: Headquarters
            note = pgettext("barbarians_building", "The headquarters is your main building.")
         }
      },
      {
         name = "hebrews_warehouse",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian warehouse: Warehouse
            lore = pgettext("barbarians_building", "‘Who still owns a warehouse is not yet defeated!’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian warehouse: Warehouse
            lore_author = pgettext("barbarians_building", "Berthron, chief military adviser of Chat’Karuth,<br>when they lost the headquarters in the battle around the heights of Kal’Megarath"),
            -- TRANSLATORS: Purpose helptext for a Barbarian warehouse: Warehouse
            purpose = pgettext("barbarians_building", "Your workers and soldiers will find shelter here. Also stores your wares and tools.")
         }
      },
      {
         name = "hebrews_port",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian warehouse: Port
            lore = pgettext("barbarians_building", "‘I prefer the planks of a ship to any fortress, no matter how strong it is.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian warehouse: Port
            lore_author = pgettext("barbarians_building", "Captain Thanlas the Elder,<br>Explorer"),
            -- TRANSLATORS: Purpose helptext for a Barbarian warehouse: Port
            purpose = pgettext("barbarians_building", "Serves as a base for overseas colonization and trade. Also stores your soldiers, wares and tools."),
            -- TRANSLATORS: Note helptext for an Barbarian warehouse: Port
            note = pgettext("barbarians_building", "Similar to the Headquarters a Port can be attacked and destroyed by an enemy. It is recommendable to send soldiers to defend it.")
         }
      },

      -- Small
      {
         name = "hebrews_fishers_hut",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Quarry
            lore = pgettext("barbarians_building", "‘We open up roads and make houses from mountains.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Quarry
            lore_author = pgettext("barbarians_building", "Slogan of the stonemasons’ guild"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Quarry
            purpose = pgettext("barbarians_building", "Cuts raw pieces of granite out of rocks in the vicinity."),
            -- TRANSLATORS: Note helptext for a Barbarian production site: Quarry
            note = pgettext("barbarians_building", "The quarry needs rocks to cut within the work area."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Quarry
            performance = pgettext("barbarians_building", "The stonemason pauses %s before going back to work again."):bformat(format_minutes_seconds(1, 5))
         }
      },
      {
         name = "hebrews_well",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Quarry
            lore = pgettext("barbarians_building", "‘We open up roads and make houses from mountains.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Quarry
            lore_author = pgettext("barbarians_building", "Slogan of the stonemasons’ guild"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Quarry
            purpose = pgettext("barbarians_building", "Cuts raw pieces of granite out of rocks in the vicinity."),
            -- TRANSLATORS: Note helptext for a Barbarian production site: Quarry
            note = pgettext("barbarians_building", "The quarry needs rocks to cut within the work area."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Quarry
            performance = pgettext("barbarians_building", "The stonemason pauses %s before going back to work again."):bformat(format_minutes_seconds(1, 5))
         }
      },
      {
         name = "hebrews_clay_pit",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Quarry
            lore = pgettext("barbarians_building", "‘We open up roads and make houses from mountains.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Quarry
            lore_author = pgettext("barbarians_building", "Slogan of the stonemasons’ guild"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Quarry
            purpose = pgettext("barbarians_building", "Cuts raw pieces of granite out of rocks in the vicinity."),
            -- TRANSLATORS: Note helptext for a Barbarian production site: Quarry
            note = pgettext("barbarians_building", "The quarry needs rocks to cut within the work area."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Quarry
            performance = pgettext("barbarians_building", "The stonemason pauses %s before going back to work again."):bformat(format_minutes_seconds(1, 5))
         }
      },
      {
         name = "hebrews_shepherds",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Quarry
            lore = pgettext("barbarians_building", "‘We open up roads and make houses from mountains.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Quarry
            lore_author = pgettext("barbarians_building", "Slogan of the stonemasons’ guild"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Quarry
            purpose = pgettext("barbarians_building", "Cuts raw pieces of granite out of rocks in the vicinity."),
            -- TRANSLATORS: Note helptext for a Barbarian production site: Quarry
            note = pgettext("barbarians_building", "The quarry needs rocks to cut within the work area."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Quarry
            performance = pgettext("barbarians_building", "The stonemason pauses %s before going back to work again."):bformat(format_minutes_seconds(1, 5))
         }
      },
      {
         name = "hebrews_quarry",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Quarry
            lore = pgettext("barbarians_building", "‘We open up roads and make houses from mountains.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Quarry
            lore_author = pgettext("barbarians_building", "Slogan of the stonemasons’ guild"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Quarry
            purpose = pgettext("barbarians_building", "Cuts raw pieces of granite out of rocks in the vicinity."),
            -- TRANSLATORS: Note helptext for a Barbarian production site: Quarry
            note = pgettext("barbarians_building", "The quarry needs rocks to cut within the work area."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Quarry
            performance = pgettext("barbarians_building", "The stonemason pauses %s before going back to work again."):bformat(format_minutes_seconds(1, 5))
         }
      },
      {
         name = "hebrews_lumberjacks_hut",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Lumberjack_s Hut
            lore = pgettext("barbarians_building", "‘Take 200 hits to fell a tree and you’re a baby. Take 100 and you’re a soldier. Take 50 and you’re a hero. Take 20 and soon you will be a honorable lumberjack.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Lumberjack_s Hut
            lore_author = pgettext("barbarians_building", "Krumta, carpenter of Chat’Karuth"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Lumberjack_s Hut
            purpose = pgettext("building", "Fells trees in the surrounding area and processes them into logs."),
            -- TRANSLATORS: Note helptext for a Barbarian production site: Lumberjack_s Hut
            note = pgettext("barbarians_building", "The lumberjack’s hut needs trees to fell within the work area."),
            performance = {
               -- TRANSLATORS: Performance helptext for a Barbarian production site: Lumberjack_s Hut
               pgettext("barbarians_building", "The lumberjack needs %s to fell a tree, not counting the time he needs to reach the destination and go home again."):bformat(format_seconds(17)),
               -- TRANSLATORS: Performance helptext for a Barbarian production site: Lumberjack_s Hut
               pgettext("barbarians_building", "Afterwards he rests in the hut for %s."):bformat(format_seconds(20))
            }
         }
      },
      --[[
      {
         name = "barbarians_well",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Well
            lore = pgettext("barbarians_building", [[‘Oh how sweet is the source of life,<br> that comes down from the sky <br> and lets the earth drink.’] ]),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Well
            lore_author = pgettext("barbarians_building", "Song written by Sigurd the Bard when the first rain fell after the Great Drought in the 21ˢᵗ year of Chat’Karuth’s reign."),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Well
            purpose = pgettext("building", "Draws water out of the deep."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Well
            performance = pgettext("barbarians_building", "The carrier needs %s to get one bucket full of water."):bformat(format_seconds(40))
         }
      },
      ]]--
      {
         name = "hebrews_scouts_hut",
         helptexts = {
            no_scouting_building_connected = pgettext("barbarians_building", "You need to connect this flag to a scout’s hut before you can send a scout here."),
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Scout's Hut
            lore = pgettext("barbarians_building", "‘Behind the next hill there might be wealth and happiness but also hostility and doom.<br>He who will not explore it commits the crime of stupidity.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Scout's Hut
            lore_author = pgettext("barbarians_building", "Chat’Karuth<br>at the oath taking ceremony of the first scout troupe"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Scout's Hut
            purpose = pgettext("building", "Explores unknown territory.")
         }
      },

      -- Medium
      {
         name = "hebrews_butchery",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Bakery
            lore = pgettext("barbarians_building", "‘He who has enough bread will never be too tired to dig the ore and wield the ax.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Bakery
            lore_author = pgettext("barbarians_building", "Khantarakh, ‘The Modern Barbarian Economy’,<br>3ʳᵈ cowhide ‘Craftsmanship and Trade’"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Bakery
            purpose = pgettext("barbarians_building", "Bakes pitta bread for soldiers and miners alike."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Bakery
            performance = pgettext("barbarians_building", "If all needed wares are delivered in time, this building can produce a pitta bread in %s on average."):bformat(format_seconds(34))
         }
      },
      {
         name = "hebrews_bakery",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Bakery
            lore = pgettext("barbarians_building", "‘He who has enough bread will never be too tired to dig the ore and wield the ax.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Bakery
            lore_author = pgettext("barbarians_building", "Khantarakh, ‘The Modern Barbarian Economy’,<br>3ʳᵈ cowhide ‘Craftsmanship and Trade’"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Bakery
            purpose = pgettext("barbarians_building", "Bakes pitta bread for soldiers and miners alike."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Bakery
            performance = pgettext("barbarians_building", "If all needed wares are delivered in time, this building can produce a pitta bread in %s on average."):bformat(format_seconds(34))
         }
      },
      {
         name = "hebrews_threshing_floor",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Bakery
            lore = pgettext("barbarians_building", "‘He who has enough bread will never be too tired to dig the ore and wield the ax.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Bakery
            lore_author = pgettext("barbarians_building", "Khantarakh, ‘The Modern Barbarian Economy’,<br>3ʳᵈ cowhide ‘Craftsmanship and Trade’"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Bakery
            purpose = pgettext("barbarians_building", "Bakes pitta bread for soldiers and miners alike."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Bakery
            performance = pgettext("barbarians_building", "If all needed wares are delivered in time, this building can produce a pitta bread in %s on average."):bformat(format_seconds(34))
         }
      },
      {
         name = "hebrews_mill",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Bakery
            lore = pgettext("barbarians_building", "‘He who has enough bread will never be too tired to dig the ore and wield the ax.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Bakery
            lore_author = pgettext("barbarians_building", "Khantarakh, ‘The Modern Barbarian Economy’,<br>3ʳᵈ cowhide ‘Craftsmanship and Trade’"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Bakery
            purpose = pgettext("barbarians_building", "Bakes pitta bread for soldiers and miners alike."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Bakery
            performance = pgettext("barbarians_building", "If all needed wares are delivered in time, this building can produce a pitta bread in %s on average."):bformat(format_seconds(34))
         }
      },

      -- Big
      {
         name = "hebrews_donkeyfarm",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Cattle Farm
            lore = pgettext("barbarians_building", "‘The smart leader builds roads, while the really wise leader breeds cattle.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Cattle Farm
            lore_author = pgettext("barbarians_building", "Khantarakh, ‘The Modern Barbarian Economy’,<br> 5ᵗʰ cowhide ‘Traffic and Logistics’"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Cattle Farm
            purpose = pgettext("barbarians_building", "Breeds strong oxen for adding them to the transportation system."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Cattle Farm
            performance = pgettext("barbarians_building", "If all needed wares are delivered in time, this building can produce an ox in %s on average."):bformat(format_seconds(30))
         }
      },
      {
         name = "hebrews_farm",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Farm
            lore = pgettext("barbarians_building", [[‘See the crop fields from here to the horizons. They are a huge, heaving, golden sea.<br>]] ..
                  [[Oh wheat, source of wealth, soul of beer, strength of our warriors!’]]),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Farm
            lore_author = pgettext("barbarians_building", "Line from the harvesting song ‘The Wealth of the Fields’"),
            -- TRANSLATORS: Purpose helptext for production site: Farm
            purpose = pgettext("building", "Sows and harvests wheat."),
            -- TRANSLATORS: Performance helptext for production site: Farm
            performance = pgettext("barbarians_building", "The farmer needs %1% on average to sow and harvest a sheaf of wheat."):bformat(format_minutes_seconds(1, 40))
         }
      },
      {
         name = "hebrews_oliveplant",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Farm
            lore = pgettext("barbarians_building", [[‘See the crop fields from here to the horizons. They are a huge, heaving, golden sea.<br>]] ..
                  [[Oh wheat, source of wealth, soul of beer, strength of our warriors!’]]),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Farm
            lore_author = pgettext("barbarians_building", "Line from the harvesting song ‘The Wealth of the Fields’"),
            -- TRANSLATORS: Purpose helptext for production site: Farm
            purpose = pgettext("building", "Sows and harvests wheat."),
            -- TRANSLATORS: Performance helptext for production site: Farm
            performance = pgettext("barbarians_building", "The farmer needs %1% on average to sow and harvest a sheaf of wheat."):bformat(format_minutes_seconds(1, 40))
         }
      },

      -- Mines
      --[[
      {
         name = "barbarians_granitemine",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Granite Mine
            lore = pgettext("barbarians_building", "‘I can handle tons of granite, man, but no more of your vain prattle.’"),
            lore_author = {
               -- TRANSLATORS: Lore author helptext for a Barbarian production site: Granite Mine, part 1
               pgettext("barbarians_building", "This phrase was the reply Rimbert the miner – later known as Rimbert the loner – gave, when he was asked to remain seated on an emergency meeting at Stonford in the year of the great flood."),
               -- TRANSLATORS: Lore author helptext for a Barbarian production site: Granite Mine, part 2
               pgettext("barbarians_building", "The same man had all the 244 granite blocks ready only a week later, and they still fortify the city’s levee.")
            },
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Granite Mine
            purpose = pgettext("barbarians_building", "Carves granite out of the rock in mountain terrain."),
            note = {
               -- TRANSLATORS: 'It' is a mine
               pgettext("barbarians_building", "It cannot be enhanced.")
            },
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Granite Mine
            performance = pgettext("barbarians_building", "If the food supply is steady, this mine can produce granite in %s on average."):bformat(format_seconds(24))
         }
      },
      {
         name = "barbarians_coalmine",
         helptexts = {
            lore = {
               -- TRANSLATORS: Lore helptext for production site: Coal Mine, part 1
               pgettext("barbarians_building", "Ages ago, the Barbarians learned to delve into mountainsides for that black material that feeds their furnaces."),
               -- TRANSLATORS: Lore helptext for production site: Coal Mine, part 2
               pgettext("barbarians_building", "Wood may serve for a household fire and to keep you warm, but when it comes to working with iron or gold, there is no way around coal.")
            },
            -- TRANSLATORS: Purpose helptext for production site: Coal Mine
            purpose = pgettext("building", "Digs coal out of the ground in mountain terrain."),
            -- TRANSLATORS: Performance helptext for production site: Coal Mine
            performance = pgettext("barbarians_building", "If the food supply is steady, this mine can produce coal in %s on average."):bformat(format_seconds(36))
         }
      },
      {
         name = "barbarians_ironmine",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Iron Mine
            lore = pgettext("barbarians_building", "‘I look at my own pick wearing away day by day and I realize why my work is important.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Iron Mine
            lore_author = pgettext("barbarians_building", "Quote from an anonymous miner."),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Iron Mine
            purpose = pgettext("building", "Digs iron ore out of the ground in mountain terrain."),
            -- TRANSLATORS: Performance helptext for a Barbarian production site: Iron Mine
            performance = pgettext("barbarians_building", "If the food supply is steady, this mine can produce iron ore in %s on average."):bformat(format_minutes_seconds(1, 9))
         }
      },
      {
         name = "barbarians_goldmine",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Gold Mine
            lore = pgettext("barbarians_building", "‘Soft and supple.<br> And yet untouched by time and weather.<br> Rays of sun, wrought into eternity…’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Gold Mine
            lore_author = pgettext("barbarians_building", "Excerpt from ‘Our Treasures Underground’,<br> a traditional Barbarian song."),
            -- TRANSLATORS: Purpose helptext for production site: Gold Mine
            purpose = pgettext("building", "Digs gold ore out of the ground in mountain terrain."),
            -- TRANSLATORS: Performance helptext for production site: Gold Mine
            performance = pgettext("barbarians_building", "If the food supply is steady, this mine can produce gold ore in %s on average."):bformat(format_minutes_seconds(1, 9))
         }
      },
      ]]--

      -- Training Sites
      {
         name = "hebrews_arena",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian training site: Battle Arena
            lore = pgettext("barbarians_building", "‘No better friend you have in battle than the enemy’s blow that misses.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian training site: Battle Arena
            lore_author = pgettext("barbarians_building", "Said to originate from Neidhardt, the famous trainer."),
            purpose = {
               -- TRANSLATORS: Purpose helptext for a Barbarian training site: Battle Arena, part 1
               pgettext("barbarians_building", "Trains soldiers in ‘Evade’."),
               -- TRANSLATORS: Purpose helptext for a Barbarian training site: Battle Arena, part 2
               pgettext("barbarians_building", "‘Evade’ increases the soldier’s chance not to be hit by the enemy and so to remain totally unaffected.")
            },
            -- TRANSLATORS: Note helptext for a Barbarian training site: Battle Arena
            note = pgettext("barbarians_building", "Barbarian soldiers cannot be trained in ‘Defense’ and will remain at their initial level."),
            -- TRANSLATORS: Performance helptext for a Barbarian training site: Battle Arena
            performance = pgettext("barbarians_building", "If all needed wares are delivered in time, a battle arena can train evade for one soldier from 0 to the highest level in %s on average."):bformat(format_minutes_seconds(1, 10))
         }
      },
      {
         name = "hebrews_trainingcamp",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian training site: Training Camp
            lore = pgettext("barbarians_building", "‘He who is strong shall neither forgive nor forget, but revenge injustice suffered – in the past and for all future.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian training site: Training Camp
            lore_author = pgettext("barbarians_building", "Chief Chat’Karuth in a speech to his army."),
            purpose = {
               -- TRANSLATORS: Purpose helptext for a Barbarian training site: Training Camp, part 1
               pgettext("barbarians_building", "Trains soldiers in ‘Attack’ and in ‘Health’."),
               -- TRANSLATORS: Purpose helptext for a Barbarian training site: Training Camp, part 2
               pgettext("barbarians_building", "Equips the soldiers with all necessary weapons and armor parts.")
            },
            -- TRANSLATORS: Note helptext for a Barbarian building: Training Camp
            note = pgettext("barbarians_building", "Barbarian soldiers cannot be trained in ‘Defense’ and will remain at their initial level."),
            -- TRANSLATORS: Performance helptext for a Barbarian training site: Training Camp
            performance = pgettext("barbarians_building", "If all needed wares are delivered in time, a training camp can train one new soldier in attack and health to the final level in %s on average."):bformat(format_minutes_seconds(4, 40))
         }
      },

      -- Military Sites
      {
         name = "hebrews_tent_small",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian military site: Sentry
            lore = pgettext("barbarians_building", "‘The log cabin was so small that two men could hardly live there. But we were young and carefree. We just relished our freedom and the responsibility as an outpost.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian military site: Sentry
            lore_author = pgettext("barbarians_building", "Boldreth,<br>about his time as young soldier"),
            -- TRANSLATORS: Purpose helptext for a Barbarian military site: Sentry
            purpose = pgettext("barbarians_building", "Garrisons soldiers to expand your territory."),
            -- TRANSLATORS: Note helptext for a Barbarian military site: Sentry
            note = pgettext("barbarians_building", "If you’re low on soldiers to occupy new military sites, use the downward arrow button to decrease the capacity. You can also click on a soldier to send him away.")
         }
      },
      --[[
      {
         name = "barbarians_barrier",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian military site: Barrier
            lore = pgettext("barbarians_building", "‘When we looked down to the valley from our newly established barrier, we felt that the spirit of our fathers was with us.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian military site: Barrier
            lore_author = pgettext("barbarians_building", "Ballad ‘The Battle of Kal’mavrath’ by Hakhor the Bard"),
            -- TRANSLATORS: Purpose helptext for a Barbarian military site: Barrier
            purpose = pgettext("barbarians_building", "Garrisons soldiers to expand your territory."),
            -- TRANSLATORS: Note helptext for a Barbarian military site: Barrier
            note = pgettext("barbarians_building", "If you’re low on soldiers to occupy new military sites, use the downward arrow button to decrease the capacity. You can also click on a soldier to send him away.")
         }
      },

      {
         name = "barbarians_tower",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian military site: Tower
            lore = pgettext("barbarians_building", "‘From the height of our tower we could see far into enemy territory. The enemy was well prepared, but we also noticed some weak points in their defense.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian military site: Tower
            lore_author = pgettext("barbarians_building", "Ballad ‘The Battle of Kal’mavrath’ by Hakhor the Bard"),
            -- TRANSLATORS: Purpose helptext for a Barbarian military site: Tower
            purpose = pgettext("barbarians_building", "Garrisons soldiers to expand your territory."),
            -- TRANSLATORS: Note helptext for a Barbarian military site: Tower
            note = pgettext("barbarians_building", "If you’re low on soldiers to occupy new military sites, use the downward arrow button to decrease the capacity. You can also click on a soldier to send him away.")
         }
      },
      {
         name = "barbarians_fortress",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian military site: Fortress
            lore = pgettext("barbarians_building", "‘This stronghold made from blackwood and stones will be a hard nut to crack for them.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian military site: Fortress
            lore_author = pgettext("barbarians_building", "Berthron,<br>chief military adviser of Chat’Karuth"),
            -- TRANSLATORS: Purpose helptext for a Barbarian military site: Fortress
            purpose = pgettext("barbarians_building", "Garrisons soldiers to expand your territory."),
            -- TRANSLATORS: Note helptext for a Barbarian military site: Fortress
            note = pgettext("barbarians_building", "If you’re low on soldiers to occupy new military sites, use the downward arrow button to decrease the capacity. You can also click on a soldier to send him away.")
         }
      },
      {
         name = "barbarians_citadel",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian military site: Citadel
            lore = pgettext("barbarians_building", "‘The Citadel of Adlen surely is the finest masterpiece of Barbarian craftsmanship. Nothing as strong and big and beautiful has ever been built in such a short time.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian military site: Citadel
            lore_author = pgettext("barbarians_building", "Colintan, chief planner of the Citadel of Adlen,<br>at its opening ceremony"),
            -- TRANSLATORS: Purpose helptext for a Barbarian military site: Citadel
            purpose = pgettext("barbarians_building", "Garrisons soldiers to expand your territory."),
            -- TRANSLATORS: Note helptext for a Barbarian military site: Citadel
            note = pgettext("barbarians_building", "If you’re low on soldiers to occupy new military sites, use the downward arrow button to decrease the capacity. You can also click on a soldier to send him away.")
         }
      },

      -- Seafaring/Ferry Sites - these are only displayed on seafaring/ferry maps
      {
         name = "barbarians_ferry_yard",
         helptexts = {
            -- TRANSLATORS: Purpose helptext for production site: Ferry Yard
            purpose = pgettext("building", "Builds ferries."),
            note = {
               -- TRANSLATORS: Note helptext for a Barbarian production site: Ferry Yard, part 1
               pgettext("building", "Needs water nearby. Be aware ferries carry wares only, no workers."),
               -- TRANSLATORS: Note helptext for a Barbarian production site: Ferry Yard, part 2
               pgettext("building", "Roads and trees along the shoreline block access to water."),
            }
         }
      },
      {
         name = "barbarians_shipyard",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian production site: Shipyard
            lore = pgettext("barbarians_building", [[‘When I saw the Saxnot for the first time, her majestic dragon head already looked up to the skies and the master was about to install the square sail.<br>] ] ..
                                          [[It was the most noble ship I ever saw.’] ]),
            -- TRANSLATORS: Lore author helptext for a Barbarian production site: Shipyard
            lore_author = pgettext("barbarians_building", "Captain Thanlas the Elder,<br>Explorer"),
            -- TRANSLATORS: Purpose helptext for a Barbarian production site: Shipyard
            purpose = pgettext("building", "Constructs ships that are used for overseas colonization and for trading between ports."),
            note = {
               -- TRANSLATORS: Note helptext for a Barbarian production site: Shipyard, part 1
               pgettext("building", "Needs wide open water nearby."),
               -- TRANSLATORS: Note helptext for a Barbarian production site: Shipyard, part 2
               pgettext("building", "Roads and trees along the shoreline block access to water."),
            }
         }
      },
      ]]--

      -- Partially Finished Buildings - these are the same 2 buildings for all tribes
      {
         name = "constructionsite",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian building: Construction Site
            lore = pgettext("building", "‘Don’t swear at the builder who is short of building materials.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian building: Construction Site
            lore_author = pgettext("building", "Proverb widely used for impossible tasks of any kind"),
            -- TRANSLATORS: Purpose helptext for a Barbarian building: Construction Site
            purpose = pgettext("building", "A new building is being built at this construction site.")
         }
      },
      {
         name = "dismantlesite",
         helptexts = {
            -- TRANSLATORS: Lore helptext for a Barbarian building: Dismantle Site
            lore = pgettext("building", "‘New paths will appear when you are willing to tear down the old.’"),
            -- TRANSLATORS: Lore author helptext for a Barbarian building: Dismantle Site
            lore_author = pgettext("building", "Proverb"),
            -- TRANSLATORS: Purpose helptext for a Barbarian building: Dismantle Site
            purpose = pgettext("building", "A building is being dismantled at this dismantle site, returning some of the resources that were used during this building’s construction to your tribe’s stores.")
         }
      },
   },

   warehouse_names = {
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Tel Aviv"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Jerusalem"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Ashdod"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Ashkelon"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Beit El"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Betlehem"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Gaza"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Nob"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Bersheva"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Carmel"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Jaffa"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Jericho"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Timnah"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Ziklag"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Tyre"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Sidon"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Susa"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Hebron"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Ekron"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Babylon"),
      -- TRANSLATORS: This Barbarian warehouse is inspired by a biblical town name.
      pgettext("warehousename", "Nimrod"),
   },

   -- Productionsite status strings

   -- TRANSLATORS: Productivity label on a Barbarian building if there is 1 worker missing
   productionsite_worker_missing = pgettext("barbarians", "Worker missing"),
   -- TRANSLATORS: Productivity label on a Barbarian building if there is 1 worker coming
   productionsite_worker_coming = pgettext("barbarians", "Worker is coming"),
   -- TRANSLATORS: Productivity label on a Barbarian building if there is more than 1 worker missing. If you need plural forms here, please let us know.
   productionsite_workers_missing = pgettext("barbarians", "Workers missing"),
   -- TRANSLATORS: Productivity label on a Barbarian building if there is more than 1 worker coming. If you need plural forms here, please let us know.
   productionsite_workers_coming = pgettext("barbarians", "Workers are coming"),
   -- TRANSLATORS: Productivity label on a Barbarian building if there is 1 experienced worker missing
   productionsite_experienced_worker_missing = pgettext("barbarians", "Expert missing"),
   -- TRANSLATORS: Productivity label on a Barbarian building if there is more than 1 experienced worker missing. If you need plural forms here, please let us know.
   productionsite_experienced_workers_missing = pgettext("barbarians", "Experts missing"),

   -- Soldier strings to be used in Military Status strings

   soldier_context = "barbarians_soldier",
   soldier_0_sg = "%1% soldier (+%2%)",
   soldier_0_pl = "%1% soldiers (+%2%)",
   soldier_1_sg = "%1% soldier",
   soldier_1_pl = "%1% soldiers",
   soldier_2_sg = "%1%(+%2%) soldier (+%3%)",
   soldier_2_pl = "%1%(+%2%) soldiers (+%3%)",
   soldier_3_sg = "%1%(+%2%) soldier",
   soldier_3_pl = "%1%(+%2%) soldiers",
   -- TRANSLATORS: %1% is the number of Barbarian soldiers the plural refers to. %2% is the maximum number of soldier slots in the building.
   UNUSED_soldier_0 = npgettext("barbarians_soldier", "%1% soldier (+%2%)", "%1% soldiers (+%2%)", 0),
   -- TRANSLATORS: Number of Barbarian soldiers stationed at a militarysite.
   UNUSED_soldier_1 = npgettext("barbarians_soldier", "%1% soldier", "%1% soldiers", 0),
   -- TRANSLATORS: %1% is the number of Barbarian soldiers the plural refers to. %2% are currently open soldier slots in the building. %3% is the maximum number of soldier slots in the building
   UNUSED_soldier_2 = npgettext("barbarians_soldier", "%1%(+%2%) soldier (+%3%)", "%1%(+%2%) soldiers (+%3%)", 0),
   -- TRANSLATORS: %1% is the number of Barbarian soldiers the plural refers to. %2% are currently open soldier slots in the building.
   UNUSED_soldier_3 = npgettext("barbarians_soldier", "%1%(+%2%) soldier", "%1%(+%2%) soldiers", 0),

   -- Special types
   builder = "hebrews_builder",
   carriers = {"hebrews_carrier", "hebrews_donkey"},
   geologist = "hebrews_geologist",
   scouts_house = "hebrews_scouts_hut",
   soldier = "barbarians_soldier",
   ship = "barbarians_ship",
   ferry = "hebrews_ferry",
   port = "hebrews_port",

   fastplace = {
      warehouse = "hebrews_warehouse",
      port = "hebrews_port",
      training_small = "hebrews_arena",
      training_large = "hebrews_trainingcamp",
      military_small_primary = "barbarians_sentry",
      military_medium_primary = "barbarians_barrier",
      military_tower = "barbarians_tower",
      military_fortress = "barbarians_fortress",
      woodcutter = "hebrews_lumberjacks_hut",
      quarry = "hebrews_quarry",
      building_materials_primary = "hebrews_claydigger",
      building_materials_secondary = "barbarians_lime_kiln",
      building_materials_tertiary = "barbarians_reed_yard",
      fisher = "hebrews_fishers_hut",
      well = "hebrews_well",
      farm_primary = "barbarians_farm",
      bakery = "barbarians_bakery",
      shipyard = "barbarians_shipyard",
      ferry_yard = "barbarians_ferry_yard",
      scout = "hebrews_scouts_hut",
      barracks = "barbarians_barracks",
      second_carrier = "barbarians_cattlefarm",
      charcoal = "barbarians_charcoal_kiln",
      mine_stone = "barbarians_granitemine",
      mine_coal = "barbarians_coalmine",
      mine_iron = "barbarians_ironmine",
      mine_gold = "barbarians_goldmine",
   },
}

pop_textdomain()
