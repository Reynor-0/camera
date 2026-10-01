/*
 * 文件用途：实现 Unix SOCK_SEQPACKET 监听、对端身份读取和消息收发。
 * 所属层次：control，只负责本地 IPC，不执行停车业务规则。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "parking/control/server.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace parking {

namespace {

std::runtime_error systemError(const std::string& operation,
                               const std::string& path) {
    const int saved_errno = errno;
    std::ostringstream stream;
    stream << operation;
    if (!path.empty()) {
        stream << " '" << path << "'";
    }
    stream << " failed: " << std::strerror(saved_errno)
           << " (errno=" << saved_errno << ')';
    return std::runtime_error(stream.str());
}

void setCloseOnExec(int socket_fd) {
    const int flags = fcntl(socket_fd, F_GETFD);
    if (flags < 0 || fcntl(socket_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw systemError("fcntl(FD_CLOEXEC)", "");
    }
}

void ensureParentDirectory(const std::string& socket_path) {
    const std::string::size_type slash = socket_path.find_last_of('/');
    const std::string parent = slash == 0U ? "/" : socket_path.substr(0U, slash);
    if (parent == "/") {
        return;
    }
    if (mkdir(parent.c_str(), 0755) == 0 || errno == EEXIST) {
        struct stat status;
        if (stat(parent.c_str(), &status) != 0) {
            throw systemError("stat", parent);
        }
        if (!S_ISDIR(status.st_mode)) {
            throw std::runtime_error("control socket parent is not a directory: " +
                                     parent);
        }
        return;
    }
    throw systemError("mkdir", parent);
}

void removeStaleSocket(const std::string& socket_path) {
    struct stat status;
    if (lstat(socket_path.c_str(), &status) != 0) {
        if (errno == ENOENT) {
            return;
        }
        throw systemError("lstat", socket_path);
    }
    if (!S_ISSOCK(status.st_mode)) {
        throw std::runtime_error(
            "refusing to replace a non-socket control path: " + socket_path);
    }
    if (unlink(socket_path.c_str()) != 0) {
        throw systemError("unlink stale socket", socket_path);
    }
}

}  // namespace

PeerCredentials::PeerCredentials()
    : process_id(0), user_id(0U), group_id(0U) {}

ManualCardServer::ManualCardServer(const std::string& socket_path,
                                   unsigned int socket_mode)
    : socket_fd_(-1),
      socket_path_(socket_path),
      owns_socket_path_(false) {
    if (socket_path_.empty() || socket_path_[0] != '/') {
        throw std::invalid_argument(
            "manual control socket path must be absolute");
    }
    struct sockaddr_un address;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("manual control socket path is too long");
    }
    ensureParentDirectory(socket_path_);
    removeStaleSocket(socket_path_);

    socket_fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (socket_fd_ < 0) {
        throw systemError("socket(AF_UNIX, SOCK_SEQPACKET)", socket_path_);
    }

    try {
        setCloseOnExec(socket_fd_);
        std::memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(),
                    socket_path_.size() + 1U);
        if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&address),
                 sizeof(address)) != 0) {
            throw systemError("bind", socket_path_);
        }
        owns_socket_path_ = true;
        if (chmod(socket_path_.c_str(),
                  static_cast<mode_t>(socket_mode)) != 0) {
            throw systemError("chmod", socket_path_);
        }
        if (listen(socket_fd_, 16) != 0) {
            throw systemError("listen", socket_path_);
        }
    } catch (...) {
        close(socket_fd_);
        socket_fd_ = -1;
        if (owns_socket_path_) {
            unlink(socket_path_.c_str());
            owns_socket_path_ = false;
        }
        throw;
    }
}

ManualCardServer::~ManualCardServer() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
    }
    if (owns_socket_path_) {
        unlink(socket_path_.c_str());
    }
}

int ManualCardServer::fileDescriptor() const noexcept {
    return socket_fd_;
}

int ManualCardServer::acceptClient(PeerCredentials* credentials) const {
    if (credentials == nullptr) {
        throw std::invalid_argument(
            "acceptClient requires a non-null credentials pointer");
    }
    const int client_fd = accept(socket_fd_, nullptr, nullptr);
    if (client_fd < 0) {
        throw systemError("accept", socket_path_);
    }
    try {
        setCloseOnExec(client_fd);
        struct ucred native_credentials;
        socklen_t length = static_cast<socklen_t>(sizeof(native_credentials));
        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED,
                       &native_credentials, &length) != 0) {
            throw systemError("getsockopt(SO_PEERCRED)", socket_path_);
        }
        credentials->process_id = native_credentials.pid;
        credentials->user_id = native_credentials.uid;
        credentials->group_id = native_credentials.gid;
    } catch (...) {
        close(client_fd);
        throw;
    }
    return client_fd;
}

std::string ManualCardServer::receivePacket(int socket_fd,
                                            std::size_t maximum_bytes) {
    if (maximum_bytes == 0U) {
        throw std::invalid_argument("maximum packet size cannot be zero");
    }
    std::vector<char> buffer(maximum_bytes);
    ssize_t received;
    do {
        received = recv(socket_fd, &buffer[0], buffer.size(), MSG_TRUNC);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        throw systemError("recv", "");
    }
    if (received == 0) {
        throw std::runtime_error("control client closed without a request");
    }
    if (static_cast<std::size_t>(received) > maximum_bytes) {
        throw std::runtime_error("control packet exceeds maximum size");
    }
    return std::string(&buffer[0], static_cast<std::size_t>(received));
}

void ManualCardServer::sendPacket(int socket_fd, const std::string& packet) {
    ssize_t sent;
    do {
        sent = send(socket_fd, packet.data(), packet.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        throw systemError("send", "");
    }
    if (static_cast<std::size_t>(sent) != packet.size()) {
        throw std::runtime_error("kernel accepted a partial seqpacket message");
    }
}

}  // namespace parking
