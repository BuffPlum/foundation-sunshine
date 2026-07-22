/**
 * @file src/file_mapping.h
 * @brief Core directory mapping types and path safety helpers.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace file_mapping {
  enum class sharing_mode_e {
    read_only,
    full_disk
  };

  enum class access_mode_e {
    read,
    readwrite
  };

  enum class resolve_error_e {
    none,
    invalid_mapping_id,
    invalid_root,
    absolute_path,
    invalid_relative_path,
    reserved_name,
    path_escape,
    reparse_point_blocked,
    not_found,
    filesystem_error
  };

  struct mapping_t {
    std::string id;
    std::string name;
    std::string volume_label;
    std::filesystem::path local_root;
    access_mode_e mode = access_mode_e::read;
    bool allow_delete = false;
    bool allow_execute = false;
    bool follow_reparse_points = false;
    std::uintmax_t max_file_size = 0;
    std::vector<std::string> clients;
  };

  struct resolve_result_t {
    bool ok = false;
    resolve_error_e error = resolve_error_e::none;
    std::filesystem::path resolved_path;
    std::string message;
  };

  bool
  is_valid_mapping_id(std::string_view id);

  bool
  is_reserved_windows_name(std::string_view segment);

  bool
  is_safe_relative_path(std::string_view remote_path, std::string *message = nullptr);

  std::optional<sharing_mode_e>
  parse_sharing_mode(std::string_view mode);

  std::string_view
  sharing_mode_name(sharing_mode_e mode);

  void
  set_sharing_mode(sharing_mode_e mode);

  sharing_mode_e
  sharing_mode();

  /**
   * Enumerate filesystem roots for the explicitly enabled BuffPlum full-disk
   * mode. On Windows, each currently mounted drive is exposed as an independent
   * mapping (drive-c, drive-d, ...). The default read-only mode never calls
   * this provider and continues to use the upstream authorized mapping store.
   */
  std::vector<mapping_t>
  enumerate_host_roots();

  resolve_result_t
  resolve_path(const mapping_t &mapping, std::string_view remote_path, bool must_exist = false);
}  // namespace file_mapping
