/*
    ============================================================================
    RedisExt SQF Usage Examples
    Arma 2 / DayZ / Origins Mod Redis Integration
    ============================================================================
    
    This file demonstrates how to interact with RedisExt.dll from Arma 2 SQF.
    All calls use the standard RVExtension entry point:
        "RedisExt" callExtension "<command>"
*/

// ============================================================================
// 1. Core CallExtension Wrapper with Auto-Chunking Support
// ----------------------------------------------------------------------------
// Arma 2 callExtension has a return buffer limit of 4096 bytes.
// RedisExt handles large payloads (e.g., 301 vehicle lists or 502 house lists)
// by returning chunked responses:
//   ["CHUNK", "<token>", "<offset>", "<partial_data>"]
//   ["FINAL", "<partial_data>"]
// ============================================================================

fnc_redisCall = {
    private ["_query", "_resultRaw", "_parsed", "_type"];
    _query = _this select 0;

    _resultRaw = "RedisExt" callExtension _query;
    if (_resultRaw == "") exitWith { ["ERROR", "EMPTY_EXTENSION_RESPONSE"] };

    _parsed = call compile _resultRaw;
    if (isNil "_parsed") exitWith { ["ERROR", "PARSE_FAILED", _resultRaw] };

    _type = _parsed select 0;

    // Handle large response chunking
    if (_type == "CHUNK") then {
        private ["_token", "_offset", "_accumulated", "_done", "_fetchQuery", "_nextRaw", "_nextParsed"];
        _token = _parsed select 1;
        _offset = _parsed select 2;
        _accumulated = _parsed select 3;
        _done = false;

        while {!_done} do {
            _fetchQuery = format ["CHILD:FETCH:%1:%2:", _token, _offset];
            _nextRaw = "RedisExt" callExtension _fetchQuery;
            _nextParsed = call compile _nextRaw;

            if (isNil "_nextParsed") exitWith {
                _accumulated = "";
                _done = true;
            };

            if ((_nextParsed select 0) == "CHUNK") then {
                _offset = _nextParsed select 2;
                _accumulated = _accumulated + (_nextParsed select 3);
            } else {
                if ((_nextParsed select 0) == "FINAL") then {
                    _accumulated = _accumulated + (_nextParsed select 1);
                };
                _done = true;
            };
        };

        _parsed = call compile _accumulated;
        if (isNil "_parsed") exitWith { ["ERROR", "CHUNK_PARSE_FAILED"] };
    };

    _parsed
};


// ============================================================================
// 2. Player Login & Character Check (CHILD:101)
// ----------------------------------------------------------------------------
// Syntax: CHILD:101:<PlayerUID>:<DayZInstance>:<PlayerName>:
// Returns:
//   ["PASS", true, <charId>, <worldspace>, <inventory>, <backpack>, <survival>, <model>]
//   ["PASS", false] (character not found / dead -> spawn new)
// ============================================================================

fnc_playerLogin = {
    private ["_playerUID", "_playerName", "_query", "_res"];
    _playerUID  = _this select 0;
    _playerName = _this select 1;

    _query = format ["CHILD:101:%1:1:%2:", _playerUID, _playerName];
    _res = [_query] call fnc_redisCall;

    if ((_res select 0) == "PASS") then {
        private ["_hasChar"];
        _hasChar = _res select 1;
        if (_hasChar) then {
            diag_log format ["[LOGIN] Existing character ID=%1 found for UID=%2", _res select 2, _playerUID];
        } else {
            diag_log format ["[LOGIN] No active character for UID=%2. Ready for creation.", _playerUID];
        };
    } else {
        diag_log format ["[LOGIN ERROR] %1", _res];
    };
    _res
};


// ============================================================================
// 3. Player Character Creation (CHILD:102)
// ----------------------------------------------------------------------------
// Syntax: CHILD:102:<PlayerUID>:<DayZInstance>:<PlayerName>:<ModelClass>:
// Returns: ["PASS", <newCharacterId>]
// ============================================================================

fnc_playerCreate = {
    private ["_playerUID", "_playerName", "_modelClass", "_query", "_res"];
    _playerUID  = _this select 0;
    _playerName = _this select 1;
    _modelClass = _this select 2;

    _query = format ["CHILD:102:%1:1:%2:%3:", _playerUID, _playerName, _modelClass];
    _res = [_query] call fnc_redisCall;

    if ((_res select 0) == "PASS") then {
        diag_log format ["[SPAWN] New character created: ID=%1", _res select 1];
    };
    _res
};


// ============================================================================
// 4. Character State Sync & Saving (CHILD:201)
// ----------------------------------------------------------------------------
// Syntax: CHILD:201:<CharID>:<Worldspace>:<Inventory>:<Backpack>:<Medical>:<Stats>:<State>:<TimeAlive>:
// Returns: ["PASS"]
// ============================================================================

fnc_characterSync = {
    private ["_charId", "_pos", "_inv", "_bp", "_med", "_query", "_res"];
    _charId = _this select 0;
    _pos    = str (_this select 1); // e.g. [dir, [x, y, z]]
    _inv    = str (_this select 2); // [weapons, magazines]
    _bp     = str (_this select 3); // [backpackType, [bpWeap, bpMag]]
    _med    = str [false, 12000, 0, 0, 0, 0]; // [isBleeding, blood, etc.]

    _query = format ["CHILD:201:%1:%2:%3:%4:%5:[0,0,0,0]:0:120:", _charId, _pos, _inv, _bp, _med];
    _res = [_query] call fnc_redisCall;
    _res
};


// ============================================================================
// 5. Character Death (CHILD:202)
// ----------------------------------------------------------------------------
// Syntax: CHILD:202:<CharID>:
// Marks character Alive=0 and sets corpse expiration TTL (7 days)
// Returns: ["PASS"]
// ============================================================================

fnc_characterKill = {
    private ["_charId", "_query", "_res"];
    _charId = _this select 0;

    _query = format ["CHILD:202:%1:", _charId];
    _res = [_query] call fnc_redisCall;
    diag_log format ["[DEATH] Character ID=%1 marked dead in Redis: %2", _charId, _res];
    _res
};


// ============================================================================
// 6. Bulk Vehicles Loading (CHILD:301)
// ----------------------------------------------------------------------------
// Syntax: CHILD:301:<DayZInstance>:
// Returns: ["PASS", [[id, class, damage, [weapons, magazines], fuel, worldspace], ...]]
// ============================================================================

fnc_loadAllVehicles = {
    private ["_query", "_res", "_vehList"];
    _query = "CHILD:301:1:";
    _res = [_query] call fnc_redisCall;

    if ((_res select 0) == "PASS") then {
        _vehList = _res select 1;
        diag_log format ["[VEHICLES] Loaded %1 vehicles from Redis", count _vehList];
        {
            private ["_id", "_class", "_damage", "_cargo", "_fuel", "_ws"];
            _id     = _x select 0;
            _class  = _x select 1;
            _damage = _x select 2;
            _cargo  = _x select 3;
            _fuel   = _x select 4;
            _ws     = _x select 5;
            // Spawn / restore vehicle in the world...
        } forEach _vehList;
    };
    _res
};


// ============================================================================
// 7. Save Vehicle State (CHILD:302)
// ----------------------------------------------------------------------------
// Syntax: CHILD:302:<VehicleID>:<Worldspace>:<Inventory>:<Hitpoints>:<Fuel>:<Damage>:
// Returns: ["PASS"]
// ============================================================================

fnc_saveVehicle = {
    private ["_vehId", "_ws", "_inv", "_hitpoints", "_fuel", "_damage", "_query", "_res"];
    _vehId     = _this select 0;
    _ws        = str (_this select 1);
    _inv       = str (_this select 2);
    _hitpoints = str (_this select 3);
    _fuel      = _this select 4;
    _damage    = _this select 5;

    _query = format ["CHILD:302:%1:%2:%3:%4:%5:%6:", _vehId, _ws, _inv, _hitpoints, _fuel, _damage];
    _res = [_query] call fnc_redisCall;
    _res
};


// ============================================================================
// 8. Ban System (CHILD:401)
// ----------------------------------------------------------------------------
// Syntax:
//   Check: CHILD:401:<UID>:check:
//   Add:   CHILD:401:<UID>:add:<reason>:<bannerUID>:
// ============================================================================

fnc_checkPlayerBan = {
    private ["_uid", "_query", "_res"];
    _uid = _this select 0;
    _query = format ["CHILD:401:%1:check:", _uid];
    _res = [_query] call fnc_redisCall;
    // Returns: ["PASS", [isBanned, reason, banner, date]]
    _res
};

fnc_banPlayer = {
    private ["_uid", "_reason", "_adminUID", "_query", "_res"];
    _uid      = _this select 0;
    _reason   = _this select 1;
    _adminUID = _this select 2;
    _query = format ["CHILD:401:%1:add:%2:%3:", _uid, _reason, _adminUID];
    _res = [_query] call fnc_redisCall;
    _res
};


// ============================================================================
// 9. Origins Building / House Creation (CHILD:501)
// ----------------------------------------------------------------------------
// Syntax: CHILD:501:<class>:<worldspace>:<ownerUID>:<ownerName>:<buildTime>:
// Returns:
//   ["PASS", "<houseId>"]
//   ["ERROR", "LIMIT_EXCEEDED"]
// ============================================================================

fnc_createHouse = {
    private ["_class", "_ws", "_ownerUID", "_ownerName", "_buildTime", "_query", "_res"];
    _class     = _this select 0;
    _ws        = str (_this select 1);
    _ownerUID  = _this select 2;
    _ownerName = _this select 3;
    _buildTime = round (time);

    _query = format ["CHILD:501:%1:%2:%3:%4:%5:", _class, _ws, _ownerUID, _ownerName, _buildTime];
    _res = [_query] call fnc_redisCall;

    if ((_res select 0) == "PASS") then {
        diag_log format ["[BUILD] Created house ID=%1 for UID=%2", _res select 1, _ownerUID];
    } else {
        diag_log format ["[BUILD ERROR] Cannot build: %1", _res];
    };
    _res
};


// ============================================================================
// 10. Permanent House Deletion (CHILD:504)
// ----------------------------------------------------------------------------
// Syntax: CHILD:504:<houseId>:<deletedBy>:
// Completely deletes house from Global:Houses, PlayerHouses, and Redis database.
// Returns: ["PASS"] / ["ERROR", "NOT_FOUND"]
// ============================================================================

fnc_deleteHouse = {
    private ["_houseId", "_deletedBy", "_query", "_res"];
    _houseId   = _this select 0;
    _deletedBy = _this select 1; // "owner" or "admin:nickname"

    _query = format ["CHILD:504:%1:%2:", _houseId, _deletedBy];
    _res = [_query] call fnc_redisCall;
    diag_log format ["[BUILD DELETE] House ID=%1 deleted by %2: %3", _houseId, _deletedBy, _res];
    _res
};


// ============================================================================
// 11. Player Real Nickname Management (CHILD:508 & CHILD:509)
// ----------------------------------------------------------------------------
// Saves unadulterated player nickname for Web panel lookup / stats.
// Special characters are escaped as:
//   '\' -> "\\"
//   ':' -> "\c"
//   '"' -> "\q"
// ============================================================================

fnc_escapeNick = {
    private ["_raw", "_arr", "_out"];
    _raw = _this select 0;
    _arr = toArray _raw;
    _out = "";
    {
        switch (_x) do {
            case 92: { _out = _out + "\\\\"; }; // Backslash
            case 58: { _out = _out + "\\c";  }; // Colon
            case 34: { _out = _out + "\\q";  }; // Quote
            case 10: { _out = _out + "\\n";  }; // LF
            case 13: { _out = _out + "\\r";  }; // CR
            default  { _out = _out + (toString [_x]); };
        };
    } forEach _arr;
    _out
};

fnc_savePlayerNick = {
    private ["_uid", "_nick", "_escaped", "_query", "_res"];
    _uid     = _this select 0;
    _nick    = _this select 1;
    _escaped = [_nick] call fnc_escapeNick;

    _query = format ["CHILD:508:%1:%2:", _uid, _escaped];
    _res = [_query] call fnc_redisCall;
    _res
};

fnc_getPlayerNick = {
    private ["_uid", "_query", "_res"];
    _uid = _this select 0;

    _query = format ["CHILD:509:%1:", _uid];
    _res = [_query] call fnc_redisCall;
    if ((_res select 0) == "PASS") then {
        _res select 1 // Returns string nickname
    } else {
        ""
    };
};


// ============================================================================
// 12. House Construction / Upgrade Cooldown (CHILD:510)
// ----------------------------------------------------------------------------
// Syntax: CHILD:510:<houseId>:<cooldownSeconds>:
// Sets temporary Redis key HouseCooldown:<id> with auto-expiration (TTL).
// While the key exists, upgrade attempts are rejected.
// ============================================================================

fnc_setHouseCooldown = {
    private ["_houseId", "_seconds", "_query", "_res"];
    _houseId = _this select 0;
    _seconds = _this select 1;

    _query = format ["CHILD:510:%1:%2:", _houseId, _seconds];
    _res = [_query] call fnc_redisCall;
    diag_log format ["[COOLDOWN] Set %1s cooldown on House ID=%2: %3", _seconds, _houseId, _res];
    _res
};


// ============================================================================
// 13. Dynamic Redis Database Selection (CHILD:999)
// ----------------------------------------------------------------------------
// Syntax: CHILD:999:<dbIndex>:
// Switches active Redis DB index (e.g. 0 for Chernarus, 1 for Taviana)
// ============================================================================

fnc_switchDatabase = {
    private ["_dbIndex", "_query", "_res"];
    _dbIndex = _this select 0;

    _query = format ["CHILD:999:%1:", _dbIndex];
    _res = [_query] call fnc_redisCall;
    diag_log format ["[DB SWITCH] Switched Redis database to DB %1: %2", _dbIndex, _res];
    _res
};
