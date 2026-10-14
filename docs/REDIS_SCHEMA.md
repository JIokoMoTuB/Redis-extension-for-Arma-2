# RedisExt Data Schema

This document outlines the Redis key-value structure, data types, and lifecycle policies utilized by RedisExt.

---

## Key Structure Overview

| Key Pattern | Redis Type | Description | Expiration (TTL) |
| :--- | :--- | :--- | :--- |
| `Global:NextPlayerID` | String (Integer) | Auto-incrementing sequence for player character IDs | Permanent |
| `Global:NextVehicleID` | String (Integer) | Auto-incrementing sequence for vehicle IDs | Permanent |
| `Global:NextObjectID` | String (Integer) | Auto-incrementing sequence for object IDs | Permanent |
| `Global:NextHouseID` | String (Integer) | Auto-incrementing sequence for house IDs | Permanent |
| `Global:Vehicles:<Instance>` | Set | Set of active Vehicle IDs for a given server instance | Permanent |
| `Global:Houses` | Set | Set of all active House IDs in the world | Permanent |
| `Player:<PlayerUID>` | String | Maps Player UID to their currently active Character ID | Permanent |
| `Character:<CharID>` | Hash | Stores full character state, stats, inventory, position | 7 days on death |
| `Vehicle:<VehID>` | Hash | Stores vehicle position, damage, fuel, and cargo inventory | Permanent |
| `Object:<ObjID>` | Hash | Stores placed base objects and deployables | Permanent |
| `House:<HouseID>` | Hash | Stores house class, owner, stage, inventory, doors | Permanent |
| `PlayerHouses:<PlayerUID>`| Set | Set of House IDs owned by a specific player UID | Permanent |
| `HouseCooldown:<HouseID>` | String | Upgrade cooldown lock flag | Automatic (`EX <sec>`) |
| `Nick:<PlayerUID>` | String | Exact player display name (for Web UI / search) | Permanent |
| `Ban:<PlayerUID>` | Hash | Stores ban reason, admin UID, and ban timestamp | Permanent |

---

## Hash Field Definitions

### `Character:<CharID>`
* `PlayerUID`: Unique player identifier (GUID / SteamID).
* `PlayerName`: Cleaned player display name.
* `Worldspace`: Arma 2 coordinate array string `[dir, [x, y, z]]`.
* `Inventory`: Array string containing weapons and magazines `[[wep1, ...], [mag1, ...]]`.
* `Backpack`: Array string containing backpack class and contents `["DZ_Backpack_EP1", [[...],[...]]]`.
* `Medical`: Array string representing health, blood, bleeding, unconscious states.
* `Survival`: Array string representing food, water, body temperature.
* `Model`: Player character model class (e.g., `Survivor2_DZ`, `Hero_DZ`).
* `Alive`: Integer flag (`1` = active/alive, `0` = dead).
* `Kills`: Array of bandit, human, and zombie kill counters.
* `Humanity`: Player humanity score.
* `TimeAlive`: Total accumulated minutes alive.

### `House:<HouseID>`
* `Class`: Class name of the building (e.g., `Uroven1DrevenaBudka`, `Origins_Fence_Wood`).
* `OwnerUID`: Player UID who founded the structure.
* `OwnerName`: Player name at the time of construction.
* `Stage`: Current construction stage index (1, 2, 3...).
* `Alive`: Active state (`1` = active in world, `0` = deleted).
* `Worldspace`: Coordinate array string `[dir, [x, y, z]]`.
* `Inventory`: Storage containers and gear saved inside the house.
* `OpenState`: Lock / door status (`0` = locked/closed, `1` = unlocked/open).
* `BuildTime`: Unix timestamp of creation.
* `DeletedBy`: Who initiated deletion (`owner`, `admin:<name>`, `system`).
* `DeletedAt`: Unix timestamp when marked deleted.

### `Vehicle:<VehID>`
* `Class`: Vehicle classname (e.g., `SUV_TK_CIV_EP1`, `PBX`).
* `Worldspace`: Coordinate array string `[dir, [x, y, z]]`.
* `Inventory`: Cargo weapons, magazines, and backpacks.
* `Hitpoints`: Component damage states (tires, engine, fuel tank).
* `Fuel`: Current fuel fraction (0.0 to 1.0).
* `Damage`: Overall vehicle hull damage (0.0 to 1.0).
* `Alive`: Active state (`1` = alive, `0` = destroyed).

### `Ban:<PlayerUID>`
* `Reason`: Reason string provided when ban was issued.
* `Banner`: UID of the administrator who enacted the ban.
* `Date`: Date / timestamp string when banned.
