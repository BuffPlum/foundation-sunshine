/**
 * @file src/file_mapping_operations.cpp
 * @brief File mapping RPC execution helpers.
 */
#include "file_mapping_operations.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace file_mapping::operations {
  namespace {
    namespace fs = std::filesystem;

    std::uint64_t
    request_id(const nlohmann::json &body) {
      if (!body.contains("id")) {
        return 0;
      }
      if (body["id"].is_number_unsigned()) {
        return body["id"].get<std::uint64_t>();
      }
      if (body["id"].is_number_integer()) {
        const auto id = body["id"].get<std::int64_t>();
        return id < 0 ? 0 : static_cast<std::uint64_t>(id);
      }
      return 0;
    }

    nlohmann::json
    error_response(const nlohmann::json &body, std::string code, std::string message) {
      return rpc::make_error(request_id(body), std::move(code), std::move(message));
    }

    nlohmann::json
    result_response(const nlohmann::json &body) {
      return {
        { "type", "result" },
        { "id", request_id(body) },
        { "ok", true }
      };
    }

    std::vector<mapping_t>
    current_mappings(const execution_context_t &context) {
      return context.mapping_provider ? context.mapping_provider() : context.mappings;
    }

    const mapping_t *
    find_mapping(const std::vector<mapping_t> &mappings, const std::string &id) {
      auto it = std::find_if(mappings.begin(), mappings.end(), [&](const mapping_t &mapping) {
        return mapping.id == id;
      });
      return it == mappings.end() ? nullptr : &*it;
    }

    bool
    client_allowed(const mapping_t &mapping, const std::string &peer_uuid) {
      return mapping.clients.empty() || std::find(mapping.clients.begin(), mapping.clients.end(), peer_uuid) != mapping.clients.end();
    }

    std::string
    resolve_error_code(resolve_error_e error) {
      switch (error) {
        case resolve_error_e::invalid_mapping_id:
          return "invalid_mapping_id";
        case resolve_error_e::invalid_root:
          return "invalid_root";
        case resolve_error_e::absolute_path:
          return "absolute_path";
        case resolve_error_e::invalid_relative_path:
          return "invalid_path";
        case resolve_error_e::reserved_name:
          return "reserved_name";
        case resolve_error_e::path_escape:
          return "path_escape";
        case resolve_error_e::reparse_point_blocked:
          return "reparse_point_blocked";
        case resolve_error_e::not_found:
          return "not_found";
        case resolve_error_e::filesystem_error:
          return "filesystem_error";
        case resolve_error_e::none:
          break;
      }
      return "resolve_error";
    }

    std::string
    file_kind(const fs::directory_entry &entry, std::error_code &ec) {
      if (entry.is_directory(ec)) {
        return "directory";
      }
      ec.clear();
      if (entry.is_regular_file(ec)) {
        return "file";
      }
      ec.clear();
      return "other";
    }

    std::string
    file_kind(const fs::path &path, std::error_code &ec) {
      if (fs::is_directory(path, ec)) {
        return "directory";
      }
      ec.clear();
      if (fs::is_regular_file(path, ec)) {
        return "file";
      }
      ec.clear();
      return "other";
    }

    std::uint64_t
    file_size_or_zero(const fs::path &path, std::error_code &ec) {
      auto size = fs::file_size(path, ec);
      if (ec) {
        ec.clear();
        return 0;
      }
      return static_cast<std::uint64_t>(size);
    }

    std::string
    base64_encode(const std::vector<unsigned char> &bytes) {
      static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve(((bytes.size() + 2) / 3) * 4);

      for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto b0 = bytes[i];
        const auto b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const auto b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;

        out.push_back(alphabet[(b0 >> 2) & 0x3f]);
        out.push_back(alphabet[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0f)]);
        out.push_back(i + 1 < bytes.size() ? alphabet[((b1 & 0x0f) << 2) | ((b2 >> 6) & 0x03)] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[b2 & 0x3f] : '=');
      }

      return out;
    }

    std::optional<std::vector<unsigned char>>
    base64_decode(std::string_view text) {
      auto decode_char = [](unsigned char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
      };

      if (text.size() % 4 != 0) {
        return std::nullopt;
      }

      std::vector<unsigned char> out;
      out.reserve((text.size() / 4) * 3);
      for (std::size_t i = 0; i < text.size(); i += 4) {
        const auto c0 = decode_char(static_cast<unsigned char>(text[i]));
        const auto c1 = decode_char(static_cast<unsigned char>(text[i + 1]));
        const auto c2 = text[i + 2] == '=' ? -2 : decode_char(static_cast<unsigned char>(text[i + 2]));
        const auto c3 = text[i + 3] == '=' ? -2 : decode_char(static_cast<unsigned char>(text[i + 3]));
        if (c0 < 0 || c1 < 0 || c2 == -1 || c3 == -1 ||
            (c2 == -2 && c3 != -2) ||
            (c2 == -2 && i + 4 != text.size()) ||
            (c3 == -2 && i + 4 != text.size())) {
          return std::nullopt;
        }

        out.push_back(static_cast<unsigned char>((c0 << 2) | (c1 >> 4)));
        if (c2 != -2) {
          out.push_back(static_cast<unsigned char>(((c1 & 0x0f) << 4) | (c2 >> 2)));
        }
        if (c3 != -2) {
          out.push_back(static_cast<unsigned char>(((c2 & 0x03) << 6) | c3));
        }
      }
      return out;
    }

    bool
    is_valid_upload_id(std::string_view id) {
      return !id.empty() && id.size() <= 96 &&
             std::all_of(id.begin(), id.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '-' || ch == '_';
             });
    }

    bool
    is_upload_staging_name(std::string_view name) {
      return name.starts_with(".moonlight-upload-") && name.ends_with(".part");
    }

    fs::path
    upload_staging_path(const fs::path &parent, std::string_view upload_id, std::string_view remote_path) {
      const auto path_hash = std::hash<std::string_view> {}(remote_path);
      return parent / (".moonlight-upload-" + std::string { upload_id } + "-" + std::to_string(path_hash) + ".part");
    }

    void
    cleanup_stale_uploads(const fs::path &parent) {
      std::error_code ec;
      const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
      for (const auto &entry : fs::directory_iterator(parent, ec)) {
        if (ec) {
          return;
        }
        const auto name = entry.path().filename().generic_string();
        if (!is_upload_staging_name(name) || !entry.is_regular_file(ec)) {
          ec.clear();
          continue;
        }
        const auto modified = entry.last_write_time(ec);
        if (!ec && modified < cutoff) {
          fs::remove(entry.path(), ec);
        }
        ec.clear();
      }
    }

    bool
    require_string_field(const nlohmann::json &body, const char *name, std::string &out) {
      if (!body.contains(name) || !body[name].is_string()) {
        return false;
      }
      out = body[name].get<std::string>();
      return true;
    }

    bool
    optional_string_field(const nlohmann::json &body, const char *name, std::string &out, nlohmann::json &error) {
      if (!body.contains(name)) {
        out.clear();
        return true;
      }
      if (!body[name].is_string()) {
        error = error_response(body, "bad_request", std::string("field must be a string: ") + name);
        return false;
      }
      out = body[name].get<std::string>();
      return true;
    }

    bool
    optional_uint64_field(const nlohmann::json &body, const char *name, std::uint64_t fallback, std::uint64_t &out, nlohmann::json &error) {
      if (!body.contains(name)) {
        out = fallback;
        return true;
      }
      if (body[name].is_number_unsigned()) {
        out = body[name].get<std::uint64_t>();
        return true;
      }
      if (body[name].is_number_integer()) {
        const auto value = body[name].get<std::int64_t>();
        if (value >= 0) {
          out = static_cast<std::uint64_t>(value);
          return true;
        }
      }
      error = error_response(body, "bad_request", std::string("field must be a non-negative integer: ") + name);
      return false;
    }

    bool
    optional_bool_field(const nlohmann::json &body, const char *name, bool fallback, bool &out, nlohmann::json &error) {
      if (!body.contains(name)) {
        out = fallback;
        return true;
      }
      if (!body[name].is_boolean()) {
        error = error_response(body, "bad_request", std::string("field must be a boolean: ") + name);
        return false;
      }
      out = body[name].get<bool>();
      return true;
    }

    enum class conflict_policy_e {
      reject,
      overwrite,
      keep_both
    };

    bool
    optional_conflict_policy(const nlohmann::json &body, conflict_policy_e &out, nlohmann::json &error) {
      std::string value = "reject";
      if (body.contains("conflict_policy")) {
        if (!body["conflict_policy"].is_string()) {
          error = error_response(body, "bad_request", "field must be a string: conflict_policy");
          return false;
        }
        value = body["conflict_policy"].get<std::string>();
      }

      if (value == "reject") {
        out = conflict_policy_e::reject;
      }
      else if (value == "overwrite") {
        out = conflict_policy_e::overwrite;
      }
      else if (value == "keep_both") {
        out = conflict_policy_e::keep_both;
      }
      else {
        error = error_response(body, "bad_request", "conflict_policy must be reject, overwrite, or keep_both");
        return false;
      }
      return true;
    }

    fs::path
    unique_sibling_path(const fs::path &requested) {
      const auto parent = requested.parent_path();
      const auto extension = requested.extension();
      const auto stem = requested.stem();

      // Match Windows Explorer's familiar "name (1).ext" convention. The
      // lookup happens on the host immediately before commit so two clients
      // cannot accidentally choose the same suffix based on a stale listing.
      std::error_code ec;
      for (std::uint32_t index = 1; index < std::numeric_limits<std::uint32_t>::max(); ++index) {
        const auto filename = stem.native() +
                              fs::path(" (" + std::to_string(index) + ")").native() +
                              extension.native();
        auto candidate = parent / filename;
        const bool exists = fs::exists(candidate, ec);
        // Permission and I/O errors must not turn into a practically infinite
        // suffix search. Return failure and let the RPC surface the filesystem
        // problem to the client.
        if (ec) {
          return {};
        }
        if (!exists) {
          return candidate;
        }
      }
      return {};
    }

    std::string
    remote_path_with_filename(std::string_view requested, const fs::path &actual) {
      const auto slash = requested.find_last_of('/');
      const auto filename = actual.filename().generic_string();
      return slash == std::string_view::npos ?
               filename :
               std::string { requested.substr(0, slash + 1) } + filename;
    }

    bool
    commit_upload(const fs::path &staging, const fs::path &destination, bool overwrite, std::error_code &ec) {
#ifdef _WIN32
      if (overwrite) {
        // std::filesystem::rename() cannot replace an existing file on
        // Windows. MoveFileExW preserves the staging-file transaction while
        // explicitly opting into replacement.
        if (MoveFileExW(staging.c_str(),
                        destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
          ec.clear();
          return true;
        }
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
      }
#endif
      fs::rename(staging, destination, ec);
      return !ec;
    }

    std::optional<mapping_t>
    require_mapping(const nlohmann::json &body, const execution_context_t &context, nlohmann::json &error) {
      std::string mapping_id;
      if (!require_string_field(body, "mapping", mapping_id)) {
        error = error_response(body, "bad_request", "missing string field: mapping");
        return std::nullopt;
      }

      auto mappings = current_mappings(context);
      const auto *mapping = find_mapping(mappings, mapping_id);
      if (!mapping) {
        error = error_response(body, "mapping_not_found", "mapping was not found");
        return std::nullopt;
      }
      if (!client_allowed(*mapping, context.peer_uuid)) {
        error = error_response(body, "forbidden", "client is not allowed to access mapping");
        return std::nullopt;
      }
      return *mapping;
    }

    nlohmann::json
    execute_list(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }

      std::string remote_path;
      if (!optional_string_field(body, "path", remote_path, error)) {
        return error;
      }
      auto resolved = resolve_path(*mapping, remote_path, true);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      if (!fs::is_directory(resolved.resolved_path, ec)) {
        return error_response(body, "not_directory", "path is not a directory");
      }

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["entries"] = nlohmann::json::array();
      out["truncated"] = false;

      std::uint32_t count = 0;
      for (const auto &entry : fs::directory_iterator(resolved.resolved_path, ec)) {
        if (ec) {
          return error_response(body, "filesystem_error", ec.message());
        }
        if (context.max_list_entries != 0 && count >= context.max_list_entries) {
          out["truncated"] = true;
          break;
        }

        std::error_code entry_ec;
        if (is_upload_staging_name(entry.path().filename().generic_string())) {
          continue;
        }
        const auto kind = file_kind(entry, entry_ec);
        const auto name = entry.path().filename().generic_string();
        nlohmann::json item {
          { "name", name },
          { "kind", kind }
        };
        if (kind == "file") {
          item["size"] = file_size_or_zero(entry.path(), entry_ec);
        }
        out["entries"].push_back(std::move(item));
        ++count;
      }
      if (ec) {
        return error_response(body, "filesystem_error", ec.message());
      }

      return out;
    }

    nlohmann::json
    execute_stat(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }

      std::string remote_path;
      if (!optional_string_field(body, "path", remote_path, error)) {
        return error;
      }
      auto resolved = resolve_path(*mapping, remote_path, true);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      const auto kind = file_kind(resolved.resolved_path, ec);
      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["kind"] = kind;
      if (kind == "file") {
        out["size"] = file_size_or_zero(resolved.resolved_path, ec);
      }
      return out;
    }

    nlohmann::json
    execute_read(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }

      std::string remote_path;
      if (!optional_string_field(body, "path", remote_path, error)) {
        return error;
      }
      auto resolved = resolve_path(*mapping, remote_path, true);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      if (!fs::is_regular_file(resolved.resolved_path, ec)) {
        return error_response(body, "not_file", "path is not a regular file");
      }

      const auto total_size = file_size_or_zero(resolved.resolved_path, ec);
      if (mapping->max_file_size != 0 && total_size > mapping->max_file_size) {
        return error_response(body, "file_too_large", "file exceeds mapping max_file_size");
      }

      std::uint64_t offset = 0;
      if (!optional_uint64_field(body, "offset", 0, offset, error)) {
        return error;
      }
      std::uint64_t requested_length = 64 * 1024;
      if (!optional_uint64_field(body, "length", requested_length, requested_length, error)) {
        return error;
      }
      if (requested_length > std::numeric_limits<std::uint32_t>::max()) {
        return error_response(body, "bad_request", "read length is too large");
      }
      auto length = static_cast<std::uint32_t>(requested_length);
      length = std::min(length, context.max_read_bytes);
      if (length == 0) {
        return error_response(body, "bad_request", "read length must be non-zero");
      }

      std::ifstream in(resolved.resolved_path, std::ios::binary);
      if (!in) {
        return error_response(body, "open_failed", "failed to open file for reading");
      }

      in.seekg(static_cast<std::streamoff>(offset));
      if (!in && !in.eof()) {
        return error_response(body, "seek_failed", "failed to seek file");
      }

      std::vector<unsigned char> bytes(length);
      in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      const auto bytes_read = static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount()));
      bytes.resize(bytes_read);

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["offset"] = offset;
      out["bytes_read"] = bytes_read;
      out["total_size"] = total_size;
      out["eof"] = offset + bytes_read >= total_size;
      out["encoding"] = "base64";
      out["data"] = base64_encode(bytes);
      return out;
    }

    nlohmann::json
    execute_mkdir(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }
      if (mapping->mode != access_mode_e::readwrite) {
        return error_response(body, "read_only", "mapping does not allow uploads");
      }

      std::string remote_path;
      if (!require_string_field(body, "path", remote_path) || remote_path.empty()) {
        return error_response(body, "bad_request", "missing non-empty string field: path");
      }
      auto resolved = resolve_path(*mapping, remote_path, false);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      conflict_policy_e conflict_policy;
      if (!optional_conflict_policy(body, conflict_policy, error)) {
        return error;
      }
      if (fs::exists(resolved.resolved_path, ec)) {
        if (fs::is_directory(resolved.resolved_path, ec) &&
            conflict_policy != conflict_policy_e::keep_both) {
          auto out = result_response(body);
          out["mapping"] = mapping->id;
          out["path"] = remote_path;
          out["created"] = false;
          return out;
        }
        if (conflict_policy == conflict_policy_e::keep_both) {
          resolved.resolved_path = unique_sibling_path(resolved.resolved_path);
          if (resolved.resolved_path.empty()) {
            return error_response(body, "filesystem_error", "could not allocate a unique directory name");
          }
          remote_path = remote_path_with_filename(remote_path, resolved.resolved_path);
        }
        else {
          return error_response(body, "already_exists", "a non-directory item already exists at path");
        }
      }
      ec.clear();

      const auto parent = resolved.resolved_path.parent_path();
      if (!fs::is_directory(parent, ec)) {
        return error_response(body, "parent_not_found", "parent directory does not exist");
      }
      if (!fs::create_directory(resolved.resolved_path, ec) || ec) {
        return error_response(body, "filesystem_error", ec ? ec.message() : "failed to create directory");
      }

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["created"] = true;
      return out;
    }

    nlohmann::json
    execute_rename(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }
      if (mapping->mode != access_mode_e::readwrite) {
        return error_response(body, "read_only", "mapping does not allow file changes");
      }

      std::string remote_path;
      std::string destination_path;
      if (!require_string_field(body, "path", remote_path) || remote_path.empty() ||
          !require_string_field(body, "destination_path", destination_path) || destination_path.empty()) {
        return error_response(body, "bad_request", "rename requires non-empty path and destination_path");
      }

      auto source = resolve_path(*mapping, remote_path, true);
      auto destination = resolve_path(*mapping, destination_path, false);
      if (!source.ok) {
        return error_response(body, resolve_error_code(source.error), source.message);
      }
      if (!destination.ok) {
        return error_response(body, resolve_error_code(destination.error), destination.message);
      }

      std::error_code ec;
      const auto root = fs::weakly_canonical(mapping->local_root, ec);
      if (ec) {
        return error_response(body, "filesystem_error", ec.message());
      }
      if (source.resolved_path == root || destination.resolved_path == root) {
        return error_response(body, "bad_request", "a mapping root cannot be renamed");
      }
      if (fs::exists(destination.resolved_path, ec)) {
        return error_response(body, "already_exists", "rename destination already exists");
      }
      ec.clear();
      if (!fs::is_directory(destination.resolved_path.parent_path(), ec)) {
        return error_response(body, "parent_not_found", "rename destination parent does not exist");
      }
      ec.clear();

      fs::rename(source.resolved_path, destination.resolved_path, ec);
      if (ec) {
        return error_response(body, "filesystem_error", ec.message());
      }

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = destination_path;
      return out;
    }

    nlohmann::json
    execute_remove(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }
      if (mapping->mode != access_mode_e::readwrite || !mapping->allow_delete) {
        return error_response(body, "delete_not_allowed", "mapping does not allow deletion");
      }

      std::string remote_path;
      bool recursive = false;
      if (!require_string_field(body, "path", remote_path) || remote_path.empty()) {
        return error_response(body, "bad_request", "delete requires a non-empty path");
      }
      if (!optional_bool_field(body, "recursive", false, recursive, error)) {
        return error;
      }

      auto resolved = resolve_path(*mapping, remote_path, true);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      const auto root = fs::weakly_canonical(mapping->local_root, ec);
      if (ec) {
        return error_response(body, "filesystem_error", ec.message());
      }
      if (resolved.resolved_path == root || is_upload_staging_name(resolved.resolved_path.filename().generic_string())) {
        return error_response(body, "bad_request", "a mapping root or upload staging file cannot be deleted");
      }

      std::uintmax_t removed = 0;
      if (recursive && fs::is_directory(resolved.resolved_path, ec)) {
        removed = fs::remove_all(resolved.resolved_path, ec);
      }
      else {
        removed = fs::remove(resolved.resolved_path, ec) ? 1 : 0;
      }
      if (ec) {
        return error_response(body, "filesystem_error", ec.message());
      }

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["removed"] = removed;
      return out;
    }

    nlohmann::json
    execute_write(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }
      if (mapping->mode != access_mode_e::readwrite) {
        return error_response(body, "read_only", "mapping does not allow uploads");
      }

      std::string remote_path;
      std::string upload_id;
      std::string encoded;
      if (!require_string_field(body, "path", remote_path) || remote_path.empty()) {
        return error_response(body, "bad_request", "missing non-empty string field: path");
      }
      if (!require_string_field(body, "upload_id", upload_id) || !is_valid_upload_id(upload_id)) {
        return error_response(body, "bad_request", "upload_id must contain only letters, digits, '-' or '_'");
      }
      if (!require_string_field(body, "data", encoded)) {
        return error_response(body, "bad_request", "missing string field: data");
      }

      auto bytes = base64_decode(encoded);
      if (!bytes) {
        return error_response(body, "bad_request", "data is not valid base64");
      }
      if (bytes->size() > context.max_write_bytes) {
        return error_response(body, "chunk_too_large", "write chunk exceeds the configured limit");
      }

      std::uint64_t offset = 0;
      std::uint64_t total_size = 0;
      bool begin = false;
      bool complete = false;
      conflict_policy_e conflict_policy;
      if (!body.contains("total_size")) {
        return error_response(body, "bad_request", "missing non-negative integer field: total_size");
      }
      if (!optional_uint64_field(body, "offset", 0, offset, error) ||
          !optional_uint64_field(body, "total_size", 0, total_size, error) ||
          !optional_bool_field(body, "begin", false, begin, error) ||
          !optional_bool_field(body, "complete", false, complete, error) ||
          !optional_conflict_policy(body, conflict_policy, error)) {
        return error;
      }
      if (begin && offset != 0) {
        return error_response(body, "bad_request", "the first upload chunk must start at offset zero");
      }
      if (mapping->max_file_size != 0 &&
          (total_size > mapping->max_file_size ||
            offset > mapping->max_file_size ||
            bytes->size() > mapping->max_file_size - offset)) {
        return error_response(body, "file_too_large", "upload exceeds mapping max_file_size");
      }
      if (total_size != 0 && offset + bytes->size() > total_size) {
        return error_response(body, "bad_request", "write chunk exceeds declared total_size");
      }

      auto resolved = resolve_path(*mapping, remote_path, false);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }
      std::error_code root_ec;
      const auto canonical_root = fs::weakly_canonical(mapping->local_root, root_ec);
      if (root_ec) {
        return error_response(body, "filesystem_error", root_ec.message());
      }
      if (resolved.resolved_path == canonical_root) {
        return error_response(body, "bad_request", "upload path must name a file");
      }
      if (is_upload_staging_name(resolved.resolved_path.filename().generic_string())) {
        return error_response(body, "invalid_path", "upload path uses a reserved staging file name");
      }

      std::error_code ec;
      const auto parent = resolved.resolved_path.parent_path();
      if (!fs::is_directory(parent, ec)) {
        return error_response(body, "parent_not_found", "parent directory does not exist");
      }
      ec.clear();

      if (begin) {
        if (fs::exists(resolved.resolved_path, ec)) {
          if (conflict_policy == conflict_policy_e::reject) {
            return error_response(body, "already_exists", "destination file already exists");
          }
          if (!fs::is_regular_file(resolved.resolved_path, ec) &&
              conflict_policy == conflict_policy_e::overwrite) {
            return error_response(body, "already_exists", "overwrite destination is not a regular file");
          }
          if (conflict_policy == conflict_policy_e::keep_both) {
            resolved.resolved_path = unique_sibling_path(resolved.resolved_path);
            if (resolved.resolved_path.empty()) {
              return error_response(body, "filesystem_error", "could not allocate a unique file name");
            }
            remote_path = remote_path_with_filename(remote_path, resolved.resolved_path);
          }
        }
        ec.clear();
        const auto actual_staging = upload_staging_path(
          resolved.resolved_path.parent_path(), upload_id, remote_path);
        cleanup_stale_uploads(parent);
        fs::remove(actual_staging, ec);
        ec.clear();
        std::ofstream create(actual_staging, std::ios::binary | std::ios::trunc);
        if (!create) {
          return error_response(body, "open_failed", "failed to create upload staging file");
        }
      }
      const auto actual_staging = upload_staging_path(
        resolved.resolved_path.parent_path(), upload_id, remote_path);
      if (!begin && !fs::is_regular_file(actual_staging, ec)) {
        return error_response(body, "upload_not_found", "upload staging file was not found");
      }
      ec.clear();

      const auto staging_size = file_size_or_zero(actual_staging, ec);
      if (staging_size != offset) {
        return error_response(body, "offset_mismatch", "write offset does not match received upload size");
      }

      std::ofstream out_file(actual_staging, std::ios::binary | std::ios::app);
      if (!out_file) {
        return error_response(body, "open_failed", "failed to open upload staging file");
      }
      if (!bytes->empty()) {
        out_file.write(reinterpret_cast<const char *>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
      }
      out_file.flush();
      if (!out_file) {
        return error_response(body, "write_failed", "failed to write upload chunk");
      }
      out_file.close();

      const auto next_offset = offset + bytes->size();
      if (complete) {
        if (total_size != 0 && next_offset != total_size) {
          fs::remove(actual_staging, ec);
          return error_response(body, "size_mismatch", "received size does not match declared total_size");
        }
        if (fs::exists(resolved.resolved_path, ec)) {
          if (conflict_policy == conflict_policy_e::reject) {
            ec.clear();
            fs::remove(actual_staging, ec);
            return error_response(body, "already_exists", "destination file already exists");
          }
          if (conflict_policy == conflict_policy_e::keep_both) {
            resolved.resolved_path = unique_sibling_path(resolved.resolved_path);
            if (resolved.resolved_path.empty()) {
              fs::remove(actual_staging, ec);
              return error_response(body, "filesystem_error", "could not allocate a unique file name");
            }
            remote_path = remote_path_with_filename(remote_path, resolved.resolved_path);
          }
          else if (!fs::is_regular_file(resolved.resolved_path, ec)) {
            fs::remove(actual_staging, ec);
            return error_response(body, "already_exists", "overwrite destination is not a regular file");
          }
        }
        ec.clear();
        if (!commit_upload(actual_staging,
                           resolved.resolved_path,
                           conflict_policy == conflict_policy_e::overwrite,
                           ec)) {
          return error_response(body, "commit_failed", ec.message());
        }
      }

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["upload_id"] = upload_id;
      out["bytes_written"] = bytes->size();
      out["next_offset"] = next_offset;
      out["completed"] = complete;
      return out;
    }
  }  // namespace

  nlohmann::json
  mapping_to_json(const mapping_t &mapping) {
    auto capabilities = nlohmann::json::array({ "list", "stat", "read" });
    if (mapping.mode == access_mode_e::readwrite) {
      capabilities.push_back("write");
      capabilities.push_back("mkdir");
      capabilities.push_back("rename");
      if (mapping.allow_delete) {
        capabilities.push_back("delete");
      }
    }
    return {
      { "id", mapping.id },
      { "name", mapping.name },
      { "volume_label", mapping.volume_label },
      { "side", "host" },
      { "mode", mapping.mode == access_mode_e::read ? "read" : "readwrite" },
      { "capabilities", std::move(capabilities) }
    };
  }

  nlohmann::json
  execute_control_message(const rpc::parse_result_t &message, const execution_context_t &context) {
    switch (message.type) {
      case rpc::message_type_e::list:
        return execute_list(message, context);
      case rpc::message_type_e::stat:
        return execute_stat(message, context);
      case rpc::message_type_e::read:
        return execute_read(message, context);
      case rpc::message_type_e::write:
        return execute_write(message, context);
      case rpc::message_type_e::mkdir:
        return execute_mkdir(message, context);
      case rpc::message_type_e::rename:
        return execute_rename(message, context);
      case rpc::message_type_e::remove:
        return execute_remove(message, context);
      default:
        return error_response(message.body, "unsupported_operation", "file mapping operation is not supported");
    }
  }
}  // namespace file_mapping::operations
