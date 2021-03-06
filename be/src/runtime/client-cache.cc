// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/client-cache.h"

#include <sstream>
#include <thrift/server/TServer.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>
#include <memory>

#include <boost/foreach.hpp>

#include "common/logging.h"
#include "util/thrift-util.h"
#include "gen-cpp/ImpalaInternalService.h"

using namespace std;
using namespace boost;
using namespace apache::thrift;
using namespace apache::thrift::server;
using namespace apache::thrift::transport;
using namespace apache::thrift::protocol;

namespace impala {

BackendClientCache::BackendClientCache(int max_clients, int max_clients_per_backend)
  : max_clients_(max_clients),
    max_clients_per_backend_(max_clients_per_backend) {
}

Status BackendClientCache::GetClient(
    const pair<string, int>& hostport, ImpalaInternalServiceClient** client) {
  VLOG_RPC << "GetClient("
           << hostport.first << ":" << hostport.second << ")";
  lock_guard<mutex> l(lock_);
  ClientCache::iterator cache_entry = client_cache_.find(hostport);
  if (cache_entry == client_cache_.end()) {
    cache_entry =
        client_cache_.insert(make_pair(hostport, list<BackendClient*>())).first;
    DCHECK(cache_entry != client_cache_.end());
  }

  list<BackendClient*>& info_list = cache_entry->second;
  if (!info_list.empty()) {
    *client = info_list.front()->iface();
    VLOG_RPC << "GetClient(): adding client for "
             << info_list.front()->ipaddress()
             << ":" << info_list.front()->port();
    info_list.pop_front();
  } else {
    auto_ptr<BackendClient> info(
        new BackendClient(cache_entry->first.first, cache_entry->first.second));
    RETURN_IF_ERROR(info->Open());
    client_map_[info->iface()] = info.get();
    VLOG_CONNECTION << "GetClient(): creating client for "
                    << info->ipaddress() << ":" << info->port();
    *client = info.release()->iface();
  }

  return Status::OK;
}

Status BackendClientCache::ReopenClient(ImpalaInternalServiceClient* client) {
  lock_guard<mutex> l(lock_);
  ClientMap::iterator i = client_map_.find(client);
  DCHECK(i != client_map_.end());
  BackendClient* info = i->second;
  RETURN_IF_ERROR(info->Close());
  RETURN_IF_ERROR(info->Open());
  return Status::OK;
}

void BackendClientCache::ReleaseClient(ImpalaInternalServiceClient* client) {
  lock_guard<mutex> l(lock_);
  ClientMap::iterator i = client_map_.find(client);
  DCHECK(i != client_map_.end());
  BackendClient* info = i->second;
  VLOG_RPC << "releasing client for "
           << info->ipaddress() << ":" << info->port();
  ClientCache::iterator j =
      client_cache_.find(make_pair(info->ipaddress(), info->port()));
  DCHECK(j != client_cache_.end());
  j->second.push_back(info);
}

void BackendClientCache::CloseConnections(const pair<string, int>& hostport) {
  lock_guard<mutex> l(lock_);
  ClientCache::iterator cache_entry = client_cache_.find(hostport);
  if (cache_entry == client_cache_.end()) return;
  VLOG_RPC << "Invalidating all " << cache_entry->second.size() << " clients for: " 
           << hostport.first << ":" << hostport.second;
  BOOST_FOREACH(BackendClient* client, cache_entry->second) {
    client->Close();
  }
}

string BackendClientCache::DebugString() {
  lock_guard<mutex> l(lock_);
  stringstream out;
  out << "BackendClientCache(#hosts=" << client_cache_.size()
      << " [";
  for (ClientCache::iterator i = client_cache_.begin(); i != client_cache_.end(); ++i) {
    if (i != client_cache_.begin()) out << " ";
    out << i->first.first << ":" << i->first.second << ":" << i->second.size();
  }
  out << "])";
  return out.str();
}

void BackendClientCache::TestShutdown() {
  vector<pair<string, int> > hostports;
  {
    lock_guard<mutex> l(lock_);
    for (ClientCache::iterator i = client_cache_.begin(); i != client_cache_.end();
         ++i) {
      hostports.push_back(i->first);
    }
  }

  for (int i = 0; i < hostports.size(); ++i) {
    CloseConnections(hostports[i]);
  }
}

}
