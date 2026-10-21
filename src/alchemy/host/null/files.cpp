// Null host backend, area "files": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/files.hpp"

namespace alchemy::host::files {

Result<Handle> open(std::string_view, Access, Create, std::uint32_t) { return Error::unsupported; }
Result<std::size_t> read_at(Handle, Bytes, std::uint64_t) { return Error::unsupported; }
Result<std::size_t> write_at(Handle, ConstBytes, std::uint64_t) { return Error::unsupported; }
Result<std::uint64_t> size(Handle) { return Error::unsupported; }
Result<std::size_t> read(Handle, Bytes) { return Error::unsupported; }
Result<std::size_t> write(Handle, ConstBytes) { return Error::unsupported; }
Result<Attributes> attributes(std::string_view) { return Error::unsupported; }
Result<Times> times(std::string_view) { return Error::unsupported; }
Result<Volume> volume(std::string_view) { return Error::unsupported; }
Result<std::vector<std::string>> volume_roots() { return Error::unsupported; }
Result<std::string> create_temporary(std::string_view) { return Error::unsupported; }
Status replace(std::string_view, std::string_view) { return Error::unsupported; }
Result<Handle> open_metadata(std::string_view) { return Error::unsupported; }
Result<Info> info(Handle) { return Error::unsupported; }
Result<std::string> final_path(Handle) { return Error::unsupported; }
Result<Handle> watch_directory(std::string_view) { return Error::unsupported; }
Result<bool> directory_changed(Handle) { return Error::unsupported; }
void close_watch(Handle) noexcept {}
Result<std::unique_ptr<TreeWatch>> watch_tree(std::string_view) { return Error::unsupported; }
Result<Handle> open_null(Access) { return Error::unsupported; }
Result<std::uint64_t> seek(Handle, std::int64_t, Whence) { return Error::unsupported; }
Status set_size(Handle, std::uint64_t) { return Error::unsupported; }
Status flush(Handle) { return Error::unsupported; }
Status sync_directory(std::string_view) { return Error::unsupported; }
bool is_character_device(Handle) noexcept { return false; }
Kind kind(Handle) noexcept { return Kind::unknown; }
Status remove_open(Handle) { return Error::unsupported; }
Status create_directory(std::string_view, bool) { return Error::unsupported; }
Status set_read_only(std::string_view, bool) { return Error::unsupported; }
Result<Pipe> create_pipe() { return Error::unsupported; }
Result<std::size_t> pipe_available(Handle) { return Error::unsupported; }
Result<Metadata> metadata(std::string_view, bool) { return Error::unsupported; }
Result<Metadata> metadata(Handle) { return Error::unsupported; }
Result<Identity> identity(Handle) { return Error::unsupported; }
Status lock(Handle, std::uint64_t, std::uint64_t, bool) { return Error::unsupported; }
Status unlock(Handle, std::uint64_t, std::uint64_t) { return Error::unsupported; }
Error error(const std::error_code& code) noexcept { return code ? Error::other : Error::none; }
Status set_times(std::string_view, std::optional<std::int64_t>, std::optional<std::int64_t>) { return Error::unsupported; }
Status rename(std::string_view, std::string_view, bool) { return Error::unsupported; }
Status access(std::string_view, bool) { return Error::unsupported; }
bool is_console(Handle) noexcept { return false; }
Result<std::vector<std::byte>> get_attribute(std::string_view, std::string_view) { return Error::unsupported; }
Result<bool> has_attribute(std::string_view, std::string_view) { return Error::unsupported; }
Status set_attribute(std::string_view, std::string_view, ConstBytes) { return Error::unsupported; }
Status remove_attribute(std::string_view, std::string_view) { return Error::unsupported; }
Result<std::vector<std::string>> list_attributes(std::string_view) { return Error::unsupported; }
Status create_symbolic_link(std::string_view, std::string_view, bool) { return Error::unsupported; }
Status create_junction(std::string_view, std::string_view) { return Error::unsupported; }
Status create_hard_link(std::string_view, std::string_view) { return Error::unsupported; }
Status remove_tree(std::string_view) { return Error::unsupported; }

} // namespace alchemy::host::files
