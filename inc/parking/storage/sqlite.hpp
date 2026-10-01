/**
 * @file sqlite.hpp
 * @brief 定义 parkingd 使用的 SQLite 持久化存储实现。
 */
#pragma once

#include "parking/storage/repository.hpp"

#include <cstddef>
#include <string>

struct sqlite3;

namespace parking {

/**
 * @brief 使用单个 SQLite 连接持久化 parking session 和 event journal。
 *
 * 构造时创建父目录、打开数据库、设置 WAL/FULL/foreign_keys/busy_timeout、执行版本化
 * schema migration 并运行 quick_check。对象拥有 sqlite3 connection，不可复制或移动，
 * 仅允许 `parkingd` 单 writer 线程调用。
 */
class SQLiteParkingRepository : public ParkingRepository {
public:
    /**
     * @brief 打开或创建停车数据库。
     * @param database_path 数据库绝对路径，或主机测试使用的 `:memory:`。
     * @throws std::invalid_argument 路径为空或不是绝对路径。
     * @throws std::runtime_error 目录、SQLite 打开、PRAGMA、migration 或 quick_check 失败。
     */
    explicit SQLiteParkingRepository(const std::string& database_path);
    ~SQLiteParkingRepository();

    SQLiteParkingRepository(const SQLiteParkingRepository&) = delete;
    SQLiteParkingRepository& operator=(const SQLiteParkingRepository&) = delete;

    void beginWriteTransaction() override;
    void commitTransaction() override;
    void rollbackTransaction() noexcept override;

    bool findActiveByCard(const CardId& card,
                          ParkingSession* session) const override;
    bool createActive(const ParkingSession& session) override;
    bool closeActive(const ParkingSession& session) override;
    bool findProcessedEvent(const std::string& event_id,
                            std::string* fingerprint,
                            AccessResult* result) const override;
    bool storeProcessedEvent(const CardPresentedEvent& event,
                             const std::string& fingerprint,
                             const AccessResult& result) override;
    std::size_t sessionCount() const override;
    std::size_t activeSessionCount() const override;

    /**
     * @brief 运行 `PRAGMA quick_check` 并要求结果严格为 `ok`。
     * @throws std::runtime_error 数据库损坏或检查语句失败。
     */
    void quickCheck() const;

    /** @brief 返回构造时使用的数据库路径。 */
    const std::string& databasePath() const noexcept;

private:
    void execute(const std::string& sql) const;
    void configure();
    void migrateSchema();
    std::size_t countSessions(bool active_only) const;

    sqlite3* database_;
    std::string database_path_;
    bool transaction_active_;
};

}  // namespace parking
