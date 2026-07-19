#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "src/file_mapping/file_mapping.h"
#include "src/file_mapping/file_mapping_operations.h"
#include "src/file_mapping/file_mapping_rpc.h"

namespace {
  namespace fs = std::filesystem;

  bool
  execute_ok(const nlohmann::json &request,
    const file_mapping::operations::execution_context_t &context,
    nlohmann::json &response) {
    const auto parsed = file_mapping::rpc::parse_control_message(request.dump());
    if (!parsed.ok) {
      std::cerr << "parse failed: " << parsed.error << '\n';
      return false;
    }
    response = file_mapping::operations::execute_control_message(parsed, context);
    if (!response.value("ok", false)) {
      std::cerr << "operation failed: " << response.dump() << '\n';
      return false;
    }
    return true;
  }
}  // namespace

int
main() {
  auto roots = file_mapping::enumerate_host_roots();
  if (roots.empty()) {
    std::cerr << "no host roots were enumerated\n";
    return 1;
  }

  std::set<std::string> ids;
  for (const auto &mapping : roots) {
    if (!ids.insert(mapping.id).second || mapping.local_root.empty()) {
      std::cerr << "invalid or duplicate mapping: " << mapping.id << '\n';
      return 2;
    }
    std::cout << mapping.id << '=' << mapping.local_root.generic_string() << '\n';
  }

  const fs::path temp = fs::temp_directory_path();
  const auto root_it = std::find_if(roots.begin(), roots.end(), [&](const file_mapping::mapping_t &mapping) {
    std::error_code ec;
    const auto relative = fs::relative(temp, mapping.local_root, ec);
    return !ec && !relative.empty() && *relative.begin() != "..";
  });
  if (root_it == roots.end()) {
    std::cerr << "temporary directory is not under an enumerated root\n";
    return 3;
  }

  const auto unique = std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count());
  const fs::path smoke_dir = temp / ("foundation-file-transfer-smoke-" + unique);
  std::error_code ec;
  if (!fs::create_directory(smoke_dir, ec) || ec) {
    std::cerr << "failed to create smoke directory: " << ec.message() << '\n';
    return 4;
  }

  const auto cleanup = [&]() {
    std::error_code ignored;
    fs::remove_all(smoke_dir, ignored);
  };

  const std::string directory_path = fs::relative(smoke_dir, root_it->local_root).generic_string();
  const std::string file_path = directory_path + "/roundtrip.txt";
  file_mapping::operations::execution_context_t context;
  context.mappings = roots;
  nlohmann::json response;

  const nlohmann::json write_request {
    { "type", "write" },
    { "id", 1 },
    { "mapping", root_it->id },
    { "path", file_path },
    { "upload_id", "full-disk-smoke" },
    { "offset", 0 },
    { "total_size", 19 },
    { "data", "ZnVsbC1kaXNrLXJvdW5kdHJpcA==" },
    { "encoding", "base64" },
    { "begin", true },
    { "complete", true }
  };
  if (!execute_ok(write_request, context, response) || !response.value("completed", false)) {
    cleanup();
    return 5;
  }

  const nlohmann::json list_request {
    { "type", "list" },
    { "id", 2 },
    { "mapping", root_it->id },
    { "path", directory_path }
  };
  if (!execute_ok(list_request, context, response) || response["entries"].size() != 1) {
    cleanup();
    return 6;
  }

  const nlohmann::json read_request {
    { "type", "read" },
    { "id", 3 },
    { "mapping", root_it->id },
    { "path", file_path },
    { "offset", 0 },
    { "length", 64 }
  };
  if (!execute_ok(read_request, context, response) ||
      response.value("data", std::string {}) != "ZnVsbC1kaXNrLXJvdW5kdHJpcA==") {
    cleanup();
    return 7;
  }

  cleanup();
  std::cout << "full_disk_file_transfer_smoke=passed\n";
  return 0;
}
