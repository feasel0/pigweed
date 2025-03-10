// Copyright 2023 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_advertiser.h"

#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {
namespace pwemb = pw::bluetooth::emboss;

ExtendedLowEnergyAdvertiser::ExtendedLowEnergyAdvertiser(
    hci::Transport::WeakPtr hci_ptr)
    : LowEnergyAdvertiser(std::move(hci_ptr)) {
  event_handler_id_ = hci()->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLEAdvertisingSetTerminatedSubeventCode,
      [this](const EventPacket& event) {
        OnAdvertisingSetTerminatedEvent(event);
        return CommandChannel::EventCallbackResult::kContinue;
      });
}

ExtendedLowEnergyAdvertiser::~ExtendedLowEnergyAdvertiser() {
  // This object is probably being destroyed because the stack is shutting down,
  // in which case the HCI layer may have already been destroyed.
  if (!hci().is_alive() || !hci()->command_channel()) {
    return;
  }

  hci()->command_channel()->RemoveEventHandler(event_handler_id_);

  // TODO(fxbug.dev/42063496): This will only cancel one advertisement,
  // after which the SequentialCommandRunner will have been destroyed and no
  // further commands will be sent.
  StopAdvertising();
}

size_t ExtendedLowEnergyAdvertiser::GetSizeLimit(bool extended_pdu) const {
  if (extended_pdu) {
    return hci_spec::kMaxLEExtendedAdvertisingDataLength;
  }

  return hci_spec::kMaxLEAdvertisingDataLength;
}

ExtendedLowEnergyAdvertiser::AdvertisingEventPropertiesBits
ExtendedLowEnergyAdvertiser::AdvertisingTypeToLegacyPduEventBits(
    pwemb::LEAdvertisingType type) {
  ExtendedLowEnergyAdvertiser::AdvertisingEventPropertiesBits bits =
      hci_spec::kLEAdvEventPropBitUseLegacyPDUs;

  // Bluetooth Spec Volume 4, Part E, Section 7.8.53, Table 7.2 defines the
  // mapping of legacy PDU types to the corresponding bits within
  // adv_event_properties.
  if (type == pwemb::LEAdvertisingType::CONNECTABLE_AND_SCANNABLE_UNDIRECTED) {
    bits |= hci_spec::kLEAdvEventPropBitConnectable;
    bits |= hci_spec::kLEAdvEventPropBitScannable;
  } else if (type ==
             pwemb::LEAdvertisingType::CONNECTABLE_LOW_DUTY_CYCLE_DIRECTED) {
    bits |= hci_spec::kLEAdvEventPropBitConnectable;
    bits |= hci_spec::kLEAdvEventPropBitDirected;
  } else if (type ==
             pwemb::LEAdvertisingType::CONNECTABLE_HIGH_DUTY_CYCLE_DIRECTED) {
    bits |= hci_spec::kLEAdvEventPropBitConnectable;
    bits |= hci_spec::kLEAdvEventPropBitDirected;
    bits |= hci_spec::kLEAdvEventPropBitHighDutyCycleDirectedConnectable;
  } else if (type == pwemb::LEAdvertisingType::SCANNABLE_UNDIRECTED) {
    bits |= hci_spec::kLEAdvEventPropBitScannable;
  } else if (type == pwemb::LEAdvertisingType::NOT_CONNECTABLE_UNDIRECTED) {
    // no extra bits set
  }

  return bits;
}

EmbossCommandPacket ExtendedLowEnergyAdvertiser::BuildEnablePacket(
    const DeviceAddress& address,
    pwemb::GenericEnableParam enable,
    bool extended_pdu) {
  // We only enable or disable a single address at a time. The multiply by 1 is
  // set explicitly to show that data[] within
  // LESetExtendedAdvertisingEnableData is of size 1.
  constexpr size_t kPacketSize =
      pwemb::LESetExtendedAdvertisingEnableCommand::MinSizeInBytes() +
      (1 * pwemb::LESetExtendedAdvertisingEnableData::IntrinsicSizeInBytes());
  auto packet = hci::EmbossCommandPacket::New<
      pwemb::LESetExtendedAdvertisingEnableCommandWriter>(
      hci_spec::kLESetExtendedAdvertisingEnable, kPacketSize);
  auto view = packet.view_t();
  view.enable().Write(enable);
  view.num_sets().Write(1);

  std::optional<hci_spec::AdvertisingHandle> handle =
      advertising_handle_map_.GetHandle(address, extended_pdu);
  BT_ASSERT(handle);

  view.data()[0].advertising_handle().Write(handle.value());
  view.data()[0].duration().Write(hci_spec::kNoAdvertisingDuration);
  view.data()[0].max_extended_advertising_events().Write(
      hci_spec::kNoMaxExtendedAdvertisingEvents);

  return packet;
}

std::optional<EmbossCommandPacket>
ExtendedLowEnergyAdvertiser::BuildSetAdvertisingParams(
    const DeviceAddress& address,
    pwemb::LEAdvertisingType type,
    pwemb::LEOwnAddressType own_address_type,
    const AdvertisingIntervalRange& interval,
    bool extended_pdu) {
  auto packet = hci::EmbossCommandPacket::New<
      pwemb::LESetExtendedAdvertisingParametersV1CommandWriter>(
      hci_spec::kLESetExtendedAdvertisingParameters);
  auto view = packet.view_t();

  // advertising handle
  std::optional<hci_spec::AdvertisingHandle> handle =
      advertising_handle_map_.MapHandle(address, extended_pdu);
  if (!handle) {
    bt_log(WARN,
           "hci-le",
           "could not allocate advertising handle for address: %s",
           bt_str(address));
    return std::nullopt;
  }
  view.advertising_handle().Write(handle.value());

  // advertising event properties
  std::optional<AdvertisingEventPropertiesBits> bits =
      AdvertisingTypeToLegacyPduEventBits(type);
  if (!bits) {
    bt_log(WARN,
           "hci-le",
           "could not generate event bits for type: %hhu",
           static_cast<unsigned char>(type));
    return std::nullopt;
  }
  uint16_t properties = bits.value();
  view.advertising_event_properties().BackingStorage().WriteUInt(properties);

  // advertising interval, NOTE: LE advertising parameters allow for up to 3
  // octets (10 ms to 10428 s) to configure an advertising interval. However, we
  // expose only the recommended advertising interval configurations to users,
  // as specified in the Bluetooth Spec Volume 3, Part C, Appendix A. These
  // values are expressed as uint16_t so we simply copy them (taking care of
  // endianness) into the 3 octets as is.
  view.primary_advertising_interval_min().Write(interval.min());
  view.primary_advertising_interval_max().Write(interval.max());

  // advertise on all channels
  view.primary_advertising_channel_map().channel_37().Write(true);
  view.primary_advertising_channel_map().channel_38().Write(true);
  view.primary_advertising_channel_map().channel_39().Write(true);

  view.own_address_type().Write(own_address_type);
  view.advertising_filter_policy().Write(
      pwemb::LEAdvertisingFilterPolicy::ALLOW_ALL);
  view.advertising_tx_power().Write(
      hci_spec::kLEExtendedAdvertisingTxPowerNoPreference);
  view.scan_request_notification_enable().Write(
      pwemb::GenericEnableParam::DISABLE);

  // TODO(fxbug.dev/42161929): using legacy PDUs requires advertisements
  // on the LE 1M PHY.
  view.primary_advertising_phy().Write(pwemb::LEPrimaryAdvertisingPHY::LE_1M);
  view.secondary_advertising_phy().Write(
      pwemb::LESecondaryAdvertisingPHY::LE_1M);

  // Payload values were initialized to zero above. By not setting the values
  // for the following fields, we are purposely ignoring them:
  //
  // advertising_sid: We use only legacy PDUs, the controller ignores this field
  // in that case peer_address: We don't support directed advertising yet
  // peer_address_type: We don't support directed advertising yet
  // secondary_adv_max_skip: We use only legacy PDUs, the controller ignores
  // this field in that case

  return packet;
}

std::vector<EmbossCommandPacket>
ExtendedLowEnergyAdvertiser::BuildSetAdvertisingData(
    const DeviceAddress& address,
    const AdvertisingData& data,
    AdvFlags flags,
    bool extended_pdu) {
  AdvertisingData adv_data;
  data.Copy(&adv_data);
  if (staged_advertising_parameters_.include_tx_power_level) {
    adv_data.SetTxPower(staged_advertising_parameters_.selected_tx_power_level);
  }
  size_t block_size = adv_data.CalculateBlockSize(/*include_flags=*/true);

  size_t kPayloadSize =
      pwemb::LESetExtendedAdvertisingDataCommandView::MinSizeInBytes().Read() +
      block_size;
  auto packet = EmbossCommandPacket::New<
      pwemb::LESetExtendedAdvertisingDataCommandWriter>(
      hci_spec::kLESetExtendedAdvertisingData, kPayloadSize);
  auto params = packet.view_t();

  // advertising handle
  std::optional<hci_spec::AdvertisingHandle> handle =
      advertising_handle_map_.GetHandle(address, extended_pdu);
  BT_ASSERT(handle);
  params.advertising_handle().Write(handle.value());

  // TODO(fxbug.dev/42161929): We support only legacy PDUs and do not support
  // fragmented extended advertising data at this time.
  params.operation().Write(
      pw::bluetooth::emboss::LESetExtendedAdvDataOp::COMPLETE);
  params.fragment_preference().Write(
      pw::bluetooth::emboss::LEExtendedAdvFragmentPreference::
          SHOULD_NOT_FRAGMENT);

  // advertising data
  params.advertising_data_length().Write(static_cast<uint8_t>(block_size));
  MutableBufferView data_view(params.advertising_data().BackingStorage().data(),
                              params.advertising_data_length().Read());
  adv_data.WriteBlock(&data_view, flags);

  std::vector<EmbossCommandPacket> packets;
  packets.reserve(1);
  packets.emplace_back(std::move(packet));
  return packets;
}

EmbossCommandPacket ExtendedLowEnergyAdvertiser::BuildUnsetAdvertisingData(
    const DeviceAddress& address, bool extended_pdu) {
  constexpr size_t kPacketSize =
      pwemb::LESetExtendedAdvertisingDataCommandView::MinSizeInBytes().Read();
  auto packet = EmbossCommandPacket::New<
      pwemb::LESetExtendedAdvertisingDataCommandWriter>(
      hci_spec::kLESetExtendedAdvertisingData, kPacketSize);
  auto payload = packet.view_t();

  // advertising handle
  std::optional<hci_spec::AdvertisingHandle> handle =
      advertising_handle_map_.GetHandle(address, extended_pdu);
  BT_ASSERT(handle);
  payload.advertising_handle().Write(handle.value());

  // TODO(fxbug.dev/42161929): We support only legacy PDUs and do not
  // support fragmented extended advertising data at this time.
  payload.operation().Write(pwemb::LESetExtendedAdvDataOp::COMPLETE);
  payload.fragment_preference().Write(
      pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);
  payload.advertising_data_length().Write(0);

  return packet;
}

std::vector<EmbossCommandPacket>
ExtendedLowEnergyAdvertiser::BuildSetScanResponse(const DeviceAddress& address,
                                                  const AdvertisingData& data,
                                                  bool extended_pdu) {
  AdvertisingData scan_rsp;
  data.Copy(&scan_rsp);
  if (staged_advertising_parameters_.include_tx_power_level) {
    scan_rsp.SetTxPower(staged_advertising_parameters_.selected_tx_power_level);
  }
  size_t block_size = scan_rsp.CalculateBlockSize();

  size_t kPayloadSize =
      pwemb::LESetExtendedScanResponseDataCommandView::MinSizeInBytes().Read() +
      block_size;
  auto packet = EmbossCommandPacket::New<
      pwemb::LESetExtendedScanResponseDataCommandWriter>(
      hci_spec::kLESetExtendedScanResponseData, kPayloadSize);
  auto params = packet.view_t();

  // advertising handle
  std::optional<hci_spec::AdvertisingHandle> handle =
      advertising_handle_map_.GetHandle(address, extended_pdu);
  BT_ASSERT(handle);
  params.advertising_handle().Write(handle.value());

  // TODO(fxbug.dev/42161929): We support only legacy PDUs and do not
  // support fragmented extended advertising data at this time.
  params.operation().Write(pwemb::LESetExtendedAdvDataOp::COMPLETE);
  params.fragment_preference().Write(
      pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);

  // scan response data
  params.scan_response_data_length().Write(static_cast<uint8_t>(block_size));
  MutableBufferView scan_rsp_view(
      params.scan_response_data().BackingStorage().data(),
      params.scan_response_data_length().Read());
  scan_rsp.WriteBlock(&scan_rsp_view, std::nullopt);

  std::vector<EmbossCommandPacket> packets;
  packets.reserve(1);
  packets.emplace_back(std::move(packet));
  return packets;
}

EmbossCommandPacket ExtendedLowEnergyAdvertiser::BuildUnsetScanResponse(
    const DeviceAddress& address, bool extended_pdu) {
  constexpr size_t kPacketSize =
      pwemb::LESetExtendedScanResponseDataCommandView::MinSizeInBytes().Read();
  auto packet = EmbossCommandPacket::New<
      pwemb::LESetExtendedScanResponseDataCommandWriter>(
      hci_spec::kLESetExtendedScanResponseData, kPacketSize);
  auto payload = packet.view_t();

  // advertising handle
  std::optional<hci_spec::AdvertisingHandle> handle =
      advertising_handle_map_.GetHandle(address, extended_pdu);
  BT_ASSERT(handle);
  payload.advertising_handle().Write(handle.value());

  // TODO(fxbug.dev/42161929): We support only legacy PDUs and do not
  // support fragmented extended advertising data at this time.
  payload.operation().Write(pwemb::LESetExtendedAdvDataOp::COMPLETE);
  payload.fragment_preference().Write(
      pwemb::LEExtendedAdvFragmentPreference::SHOULD_NOT_FRAGMENT);
  payload.scan_response_data_length().Write(0);

  return packet;
}

EmbossCommandPacket ExtendedLowEnergyAdvertiser::BuildRemoveAdvertisingSet(
    const DeviceAddress& address, bool extended_pdu) {
  std::optional<hci_spec::AdvertisingHandle> handle =
      advertising_handle_map_.GetHandle(address, extended_pdu);
  BT_ASSERT(handle);
  auto packet =
      hci::EmbossCommandPacket::New<pwemb::LERemoveAdvertisingSetCommandWriter>(
          hci_spec::kLERemoveAdvertisingSet);
  auto view = packet.view_t();
  view.advertising_handle().Write(handle.value());

  return packet;
}

void ExtendedLowEnergyAdvertiser::OnSetAdvertisingParamsComplete(
    const EventPacket& event) {
  BT_ASSERT(event.event_code() == hci_spec::kCommandCompleteEventCode);
  BT_ASSERT(
      event.params<hci_spec::CommandCompleteEventParams>().command_opcode ==
      hci_spec::kLESetExtendedAdvertisingParameters);

  Result<> result = event.ToResult();
  if (bt_is_error(result,
                  WARN,
                  "hci-le",
                  "set advertising parameters, error received: %s",
                  bt_str(result))) {
    return;  // full error handling done in super class, can just return here
  }

  auto params = event.return_params<
      hci_spec::LESetExtendedAdvertisingParametersReturnParams>();
  BT_ASSERT(params);

  if (staged_advertising_parameters_.include_tx_power_level) {
    staged_advertising_parameters_.selected_tx_power_level =
        params->selected_tx_power;
  }
}

void ExtendedLowEnergyAdvertiser::StartAdvertising(
    const DeviceAddress& address,
    const AdvertisingData& data,
    const AdvertisingData& scan_rsp,
    const AdvertisingOptions& options,
    ConnectionCallback connect_callback,
    ResultFunction<> result_callback) {
  // if there is an operation currently in progress, enqueue this operation and
  // we will get to it the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(INFO,
           "hci-le",
           "hci cmd runner not ready, queuing advertisement commands for now");

    AdvertisingData copied_data;
    data.Copy(&copied_data);

    AdvertisingData copied_scan_rsp;
    scan_rsp.Copy(&copied_scan_rsp);

    op_queue_.push([this,
                    address,
                    data = std::move(copied_data),
                    scan_rsp = std::move(copied_scan_rsp),
                    options,
                    conn_cb = std::move(connect_callback),
                    result_cb = std::move(result_callback)]() mutable {
      StartAdvertising(address,
                       data,
                       scan_rsp,
                       options,
                       std::move(conn_cb),
                       std::move(result_cb));
    });

    return;
  }

  fit::result<HostError> result =
      CanStartAdvertising(address, data, scan_rsp, options);
  if (result.is_error()) {
    result_callback(ToResult(result.error_value()));
    return;
  }

  if (IsAdvertising(address, options.extended_pdu)) {
    bt_log(DEBUG,
           "hci-le",
           "updating existing advertisement for %s",
           bt_str(address));
  }

  staged_advertising_parameters_.clear();
  staged_advertising_parameters_.include_tx_power_level =
      options.include_tx_power_level;
  staged_advertising_parameters_.extended_pdu = options.extended_pdu;

  // Core Spec, Volume 4, Part E, Section 7.8.58: "the number of advertising
  // sets that can be supported is not fixed and the Controller can change it at
  // any time. The memory used to store advertising sets can also be used for
  // other purposes."
  //
  // Depending on the memory profile of the controller, a new advertising set
  // may or may not be accepted. We could use
  // HCI_LE_Read_Number_of_Supported_Advertising_Sets to check if the controller
  // has space for another advertising set. However, the value may change after
  // the read and before the addition of the advertising set. Furthermore,
  // sending an extra HCI command increases the latency of our stack. Instead,
  // we simply attempt to add. If the controller is unable to support another
  // advertising set, it will respond with a memory capacity exceeded error.
  StartAdvertisingInternal(address,
                           data,
                           scan_rsp,
                           options,
                           std::move(connect_callback),
                           std::move(result_callback));
}

void ExtendedLowEnergyAdvertiser::StopAdvertising() {
  LowEnergyAdvertiser::StopAdvertising();
  advertising_handle_map_.Clear();

  // std::queue doesn't have a clear method so we have to resort to this
  // tomfoolery :(
  decltype(op_queue_) empty;
  std::swap(op_queue_, empty);
}

void ExtendedLowEnergyAdvertiser::StopAdvertising(const DeviceAddress& address,
                                                  bool extended_pdu) {
  // if there is an operation currently in progress, enqueue this operation and
  // we will get to it the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(
        INFO,
        "hci-le",
        "hci cmd runner not ready, queueing stop advertising command for now");
    op_queue_.push([this, address, extended_pdu]() {
      StopAdvertising(address, extended_pdu);
    });
    return;
  }

  LowEnergyAdvertiser::StopAdvertisingInternal(address, extended_pdu);
  advertising_handle_map_.RemoveAddress(address, extended_pdu);
}

void ExtendedLowEnergyAdvertiser::OnIncomingConnection(
    hci_spec::ConnectionHandle handle,
    pwemb::ConnectionRole role,
    const DeviceAddress& peer_address,
    const hci_spec::LEConnectionParameters& conn_params) {
  // Core Spec Volume 4, Part E, Section 7.8.56: Incoming connections to LE
  // Extended Advertising occur through two events: HCI_LE_Connection_Complete
  // and HCI_LE_Advertising_Set_Terminated. This method is called as a result of
  // the HCI_LE_Connection_Complete event. At this point, we only have a
  // connection handle but don't know the locally advertised address that the
  // connection is for. Until we receive the HCI_LE_Advertising_Set_Terminated
  // event, we stage these parameters.
  staged_connections_[handle] = {role, peer_address, conn_params};
}

// The HCI_LE_Advertising_Set_Terminated event contains the mapping between
// connection handle and advertising handle. After the
// HCI_LE_Advertising_Set_Terminated event, we have all the information
// necessary to create a connection object within the Host layer.
void ExtendedLowEnergyAdvertiser::OnAdvertisingSetTerminatedEvent(
    const EventPacket& event) {
  BT_ASSERT(event.event_code() == hci_spec::kLEMetaEventCode);
  BT_ASSERT(event.params<hci_spec::LEMetaEventParams>().subevent_code ==
            hci_spec::kLEAdvertisingSetTerminatedSubeventCode);

  Result<> result = event.ToResult();
  if (bt_is_error(result,
                  ERROR,
                  "hci-le",
                  "advertising set terminated event, error received %s",
                  bt_str(result))) {
    return;
  }

  auto params = event.subevent_params<
      hci_spec::LEAdvertisingSetTerminatedSubeventParams>();
  BT_ASSERT(params);

  hci_spec::ConnectionHandle connection_handle = params->connection_handle;
  auto staged_parameters_node = staged_connections_.extract(connection_handle);

  if (staged_parameters_node.empty()) {
    bt_log(ERROR,
           "hci-le",
           "advertising set terminated event, staged params not available "
           "(handle: %d)",
           params->adv_handle);
    return;
  }

  hci_spec::AdvertisingHandle adv_handle = params->adv_handle;
  std::optional<DeviceAddress> opt_local_address =
      advertising_handle_map_.GetAddress(adv_handle);

  // We use the identity address as the local address if we aren't advertising
  // or otherwise don't know about this advertising set. This is obviously
  // wrong. However, the link will be disconnected in that case before it can
  // propagate to higher layers.
  static DeviceAddress identity_address =
      DeviceAddress(DeviceAddress::Type::kLEPublic, {0});
  DeviceAddress local_address = identity_address;
  if (opt_local_address) {
    local_address = opt_local_address.value();
  }

  StagedConnectionParameters staged = staged_parameters_node.mapped();

  CompleteIncomingConnection(connection_handle,
                             staged.role,
                             local_address,
                             staged.peer_address,
                             staged.conn_params,
                             staged_advertising_parameters_.extended_pdu);

  staged_advertising_parameters_.clear();
}

void ExtendedLowEnergyAdvertiser::OnCurrentOperationComplete() {
  if (op_queue_.empty()) {
    return;  // no more queued operations so nothing to do
  }

  fit::closure closure = std::move(op_queue_.front());
  op_queue_.pop();
  closure();
}

}  // namespace bt::hci
