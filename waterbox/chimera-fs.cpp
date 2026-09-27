// The filesystem Azahar's FileUtil stands on in this core (see patch 0005 and
// extern/azahar/src/common/chimera_fs.h for the interface).
//
// Two kinds of file:
//
//  * what the machine writes - the NAND, the SD card, save data, system data -
//    lives in an in-memory tree, in the machine's own memory. A savestate
//    carries all of it, a rewind takes it back, and the native and sandboxed
//    flavors hold the same bytes by construction.
//
//  * the project's files - the game, aes_keys.txt, seeddb.bin - are mounted
//    read-only by the frontend. Each is opened ONCE, at Init, in a fixed order,
//    and every stream on it reads through that one descriptor at an explicit
//    offset. A descriptor opened later, or one per stream, is the #118 bug:
//    the stream lives in the machine's memory and a savestate carries its
//    number, but the sandbox's open files are not in any savestate, so after a
//    load the number means another file or nothing. Opened at Init in the same
//    order every run, the numbers are the same in every host this machine is
//    ever loaded into, and nothing ever seeks them relative to a position a
//    savestate did not carry.
//
// A stream is a stdio FILE made with fopencookie(3), so FileUtil's fread,
// fwrite, fseeko and friends work on it unchanged. Directory listings come out
// sorted: a host folder lists in whatever order its filesystem likes, and a
// machine must not depend on that.
// SPDX-License-Identifier: MIT

#include "chimera-fs-host.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "common/chimera_fs.h"

namespace
{
struct Data
{
  std::vector<uint8_t> bytes;
  int host_fd = -1;  // a mounted project file, read-only
  uint64_t host_size = 0;
  uint64_t Size() const { return host_fd >= 0 ? host_size : bytes.size(); }
};

struct Node
{
  bool dir = false;
  std::shared_ptr<Data> data;  // files only
};

struct Stream
{
  std::shared_ptr<Data> data;
  uint64_t pos = 0;
  bool can_read = false, can_write = false, append = false;
};

// Constructed on first use: FileUtil can be reached from static initializers.
std::map<std::string, Node>& Tree()
{
  static std::map<std::string, Node>* tree = [] {
    auto* t = new std::map<std::string, Node>();
    (*t)["/"] = Node{true, nullptr};
    return t;
  }();
  return *tree;
}

std::map<std::FILE*, Stream*>& Streams()
{
  static auto* streams = new std::map<std::FILE*, Stream*>();
  return *streams;
}

// Absolute, single slashes, no trailing slash, "." and ".." resolved.
std::string Normalize(const std::string& in)
{
  std::vector<std::string> parts;
  std::string cur;
  auto flush = [&] {
    if (cur.empty() || cur == ".")
    {
    }
    else if (cur == "..")
    {
      if (!parts.empty())
        parts.pop_back();
    }
    else
      parts.push_back(cur);
    cur.clear();
  };
  for (char c : in)
  {
    if (c == '/' || c == '\\')
      flush();
    else
      cur += c;
  }
  flush();
  std::string out;
  for (const auto& p : parts)
    out += "/" + p;
  return out.empty() ? "/" : out;
}

std::string Parent(const std::string& norm)
{
  const auto slash = norm.rfind('/');
  return slash == 0 ? "/" : norm.substr(0, slash);
}

Node* Find(const std::string& norm)
{
  auto it = Tree().find(norm);
  return it == Tree().end() ? nullptr : &it->second;
}

bool ParentIsDir(const std::string& norm)
{
  const Node* p = Find(Parent(norm));
  return p && p->dir;
}

// A mounted file about to be written becomes the machine's own copy. Games
// never write their own image; this is for a firmware-provided file a machine
// changes, which is small.
bool MakeWritable(Data& d)
{
  if (d.host_fd < 0)
    return true;
  if (d.host_size > (64ull << 20))
  {
    errno = EROFS;
    return false;
  }
  std::vector<uint8_t> copy(d.host_size);
  uint64_t done = 0;
  while (done < d.host_size)
  {
    if (lseek(d.host_fd, static_cast<off_t>(done), SEEK_SET) < 0)
      return false;
    const ssize_t n = read(d.host_fd, copy.data() + done, d.host_size - done);
    if (n <= 0)
    {
      errno = EIO;
      return false;
    }
    done += static_cast<uint64_t>(n);
  }
  d.bytes = std::move(copy);
  d.host_fd = -1;  // the descriptor stays open for the project's lifetime
  d.host_size = 0;
  return true;
}

size_t ReadData(Data& d, uint64_t pos, void* buf, size_t size)
{
  const uint64_t total = d.Size();
  if (pos >= total)
    return 0;
  if (size > total - pos)
    size = static_cast<size_t>(total - pos);
  if (d.host_fd < 0)
  {
    std::memcpy(buf, d.bytes.data() + pos, size);
    return size;
  }
  size_t done = 0;
  while (done < size)
  {
    if (lseek(d.host_fd, static_cast<off_t>(pos + done), SEEK_SET) < 0)
      break;
    const ssize_t n = read(d.host_fd, static_cast<uint8_t*>(buf) + done, size - done);
    if (n <= 0)
      break;
    done += static_cast<size_t>(n);
  }
  return done;
}

ssize_t CookieRead(void* cookie, char* buf, size_t size)
{
  auto* s = static_cast<Stream*>(cookie);
  if (!s->can_read)
  {
    errno = EBADF;
    return -1;
  }
  const size_t n = ReadData(*s->data, s->pos, buf, size);
  s->pos += n;
  return static_cast<ssize_t>(n);
}

ssize_t CookieWrite(void* cookie, const char* buf, size_t size)
{
  auto* s = static_cast<Stream*>(cookie);
  if (!s->can_write || !MakeWritable(*s->data))
  {
    errno = EBADF;
    return 0;
  }
  auto& bytes = s->data->bytes;
  if (s->append)
    s->pos = bytes.size();
  if (s->pos + size > bytes.size())
    bytes.resize(s->pos + size);
  std::memcpy(bytes.data() + s->pos, buf, size);
  s->pos += size;
  return static_cast<ssize_t>(size);
}

int CookieSeek(void* cookie, off64_t* offset, int whence)
{
  auto* s = static_cast<Stream*>(cookie);
  int64_t base = 0;
  if (whence == SEEK_CUR)
    base = static_cast<int64_t>(s->pos);
  else if (whence == SEEK_END)
    base = static_cast<int64_t>(s->data->Size());
  const int64_t target = base + *offset;
  if (target < 0)
  {
    errno = EINVAL;
    return -1;
  }
  s->pos = static_cast<uint64_t>(target);
  *offset = target;
  return 0;
}

int CookieClose(void* cookie)
{
  auto* s = static_cast<Stream*>(cookie);
  for (auto it = Streams().begin(); it != Streams().end(); ++it)
  {
    if (it->second == s)
    {
      Streams().erase(it);
      break;
    }
  }
  delete s;
  return 0;
}

Stream* StreamOf(std::FILE* f)
{
  auto it = Streams().find(f);
  return it == Streams().end() ? nullptr : it->second;
}
}  // namespace

namespace ChimeraFS
{
int Stat(const std::string& path, struct stat* st)
{
  const Node* n = Find(Normalize(path));
  if (!n)
  {
    errno = ENOENT;
    return -1;
  }
  std::memset(st, 0, sizeof *st);
  st->st_mode = n->dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
  st->st_size = n->dir ? 0 : static_cast<off_t>(n->data->Size());
  st->st_nlink = 1;
  return 0;
}

int Unlink(const std::string& path)
{
  const std::string norm = Normalize(path);
  Node* n = Find(norm);
  if (!n)
  {
    errno = ENOENT;
    return -1;
  }
  if (n->dir)
  {
    errno = EISDIR;
    return -1;
  }
  Tree().erase(norm);  // an open stream keeps the data, as on POSIX
  return 0;
}

int Mkdir(const std::string& path)
{
  const std::string norm = Normalize(path);
  if (Find(norm))
  {
    errno = EEXIST;
    return -1;
  }
  if (!ParentIsDir(norm))
  {
    errno = ENOENT;
    return -1;
  }
  Tree()[norm] = Node{true, nullptr};
  return 0;
}

int Rmdir(const std::string& path)
{
  const std::string norm = Normalize(path);
  Node* n = Find(norm);
  if (!n)
  {
    errno = ENOENT;
    return -1;
  }
  if (!n->dir || norm == "/")
  {
    errno = n->dir ? EBUSY : ENOTDIR;
    return -1;
  }
  auto below = Tree().lower_bound(norm + "/");
  if (below != Tree().end() && below->first.compare(0, norm.size() + 1, norm + "/") == 0)
  {
    errno = ENOTEMPTY;
    return -1;
  }
  Tree().erase(norm);
  return 0;
}

int Rename(const std::string& from, const std::string& to)
{
  const std::string src = Normalize(from), dst = Normalize(to);
  Node* n = Find(src);
  if (!n)
  {
    errno = ENOENT;
    return -1;
  }
  if (src == dst)
    return 0;
  if (!ParentIsDir(dst))
  {
    errno = ENOENT;
    return -1;
  }
  if (Node* existing = Find(dst))
  {
    if (existing->dir != n->dir)
    {
      errno = existing->dir ? EISDIR : ENOTDIR;
      return -1;
    }
    if (existing->dir && List(dst) && !List(dst)->empty())
    {
      errno = ENOTEMPTY;
      return -1;
    }
    Tree().erase(dst);
    n = Find(src);
  }
  std::vector<std::pair<std::string, Node>> moved;
  moved.emplace_back(dst, *n);
  if (n->dir)
  {
    const std::string prefix = src + "/";
    for (auto it = Tree().lower_bound(prefix);
         it != Tree().end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
      moved.emplace_back(dst + it->first.substr(src.size()), it->second);
    Tree().erase(Tree().lower_bound(prefix), Tree().lower_bound(src + "0"));  // '0' follows '/'
  }
  Tree().erase(src);
  for (auto& m : moved)
    Tree()[m.first] = m.second;
  return 0;
}

std::optional<std::vector<std::string>> List(const std::string& dir)
{
  const std::string norm = Normalize(dir);
  const Node* n = Find(norm);
  if (!n || !n->dir)
    return std::nullopt;
  const std::string prefix = norm == "/" ? "/" : norm + "/";
  std::vector<std::string> out;
  for (auto it = Tree().lower_bound(prefix);
       it != Tree().end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
  {
    const std::string rest = it->first.substr(prefix.size());
    if (!rest.empty() && rest.find('/') == std::string::npos)
      out.push_back(rest);
  }
  return out;
}

std::FILE* Open(const std::string& path, const char* mode)
{
  const std::string norm = Normalize(path);
  const std::string m = mode ? mode : "r";
  const bool plus = m.find('+') != std::string::npos;
  const char kind = m.empty() ? 'r' : m[0];
  Node* n = Find(norm);
  if (n && n->dir)
  {
    errno = EISDIR;
    return nullptr;
  }
  if (kind == 'r' && !n)
  {
    errno = ENOENT;
    return nullptr;
  }
  if ((kind == 'w' || kind == 'a') && !n)
  {
    if (!ParentIsDir(norm))
    {
      errno = ENOENT;
      return nullptr;
    }
    Tree()[norm] = Node{false, std::make_shared<Data>()};
    n = Find(norm);
  }
  if (kind != 'r' && kind != 'w' && kind != 'a')
  {
    errno = EINVAL;
    return nullptr;
  }
  if (kind == 'w')
  {
    if (!MakeWritable(*n->data))
      return nullptr;
    n->data->bytes.clear();
  }
  auto* s = new Stream{n->data, 0, kind == 'r' || plus, kind != 'r' || plus, kind == 'a'};
  const cookie_io_functions_t io = {CookieRead, CookieWrite, CookieSeek, CookieClose};
  std::FILE* f = fopencookie(s, mode, io);
  if (!f)
  {
    delete s;
    return nullptr;
  }
  Streams()[f] = s;
  return f;
}

std::size_t ReadAt(std::FILE* f, void* data, std::size_t size, unsigned long long offset)
{
  Stream* s = StreamOf(f);
  if (!s)
    return 0;
  std::fflush(f);
  return ReadData(*s->data, offset, data, size);
}

int Truncate(std::FILE* f, unsigned long long size)
{
  Stream* s = StreamOf(f);
  if (!s || !s->can_write)
  {
    errno = EBADF;
    return -1;
  }
  std::fflush(f);
  if (!MakeWritable(*s->data))
    return -1;
  s->data->bytes.resize(size);
  return 0;
}
}  // namespace ChimeraFS

namespace ChimeraFSHost
{
bool MountFile(const std::string& path, const char* host_name)
{
  const std::string norm = Normalize(path);
  std::string dir = Parent(norm);
  // parents on the way, as a project's folder would have them
  std::vector<std::string> missing;
  while (!Find(dir))
  {
    missing.push_back(dir);
    dir = Parent(dir);
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it)
    Tree()[*it] = Node{true, nullptr};
  const int fd = open(host_name, O_RDONLY);
  if (fd < 0)
    return false;
  const off_t size = lseek(fd, 0, SEEK_END);
  if (size < 0)
  {
    close(fd);
    return false;
  }
  auto data = std::make_shared<Data>();
  data->host_fd = fd;
  data->host_size = static_cast<uint64_t>(size);
  Tree()[norm] = Node{false, data};
  return true;
}

bool WriteFile(const std::string& path, const void* bytes, size_t size)
{
  const std::string norm = Normalize(path);
  std::string dir = Parent(norm);
  std::vector<std::string> missing;
  while (!Find(dir))
  {
    missing.push_back(dir);
    dir = Parent(dir);
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it)
    Tree()[*it] = Node{true, nullptr};
  if (Node* n = Find(norm); n && n->dir)
    return false;
  auto data = std::make_shared<Data>();
  data->bytes.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + size);
  Tree()[norm] = Node{false, data};
  return true;
}

std::vector<std::string> Files(const std::string& under)
{
  const std::string norm = Normalize(under);
  const std::string prefix = norm == "/" ? "/" : norm + "/";
  std::vector<std::string> out;
  for (auto it = Tree().lower_bound(prefix);
       it != Tree().end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
    if (!it->second.dir)
      out.push_back(it->first);
  return out;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
  Node* n = Find(Normalize(path));
  if (!n || n->dir)
    return false;
  out.resize(n->data->Size());
  return ReadData(*n->data, 0, out.data(), out.size()) == out.size();
}
}  // namespace ChimeraFSHost
