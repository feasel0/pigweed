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

#pragma once
#include <lib/fit/function.h>

#include <memory>
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_class.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/le_connection_parameters.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test_double_base.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"

namespace bt::testing {

namespace android_hci = pw::bluetooth::vendor::android_hci;

class FakePeer;

// FakeController emulates a real Bluetooth controller. It can be configured to
// respond to HCI commands in a predictable manner.
class FakeController final : public ControllerTestDoubleBase,
                             public WeakSelf<FakeController> {
 public:
  // Global settings for the FakeController. These can be used to initialize a
  // FakeController and/or to re-configure an existing one.
  struct Settings final {
    // The default constructor initializes all fields to 0, unless another
    // default is specified below.
    Settings() = default;
    ~Settings() = default;

    void ApplyDualModeDefaults();
    void ApplyLEOnlyDefaults();
    void ApplyLegacyLEConfig();
    void ApplyExtendedLEConfig();
    void ApplyAndroidVendorExtensionDefaults();

    void AddBREDRSupportedCommands();
    void AddLESupportedCommands();

    bool is_event_unmasked(hci_spec::LEEventMask event) const;

    // The time elapsed from the receipt of a LE Create Connection command until
    // the resulting LE Connection Complete event.
    pw::chrono::SystemClock::duration le_connection_delay =
        std::chrono::seconds(0);

    // HCI settings.
    pw::bluetooth::emboss::CoreSpecificationVersion hci_version =
        pw::bluetooth::emboss::CoreSpecificationVersion::V5_0;
    uint8_t num_hci_command_packets = 250;
    uint64_t event_mask = 0;
    uint64_t le_event_mask = 0;

    // BD_ADDR (BR/EDR) or Public Device Address (LE)
    DeviceAddress bd_addr;

    // Local supported features and commands.
    uint64_t lmp_features_page0 = 0;
    uint64_t lmp_features_page1 = 0;
    uint64_t lmp_features_page2 = 0;
    uint64_t le_features = 0;
    uint64_t le_supported_states = 0;
    uint8_t supported_commands[64] = {0};

    // Buffer Size.
    uint16_t acl_data_packet_length = 0;
    uint8_t total_num_acl_data_packets = 0;
    uint16_t le_acl_data_packet_length = 0;
    uint8_t le_total_num_acl_data_packets = 0;
    uint16_t synchronous_data_packet_length = 0;
    uint8_t total_num_synchronous_data_packets = 0;
    uint16_t iso_data_packet_length = 0;
    uint8_t total_num_iso_data_packets = 0;

    // Vendor extensions
    StaticPacket<android_hci::LEGetVendorCapabilitiesCommandCompleteEventWriter>
        android_extension_settings;
  };

  // Configuration of an L2CAP channel for A2DP offloading.
  struct OffloadedA2dpChannel final {
    android_hci::A2dpCodecType codec_type = android_hci::A2dpCodecType::SBC;
    uint16_t max_latency = 0;
    StaticPacket<android_hci::A2dpScmsTEnableWriter> scms_t_enable;
    android_hci::A2dpSamplingFrequency sampling_frequency =
        android_hci::A2dpSamplingFrequency::HZ_44100;
    android_hci::A2dpBitsPerSample bits_per_sample =
        android_hci::A2dpBitsPerSample::BITS_PER_SAMPLE_16;
    android_hci::A2dpChannelMode channel_mode =
        android_hci::A2dpChannelMode::MONO;
    uint32_t encoded_audio_bitrate = 0;
    hci_spec::ConnectionHandle connection_handle = 0;
    l2cap::ChannelId l2cap_channel_id = 0;
    uint16_t l2cap_mtu_size = 0;
  };

  // Current device low energy scan state.
  struct LEScanState final {
    bool enabled = false;
    pw::bluetooth::emboss::LEScanType scan_type =
        pw::bluetooth::emboss::LEScanType::PASSIVE;
    pw::bluetooth::emboss::LEOwnAddressType own_address_type =
        pw::bluetooth::emboss::LEOwnAddressType::PUBLIC;
    pw::bluetooth::emboss::LEScanFilterPolicy filter_policy =
        pw::bluetooth::emboss::LEScanFilterPolicy::BASIC_UNFILTERED;
    uint16_t scan_interval = 0;
    uint16_t scan_window = 0;
    bool filter_duplicates = false;
    uint16_t duration = 0;
    uint16_t period = 0;
  };

  // Current device basic advertising state
  struct LEAdvertisingState final {
    BufferView advertised_view() const { return BufferView(data, data_length); }
    BufferView scan_rsp_view() const {
      return BufferView(scan_rsp_data, scan_rsp_length);
    }

    bool IsDirectedAdvertising() const;
    bool IsScannableAdvertising() const;
    bool IsConnectableAdvertising() const;

    bool enabled = false;
    pw::bluetooth::emboss::LEAdvertisingType adv_type = pw::bluetooth::emboss::
        LEAdvertisingType::CONNECTABLE_AND_SCANNABLE_UNDIRECTED;

    std::optional<DeviceAddress> random_address;
    pw::bluetooth::emboss::LEOwnAddressType own_address_type =
        pw::bluetooth::emboss::LEOwnAddressType::PUBLIC;

    uint32_t interval_min = 0;
    uint32_t interval_max = 0;

    uint8_t data_length = 0;
    uint8_t data[hci_spec::kMaxLEAdvertisingDataLength] = {0};
    uint8_t scan_rsp_length = 0;
    uint8_t scan_rsp_data[hci_spec::kMaxLEAdvertisingDataLength] = {0};
  };

  // The parameters of the most recent low energy connection initiation request
  struct LEConnectParams final {
    enum class InitiatingPHYs {
      kLE_1M = 0,
      kLE_2M,
      kLE_Coded,
    };

    struct Parameters {
      uint16_t scan_interval = 0;
      uint16_t scan_window = 0;
      uint16_t connection_interval_min = 0;
      uint16_t connection_interval_max = 0;
      uint16_t max_latency = 0;
      uint16_t supervision_timeout = 0;
      uint16_t min_ce_length = 0;
      uint16_t max_ce_length = 0;
    };

    bool use_filter_policy = false;
    pw::bluetooth::emboss::LEOwnAddressType own_address_type;
    DeviceAddress peer_address;
    std::unordered_map<InitiatingPHYs, Parameters> phy_conn_params;
  };

  // Constructor initializes the controller with the minimal default settings
  // (equivalent to calling Settings::ApplyDefaults()).
  explicit FakeController(pw::async::Dispatcher& pw_dispatcher)
      : ControllerTestDoubleBase(pw_dispatcher), WeakSelf(this) {}
  ~FakeController() override = default;

  // Resets the controller settings.
  void set_settings(const Settings& settings) { settings_ = settings; }

  // Always respond to the given command |opcode| with an Command Status event
  // specifying |status|.
  void SetDefaultCommandStatus(hci_spec::OpCode opcode,
                               pw::bluetooth::emboss::StatusCode status);
  void ClearDefaultCommandStatus(hci_spec::OpCode opcode);

  // Tells the FakeController to always respond to the given command opcode with
  // a Command Complete event specifying the given HCI status code.
  void SetDefaultResponseStatus(hci_spec::OpCode opcode,
                                pw::bluetooth::emboss::StatusCode status);
  void ClearDefaultResponseStatus(hci_spec::OpCode opcode);

  // Returns the current LE scan state.
  const LEScanState& le_scan_state() const { return le_scan_state_; }

  // Returns the current LE advertising state for legacy advertising
  const LEAdvertisingState& legacy_advertising_state() const {
    return legacy_advertising_state_;
  }

  // Returns the current LE advertising state for extended advertising, for the
  // given advertising handle
  const LEAdvertisingState& extended_advertising_state(
      hci_spec::AdvertisingHandle handle) {
    return extended_advertising_states_[handle];
  }

  // Returns the most recent LE connection request parameters.
  const std::optional<LEConnectParams>& le_connect_params() const {
    return le_connect_params_;
  }

  // Store the most recent LE Connection Parameters for inspection
  void CaptureLEConnectParams(
      const pw::bluetooth::emboss::LECreateConnectionCommandView& params);

  // Store the most recent LE Connection Parameters for inspection
  void CaptureLEConnectParams(
      const pw::bluetooth::emboss::LEExtendedCreateConnectionCommandV1View&
          params);

  // Returns the current local name set in the controller
  const std::string& local_name() const { return local_name_; }

  // Returns the current class of device.
  const DeviceClass& device_class() const { return device_class_; }

  // Adds a fake remote peer. Returns false if a peer with the same address
  // was previously added.
  bool AddPeer(std::unique_ptr<FakePeer> peer);

  // Removes a previously registered peer with the given device |address|.
  // Does nothing if |address| is unrecognized.
  void RemovePeer(const DeviceAddress& address);

  // Returns a pointer to the FakePeer with the given |address|. Returns
  // nullptr if the |address| is unknown.
  FakePeer* FindPeer(const DeviceAddress& address);

  // Counters for HCI commands received.
  int le_create_connection_command_count() const {
    return le_create_connection_command_count_;
  }
  int acl_create_connection_command_count() const {
    return acl_create_connection_command_count_;
  }

  // Setting this callback allows test code to introspect the
  // LECreateConnectionCommand from bt-host, but does not affect
  // FakeController's handling of the command (i.e. this method exists solely
  // for introspection). To change how FakeController responds to an
  // LECreateConnectionCommand, use the FakePeer::set_connect_status or
  // FakePeer::set_connect_response methods.
  void set_le_create_connection_command_callback(
      fit::function<void(pw::bluetooth::emboss::LECreateConnectionCommandView)>
          callback) {
    le_create_connection_cb_ = std::move(callback);
  }

  // Sets a callback to be invoked when the the base controller parameters
  // change due to a HCI command. These parameters are:
  //
  //   - The local name.
  //   - The local class of device.
  void set_controller_parameters_callback(fit::closure callback) {
    controller_parameters_cb_ = std::move(callback);
  }

  // Sets a callback to be invoked when the scan state changes.
  using ScanStateCallback = fit::function<void(bool enabled)>;
  void set_scan_state_callback(ScanStateCallback callback) {
    scan_state_cb_ = std::move(callback);
  }

  // Sets a callback to be invoked when the LE Advertising state changes.
  void set_advertising_state_callback(fit::closure callback) {
    advertising_state_cb_ = std::move(callback);
  }

  // Sets a callback to be invoked on connection events.
  using ConnectionStateCallback =
      fit::function<void(const DeviceAddress&,
                         hci_spec::ConnectionHandle handle,
                         bool connected,
                         bool canceled)>;
  void set_connection_state_callback(ConnectionStateCallback callback) {
    conn_state_cb_ = std::move(callback);
  }

  // Sets a callback to be invoked when LE connection parameters are updated
  // for a fake device.
  using LEConnectionParametersCallback = fit::function<void(
      const DeviceAddress&, const hci_spec::LEConnectionParameters&)>;
  void set_le_connection_parameters_callback(
      LEConnectionParametersCallback callback) {
    le_conn_params_cb_ = std::move(callback);
  }

  // Sets a callback to be invoked just before LE Read Remote Feature commands
  // are handled.
  void set_le_read_remote_features_callback(fit::closure callback) {
    le_read_remote_features_cb_ = std::move(callback);
  }

  // Sends a HCI event with the given parameters.
  void SendEvent(hci_spec::EventCode event_code, const ByteBuffer& payload);

  // Sends an HCI event, filling in the parameters in a provided event packet.
  void SendEvent(hci_spec::EventCode event_code,
                 hci::EmbossEventPacket* packet);

  // Sends a LE Meta event with the given parameters.
  void SendLEMetaEvent(hci_spec::EventCode subevent_code,
                       const ByteBuffer& payload);

  // Sends an ACL data packet with the given parameters.
  void SendACLPacket(hci_spec::ConnectionHandle handle,
                     const ByteBuffer& payload);

  // Sends a L2CAP basic frame.
  void SendL2CAPBFrame(hci_spec::ConnectionHandle handle,
                       l2cap::ChannelId channel_id,
                       const ByteBuffer& payload);

  // Sends a L2CAP control frame over a signaling channel. If |is_le| is true,
  // then the LE signaling channel will be used.
  void SendL2CAPCFrame(hci_spec::ConnectionHandle handle,
                       bool is_le,
                       l2cap::CommandCode code,
                       uint8_t id,
                       const ByteBuffer& payload);

  void SendNumberOfCompletedPacketsEvent(hci_spec::ConnectionHandle handle,
                                         uint16_t num);

  // Sets up a LE link to the device with the given |addr|. FakeController
  // will report a connection event in which it is in the given |role|.
  void ConnectLowEnergy(const DeviceAddress& addr,
                        pw::bluetooth::emboss::ConnectionRole role =
                            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  // Sends an HCI Connection Request event.
  void SendConnectionRequest(const DeviceAddress& addr,
                             pw::bluetooth::emboss::LinkType link_type);

  // Tells a fake device to initiate the L2CAP Connection Parameter Update
  // procedure using the given |params|. Has no effect if a connected fake
  // device with the given |addr| is not found.
  void L2CAPConnectionParameterUpdate(
      const DeviceAddress& addr,
      const hci_spec::LEPreferredConnectionParameters& params);

  // Sends an LE Meta Event Connection Update Complete Subevent. Used to
  // simulate updates initiated by LE central or spontaneously by the
  // controller.
  void SendLEConnectionUpdateCompleteSubevent(
      hci_spec::ConnectionHandle handle,
      const hci_spec::LEConnectionParameters& params,
      pw::bluetooth::emboss::StatusCode status =
          pw::bluetooth::emboss::StatusCode::SUCCESS);

  // Marks the FakePeer with address |address| as disconnected and sends a HCI
  // Disconnection Complete event for all of its links.
  void Disconnect(
      const DeviceAddress& addr,
      pw::bluetooth::emboss::StatusCode reason =
          pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);

  // Send HCI Disconnection Complete event for |handle|.
  void SendDisconnectionCompleteEvent(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::StatusCode reason =
          pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);

  // Send HCI encryption change event for |handle| with the given parameters.
  void SendEncryptionChangeEvent(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::StatusCode status,
      pw::bluetooth::emboss::EncryptionStatus encryption_enabled);

  // Callback to invoke when a packet is received over the data channel. Care
  // should be taken to ensure that a callback with a reference to test case
  // variables is not invoked when tearing down.
  using DataCallback = fit::function<void(const ByteBuffer& packet)>;
  void SetDataCallback(DataCallback callback,
                       pw::async::Dispatcher& pw_dispatcher);
  void ClearDataCallback();

  // Callback to invoke when a packet is received over the SCO data channel.
  void SetScoDataCallback(DataCallback callback) {
    sco_data_callback_ = std::move(callback);
  }
  void ClearScoDataCallback() { sco_data_callback_ = nullptr; }

  // Callback to invoke when a packet is received over the ISO data channel.
  void SetIsoDataCallback(DataCallback callback) {
    iso_data_callback_ = std::move(callback);
  }
  void ClearIsoDataCallback() { iso_data_callback_ = nullptr; }

  // Automatically send HCI Number of Completed Packets event for each packet
  // received. Enabled by default.
  void set_auto_completed_packets_event_enabled(bool enabled) {
    auto_completed_packets_event_enabled_ = enabled;
  }

  // Automatically send HCI Disconnection Complete event when HCI Disconnect
  // command received. Enabled by default.
  void set_auto_disconnection_complete_event_enabled(bool enabled) {
    auto_disconnection_complete_event_enabled_ = enabled;
  }

  // Sets the response flag for a TX Power Level Read.
  // Enabled by default (i.e it will respond to TXPowerLevelRead by default).
  void set_tx_power_level_read_response_flag(bool respond) {
    respond_to_tx_power_read_ = respond;
  }

  // Upon reception of a command packet with `opcode`, FakeController invokes
  // `pause_listener` with a closure. The command will hang until this closure
  // is invoked, enabling clients to control the timing of command completion.
  void pause_responses_for_opcode(
      hci_spec::OpCode code, fit::function<void(fit::closure)> pause_listener) {
    paused_opcode_listeners_[code] = std::move(pause_listener);
  }

  void clear_pause_listener_for_opcode(hci_spec::OpCode code) {
    paused_opcode_listeners_.erase(code);
  }

  // Called when a HCI_LE_Read_Advertising_Channel_Tx_Power command is
  // received.
  void OnLEReadAdvertisingChannelTxPower();

  // Inform the controller that the advertising handle is connected via the
  // connection handle. This method then generates the necessary LE Meta
  // Events (e.g. Advertising Set Terminated) to inform extended advertising
  // listeners.
  void SendLEAdvertisingSetTerminatedEvent(
      hci_spec::ConnectionHandle conn_handle,
      hci_spec::AdvertisingHandle adv_handle);

  // Inform the controller that the advertising handle is connected via the
  // connection handle. This method then generates the necessary Vendor Event
  // (e.g. LE multi-advertising state change sub-event) to inform Android
  // Multiple Advertising listeners.
  void SendAndroidLEMultipleAdvertisingStateChangeSubevent(
      hci_spec::ConnectionHandle conn_handle,
      hci_spec::AdvertisingHandle adv_handle);

  // The maximum number of advertising sets supported by the controller. Core
  // Spec Volume 4, Part E, Section 7.8.58: the memory used to store
  // advertising sets can also be used for other purposes. This value can
  // change over time.
  uint8_t num_supported_advertising_sets() const {
    return num_supported_advertising_sets_;
  }
  void set_num_supported_advertising_sets(uint8_t value) {
    BT_ASSERT(value >= extended_advertising_states_.size());
    BT_ASSERT(value <= hci_spec::kAdvertisingHandleMax +
                           1);  // support advertising handle of 0
    num_supported_advertising_sets_ = value;
  }

  // Controller overrides:
  void SendCommand(pw::span<const std::byte> command) override;

  void SendAclData(pw::span<const std::byte> data) override {
    // Post the packet to simulate async HCI behavior.
    (void)heap_dispatcher().Post(
        [self = GetWeakPtr(), data = DynamicByteBuffer(BufferView(data))](
            pw::async::Context /*ctx*/, pw::Status status) {
          if (self.is_alive() && status.ok()) {
            self->OnACLDataPacketReceived(BufferView(data));
          }
        });
  }

  void SendScoData(pw::span<const std::byte> data) override {
    // Post the packet to simulate async HCI behavior.
    (void)heap_dispatcher().Post(
        [self = GetWeakPtr(), data = DynamicByteBuffer(BufferView(data))](
            pw::async::Context /*ctx*/, pw::Status status) {
          if (self.is_alive() && status.ok()) {
            self->OnScoDataPacketReceived(BufferView(data));
          }
        });
  }

  void SendIsoData(pw::span<const std::byte> data) override {
    // Post the packet to simulate async HCI behavior.
    (void)heap_dispatcher().Post(
        [self = GetWeakPtr(), data = DynamicByteBuffer(BufferView(data))](
            pw::async::Context /*ctx*/, pw::Status status) {
          if (self.is_alive() && status.ok()) {
            self->OnIsoDataPacketReceived(BufferView(data));
          }
        });
  }

  // Sends a single LE advertising report for the given peer. This method will
  // send a legacy or extended advertising report, depending on which one the
  // peer is configured to send.
  //
  // Does nothing if a LE scan is not currently enabled or if the peer doesn't
  // support advertising.
  void SendAdvertisingReport(const FakePeer& peer);

  // Sends a single LE advertising report including the scan response for the
  // given peer. This method will send a legacy or extended advertising
  // report, depending on which one the peer is configured to send.
  //
  // Does nothing if a LE scan is not currently enabled or if the peer doesn't
  // support advertising.
  void SendScanResponseReport(const FakePeer& peer);

 private:
  static bool IsValidAdvertisingHandle(hci_spec::AdvertisingHandle handle) {
    return handle <= hci_spec::kAdvertisingHandleMax;
  }

  // Helper function to capture_le_connect_params
  void CaptureLEConnectParamsForPHY(
      const pw::bluetooth::emboss::LEExtendedCreateConnectionCommandV1View&
          params,
      LEConnectParams::InitiatingPHYs phy);

  // Finds and returns the FakePeer with the given parameters or nullptr if no
  // such device exists.
  FakePeer* FindByConnHandle(hci_spec::ConnectionHandle handle);

  // Returns the next available L2CAP signaling channel command ID.
  uint8_t NextL2CAPCommandId();

  // Sends a HCI_Command_Complete event with the given status in response to
  // the command with |opcode|.
  //
  // NOTE: This method returns only a status field. Some HCI commands have
  // multiple fields in their return message. In those cases, it's better (and
  // clearer) to use the other RespondWithCommandComplete (ByteBuffer as
  // second parameter) instead.
  void RespondWithCommandComplete(hci_spec::OpCode opcode,
                                  pw::bluetooth::emboss::StatusCode status);

  // Sends a HCI_Command_Complete event in response to the command with
  // |opcode| and using the given data as the parameter payload.
  void RespondWithCommandComplete(hci_spec::OpCode opcode,
                                  const ByteBuffer& params);

  // Sends an HCI_Command_Complete event in response to the command with
  // |opcode| and using the provided event packet, filling in the event header
  // fields.
  void RespondWithCommandComplete(hci_spec::OpCode opcode,
                                  hci::EmbossEventPacket* packet);

  // Sends a HCI_Command_Status event in response to the command with |opcode|
  // and using the given data as the parameter payload.
  void RespondWithCommandStatus(hci_spec::OpCode opcode,
                                pw::bluetooth::emboss::StatusCode status);

  // If a default Command Status event status has been set for the given
  // |opcode|, send a Command Status event and returns true.
  bool MaybeRespondWithDefaultCommandStatus(hci_spec::OpCode opcode);

  // If a default status has been configured for the given opcode, sends back
  // an error Command Complete event and returns true. Returns false if no
  // response was set.
  bool MaybeRespondWithDefaultStatus(hci_spec::OpCode opcode);

  // Sends Inquiry Response reports for known BR/EDR devices.
  void SendInquiryResponses();

  // Sends LE advertising reports for all known peers with advertising data,
  // if a scan is currently enabled. If duplicate filtering is disabled then
  // the reports are continued to be sent until scan is disabled.
  void SendAdvertisingReports();

  // Notifies |controller_parameters_cb_|.
  void NotifyControllerParametersChanged();

  // Notifies |advertising_state_cb_|
  void NotifyAdvertisingState();

  // Notifies |conn_state_cb_| with the given parameters.
  void NotifyConnectionState(const DeviceAddress& addr,
                             hci_spec::ConnectionHandle handle,
                             bool connected,
                             bool canceled = false);

  // Notifies |le_conn_params_cb_|
  void NotifyLEConnectionParameters(
      const DeviceAddress& addr,
      const hci_spec::LEConnectionParameters& params);

  template <typename T>
  void SendEnhancedConnectionCompleteEvent(
      pw::bluetooth::emboss::StatusCode status,
      const T& params,
      uint16_t interval,
      uint16_t max_latency,
      uint16_t supervision_timeout);

  void SendConnectionCompleteEvent(
      pw::bluetooth::emboss::StatusCode status,
      const pw::bluetooth::emboss::LECreateConnectionCommandView& params,
      uint16_t interval);

  // Called when a HCI_Create_Connection command is received.
  void OnCreateConnectionCommandReceived(
      const pw::bluetooth::emboss::CreateConnectionCommandView& params);

  // Called when a HCI_LE_Create_Connection command is received.
  void OnLECreateConnectionCommandReceived(
      const pw::bluetooth::emboss::LECreateConnectionCommandView& params);

  // Called when a HCI_LE_Create_Connection command is received.
  void OnLEExtendedCreateConnectionCommandReceived(
      const pw::bluetooth::emboss::LEExtendedCreateConnectionCommandV1View&
          params);

  // Called when a HCI_LE_Connection_Update command is received.
  void OnLEConnectionUpdateCommandReceived(
      const pw::bluetooth::emboss::LEConnectionUpdateCommandView& params);

  // Called when a HCI_Disconnect command is received.
  void OnDisconnectCommandReceived(
      const pw::bluetooth::emboss::DisconnectCommandView& params);

  // Called when a HCI_LE_Write_Host_Support command is received.
  void OnWriteLEHostSupportCommandReceived(
      const pw::bluetooth::emboss::WriteLEHostSupportCommandView& params);

  // Called when a HCI_Write_Secure_Connections_Host_Support command is
  // received.
  void OnWriteSecureConnectionsHostSupport(
      const pw::bluetooth::emboss::WriteSecureConnectionsHostSupportCommandView&
          params);

  // Called when a HCI_Reset command is received.
  void OnReset();

  // Called when a HCI_Inquiry command is received.
  void OnInquiry(const pw::bluetooth::emboss::InquiryCommandView& params);

  // Called when a HCI_LE_Set_Scan_Enable command is received.
  void OnLESetScanEnable(
      const pw::bluetooth::emboss::LESetScanEnableCommandView& params);

  // Called when a HCI_LE_Set_Extended_Scan_Enable command is received.
  void OnLESetExtendedScanEnable(
      const pw::bluetooth::emboss::LESetExtendedScanEnableCommandView& params);

  // Called when a HCI_LE_Set_Scan_Parameters command is received.

  void OnLESetScanParameters(
      const pw::bluetooth::emboss::LESetScanParametersCommandView& params);

  // Called when a HCI_LE_Extended_Set_Scan_Parameters command is received.
  void OnLESetExtendedScanParameters(
      const pw::bluetooth::emboss::LESetExtendedScanParametersCommandView&
          params);

  // Called when a HCI_Read_Local_Extended_Features command is received.
  void OnReadLocalExtendedFeatures(
      const pw::bluetooth::emboss::ReadLocalExtendedFeaturesCommandView&
          params);

  // Called when a HCI_SetEventMask command is received.
  void OnSetEventMask(
      const pw::bluetooth::emboss::SetEventMaskCommandView& params);

  // Called when a HCI_LE_Set_Event_Mask command is received.
  void OnLESetEventMask(
      const pw::bluetooth::emboss::LESetEventMaskCommandView& params);

  // Called when a HCI_LE_Read_Buffer_Size [v1] command is received.
  void OnLEReadBufferSizeV1();

  // Called when a HCI_LE_Read_Buffer_Size [v2] command is received.
  void OnLEReadBufferSizeV2();

  // Called when a HCI_LE_Read_Supported_States command is received.
  void OnLEReadSupportedStates();

  // Called when a HCI_LE_Read_Local_Supported_Features command is received.
  void OnLEReadLocalSupportedFeatures();

  // Called when a HCI_LE_Create_Connection_Cancel command is received.
  void OnLECreateConnectionCancel();

  // Called when a HCI_Write_Extended_Inquiry_Response command is received.
  void OnWriteExtendedInquiryResponse(
      const pw::bluetooth::emboss::WriteExtendedInquiryResponseCommandView&
          params);

  // Called when a HCI_Write_Simple_PairingMode command is received.
  void OnWriteSimplePairingMode(
      const pw::bluetooth::emboss::WriteSimplePairingModeCommandView& params);

  // Called when a HCI_Read_Simple_Pairing_Mode command is received.
  void OnReadSimplePairingMode();

  // Called when a HCI_Write_Page_Scan_Type command is received.
  void OnWritePageScanType(
      const pw::bluetooth::emboss::WritePageScanTypeCommandView& params);

  // Called when a HCI_Read_Page_Scan_Type command is received.
  void OnReadPageScanType();

  // Called when a HCI_Write_Inquiry_Mode command is received.
  void OnWriteInquiryMode(
      const pw::bluetooth::emboss::WriteInquiryModeCommandView& params);

  // Called when a HCI_Read_Inquiry_Mode command is received.
  void OnReadInquiryMode();

  // Called when a HCI_Write_Class_OfDevice command is received.
  void OnWriteClassOfDevice(
      const pw::bluetooth::emboss::WriteClassOfDeviceCommandView& params);

  // Called when a HCI_Write_Page_Scan_Activity command is received.
  void OnWritePageScanActivity(
      const pw::bluetooth::emboss::WritePageScanActivityCommandView& params);

  // Called when a HCI_Read_Page_Scan_Activity command is received.
  void OnReadPageScanActivity();

  // Called when a HCI_Write_Scan_Enable command is received.
  void OnWriteScanEnable(
      const pw::bluetooth::emboss::WriteScanEnableCommandView& params);

  // Called when a HCI_Read_Scan_Enable command is received.
  void OnReadScanEnable();

  // Called when a HCI_Read_Local_Name command is received.
  void OnReadLocalName();

  // Called when a HCI_Write_Local_Name command is received.
  void OnWriteLocalName(
      const pw::bluetooth::emboss::WriteLocalNameCommandView& params);

  // Called when a HCI_Create_Connection_Cancel command is received.
  void OnCreateConnectionCancel();

  // Called when a HCI_Read_Buffer_Size command is received.
  void OnReadBufferSize();

  // Called when a HCI_Read_BRADDR command is received.
  void OnReadBRADDR();

  // Called when a HCI_LE_Set_Advertising_Enable command is received.
  void OnLESetAdvertisingEnable(
      const pw::bluetooth::emboss::LESetAdvertisingEnableCommandView& params);

  // Called when a HCI_LE_Set_Scan_Response_Data command is received.
  void OnLESetScanResponseData(
      const pw::bluetooth::emboss::LESetScanResponseDataCommandView& params);

  // Called when a HCI_LE_Set_Advertising_Data command is received.
  void OnLESetAdvertisingData(
      const pw::bluetooth::emboss::LESetAdvertisingDataCommandView& params);

  // Called when a HCI_LE_Set_Advertising_Parameters command is received.
  void OnLESetAdvertisingParameters(
      const pw::bluetooth::emboss::LESetAdvertisingParametersCommandView&
          params);

  // Called when a HCI_LE_Set_Random_Address command is received.
  void OnLESetRandomAddress(
      const pw::bluetooth::emboss::LESetRandomAddressCommandView& params);

  // Called when a HCI_LE_Set_Advertising_Set_Random_Address command is
  // received.
  void OnLESetAdvertisingSetRandomAddress(
      const pw::bluetooth::emboss::LESetAdvertisingSetRandomAddressCommandView&
          params);

  // Called when a HCI_LE_Set_Extended_Advertising_Data command is received.
  void OnLESetExtendedAdvertisingParameters(
      const pw::bluetooth::emboss::
          LESetExtendedAdvertisingParametersV1CommandView& params);

  // Called when a HCI_LE_Set_Extended_Advertising_Data command is received.
  void OnLESetExtendedAdvertisingData(
      const pw::bluetooth::emboss::LESetExtendedAdvertisingDataCommandView&
          params);

  // Called when a HCI_LE_Set_Extended_Scan_Response_Data command is received.
  void OnLESetExtendedScanResponseData(
      const pw::bluetooth::emboss::LESetExtendedScanResponseDataCommandView&
          params);

  // Called when a HCI_LE_Set_Extended_Advertising_Enable command is received.
  void OnLESetExtendedAdvertisingEnable(
      const pw::bluetooth::emboss::LESetExtendedAdvertisingEnableCommandView&
          params);

  // Called when a HCI_LE_Read_Maximum_Advertising_Data_Length command is
  // received.
  void OnLEReadMaximumAdvertisingDataLength();

  // Called when a HCI_LE_Read_Number_of_Supported_Advertising_Sets command is
  // received.
  void OnLEReadNumberOfSupportedAdvertisingSets();

  // Called when a HCI_LE_Remove_Advertising_Set command is received.
  void OnLERemoveAdvertisingSet(
      const hci_spec::LERemoveAdvertisingSetCommandParams& params);

  // Called when a HCI_LE_Clear_Advertising_Sets command is received.
  void OnLEClearAdvertisingSets();

  // Called when a HCI_Read_Local_Supported_Features command is received.
  void OnReadLocalSupportedFeatures();

  // Called when a HCI_Read_Local_Supported_Commands command is received.
  void OnReadLocalSupportedCommands();

  // Called when a HCI_Read_Local_Version_Info command is received.
  void OnReadLocalVersionInfo();

  // Interrogation command handlers:

  // Called when a HCI_Read_Remote_Name_Request command is received.
  void OnReadRemoteNameRequestCommandReceived(
      const pw::bluetooth::emboss::RemoteNameRequestCommandView& params);

  // Called when a HCI_Read_Remote_Supported_Features command is received.
  void OnReadRemoteSupportedFeaturesCommandReceived(
      const pw::bluetooth::emboss::ReadRemoteSupportedFeaturesCommandView&
          params);

  // Called when a HCI_Read_Remote_Version_Information command is received.
  void OnReadRemoteVersionInfoCommandReceived(
      const pw::bluetooth::emboss::ReadRemoteVersionInfoCommandView& params);

  // Called when a HCI_Read_Remote_Extended_Features command is received.
  void OnReadRemoteExtendedFeaturesCommandReceived(
      const pw::bluetooth::emboss::ReadRemoteExtendedFeaturesCommandView&
          params);

  // Pairing command handlers:

  // Called when a HCI_Authentication_Requested command is received.
  void OnAuthenticationRequestedCommandReceived(
      const pw::bluetooth::emboss::AuthenticationRequestedCommandView& params);

  // Called when a HCI_Link_Key_Request_Reply command is received.
  void OnLinkKeyRequestReplyCommandReceived(
      const pw::bluetooth::emboss::LinkKeyRequestReplyCommandView& params);

  // Called when a HCI_Link_Key_Request_Negative_Reply command is received.
  void OnLinkKeyRequestNegativeReplyCommandReceived(
      const pw::bluetooth::emboss::LinkKeyRequestNegativeReplyCommandView&
          params);

  // Called when a HCI_IO_Capability_Request_Reply command is received.
  void OnIOCapabilityRequestReplyCommand(
      const pw::bluetooth::emboss::IoCapabilityRequestReplyCommandView& params);

  // Called when a HCI_User_Confirmation_Request_Reply command is received.
  void OnUserConfirmationRequestReplyCommand(
      const pw::bluetooth::emboss::UserConfirmationRequestReplyCommandView&
          params);

  // Called when a HCI_User_Confirmation_Request_Negative_Reply command is
  // received.
  void OnUserConfirmationRequestNegativeReplyCommand(
      const pw::bluetooth::emboss::
          UserConfirmationRequestNegativeReplyCommandView& params);

  // Called when a HCI_Set_Connection_Encryption command is received.
  void OnSetConnectionEncryptionCommand(
      const pw::bluetooth::emboss::SetConnectionEncryptionCommandView& params);

  // Called when a HCI_Read_Encryption_Key_Size command is received.
  void OnReadEncryptionKeySizeCommand(
      const pw::bluetooth::emboss::ReadEncryptionKeySizeCommandView& params);

  // Called when a HCI_Enhanced_Accept_Synchronous_Connection_Request command
  // is received.
  void OnEnhancedAcceptSynchronousConnectionRequestCommand(
      const pw::bluetooth::emboss::
          EnhancedAcceptSynchronousConnectionRequestCommandView& params);

  // Called when a HCI_Enhanced_Setup_Synchronous_Connection command is
  // received.
  void OnEnhancedSetupSynchronousConnectionCommand(
      const pw::bluetooth::emboss::
          EnhancedSetupSynchronousConnectionCommandView& params);

  // Called when a HCI_LE_Read_Remote_Features_Command is received.
  void OnLEReadRemoteFeaturesCommand(
      const hci_spec::LEReadRemoteFeaturesCommandParams& params);

  // Called when a HCI_LE_Enable_Encryption command is received, responds with
  // a successful encryption change event.
  void OnLEStartEncryptionCommand(
      const pw::bluetooth::emboss::LEEnableEncryptionCommandView& params);

  void OnWriteSynchronousFlowControlEnableCommand(
      const pw::bluetooth::emboss::WriteSynchronousFlowControlEnableCommandView&
          params);

  void OnAndroidLEGetVendorCapabilities();

  void OnAndroidA2dpOffloadCommand(
      const PacketView<hci_spec::CommandHeader>& command_packet);

  void OnAndroidStartA2dpOffload(
      const android_hci::StartA2dpOffloadCommandView& params);

  void OnAndroidStopA2dpOffload();

  void OnAndroidLEMultiAdvt(
      const PacketView<hci_spec::CommandHeader>& command_packet);

  void OnAndroidLEMultiAdvtSetAdvtParam(
      const android_hci::LEMultiAdvtSetAdvtParamCommandView& params);

  void OnAndroidLEMultiAdvtSetAdvtData(
      const android_hci::LEMultiAdvtSetAdvtDataCommandView& params);

  void OnAndroidLEMultiAdvtSetScanResp(
      const android_hci::LEMultiAdvtSetScanRespDataCommandView& params);

  void OnAndroidLEMultiAdvtSetRandomAddr(
      const android_hci::LEMultiAdvtSetRandomAddrCommandView& params);

  void OnAndroidLEMultiAdvtEnable(
      const android_hci::LEMultiAdvtEnableCommandView& params);

  // Called when a command with an OGF of hci_spec::kVendorOGF is received.
  void OnVendorCommand(
      const PacketView<hci_spec::CommandHeader>& command_packet);

  // Respond to a command packet. This may be done immediately upon reception
  // or via a client- triggered callback if pause_responses_for_opcode has
  // been called for that command's opcode.
  void HandleReceivedCommandPacket(
      const PacketView<hci_spec::CommandHeader>& command_packet);
  void HandleReceivedCommandPacket(
      const hci::EmbossCommandPacket& command_packet);

  void OnCommandPacketReceived(
      const PacketView<hci_spec::CommandHeader>& command_packet);
  void OnACLDataPacketReceived(const ByteBuffer& acl_data_packet);
  void OnScoDataPacketReceived(const ByteBuffer& sco_data_packet);
  void OnIsoDataPacketReceived(const ByteBuffer& iso_data_packet);

  const uint8_t BIT_1 = 1;
  bool isBREDRPageScanEnabled() const {
    return (bredr_scan_state_ >> BIT_1) & BIT_1;
  }

  enum class AdvertisingProcedure : uint8_t {
    kUnknown,
    kLegacy,
    kExtended,
  };

  const AdvertisingProcedure& advertising_procedure() const {
    return advertising_procedure_;
  }

  bool EnableLegacyAdvertising();
  bool EnableExtendedAdvertising();

  Settings settings_;

  // Value is non-null when A2DP offload is started, and null when it is
  // stopped.
  std::optional<OffloadedA2dpChannel> offloaded_a2dp_channel_state_;

  LEScanState le_scan_state_;
  LEAdvertisingState legacy_advertising_state_;
  std::unordered_map<hci_spec::AdvertisingHandle, LEAdvertisingState>
      extended_advertising_states_;

  // Used for BR/EDR Scans
  uint8_t bredr_scan_state_ = 0x00;
  pw::bluetooth::emboss::PageScanType page_scan_type_ =
      pw::bluetooth::emboss::PageScanType::STANDARD_SCAN;
  uint16_t page_scan_interval_ = 0x0800;
  uint16_t page_scan_window_ = 0x0012;

  // The GAP local name, as written/read by HCI_(Read/Write)_Local_Name. While
  // the aforementioned HCI commands carry the name in a 248 byte buffer,
  // |local_name_| contains the intended value.
  std::string local_name_;

  // The local device class configured by HCI_Write_Class_of_Device.
  DeviceClass device_class_;

  // Variables used for
  // HCI_LE_Create_Connection/HCI_LE_Create_Connection_Cancel.
  uint16_t next_conn_handle_ = 0u;
  SmartTask le_connect_rsp_task_{pw_dispatcher()};
  std::optional<LEConnectParams> le_connect_params_;
  bool le_connect_pending_ = false;

  // Variables used for
  // HCI_BREDR_Create_Connection/HCI_BREDR_Create_Connection_Cancel.
  bool bredr_connect_pending_ = false;
  DeviceAddress pending_bredr_connect_addr_;
  SmartTask bredr_connect_rsp_task_{pw_dispatcher()};

  // ID used for L2CAP LE signaling channel commands.
  uint8_t next_le_sig_id_ = 1u;

  // Used to indicate whether to respond back to TX Power Level read or not.
  bool respond_to_tx_power_read_ = true;

  // The Inquiry Mode that the controller is in.  Determines what types of
  // events are faked when a hci_spec::kInquiry is started.
  pw::bluetooth::emboss::InquiryMode inquiry_mode_;

  // The maximum number of advertising sets supported by the controller
  uint8_t num_supported_advertising_sets_ = 1;

  // The number of results left in Inquiry Mode operation.
  // If negative, no limit has been set.
  int16_t inquiry_num_responses_left_;

  // Used to setup default Command Status event responses.
  std::unordered_map<hci_spec::OpCode, pw::bluetooth::emboss::StatusCode>
      default_command_status_map_;

  // Used to setup default Command Complete event status responses (for
  // simulating errors)
  std::unordered_map<hci_spec::OpCode, pw::bluetooth::emboss::StatusCode>
      default_status_map_;

  // The set of fake peers that are visible.
  std::unordered_map<DeviceAddress, std::unique_ptr<FakePeer>> peers_;

  // Callbacks and counters that are intended for unit tests.
  int le_create_connection_command_count_ = 0;
  int acl_create_connection_command_count_ = 0;

  fit::function<void(pw::bluetooth::emboss::LECreateConnectionCommandView)>
      le_create_connection_cb_;
  fit::closure controller_parameters_cb_;
  ScanStateCallback scan_state_cb_;
  fit::closure advertising_state_cb_;
  ConnectionStateCallback conn_state_cb_;
  LEConnectionParametersCallback le_conn_params_cb_;
  fit::closure le_read_remote_features_cb_;

  // Associates opcodes with client-supplied pause listeners. Commands with
  // these opcodes will hang with no response until the client invokes the
  // passed-out closure.
  std::unordered_map<hci_spec::OpCode, fit::function<void(fit::closure)>>
      paused_opcode_listeners_;

  // Called when ACL data packets received.
  DataCallback acl_data_callback_ = nullptr;
  std::optional<pw::async::HeapDispatcher> data_dispatcher_;

  // Called when SCO data packets received.
  DataCallback sco_data_callback_ = nullptr;

  // Called when ISO data packets received.
  DataCallback iso_data_callback_ = nullptr;

  bool auto_completed_packets_event_enabled_ = true;
  bool auto_disconnection_complete_event_enabled_ = true;

  AdvertisingProcedure advertising_procedure_ = AdvertisingProcedure::kUnknown;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeController);
};

}  // namespace bt::testing
