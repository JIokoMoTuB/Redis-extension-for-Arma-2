# RedisExt API Reference

RedisExt is a high-performance 32-bit C++ extension for **Arma 2: Operation Arrowhead**, **DayZ**, and **Origins Mod 1.8.3**, designed as a modern, ultra-fast Redis database connector replacing slow legacy MySQL extensions.

---

## Extension Interface

All communications between Arma 2 SQF and the DLL use the native engine extension handler:

```sqf
_result = "RedisExt" callExtension "CHILD:<MethodID>:<Param1>:<Param2>:...:";
```

### Buffer Management & Automatic Chunking
Arma 2 imposes a 4096-byte limit on values returned by `callExtension`. For large queries (such as world vehicle lists or full house rosters), RedisExt implements an automatic chunking mechanism:

1. **Initial Response (exceeds buffer):**
   ```sqf
   ["CHUNK", "<SessionToken>", "<NextByteOffset>", "<PartialDataChunk>"]
   ```
2. **Successive Query:**
   ```sqf
   "RedisExt" callExtension "CHILD:FETCH:<SessionToken>:<NextByteOffset>:"
   ```
3. **Subsequent Response(s):**
   ```sqf
   ["CHUNK", "<SessionToken>", "<NewOffset>", "<PartialDataChunk>"]
   ```
4. **Final Response:**
   ```sqf
   ["FINAL", "<LastDataChunk>"]
   ```

---

## Method Directory

### Player & Character Management

#### `CHILD:101` — Player Login & Character Lookup
Looks up the player's active character record.
* **Syntax:** `CHILD:101:<PlayerUID>:<DayZInstance>:<PlayerName>:`
* **Returns:**
  * Existing character:
    ```sqf
    ["PASS", true, <charId>, <worldspace>, <inventory>, <backpack>, <survival>, <model>]
    ```
  * No active character found:
    ```sqf
    ["PASS", false]
    ```
  * Error:
    ```sqf
    ["ERROR", "<reason>"]
    ```

#### `CHILD:102` — Character Creation & First Spawn
Creates a brand-new character entry in Redis.
* **Syntax:** `CHILD:102:<PlayerUID>:<DayZInstance>:<PlayerName>:<ModelClass>:`
* **Returns:** `["PASS", <newCharacterId>]`

#### `CHILD:201` — Character State Synchronization
Periodically persists player character position, gear, health, and medical state.
* **Syntax:** `CHILD:201:<CharID>:<Worldspace>:<Inventory>:<Backpack>:<Medical>:<Stats>:<State>:<TimeAlive>:`
* **Returns:** `["PASS"]`

#### `CHILD:202` — Character Death
Marks character `Alive = 0`, records time of death, and assigns an automatic 7-day TTL (time-to-live) on the corpse before cleanup.
* **Syntax:** `CHILD:202:<CharID>:`
* **Returns:** `["PASS"]`

---

### Vehicles & World Objects

#### `CHILD:301` — Bulk Vehicle Load
Retrieves all active vehicles for the specified instance.
* **Syntax:** `CHILD:301:<DayZInstance>:`
* **Returns:**
  ```sqf
  ["PASS", [
      [<VehicleID>, "<Class>", <Damage>, [<Weapons>, <Magazines>], <Fuel>, <Worldspace>],
      ...
  ]]
  ```

#### `CHILD:302` — Vehicle State Save
Updates position, cargo inventory, component damage (hitpoints), fuel level, and overall hull damage.
* **Syntax:** `CHILD:302:<VehicleID>:<Worldspace>:<Inventory>:<Hitpoints>:<Fuel>:<Damage>:`
* **Returns:** `["PASS"]`

#### `CHILD:303` — Spawn / Create Vehicle
Registers a newly spawned vehicle in Redis.
* **Syntax:** `CHILD:303:<Class>:<Worldspace>:<Inventory>:<Hitpoints>:<Fuel>:<Damage>:<Instance>:`
* **Returns:** `["PASS", <newVehicleID>]`

#### `CHILD:304` — Object Update
Updates generic placed object attributes.
* **Syntax:** `CHILD:304:<ObjectID>:<Worldspace>:<Inventory>:`
* **Returns:** `["PASS"]`

#### `CHILD:305` — Object Sync
Full synchronization for world objects and deployed kits.
* **Syntax:** `CHILD:305:<ObjectID>:<Worldspace>:<Inventory>:<Damage>:`
* **Returns:** `["PASS"]`

#### `CHILD:306` — Destroy Vehicle
Marks vehicle as destroyed (`Alive = 0`) and unlinks it from the active world set.
* **Syntax:** `CHILD:306:<VehicleID>:`
* **Returns:** `["PASS"]`

---

### Ban System

#### `CHILD:401` — Ban System Interface
* **Check Status:**
  * **Syntax:** `CHILD:401:<UID>:check:`
  * **Returns:** `["PASS", [<isBanned>, "<reason>", "<bannerUID>", "<timestamp>"]]`
* **Add Ban:**
  * **Syntax:** `CHILD:401:<UID>:add:<reason>:<bannerUID>:`
  * **Returns:** `["PASS"]`

---

### Origins Mod House & Base Building

#### `CHILD:501` — House Creation (Stage 1 / Foundation)
Creates a new house/structure record with automated limit checks.
* **Syntax:** `CHILD:501:<class>:<worldspace>:<ownerUID>:<ownerName>:<buildTime>:`
* **Limits:**
  * Unlimited types (fences, watchtowers, gates, walls) can be placed without limit.
  * Standard houses (Uroven 1, 2, 3...) are limited to 1 active house per type per player.
  * Destroyed or deleted structures (`Alive = 0`) do not count towards the limit.
* **Returns:**
  * Success: `["PASS", "<houseID>"]`
  * Limit exceeded: `["ERROR", "LIMIT_EXCEEDED"]`

#### `CHILD:502` — Bulk House Loading
Loads all active structures into the game world upon server startup.
* **Syntax:** `CHILD:502:`
* **Returns:**
  ```sqf
  ["PASS", [
      ["<id>", "<class>", <stage>, <isAlive>, "<ownerUID>", "<ownerName>", <worldspace>, <inventory>, <buildTime>, <openState>],
      ...
  ]]
  ```

#### `CHILD:503` — House State & Upgrade Save
Saves construction stage, lock state, container inventory, and door states.
* **Syntax:** `CHILD:503:<houseID>:<stage>:<inventory>:<openState>:`
* **Returns:** `["PASS"]`

#### `CHILD:504` — Permanent House Deletion
Completely deletes a structure from `Global:Houses`, removes it from the player's set `PlayerHouses:<UID>`, and issues a `DEL` on `House:<id>`.
* **Syntax:** `CHILD:504:<houseID>:<deletedBy>:`
* **Returns:** `["PASS"]` or `["ERROR", "NOT_FOUND"]`

#### `CHILD:505` — Check House Ownership
Checks if a player has an active house of a specific class.
* **Syntax:** `CHILD:505:<ownerUID>:<class>:`
* **Returns:** `["PASS", "<houseID>"]` (if exists) or `["PASS", ""]`

#### `CHILD:506` — List Player Houses (Admin / Audit)
Returns all structures owned by a player UID.
* **Syntax:** `CHILD:506:<ownerUID>:`
* **Returns:**
  ```sqf
  ["PASS", [
      ["<id>", "<class>", <stage>, <alive>, "<deletedBy>", <deletedAt>, <worldspace>, <buildTime>],
      ...
  ]]
  ```

#### `CHILD:507` — Deprecated Restore Handler
Legacy restore method disabled in favor of permanent deletion.
* **Syntax:** `CHILD:507:...`
* **Returns:** `["ERROR", "RESTORE_REMOVED"]`

#### `CHILD:508` — Save Real Player Nickname
Stores an unedited player nickname in `Nick:<UID>` for web interfaces and search dashboards.
* **Syntax:** `CHILD:508:<UID>:<escaped_nick>:`
* **Special Character Encoding:**
  * `\` -> `\\`
  * `:` -> `\c`
  * `"` -> `\q`
  * LF  -> `\n`
  * CR  -> `\r`
* **Returns:** `["PASS"]`

#### `CHILD:509` — Retrieve Player Nickname
Fetches the player's saved nickname.
* **Syntax:** `CHILD:509:<UID>:`
* **Returns:** `["PASS", "<nickname>"]`

#### `CHILD:510` — Set House Construction Cooldown
Applies a temporary lock key (`HouseCooldown:<id>`) with Redis auto-expiration TTL (seconds).
* **Syntax:** `CHILD:510:<houseID>:<seconds>:`
* **Returns:** `["PASS"]`

---

### System & Database Management

#### `CHILD:999` — Switch Database Index
Dynamically switches the active Redis database (0–15).
* **Syntax:** `CHILD:999:<dbIndex>:`
* **Returns:** `["PASS"]`
