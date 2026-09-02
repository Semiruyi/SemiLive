#include "publisher/infrastructure/capture/dxgi_desktop_capture_backend.hpp"

#include "publisher/infrastructure/capture/desktop_pointer_compositor.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace semilive::publisher::infra::capture {
namespace {

using Microsoft::WRL::ComPtr;
using contracts::capture::DesktopCaptureInfo;
using contracts::capture::DesktopCaptureIssue;
using contracts::capture::DesktopCaptureObservation;
using contracts::capture::DesktopCaptureOperation;
using contracts::capture::DesktopCaptureResult;
using contracts::capture::DesktopImage;
using contracts::capture::DesktopNoChange;
using contracts::capture::DesktopOutputSelection;
using contracts::capture::DesktopOutputSelector;
using contracts::capture::DesktopTemporarilyUnavailable;

struct OutputCandidate {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC description{};
};

std::int64_t native_code(const HRESULT result) noexcept {
    return static_cast<std::int64_t>(result);
}

DesktopCaptureIssue make_issue(const DesktopCaptureOperation operation,
                               const HRESULT result,
                               const std::string_view context) {
    std::ostringstream message;
    message << context << " (HRESULT 0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(result) << ')';
    return DesktopCaptureIssue{operation, native_code(result), message.str()};
}

DesktopCaptureIssue make_issue(const DesktopCaptureOperation operation,
                               std::string message) {
    return DesktopCaptureIssue{operation, 0, std::move(message)};
}

bool contains_desktop_origin(const RECT& rectangle) noexcept {
    return rectangle.left <= 0 && rectangle.right > 0 &&
           rectangle.top <= 0 && rectangle.bottom > 0;
}

std::string narrow_output_name(const wchar_t* name) {
    const auto required = WideCharToMultiByte(
        CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return "Unknown desktop output";
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const auto converted = WideCharToMultiByte(
        CP_UTF8, 0, name, -1, result.data(), required, nullptr, nullptr);
    if (converted != required) {
        return "Unknown desktop output";
    }
    result.pop_back();
    return result;
}

std::expected<ComPtr<IDXGIFactory1>, DesktopCaptureIssue> create_factory(
    const DesktopCaptureOperation operation) {
    ComPtr<IDXGIFactory1> factory;
    const auto result = CreateDXGIFactory1(
        __uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(result)) {
        return std::unexpected{
            make_issue(operation, result, "failed to create DXGI factory")};
    }
    return factory;
}

std::expected<OutputCandidate, DesktopCaptureIssue> select_output(
    const DesktopOutputSelector selector,
    const DesktopCaptureOperation operation) {
    const auto factory_result = create_factory(operation);
    if (!factory_result) {
        return std::unexpected{factory_result.error()};
    }

    std::uint32_t attached_index = 0;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto adapter_result =
            (*factory_result)->EnumAdapters1(adapter_index, adapter.GetAddressOf());
        if (adapter_result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapter_result)) {
            return std::unexpected{
                make_issue(operation, adapter_result, "failed to enumerate DXGI adapters")};
        }

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const auto output_result =
                adapter->EnumOutputs(output_index, output.GetAddressOf());
            if (output_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(output_result)) {
                return std::unexpected{
                    make_issue(operation, output_result, "failed to enumerate DXGI outputs")};
            }

            DXGI_OUTPUT_DESC description{};
            const auto description_result = output->GetDesc(&description);
            if (FAILED(description_result)) {
                return std::unexpected{
                    make_issue(operation, description_result, "failed to describe DXGI output")};
            }
            if (description.AttachedToDesktop == FALSE) {
                continue;
            }

            const bool selected =
                (selector.selection == DesktopOutputSelection::Primary &&
                 contains_desktop_origin(description.DesktopCoordinates)) ||
                (selector.selection == DesktopOutputSelection::Index &&
                 selector.index == attached_index);
            if (selected) {
                return OutputCandidate{std::move(adapter), std::move(output), description};
            }
            ++attached_index;
        }
    }

    return std::unexpected{
        make_issue(operation, DXGI_ERROR_NOT_FOUND, "requested desktop output was not found")};
}

std::expected<OutputCandidate, DesktopCaptureIssue> find_output_by_name(
    const std::wstring& device_name,
    const DesktopCaptureOperation operation) {
    const auto factory_result = create_factory(operation);
    if (!factory_result) {
        return std::unexpected{factory_result.error()};
    }

    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto adapter_result =
            (*factory_result)->EnumAdapters1(adapter_index, adapter.GetAddressOf());
        if (adapter_result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapter_result)) {
            return std::unexpected{
                make_issue(operation, adapter_result, "failed to enumerate DXGI adapters")};
        }

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const auto output_result =
                adapter->EnumOutputs(output_index, output.GetAddressOf());
            if (output_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(output_result)) {
                return std::unexpected{
                    make_issue(operation, output_result, "failed to enumerate DXGI outputs")};
            }

            DXGI_OUTPUT_DESC description{};
            const auto description_result = output->GetDesc(&description);
            if (FAILED(description_result)) {
                return std::unexpected{
                    make_issue(operation, description_result, "failed to describe DXGI output")};
            }
            if (description.AttachedToDesktop != FALSE &&
                device_name == description.DeviceName) {
                return OutputCandidate{std::move(adapter), std::move(output), description};
            }
        }
    }

    return std::unexpected{
        make_issue(operation, DXGI_ERROR_NOT_FOUND, "original desktop output was not found")};
}

bool is_recoverable(const HRESULT result) noexcept {
    return result == DXGI_ERROR_ACCESS_LOST ||
           result == DXGI_ERROR_DEVICE_REMOVED ||
           result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DRIVER_INTERNAL_ERROR ||
           result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ||
           result == DXGI_ERROR_NOT_FOUND;
}

bool is_recoverable(const DesktopCaptureIssue& issue) noexcept {
    return is_recoverable(static_cast<HRESULT>(issue.native_code));
}

class AcquiredFrame final {
public:
    explicit AcquiredFrame(IDXGIOutputDuplication* duplication) noexcept
        : duplication_{duplication} {}

    ~AcquiredFrame() {
        if (duplication_ != nullptr) {
            (void)duplication_->ReleaseFrame();
        }
    }

    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;

    [[nodiscard]] HRESULT release() noexcept {
        if (duplication_ == nullptr) {
            return S_OK;
        }
        auto* duplication = std::exchange(duplication_, nullptr);
        return duplication->ReleaseFrame();
    }

private:
    IDXGIOutputDuplication* duplication_ = nullptr;
};

class MappedTexture final {
public:
    MappedTexture(ID3D11DeviceContext* context,
                  ID3D11Texture2D* texture,
                  const D3D11_MAPPED_SUBRESOURCE mapped) noexcept
        : context_{context}, texture_{texture}, mapped_{mapped} {}

    ~MappedTexture() {
        if (context_ != nullptr) {
            context_->Unmap(texture_, 0);
        }
    }

    MappedTexture(const MappedTexture&) = delete;
    MappedTexture& operator=(const MappedTexture&) = delete;

    [[nodiscard]] const D3D11_MAPPED_SUBRESOURCE& get() const noexcept {
        return mapped_;
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped_{};
};

std::expected<DesktopImage, DesktopCaptureIssue> copy_desktop_image(
    const D3D11_TEXTURE2D_DESC& source_description,
    const DXGI_MODE_ROTATION rotation,
    const D3D11_MAPPED_SUBRESOURCE& mapped) {
    const bool swaps_dimensions =
        rotation == DXGI_MODE_ROTATION_ROTATE90 ||
        rotation == DXGI_MODE_ROTATION_ROTATE270;
    const auto width = swaps_dimensions ? source_description.Height : source_description.Width;
    const auto height = swaps_dimensions ? source_description.Width : source_description.Height;

    if (width == 0 || height == 0 ||
        width > std::numeric_limits<std::uint32_t>::max() / 4U) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Copy,
                       "DXGI desktop image dimensions are not representable")};
    }

    const auto stride = width * 4U;
    const auto byte_count = static_cast<std::uint64_t>(stride) * height;
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Copy,
                       "DXGI desktop image buffer size is not representable")};
    }

    DesktopImage image;
    image.bgra.resize(static_cast<std::size_t>(byte_count));
    image.width = width;
    image.height = height;
    image.stride = stride;

    const auto* source = static_cast<const std::byte*>(mapped.pData);
    if (rotation == DXGI_MODE_ROTATION_UNSPECIFIED ||
        rotation == DXGI_MODE_ROTATION_IDENTITY) {
        const auto source_row_bytes = static_cast<std::size_t>(source_description.Width) * 4U;
        for (std::uint32_t y = 0; y < source_description.Height; ++y) {
            std::memcpy(image.bgra.data() + static_cast<std::size_t>(y) * stride,
                        source + static_cast<std::size_t>(y) * mapped.RowPitch,
                        source_row_bytes);
        }
        return image;
    }

    for (std::uint32_t source_y = 0; source_y < source_description.Height; ++source_y) {
        for (std::uint32_t source_x = 0; source_x < source_description.Width; ++source_x) {
            std::uint32_t destination_x = 0;
            std::uint32_t destination_y = 0;
            switch (rotation) {
                case DXGI_MODE_ROTATION_ROTATE90:
                    destination_x = source_description.Height - 1U - source_y;
                    destination_y = source_x;
                    break;
                case DXGI_MODE_ROTATION_ROTATE180:
                    destination_x = source_description.Width - 1U - source_x;
                    destination_y = source_description.Height - 1U - source_y;
                    break;
                case DXGI_MODE_ROTATION_ROTATE270:
                    destination_x = source_y;
                    destination_y = source_description.Width - 1U - source_x;
                    break;
                default:
                    return std::unexpected{
                        make_issue(DesktopCaptureOperation::Copy,
                                   "DXGI output uses an unsupported rotation")};
            }

            const auto source_offset =
                static_cast<std::size_t>(source_y) * mapped.RowPitch +
                static_cast<std::size_t>(source_x) * 4U;
            const auto destination_offset =
                static_cast<std::size_t>(destination_y) * stride +
                static_cast<std::size_t>(destination_x) * 4U;
            std::memcpy(image.bgra.data() + destination_offset,
                        source + source_offset,
                        4U);
        }
    }
    return image;
}

std::expected<DesktopPointerShapeType, DesktopCaptureIssue> pointer_shape_type(
    const UINT native_type) {
    switch (native_type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
            return DesktopPointerShapeType::Color;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
            return DesktopPointerShapeType::Monochrome;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
            return DesktopPointerShapeType::MaskedColor;
        default:
            return std::unexpected{
                make_issue(DesktopCaptureOperation::Pointer,
                           "DXGI returned an unsupported desktop pointer shape type")};
    }
}

}  // namespace

struct DxgiDesktopCaptureBackend::Impl {
    [[nodiscard]] std::expected<void, DesktopCaptureIssue> initialize(
        OutputCandidate candidate,
        const DesktopCaptureOperation operation) {
        ComPtr<ID3D11Device> new_device;
        ComPtr<ID3D11DeviceContext> new_context;
        D3D_FEATURE_LEVEL feature_level{};
        const auto device_result = D3D11CreateDevice(
            candidate.adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            new_device.GetAddressOf(),
            &feature_level,
            new_context.GetAddressOf());
        if (FAILED(device_result)) {
            return std::unexpected{
                make_issue(operation, device_result, "failed to create D3D11 device")};
        }

        ComPtr<IDXGIOutput1> output1;
        const auto output_result = candidate.output.As(&output1);
        if (FAILED(output_result)) {
            return std::unexpected{
                make_issue(operation, output_result, "failed to query IDXGIOutput1")};
        }

        ComPtr<IDXGIOutputDuplication> new_duplication;
        const auto duplication_result =
            output1->DuplicateOutput(new_device.Get(), new_duplication.GetAddressOf());
        if (FAILED(duplication_result)) {
            return std::unexpected{
                make_issue(operation, duplication_result, "failed to duplicate DXGI output")};
        }

        DXGI_OUTDUPL_DESC duplication_description{};
        new_duplication->GetDesc(&duplication_description);
        if (duplication_description.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            return std::unexpected{
                make_issue(operation, "DXGI output does not use BGRA8 format")};
        }

        adapter = std::move(candidate.adapter);
        output = std::move(candidate.output);
        output_description = candidate.description;
        device = std::move(new_device);
        context = std::move(new_context);
        duplication = std::move(new_duplication);
        duplication_description_ = duplication_description;
        staging.Reset();
        reset_pointer_state();
        needs_reinitialize = false;
        return {};
    }

    void reset_pointer_state() noexcept {
        pointer_shape.reset();
        pointer_position = {};
        pointer_visible = false;
    }

    void release_capture_resources() noexcept {
        staging.Reset();
        duplication.Reset();
        context.Reset();
        device.Reset();
        output.Reset();
        adapter.Reset();
        duplication_description_ = {};
        reset_pointer_state();
    }

    void begin_recovery() noexcept {
        release_capture_resources();
        needs_reinitialize = true;
    }

    [[nodiscard]] std::expected<void, DesktopCaptureIssue> reinitialize() {
        auto candidate = find_output_by_name(
            output_device_name, DesktopCaptureOperation::Reinitialize);
        if (!candidate) {
            return std::unexpected{candidate.error()};
        }
        return initialize(std::move(*candidate), DesktopCaptureOperation::Reinitialize);
    }

    [[nodiscard]] std::expected<void, DesktopCaptureIssue> ensure_staging(
        const D3D11_TEXTURE2D_DESC& source_description) {
        if (staging) {
            D3D11_TEXTURE2D_DESC staging_description{};
            staging->GetDesc(&staging_description);
            if (staging_description.Width == source_description.Width &&
                staging_description.Height == source_description.Height &&
                staging_description.Format == source_description.Format) {
                return {};
            }
            staging.Reset();
        }

        auto description = source_description;
        description.BindFlags = 0;
        description.MiscFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const auto result = device->CreateTexture2D(&description, nullptr, staging.GetAddressOf());
        if (FAILED(result)) {
            return std::unexpected{
                make_issue(DesktopCaptureOperation::Copy,
                           result,
                           "failed to create DXGI staging texture")};
        }
        return {};
    }

    [[nodiscard]] std::expected<void, DesktopCaptureIssue> update_pointer(
        const DXGI_OUTDUPL_FRAME_INFO& frame_info) {
        if (!compose_pointer) {
            return {};
        }

        if (frame_info.LastMouseUpdateTime.QuadPart != 0) {
            pointer_visible = frame_info.PointerPosition.Visible != FALSE;
            // DXGI defines Position as the shape's top-left corner in the
            // output-relative, display-oriented coordinate space. HotSpot is
            // informational and must not be subtracted here.
            pointer_position = DesktopPointerPosition{
                frame_info.PointerPosition.Position.x,
                frame_info.PointerPosition.Position.y,
            };
        }

        if (frame_info.PointerShapeBufferSize == 0) {
            return {};
        }

        std::vector<std::byte> shape_buffer(frame_info.PointerShapeBufferSize);
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info{};
        UINT required_size = 0;
        auto result = duplication->GetFramePointerShape(
            static_cast<UINT>(shape_buffer.size()),
            shape_buffer.data(),
            &required_size,
            &shape_info);
        if (result == DXGI_ERROR_MORE_DATA && required_size > shape_buffer.size()) {
            shape_buffer.resize(required_size);
            result = duplication->GetFramePointerShape(
                static_cast<UINT>(shape_buffer.size()),
                shape_buffer.data(),
                &required_size,
                &shape_info);
        }
        if (FAILED(result)) {
            return std::unexpected{
                make_issue(DesktopCaptureOperation::Pointer,
                           result,
                           "failed to retrieve the DXGI desktop pointer shape")};
        }
        if (required_size > shape_buffer.size()) {
            return std::unexpected{
                make_issue(DesktopCaptureOperation::Pointer,
                           "DXGI desktop pointer shape exceeded its reported buffer size")};
        }
        shape_buffer.resize(required_size);

        const auto type_result = pointer_shape_type(shape_info.Type);
        if (!type_result) {
            return std::unexpected{type_result.error()};
        }
        pointer_shape = DesktopPointerShape{
            *type_result,
            shape_info.Width,
            shape_info.Height,
            shape_info.Pitch,
            std::move(shape_buffer),
        };
        return {};
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    DXGI_OUTPUT_DESC output_description{};
    DXGI_OUTDUPL_DESC duplication_description_{};
    std::wstring output_device_name;
    std::optional<DesktopPointerShape> pointer_shape;
    DesktopPointerPosition pointer_position{};
    std::thread::id owner_thread;
    bool compose_pointer = false;
    bool pointer_visible = false;
    bool open = false;
    bool needs_reinitialize = false;
};

DxgiDesktopCaptureBackend::DxgiDesktopCaptureBackend()
    : impl_{std::make_unique<Impl>()} {}

DxgiDesktopCaptureBackend::~DxgiDesktopCaptureBackend() {
    close();
}

std::expected<DesktopCaptureInfo, DesktopCaptureIssue>
DxgiDesktopCaptureBackend::open(
    const contracts::capture::DesktopCaptureConfig& config) {
    if (impl_->open) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Open,
                       "DXGI desktop capture backend is already open")};
    }
    if (config.output.selection != DesktopOutputSelection::Primary &&
        config.output.selection != DesktopOutputSelection::Index) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Open,
                       "desktop output selection is invalid")};
    }

    auto candidate = select_output(config.output, DesktopCaptureOperation::Open);
    if (!candidate) {
        return std::unexpected{candidate.error()};
    }

    const std::wstring output_device_name = candidate->description.DeviceName;
    const auto output_name = narrow_output_name(candidate->description.DeviceName);
    impl_->compose_pointer = config.compose_pointer;
    const auto initialize_result =
        impl_->initialize(std::move(*candidate), DesktopCaptureOperation::Open);
    if (!initialize_result) {
        impl_->release_capture_resources();
        impl_->compose_pointer = false;
        return std::unexpected{initialize_result.error()};
    }

    const auto& mode = impl_->duplication_description_.ModeDesc;
    const auto rotation = impl_->duplication_description_.Rotation;
    const bool swaps_dimensions =
        rotation == DXGI_MODE_ROTATION_ROTATE90 ||
        rotation == DXGI_MODE_ROTATION_ROTATE270;
    impl_->output_device_name = output_device_name;
    impl_->owner_thread = std::this_thread::get_id();
    impl_->open = true;
    return DesktopCaptureInfo{
        output_name,
        swaps_dimensions ? mode.Height : mode.Width,
        swaps_dimensions ? mode.Width : mode.Height,
    };
}

DesktopCaptureResult DxgiDesktopCaptureBackend::capture_latest() {
    if (!impl_->open) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Acquire,
                       "DXGI desktop capture backend is not open")};
    }
    if (impl_->owner_thread != std::this_thread::get_id()) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Acquire,
                       "DXGI desktop capture backend was called from a different thread")};
    }

    if (impl_->needs_reinitialize) {
        const auto result = impl_->reinitialize();
        if (!result) {
            if (is_recoverable(result.error())) {
                return DesktopCaptureObservation{
                    DesktopTemporarilyUnavailable{result.error()}};
            }
            return std::unexpected{result.error()};
        }
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource> desktop_resource;
    const auto acquire_result = impl_->duplication->AcquireNextFrame(
        0, &frame_info, desktop_resource.GetAddressOf());
    if (acquire_result == DXGI_ERROR_WAIT_TIMEOUT) {
        return DesktopCaptureObservation{DesktopNoChange{}};
    }
    if (FAILED(acquire_result)) {
        auto error = make_issue(
            DesktopCaptureOperation::Acquire,
            acquire_result,
            "failed to acquire the latest DXGI desktop frame");
        if (is_recoverable(acquire_result)) {
            impl_->begin_recovery();
            return DesktopCaptureObservation{
                DesktopTemporarilyUnavailable{std::move(error)}};
        }
        return std::unexpected{std::move(error)};
    }
    AcquiredFrame acquired_frame{impl_->duplication.Get()};

    const auto pointer_result = impl_->update_pointer(frame_info);
    if (!pointer_result) {
        if (is_recoverable(pointer_result.error())) {
            (void)acquired_frame.release();
            impl_->begin_recovery();
            return DesktopCaptureObservation{
                DesktopTemporarilyUnavailable{pointer_result.error()}};
        }
        return std::unexpected{pointer_result.error()};
    }

    ComPtr<ID3D11Texture2D> desktop_texture;
    const auto texture_result = desktop_resource.As(&desktop_texture);
    if (FAILED(texture_result)) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Copy,
                       texture_result,
                       "failed to query the DXGI desktop texture")};
    }

    D3D11_TEXTURE2D_DESC source_description{};
    desktop_texture->GetDesc(&source_description);
    if (source_description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return std::unexpected{
            make_issue(DesktopCaptureOperation::Copy,
                       "acquired DXGI desktop texture is not BGRA8")};
    }

    const auto staging_result = impl_->ensure_staging(source_description);
    if (!staging_result) {
        if (is_recoverable(staging_result.error())) {
            (void)acquired_frame.release();
            impl_->begin_recovery();
            return DesktopCaptureObservation{
                DesktopTemporarilyUnavailable{staging_result.error()}};
        }
        return std::unexpected{staging_result.error()};
    }

    impl_->context->CopyResource(impl_->staging.Get(), desktop_texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto map_result = impl_->context->Map(
        impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_result)) {
        auto error = make_issue(
            DesktopCaptureOperation::Map,
            map_result,
            "failed to map the DXGI staging texture");
        if (is_recoverable(map_result)) {
            (void)acquired_frame.release();
            impl_->begin_recovery();
            return DesktopCaptureObservation{
                DesktopTemporarilyUnavailable{std::move(error)}};
        }
        return std::unexpected{std::move(error)};
    }

    std::expected<DesktopImage, DesktopCaptureIssue> image_result;
    {
        const MappedTexture mapped_texture{
            impl_->context.Get(), impl_->staging.Get(), mapped};
        image_result = copy_desktop_image(
            source_description,
            impl_->duplication_description_.Rotation,
            mapped_texture.get());
    }
    if (!image_result) {
        return std::unexpected{image_result.error()};
    }

    if (impl_->compose_pointer && impl_->pointer_visible && impl_->pointer_shape) {
        const auto compose_result = compose_desktop_pointer(
            *image_result, *impl_->pointer_shape, impl_->pointer_position);
        if (!compose_result) {
            return std::unexpected{
                make_issue(DesktopCaptureOperation::Pointer,
                           "failed to compose the DXGI desktop pointer: " +
                               compose_result.error())};
        }
    }

    const auto release_result = acquired_frame.release();
    if (FAILED(release_result)) {
        auto error = make_issue(
            DesktopCaptureOperation::Acquire,
            release_result,
            "failed to release the DXGI desktop frame");
        if (is_recoverable(release_result)) {
            impl_->begin_recovery();
            return DesktopCaptureObservation{
                DesktopTemporarilyUnavailable{std::move(error)}};
        }
        return std::unexpected{std::move(error)};
    }
    return DesktopCaptureObservation{std::move(*image_result)};
}

void DxgiDesktopCaptureBackend::close() noexcept {
    impl_->release_capture_resources();
    impl_->output_device_name.clear();
    impl_->owner_thread = {};
    impl_->compose_pointer = false;
    impl_->open = false;
    impl_->needs_reinitialize = false;
}

}  // namespace semilive::publisher::infra::capture
