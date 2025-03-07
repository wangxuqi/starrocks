// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/util/brpc_stub_cache.h

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

#include <memory>
#include <mutex>

#include "gen_cpp/Types_types.h" // TNetworkAddress
#include "gen_cpp/internal_service.pb.h"
#include "gen_cpp/starrocks_internal_service.pb.h"
#include "service/brpc.h"
#include "util/spinlock.h"
#include "util/starrocks_metrics.h"

namespace starrocks {

// map used
class BrpcStubCache {
public:
    BrpcStubCache() {
        _stub_map.init(239);
        REGISTER_GAUGE_STARROCKS_METRIC(brpc_endpoint_stub_count, [this]() {
            std::lock_guard<SpinLock> l(_lock);
            return _stub_map.size();
        });
    }
    ~BrpcStubCache() {
        for (auto& stub : _stub_map) {
            delete stub.second;
        }
    }

    PBackendService_Stub* get_stub(const butil::EndPoint& endpoint) {
        std::lock_guard<SpinLock> l(_lock);
        auto stub_ptr = _stub_map.seek(endpoint);
        if (stub_ptr != nullptr) {
            return *stub_ptr;
        }
        // new one stub and insert into map
        brpc::ChannelOptions options;
        std::unique_ptr<brpc::Channel> channel(new brpc::Channel());
        if (channel->Init(endpoint, &options)) {
            return nullptr;
        }
        auto stub = new PBackendService_Stub(channel.release(), google::protobuf::Service::STUB_OWNS_CHANNEL);
        _stub_map.insert(endpoint, stub);
        return stub;
    }

    PBackendService_Stub* get_stub(const TNetworkAddress& taddr) {
        butil::EndPoint endpoint;
        if (str2endpoint(taddr.hostname.c_str(), taddr.port, &endpoint)) {
            LOG(WARNING) << "unknown endpoint, hostname=" << taddr.hostname;
            return nullptr;
        }
        return get_stub(endpoint);
    }

    PBackendService_Stub* get_stub(const std::string& host, int port) {
        butil::EndPoint endpoint;
        if (str2endpoint(host.c_str(), port, &endpoint)) {
            LOG(WARNING) << "unknown endpoint, hostname=" << host;
            return nullptr;
        }
        return get_stub(endpoint);
    }

private:
    SpinLock _lock;
    butil::FlatMap<butil::EndPoint, PBackendService_Stub*> _stub_map;
};

} // namespace starrocks
