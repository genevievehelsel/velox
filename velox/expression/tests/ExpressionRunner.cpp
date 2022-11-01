/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include "velox/common/memory/Memory.h"
#include "velox/core/QueryCtx.h"
#include "velox/exec/tests/utils/QueryAssertions.h"
#include "velox/expression/Expr.h"
#include "velox/expression/tests/ExpressionRunner.h"
#include "velox/expression/tests/ExpressionVerifier.h"
#include "velox/parse/Expressions.h"
#include "velox/parse/ExpressionsParser.h"
#include "velox/parse/TypeResolver.h"
#include "velox/vector/VectorSaver.h"

namespace facebook::velox::test {

namespace {
/// Parse comma-separated SQL expressions.
std::vector<core::TypedExprPtr> parseSql(
    const std::string& sql,
    const TypePtr& inputType,
    memory::MemoryPool* pool) {
  auto exprs = parse::parseMultipleExpressions(sql, {});

  std::vector<core::TypedExprPtr> typedExprs;
  typedExprs.reserve(exprs.size());
  for (const auto& expr : exprs) {
    typedExprs.push_back(core::Expressions::inferTypes(expr, inputType, pool));
  }
  return typedExprs;
}

/// Creates a RowVector from a list of child vectors. Uses _col0, _col1,..
/// auto-generated names for the RowType.
RowVectorPtr createRowVector(
    const std::vector<VectorPtr>& vectors,
    vector_size_t size,
    memory::MemoryPool* pool) {
  auto n = vectors.size();

  std::vector<std::string> names;
  names.reserve(n);
  std::vector<TypePtr> types;
  types.reserve(n);
  for (auto i = 0; i < n; ++i) {
    names.push_back(fmt::format("_col{}", i));
    types.push_back(vectors[i]->type());
  }

  return std::make_shared<RowVector>(
      pool, ROW(std::move(names), std::move(types)), nullptr, size, vectors);
}

void evaluateAndPrintResults(
    exec::ExprSet& exprSet,
    const RowVectorPtr& data,
    const SelectivityVector& rows,
    core::ExecCtx& execCtx) {
  exec::EvalCtx evalCtx(&execCtx, &exprSet, data.get());

  std::vector<VectorPtr> results(1);
  exprSet.eval(rows, evalCtx, results);

  // Print the results.
  auto rowResult = createRowVector(results, rows.size(), execCtx.pool());
  std::cout << "Result: " << rowResult->type()->toString() << std::endl;
  exec::test::printResults(rowResult, std::cout);
}

vector_size_t adjustNumRows(vector_size_t numRows, vector_size_t size) {
  return numRows > 0 && numRows < size ? numRows : size;
}
} // namespace

void ExpressionRunner::run(
    const std::string& inputPath,
    const std::string& sql,
    const std::string& resultPath,
    const std::string& mode,
    vector_size_t numRows) {
  VELOX_CHECK(!sql.empty());

  std::shared_ptr<core::QueryCtx> queryCtx{core::QueryCtx::createForTest()};
  std::unique_ptr<memory::MemoryPool> pool{
      memory::getDefaultScopedMemoryPool()};
  core::ExecCtx execCtx{pool.get(), queryCtx.get()};

  RowVectorPtr inputVector;
  if (inputPath.empty()) {
    inputVector = std::make_shared<RowVector>(
        pool.get(), ROW({}), nullptr, 1, std::vector<VectorPtr>{});
  } else {
    inputVector = std::dynamic_pointer_cast<RowVector>(
        restoreVectorFromFile(inputPath.c_str(), pool.get()));
    VELOX_CHECK_NOT_NULL(
        inputVector,
        "Input vector is not a RowVector: {}",
        inputVector->toString());
    VELOX_CHECK_GT(inputVector->size(), 0, "Input vector must not be empty.");
  }

  parse::registerTypeResolver();
  auto typedExprs = parseSql(sql, inputVector->type(), pool.get());

  VectorPtr resultVector;
  if (!resultPath.empty()) {
    resultVector = restoreVectorFromFile(resultPath.c_str(), pool.get());
  }

  SelectivityVector rows(adjustNumRows(numRows, inputVector->size()));

  LOG(INFO) << "Evaluating SQL expression(s): " << sql;

  if (mode == "verify") {
    VELOX_CHECK_EQ(
        1, typedExprs.size(), "'verify' mode supports only one SQL expression");
    test::ExpressionVerifier(&execCtx, {false, ""})
        .verify(typedExprs[0], inputVector, std::move(resultVector), true);
  } else if (mode == "common") {
    exec::ExprSet exprSet(typedExprs, &execCtx);
    evaluateAndPrintResults(exprSet, inputVector, rows, execCtx);
  } else if (mode == "simplified") {
    exec::ExprSetSimplified exprSet(typedExprs, &execCtx);
    evaluateAndPrintResults(exprSet, inputVector, rows, execCtx);
  } else {
    VELOX_FAIL("Unknown expression runner mode: [{}].", mode);
  }
}

} // namespace facebook::velox::test