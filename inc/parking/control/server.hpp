/**
 * @file server.hpp
 * @brief 定义 parkingd 的本地 Unix seqpacket 控制服务器。
 */
#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>

namespace parking {

/** @brief 保存 Unix domain socket 对端的内核认证身份。 */
struct PeerCredentials {
    PeerCredentials();

    pid_t process_id;
    uid_t user_id;
    gid_t group_id;
};

/**
 * @brief 拥有 `parkingd` 手动控制 Unix `SOCK_SEQPACKET` 监听 socket。
 *
 * 构造时创建父目录、拒绝覆盖非 socket 文件、清除同路径的陈旧 socket 并开始监听。
 * 析构时关闭 fd 并只删除本对象成功绑定的 socket path。对象不可复制、不可移动，默认
 * 由 `parkingd` 单线程事件循环独占。
 */
class ManualCardServer {
public:
    /**
     * @brief 创建并监听手动刷卡控制 socket。
     * @param socket_path Unix socket 绝对路径，必须能放入 `sockaddr_un::sun_path`。
     * @param socket_mode 创建后的 POSIX 权限位，例如 0660。
     * @throws std::runtime_error 目录、socket、bind、chmod 或 listen 失败。
     * @throws std::invalid_argument 路径为空、不是绝对路径或过长。
     */
    ManualCardServer(const std::string& socket_path, unsigned int socket_mode);
    ~ManualCardServer();

    ManualCardServer(const ManualCardServer&) = delete;
    ManualCardServer& operator=(const ManualCardServer&) = delete;

    /** @brief 返回由本对象拥有的监听 fd 借用值，调用方不得关闭。 */
    int fileDescriptor() const noexcept;

    /**
     * @brief 接受一个本地客户端并读取其内核认证身份。
     * @param credentials 非空输出指针，成功时写入 pid/uid/gid。
     * @return 新连接 fd，所有权交给调用方并必须关闭。
     * @throws std::runtime_error accept、fcntl 或 SO_PEERCRED 失败。
     */
    int acceptClient(PeerCredentials* credentials) const;

    /**
     * @brief 接收一条完整 seqpacket 消息。
     * @param socket_fd 已连接的 Unix seqpacket fd。
     * @param maximum_bytes 可接受的 payload 上限。
     * @return 不含尾随 NUL 的完整 payload。
     * @throws std::runtime_error 对端关闭、接收失败或消息超过上限。
     */
    static std::string receivePacket(int socket_fd,
                                     std::size_t maximum_bytes);

    /**
     * @brief 原子发送一条 seqpacket 消息。
     * @param socket_fd 已连接的 Unix seqpacket fd。
     * @param packet 不含 NUL 的 payload。
     * @throws std::runtime_error 发送失败或内核没有接受完整消息。
     */
    static void sendPacket(int socket_fd, const std::string& packet);

private:
    int socket_fd_;
    std::string socket_path_;
    bool owns_socket_path_;
};

}  // namespace parking
