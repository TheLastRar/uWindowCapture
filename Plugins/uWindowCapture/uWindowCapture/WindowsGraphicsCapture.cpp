#include "WindowsGraphicsCapture.h"
#include "WindowManager.h"
#include "UploadManager.h"
#include "Debug.h"
#include <inspectable.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.foundation.h>
#include <Windows.h>

using namespace winrt;
using namespace winrt::Windows;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace ::Windows::Graphics::DirectX::Direct3D11;

#pragma comment(lib, "windowsapp")


bool WindowsGraphicsCapture::IsSupported()
{
    return GraphicsCaptureSession::IsSupported();
}


bool WindowsGraphicsCapture::IsCursorCaptureEnabledApiSupported()
{
    using ApiInfo = winrt::Windows::Foundation::Metadata::ApiInformation;

    if (!IsSupported()) return false;

    static bool isChecked = false;
    static bool isEnabled = false;
    if (isChecked) return isEnabled;
    isChecked = true;

    try
    {
        isEnabled = ApiInfo::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsCursorCaptureEnabled");
    }
    catch (const std::exception &e)
    {
        Debug::Log("WindowsGraphicsCapture::IsCursorCaptureEnabledApiAvailable() throws an exception :", e.what());
    }
    catch (...)
    {
        Debug::Log("WindowsGraphicsCapture::IsCursorCaptureEnabledApiAvailable() throws an unknown exception.");
    }

    return isEnabled;
}


WindowsGraphicsCapture::WindowsGraphicsCapture(HWND hWnd)
    : hWnd_(hWnd)
    , hMonitor_(NULL)
{
    CreateItem();
}


WindowsGraphicsCapture::WindowsGraphicsCapture(HMONITOR hMonitor)
    : hMonitor_(hMonitor)
    , hWnd_(NULL)
{
    CreateItem();
}


void WindowsGraphicsCapture::CreateItem()
{
    if (!IsSupported()) return;

    try
    {
        const auto factory = get_activation_factory<GraphicsCaptureItem>();
        const auto interop = factory.as<IGraphicsCaptureItemInterop>();
        if (hWnd_)
        {
            interop->CreateForWindow(
                hWnd_,
                guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), 
                reinterpret_cast<void**>(put_abi(graphicsCaptureItem_)));
        }
        else
        {
            interop->CreateForMonitor(
                hMonitor_,
                guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), 
                reinterpret_cast<void**>(put_abi(graphicsCaptureItem_)));
        }
    }
    catch (const std::exception &e)
    {
        Debug::Log("WindowsGraphicsCapture::CreateItem() throws an exception :", e.what());
    }
    catch (...)
    {
        Debug::Log("WindowsGraphicsCapture::CreateItem() throws an unknown exception.");
    }
}


void WindowsGraphicsCapture::CreatePoolAndSession()
{
    if (!graphicsCaptureItem_) return;

    size_ = graphicsCaptureItem_.Size();

    const auto& manager = WindowManager::Get().GetWindowsGraphicsCaptureManager();
    auto &device = manager->GetDevice();

    try
    {
        captureFramePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            device, 
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            size_);
        graphicsCaptureSession_ = captureFramePool_.CreateCaptureSession(graphicsCaptureItem_);
    }
    catch (const std::exception &e)
    {
        Debug::Log("WindowsGraphicsCapture::CreatePoolAndSession() throws an exception :", e.what());
    }
    catch (...)
    {
        Debug::Log("WindowsGraphicsCapture::CreatePoolAndSession() throws an unknown exception.");
    }
}


void WindowsGraphicsCapture::DestroySession()
{
    if (graphicsCaptureSession_)
    {
        graphicsCaptureSession_.Close();
        graphicsCaptureSession_ = nullptr;
    }

    if (captureFramePool_)
    {
        captureFramePool_.Close();
        captureFramePool_ = nullptr;
    }
}


WindowsGraphicsCapture::~WindowsGraphicsCapture()
{
    DestroySession();
}


void WindowsGraphicsCapture::Start()
{
    if (isStarted_) return;
    isStarted_ = true;

    if (const auto& manager = WindowManager::Get().GetWindowsGraphicsCaptureManager())
    {
        manager->Add(shared_from_this());
    }

    CreatePoolAndSession();

    if (graphicsCaptureSession_)
    {
        graphicsCaptureSession_.StartCapture();
    }
}


void WindowsGraphicsCapture::RequestStop()
{
    hasStopRequested_ = true;
}


void WindowsGraphicsCapture::Stop()
{
    hasStopRequested_ = false;
    stopTimer_ = 0.f;

    if (!isStarted_) return;
    isStarted_ = false;

    if (const auto& manager = WindowManager::Get().GetWindowsGraphicsCaptureManager())
    {
        manager->Remove(shared_from_this());
    }

    DestroySession();
}


bool WindowsGraphicsCapture::ShouldStop() const
{
    constexpr float timer = 1.f;
    return stopTimer_ > timer || hasStopRequested_;
}


void WindowsGraphicsCapture::Update(float dt)
{
    if (!IsStarted()) return;

    stopTimer_ += dt;
}


void WindowsGraphicsCapture::EnableCursorCapture(bool enabled)
{
    if (!IsCursorCaptureEnabledApiSupported())
    {
        Debug::Log("CursorCaptureEnabled API is not available.");
        return;
    }

    if (graphicsCaptureSession_) 
    {
        graphicsCaptureSession_.IsCursorCaptureEnabled(enabled);
    }
}


WindowsGraphicsCapture::Result WindowsGraphicsCapture::TryGetLatestResult()
{
    stopTimer_ = 0.f;

    if (!captureFramePool_) return {};

    Direct3D11CaptureFrame frame = nullptr;
    while (const auto nextFrame = captureFramePool_.TryGetNextFrame())
    {
        frame = nextFrame;
    }
    if (!frame) return {};

    const auto surface = frame.Surface();
    if (!surface) return {};

    auto access = surface.as<IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    const auto hr = access->GetInterface(guid_of<ID3D11Texture2D>(), texture.put_void());
    if (FAILED(hr)) return {};

    const auto size = frame.ContentSize();
    const bool hasSizeChanged = 
        (size_.Width != size.Width) || 
        (size_.Height != size.Height);

    return { texture.get(), size.Width, size.Height, hasSizeChanged };
}


void WindowsGraphicsCapture::ChangePoolSize(int width, int height)
{
    if (!captureFramePool_) return;

    const auto& manager = WindowManager::Get().GetWindowsGraphicsCaptureManager();
    auto &device = manager->GetDevice();

    size_ = { width, height };
    captureFramePool_.Recreate(
        device, 
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        size_);
}


// ---


IDirect3DDevice & WindowsGraphicsCaptureManager::GetDevice()
{
    if (!deviceWinRt_)
    {
        const auto& uploader = WindowManager::Get().GetUploadManager();
        while (!uploader->IsReady())
        {
            const std::chrono::microseconds waitTime(100);
            std::this_thread::sleep_for(waitTime);
        }

        if (auto device = uploader->GetDevice())
        {
            com_ptr<IDXGIDevice> dxgiDevice;
            const auto hr = device->QueryInterface<IDXGIDevice>(dxgiDevice.put());
            if (SUCCEEDED(hr))
            {
                com_ptr<::IInspectable> deviceWinRt;
                ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), deviceWinRt.put());
                deviceWinRt_ = deviceWinRt.as<IDirect3DDevice>();
            }
        }
    }

    return deviceWinRt_;
}


void WindowsGraphicsCaptureManager::Add(const std::shared_ptr<WindowsGraphicsCapture>& instance)
{
    std::scoped_lock lock(instancesMutex_);
    instances_.push_back(instance);
}


void WindowsGraphicsCaptureManager::Remove(const std::shared_ptr<WindowsGraphicsCapture>& instance)
{
    std::scoped_lock lock(instancesMutex_);
    instances_.erase(
        std::remove(
            instances_.begin(),
            instances_.end(),
            instance),
        instances_.end());
}


void WindowsGraphicsCaptureManager::Update(float dt)
{
    std::scoped_lock lock(instancesMutex_);

    std::vector<Ptr> toBeStoppedInstances;
    for (const auto& instance : instances_)
    {
        if (instance->ShouldStop()) 
        {
            std::scoped_lock lock(removedInstancesMutex_);
            removedInstances_.insert(instance);
            toBeStoppedInstances.push_back(instance);
            continue;
        }

        if (instance->IsValid())
        {
            instance->Update(dt);
        }
    }
}


void WindowsGraphicsCaptureManager::StopNonUpdatedInstances()
{
    std::scoped_lock lock(removedInstancesMutex_);

    for (auto &instance : removedInstances_)
    {
        instance->Stop();
    }

    removedInstances_.clear();
}


void WindowsGraphicsCaptureManager::StopAllInstances()
{
    StopNonUpdatedInstances();

    decltype(instances_) instances;
    {
        std::scoped_lock lock(instancesMutex_);
        instances = instances_;
        instances_.clear();
    }

    for (auto &instance : instances)
    {
        instance->Stop();
    }
}