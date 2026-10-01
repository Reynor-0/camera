/*
 * 文件用途：实现 SQLite schema、事务、session 历史和 event 幂等日志。
 * 所属层次：storage，是 parkingd 当前使用的持久化 adapter。
 */
#include "parking/storage/sqlite.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

#include <sqlite3.h>

namespace parking {

namespace {

class Statement {
public:
    Statement(sqlite3* database,
              const std::string& database_path,
              const char* sql)
        : database_(database),
          database_path_(database_path),
          statement_(nullptr) {
        const int result = sqlite3_prepare_v2(database_, sql, -1,
                                              &statement_, nullptr);
        if (result != SQLITE_OK) {
            std::ostringstream message;
            message << "sqlite prepare failed for '" << database_path_
                    << "': " << sqlite3_errmsg(database_)
                    << " (code=" << result << ')';
            throw std::runtime_error(message.str());
        }
    }

    ~Statement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3* database_;
    std::string database_path_;
    sqlite3_stmt* statement_;
};

std::runtime_error sqliteError(sqlite3* database,
                               const std::string& database_path,
                               const std::string& operation,
                               int result) {
    std::ostringstream message;
    message << operation << " failed for '" << database_path << "': "
            << sqlite3_errmsg(database) << " (code=" << result << ')';
    return std::runtime_error(message.str());
}

void requireBind(sqlite3* database,
                 const std::string& database_path,
                 int result,
                 const char* field_name) {
    if (result != SQLITE_OK) {
        throw sqliteError(database, database_path,
                          std::string("bind ") + field_name, result);
    }
}

void bindText(sqlite3* database,
              const std::string& database_path,
              sqlite3_stmt* statement,
              int index,
              const std::string& value,
              const char* field_name) {
    if (value.size() > static_cast<std::size_t>(
                           std::numeric_limits<int>::max())) {
        throw std::invalid_argument(std::string(field_name) +
                                    " exceeds SQLite text length limit");
    }
    requireBind(database, database_path,
                sqlite3_bind_text(statement, index, value.c_str(),
                                  static_cast<int>(value.size()),
                                  SQLITE_TRANSIENT),
                field_name);
}

void bindInt64(sqlite3* database,
               const std::string& database_path,
               sqlite3_stmt* statement,
               int index,
               std::int64_t value,
               const char* field_name) {
    requireBind(database, database_path,
                sqlite3_bind_int64(statement, index,
                                   static_cast<sqlite3_int64>(value)),
                field_name);
}

std::string columnText(sqlite3_stmt* statement, int index) {
    const unsigned char* value = sqlite3_column_text(statement, index);
    if (value == nullptr) {
        return std::string();
    }
    return reinterpret_cast<const char*>(value);
}

CardEventSource parseSource(const std::string& source) {
    if (source == "serial_rfid") {
        return CardEventSource::kSerialRfid;
    }
    if (source == "manual_simulator") {
        return CardEventSource::kManualSimulator;
    }
    throw std::runtime_error("database contains an unknown card event source");
}

AccessOutcome parseOutcome(const std::string& outcome) {
    if (outcome == "ENTRY_GRANTED") {
        return AccessOutcome::kEntryGranted;
    }
    if (outcome == "EXIT_GRANTED") {
        return AccessOutcome::kExitGranted;
    }
    if (outcome == "REJECTED") {
        return AccessOutcome::kRejected;
    }
    throw std::runtime_error("database contains an unknown access outcome");
}

AccessReason parseReason(const std::string& reason) {
    if (reason == "NONE") {
        return AccessReason::kNone;
    }
    if (reason == "DUPLICATE_ENTRY") {
        return AccessReason::kDuplicateEntry;
    }
    if (reason == "NO_ACTIVE_SESSION") {
        return AccessReason::kNoActiveSession;
    }
    if (reason == "RECOGNITION_FAILED") {
        return AccessReason::kRecognitionFailed;
    }
    if (reason == "INVALID_EVENT_ID") {
        return AccessReason::kInvalidEventId;
    }
    if (reason == "EVENT_ID_CONFLICT") {
        return AccessReason::kEventIdConflict;
    }
    if (reason == "INVALID_EVENT_TIME") {
        return AccessReason::kInvalidEventTime;
    }
    if (reason == "REPOSITORY_CONFLICT") {
        return AccessReason::kRepositoryConflict;
    }
    throw std::runtime_error("database contains an unknown access reason");
}

void ensureDirectory(const std::string& path) {
    struct stat status;
    if (stat(path.c_str(), &status) == 0) {
        if (!S_ISDIR(status.st_mode)) {
            throw std::runtime_error("database parent is not a directory: " +
                                     path);
        }
        return;
    }
    if (errno != ENOENT) {
        std::ostringstream message;
        message << "stat '" << path << "' failed: " << std::strerror(errno)
                << " (errno=" << errno << ')';
        throw std::runtime_error(message.str());
    }
    if (mkdir(path.c_str(), 0750) != 0 && errno != EEXIST) {
        std::ostringstream message;
        message << "mkdir '" << path << "' failed: " << std::strerror(errno)
                << " (errno=" << errno << ')';
        throw std::runtime_error(message.str());
    }
}

void ensureDatabaseParent(const std::string& database_path) {
    if (database_path == ":memory:") {
        return;
    }
    std::string::size_type slash = database_path.find_last_of('/');
    if (slash == std::string::npos || slash == 0U) {
        return;
    }
    const std::string parent = database_path.substr(0U, slash);
    std::string::size_type position = 1U;
    while (true) {
        const std::string::size_type next = parent.find('/', position);
        ensureDirectory(parent.substr(0U, next));
        if (next == std::string::npos) {
            break;
        }
        position = next + 1U;
    }
}

std::int64_t monotonicToSqlite(std::uint64_t value) {
    const std::uint64_t maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (value > maximum) {
        throw std::runtime_error("monotonic timestamp exceeds SQLite INTEGER");
    }
    return static_cast<std::int64_t>(value);
}

std::int64_t queryPragmaInteger(sqlite3* database,
                                const std::string& database_path,
                                const char* sql) {
    Statement statement(database, database_path, sql);
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW) {
        throw sqliteError(database, database_path, sql, result);
    }
    const std::int64_t value = static_cast<std::int64_t>(
        sqlite3_column_int64(statement.get(), 0));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string(sql) +
                                 " returned more than one row");
    }
    return value;
}

}  // namespace

SQLiteParkingRepository::SQLiteParkingRepository(
    const std::string& database_path)
    : database_(nullptr),
      database_path_(database_path),
      transaction_active_(false) {
    if (database_path_.empty() ||
        (database_path_ != ":memory:" && database_path_[0] != '/')) {
        throw std::invalid_argument(
            "SQLite database path must be absolute or :memory:");
    }
    ensureDatabaseParent(database_path_);

    const mode_t previous_umask = umask(0007);
    const int result = sqlite3_open_v2(
        database_path_.c_str(), &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    umask(previous_umask);
    if (result != SQLITE_OK) {
        const std::string detail = database_ == nullptr
                                       ? "sqlite returned no connection"
                                       : sqlite3_errmsg(database_);
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
            database_ = nullptr;
        }
        std::ostringstream message;
        message << "sqlite open failed for '" << database_path_
                << "': " << detail << " (code=" << result << ')';
        throw std::runtime_error(message.str());
    }

    try {
        if (database_path_ != ":memory:" &&
            chmod(database_path_.c_str(), 0640) != 0) {
            const int saved_errno = errno;
            std::ostringstream message;
            message << "chmod database '" << database_path_
                    << "' failed: " << std::strerror(saved_errno)
                    << " (errno=" << saved_errno << ')';
            throw std::runtime_error(message.str());
        }
        configure();
        migrateSchema();
        quickCheck();
    } catch (...) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
        throw;
    }
}

SQLiteParkingRepository::~SQLiteParkingRepository() {
    rollbackTransaction();
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
    }
}

void SQLiteParkingRepository::execute(const std::string& sql) const {
    char* error_message = nullptr;
    const int result = sqlite3_exec(database_, sql.c_str(), nullptr, nullptr,
                                    &error_message);
    if (result != SQLITE_OK) {
        const std::string detail = error_message == nullptr
                                       ? sqlite3_errmsg(database_)
                                       : error_message;
        sqlite3_free(error_message);
        std::ostringstream message;
        message << "sqlite exec failed for '" << database_path_
                << "': " << detail << " (code=" << result << ')';
        throw std::runtime_error(message.str());
    }
}

void SQLiteParkingRepository::configure() {
    if (sqlite3_busy_timeout(database_, 3000) != SQLITE_OK) {
        throw sqliteError(database_, database_path_, "sqlite3_busy_timeout",
                          sqlite3_errcode(database_));
    }
    execute("PRAGMA foreign_keys=ON;");
    if (database_path_ != ":memory:") {
        execute("PRAGMA journal_mode=WAL;");
    }
    execute("PRAGMA synchronous=FULL;");
    execute("PRAGMA wal_autocheckpoint=100;");
    if (queryPragmaInteger(database_, database_path_,
                           "PRAGMA foreign_keys;") != 1 ||
        queryPragmaInteger(database_, database_path_,
                           "PRAGMA synchronous;") != 2 ||
        queryPragmaInteger(database_, database_path_,
                           "PRAGMA wal_autocheckpoint;") != 100 ||
        queryPragmaInteger(database_, database_path_,
                           "PRAGMA busy_timeout;") != 3000) {
        throw std::runtime_error(
            "SQLite connection did not retain required PRAGMA settings");
    }
    if (database_path_ != ":memory:") {
        Statement journal_mode(database_, database_path_,
                               "PRAGMA journal_mode;");
        const int result = sqlite3_step(journal_mode.get());
        if (result != SQLITE_ROW ||
            columnText(journal_mode.get(), 0) != "wal" ||
            sqlite3_step(journal_mode.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "SQLite database did not enter WAL journal mode");
        }
    }
}

void SQLiteParkingRepository::migrateSchema() {
    beginWriteTransaction();
    try {
        execute(
            "CREATE TABLE IF NOT EXISTS schema_meta ("
            "key TEXT PRIMARY KEY, value TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS parking_sessions ("
            "session_id TEXT PRIMARY KEY,"
            "entry_event_id TEXT NOT NULL UNIQUE,"
            "card_uid TEXT NOT NULL,"
            "plate_number TEXT NOT NULL,"
            "entry_confidence_millionths INTEGER NOT NULL "
            "CHECK(entry_confidence_millionths BETWEEN 0 AND 1000000),"
            "entry_source TEXT NOT NULL "
            "CHECK(entry_source IN ('serial_rfid','manual_simulator')),"
            "entry_source_instance TEXT NOT NULL,"
            "entry_utc_ms INTEGER NOT NULL,"
            "active INTEGER NOT NULL CHECK(active IN (0,1)),"
            "exit_event_id TEXT UNIQUE,"
            "exit_source TEXT CHECK(exit_source IS NULL OR "
            "exit_source IN ('serial_rfid','manual_simulator')),"
            "exit_source_instance TEXT,"
            "exit_utc_ms INTEGER,"
            "parking_duration_seconds INTEGER "
            "CHECK(parking_duration_seconds IS NULL OR "
            "parking_duration_seconds>=0),"
            "fee_cent INTEGER CHECK(fee_cent IS NULL OR fee_cent>=0));"
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_active_card "
            "ON parking_sessions(card_uid) WHERE active=1;"
            "CREATE TABLE IF NOT EXISTS access_events ("
            "event_id TEXT PRIMARY KEY,"
            "fingerprint TEXT NOT NULL,"
            "card_uid TEXT NOT NULL,"
            "direction TEXT NOT NULL CHECK(direction IN ('entry','exit')),"
            "source TEXT NOT NULL "
            "CHECK(source IN ('serial_rfid','manual_simulator')),"
            "source_instance TEXT NOT NULL,"
            "receive_monotonic_ns INTEGER NOT NULL,"
            "receive_utc_ms INTEGER NOT NULL,"
            "outcome TEXT NOT NULL CHECK(outcome IN "
            "('ENTRY_GRANTED','EXIT_GRANTED','REJECTED')),"
            "reason TEXT NOT NULL CHECK(reason IN "
            "('NONE','DUPLICATE_ENTRY','NO_ACTIVE_SESSION',"
            "'RECOGNITION_FAILED','INVALID_EVENT_ID','EVENT_ID_CONFLICT',"
            "'INVALID_EVENT_TIME','REPOSITORY_CONFLICT')),"
            "session_id TEXT,"
            "plate_number TEXT,"
            "parking_duration_seconds INTEGER NOT NULL "
            "CHECK(parking_duration_seconds>=0),"
            "fee_cent INTEGER NOT NULL CHECK(fee_cent>=0));"
            "INSERT OR IGNORE INTO schema_meta(key,value) "
            "VALUES('schema_version','1');");

        Statement version(database_, database_path_,
                          "SELECT value FROM schema_meta "
                          "WHERE key='schema_version';");
        const int step_result = sqlite3_step(version.get());
        if (step_result != SQLITE_ROW ||
            columnText(version.get(), 0) != "1" ||
            sqlite3_step(version.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "unsupported or malformed parking database schema version");
        }
        commitTransaction();
    } catch (...) {
        rollbackTransaction();
        throw;
    }
}

void SQLiteParkingRepository::beginWriteTransaction() {
    if (transaction_active_) {
        throw std::runtime_error("SQLite write transaction is already active");
    }
    execute("BEGIN IMMEDIATE;");
    transaction_active_ = true;
}

void SQLiteParkingRepository::commitTransaction() {
    if (!transaction_active_) {
        throw std::runtime_error("no SQLite write transaction is active");
    }
    execute("COMMIT;");
    transaction_active_ = false;
}

void SQLiteParkingRepository::rollbackTransaction() noexcept {
    if (!transaction_active_ || database_ == nullptr) {
        return;
    }
    sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
    transaction_active_ = false;
}

bool SQLiteParkingRepository::findActiveByCard(
    const CardId& card,
    ParkingSession* session) const {
    if (session == nullptr) {
        throw std::invalid_argument(
            "findActiveByCard requires a non-null output pointer");
    }
    Statement statement(
        database_, database_path_,
        "SELECT session_id,entry_event_id,plate_number,"
        "entry_confidence_millionths,entry_source,entry_source_instance,"
        "entry_utc_ms FROM parking_sessions "
        "WHERE card_uid=?1 AND active=1;");
    bindText(database_, database_path_, statement.get(), 1, card.value(),
             "card_uid");
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return false;
    }
    if (result != SQLITE_ROW) {
        throw sqliteError(database_, database_path_, "select active session",
                          result);
    }

    const sqlite3_int64 confidence = sqlite3_column_int64(statement.get(), 3);
    if (confidence < 0 || confidence > 1000000) {
        throw std::runtime_error(
            "database contains an out-of-range plate confidence");
    }
    const CardPresentedEvent entry_event(
        columnText(statement.get(), 1), card, LaneDirection::kEntry,
        parseSource(columnText(statement.get(), 4)),
        columnText(statement.get(), 5), 0U,
        static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 6)));
    ParkingSession loaded(
        columnText(statement.get(), 0), entry_event,
        columnText(statement.get(), 2),
        static_cast<std::uint32_t>(confidence));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "active card uniqueness invariant is violated");
    }
    *session = loaded;
    return true;
}

bool SQLiteParkingRepository::createActive(const ParkingSession& session) {
    if (!transaction_active_) {
        throw std::runtime_error("createActive requires a write transaction");
    }
    if (!session.active) {
        return false;
    }
    Statement statement(
        database_, database_path_,
        "INSERT INTO parking_sessions("
        "session_id,entry_event_id,card_uid,plate_number,"
        "entry_confidence_millionths,entry_source,entry_source_instance,"
        "entry_utc_ms,active) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,1);");
    bindText(database_, database_path_, statement.get(), 1,
             session.session_id, "session_id");
    bindText(database_, database_path_, statement.get(), 2,
             session.entry_event_id,
             "entry_event_id");
    bindText(database_, database_path_, statement.get(), 3,
             session.card.value(), "card_uid");
    bindText(database_, database_path_, statement.get(), 4,
             session.plate_number, "plate_number");
    bindInt64(database_, database_path_, statement.get(), 5,
              static_cast<std::int64_t>(session.entry_confidence_millionths),
              "entry_confidence_millionths");
    bindText(database_, database_path_, statement.get(), 6,
             cardEventSourceName(session.entry_source), "entry_source");
    bindText(database_, database_path_, statement.get(), 7,
             session.entry_source_instance, "entry_source_instance");
    bindInt64(database_, database_path_, statement.get(), 8,
              session.entry_utc_ms, "entry_utc_ms");
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_CONSTRAINT) {
        return false;
    }
    if (result != SQLITE_DONE) {
        throw sqliteError(database_, database_path_, "insert active session",
                          result);
    }
    return true;
}

bool SQLiteParkingRepository::closeActive(const ParkingSession& session) {
    if (!transaction_active_) {
        throw std::runtime_error("closeActive requires a write transaction");
    }
    if (session.active) {
        return false;
    }
    Statement statement(
        database_, database_path_,
        "UPDATE parking_sessions SET active=0,exit_event_id=?1,"
        "exit_source=?2,exit_source_instance=?3,exit_utc_ms=?4,"
        "parking_duration_seconds=?5,fee_cent=?6 "
        "WHERE session_id=?7 AND card_uid=?8 AND active=1;");
    bindText(database_, database_path_, statement.get(), 1,
             session.exit_event_id, "exit_event_id");
    bindText(database_, database_path_, statement.get(), 2,
             cardEventSourceName(session.exit_source), "exit_source");
    bindText(database_, database_path_, statement.get(), 3,
             session.exit_source_instance, "exit_source_instance");
    bindInt64(database_, database_path_, statement.get(), 4,
              session.exit_utc_ms, "exit_utc_ms");
    bindInt64(database_, database_path_, statement.get(), 5,
              session.parking_duration_seconds, "parking_duration_seconds");
    bindInt64(database_, database_path_, statement.get(), 6,
              session.fee_cent, "fee_cent");
    bindText(database_, database_path_, statement.get(), 7,
             session.session_id, "session_id");
    bindText(database_, database_path_, statement.get(), 8,
             session.card.value(), "card_uid");
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_DONE) {
        throw sqliteError(database_, database_path_, "close active session",
                          result);
    }
    return sqlite3_changes(database_) == 1;
}

bool SQLiteParkingRepository::findProcessedEvent(
    const std::string& event_id,
    std::string* fingerprint,
    AccessResult* result) const {
    if (fingerprint == nullptr || result == nullptr) {
        throw std::invalid_argument(
            "findProcessedEvent requires non-null output pointers");
    }
    Statement statement(
        database_, database_path_,
        "SELECT fingerprint,outcome,reason,session_id,plate_number,"
        "parking_duration_seconds,fee_cent FROM access_events "
        "WHERE event_id=?1;");
    bindText(database_, database_path_, statement.get(), 1, event_id,
             "event_id");
    const int step_result = sqlite3_step(statement.get());
    if (step_result == SQLITE_DONE) {
        return false;
    }
    if (step_result != SQLITE_ROW) {
        throw sqliteError(database_, database_path_, "select access event",
                          step_result);
    }

    const std::string loaded_fingerprint = columnText(statement.get(), 0);
    AccessResult loaded(parseOutcome(columnText(statement.get(), 1)),
                        parseReason(columnText(statement.get(), 2)));
    loaded.event_id = event_id;
    loaded.session_id = columnText(statement.get(), 3);
    loaded.plate_number = columnText(statement.get(), 4);
    loaded.parking_duration_seconds = static_cast<std::int64_t>(
        sqlite3_column_int64(statement.get(), 5));
    loaded.fee_cent = static_cast<std::int64_t>(
        sqlite3_column_int64(statement.get(), 6));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "event ID uniqueness invariant is violated");
    }
    *fingerprint = loaded_fingerprint;
    *result = loaded;
    return true;
}

bool SQLiteParkingRepository::storeProcessedEvent(
    const CardPresentedEvent& event,
    const std::string& fingerprint,
    const AccessResult& result) {
    if (!transaction_active_) {
        throw std::runtime_error(
            "storeProcessedEvent requires a write transaction");
    }
    if (result.event_id != event.event_id || fingerprint.empty()) {
        throw std::invalid_argument("processed event metadata is inconsistent");
    }
    Statement statement(
        database_, database_path_,
        "INSERT INTO access_events("
        "event_id,fingerprint,card_uid,direction,source,source_instance,"
        "receive_monotonic_ns,receive_utc_ms,outcome,reason,session_id,"
        "plate_number,parking_duration_seconds,fee_cent) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);");
    bindText(database_, database_path_, statement.get(), 1,
             event.event_id, "event_id");
    bindText(database_, database_path_, statement.get(), 2,
             fingerprint, "fingerprint");
    bindText(database_, database_path_, statement.get(), 3,
             event.card.value(), "card_uid");
    bindText(database_, database_path_, statement.get(), 4,
             event.direction == LaneDirection::kEntry ? "entry" : "exit",
             "direction");
    bindText(database_, database_path_, statement.get(), 5,
             cardEventSourceName(event.source), "source");
    bindText(database_, database_path_, statement.get(), 6,
             event.source_instance, "source_instance");
    bindInt64(database_, database_path_, statement.get(), 7,
              monotonicToSqlite(event.receive_monotonic_ns),
              "receive_monotonic_ns");
    bindInt64(database_, database_path_, statement.get(), 8,
              event.receive_utc_ms, "receive_utc_ms");
    bindText(database_, database_path_, statement.get(), 9,
             accessOutcomeName(result.outcome), "outcome");
    bindText(database_, database_path_, statement.get(), 10,
             accessReasonName(result.reason), "reason");
    bindText(database_, database_path_, statement.get(), 11,
             result.session_id, "session_id");
    bindText(database_, database_path_, statement.get(), 12,
             result.plate_number, "plate_number");
    bindInt64(database_, database_path_, statement.get(), 13,
              result.parking_duration_seconds, "parking_duration_seconds");
    bindInt64(database_, database_path_, statement.get(), 14,
              result.fee_cent, "fee_cent");
    const int step_result = sqlite3_step(statement.get());
    if (step_result == SQLITE_CONSTRAINT) {
        return false;
    }
    if (step_result != SQLITE_DONE) {
        throw sqliteError(database_, database_path_, "insert access event",
                          step_result);
    }
    return true;
}

std::size_t SQLiteParkingRepository::countSessions(bool active_only) const {
    Statement statement(database_, database_path_,
                        active_only
                            ? "SELECT COUNT(*) FROM parking_sessions "
                              "WHERE active=1;"
                            : "SELECT COUNT(*) FROM parking_sessions;");
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW) {
        throw sqliteError(database_, database_path_, "count sessions", result);
    }
    const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 0);
    if (count < 0 || static_cast<unsigned long long>(count) >
                         std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("SQLite session count is out of range");
    }
    return static_cast<std::size_t>(count);
}

std::size_t SQLiteParkingRepository::sessionCount() const {
    return countSessions(false);
}

std::size_t SQLiteParkingRepository::activeSessionCount() const {
    return countSessions(true);
}

void SQLiteParkingRepository::quickCheck() const {
    Statement statement(database_, database_path_, "PRAGMA quick_check;");
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW || columnText(statement.get(), 0) != "ok" ||
        sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("SQLite quick_check did not return exactly ok");
    }
}

const std::string& SQLiteParkingRepository::databasePath() const noexcept {
    return database_path_;
}

}  // namespace parking
