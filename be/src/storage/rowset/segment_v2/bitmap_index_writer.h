// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/rowset/segment_v2/bitmap_index_writer.h

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstddef>
#include <memory>

#include "common/status.h"
#include "gen_cpp/segment_v2.pb.h"
#include "gutil/macros.h"

namespace starrocks {

class TypeInfo;
using TypeInfoPtr = std::shared_ptr<TypeInfo>;

namespace fs {
class WritableBlock;
}

namespace segment_v2 {

class BitmapIndexWriter {
public:
    static Status create(const TypeInfoPtr& type_info, std::unique_ptr<BitmapIndexWriter>* res);

    BitmapIndexWriter() = default;
    virtual ~BitmapIndexWriter() = default;

    virtual void add_values(const void* values, size_t count) = 0;

    virtual void add_nulls(uint32_t count) = 0;

    virtual Status finish(fs::WritableBlock* file, ColumnIndexMetaPB* index_meta) = 0;

    virtual uint64_t size() const = 0;

private:
    DISALLOW_COPY_AND_ASSIGN(BitmapIndexWriter);
};

} // namespace segment_v2
} // namespace starrocks
