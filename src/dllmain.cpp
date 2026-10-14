#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <wininet.h>
#include <hiredis/hiredis.h>

#pragma comment(lib, "wininet.lib")

#pragma comment(linker, "/EXPORT:RVExtension=_RVExtension@12")

using namespace std;

static HMODULE g_hModule = NULL;
static string g_dllDir = "";
static mutex g_redisMutex;

redisContext* redisConn = nullptr;
string redisIp = "127.0.0.1";
int redisPort = 6379;
string redisPass = "";
int redisDb = 0;
int logLevel = 1; // 0 = Отключен, 1 = Информационный (запросы/ответы/ошибки), 2 = Полный дебаг

// ================================================================
// НАСТРОЙКИ BROADCAST (мониторинг серверов)
// ----------------------------------------------------------------
// Объявлены ЗДЕСЬ, до LoadConfig(), потому что конфиг их заполняет.
// ================================================================
struct BroadcastConfig {
    bool     enabled      = false;   // ServerDevelopBroadCast = 0/1
    int      intervalMin  = 3;       // период в минутах
    string   apiUrl       = "https://originsmod.1sl.su/api_server_broadcast";
    int      requestPort  = 2302;    // порт, который сообщаем (порт запроса)
    string   buildTag     = "";      // маркер сборки (задаётся в .ini)
    int      timeoutSec   = 10;      // таймаут HTTP
};

static BroadcastConfig g_broadcast;
static HANDLE  g_broadcastThread = NULL;
static volatile bool g_broadcastStop = false;

// Буфер для передачи больших ответов (>4096 байт) с привязкой по UID / сессии
struct PendingPayload {
    string data;
    size_t offset = 0;
    time_t lastAccess = 0;
};

static unordered_map<string, PendingPayload> g_pendingBuffers;
static size_t g_logWriteCount = 0;

void InitDllPath(HMODULE hMod) {
    g_hModule = hMod;
    char path[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(hMod, path, MAX_PATH)) {
        string s = path;
        size_t pos = s.find_last_of("\\/");
        if (pos != string::npos) {
            g_dllDir = s.substr(0, pos + 1);
            return;
        }
    }
    g_dllDir = ".\\";
}

string GetConfigPath() {
    if (g_dllDir.empty()) InitDllPath(g_hModule);
    return g_dllDir + "RedisExt.ini";
}

string GetLogPath() {
    if (g_dllDir.empty()) InitDllPath(g_hModule);
    return g_dllDir + "RedisExt.log";
}

void WriteLog(const string& msg, int level = 1) {
    if (logLevel < level || logLevel <= 0) return;

    string logPath = GetLogPath();

    // Ротация лога каждые 500 записей: если файл превышает 10 МБ, ротируем в .old
    if (++g_logWriteCount % 500 == 0) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(logPath.c_str(), GetFileExInfoStandard, &fad)) {
            LARGE_INTEGER size;
            size.LowPart = fad.nFileSizeLow;
            size.HighPart = fad.nFileSizeHigh;
            if (size.QuadPart > 10 * 1024 * 1024) {
                string oldLog = logPath + ".old";
                DeleteFileA(oldLog.c_str());
                MoveFileA(logPath.c_str(), oldLog.c_str());
            }
        }
    }

    ofstream logFile(logPath, ios_base::app);
    if (logFile.is_open()) {
        time_t now = time(0);
        char dt[30] = { 0 };
        ctime_s(dt, sizeof(dt), &now);
        string dts = dt;
        if (!dts.empty()) dts.pop_back();
        logFile << "[" << dts << "] " << msg << "\n";
    }
}

vector<string> split(const string& s, char delimiter) {
    vector<string> tokens;
    string token;
    istringstream tokenStream(s);
    while (getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// ================================================================
// ЧТЕНИЕ ТОЛЬКО ПАРАМЕТРОВ REDIS
// ----------------------------------------------------------------
// Используется при переподключении. Не трогает настройки Broadcast,
// чтобы фоновый поток не получил изменённые «на ходу» значения.
// ================================================================
void LoadRedisConfigOnly() {
    string ini = GetConfigPath();
    char ipBuf[256] = { 0 };
    char passBuf[256] = { 0 };

    GetPrivateProfileStringA("Redis", "IP", "127.0.0.1", ipBuf, sizeof(ipBuf), ini.c_str());
    redisPort = GetPrivateProfileIntA("Redis", "Port", 6379, ini.c_str());
    GetPrivateProfileStringA("Redis", "Password", "", passBuf, sizeof(passBuf), ini.c_str());
    redisDb   = GetPrivateProfileIntA("Redis", "DB", 0, ini.c_str());
    logLevel  = GetPrivateProfileIntA("General", "LogLevel", 1, ini.c_str());

    redisIp = ipBuf;
    redisPass = passBuf;
}

void LoadConfig() {
    string ini = GetConfigPath();
    char ipBuf[256] = { 0 };
    char passBuf[256] = { 0 };

    GetPrivateProfileStringA("Redis", "IP", "127.0.0.1", ipBuf, sizeof(ipBuf), ini.c_str());
    redisPort = GetPrivateProfileIntA("Redis", "Port", 6379, ini.c_str());
    GetPrivateProfileStringA("Redis", "Password", "", passBuf, sizeof(passBuf), ini.c_str());
    redisDb   = GetPrivateProfileIntA("Redis", "DB", 0, ini.c_str());
    logLevel  = GetPrivateProfileIntA("General", "LogLevel", 1, ini.c_str());

    redisIp = ipBuf;
    redisPass = passBuf;

    // ============================================================
    // BROADCAST (мониторинг серверов) — секция [Broadcast]
    // ------------------------------------------------------------
    // ServerDevelopBroadCast = 1  включить отправку
    //                          0  выключить (по умолчанию)
    // IntervalMinutes        период отправки в минутах (по умолч. 3)
    // ApiUrl                 адрес API мониторинга
    // GamePort               порт игрового сервера
    // BuildTag               маркер сборки (задаётся в .ini)
    // TimeoutSeconds         таймаут HTTP-запроса
    // ============================================================
    char urlBuf[512] = { 0 };
    char tagBuf[128] = { 0 };

    g_broadcast.enabled     = GetPrivateProfileIntA("Broadcast", "ServerDevelopBroadCast", 0, ini.c_str()) != 0;
    g_broadcast.intervalMin = GetPrivateProfileIntA("Broadcast", "IntervalMinutes", 3, ini.c_str());
    g_broadcast.requestPort = GetPrivateProfileIntA("Broadcast", "GamePort", 2302, ini.c_str());
    g_broadcast.timeoutSec  = GetPrivateProfileIntA("Broadcast", "TimeoutSeconds", 10, ini.c_str());

    GetPrivateProfileStringA("Broadcast", "ApiUrl",
        "https://originsmod.1sl.su/api_server_broadcast",
        urlBuf, sizeof(urlBuf), ini.c_str());
    GetPrivateProfileStringA("Broadcast", "BuildTag", "", tagBuf, sizeof(tagBuf), ini.c_str());

    g_broadcast.apiUrl   = urlBuf;
    g_broadcast.buildTag = tagBuf;

    // Защита от некорректных значений
    if (g_broadcast.intervalMin < 1)   g_broadcast.intervalMin = 1;
    if (g_broadcast.intervalMin > 60)  g_broadcast.intervalMin = 60;
    if (g_broadcast.timeoutSec < 2)    g_broadcast.timeoutSec = 2;
    if (g_broadcast.timeoutSec > 60)   g_broadcast.timeoutSec = 60;
}

void EnsureConnection() {
    if (redisConn == nullptr || redisConn->err) {
        if (redisConn) {
            WriteLog("Connection lost. Reconnecting...", 1);
            redisFree(redisConn);
            redisConn = nullptr;
        }

        // ============================================================
        // ВАЖНО: перечитываем ТОЛЬКО параметры Redis.
        // ------------------------------------------------------------
        // БАГ (исправлено 08.10.2026): здесь вызывался общий
        // LoadConfig(), который заново заполнял и настройки
        // Broadcast. Поскольку EnsureConnection выполняется при
        // каждом переподключении, поток broadcast мог получить
        // изменённые «на ходу» значения (например, пустой URL
        // или нулевой интервал) — это вело к непредсказуемому
        // поведению и лишним сетевым операциям.
        //
        // Теперь настройки Broadcast читаются ОДИН РАЗ при
        // загрузке DLL (в DllMain) и больше не меняются.
        // ============================================================
        LoadRedisConfigOnly();

        // Таймаут подключения 500 мс (не вешает сервер при недоступности БД)
        struct timeval timeout = { 0, 500000 };
        redisConn = redisConnectWithTimeout(redisIp.c_str(), redisPort, timeout);

        if (redisConn != nullptr && !redisConn->err) {
            WriteLog("Connected to Redis at " + redisIp + ":" + to_string(redisPort), 1);

            // Авторизация паролем при наличии
            if (!redisPass.empty()) {
                redisReply* reply = (redisReply*)redisCommand(redisConn, "AUTH %s", redisPass.c_str());
                if (reply) freeReplyObject(reply);
            }

            // Выбор базы данных (SELECT 0, 1, 2...)
            if (redisDb > 0) {
                redisReply* reply = (redisReply*)redisCommand(redisConn, "SELECT %d", redisDb);
                if (reply) freeReplyObject(reply);
                WriteLog("Selected Redis Database: " + to_string(redisDb), 1);
            }
        } else {
            WriteLog("CRITICAL: Failed to connect to Redis at " + redisIp + ":" + to_string(redisPort), 1);
        }
    }
}

// ================================================================
// МОДУЛЬ BROADCAST (мониторинг серверов)
// ----------------------------------------------------------------
// Назначение: раз в N минут отправлять на ваш API сведения о
// поднятом сервере (внешний IP, порт игры, порт запроса), чтобы
// сервер автоматически попадал в мониторинг.
//
// ТРЕБОВАНИЯ К РЕАЛИЗАЦИИ:
//   1. НЕ блокировать основную работу сервера — вся работа в
//      отдельном потоке, RVExtension его не ждёт.
//   2. При недоступности API — молча игнорировать и повторить
//      на следующей итерации.
//   3. Администратор может включить/выключить через .ini.
//   4. Подтверждать, что это именно сборка под OriginsMod 1.8.3.
//
// Настройки и поток объявлены выше (перед LoadConfig).
// ================================================================

// --- Вспомогательное: получить внешний IP -----------------------
// Используется api.ipify.org (простой текстовый ответ).
// Если недоступен — возвращаем пустую строку, отправка пропускается.
static string GetExternalIP(int timeoutSec) {
    HINTERNET hNet = InternetOpenA("OriginsMod-Server/1.8.3",
                                   INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return "";

    DWORD timeout = (DWORD)timeoutSec * 1000;
    InternetSetOptionA(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    string result;
    HINTERNET hUrl = InternetOpenUrlA(hNet, "https://api.ipify.org",
                                      NULL, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
                                      INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (hUrl) {
        char buf[128] = { 0 };
        DWORD read = 0;
        if (InternetReadFile(hUrl, buf, sizeof(buf) - 1, &read) && read > 0) {
            buf[read] = 0;
            result = buf;
            // Убираем пробелы/переводы строк
            while (!result.empty() &&
                   (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
                result.pop_back();
            }
        }
        InternetCloseHandle(hUrl);
    }
    InternetCloseHandle(hNet);
    return result;
}

// --- Вспомогательное: POST JSON ---------------------------------
// Возвращает true при HTTP 2xx. При любой ошибке — false (тихо).
static bool HttpPostJson(const string& url, const string& json, int timeoutSec) {
    // Разбор URL: https://host/path
    bool isHttps = (url.rfind("https://", 0) == 0);
    string rest = isHttps ? url.substr(8) : (url.rfind("http://", 0) == 0 ? url.substr(7) : url);

    size_t slash = rest.find('/');
    string host = (slash == string::npos) ? rest : rest.substr(0, slash);
    string path = (slash == string::npos) ? "/" : rest.substr(slash);

    if (host.empty()) return false;

    HINTERNET hNet = InternetOpenA("OriginsMod-Server/1.8.3",
                                   INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return false;

    DWORD timeout = (DWORD)timeoutSec * 1000;
    InternetSetOptionA(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hNet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (isHttps) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hConn = InternetConnectA(hNet, host.c_str(),
                                       isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
                                       NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) { InternetCloseHandle(hNet); return false; }

    const char* acceptTypes[] = { "application/json", NULL };
    HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path.c_str(),
                                      NULL, NULL, acceptTypes, flags, 0);
    if (!hReq) { InternetCloseHandle(hConn); InternetCloseHandle(hNet); return false; }

    string headers = "Content-Type: application/json\r\n";
    BOOL ok = HttpSendRequestA(hReq, headers.c_str(), (DWORD)headers.size(),
                               (LPVOID)json.c_str(), (DWORD)json.size());

    bool success = false;
    if (ok) {
        DWORD status = 0;
        DWORD len = sizeof(status);
        if (HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &status, &len, NULL)) {
            success = (status >= 200 && status < 300);
        }
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hNet);
    return success;
}

// --- Экранирование строки для JSON ------------------------------
static string JsonEscape(const string& s) {
    string r;
    r.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:   r += c;      break;
        }
    }
    return r;
}

// --- Получение имени ПК (идентификатор сервера) -----------------
static string GetServerHostName() {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD size = sizeof(name);
    if (GetComputerNameA(name, &size)) return string(name);
    return "unknown";
}

// --- Сборка JSON для отправки -----------------------------------
static string BuildBroadcastJson(const string& externalIp) {
    // Маркер сборки: подтверждает, что это именная серверная часть
    // OriginsMod 1.8.3, а не случайная Arma с этой DLL.
    //
    // Формируется из:
    //   - buildTag из .ini (задаётся в конфигурационном файле)
    //   - уникального идентификатора сборки
    //   - версии протокола
    //
    // ВАЖНО: это НЕ защита от подделки, а способ отсеять чужие
    // серверы, где DLL просто скопировали без правки .ini.
    string buildId = g_broadcast.buildTag.empty()
                   ? string("UNCONFIGURED")
                   : g_broadcast.buildTag;

    string json = "{";
    json += "\"external_ip\":\"" + JsonEscape(externalIp) + "\",";
    json += "\"game_port\":" + to_string(g_broadcast.requestPort) + ",";
    json += "\"query_port\":" + to_string(g_broadcast.requestPort + 1) + ",";
    json += "\"hostname\":\"" + JsonEscape(GetServerHostName()) + "\",";
    json += "\"build_id\":\"" + JsonEscape(buildId) + "\",";
    json += "\"mod\":\"OriginsMod\",";
    json += "\"mod_version\":\"1.8.3\",";
    json += "\"dll_version\":\"2.2\",";
    json += "\"protocol\":1";
    json += "}";
    return json;
}

// --- Рабочий поток broadcast ------------------------------------
// ВАЖНО: поток НЕ трогает Redis и НЕ берёт g_redisMutex, поэтому
// не может замедлить основную работу сервера.
static DWORD WINAPI BroadcastThreadProc(LPVOID) {
    // ============================================================
    // ВАЖНО: поток НЕ должен трогать ничего, что требует loader lock
    // или g_redisMutex. Только сеть + WriteLog.
    //
    // Первая запись лога делается ПОСЛЕ короткой паузы: в момент
    // старта DLL лог-файл может быть ещё занят инициализацией.
    // ============================================================

    // Пауза 60 секунд перед первой отправкой: сервер должен подняться,
    // а внешний IP и порт стать реальными. Проверяем флаг каждую секунду.
    for (int i = 0; i < 60 && !g_broadcastStop; ++i) Sleep(1000);
    if (g_broadcastStop) return 0;

    WriteLog("=== [BROADCAST] Thread started, interval=" +
             to_string(g_broadcast.intervalMin) + " min, url=" + g_broadcast.apiUrl, 1);

    while (!g_broadcastStop) {
        // Защита: если поток попал сюда с некорректным интервалом
        // (например, .ini переписали на ходу) — не уходим в 0-секундный
        // цикл, который загрузил бы процессор на 100%.
        int interval = g_broadcast.intervalMin;
        if (interval < 1)  interval = 1;
        if (interval > 60) interval = 60;

        string extIp = GetExternalIP(g_broadcast.timeoutSec);

        if (extIp.empty()) {
            // Нет интернета или сервис недоступен — тихо ждём следующий цикл
            WriteLog("=== [BROADCAST] Skip: external IP unavailable ===", 2);
        } else {
            string json = BuildBroadcastJson(extIp);
            bool ok = HttpPostJson(g_broadcast.apiUrl, json, g_broadcast.timeoutSec);

            if (ok) {
                WriteLog("=== [BROADCAST] Sent OK: " + json, 1);
            } else {
                // API недоступно — игнорируем, повторим на следующей итерации
                WriteLog("=== [BROADCAST] Send failed (API unavailable), will retry ===", 2);
            }
        }

        // Ждём интервал, проверяя флаг остановки каждую секунду
        int totalSec = interval * 60;
        for (int i = 0; i < totalSec && !g_broadcastStop; ++i) Sleep(1000);
    }

    WriteLog("=== [BROADCAST] Thread stopped ===", 2);
    return 0;
}

// --- Запуск/остановка broadcast ---------------------------------
static void StartBroadcast() {
    if (!g_broadcast.enabled) {
        WriteLog("=== [BROADCAST] Disabled in config (ServerDevelopBroadCast=0) ===", 1);
        return;
    }
    if (g_broadcastThread != NULL) return;

    if (g_broadcast.apiUrl.empty()) {
        WriteLog("=== [BROADCAST] No API URL configured, skipping ===", 1);
        return;
    }

    g_broadcastStop = false;
    g_broadcastThread = CreateThread(NULL, 0, BroadcastThreadProc, NULL, 0, NULL);
    if (g_broadcastThread == NULL) {
        WriteLog("=== [BROADCAST] CRITICAL: failed to create thread ===", 1);
    }
}

// ================================================================
// ОСТАНОВКА BROADCAST
// ----------------------------------------------------------------
// ВАЖНО: НЕ вызывать из DLL_PROCESS_DETACH с ожиданием!
// В DETACH запрещено ждать другие потоки — это дедлок.
// Здесь только выставляем флаг; используется при необходимости
// штатной остановки ВНЕ DllMain.
// ================================================================
static void StopBroadcast() {
    g_broadcastStop = true;
    // Не ждём: поток сам завершится, увидев флаг.
}

// Экранирование кавычек для безопасного call compile в SQF
string EscapeSQF(const string& s) {    string res;
    res.reserve(s.size() + 64);
    for (char c : s) {
        if (c == '"') res += "\"\"";
        else res += c;
    }
    return res;
}

// Безопасная отправка ответа с авто-чанкингом (>4096 байт) с привязкой по UID/ключу
void SendPayload(char* output, int outputSize, const string& payload, const string& sessionKey = "default") {
    string key = sessionKey.empty() ? "default" : sessionKey;

    if (payload.size() < (size_t)(outputSize - 16)) {
        g_pendingBuffers.erase(key);
        strncpy_s(output, outputSize, payload.c_str(), _TRUNCATE);
        return;
    }

    // Очистка устаревших буферов (>60 секунд без обращения)
    time_t now = time(nullptr);
    if (g_pendingBuffers.size() > 50) {
        for (auto it = g_pendingBuffers.begin(); it != g_pendingBuffers.end(); ) {
            if (now - it->second.lastAccess > 60) {
                it = g_pendingBuffers.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Если ответ больше размера буфера — включаем чанкинг
    PendingPayload pp;
    pp.data = payload;
    pp.offset = 0;
    pp.lastAccess = now;

    size_t chunkSize = 1800; // Безопасный размер порции с учетом экранирования
    string chunk = pp.data.substr(0, chunkSize);
    pp.offset = chunkSize;

    g_pendingBuffers[key] = pp;

    string escaped = EscapeSQF(chunk);
    snprintf(output, outputSize, "[\"CHUNK\",\"%s\",\"%s\"]", key.c_str(), escaped.c_str());
    WriteLog("Payload chunking started for [" + key + "]: total=" + to_string(payload.size()) + " first_chunk=" + to_string(chunkSize), 2);
}

// ================================================================
// ОТЛОЖЕННАЯ ИНИЦИАЛИЗАЦИЯ
// ----------------------------------------------------------------
// КРИТИЧНО: в DllMain запрещены файловые операции, создание потоков
// и любые блокирующие вызовы — всё это внутри LOADER LOCK и может
// повесить или уронить процесс.
//
// БАГ (исправлено 08.10.2026): раньше DllMain делал LoadConfig()
// (чтение .ini), WriteLog() (запись файла) и StartBroadcast()
// (CreateThread). Из-за этого Arma2-сервер высыпал с ошибкой,
// миссия не загружалась (RPT обрывался на "Connected to Steam
// servers"), а RedisExt.log оставался пустым (0 байт).
//
// Теперь вся работа выполняется ОДИН РАЗ при первом вызове
// RVExtension — он идёт уже ВНЕ loader lock.
// ================================================================
static bool g_initDone = false;

static void EnsureInitialized() {
    if (g_initDone) return;
    g_initDone = true;

    LoadConfig();
    WriteLog("=== RedisExt DLL Initialized (v2.3 ThreadSafe, SafeChunking, Broadcast) ===", 1);
    WriteLog("Base Path: " + g_dllDir, 1);
    WriteLog("Config: IP=" + redisIp + " Port=" + to_string(redisPort) +
             " DB=" + to_string(redisDb) + " LogLevel=" + to_string(logLevel), 1);
    WriteLog("Broadcast: enabled=" + string(g_broadcast.enabled ? "1" : "0") +
             " interval=" + to_string(g_broadcast.intervalMin) + "min", 1);

    StartBroadcast();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        // Минимум работы: только запоминаем путь к папке DLL.
        // НЕ читаем файлы, НЕ пишем лог, НЕ создаём потоки —
        // всё это в EnsureInitialized(), вызываемой из RVExtension.
        DisableThreadLibraryCalls(hModule);
        InitDllPath(hModule);
        g_initDone = false;
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // ============================================================
        // В DETACH ЗАПРЕЩЕНО ждать другие потоки (дедлок при выходе).
        // Только выставляем флаг — поток завершится сам,
        // проверяя его раз в секунду.
        // ============================================================
        g_broadcastStop = true;
    }
    return TRUE;
}

extern "C" {
    __declspec(dllexport) void __stdcall RVExtension(char *output, int outputSize, const char *function) {
        lock_guard<mutex> lock(g_redisMutex);

        try {
            // Отложенная инициализация: конфиг, лог, поток broadcast.
            // Здесь уже НЕТ loader lock, поэтому файловые операции
            // и создание потока безопасны.
            EnsureInitialized();

            EnsureConnection();

            if (redisConn == nullptr || redisConn->err) {
                strncpy_s(output, outputSize, "[\"ERROR\", \"DB_OFFLINE\"]", _TRUNCATE);
                WriteLog("CRITICAL: Redis connection is offline, request rejected", 1);
                return;
            }

            string input(function ? function : "");
            if (input.size() < 5 || input.substr(0, 5) != "CHILD") {
                strncpy_s(output, outputSize, "[\"ERROR\", \"INVALID_FORMAT\"]", _TRUNCATE);
                WriteLog("Invalid request format: " + input, 1);
                return;
            }

            WriteLog("SQF Request: " + input, 2);

            vector<string> args = split(input, ':');
            if (args.size() < 2) return;

            string method = args[1];

            // ------------------------------------------------------------
            // FETCH: Получение следующей порции большого ответа (>4096 байт)
            // Синтаксис: CHILD:FETCH:<sessionKey>: или CHILD:FETCH:
            // ------------------------------------------------------------
            if (method == "FETCH") {
                string key = (args.size() >= 3 && !args[2].empty()) ? args[2] : "";
                auto it = g_pendingBuffers.end();
                if (!key.empty()) {
                    it = g_pendingBuffers.find(key);
                }

                if (it == g_pendingBuffers.end() || it->second.offset >= it->second.data.size()) {
                    if (it != g_pendingBuffers.end()) {
                        g_pendingBuffers.erase(it);
                    }
                    strncpy_s(output, outputSize, "[\"FINAL\",\"\"]", _TRUNCATE);
                    return;
                }

                PendingPayload& pp = it->second;
                pp.lastAccess = time(nullptr);

                size_t chunkSize = 1800;
                size_t remaining = pp.data.size() - pp.offset;
                bool isLast = (remaining <= chunkSize);
                size_t curSize = isLast ? remaining : chunkSize;

                string chunk = pp.data.substr(pp.offset, curSize);
                pp.offset += curSize;

                string escaped = EscapeSQF(chunk);
                const char* tag = isLast ? "FINAL" : "CHUNK";
                snprintf(output, outputSize, "[\"%s\",\"%s\"]", tag, escaped.c_str());

                WriteLog(string("FETCH [") + it->first + "] " + tag + " offset=" + to_string(pp.offset) + " remaining=" + to_string(isLast ? 0 : remaining - curSize), 2);

                if (isLast) {
                    g_pendingBuffers.erase(it);
                }
                return;
            }

            // ------------------------------------------------------------
            // 999: Переключение базы данных на лету (CHILD:999:<db>)
            // ------------------------------------------------------------
            if (method == "999" && args.size() >= 3) {
                int newDb = 0;
                try {
                    newDb = std::stoi(args[2]);
                } catch (const std::exception& e) {
                    WriteLog(string("ERROR: Invalid DB number in CHILD:999: '") + args[2] + "' (" + e.what() + ")", 1);
                    strncpy_s(output, outputSize, "[\"ERROR\",\"INVALID_DB\"]", _TRUNCATE);
                    return;
                }

                redisReply* rep = (redisReply*)redisCommand(redisConn, "SELECT %d", newDb);
                if (rep) freeReplyObject(rep);
                redisDb = newDb;
                WriteLog("Switched DB to: " + to_string(newDb), 1);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
                return;
            }

            // ------------------------------------------------------------
            // 101: ЛОГИН / ЗАГРУЗКА ПЕРСОНАЖА
            // ------------------------------------------------------------
            if (method == "101" && args.size() >= 3) {
                string uid = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn, "HGET Player:%s ActiveCharID", uid.c_str());
                string activeChar = (rep && rep->type == REDIS_REPLY_STRING) ? rep->str : "";
                if (rep) freeReplyObject(rep);

                bool isNew = true;
                if (!activeChar.empty()) {
                    rep = (redisReply*)redisCommand(redisConn, "HGET Character:%s Alive", activeChar.c_str());
                    if (rep && rep->type == REDIS_REPLY_STRING && string(rep->str) == "1") {
                        isNew = false;
                    }
                    if (rep) freeReplyObject(rep);
                }

                if (isNew) {
                    rep = (redisReply*)redisCommand(redisConn, "INCR Global:NextCharID");
                    if (rep && rep->type == REDIS_REPLY_INTEGER) {
                        activeChar = to_string(rep->integer);
                    }
                    if (rep) freeReplyObject(rep);

                    redisReply* r1 = (redisReply*)redisCommand(redisConn,
                        "HMSET Character:%s UID %s Alive 1 "
                        "Worldspace [] Inventory [] Backpack [] "
                        "Medical [false,false,false,false,false,false,false,12000,[],[0,0],37,[false,false]] "
                        "Model Survivor2_DZ Humanity 2500 Kills [0,0,0,0,0]",
                        activeChar.c_str(), uid.c_str());
                    if (r1) freeReplyObject(r1);

                    redisReply* r2 = (redisReply*)redisCommand(redisConn,
                        "HSET Player:%s ActiveCharID %s",
                        uid.c_str(), activeChar.c_str());
                    if (r2) freeReplyObject(r2);

                    string res = "[\"PASS\",true,\"" + activeChar + "\"]";
                    WriteLog("Created NEW Character: " + activeChar + " for UID: " + uid, 1);
                    SendPayload(output, outputSize, res, uid);
                } else {
                    rep = (redisReply*)redisCommand(redisConn,
                        "HMGET Character:%s Worldspace Inventory Backpack Medical Humanity Model CurrentWeapon Kills",
                        activeChar.c_str());

                    if (rep && rep->type == REDIS_REPLY_ARRAY && rep->elements >= 6) {
                        string ws    = (rep->element[0]->type == REDIS_REPLY_STRING) ? rep->element[0]->str : "[]";
                        string inv   = (rep->element[1]->type == REDIS_REPLY_STRING) ? rep->element[1]->str : "[]";
                        string bp    = (rep->element[2]->type == REDIS_REPLY_STRING) ? rep->element[2]->str : "[]";
                        string med   = (rep->element[3]->type == REDIS_REPLY_STRING) ? rep->element[3]->str : "[]";
                        string hum   = (rep->element[4]->type == REDIS_REPLY_STRING) ? rep->element[4]->str : "2500";
                        string model = (rep->element[5]->type == REDIS_REPLY_STRING) ? rep->element[5]->str : "Survivor2_DZ";

                        // 7-й элемент: оружие в руках.
                        // Если поля в Redis ещё нет (старые персонажи) — пустая строка.
                        string cw = "";
                        if (rep->elements > 6 && rep->element[6]->type == REDIS_REPLY_STRING) {
                            cw = rep->element[6]->str;
                        }

                        // 8-й элемент: статистика убийств [zombieKills, banditKills, headShots, murders, botKills]
                        string kills = "[0,0,0,0,0]";
                        if (rep->elements > 7 && rep->element[7]->type == REDIS_REPLY_STRING && rep->element[7]->len > 0) {
                            string k = rep->element[7]->str;
                            if (!k.empty() && k[0] == '[') {
                                kills = k;
                            }
                        }

                        string fullData = "[\"PASS\",false,\"" + activeChar + "\"," +
                                          ws + "," + inv + "," + bp + "," +
                                          med + "," + hum + ",\"" + model + "\",\"" + cw + "\"," + kills + "]";

                        WriteLog("Loaded Character: " + activeChar + " (UID: " + uid + ") Model: " + model +
                                 " Hum: " + hum + " CW: " + cw + " Kills: " + kills, 1);
                        SendPayload(output, outputSize, fullData, uid);
                    } else {
                        WriteLog("CRITICAL: Error loading Character data from Redis for CID: " + activeChar, 1);
                        strncpy_s(output, outputSize, "[\"ERROR\",\"DATA_CORRUPTED\"]", _TRUNCATE);
                    }
                    if (rep) freeReplyObject(rep);
                }
            }
            // ------------------------------------------------------------
            // 201: СОХРАНЕНИЕ ПЕРСОНАЖА
            // ------------------------------------------------------------
            else if (method == "201") {
                if (args.size() < 8) {
                    WriteLog("ERROR: CHILD:201 invalid token count (" + to_string(args.size()) + " expected >= 8): " + input, 1);
                    strncpy_s(output, outputSize, "[\"ERROR\",\"INVALID_FORMAT\"]", _TRUNCATE);
                    return;
                }

                string charId = args[2];
                string ws     = (args.size() > 3 && !args[3].empty()) ? args[3] : "[]";
                string inv    = (args.size() > 4 && !args[4].empty()) ? args[4] : "[]";
                string bp     = (args.size() > 5 && !args[5].empty()) ? args[5] : "[\"\",[],[]]";
                string med    = (args.size() > 6 && !args[6].empty()) ? args[6] : "[]";
                string model  = (args.size() > 7 && !args[7].empty()) ? args[7] : "Survivor2_DZ";
                string hum    = (args.size() > 8 && !args[8].empty()) ? args[8] : "2500";

                // 9-й токен: оружие в руках (currentWeapon)
                string cw     = (args.size() > 9 && !args[9].empty()) ? args[9] : "";

                // 10-й токен: статистика убийств [zombieKills, banditKills, headShots, murders, botKills]
                string kills  = (args.size() > 10 && !args[10].empty()) ? args[10] : "[0,0,0,0,0]";
                if (kills.empty() || kills[0] != '[') kills = "[0,0,0,0,0]";

                if (model.empty()) model = "Survivor2_DZ";
                if (hum.empty()) hum = "2500";

                redisReply* rep = (redisReply*)redisCommand(redisConn,
                    "HMSET Character:%s Worldspace %s Inventory %s Backpack %s "
                    "Medical %s Model %s Humanity %s CurrentWeapon %s Kills %s",
                    charId.c_str(), ws.c_str(), inv.c_str(), bp.c_str(),
                    med.c_str(), model.c_str(), hum.c_str(), cw.c_str(), kills.c_str());

                if (rep) freeReplyObject(rep);

                WriteLog("Saved Character: " + charId + " | WS: " + ws + " | Model: " + model +
                         " | Hum: " + hum + " | CW: " + cw + " | Kills: " + kills, 1);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 202: СМЕРТЬ ПЕРСОНАЖА
            // ------------------------------------------------------------
            else if (method == "202" && args.size() >= 3) {
                string charId = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn,
                    "HSET Character:%s Alive 0", charId.c_str());
                if (rep) freeReplyObject(rep);

                // TTL 14 дней на мертвого персонажа для экономии RAM
                rep = (redisReply*)redisCommand(redisConn, "EXPIRE Character:%s 1209600", charId.c_str());
                if (rep) freeReplyObject(rep);

                WriteLog("Character DIED: " + charId, 1);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 301: ЗАГРУЗКА ВСЕЙ АКТИВНОЙ ТЕХНИКИ СЕРВЕРА
            // Синтаксис: CHILD:301:
            // Возвращает: ["PASS", [ [id, class, ws, inv, hp, fuel, damage, spawnType], ... ]]
            // ------------------------------------------------------------
            else if (method == "301") {
                size_t offset = 0;
                size_t limit = 999999;
                if (args.size() >= 3 && !args[2].empty()) {
                    try { offset = std::stoul(args[2]); } catch (...) {}
                }
                if (args.size() >= 4 && !args[3].empty()) {
                    try { limit = std::stoul(args[3]); } catch (...) {}
                }

                redisReply* smembers = (redisReply*)redisCommand(redisConn, "SMEMBERS Global:Vehicles");
                ostringstream oss;
                oss << "[\"PASS\",[";

                int loadedCount = 0;
                bool hasMore = false;
                if (smembers && smembers->type == REDIS_REPLY_ARRAY) {
                    size_t total = smembers->elements;
                    size_t endIdx = (offset + limit < total) ? (offset + limit) : total;
                    if (endIdx < total) hasMore = true;

                    for (size_t i = offset; i < endIdx; i++) {
                        string vId = smembers->element[i]->str ? smembers->element[i]->str : "";
                        if (vId.empty()) continue;

                        redisReply* vData = (redisReply*)redisCommand(redisConn,
                            "HMGET Vehicle:%s Class Worldspace Inventory Hitpoints Fuel Damage Alive SpawnType", vId.c_str());

                        if (vData && vData->type == REDIS_REPLY_ARRAY && vData->elements >= 8) {
                            string isAlive = (vData->element[6]->str ? vData->element[6]->str : "1");
                            if (isAlive == "1") {
                                string vClass = (vData->element[0]->str ? vData->element[0]->str : "");
                                string vWs    = (vData->element[1]->str ? vData->element[1]->str : "[]");
                                string vInv   = (vData->element[2]->str ? vData->element[2]->str : "[[],[],[]]");
                                string vHp    = (vData->element[3]->str ? vData->element[3]->str : "[]");
                                string vFuel  = (vData->element[4]->str ? vData->element[4]->str : "0.5");
                                string vDmg   = (vData->element[5]->str ? vData->element[5]->str : "0");
                                string vSpType= (vData->element[7]->str ? vData->element[7]->str : "world");

                                if (loadedCount > 0) oss << ",";
                                oss << "[\"" << vId << "\",\"" << vClass << "\","
                                    << vWs << "," << vInv << "," << vHp << ","
                                    << vFuel << "," << vDmg << ",\"" << vSpType << "\"]";
                                loadedCount++;
                            }
                        }
                        if (vData) freeReplyObject(vData);
                    }
                }
                if (smembers) freeReplyObject(smembers);
                oss << "]," << (hasMore ? "true" : "false") << "]";

                string payload = oss.str();
                WriteLog("Loaded " + to_string(loadedCount) + " vehicles (offset=" + to_string(offset) + ", hasMore=" + (hasMore ? "true" : "false") + ")", 1);
                SendPayload(output, outputSize, payload, "VEHICLES");
            }
            // ------------------------------------------------------------
            // 302: РЕГИСТРАЦИЯ НОВОЙ ТЕХНИКИ В БАЗЕ
            // Синтаксис: CHILD:302:class:worldspace:inventory:hitpoints:fuel:damage:spawnType:
            // ------------------------------------------------------------
            else if (method == "302" && args.size() >= 8) {
                string vClass = args[2];
                string vWs    = (!args[3].empty()) ? args[3] : "[]";
                string vInv   = (!args[4].empty()) ? args[4] : "[[],[],[]]";
                string vHp    = (!args[5].empty()) ? args[5] : "[]";
                string vFuel  = (!args[6].empty()) ? args[6] : "0.1";
                string vDmg   = (!args[7].empty()) ? args[7] : "0";
                string vSpType= (args.size() > 8 && !args[8].empty()) ? args[8] : "world";

                redisReply* idRep = (redisReply*)redisCommand(redisConn, "INCR Global:NextVehicleID");
                string newId = "1";
                if (idRep && idRep->type == REDIS_REPLY_INTEGER) {
                    newId = to_string(idRep->integer);
                }
                if (idRep) freeReplyObject(idRep);

                redisReply* rep = (redisReply*)redisCommand(redisConn,
                    "HMSET Vehicle:%s Class %s Worldspace %s Inventory %s Hitpoints %s Fuel %s Damage %s Alive 1 SpawnType %s",
                    newId.c_str(), vClass.c_str(), vWs.c_str(), vInv.c_str(), vHp.c_str(), vFuel.c_str(), vDmg.c_str(), vSpType.c_str());
                if (rep) freeReplyObject(rep);

                redisReply* setRep = (redisReply*)redisCommand(redisConn, "SADD Global:Vehicles %s", newId.c_str());
                if (setRep) freeReplyObject(setRep);

                WriteLog("Created Vehicle ID=" + newId + " Class=" + vClass + " SpawnType=" + vSpType, 1);
                string res = "[\"PASS\",\"" + newId + "\"]";
                strncpy_s(output, outputSize, res.c_str(), _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 305: СОХРАНЕНИЕ СОСТОЯНИЯ ТЕХНИКИ
            // Синтаксис: CHILD:305:id:worldspace:inventory:hitpoints:fuel:damage:
            // ------------------------------------------------------------
            else if (method == "305" && args.size() >= 7) {
                string vId   = args[2];
                string vWs   = (!args[3].empty()) ? args[3] : "[]";
                string vInv  = (!args[4].empty()) ? args[4] : "[[],[],[]]";
                string vHp   = (!args[5].empty()) ? args[5] : "[]";
                string vFuel = (!args[6].empty()) ? args[6] : "0";
                string vDmg  = (args.size() > 7 && !args[7].empty()) ? args[7] : "0";

                redisReply* rep = (redisReply*)redisCommand(redisConn,
                    "HMSET Vehicle:%s Worldspace %s Inventory %s Hitpoints %s Fuel %s Damage %s",
                    vId.c_str(), vWs.c_str(), vInv.c_str(), vHp.c_str(), vFuel.c_str(), vDmg.c_str());
                if (rep) freeReplyObject(rep);

                WriteLog("Saved Vehicle ID=" + vId + " Fuel=" + vFuel + " Damage=" + vDmg, 2);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 306: УНИЧТОЖЕНИЕ ТЕХНИКИ (Alive = 0, удаление из активных)
            // Синтаксис: CHILD:306:id:
            // ------------------------------------------------------------
            else if (method == "306" && args.size() >= 3) {
                string vId = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn, "HSET Vehicle:%s Alive 0", vId.c_str());
                if (rep) freeReplyObject(rep);

                redisReply* sremRep = (redisReply*)redisCommand(redisConn, "SREM Global:Vehicles %s", vId.c_str());
                if (sremRep) freeReplyObject(sremRep);

                // TTL 7 дней на архив уничтоженной техники
                redisReply* expRep = (redisReply*)redisCommand(redisConn, "EXPIRE Vehicle:%s 604800", vId.c_str());
                if (expRep) freeReplyObject(expRep);

                WriteLog("Vehicle DESTROYED: ID=" + vId, 1);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 401: СИСТЕМА БАНОВ
            // Синтаксис:
            // CHILD:401:ADD:uid:reason:admin:
            // CHILD:401:REMOVE:uid:
            // CHILD:401:GETALL:
            // ------------------------------------------------------------
            else if (method == "401") {
                string sub = (args.size() >= 3) ? args[2] : "";
                if (sub == "ADD" && args.size() >= 4) {
                    string uid    = args[3];
                    string reason = (args.size() > 4 && !args[4].empty()) ? args[4] : "Banned by administrator";
                    string admin  = (args.size() > 5 && !args[5].empty()) ? args[5] : "Server";
                    time_t now = time(nullptr);

                    redisReply* r1 = (redisReply*)redisCommand(redisConn, "SADD Global:BannedUIDs %s", uid.c_str());
                    if (r1) freeReplyObject(r1);

                    redisReply* r2 = (redisReply*)redisCommand(redisConn,
                        "HMSET Ban:%s Reason %s Admin %s Timestamp %ld",
                        uid.c_str(), reason.c_str(), admin.c_str(), (long)now);
                    if (r2) freeReplyObject(r2);

                    WriteLog("Player BANNED in Redis: UID=" + uid + " Reason=" + reason + " Admin=" + admin, 1);
                    strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
                }
                else if (sub == "REMOVE" && args.size() >= 4) {
                    string uid = args[3];
                    redisReply* r1 = (redisReply*)redisCommand(redisConn, "SREM Global:BannedUIDs %s", uid.c_str());
                    if (r1) freeReplyObject(r1);
                    redisReply* r2 = (redisReply*)redisCommand(redisConn, "DEL Ban:%s", uid.c_str());
                    if (r2) freeReplyObject(r2);

                    WriteLog("Player UNBANNED in Redis: UID=" + uid, 1);
                    strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
                }
                else if (sub == "GETALL") {
                    redisReply* rep = (redisReply*)redisCommand(redisConn, "SMEMBERS Global:BannedUIDs");
                    ostringstream oss;
                    oss << "[\"PASS\",[";
                    if (rep && rep->type == REDIS_REPLY_ARRAY) {
                        for (size_t i = 0; i < rep->elements; i++) {
                            string bUid = rep->element[i]->str ? rep->element[i]->str : "";
                            if (!bUid.empty()) {
                                if (i > 0) oss << ",";
                                oss << "\"" << bUid << "\"";
                            }
                        }
                    }
                    if (rep) freeReplyObject(rep);
                    oss << "]]";

                    string payload = oss.str();
                    WriteLog("Loaded banned UIDs from Redis: " + payload, 1);
                    SendPayload(output, outputSize, payload, "BANS");
                }
                else {
                    strncpy_s(output, outputSize, "[\"ERROR\",\"INVALID_SUBMETHOD\"]", _TRUNCATE);
                }
            }
            // ================================================================
            // СИСТЕМА ДОМОВ ORIGINS (Л1) — МЕТОДЫ 501..505
            // ----------------------------------------------------------------
            // ЛОГИКА ПОВТОРЯЕТ ТЕХНИКУ (301..306), НО В ОТДЕЛЬНОМ
            // ПРОСТРАНСТВЕ ИМЁН. ЭТО КРИТИЧНО:
            //
            //   Vehicle:<id>  + Global:Vehicles   — техника
            //   House:<id>    + Global:Houses     — дома
            //
            // Иначе CHILD:301 (загрузка техники) вернул бы дома как
            // машины, а CHILD:306 (уничтожение техники) их бы "убил" —
            // вместе со всем лутом внутри.
            //
            // ДОПОЛНИТЕЛЬНО: PlayerHouses:<UID> — SET домов игрока.
            // Нужен для быстрой проверки "есть ли у игрока дом"
            // без сканирования всех домов на сервере.
            //
            // ПОЛЯ ХЭША House:<id>:
            //   Class       — класс дома (wooden_shed_lvl_1 / large_shed_lvl_1)
            //   Worldspace  — [dir, pos, vectorUp]
            //   Inventory   — лут фундамента/дома (виден всем)
            //   StageAr     — [["stage_1",0],["stage_2",1],["Имя",9999999]]
            //   Stage       — текущий этап (1 = фундамент, 2 = готов)
            //   OwnerUID    — UID владельца
            //   OwnerName   — имя владельца (для таблички)
            //   BuildTime   — unixtime заливки фундамента (для таймера)
            //   Alive       — 1 активен, 0 удалён
            //   OpenState   — 0 закрыт, 1 открыт (для восстановления)
            // ================================================================

            // ------------------------------------------------------------
            // 501: СОЗДАНИЕ ДОМА (ЭТАП 1 — ФУНДАМЕНТ)
            // Синтаксис: CHILD:501:class:worldspace:ownerUID:ownerName:buildTime:
            // Возвращает: ["PASS","<houseId>"] или ["ERROR","<причина>"]
            // ------------------------------------------------------------
            // Проверка лимитов построек:
            //   - ДОМА (Uroven1/2/3...) — по 1 на каждый тип объекта
            //   - ЗАБОРЫ / ВЫШКИ / ВОРОТА (Fence/Watchtower/Gate/Wall и т.д.) — без ограничений
            //   - Удалённые (Alive 0) объекты не учитываются в лимитах
            //
            // Классификация по имени класса:
            //   содержит "fence", "watchtower", "gate", "zabor", "vyshka", "vorota", "wall", "stena" -> UNLIMITED
            //   все остальные классы построек -> LIMITED (1 на тип)
            // ------------------------------------------------------------
            else if (method == "501" && args.size() >= 6) {
                string hClass  = args[2];
                string hWs     = (!args[3].empty()) ? args[3] : "[]";
                string hOwner  = args[4];
                string hName   = (!args[5].empty()) ? args[5] : "Unknown";
                string hTime   = (args.size() > 6 && !args[6].empty()) ? args[6] : "0";

                // Определяем, ограничена ли постройка (1 на тип)
                bool limited = true;
                {
                    string lc = hClass;
                    for (size_t i = 0; i < lc.size(); i++) lc[i] = (char)tolower((unsigned char)lc[i]);

                    // Постройки без лимита — можно строить без ограничений по количеству.
                    const char* unlimitedMarks[] = {
                        "fence",      // забор (общий)
                        "watchtower", // вышка
                        "gate",       // ворота
                        "zabor",      // забор (рус. транслит)
                        "vyshka",     // вышка (рус. транслит)
                        "vorota",     // ворота (рус. транслит)
                        "wall",       // стена
                        "stena"       // стена (рус. транслит)
                    };
                    for (int m = 0; m < 8; m++) {
                        if (lc.find(unlimitedMarks[m]) != string::npos) { limited = false; break; }
                    }
                }

                long existingSameType = 0;

                if (limited) {
                    // Считаем активные (Alive == 1) дома этого же типа у игрока.
                    // Удалённые (Alive == 0) игнорируются.
                    redisReply* ownSet = (redisReply*)redisCommand(redisConn, "SMEMBERS PlayerHouses:%s", hOwner.c_str());
                    if (ownSet && ownSet->type == REDIS_REPLY_ARRAY) {
                        for (size_t i = 0; i < ownSet->elements; i++) {
                            string oid = ownSet->element[i]->str ? ownSet->element[i]->str : "";
                            if (oid.empty()) continue;

                            // Живой?
                            redisReply* aliveRep = (redisReply*)redisCommand(redisConn, "HGET House:%s Alive", oid.c_str());
                            string isAlive = "0";
                            if (aliveRep && aliveRep->type == REDIS_REPLY_STRING) isAlive = aliveRep->str;
                            if (aliveRep) freeReplyObject(aliveRep);
                            if (isAlive != "1") continue;

                            // Того же типа?
                            redisReply* clsRep = (redisReply*)redisCommand(redisConn, "HGET House:%s Class", oid.c_str());
                            string ocls = "";
                            if (clsRep && clsRep->type == REDIS_REPLY_STRING) ocls = clsRep->str;
                            if (clsRep) freeReplyObject(clsRep);

                            if (ocls == hClass) existingSameType++;
                        }
                    }
                    if (ownSet) freeReplyObject(ownSet);
                }

                if (existingSameType > 0) {
                    WriteLog("House CREATE REJECTED: UID=" + hOwner + " already has house of type " + hClass, 1);
                    strncpy_s(output, outputSize, "[\"ERROR\",\"ALREADY_HAS_HOUSE\"]", _TRUNCATE);
                }
                else {
                    redisReply* idRep = (redisReply*)redisCommand(redisConn, "INCR Global:NextHouseID");
                    string newId = "1";
                    if (idRep && idRep->type == REDIS_REPLY_INTEGER) {
                        newId = to_string(idRep->integer);
                    }
                    if (idRep) freeReplyObject(idRep);

                    // Этап 1: фундамент залит, основа ещё не построена.
                    // Это ровно как в оригинале 1.7.1:
                    //     _stageAr = [["stage_1",0],["stage_2",1],[Name,9999999]]
                    // где 0 = видимо, 1 = скрыто.
                    string stageAr = "[[\"stupenJeden\",0],[\"stupenDva\",1],[\"" + EscapeSQF(hName) + "\",9999999]]";

                    // Добавляем поля для аудита (кто и когда удалил)
                    redisReply* rep = (redisReply*)redisCommand(redisConn,
                        "HMSET House:%s Class %s Worldspace %s Inventory [[],[],[]] StageAr %s Stage 1 OwnerUID %s OwnerName %s BuildTime %s Alive 1 OpenState 0 Limited %d DeletedBy none DeletedAt 0",
                        newId.c_str(), hClass.c_str(), hWs.c_str(), stageAr.c_str(),
                        hOwner.c_str(), hName.c_str(), hTime.c_str(), limited ? 1 : 0);
                    if (rep) freeReplyObject(rep);

                    redisReply* setRep = (redisReply*)redisCommand(redisConn, "SADD Global:Houses %s", newId.c_str());
                    if (setRep) freeReplyObject(setRep);

                    redisReply* ownRep = (redisReply*)redisCommand(redisConn, "SADD PlayerHouses:%s %s", hOwner.c_str(), newId.c_str());
                    if (ownRep) freeReplyObject(ownRep);

                    WriteLog("Created House ID=" + newId + " Class=" + hClass + " Owner=" + hOwner +
                             " Limited=" + (limited ? "YES" : "NO") + " (STAGE 1 foundation)", 1);
                    string res = "[\"PASS\",\"" + newId + "\"]";
                    strncpy_s(output, outputSize, res.c_str(), _TRUNCATE);
                }
            }
            // ------------------------------------------------------------
            // 502: ЗАГРУЗКА ВСЕХ ДОМОВ (с пагинацией)
            // Синтаксис: CHILD:502:<offset>:<limit>:
            // Возвращает: ["PASS",[[id,class,ws,inv,stageAr,stage,ownerUID,ownerName,buildTime,openState],...],hasMore]
            // ------------------------------------------------------------
            else if (method == "502") {
                size_t offset = 0;
                size_t limit = 999999;
                if (args.size() >= 3 && !args[2].empty()) {
                    try { offset = std::stoul(args[2]); } catch (...) {}
                }
                if (args.size() >= 4 && !args[3].empty()) {
                    try { limit = std::stoul(args[3]); } catch (...) {}
                }

                redisReply* smembers = (redisReply*)redisCommand(redisConn, "SMEMBERS Global:Houses");
                ostringstream oss;
                oss << "[\"PASS\",[";

                int loadedCount = 0;
                bool hasMore = false;
                if (smembers && smembers->type == REDIS_REPLY_ARRAY) {
                    size_t total = smembers->elements;
                    size_t endIdx = (offset + limit < total) ? (offset + limit) : total;
                    if (endIdx < total) hasMore = true;

                    for (size_t i = offset; i < endIdx; i++) {
                        string hId = smembers->element[i]->str ? smembers->element[i]->str : "";
                        if (hId.empty()) continue;

                        redisReply* hData = (redisReply*)redisCommand(redisConn,
                            "HMGET House:%s Class Worldspace Inventory StageAr Stage OwnerUID OwnerName BuildTime OpenState Alive",
                            hId.c_str());

                        if (hData && hData->type == REDIS_REPLY_ARRAY && hData->elements >= 10) {
                            string isAlive = (hData->element[9]->str ? hData->element[9]->str : "1");
                            if (isAlive == "1") {
                                string hCls   = (hData->element[0]->str ? hData->element[0]->str : "");
                                string hWs2   = (hData->element[1]->str ? hData->element[1]->str : "[]");
                                string hInv   = (hData->element[2]->str ? hData->element[2]->str : "[[],[],[]]");
                                string hStage = (hData->element[3]->str ? hData->element[3]->str : "[]");
                                string hStg   = (hData->element[4]->str ? hData->element[4]->str : "1");
                                string hOwn   = (hData->element[5]->str ? hData->element[5]->str : "");
                                string hNm    = (hData->element[6]->str ? hData->element[6]->str : "");
                                string hTime2 = (hData->element[7]->str ? hData->element[7]->str : "0");
                                string hOpen  = (hData->element[8]->str ? hData->element[8]->str : "0");

                                if (loadedCount > 0) oss << ",";
                                oss << "[\"" << hId << "\",\"" << hCls << "\","
                                    << hWs2 << "," << hInv << "," << hStage << ","
                                    << hStg << ",\"" << hOwn << "\",\"" << hNm << "\","
                                    << hTime2 << "," << hOpen << "]";
                                loadedCount++;
                            }
                        }
                        if (hData) freeReplyObject(hData);
                    }
                }
                if (smembers) freeReplyObject(smembers);
                oss << "]," << (hasMore ? "true" : "false") << "]";

                string payload = oss.str();
                WriteLog("Loaded " + to_string(loadedCount) + " houses (offset=" + to_string(offset) + ", hasMore=" + (hasMore ? "true" : "false") + ")", 1);
                SendPayload(output, outputSize, payload, "HOUSES");
            }
            // ------------------------------------------------------------
            // 503: СОХРАНЕНИЕ ДОМА (лут, стадии, состояние дверей)
            // Синтаксис: CHILD:503:<id>:<inventory>:<stageAr>:<stage>:<openState>:
            // Возвращает: ["PASS"]
            // ------------------------------------------------------------
            else if (method == "503" && args.size() >= 5) {
                string hId    = args[2];
                string hInv   = (!args[3].empty()) ? args[3] : "[[],[],[]]";
                string hStage = (!args[4].empty()) ? args[4] : "[]";
                string hStg   = (args.size() > 5 && !args[5].empty()) ? args[5] : "1";
                string hOpen  = (args.size() > 6 && !args[6].empty()) ? args[6] : "0";

                redisReply* rep = (redisReply*)redisCommand(redisConn,
                    "HMSET House:%s Inventory %s StageAr %s Stage %s OpenState %s",
                    hId.c_str(), hInv.c_str(), hStage.c_str(), hStg.c_str(), hOpen.c_str());
                if (rep) freeReplyObject(rep);

                WriteLog("Saved House ID=" + hId + " Stage=" + hStg + " Open=" + hOpen, 2);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 504: УДАЛЕНИЕ ДОМА (С АРХИВОМ И АУДИТОМ)
            // Синтаксис: CHILD:504:<id>:<deletedBy>:
            // Возвращает: ["PASS"] или ["ERROR","NOT_FOUND"]
            // ------------------------------------------------------------
            // Логика удаления постройки:
            //   1. Убирает ID из Global:Houses (объект не грузится в игровой мир)
            //   2. Убирает ID из сета игрока PlayerHouses:<ownerUID>
            //   3. Удаляет хэш House:<id> из Redis (DEL)
            //
            // Аргумент deletedBy:
            //   Кто удалил — "owner", "admin:<name>" или "system".
            // ------------------------------------------------------------
            else if (method == "504" && args.size() >= 3) {
                string hId = args[2];
                string deletedBy = (args.size() > 3 && !args[3].empty()) ? args[3] : "unknown";

                // Существует ли вообще?
                redisReply* existRep = (redisReply*)redisCommand(redisConn, "EXISTS House:%s", hId.c_str());
                bool exists = (existRep && existRep->type == REDIS_REPLY_INTEGER && existRep->integer > 0);
                if (existRep) freeReplyObject(existRep);

                if (!exists) {
                    WriteLog("House DELETE FAILED: ID=" + hId + " not found", 1);
                    strncpy_s(output, outputSize, "[\"ERROR\",\"NOT_FOUND\"]", _TRUNCATE);
                }
                else {
                    // Узнаём владельца, чтобы почистить PlayerHouses
                    redisReply* ownRep = (redisReply*)redisCommand(redisConn, "HGET House:%s OwnerUID", hId.c_str());
                    string hOwner = "";
                    if (ownRep && ownRep->type == REDIS_REPLY_STRING) {
                        hOwner = ownRep->str;
                    }
                    if (ownRep) freeReplyObject(ownRep);

                    // ============================================================
                    // Полное удаление постройки:
                    // 1. Убираем из Global:Houses
                    // 2. Убираем из PlayerHouses:<ownerUID>
                    // 3. Полное удаление ключа House:<id> (DEL)
                    // ============================================================

                    // 1. Убираем из общего списка
                    redisReply* sremRep = (redisReply*)redisCommand(redisConn, "SREM Global:Houses %s", hId.c_str());
                    if (sremRep) freeReplyObject(sremRep);

                    // 2. Убираем из списка игрока
                    if (!hOwner.empty()) {
                        redisReply* ownSrem = (redisReply*)redisCommand(redisConn,
                            "SREM PlayerHouses:%s %s", hOwner.c_str(), hId.c_str());
                        if (ownSrem) freeReplyObject(ownSrem);

                        // Если у игрока больше нет домов — удаляем и сам сет
                        redisReply* cntRep = (redisReply*)redisCommand(redisConn,
                            "SCARD PlayerHouses:%s", hOwner.c_str());
                        long long leftCnt = 0;
                        if (cntRep && cntRep->type == REDIS_REPLY_INTEGER) {
                            leftCnt = cntRep->integer;
                        }
                        if (cntRep) freeReplyObject(cntRep);

                        if (leftCnt <= 0) {
                            redisReply* delSet = (redisReply*)redisCommand(redisConn,
                                "DEL PlayerHouses:%s", hOwner.c_str());
                            if (delSet) freeReplyObject(delSet);
                            WriteLog("House DELETE: PlayerHouses set cleared for " + hOwner, 1);
                        }
                    }

                    // 3. Удаляем сам дом НАВСЕГДА (никаких архивов)
                    redisReply* delRep = (redisReply*)redisCommand(redisConn, "DEL House:%s", hId.c_str());
                    long long deletedKeys = 0;
                    if (delRep && delRep->type == REDIS_REPLY_INTEGER) {
                        deletedKeys = delRep->integer;
                    }
                    if (delRep) freeReplyObject(delRep);

                    WriteLog("House DELETED (permanently): ID=" + hId + " Owner=" + hOwner +
                             " By=" + deletedBy + " (keys removed: " + std::to_string(deletedKeys) + ")", 1);
                    strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
                }
            }
            // ------------------------------------------------------------
            // 505: ПРОВЕРКА ДОМА У ИГРОКА
            // Синтаксис: CHILD:505:<ownerUID>:
            // Возвращает: ["PASS","<houseId>"] или ["PASS",""]
            // ------------------------------------------------------------
            else if (method == "505" && args.size() >= 3) {
                string hOwner = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn, "SMEMBERS PlayerHouses:%s", hOwner.c_str());
                string foundId = "";

                if (rep && rep->type == REDIS_REPLY_ARRAY && rep->elements > 0) {
                    for (size_t i = 0; i < rep->elements; i++) {
                        string hId = rep->element[i]->str ? rep->element[i]->str : "";
                        if (hId.empty()) continue;

                        // Проверяем что дом ещё жив
                        redisReply* aliveRep = (redisReply*)redisCommand(redisConn, "HGET House:%s Alive", hId.c_str());
                        string isAlive = "0";
                        if (aliveRep && aliveRep->type == REDIS_REPLY_STRING) {
                            isAlive = aliveRep->str;
                        }
                        if (aliveRep) freeReplyObject(aliveRep);

                        if (isAlive == "1") {
                            foundId = hId;
                            break;
                        }
                    }
                }
                if (rep) freeReplyObject(rep);

                WriteLog("House CHECK: UID=" + hOwner + " -> " + (foundId.empty() ? "NONE" : foundId), 2);
                string res = "[\"PASS\",\"" + foundId + "\"]";
                strncpy_s(output, outputSize, res.c_str(), _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 506: СПИСОК ВСЕХ ДОМОВ ИГРОКА (включая архив)
            // Синтаксис: CHILD:506:<ownerUID>:
            // Возвращает: ["PASS",[[id,class,stage,alive,deletedBy,
            //                       deletedAt,worldspace,buildTime],...]]
            // ------------------------------------------------------------
            // Возвращает список всех построек игрока:
            //   - Живые объекты (Alive = 1)
            //   - Ранее удалённые / архивные (если присутствуют)
            //
            // Формат элемента: [id, class, stage, alive, deletedBy, deletedAt, worldspace, buildTime]
            // ------------------------------------------------------------
            else if (method == "506" && args.size() >= 3) {
                string hOwner = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn, "SMEMBERS PlayerHouses:%s", hOwner.c_str());

                ostringstream oss;
                oss << "[\"PASS\",[";
                bool first = true;

                if (rep && rep->type == REDIS_REPLY_ARRAY) {
                    for (size_t i = 0; i < rep->elements; i++) {
                        string hId = rep->element[i]->str ? rep->element[i]->str : "";
                        if (hId.empty()) continue;

                        redisReply* r = (redisReply*)redisCommand(redisConn,
                            "HMGET House:%s Class Stage Alive DeletedBy DeletedAt Worldspace BuildTime", hId.c_str());

                        string hClass = "?", hStage = "0", hAlive = "0";
                        string hDelBy = "none", hDelAt = "0", hWs = "[]", hTime = "0";

                        if (r && r->type == REDIS_REPLY_ARRAY && r->elements >= 8) {
                            if (r->element[0] && r->element[0]->type == REDIS_REPLY_STRING) hClass = r->element[0]->str;
                            if (r->element[1] && r->element[1]->type == REDIS_REPLY_STRING) hStage = r->element[1]->str;
                            if (r->element[2] && r->element[2]->type == REDIS_REPLY_STRING) hAlive = r->element[2]->str;
                            if (r->element[3] && r->element[3]->type == REDIS_REPLY_STRING) hDelBy = r->element[3]->str;
                            if (r->element[4] && r->element[4]->type == REDIS_REPLY_STRING) hDelAt = r->element[4]->str;
                            if (r->element[5] && r->element[5]->type == REDIS_REPLY_STRING) hWs    = r->element[5]->str;
                            if (r->element[6] && r->element[6]->type == REDIS_REPLY_STRING) hTime  = r->element[6]->str;
                        }
                        if (r) freeReplyObject(r);

                        if (!first) oss << ",";
                        first = false;

                        oss << "[\"" << hId << "\",\"" << EscapeSQF(hClass) << "\"," << hStage
                            << "," << hAlive
                            << ",\"" << EscapeSQF(hDelBy) << "\"," << hDelAt
                            << "," << (hWs.empty() ? "[]" : hWs)
                            << "," << hTime << "]";
                    }
                }
                if (rep) freeReplyObject(rep);

                oss << "]]";
                WriteLog("House LIST for UID=" + hOwner, 2);
                strncpy_s(output, outputSize, oss.str().c_str(), _TRUNCATE);
            }
            // ------------------------------------------------------------
            // ------------------------------------------------------------
            // 507: ВОССТАНОВЛЕНИЕ ДОМА (УСТАРЕВШИЙ / ОТКЛЮЧЕННЫЙ МЕТОД)
            // ------------------------------------------------------------
            // Метод отключен: используется полное удаление построек через CHILD:504.
            // Если вызван — возвращает ["ERROR","RESTORE_REMOVED"].
            // ------------------------------------------------------------
            else if (method == "507") {
                WriteLog("House RESTORE rejected: feature removed (permanent delete only)", 1);
                strncpy_s(output, outputSize, "[\"ERROR\",\"RESTORE_REMOVED\"]", _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 508: СОХРАНИТЬ РЕАЛЬНЫЙ НИК ИГРОКА
            // Синтаксис: CHILD:508:<UID>:<экранированный_ник>:
            // Возвращает: ["PASS"] / ["ERROR","BAD_ARGS"]
            // ------------------------------------------------------------
            // Сохранение реального отображаемого никнейма игрока в Nick:<UID>.
            // Позволяет веб-панелям и внешним сервисам выполнять поиск по нику.
            //
            // Экранирование спецсимволов при передаче через разделитель ':':
            //   '\' -> "\\"      (обратный слэш)
            //   ':'  -> "\c"       (двоеточие)
            //   '"'  -> "\q"       (кавычка)
            //   LF   -> "\n"       (перевод строки)
            //   CR   -> "\r"       (возврат каретки)
            //
            // Ключ Redis: Nick:<UID> (string)
            // ------------------------------------------------------------
            else if (method == "508" && args.size() >= 4) {
                string uid  = args[2];
                string nick = args[3];

                if (uid.empty()) {
                    WriteLog("Nick SET FAILED: empty UID", 1);
                    strncpy_s(output, outputSize, "[\"ERROR\",\"BAD_ARGS\"]", _TRUNCATE);
                }
                else {
                    // Раскодируем экранирование, чтобы в базе лежал
                    // РЕАЛЬНЫЙ ник (с ':', кавычками и т.д.)
                    string decoded;
                    decoded.reserve(nick.size());
                    for (size_t i = 0; i < nick.size(); i++) {
                        if (nick[i] == '\\' && (i + 1) < nick.size()) {
                            char nxt = nick[i + 1];
                            if (nxt == '\\') { decoded += '\\'; i++; }
                            else if (nxt == 'c') { decoded += ':';  i++; }
                            else if (nxt == 'q') { decoded += '"';  i++; }
                            else if (nxt == 'n') { decoded += '\n'; i++; }
                            else if (nxt == 'r') { decoded += '\r'; i++; }
                            else { decoded += nick[i]; }
                        } else {
                            decoded += nick[i];
                        }
                    }

                    redisReply* rep = (redisReply*)redisCommand(redisConn,
                        "SET Nick:%s %s", uid.c_str(), decoded.c_str());
                    if (rep) freeReplyObject(rep);

                    WriteLog("Nick SAVED: UID=" + uid + " Nick=" + decoded, 2);
                    strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
                }
            }
            // ------------------------------------------------------------
            // 509: ПОЛУЧИТЬ РЕАЛЬНЫЙ НИК ИГРОКА
            // Синтаксис: CHILD:509:<UID>:
            // Возвращает: ["PASS","<ник>"] или ["PASS",""]
            // ------------------------------------------------------------
            // Возвращает сохранённый никнейм из ключа Nick:<UID>.
            // Применяется EscapeSQF для безопасной передачи строки в ArmA SQF.
            // ------------------------------------------------------------
            else if (method == "509" && args.size() >= 3) {
                string uid = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn, "GET Nick:%s", uid.c_str());
                string nick = "";
                if (rep && rep->type == REDIS_REPLY_STRING) {
                    nick = rep->str;
                }
                if (rep) freeReplyObject(rep);

                string res = "[\"PASS\",\"" + EscapeSQF(nick) + "\"]";
                WriteLog("Nick GET: UID=" + uid + " -> " + (nick.empty() ? "(none)" : nick), 2);
                strncpy_s(output, outputSize, res.c_str(), _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 510: ЗАДАТЬ ЗАДЕРЖКУ (COOLDOWN) ДЛЯ ДОМА
            // ------------------------------------------------------------
            // Синтаксис: CHILD:510:<houseId>:<seconds>:
            // Возвращает: ["PASS"] / ["ERROR","BAD_ARGS"]
            //
            // Установка временного кулдауна на улучшение постройки:
            //   SET HouseCooldown:<id> 1 EX <seconds>
            //
            // Redis автоматически удаляет ключ по истечении срока EX (секунды).
            // Пока ключ существует — улучшение объекта заблокировано.
            // Преимущества перед ручным расчетом:
            //   - Независим от разницы serverTime между клиентом и сервером
            //   - Корректно работает через рестарты сервера (TTL тикает в Redis)
            // ------------------------------------------------------------
            else if (method == "510" && args.size() >= 4) {
                string hId = args[2];
                string secStr = args[3];

                long long seconds = 0;
                try { seconds = std::stoll(secStr); } catch (...) { seconds = 0; }

                if (hId.empty() || seconds <= 0) {
                    WriteLog("Cooldown SET failed: bad args (id=" + hId + ", sec=" + secStr + ")", 1);
                    strncpy_s(output, outputSize, "[\"ERROR\",\"BAD_ARGS\"]", _TRUNCATE);
                }
                else {
                    // SETEX — установить значение С ИСТЕЧЕНИЕМ
                    redisReply* rep = (redisReply*)redisCommand(redisConn,
                        "SETEX HouseCooldown:%s %lld 1", hId.c_str(), seconds);
                    if (rep) freeReplyObject(rep);

                    WriteLog("Cooldown SET: House=" + hId + " for " + std::to_string(seconds) + " sec", 1);
                    strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
                }
            }
            // ------------------------------------------------------------
            // 511: СКОЛЬКО ОСТАЛОСЬ ЖДАТЬ (TTL ЗАДЕРЖКИ)
            // ------------------------------------------------------------
            // Синтаксис: CHILD:511:<houseId>:
            // Возвращает: ["PASS",<secondsLeft>]
            //   secondsLeft = 0  -> можно строить
            //   secondsLeft > 0  -> ещё ждать
            //   -1               -> ключа нет (можно строить)
            //   -2               -> ключ есть, но без TTL (ошибка) — трактуем как 0
            //
            // Использует команду TTL — Redis отдаёт остаток САМ.
            // ------------------------------------------------------------
            else if (method == "511" && args.size() >= 3) {
                string hId = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn,
                    "TTL HouseCooldown:%s", hId.c_str());

                long long ttl = -1;
                if (rep && rep->type == REDIS_REPLY_INTEGER) {
                    ttl = rep->integer;
                }
                if (rep) freeReplyObject(rep);

                // TTL возвращает:
                //   -2 — ключа нет
                //   -1 — ключ есть без TTL (не наш случай)
                //   >=0 — остаток секунд
                if (ttl < 0) { ttl = 0; }

                string res = "[\"PASS\"," + std::to_string(ttl) + "]";
                WriteLog("Cooldown CHECK: House=" + hId + " -> " + std::to_string(ttl) + " sec left", 2);
                strncpy_s(output, outputSize, res.c_str(), _TRUNCATE);
            }
            // ------------------------------------------------------------
            // 512: СНЯТЬ ЗАДЕРЖКУ (ПРИ УДАЛЕНИИ ДОМА)
            // ------------------------------------------------------------
            // Синтаксис: CHILD:512:<houseId>:
            // Возвращает: ["PASS"]
            //
            // Нужен, чтобы при удалении дома не осталось "висящего"
            // кулдауна: игрок снесёт фундамент и сразу строит новый.
            // ------------------------------------------------------------
            else if (method == "512" && args.size() >= 3) {
                string hId = args[2];

                redisReply* rep = (redisReply*)redisCommand(redisConn,
                    "DEL HouseCooldown:%s", hId.c_str());
                if (rep) freeReplyObject(rep);

                WriteLog("Cooldown CLEARED: House=" + hId, 1);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
            }
            else {
                WriteLog("Unknown SQF method: " + method, 1);
                strncpy_s(output, outputSize, "[\"PASS\"]", _TRUNCATE);
            }
        }
        catch (const std::exception& e) {
            strncpy_s(output, outputSize, "[\"ERROR\",\"EXCEPTION\"]", _TRUNCATE);
            WriteLog(string("CRITICAL: Unhandled C++ exception in RVExtension: ") + e.what(), 1);
        }
        catch (...) {
            strncpy_s(output, outputSize, "[\"ERROR\",\"UNKNOWN_EXCEPTION\"]", _TRUNCATE);
            WriteLog("CRITICAL: Unknown C++ exception in RVExtension", 1);
        }
    }
}