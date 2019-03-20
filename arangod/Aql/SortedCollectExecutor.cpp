////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "SortedCollectExecutor.h"

#include "Aql/AqlValue.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Basics/Common.h"

#include <lib/Logger/LogMacros.h>

#include <utility>

using namespace arangodb;
using namespace arangodb::aql;

static const AqlValue EmptyValue;

SortedCollectExecutor::CollectGroup::CollectGroup(bool count, Infos& infos)
    : groupLength(0),
      count(count),
      infos(infos),
      _lastInputRow(InputAqlItemRow{CreateInvalidInputRowHint{}}){
  for (auto const& aggName : infos.getAggregateTypes()) {
    aggregators.emplace_back(Aggregator::fromTypeString(infos.getTransaction(), aggName));
  }
  TRI_ASSERT(infos.getAggregatedRegisters().size() == aggregators.size());
};

SortedCollectExecutor::CollectGroup::~CollectGroup() {
  for (auto& it : groupValues) {
    it.destroy();
  }
}

void SortedCollectExecutor::CollectGroup::initialize(size_t capacity) {
  groupValues.clear();

  if (capacity > 0) {
    groupValues.reserve(capacity);

    for (size_t i = 0; i < capacity; ++i) {
      groupValues.emplace_back();
    }
  }

  groupLength = 0;

  // reset aggregators
  for (auto& it : aggregators) {
    it->reset();
  }
}

void SortedCollectExecutor::CollectGroup::reset(InputAqlItemRow& input) {
  _shouldDeleteBuilderBuffer = true;
  ConditionalDeleter<VPackBuffer<uint8_t>> deleter(_shouldDeleteBuilderBuffer);
  std::shared_ptr<VPackBuffer<uint8_t>> buffer(new VPackBuffer<uint8_t>, deleter);
  _builder = VPackBuilder(buffer);

  if (!groupValues.empty()) {
    for (auto& it : groupValues) {
      it.destroy();
    }
    groupValues[0].erase();  // only need to erase [0], because we have
    // only copies of references anyway
  }

  groupLength = 0;
  _lastInputRow = input;

  // reset all aggregators
  for (auto& it : aggregators) {
    it->reset();
  }

  if (input.isInitialized()) {
    // construct the new group
    size_t i = 0;
    _builder.openArray();
    for (auto& it : infos.getGroupRegisters()) {
      this->groupValues[i] = input.getValue(it.second).clone();
      ++i;
    }

    addLine(input);
  }
}

void SortedCollectExecutor::CollectGroup::addValues(InputAqlItemRow& input,
                                                    RegisterId groupRegister) {
  if (groupRegister == ExecutionNode::MaxRegisterId) {
    // nothing to do, but still make sure we won't add the same rows again
    return;
  }

  // copy group values
  if (count) {
    groupLength += 1;
  } else {
    try {

      // groupValues.emplace_back(input.getValue(groupRegister)); // TODO check register
      for (auto& it : infos.getGroupRegisters()) {  // TODO check if this is really correct!!
        groupValues.emplace_back(input.getValue(it.second));  // ROW 328 of CollectBlock.cpp
      }
      /*
      size_t i = 0;
      for (auto& it : infos.getGroupRegisters()) {
        this->groupValues[i] = input.getValue(it.second).clone();
        ++i;
      }
      */
    } catch (...) {
      throw;
    }
  }
}

SortedCollectExecutorInfos::SortedCollectExecutorInfos(
    RegisterId nrInputRegisters, RegisterId nrOutputRegisters,
    std::unordered_set<RegisterId> registersToClear,
    std::unordered_set<RegisterId> registersToKeep,
    std::unordered_set<RegisterId>&& readableInputRegisters,
    std::unordered_set<RegisterId>&& writeableOutputRegisters,
    std::vector<std::pair<RegisterId, RegisterId>>&& groupRegisters,
    RegisterId collectRegister, RegisterId expressionRegister,
    Variable const* expressionVariable, std::vector<std::string>&& aggregateTypes,
    std::vector<std::pair<std::string, RegisterId>>&& variables,
    std::vector<std::pair<RegisterId, RegisterId>>&& aggregateRegisters,
    transaction::Methods* trxPtr, bool count)
    : ExecutorInfos(std::make_shared<std::unordered_set<RegisterId>>(readableInputRegisters),
                    std::make_shared<std::unordered_set<RegisterId>>(writeableOutputRegisters),
                    nrInputRegisters, nrOutputRegisters,
                    std::move(registersToClear), std::move(registersToKeep)),
      _aggregateTypes(aggregateTypes),
      _aggregateRegisters(aggregateRegisters),
      _groupRegisters(groupRegisters),
      _collectRegister(collectRegister),
      _expressionRegister(expressionRegister),
      _variables(variables),
      _expressionVariable(expressionVariable),
      _count(count),
      _trxPtr(trxPtr) {}

SortedCollectExecutor::SortedCollectExecutor(Fetcher& fetcher, Infos& infos)
    : _infos(infos), _fetcher(fetcher), _currentGroup(infos.getCount(), infos), _fetcherDone(false) {
  // reserve space for the current row
  _currentGroup.initialize(_infos.getGroupRegisters().size());
};

void SortedCollectExecutor::CollectGroup::addLine(InputAqlItemRow& input) {
  // remember the last valid row we had
  _lastInputRow = input;

  // calculate aggregate functions
  size_t j = 0;
  for (auto& it : this->aggregators) {
    RegisterId const reg = infos.getAggregatedRegisters()[j].second;
    it->reduce(input.getValue(reg));
    ++j;
  }

  if (infos.getCollectRegister() != ExecutionNode::MaxRegisterId) {
    if (count) {
      // increase the count
      groupLength++;
    } else if (infos.getExpressionVariable() != nullptr) {
      // compute the expression
      input.getValue(infos.getExpressionRegister()).toVelocyPack(infos.getTransaction(), _builder, false);
    } else {
      // copy variables / keep variables into result register

      _builder.openObject();
      for (auto const& pair : infos.getVariables()) {
        _builder.add(VPackValue(pair.first));
        input.getValue(pair.second).toVelocyPack(infos.getTransaction(), _builder, false);
      }
      _builder.close();
    }
  }
  TRI_IF_FAILURE("CollectGroup::addValues") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
}

bool SortedCollectExecutor::CollectGroup::isSameGroup(InputAqlItemRow& input) {
  // if we do not have valid input, return false
  if (!input.isInitialized()) {
    return false;
  } else {
    // check if groups are equal

    size_t i = 0;

    for (auto& it : infos.getGroupRegisters()) {
      // we already had a group, check if the group has changed
      // compare values 1 1 by one
      int cmp = AqlValue::Compare(infos.getTransaction(), this->groupValues[i],
                                  input.getValue(it.second), false);

      if (cmp != 0) {
        // This part pf the groupcheck differs
        return false;
      }
      ++i;
    }
    // Every part matched
    return true;
  }
}

void SortedCollectExecutor::CollectGroup::groupValuesToArray(VPackBuilder& builder) {
  builder.openArray();
  for (auto const& value : groupValues) {
    value.toVelocyPack(infos.getTransaction(), builder, false);
  }

  builder.close();
}

void SortedCollectExecutor::CollectGroup::writeToOutput(OutputAqlItemRow& output) {
  // if we do not have initialized input, just return and do not write to any register
  TRI_ASSERT(_lastInputRow.isInitialized());
  size_t i = 0;
  for (auto& it : infos.getGroupRegisters()) {
    AqlValue val = this->groupValues[i];
    AqlValueGuard guard{val, true};

    output.moveValueInto(it.first, _lastInputRow, guard);
    // ownership of value is transferred into res
    this->groupValues[i].erase();
    ++i;
  }

  // handle aggregators
  size_t j = 0;
  for (auto& it : this->aggregators) {
    AqlValue val = it->stealValue();
    AqlValueGuard guard{val, true};
    output.moveValueInto(infos.getAggregatedRegisters()[j].first, _lastInputRow, guard);
    ++j;
  }

  // set the group values
  if (infos.getCollectRegister() != ExecutionNode::MaxRegisterId) {
    if (infos.getCount()) {
      // only set group count in result register
      output.cloneValueInto(infos.getCollectRegister(), _lastInputRow,  // TODO check move
                            AqlValue(AqlValueHintUInt(static_cast<uint64_t>(this->groupLength))));
    } else {
      TRI_ASSERT(_builder.isOpenArray());
      _builder.close();

      auto buffer = _builder.steal();
      AqlValue val(buffer.get(), _shouldDeleteBuilderBuffer);
      AqlValueGuard guard{val, true};

      output.moveValueInto(infos.getCollectRegister(), _lastInputRow, guard);
    }
  }
}

std::pair<ExecutionState, NoStats> SortedCollectExecutor::produceRow(OutputAqlItemRow& output) {
  TRI_IF_FAILURE("SortedCollectExecutor::produceRow") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  if (_fetcherDone) {
    if (_currentGroup.isValid()) {
      _currentGroup.writeToOutput(output);
      InputAqlItemRow input{CreateInvalidInputRowHint{}};
      _currentGroup.reset(input);
      TRI_ASSERT(!_currentGroup.isValid());
      return {ExecutionState::DONE, {}};
    }
  }

  ExecutionState state;
  InputAqlItemRow input{CreateInvalidInputRowHint{}};

  while (true) {
    std::tie(state, input) = _fetcher.fetchRow();

    if (state == ExecutionState::WAITING) {
      return {state, {}};
    }

    if (state == ExecutionState::DONE) {
      _fetcherDone = true;
    }

    // if we are in the same group, we need to add lines to the current group
    if (_currentGroup.isSameGroup(input)) {  // << returnedDone to check wheter we hit the last row
      _currentGroup.addLine(input);

      if (state == ExecutionState::DONE) {
        TRI_ASSERT(!output.produced());
        _currentGroup.writeToOutput(output);
        // Invalidate group
        input = InputAqlItemRow{CreateInvalidInputRowHint{}};
        _currentGroup.reset(input);
        return {ExecutionState::DONE, {}};
      }
    } else {
      if (_currentGroup.isValid()) {
        // Write the current group.
        // Start a new group from input
        _currentGroup.writeToOutput(output);
        TRI_ASSERT(output.produced());
        _currentGroup.reset(input);  // reset and recreate new group
        if (input.isInitialized()) {
          return {ExecutionState::HASMORE, {}};
        }
        TRI_ASSERT(state == ExecutionState::DONE);
        return {ExecutionState::DONE, {}};
      } else {
        if (!input.isInitialized()) {
          // we got exactly 0 rows as input.
          TRI_ASSERT(state == ExecutionState::DONE);
          return {ExecutionState::DONE, {}};
        }
        // old group was not valid, do not write it
        _currentGroup.reset(input);  // reset and recreate new group
      }
    }
  }
}

// TODOS:

// 5.) build up the aggregators correctly. this seems to be missing right now. they are empty!

// use ./scripts/unittest shell_server_aql --test aql-optimizer-collect-aggregate.js for quick debuging,
// later on all tests of course
