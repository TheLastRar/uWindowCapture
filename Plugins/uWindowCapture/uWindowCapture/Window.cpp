#include <vector>
#include <algorithm>
#include "Window.h"
#include "WindowManager.h"
#include "Device.h"
#include "Debug.h"

using namespace Microsoft::WRL;



Window::Window(HWND hwnd, int id)
    : window_(hwnd)
    , id_(id)
{
    if (!IsWindow())
    {
        Debug::Error("Given window handle is not a window.");
        window_ = nullptr;
    }
}


Window::~Window()
{
    if (captureThread_.joinable())
    {
        captureThread_.join();
    }
    DeleteBitmap();

    sharedTexture_.Reset();
}


void Window::Update()
{
    isAlive_ = true;
    owner_ = ::GetWindow(window_, GW_OWNER);
    isAltTabWindow_ = IsAltTabWindow(window_);

    // set title
    const auto titleLength = GetWindowTextLengthW(window_);
    std::vector<WCHAR> buf(titleLength + 1);
    if (GetWindowTextW(window_, &buf[0], static_cast<int>(buf.size())))
    {
        title_ = &buf[0];
    }

    // Send message
    if (!hasCaptureFinished_)
    {
        Message message;
        message.type = MessageType::WindowCaptured;
        message.windowId = id_;
        message.windowHandle = window_;
        GetWindowManager()->AddMessage(message);
        hasCaptureFinished_ = true;
    }
}


HWND Window::GetHandle() const
{
    return window_;
}


HWND Window::GetOwner() const
{
    return GetWindow(window_, GW_OWNER);
}


bool Window::IsAltTab() const
{
    return isAltTabWindow_;
}


bool Window::IsDesktop() const
{
    return isDesktop_;
}


BOOL Window::IsWindow() const
{
    return ::IsWindow(window_);
}


BOOL Window::IsVisible() const
{
    return ::IsWindowVisible(window_);
}


BOOL Window::IsEnabled() const
{
    return ::IsWindowEnabled(window_);
}


BOOL Window::IsUnicode() const
{
    return ::IsWindowUnicode(window_);
}


BOOL Window::IsZoomed() const
{
    return ::IsZoomed(window_);
}


BOOL Window::IsIconic() const
{
    return ::IsIconic(window_);
}


BOOL Window::IsHungUp() const
{
    return ::IsHungAppWindow(window_);
}


BOOL Window::IsTouchable() const
{
    return ::IsTouchWindow(window_, NULL);
}


RECT Window::GetRect() const
{
    RECT rect;
    if (!::GetWindowRect(window_, &rect))
    {
        OutputApiError("GetWindowRect");
    }
    return std::move(rect);
}


UINT Window::GetX() const
{
    const auto rect = GetRect();
    return rect.left;
}


UINT Window::GetY() const
{
    const auto rect = GetRect();
    return rect.top;
}


UINT Window::GetWidth() const
{
    const auto rect = GetRect();
    return rect.right - rect.left;
}


UINT Window::GetHeight() const
{
    const auto rect = GetRect();
    return rect.bottom - rect.top;
}


UINT Window::GetZOrder() const
{
    int z = 0;
    auto hWnd = GetWindow(window_, GW_HWNDPREV);
    while (hWnd != NULL)
    {
        hWnd = GetWindow(hWnd, GW_HWNDPREV);
        ++z;
    }
    return z;
}


UINT Window::GetTitleLength() const
{
    return static_cast<UINT>(title_.length());
}


const std::wstring& Window::GetTitle() const
{
    return title_;
}


void Window::CreateBitmapIfNeeded(HDC hDc, UINT width, UINT height)
{
    if (width_ == width && height_ == height) return;
    if (width == 0 || height == 0) return;

    width_ = width;
    height_ = height;

    {
        Message message;
        message.type = MessageType::WindowSizeChanged;
        message.windowId = id_;
        message.windowHandle = window_;
        GetWindowManager()->AddMessage(message);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.ExpandIfNeeded(width * height * 4);
    }

    DeleteBitmap();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bitmap_ = ::CreateCompatibleBitmap(hDc, width, height);
    }
}


void Window::DeleteBitmap()
{
    if (bitmap_ != nullptr) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!::DeleteObject(bitmap_)) OutputApiError("DeleteObject");
        bitmap_ = nullptr;
    }
}


void Window::SetTexturePtr(ID3D11Texture2D* ptr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    unityTexture_ = ptr;
}


ID3D11Texture2D* Window::GetTexturePtr() const
{
    return unityTexture_;
}


void Window::SetCaptureMode(CaptureMode mode)
{
    mode_ = mode;
}


Window::CaptureMode Window::GetCaptureMode() const
{
    return mode_;
}


void Window::Capture()
{
    if (!IsWindow())
    {
        Debug::Error("Window doesn't exist anymore.");
        return;
    }

    if (!IsVisible() || IsIconic() || GetWidth() == 0)
    {
        return;
    }

    if (hasCaptureFinished_)
    {
        if (captureThread_.joinable()) {
            captureThread_.join();
        }

        if (mode_ != CaptureMode::None)
        {
            captureThread_ = std::thread([&] 
            {
                hasCaptureFinished_ = false;
                CaptureInternal();
                if (auto& manager = GetWindowManager())
                {
                    manager->AddToUploadList(id_);
                }
                hasCaptureFinished_ = true;
            });
        }
    }
}


void Window::CaptureInternal()
{
    auto hDc = ::GetDC(window_);

    const auto width = GetWidth();
    const auto height = GetHeight();
    if (width == 0 || height == 0)
    {
        if (!::ReleaseDC(window_, hDc)) OutputApiError("ReleaseDC");
    }
    CreateBitmapIfNeeded(hDc, width, height);

    auto hDcMem = ::CreateCompatibleDC(hDc);
    HGDIOBJ preObject = ::SelectObject(hDcMem, bitmap_);

    BOOL result = false;
    switch (mode_)
    {
        case CaptureMode::PrintWindow:
        {
            result = ::PrintWindow(window_, hDcMem, PW_RENDERFULLCONTENT);
            if (!result) OutputApiError("PrintWindow");
            break;
        }
        case CaptureMode::BitBlt:
        {
            result = ::BitBlt(hDcMem, 0, 0, width_, height_, hDc, 0, 0, SRCCOPY);
            if (!result) OutputApiError("BitBlt");
            break;
        }
        case CaptureMode::BitBltAlpha:
        {
            result = ::BitBlt(hDcMem, 0, 0, width_, height_, hDc, 0, 0, SRCCOPY | CAPTUREBLT);
            if (!result) OutputApiError("BitBlt");
            break;
        }
        default:
        {
            break;
        }
    }

    if (result)
    {
        BITMAPINFOHEADER bmi {};
        bmi.biWidth       = static_cast<LONG>(width);
        bmi.biHeight      = -static_cast<LONG>(height);
        bmi.biPlanes      = 1;
        bmi.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.biBitCount    = 32;
        bmi.biCompression = BI_RGB;
        bmi.biSizeImage   = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!::GetDIBits(hDcMem, bitmap_, 0, height, buffer_.Get(), reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS))
            {
                OutputApiError("GetDIBits");
            }
        }
    }

    ::SelectObject(hDcMem, preObject);

    if (!::DeleteDC(hDcMem)) OutputApiError("DeleteDC");
    if (!::ReleaseDC(window_, hDc)) OutputApiError("ReleaseDC");
}


void Window::UploadTextureToGpu(const std::shared_ptr<IsolatedD3D11Device>& device)
{
    bool shouldUpdateTexture = true;

    if (sharedTexture_)
    {
        D3D11_TEXTURE2D_DESC desc;
        sharedTexture_->GetDesc(&desc);
        if (desc.Width == width_ && desc.Height == height_)
        {
            shouldUpdateTexture = false;
        }
    }

    if (shouldUpdateTexture)
    {
        sharedTexture_ = device->CreateSharedTexture(width_, height_);
        if (!sharedTexture_)
        {
            Debug::Error("Window::UploadTextureToGpu() => Shared texture is null.");
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ComPtr<ID3D11DeviceContext> context;
        device->GetDevice()->GetImmediateContext(&context);
        context->UpdateSubresource(sharedTexture_.Get(), 0, nullptr, buffer_.Get(), width_ * 4, 0);
    }

    hasNewTextureUploaded_ = true;
}


void Window::Render()
{
    //if (!hasNewTextureUploaded_) return;
    //hasNewTextureUploaded_ = false;

    if (!unityTexture_ || !sharedTexture_) return;

    // Check given texture size.
    D3D11_TEXTURE2D_DESC dstDesc;
    unityTexture_->GetDesc(&dstDesc);

    D3D11_TEXTURE2D_DESC srcDesc;
    sharedTexture_->GetDesc(&srcDesc);

    if (dstDesc.Width != srcDesc.Width || 
        dstDesc.Height != srcDesc.Height)
    {
         return;
    }

    // Copy shared texture to given texture
    {
        ComPtr<ID3D11DeviceContext> context;
        GetUnityDevice()->GetImmediateContext(&context);
        context->CopyResource(unityTexture_, sharedTexture_.Get());
        //context->UpdateSubresource(unityTexture_, 0, nullptr, buffer_.Get(), width_ * 4, 0);
    }
}