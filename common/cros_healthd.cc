//
// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/common/cros_healthd.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <base/check.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/synchronization/waitable_event.h>
#include <brillo/dbus/dbus_method_invoker.h>
#include <brillo/dbus/file_descriptor.h>
#include <dbus/bus.h>
#include <dbus/cros_healthd/dbus-constants.h>
#include <dbus/message.h>
#include <mojo/public/cpp/platform/platform_channel.h>
#include <mojo/public/cpp/system/invitation.h>

#include "update_engine/cros/dbus_connection.h"

namespace chromeos_update_engine {

namespace {

using ::ash::cros_healthd::mojom::ProbeCategoryEnum;

#define SET_MOJO_VALUE(x) \
  { TelemetryCategoryEnum::x, ProbeCategoryEnum::x }
static const std::unordered_map<TelemetryCategoryEnum, ProbeCategoryEnum>
    kTelemetryMojoMapping{
        SET_MOJO_VALUE(kBattery),
        SET_MOJO_VALUE(kNonRemovableBlockDevices),
        SET_MOJO_VALUE(kCpu),
        SET_MOJO_VALUE(kTimezone),
        SET_MOJO_VALUE(kMemory),
        SET_MOJO_VALUE(kBacklight),
        SET_MOJO_VALUE(kFan),
        SET_MOJO_VALUE(kStatefulPartition),
        SET_MOJO_VALUE(kBluetooth),
        SET_MOJO_VALUE(kSystem),
        SET_MOJO_VALUE(kNetwork),
        SET_MOJO_VALUE(kAudio),
        SET_MOJO_VALUE(kBootPerformance),
        SET_MOJO_VALUE(kBus),
    };

void PrintError(const ash::cros_healthd::mojom::ProbeErrorPtr& error,
                std::string info) {
  LOG(ERROR) << "Failed to get " << info << ","
             << " error_type=" << error->type << " error_msg=" << error->msg;
}

}  // namespace

std::unique_ptr<CrosHealthdInterface> CreateCrosHealthd() {
  auto cros_healthd = std::make_unique<CrosHealthd>();
  // Call init, instead of in constructor as testing/mocks don't require the
  // `Init()` call.
  cros_healthd->Init();
  return cros_healthd;
}

void CrosHealthd::Init() {
  mojo::core::Init();
  ipc_support_ = std::make_unique<mojo::core::ScopedIPCSupport>(
      base::ThreadTaskRunnerHandle::Get() /* io_thread_task_runner */,
      mojo::core::ScopedIPCSupport::ShutdownPolicy::
          CLEAN /* blocking shutdown */);
}

TelemetryInfo* const CrosHealthd::GetTelemetryInfo() {
  return telemetry_info_.get();
}

void CrosHealthd::ProbeTelemetryInfo(
    const std::unordered_set<TelemetryCategoryEnum>& categories,
    ProbeTelemetryInfoCallback once_callback) {
  std::vector<ProbeCategoryEnum> categories_mojo;
  for (const auto& category : categories) {
    auto it = kTelemetryMojoMapping.find(category);
    if (it != kTelemetryMojoMapping.end())
      categories_mojo.push_back(it->second);
  }
  cros_healthd_service_factory_->GetProbeService(
      cros_healthd_probe_service_.BindNewPipeAndPassReceiver());
  cros_healthd_probe_service_->ProbeTelemetryInfo(
      categories_mojo,
      base::BindOnce(&CrosHealthd::OnProbeTelemetryInfo,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(once_callback)));
}

dbus::ObjectProxy* CrosHealthd::GetCrosHealthdObjectProxy() {
  return DBusConnection::Get()->GetDBus()->GetObjectProxy(
      diagnostics::kCrosHealthdServiceName,
      dbus::ObjectPath(diagnostics::kCrosHealthdServicePath));
}

void CrosHealthd::BootstrapMojo(BootstrapMojoCallback callback) {
  if (cros_healthd_service_factory_.is_bound()) {
    LOG(WARNING) << "cros_healthd is already bound, ignoring initialization.";
    std::move(callback).Run(true);
    return;
  }

  // `cros_healthd` service must be available for bootstrapping.
  GetCrosHealthdObjectProxy()->WaitForServiceToBeAvailable(
      base::BindOnce(&CrosHealthd::FinalizeBootstrap,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void CrosHealthd::FinalizeBootstrap(BootstrapMojoCallback callback,
                                    bool service_available) {
  if (!service_available) {
    LOG(ERROR) << "cros_healthd service not available.";
    std::move(callback).Run(false);
    return;
  }

  mojo::PlatformChannel channel;
  brillo::ErrorPtr error;
  auto response = brillo::dbus_utils::CallMethodAndBlock(
      GetCrosHealthdObjectProxy(),
      diagnostics::kCrosHealthdServiceInterface,
      diagnostics::kCrosHealthdBootstrapMojoConnectionMethod,
      &error,
      brillo::dbus_utils::FileDescriptor(
          channel.TakeRemoteEndpoint().TakePlatformHandle().TakeFD()),
      /*is_chrome=*/false);
  if (!response) {
    LOG(ERROR) << "Failed to bootstrap mojo connection with cros_healthd.";
    std::move(callback).Run(false);
    return;
  }

  std::string token;
  dbus::MessageReader reader(response.get());
  if (!reader.PopString(&token)) {
    LOG(ERROR) << "Failed to get token from cros_healthd DBus response.";
    std::move(callback).Run(false);
    return;
  }

  mojo::IncomingInvitation invitation =
      mojo::IncomingInvitation::Accept(channel.TakeLocalEndpoint());
  auto opt_pending_service_factory =
      mojo::PendingRemote<ash::cros_healthd::mojom::CrosHealthdServiceFactory>(
          invitation.ExtractMessagePipe(token), 0u /* version */);
  if (!opt_pending_service_factory) {
    LOG(ERROR) << "Failed to create pending service factory for cros_healthd.";
    std::move(callback).Run(false);
    return;
  }
  cros_healthd_service_factory_.Bind(std::move(opt_pending_service_factory));
  std::move(callback).Run(true);
}

void CrosHealthd::OnProbeTelemetryInfo(
    ProbeTelemetryInfoCallback once_callback,
    ash::cros_healthd::mojom::TelemetryInfoPtr result) {
  if (!result) {
    LOG(WARNING) << "Failed to parse telemetry information.";
    std::move(once_callback).Run({});
    return;
  }
  if (!ParseSystemResult(&result, telemetry_info_.get()))
    LOG(WARNING) << "Failed to parse system information.";
  if (!ParseMemoryResult(&result, telemetry_info_.get()))
    LOG(WARNING) << "Failed to parse memory information.";
  if (!ParseNonRemovableBlockDeviceResult(&result, telemetry_info_.get()))
    LOG(WARNING) << "Failed to parse non-removable block device information.";
  if (!ParseCpuResult(&result, telemetry_info_.get()))
    LOG(WARNING) << "Failed to parse physical CPU information.";
  if (!ParseBusResult(&result, telemetry_info_.get()))
    LOG(WARNING) << "Failed to parse bus information.";
  std::move(once_callback).Run(*telemetry_info_);
}

bool CrosHealthd::ParseSystemResult(
    ash::cros_healthd::mojom::TelemetryInfoPtr* result,
    TelemetryInfo* telemetry_info) {
  const auto& system_result = (*result)->system_result;
  if (system_result) {
    if (system_result->is_error()) {
      PrintError(system_result->get_error(), "system information");
      return false;
    }
    const auto& system_info = system_result->get_system_info();

    const auto& dmi_info = system_info->dmi_info;
    if (dmi_info) {
      if (dmi_info->sys_vendor.has_value())
        telemetry_info->system_info.dmi_info.sys_vendor =
            dmi_info->sys_vendor.value();
      if (dmi_info->product_name.has_value())
        telemetry_info->system_info.dmi_info.product_name =
            dmi_info->product_name.value();
      if (dmi_info->product_version.has_value())
        telemetry_info->system_info.dmi_info.product_version =
            dmi_info->product_version.value();
      if (dmi_info->bios_version.has_value())
        telemetry_info->system_info.dmi_info.bios_version =
            dmi_info->bios_version.value();
    }

    const auto& os_info = system_info->os_info;
    if (os_info) {
      telemetry_info->system_info.os_info.boot_mode =
          static_cast<TelemetryInfo::SystemInfo::OsInfo::BootMode>(
              os_info->boot_mode);
    }
  }
  return true;
}

bool CrosHealthd::ParseMemoryResult(
    ash::cros_healthd::mojom::TelemetryInfoPtr* result,
    TelemetryInfo* telemetry_info) {
  const auto& memory_result = (*result)->memory_result;
  if (memory_result) {
    if (memory_result->is_error()) {
      PrintError(memory_result->get_error(), "memory information");
      return false;
    }
    const auto& memory_info = memory_result->get_memory_info();
    if (memory_info) {
      telemetry_info->memory_info.total_memory_kib =
          memory_info->total_memory_kib;
    }
  }
  return true;
}

bool CrosHealthd::ParseNonRemovableBlockDeviceResult(
    ash::cros_healthd::mojom::TelemetryInfoPtr* result,
    TelemetryInfo* telemetry_info) {
  const auto& non_removable_block_device_result =
      (*result)->block_device_result;
  if (non_removable_block_device_result) {
    if (non_removable_block_device_result->is_error()) {
      PrintError(non_removable_block_device_result->get_error(),
                 "non-removable block device information");
      return false;
    }
    const auto& non_removable_block_device_infos =
        non_removable_block_device_result->get_block_device_info();
    for (const auto& non_removable_block_device_info :
         non_removable_block_device_infos) {
      telemetry_info->block_device_info.push_back({
          .size = non_removable_block_device_info->size,
      });
    }
  }
  return true;
}

bool CrosHealthd::ParseCpuResult(
    ash::cros_healthd::mojom::TelemetryInfoPtr* result,
    TelemetryInfo* telemetry_info) {
  const auto& cpu_result = (*result)->cpu_result;
  if (cpu_result) {
    if (cpu_result->is_error()) {
      PrintError(cpu_result->get_error(), "CPU information");
      return false;
    }
    const auto& cpu_info = cpu_result->get_cpu_info();
    for (const auto& physical_cpu : cpu_info->physical_cpus) {
      if (physical_cpu->model_name.has_value()) {
        telemetry_info->cpu_info.physical_cpus.push_back({
            .model_name = physical_cpu->model_name.value(),
        });
      }
    }
  }
  return true;
}

bool CrosHealthd::ParseBusResult(
    ash::cros_healthd::mojom::TelemetryInfoPtr* result,
    TelemetryInfo* telemetry_info) {
  const auto& bus_result = (*result)->bus_result;
  if (bus_result) {
    if (bus_result->is_error()) {
      PrintError(bus_result->get_error(), "bus information");
      return false;
    }
    const auto& bus_devices = bus_result->get_bus_devices();
    for (const auto& bus_device : bus_devices) {
      if (!bus_device->bus_info)
        continue;
      switch (bus_device->bus_info->which()) {
        case ash::cros_healthd::mojom::BusInfo::Tag::kPciBusInfo: {
          const auto& pci_bus_info = bus_device->bus_info->get_pci_bus_info();
          telemetry_info->bus_devices.push_back({
              .device_class =
                  static_cast<TelemetryInfo::BusDevice::BusDeviceClass>(
                      bus_device->device_class),
              .bus_type_info =
                  TelemetryInfo::BusDevice::PciBusInfo{
                      .vendor_id = pci_bus_info->vendor_id,
                      .device_id = pci_bus_info->device_id,
                      .driver = pci_bus_info->driver.has_value()
                                    ? pci_bus_info->driver.value()
                                    : "",
                  },
          });
          break;
        }
        case ash::cros_healthd::mojom::BusInfo::Tag::kUsbBusInfo: {
          const auto& usb_bus_info = bus_device->bus_info->get_usb_bus_info();
          telemetry_info->bus_devices.push_back({
              .device_class =
                  static_cast<TelemetryInfo::BusDevice::BusDeviceClass>(
                      bus_device->device_class),
              .bus_type_info =
                  TelemetryInfo::BusDevice::UsbBusInfo{
                      .vendor_id = usb_bus_info->vendor_id,
                      .product_id = usb_bus_info->product_id,
                  },
          });
          break;
        }
        case ash::cros_healthd::mojom::BusInfo::Tag::kThunderboltBusInfo: {
          break;
        }
        case ash::cros_healthd::mojom::BusInfo::Tag::kUnmappedField: {
          LOG(ERROR) << "Get unmapped Mojo fields by retrieving bus info from "
                        "cros_healthd";
          break;
        }
      }
    }
  }
  return true;
}

}  // namespace chromeos_update_engine
