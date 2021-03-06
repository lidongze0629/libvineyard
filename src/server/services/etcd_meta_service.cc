/** Copyright 2020 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "server/services/etcd_meta_service.h"

#include <string>
#include <vector>

#include "etcd/v3/Transaction.hpp"

#include "common/util/boost.h"
#include "common/util/logging.h"

#define BACKOFF_RETRY_TIME 10

namespace vineyard {

void EtcdWatchHandler::operator()(pplx::task<etcd::Response> const& resp_task) {
  this->operator()(resp_task.get());
}

void EtcdWatchHandler::operator()(etcd::Response const& resp) {
  VLOG(10) << "etcd watch use " << resp.duration().count()
           << " microseconds, event size = " << resp.events().size();
  std::vector<EtcdMetaService::op_t> ops;
  ops.reserve(resp.events().size());
  for (auto const& event : resp.events()) {
    std::string const& key = event.kv().key();
    if (!filter_prefix_.empty() &&
        boost::algorithm::starts_with(key, filter_prefix_)) {
      // FIXME: for simplicity, we don't care the instance-lock related keys.
      continue;
    }
    EtcdMetaService::op_t op;
    std::string op_key = boost::algorithm::erase_head_copy(key, prefix_.size());
    switch (event.type()) {
    case mvccpb::Event::PUT: {
      auto op = EtcdMetaService::op_t::Put(op_key, event.kv().value(),
                                           event.kv().mod_revision());
      ops.emplace_back(op);
      break;
    }
    case mvccpb::Event::DELETE: {
      auto op = EtcdMetaService::op_t::Del(op_key, event.kv().mod_revision());
      ops.emplace_back(op);
      break;
    }
    default: {
      // invalid event type.
      break;
    }
    }
  }
  auto status = Status::EtcdError(resp.error_code(), resp.error_message());
  asio::post(ctx_, boost::bind(callback_, status, ops, resp.index()));
}

void EtcdMetaService::requestLock(
    std::string lock_name,
    callback_t<std::shared_ptr<ILock>> callback_after_locked) {
  etcd_->lock(prefix_ + lock_name)
      .then([this, callback_after_locked](
                pplx::task<etcd::Response> const& resp_task) {
        auto const& resp = resp_task.get();
        VLOG(10) << "etcd lock use " << resp.duration().count()
                 << " microseconds";
        auto lock_key = resp.lock_key();
        auto lock_ptr = std::make_shared<EtcdLock>(
            [this, lock_key](const Status& status, unsigned& rev) {
              // ensure the lock get released.
              auto unlock_resp = this->etcd_->unlock(lock_key).get();
              if (unlock_resp.is_ok()) {
                rev = unlock_resp.index();
              }
              return Status::EtcdError(unlock_resp.error_code(),
                                       unlock_resp.error_message());
            },
            resp.index());
        auto status =
            Status::EtcdError(resp.error_code(), resp.error_message());
        boost::asio::post(server_ptr_->GetIOContext(),
                          boost::bind(callback_after_locked, status, lock_ptr));
      });
}

void EtcdMetaService::commitUpdates(
    const std::vector<op_t>& changes,
    callback_t<unsigned> callback_after_updated) {
  etcdv3::Transaction tx;
  for (auto const& op : changes) {
    if (op.op == op_t::kPut) {
      tx.setup_put(prefix_ + op.kv.key, op.kv.value);
    } else if (op.op == op_t::kDel) {
      tx.setup_delete(prefix_ + op.kv.key);
    }
  }
  etcd_->txn(tx).then([this, callback_after_updated](
                          pplx::task<etcd::Response> const& resp_task) {
    auto resp = resp_task.get();
    VLOG(10) << "etcd txn use " << resp.duration().count() << " microseconds";
    auto status = Status::EtcdError(resp.error_code(), resp.error_message());
    boost::asio::post(
        server_ptr_->GetIOContext(),
        boost::bind(callback_after_updated, status, resp.index()));
  });
}

void EtcdMetaService::requestAll(
    const std::string& prefix, unsigned base_rev,
    callback_t<const std::vector<kv_t>&, unsigned> callback) {
  etcd_->ls(prefix_ + prefix)
      .then([this, callback](pplx::task<etcd::Response> resp_task) {
        auto resp = resp_task.get();
        VLOG(10) << "etcd ls use " << resp.duration().count()
                 << " microseconds";
        std::vector<IMetaService::kv_t> kvs(resp.keys().size());
        for (size_t i = 0; i < resp.keys().size(); ++i) {
          IMetaService::kv_t kv;
          kv.key =
              boost::algorithm::erase_head_copy(resp.key(i), prefix_.size());
          kv.value = resp.value(i).as_string();
          kvs.emplace_back(kv);
        }
        auto status =
            Status::EtcdError(resp.error_code(), resp.error_message());
        boost::asio::post(server_ptr_->GetIOContext(),
                          boost::bind(callback, status, kvs, resp.index()));
      });
}

void EtcdMetaService::requestUpdates(
    const std::string& prefix, unsigned since_rev,
    callback_t<const std::vector<op_t>&, unsigned> callback) {
  // NB: watching from latest version (since_rev) + 1
  etcd_->watch(prefix_ + prefix, since_rev + 1, true)
      .then(EtcdWatchHandler(server_ptr_->GetIOContext(), callback, prefix_,
                             prefix_ + meta_sync_lock_));
}

void EtcdMetaService::startDaemonWatch(
    const std::string& prefix, unsigned since_rev,
    callback_t<const std::vector<op_t>&, unsigned> callback) {
  try {
    this->watcher_.reset(new etcd::Watcher(
        *etcd_, prefix_ + prefix, since_rev + 1,
        EtcdWatchHandler(server_ptr_->GetIOContext(), callback, prefix_,
                         prefix_ + meta_sync_lock_),
        true));
    this->watcher_->Wait([this, prefix, callback](bool cancalled) {
      if (cancalled) {
        return;
      }
      this->retryDaeminWatch(prefix, this->rev_, callback);
    });
  } catch (std::runtime_error& e) {
    LOG(ERROR) << "Failed to create daemon etcd watcher: " << e.what();
    this->retryDaeminWatch(prefix, since_rev, callback);
  }
}

void EtcdMetaService::retryDaeminWatch(
    const std::string& prefix, unsigned since_rev,
    callback_t<const std::vector<op_t>&, unsigned> callback) {
  backoff_timer_.reset(new asio::steady_timer(
      server_ptr_->GetIOContext(), asio::chrono::seconds(BACKOFF_RETRY_TIME)));
  backoff_timer_->async_wait([this, prefix, since_rev, callback](
                                 const boost::system::error_code& error) {
    if (error) {
      LOG(ERROR) << "backoff timer error: " << error << ", " << error.message();
    }
    // retry
    LOG(INFO) << "retrying to connect etcd...";
    this->startDaemonWatch(prefix, since_rev, callback);
  });
}

}  // namespace vineyard
