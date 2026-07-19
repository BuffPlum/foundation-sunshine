/**
 * @file tests/unit/test_file_mapping_operations.cpp
 * @brief Test src/file_mapping_operations.*.
 */
#include <src/file_mapping/file_mapping_operations.h>
#include <src/file_mapping/file_mapping_store.h>

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;

  struct temp_tree_t {
    fs::path root;

    temp_tree_t():
        root(fs::temp_directory_path() / fs::path("sunshine_file_mapping_ops_test")) {
      std::error_code ec;
      fs::remove_all(root, ec);
      fs::create_directories(root / "nested");
      std::ofstream(root / "hello.txt", std::ios::binary) << "hello world";
      std::ofstream(root / "nested" / "child.txt", std::ios::binary) << "child";
    }

    ~temp_tree_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };

  file_mapping::operations::execution_context_t
  make_context(const fs::path &root) {
    file_mapping::mapping_t mapping;
    mapping.id = "host-test";
    mapping.name = "Host Test";
    mapping.local_root = root;
    mapping.clients = { "client-uuid" };

    file_mapping::operations::execution_context_t context;
    context.peer_uuid = "client-uuid";
    context.mappings.push_back(std::move(mapping));
    return context;
  }

  file_mapping::operations::execution_context_t
  make_writable_context(const fs::path &root) {
    auto context = make_context(root);
    context.mappings[0].mode = file_mapping::access_mode_e::readwrite;
    context.mappings[0].allow_delete = true;
    return context;
  }
}  // namespace

TEST(FileMappingOperations, ListsDirectoryEntries) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"list","id":1,"mapping":"host-test","path":""})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_TRUE(result["ok"].get<bool>());
  EXPECT_EQ(result["entries"].size(), 2);
}

TEST(FileMappingOperations, TruncatesLargeDirectoryListings) {
  temp_tree_t tree;
  std::ofstream(tree.root / "extra.txt", std::ios::binary) << "extra";
  auto context = make_context(tree.root);
  context.max_list_entries = 2;

  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"list","id":9,"mapping":"host-test","path":""})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_EQ(result["entries"].size(), 2);
  EXPECT_TRUE(result["truncated"].get<bool>());
}

TEST(FileMappingOperations, StatsFile) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"stat","id":2,"mapping":"host-test","path":"hello.txt"})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_EQ(result["kind"].get<std::string>(), "file");
  EXPECT_EQ(result["size"].get<int>(), 11);
}

TEST(FileMappingOperations, ReadsFileChunkAsBase64) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"read","id":3,"mapping":"host-test","path":"hello.txt","offset":6,"length":5})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_EQ(result["bytes_read"].get<int>(), 5);
  EXPECT_EQ(result["encoding"].get<std::string>(), "base64");
  EXPECT_EQ(result["data"].get<std::string>(), "d29ybGQ=");
  EXPECT_TRUE(result["eof"].get<bool>());
}

TEST(FileMappingOperations, RejectsFilesAboveMappingLimit) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  context.mappings[0].max_file_size = 4;
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"read","id":4,"mapping":"host-test","path":"hello.txt","offset":0,"length":5})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "error");
  EXPECT_EQ(result["code"].get<std::string>(), "file_too_large");
}

TEST(FileMappingOperations, RejectsMalformedRequestFields) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto bad_path = file_mapping::rpc::parse_control_message(R"({"type":"list","id":"bad","mapping":"host-test","path":7})");
  ASSERT_TRUE(bad_path.ok) << bad_path.error;

  auto path_result = file_mapping::operations::execute_control_message(bad_path, context);
  EXPECT_EQ(path_result["type"].get<std::string>(), "error");
  EXPECT_EQ(path_result["id"].get<int>(), 0);
  EXPECT_EQ(path_result["code"].get<std::string>(), "bad_request");

  auto bad_length = file_mapping::rpc::parse_control_message(R"({"type":"read","id":11,"mapping":"host-test","path":"hello.txt","length":-1})");
  ASSERT_TRUE(bad_length.ok) << bad_length.error;
  auto length_result = file_mapping::operations::execute_control_message(bad_length, context);
  EXPECT_EQ(length_result["type"].get<std::string>(), "error");
  EXPECT_EQ(length_result["code"].get<std::string>(), "bad_request");
}

TEST(FileMappingOperations, RejectsOpenAsUnsupported) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"open","id":12,"mapping":"host-test","path":"hello.txt"})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "error");
  EXPECT_EQ(result["code"].get<std::string>(), "unsupported_operation");
}

TEST(FileMappingOperations, RejectsUnauthorizedClient) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  context.peer_uuid = "other-client";
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"list","id":5,"mapping":"host-test","path":""})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "error");
  EXPECT_EQ(result["code"].get<std::string>(), "forbidden");
}

TEST(FileMappingOperations, RejectsAbsoluteAndParentTraversalPaths) {
  temp_tree_t tree;
  auto context = make_context(tree.root);

  auto absolute = file_mapping::rpc::parse_control_message(R"({"type":"read","id":6,"mapping":"host-test","path":"C:/Windows/win.ini"})");
  ASSERT_TRUE(absolute.ok) << absolute.error;
  auto absolute_result = file_mapping::operations::execute_control_message(absolute, context);
  EXPECT_EQ(absolute_result["type"].get<std::string>(), "error");
  EXPECT_EQ(absolute_result["code"].get<std::string>(), "absolute_path");

  auto traversal = file_mapping::rpc::parse_control_message(R"({"type":"read","id":7,"mapping":"host-test","path":"../outside.txt"})");
  ASSERT_TRUE(traversal.ok) << traversal.error;
  auto traversal_result = file_mapping::operations::execute_control_message(traversal, context);
  EXPECT_EQ(traversal_result["type"].get<std::string>(), "error");
  EXPECT_EQ(traversal_result["code"].get<std::string>(), "invalid_path");
}

TEST(FileMappingOperations, UsesMappingProviderForRuntimeUpdates) {
  temp_tree_t tree;
  file_mapping_store::store_t store;

  file_mapping::operations::execution_context_t context;
  context.peer_uuid = "client-uuid";
  context.mapping_provider = [&store]() {
    return store.snapshot();
  };

  auto created = store.add_quick_share(tree.root);
  ASSERT_TRUE(created.ok) << created.error;

  const auto message = std::string { R"({"type":"list","id":8,"mapping":")" } + created.mapping.id + R"(","path":""})";
  auto parsed = file_mapping::rpc::parse_control_message(message);
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_TRUE(result["ok"].get<bool>());
  EXPECT_EQ(result["mapping"].get<std::string>(), created.mapping.id);
  EXPECT_EQ(result["entries"].size(), 2);
}

TEST(FileMappingOperations, UploadsFileInTransactionalChunks) {
  temp_tree_t tree;
  auto context = make_writable_context(tree.root);

  auto first = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":20,"mapping":"host-test","path":"received.txt","upload_id":"upload-1","offset":0,"total_size":11,"begin":true,"complete":false,"data":"aGVsbG8g"})");
  ASSERT_TRUE(first.ok) << first.error;
  auto first_result = file_mapping::operations::execute_control_message(first, context);
  ASSERT_EQ(first_result["type"], "result") << first_result.dump();
  EXPECT_EQ(first_result["next_offset"], 6);
  EXPECT_FALSE(fs::exists(tree.root / "received.txt"));

  auto last = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":21,"mapping":"host-test","path":"received.txt","upload_id":"upload-1","offset":6,"total_size":11,"begin":false,"complete":true,"data":"d29ybGQ="})");
  ASSERT_TRUE(last.ok) << last.error;
  auto last_result = file_mapping::operations::execute_control_message(last, context);
  ASSERT_EQ(last_result["type"], "result") << last_result.dump();
  EXPECT_TRUE(last_result["completed"].get<bool>());

  std::ifstream in(tree.root / "received.txt", std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "hello world");
}

TEST(FileMappingOperations, CreatesUploadDirectories) {
  temp_tree_t tree;
  auto context = make_writable_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(
    R"({"type":"mkdir","id":22,"mapping":"host-test","path":"incoming"})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  ASSERT_EQ(result["type"], "result") << result.dump();
  EXPECT_TRUE(fs::is_directory(tree.root / "incoming"));
}

TEST(FileMappingOperations, RejectsUploadToReadOnlyMapping) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":23,"mapping":"host-test","path":"received.txt","upload_id":"upload-2","offset":0,"begin":true,"complete":true,"data":"eA=="})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"], "error");
  EXPECT_EQ(result["code"], "read_only");
  EXPECT_FALSE(fs::exists(tree.root / "received.txt"));
}

TEST(FileMappingOperations, RejectsImplicitOverwriteAndUploadPathTraversal) {
  temp_tree_t tree;
  auto context = make_writable_context(tree.root);

  auto overwrite = file_mapping::rpc::parse_control_message(
    // Omitting conflict_policy preserves the protocol's backward-compatible
    // Reject default; replacement requires an explicit Overwrite request.
    R"({"type":"write","id":24,"mapping":"host-test","path":"hello.txt","upload_id":"upload-3","offset":0,"total_size":1,"begin":true,"complete":true,"data":"eA=="})");
  ASSERT_TRUE(overwrite.ok) << overwrite.error;
  auto overwrite_result = file_mapping::operations::execute_control_message(overwrite, context);
  EXPECT_EQ(overwrite_result["code"], "already_exists");

  auto traversal = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":25,"mapping":"host-test","path":"../outside.txt","upload_id":"upload-4","offset":0,"total_size":1,"begin":true,"complete":true,"data":"eA=="})");
  ASSERT_TRUE(traversal.ok) << traversal.error;
  auto traversal_result = file_mapping::operations::execute_control_message(traversal, context);
  EXPECT_EQ(traversal_result["code"], "invalid_path");
}

TEST(FileMappingOperations, KeepsBothUploadsWithExplorerStyleSuffixes) {
  temp_tree_t tree;
  auto context = make_writable_context(tree.root);

  auto first = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":30,"mapping":"host-test","path":"hello.txt","upload_id":"keep-1","offset":0,"total_size":1,"begin":true,"complete":true,"conflict_policy":"keep_both","data":"eA=="})");
  ASSERT_TRUE(first.ok) << first.error;
  auto first_result = file_mapping::operations::execute_control_message(first, context);
  ASSERT_EQ(first_result["type"], "result") << first_result.dump();
  EXPECT_EQ(first_result["path"], "hello (1).txt");
  EXPECT_TRUE(fs::exists(tree.root / "hello (1).txt"));

  auto second = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":31,"mapping":"host-test","path":"hello.txt","upload_id":"keep-2","offset":0,"total_size":1,"begin":true,"complete":true,"conflict_policy":"keep_both","data":"eQ=="})");
  ASSERT_TRUE(second.ok) << second.error;
  auto second_result = file_mapping::operations::execute_control_message(second, context);
  ASSERT_EQ(second_result["type"], "result") << second_result.dump();
  EXPECT_EQ(second_result["path"], "hello (2).txt");
  EXPECT_TRUE(fs::exists(tree.root / "hello (2).txt"));
}

TEST(FileMappingOperations, OverwritesExistingFileWhenExplicitlyRequested) {
  temp_tree_t tree;
  auto context = make_writable_context(tree.root);

  auto parsed = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":32,"mapping":"host-test","path":"hello.txt","upload_id":"overwrite-1","offset":0,"total_size":3,"begin":true,"complete":true,"conflict_policy":"overwrite","data":"bmV3"})");
  ASSERT_TRUE(parsed.ok) << parsed.error;
  auto result = file_mapping::operations::execute_control_message(parsed, context);
  ASSERT_EQ(result["type"], "result") << result.dump();

  std::ifstream in(tree.root / "hello.txt", std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "new");
}

TEST(FileMappingOperations, RenamesCreatesAndDeletesItems) {
  temp_tree_t tree;
  auto context = make_writable_context(tree.root);

  auto mkdir = file_mapping::rpc::parse_control_message(
    R"({"type":"mkdir","id":33,"mapping":"host-test","path":"nested","conflict_policy":"keep_both"})");
  ASSERT_TRUE(mkdir.ok) << mkdir.error;
  auto mkdir_result = file_mapping::operations::execute_control_message(mkdir, context);
  ASSERT_EQ(mkdir_result["type"], "result") << mkdir_result.dump();
  EXPECT_EQ(mkdir_result["path"], "nested (1)");
  EXPECT_TRUE(fs::is_directory(tree.root / "nested (1)"));

  auto create = file_mapping::rpc::parse_control_message(
    R"({"type":"write","id":34,"mapping":"host-test","path":"empty.txt","upload_id":"empty-1","offset":0,"total_size":0,"begin":true,"complete":true,"conflict_policy":"reject","data":""})");
  ASSERT_TRUE(create.ok) << create.error;
  auto create_result = file_mapping::operations::execute_control_message(create, context);
  ASSERT_EQ(create_result["type"], "result") << create_result.dump();
  EXPECT_TRUE(fs::is_regular_file(tree.root / "empty.txt"));

  auto rename = file_mapping::rpc::parse_control_message(
    R"({"type":"rename","id":35,"mapping":"host-test","path":"empty.txt","destination_path":"renamed.txt"})");
  ASSERT_TRUE(rename.ok) << rename.error;
  auto rename_result = file_mapping::operations::execute_control_message(rename, context);
  ASSERT_EQ(rename_result["type"], "result") << rename_result.dump();
  EXPECT_TRUE(fs::is_regular_file(tree.root / "renamed.txt"));

  auto remove = file_mapping::rpc::parse_control_message(
    // A custom raw-string delimiter avoids the ")" in the Explorer-style
    // suffix terminating the C++ literal early.
    R"json({"type":"delete","id":36,"mapping":"host-test","path":"nested (1)","recursive":true})json");
  ASSERT_TRUE(remove.ok) << remove.error;
  auto remove_result = file_mapping::operations::execute_control_message(remove, context);
  ASSERT_EQ(remove_result["type"], "result") << remove_result.dump();
  EXPECT_FALSE(fs::exists(tree.root / "nested (1)"));
}

TEST(FileMappingOperations, NeverRenamesOrDeletesMappingRoot) {
  temp_tree_t tree;
  auto context = make_writable_context(tree.root);

  auto rename = file_mapping::rpc::parse_control_message(
    R"({"type":"rename","id":37,"mapping":"host-test","path":"","destination_path":"renamed-root"})");
  ASSERT_TRUE(rename.ok) << rename.error;
  auto rename_result = file_mapping::operations::execute_control_message(rename, context);
  EXPECT_EQ(rename_result["code"], "bad_request");

  auto remove = file_mapping::rpc::parse_control_message(
    R"({"type":"delete","id":38,"mapping":"host-test","path":"","recursive":true})");
  ASSERT_TRUE(remove.ok) << remove.error;
  auto remove_result = file_mapping::operations::execute_control_message(remove, context);
  EXPECT_EQ(remove_result["code"], "bad_request");
}
