//  Copyright (c) 2017-present, Qihoo, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include <gtest/gtest.h>
#include <iostream>
#include <thread>

#include "src/lists_filter.h"
#include "src/redis.h"
#include "storage/storage.h"

using storage::EncodeFixed64;
using storage::ListsDataFilter;
using storage::ListsDataKey;
using storage::ListsMetaValue;
using storage::Slice;
using storage::Status;

class ListsFilterTest : public ::testing::Test {
 public:
  ListsFilterTest() {
    std::string db_path = "./db/list_meta";
    if (access(db_path.c_str(), F_OK) != 0) {
      mkdir(db_path.c_str(), 0755);
    }

#ifdef USE_S3
    // rocksdb-cloud env
    rocksdb::CloudFileSystemOptions cloud_fs_opts;
    cloud_fs_opts.endpoint_override = "http://127.0.0.1:9000";
    cloud_fs_opts.credentials.InitializeSimple("minioadmin", "minioadmin");
    assert(cloud_fs_opts.credentials.HasValid().ok()); // TODO: add handle error 
    std::string s3_path = db_path[0] == '.' ? db_path.substr(1) : db_path;
    cloud_fs_opts.src_bucket.SetBucketName("database.unit.test", "pika.");
    cloud_fs_opts.src_bucket.SetObjectPath(s3_path);
    cloud_fs_opts.dest_bucket.SetBucketName("database.unit.test", "pika.");
    cloud_fs_opts.dest_bucket.SetObjectPath(s3_path);
    rocksdb::CloudFileSystem* cfs = nullptr;
    Status s = rocksdb::CloudFileSystem::NewAwsFileSystem(
      rocksdb::FileSystem::Default(), 
      cloud_fs_opts, 
      nullptr, 
      &cfs
    );
    assert(s.ok());
    std::shared_ptr<rocksdb::CloudFileSystem> cloud_fs(cfs);
    cloud_env = NewCompositeEnv(cloud_fs);
    assert(cloud_env);
    options.env = cloud_env.get();
    options.create_if_missing = true;
    s = rocksdb::DBCloud::Open(options, db_path, "", 0, &meta_db);
#else
    s = rocksdb::DB::Open(options, db_path, &meta_db)
#endif

    if (s.ok()) {
      // create column family
      rocksdb::ColumnFamilyHandle* cf;
      s = meta_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "data_cf", &cf);
      delete cf;
      delete meta_db;
    }

    rocksdb::ColumnFamilyOptions meta_cf_ops(options);
    rocksdb::ColumnFamilyOptions data_cf_ops(options);

    // Meta CF
    column_families.emplace_back(rocksdb::kDefaultColumnFamilyName, meta_cf_ops);
    // Data CF
    column_families.emplace_back("data_cf", data_cf_ops);

#ifdef USE_S3
    s = rocksdb::DBCloud::Open(options, db_path, column_families, "", 0, &handles, &meta_db);
#else
    s = rocksdb::DB::Open(options, db_path, column_families, &handles, &meta_db);
#endif
  }
  ~ListsFilterTest() override = default;

  void SetUp() override {}
  void TearDown() override {
    for (auto handle : handles) {
      delete handle;
    }
    delete meta_db;
  }

  storage::Options options;
#ifdef USE_S3
  rocksdb::DBCloud* meta_db;
#else
  rocksdb::DB* meta_db;
#endif
  std::unique_ptr<rocksdb::Env> cloud_env;
  storage::Status s;

  std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
  std::vector<rocksdb::ColumnFamilyHandle*> handles;
};

// Meta Filter
TEST_F(ListsFilterTest, MetaFilterTest) {
  char str[8];
  bool filter_result;
  bool value_changed;
  int32_t version = 0;
  std::string new_value;

  // Test Meta Filter
  auto lists_meta_filter = std::make_unique<storage::ListsMetaFilter>();
  ASSERT_TRUE(lists_meta_filter != nullptr);

  // Timeout timestamp is not set, but it's an empty list.
  EncodeFixed64(str, 0);
  ListsMetaValue lists_meta_value1(Slice(str, sizeof(uint64_t)));
  lists_meta_value1.UpdateVersion();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  filter_result =
      lists_meta_filter->Filter(0, "FILTER_TEST_KEY", lists_meta_value1.Encode(), &new_value, &value_changed);
  ASSERT_EQ(filter_result, true);

  // Timeout timestamp is not set, it's not an empty list.
  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value2(Slice(str, sizeof(uint64_t)));
  lists_meta_value2.UpdateVersion();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  filter_result =
      lists_meta_filter->Filter(0, "FILTER_TEST_KEY", lists_meta_value2.Encode(), &new_value, &value_changed);
  ASSERT_EQ(filter_result, false);

  // Timeout timestamp is set, but not expired.
  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value3(Slice(str, sizeof(uint64_t)));
  lists_meta_value3.UpdateVersion();
  lists_meta_value3.SetRelativeTimestamp(3);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  filter_result =
      lists_meta_filter->Filter(0, "FILTER_TEST_KEY", lists_meta_value3.Encode(), &new_value, &value_changed);
  ASSERT_EQ(filter_result, false);

  // Timeout timestamp is set, already expired.
  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value4(Slice(str, sizeof(uint64_t)));
  lists_meta_value4.UpdateVersion();
  lists_meta_value4.SetRelativeTimestamp(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  storage::ParsedListsMetaValue parsed_meta_value(lists_meta_value4.Encode());
  filter_result =
      lists_meta_filter->Filter(0, "FILTER_TEST_KEY", lists_meta_value4.Encode(), &new_value, &value_changed);
  ASSERT_EQ(filter_result, true);
}

// Data Filter
TEST_F(ListsFilterTest, DataFilterTest) {
  char str[8];
  bool filter_result;
  bool value_changed;
  int32_t version = 0;
  std::string new_value;

  // Timeout timestamp is not set, the version is valid.
  auto lists_data_filter1 = std::make_unique<ListsDataFilter>(meta_db, &handles);
  ASSERT_TRUE(lists_data_filter1 != nullptr);

  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value1(Slice(str, sizeof(uint64_t)));
  version = lists_meta_value1.UpdateVersion();
  s = meta_db->Put(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY", lists_meta_value1.Encode());
  ASSERT_TRUE(s.ok());

  ListsDataKey lists_data_key1("FILTER_TEST_KEY", version, 1);
  filter_result =
      lists_data_filter1->Filter(0, lists_data_key1.Encode(), "FILTER_TEST_VALUE", &new_value, &value_changed);
  ASSERT_EQ(filter_result, false);
  s = meta_db->Delete(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY");
  ASSERT_TRUE(s.ok());

  // Timeout timestamp is set, but not expired.
  auto lists_data_filter2 = std::make_unique<ListsDataFilter>(meta_db, &handles);
  ASSERT_TRUE(lists_data_filter2 != nullptr);

  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value2(Slice(str, sizeof(uint64_t)));
  version = lists_meta_value2.UpdateVersion();
  lists_meta_value2.SetRelativeTimestamp(1);
  s = meta_db->Put(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY", lists_meta_value2.Encode());
  ASSERT_TRUE(s.ok());
  ListsDataKey lists_data_key2("FILTER_TEST_KEY", version, 1);
  filter_result =
      lists_data_filter2->Filter(0, lists_data_key2.Encode(), "FILTER_TEST_VALUE", &new_value, &value_changed);
  ASSERT_EQ(filter_result, false);
  s = meta_db->Delete(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY");
  ASSERT_TRUE(s.ok());

  // Timeout timestamp is set, already expired.
  auto lists_data_filter3 = std::make_unique<ListsDataFilter>(meta_db, &handles);
  ASSERT_TRUE(lists_data_filter3 != nullptr);

  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value3(Slice(str, sizeof(uint64_t)));
  version = lists_meta_value3.UpdateVersion();
  lists_meta_value3.SetRelativeTimestamp(1);
  s = meta_db->Put(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY", lists_meta_value3.Encode());
  ASSERT_TRUE(s.ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  ListsDataKey lists_data_key3("FILTER_TEST_KEY", version, 1);
  filter_result =
      lists_data_filter3->Filter(0, lists_data_key3.Encode(), "FILTER_TEST_VALUE", &new_value, &value_changed);
  ASSERT_EQ(filter_result, true);
  s = meta_db->Delete(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY");
  ASSERT_TRUE(s.ok());

  // Timeout timestamp is not set, the version is invalid
  auto lists_data_filter4 = std::make_unique<ListsDataFilter>(meta_db, &handles);
  ASSERT_TRUE(lists_data_filter4 != nullptr);

  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value4(Slice(str, sizeof(uint64_t)));
  version = lists_meta_value4.UpdateVersion();
  s = meta_db->Put(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY", lists_meta_value4.Encode());
  ASSERT_TRUE(s.ok());
  ListsDataKey lists_data_key4("FILTER_TEST_KEY", version, 1);
  version = lists_meta_value4.UpdateVersion();
  s = meta_db->Put(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY", lists_meta_value4.Encode());
  ASSERT_TRUE(s.ok());
  filter_result =
      lists_data_filter4->Filter(0, lists_data_key4.Encode(), "FILTER_TEST_VALUE", &new_value, &value_changed);
  ASSERT_EQ(filter_result, true);
  s = meta_db->Delete(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY");
  ASSERT_TRUE(s.ok());

  // Meta data has been clear
  auto lists_data_filter5 = std::make_unique<ListsDataFilter>(meta_db, &handles);
  ASSERT_TRUE(lists_data_filter5 != nullptr);

  EncodeFixed64(str, 1);
  ListsMetaValue lists_meta_value5(Slice(str, sizeof(uint64_t)));
  version = lists_meta_value5.UpdateVersion();
  s = meta_db->Put(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY", lists_meta_value5.Encode());
  ASSERT_TRUE(s.ok());
  ListsDataKey lists_data_value5("FILTER_TEST_KEY", version, 1);
  s = meta_db->Delete(rocksdb::WriteOptions(), handles[0], "FILTER_TEST_KEY");
  ASSERT_TRUE(s.ok());
  filter_result =
      lists_data_filter5->Filter(0, lists_data_value5.Encode(), "FILTER_TEST_VALUE", &new_value, &value_changed);
  ASSERT_EQ(filter_result, true);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
