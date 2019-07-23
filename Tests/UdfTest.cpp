/*
 * Copyright 2019 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <csignal>
#include <exception>
#include <memory>
#include <vector>
#include "Catalog/Catalog.h"
#include "Catalog/DBObject.h"
#include "DataMgr/DataMgr.h"
#include "QueryEngine/Execute.h"
#include "QueryEngine/ExtensionFunctionsWhitelist.h"
#include "QueryEngine/ResultSet.h"
#include "QueryEngine/UDFCompiler.h"
#include "QueryRunner/QueryRunner.h"
#include "Shared/Logger.h"
#include "Shared/MapDParameters.h"
#include "TestHelpers.h"

#ifndef BASE_PATH
#define BASE_PATH "./tmp"
#endif

using namespace Catalog_Namespace;
using namespace TestHelpers;

using QR = QueryRunner::QueryRunner;

#define SKIP_NO_GPU()                                        \
  if (skip_tests(dt)) {                                      \
    CHECK(dt == ExecutorDeviceType::GPU);                    \
    LOG(WARNING) << "GPU not available, skipping GPU tests"; \
    continue;                                                \
  }

namespace {

std::shared_ptr<Calcite> g_calcite = nullptr;
std::string udf_file_name_base("../../Tests/Udf/udf_sample");

std::shared_ptr<ResultSet> run_multiple_agg(const std::string& query_str,
                                            const ExecutorDeviceType device_type,
                                            const bool allow_loop_joins) {
  return QR::get()->runSQL(query_str, device_type, true, allow_loop_joins);
}

std::shared_ptr<ResultSet> run_multiple_agg(const std::string& query_str,
                                            const ExecutorDeviceType device_type) {
  return run_multiple_agg(query_str, device_type, true);
}

TargetValue run_simple_agg(const std::string& query_str,
                           const ExecutorDeviceType device_type,
                           const bool allow_loop_joins = true) {
  auto rows = run_multiple_agg(query_str, device_type, allow_loop_joins);
  auto crt_row = rows->getNextRow(true, true);
  CHECK_EQ(size_t(1), crt_row.size());
  return crt_row[0];
}

std::string get_udf_filename() {
  return udf_file_name_base + ".cpp";
}

std::string get_udf_cpu_ir_filename() {
  return udf_file_name_base + "_cpu.bc";
}

std::string get_udf_gpu_ir_filename() {
  return udf_file_name_base + "_gpu.bc";
}

std::string get_udf_ast_filename() {
  return udf_file_name_base + ".ast";
}

bool skip_tests(const ExecutorDeviceType device_type) {
#ifdef HAVE_CUDA
  return device_type == ExecutorDeviceType::GPU && !QR::get()->gpusPresent();
#else
  return device_type == ExecutorDeviceType::GPU;
#endif
}

inline void run_ddl_statement(const std::string& query) {
  QR::get()->runDDLStatement(query);
}

class SQLTestEnv : public ::testing::Environment {
 public:
  void SetUp() override {
    boost::filesystem::path udf_file((get_udf_filename()));
    if (!boost::filesystem::exists(udf_file)) {
      throw std::runtime_error("udf file: " + udf_file.string() + " does not exist");
    }

    UdfCompiler compiler(udf_file.string());
    auto compile_result = compiler.compileUdf();
    EXPECT_EQ(compile_result, 0);

    QR::init(BASE_PATH, compiler.getAstFileName());

    g_calcite = QR::get()->getCalcite();
  }

  void TearDown() override {
    boost::filesystem::path cpu_ir_file(get_udf_cpu_ir_filename());
    if (boost::filesystem::exists(cpu_ir_file)) {
      boost::filesystem::remove(cpu_ir_file);
    }

    boost::filesystem::path gpu_ir_file(get_udf_gpu_ir_filename());
    if (boost::filesystem::exists(gpu_ir_file)) {
      boost::filesystem::remove(gpu_ir_file);
    }

    boost::filesystem::path udf_ast_file(get_udf_ast_filename());
    if (boost::filesystem::exists(udf_ast_file)) {
      boost::filesystem::remove(udf_ast_file);
    }

    QR::reset();
  }
};
}  // namespace

class UDFCompilerTest : public ::testing::Test {
 protected:
  UDFCompilerTest() : udf_file_(boost::filesystem::path(get_udf_filename())) {
    if (!boost::filesystem::exists(udf_file_)) {
      throw std::runtime_error("udf file: " + udf_file_.string() + " does not exist");
    }

    setup_objects();
  }

  ~UDFCompilerTest() override { remove_objects(); }

  void SetUp() override {}

  void TearDown() override {}

  void setup_objects() {}

  void remove_objects() {}

  std::string getUdfFileName() const { return udf_file_.string(); }

 private:
  boost::filesystem::path udf_file_;
};

TEST_F(UDFCompilerTest, CompileTest) {
  UdfCompiler compiler(getUdfFileName());
  auto compile_result = compiler.compileUdf();

  EXPECT_EQ(compile_result, 0);
  // TODO cannot test invalid file path because the compileUdf function uses
  // LOG(FATAL) which stops the process and does not return
}

TEST_F(UDFCompilerTest, CalciteRegistration) {
  UdfCompiler compiler(getUdfFileName());
  auto compile_result = compiler.compileUdf();

  ASSERT_EQ(compile_result, 0);

  ASSERT_TRUE(g_calcite != nullptr);

  auto signature = ExtensionFunctionsWhitelist::get_udf("udf_truerange");
  ASSERT_NE(signature, nullptr);

  auto signature2 = ExtensionFunctionsWhitelist::get_udf("udf_truehigh");
  ASSERT_NE(signature2, nullptr);

  auto signature3 = ExtensionFunctionsWhitelist::get_udf("udf_truelow");
  ASSERT_NE(signature3, nullptr);

  auto signature4 = ExtensionFunctionsWhitelist::get_udf("udf_range");
  ASSERT_NE(signature4, nullptr);

  auto signature5 = ExtensionFunctionsWhitelist::get_udf("udf_range_int");
  ASSERT_NE(signature5, nullptr);

  auto signature6 = ExtensionFunctionsWhitelist::get_udf("udf_range_integer");
  ASSERT_EQ(signature6, nullptr);
}

TEST_F(UDFCompilerTest, UdfQuery) {
  UdfCompiler compiler(getUdfFileName());
  auto compile_result = compiler.compileUdf();

  ASSERT_EQ(compile_result, 0);

  run_ddl_statement("DROP TABLE IF EXISTS stocks;");
  run_ddl_statement(
      "CREATE TABLE stocks(symbol text, open_p int, high_p int, "
      "low_p int, close_p int, entry_d DATE);");

  std::string insert1(
      "INSERT into stocks VALUES ('NVDA', '178', '178', '171', '173', '2019-05-07');");
  EXPECT_NO_THROW(run_multiple_agg(insert1, ExecutorDeviceType::CPU));

  std::string insert2(
      "INSERT into stocks VALUES ('NVDA', '175', '181', '174', '178', '2019-05-06');");
  EXPECT_NO_THROW(run_multiple_agg(insert2, ExecutorDeviceType::CPU));

  std::string insert3(
      "INSERT into stocks VALUES ('NVDA', '183', '184', '181', '183', '2019-05-03');");
  EXPECT_NO_THROW(run_multiple_agg(insert3, ExecutorDeviceType::CPU));

  for (auto dt : {ExecutorDeviceType::CPU, ExecutorDeviceType::GPU}) {
    SKIP_NO_GPU();
    ASSERT_EQ(7,
              v<int64_t>(run_simple_agg("SELECT udf_range_int(high_p, low_p) from stocks "
                                        "where entry_d = '2019-05-06';",
                                        dt)));
    ASSERT_EQ(3,
              v<int64_t>(run_simple_agg("SELECT udf_range_int(high_p, low_p) from stocks "
                                        "where entry_d = '2019-05-03';",
                                        dt)));
  }

  EXPECT_THROW(run_simple_agg("SELECT udf_range_integer(high_p, low_p) from stocks where "
                              "entry_d = '2019-05-06';",
                              ExecutorDeviceType::CPU),
               std::exception);

  run_ddl_statement("DROP TABLE stocks;");
}

int main(int argc, char** argv) {
  TestHelpers::init_logger_stderr_only(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new SQLTestEnv);

  int err{0};
  try {
    err = RUN_ALL_TESTS();
  } catch (const std::exception& e) {
    LOG(ERROR) << e.what();
  }
  return err;
}
