#pragma once

#include <cstdint>
#include <string>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::io {

void state(ps2::state::Stream &s);

int open(const std::string &path, std::uint32_t flags);
int close(int fd);
int read(int fd, std::uint8_t *out, std::uint32_t size);
int write(int fd, const std::uint8_t *data, std::uint32_t size);
bool captured(int fd);
std::int64_t seek(int fd, std::int64_t offset, int whence);
bool stat(const std::string &path, std::uint32_t &mode, std::uint32_t &size);
int make_dir(const std::string &path);
int remove(const std::string &path);
int rename_file(const std::string &from, const std::string &to);
int open_dir(const std::string &path);
int close_dir(int fd);
int read_dir(int fd, std::string &name, std::uint32_t &mode, std::uint32_t &size);
void reset();

}
