/*
 * Copyright 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "l2cap/le/facade.h"

#include "grpc/grpc_event_queue.h"
#include "l2cap/le/dynamic_channel.h"
#include "l2cap/le/dynamic_channel_manager.h"
#include "l2cap/le/dynamic_channel_service.h"
#include "l2cap/le/facade.grpc.pb.h"
#include "l2cap/le/l2cap_le_module.h"
#include "l2cap/psm.h"
#include "packet/raw_builder.h"

namespace bluetooth {
namespace l2cap {
namespace le {

class L2capLeModuleFacadeService : public L2capLeModuleFacade::Service {
 public:
  L2capLeModuleFacadeService(L2capLeModule* l2cap_layer, os::Handler* facade_handler)
      : l2cap_layer_(l2cap_layer), facade_handler_(facade_handler) {
    ASSERT(l2cap_layer_ != nullptr);
    ASSERT(facade_handler_ != nullptr);
  }

  ::grpc::Status FetchL2capData(::grpc::ServerContext* context, const ::google::protobuf::Empty* request,
                                ::grpc::ServerWriter<::bluetooth::l2cap::le::L2capPacket>* writer) override {
    return pending_l2cap_data_.RunLoop(context, writer);
  }

  ::grpc::Status OpenDynamicChannel(::grpc::ServerContext* context, const OpenDynamicChannelRequest* request,
                                    OpenDynamicChannelResponse* response) override {
    auto service_helper = dynamic_channel_helper_map_.find(request->psm());
    if (service_helper == dynamic_channel_helper_map_.end()) {
      return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "Psm not registered");
    }
    request->remote();
    hci::Address peer_address;
    ASSERT(hci::Address::FromString(request->remote().address().address(), peer_address));
    // TODO: Support different address type
    hci::AddressWithType peer(peer_address, hci::AddressType::RANDOM_DEVICE_ADDRESS);
    service_helper->second->Connect(peer);
    response->set_status(
        static_cast<int>(service_helper->second->channel_open_fail_reason_.l2cap_connection_response_result));
    return ::grpc::Status::OK;
  }

  ::grpc::Status CloseDynamicChannel(::grpc::ServerContext* context, const CloseDynamicChannelRequest* request,
                                     ::google::protobuf::Empty* response) override {
    auto service_helper = dynamic_channel_helper_map_.find(request->psm());
    if (service_helper == dynamic_channel_helper_map_.end()) {
      return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "Psm not registered");
    }
    if (service_helper->second->channel_ == nullptr) {
      return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "Channel not open");
    }
    service_helper->second->channel_->Close();
    return ::grpc::Status::OK;
  }

  ::grpc::Status SetDynamicChannel(::grpc::ServerContext* context,
                                   const ::bluetooth::l2cap::le::SetEnableDynamicChannelRequest* request,
                                   ::google::protobuf::Empty* response) override {
    if (request->enable()) {
      dynamic_channel_helper_map_.emplace(request->psm(), std::make_unique<L2capDynamicChannelHelper>(
                                                              this, l2cap_layer_, facade_handler_, request->psm()));
      return ::grpc::Status::OK;
    } else {
      auto service_helper = dynamic_channel_helper_map_.find(request->psm());
      if (service_helper == dynamic_channel_helper_map_.end()) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "Psm not registered");
      }
      service_helper->second->service_->Unregister(common::BindOnce([] {}), facade_handler_);
      return ::grpc::Status::OK;
    }
  }

  ::grpc::Status SendDynamicChannelPacket(::grpc::ServerContext* context,
                                          const ::bluetooth::l2cap::le::DynamicChannelPacket* request,
                                          ::google::protobuf::Empty* response) override {
    std::unique_lock<std::mutex> lock(channel_map_mutex_);
    if (dynamic_channel_helper_map_.find(request->psm()) == dynamic_channel_helper_map_.end()) {
      return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "Psm not registered");
    }
    std::vector<uint8_t> packet(request->payload().begin(), request->payload().end());
    if (!dynamic_channel_helper_map_[request->psm()]->SendPacket(packet)) {
      return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "Channel not open");
    }
    return ::grpc::Status::OK;
  }

  class L2capDynamicChannelHelper {
   public:
    L2capDynamicChannelHelper(L2capLeModuleFacadeService* service, L2capLeModule* l2cap_layer, os::Handler* handler,
                              Psm psm)
        : facade_service_(service), l2cap_layer_(l2cap_layer), handler_(handler), psm_(psm) {
      dynamic_channel_manager_ = l2cap_layer_->GetDynamicChannelManager();
      dynamic_channel_manager_->RegisterService(
          psm, {}, {},
          common::BindOnce(&L2capDynamicChannelHelper::on_l2cap_service_registration_complete,
                           common::Unretained(this)),
          common::Bind(&L2capDynamicChannelHelper::on_connection_open, common::Unretained(this)), handler_);
    }

    ~L2capDynamicChannelHelper() {
      if (channel_ != nullptr) {
        channel_->GetQueueUpEnd()->UnregisterDequeue();
        channel_ = nullptr;
      }
    }

    void Connect(hci::AddressWithType address) {
      dynamic_channel_manager_->ConnectChannel(
          address, {}, psm_, common::Bind(&L2capDynamicChannelHelper::on_connection_open, common::Unretained(this)),
          common::Bind(&L2capDynamicChannelHelper::on_connect_fail, common::Unretained(this)), handler_);
      std::unique_lock<std::mutex> lock(channel_open_cv_mutex_);
      if (!channel_open_cv_.wait_for(lock, std::chrono::seconds(2), [this] { return channel_ != nullptr; })) {
        LOG_WARN("Channel is not open for psm %d", psm_);
      }
    }

    void disconnect() {
      channel_->Close();
    }

    void on_l2cap_service_registration_complete(DynamicChannelManager::RegistrationResult registration_result,
                                                std::unique_ptr<DynamicChannelService> service) {
      if (registration_result != DynamicChannelManager::RegistrationResult::SUCCESS) {
        LOG_ERROR("Service registration failed");
      } else {
        service_ = std::move(service);
      }
    }

    // invoked from Facade Handler
    void on_connection_open(std::unique_ptr<DynamicChannel> channel) {
      {
        std::unique_lock<std::mutex> lock(channel_open_cv_mutex_);
        channel_ = std::move(channel);
      }
      channel_open_cv_.notify_all();
      channel_->RegisterOnCloseCallback(
          facade_service_->facade_handler_,
          common::BindOnce(&L2capDynamicChannelHelper::on_close_callback, common::Unretained(this)));
      channel_->GetQueueUpEnd()->RegisterDequeue(
          facade_service_->facade_handler_,
          common::Bind(&L2capDynamicChannelHelper::on_incoming_packet, common::Unretained(this)));
    }

    void on_close_callback(hci::ErrorCode error_code) {
      {
        std::unique_lock<std::mutex> lock(channel_open_cv_mutex_);
        channel_->GetQueueUpEnd()->UnregisterDequeue();
      }
      channel_ = nullptr;
    }

    void on_connect_fail(DynamicChannelManager::ConnectionResult result) {
      {
        std::unique_lock<std::mutex> lock(channel_open_cv_mutex_);
        channel_ = nullptr;
        channel_open_fail_reason_ = result;
      }
      channel_open_cv_.notify_all();
    }

    void on_incoming_packet() {
      auto packet = channel_->GetQueueUpEnd()->TryDequeue();
      std::string data = std::string(packet->begin(), packet->end());
      L2capPacket l2cap_data;
      l2cap_data.set_psm(psm_);
      l2cap_data.set_payload(data);
      facade_service_->pending_l2cap_data_.OnIncomingEvent(l2cap_data);
    }

    bool SendPacket(std::vector<uint8_t> packet) {
      if (channel_ == nullptr) {
        std::unique_lock<std::mutex> lock(channel_open_cv_mutex_);
        if (!channel_open_cv_.wait_for(lock, std::chrono::seconds(2), [this] { return channel_ != nullptr; })) {
          LOG_WARN("Channel is not open for psm %d", psm_);
          return false;
        }
      }
      std::promise<void> promise;
      auto future = promise.get_future();
      channel_->GetQueueUpEnd()->RegisterEnqueue(
          handler_, common::Bind(&L2capDynamicChannelHelper::enqueue_callback, common::Unretained(this), packet,
                                 common::Passed(std::move(promise))));
      auto status = future.wait_for(std::chrono::milliseconds(500));
      if (status != std::future_status::ready) {
        LOG_ERROR("Can't send packet because the previous packet wasn't sent yet");
        return false;
      }
      return true;
    }

    std::unique_ptr<packet::BasePacketBuilder> enqueue_callback(std::vector<uint8_t> packet,
                                                                std::promise<void> promise) {
      auto packet_one = std::make_unique<packet::RawBuilder>(2000);
      packet_one->AddOctets(packet);
      channel_->GetQueueUpEnd()->UnregisterEnqueue();
      promise.set_value();
      return packet_one;
    };

    L2capLeModuleFacadeService* facade_service_;
    L2capLeModule* l2cap_layer_;
    os::Handler* handler_;
    std::unique_ptr<DynamicChannelManager> dynamic_channel_manager_;
    std::unique_ptr<DynamicChannelService> service_;
    std::unique_ptr<DynamicChannel> channel_ = nullptr;
    Psm psm_;
    DynamicChannelManager::ConnectionResult channel_open_fail_reason_;
    std::condition_variable channel_open_cv_;
    std::mutex channel_open_cv_mutex_;
  };

  L2capLeModule* l2cap_layer_;
  os::Handler* facade_handler_;
  std::mutex channel_map_mutex_;
  std::map<Psm, std::unique_ptr<L2capDynamicChannelHelper>> dynamic_channel_helper_map_;
  ::bluetooth::grpc::GrpcEventQueue<L2capPacket> pending_l2cap_data_{"FetchL2capData"};
};

void L2capLeModuleFacadeModule::ListDependencies(ModuleList* list) {
  ::bluetooth::grpc::GrpcFacadeModule::ListDependencies(list);
  list->add<l2cap::le::L2capLeModule>();
}

void L2capLeModuleFacadeModule::Start() {
  ::bluetooth::grpc::GrpcFacadeModule::Start();
  service_ = new L2capLeModuleFacadeService(GetDependency<l2cap::le::L2capLeModule>(), GetHandler());
}

void L2capLeModuleFacadeModule::Stop() {
  delete service_;
  ::bluetooth::grpc::GrpcFacadeModule::Stop();
}

::grpc::Service* L2capLeModuleFacadeModule::GetService() const {
  return service_;
}

const ModuleFactory L2capLeModuleFacadeModule::Factory =
    ::bluetooth::ModuleFactory([]() { return new L2capLeModuleFacadeModule(); });

}  // namespace le
}  // namespace l2cap
}  // namespace bluetooth
