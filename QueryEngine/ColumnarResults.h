/*
 * Copyright 2017 MapD Technologies, Inc.
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

#ifndef COLUMNAR_RESULTS_H
#define COLUMNAR_RESULTS_H
#include "ResultSet.h"
#include "Shared/SqlTypesLayout.h"

#include "../Shared/checked_alloc.h"

#include <memory>
#include <unordered_map>

class ColumnarConversionNotSupported : public std::runtime_error {
 public:
  ColumnarConversionNotSupported()
      : std::runtime_error(
            "Columnar conversion not supported for variable length types") {}
};

class ColumnarResults {
 public:
  ColumnarResults(const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                  const ResultSet& rows,
                  const size_t num_columns,
                  const std::vector<SQLTypeInfo>& target_types);

  ColumnarResults(const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                  const int8_t* one_col_buffer,
                  const size_t num_rows,
                  const SQLTypeInfo& target_type);

  static std::unique_ptr<ColumnarResults> mergeResults(
      const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
      const std::vector<std::unique_ptr<ColumnarResults>>& sub_results);

  const std::vector<const int8_t*>& getColumnBuffers() const { return column_buffers_; }

  const size_t size() const { return num_rows_; }

  const SQLTypeInfo& getColumnType(const int col_id) const {
    CHECK_GE(col_id, 0);
    CHECK_LT(static_cast<size_t>(col_id), target_types_.size());
    return target_types_[col_id];
  }

 private:
  ColumnarResults(const size_t num_rows, const std::vector<SQLTypeInfo>& target_types)
      : num_rows_(num_rows), target_types_(target_types) {}
  inline void writeBackCell(const TargetValue& col_val,
                            const size_t row_idx,
                            const size_t column_idx);
  void materializeAllColumns(const ResultSet& rows, const size_t num_columns);
  void copyAllNonLazyColumns(const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
                             const ResultSet& rows,
                             const size_t num_columns);
  void materializeAllLazyColumns(const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
                                 const ResultSet& rows,
                                 const size_t num_columns);

  std::vector<const int8_t*> column_buffers_;
  size_t num_rows_;
  const std::vector<SQLTypeInfo> target_types_;
};

typedef std::
    unordered_map<int, std::unordered_map<int, std::shared_ptr<const ColumnarResults>>>
        ColumnCacheMap;
#endif  // COLUMNAR_RESULTS_H
