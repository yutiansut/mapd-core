/*
 * Copyright 2018 OmniSci, Inc.
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

#pragma once

#include "sqldefs.h"

#include <string>
#include "Logger.h"

inline std::string sql_window_function_to_str(const SqlWindowFunctionKind kind) {
  switch (kind) {
    case SqlWindowFunctionKind::ROW_NUMBER: {
      return "ROW_NUMBER";
    }
    case SqlWindowFunctionKind::RANK: {
      return "RANK";
    }
    case SqlWindowFunctionKind::DENSE_RANK: {
      return "DENSE_RANK";
    }
    case SqlWindowFunctionKind::PERCENT_RANK: {
      return "PERCENT_RANK";
    }
    case SqlWindowFunctionKind::CUME_DIST: {
      return "CUME_DIST";
    }
    case SqlWindowFunctionKind::NTILE: {
      return "NTILE";
    }
    case SqlWindowFunctionKind::LAG: {
      return "LAG";
    }
    case SqlWindowFunctionKind::LEAD: {
      return "LEAD";
    }
    case SqlWindowFunctionKind::FIRST_VALUE: {
      return "FIRST_VALUE";
    }
    case SqlWindowFunctionKind::LAST_VALUE: {
      return "LAST_VALUE";
    }
    case SqlWindowFunctionKind::AVG: {
      return "AVG";
    }
    case SqlWindowFunctionKind::MIN: {
      return "MIN";
    }
    case SqlWindowFunctionKind::MAX: {
      return "MAX";
    }
    case SqlWindowFunctionKind::SUM: {
      return "SUM";
    }
    case SqlWindowFunctionKind::COUNT: {
      return "COUNT";
    }
    case SqlWindowFunctionKind::SUM_INTERNAL: {
      return "SUM_INTERNAL";
    }
    default: {
      LOG(FATAL) << "Invalid window function kind";
      return "";
    }
  }
}
