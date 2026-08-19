#include "pch.h"
#include "CustomBlurEffect.h"
#include "CustomInvertEffect.h"
#include "CustomLiquidGlassEffect.h"
#include "GaussianBlurEffect.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#if 0
static auto _x = [] {
    wchar_t path[MAX_PATH * 2];
    // Attempt to load custom effect DLL to test cross-version stability.
    DWORD dwLen = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    constexpr wchar_t kCustomEffectDllName[] = L"wuceffectsi.dll";
    if (dwLen > 0 && dwLen + ARRAYSIZE(kCustomEffectDllName) < ARRAYSIZE(path))
    {
        if (auto slash = wcsrchr(path, L'\\'))
        {
            memcpy(slash + 1, kCustomEffectDllName, sizeof(kCustomEffectDllName));
            return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        }
    }
    return (HMODULE)0;
}();
#endif

using namespace winrt;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Microsoft::UI::Composition;
using namespace Microsoft::UI::Input;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media::Animation;

namespace Imaging = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace Media = winrt::Microsoft::UI::Xaml::Media;

namespace
{
    constexpr float kInitialBackdropWidth = 420.0f;
    constexpr float kInitialBackdropHeight = 260.0f;
    constexpr float kInitialBackdropOffsetX = 268.0f;
    constexpr float kInitialBackdropOffsetY = 80.0f;
    constexpr float kMinimumBackdropWidth = 120.0f;
    constexpr float kMinimumBackdropHeight = 80.0f;
    constexpr float kResizeGripSize = 32.0f;

    bool IsSupportedImageFile(StorageFile const& file)
    {
        std::wstring extension{ file.FileType() };
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(std::towlower(value));
        });

        return extension == L".jpg" ||
            extension == L".jpeg" ||
            extension == L".png" ||
            extension == L".bmp" ||
            extension == L".gif" ||
            extension == L".webp";
    }
}

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace winrt::WUILiquidGlassDemo_WUI3::implementation
{
    MainWindow::MainWindow()
    {
    }

    void MainWindow::InitializeComponent()
    {
        MainWindowT::InitializeComponent();

        StartDynamicScene();
        InitializeBackdropCursors();

        m_borderWidth = static_cast<float>(BorderWidthSlider().Value());
        BackdropFrame().BorderThickness({ m_borderWidth, m_borderWidth, m_borderWidth, m_borderWidth });
        Controls::Canvas::SetLeft(BackdropFrame(), kInitialBackdropOffsetX);
        Controls::Canvas::SetTop(BackdropFrame(), kInitialBackdropOffsetY);
        BackdropFrame().Width(kInitialBackdropWidth);
        BackdropFrame().Height(kInitialBackdropHeight);

        InitializeBackdropBrush();
        ApplyBackdropEffect();

        ClampBackdropFrameRect();
        UpdateLiquidGlassControlsState();
    }

    void MainWindow::StartDynamicScene()
    {
        if (auto storyboard = Root().Resources()
            .Lookup(box_value(L"DynamicSceneStoryboard"))
            .try_as<Storyboard>())
        {
            storyboard.Begin();
        }
    }

    void MainWindow::InitializeBackdropBrush()
    {
        auto const factory = get_activation_factory<
            Media::XamlCompositionBrushBase,
            Windows::Foundation::IActivationFactory>();

        m_backdropXamlBrush = factory.ActivateInstance<Media::XamlCompositionBrushBase>();
        m_backdropBrushProtected = m_backdropXamlBrush.as<Media::IXamlCompositionBrushBaseProtected>();
        m_backdropXamlBrush.FallbackColor(winrt::Windows::UI::Color{ 0x99, 0xff, 0xff, 0xff });
        BackdropFrame().Background(m_backdropXamlBrush.as<Media::Brush>());
    }

    fire_and_forget MainWindow::PickBackgroundImageAsync()
    {
        auto lifetime = get_strong();

        try
        {
            FileOpenPicker picker;
            picker.ViewMode(PickerViewMode::Thumbnail);
            picker.SuggestedStartLocation(PickerLocationId::PicturesLibrary);
            picker.FileTypeFilter().Append(L".jpg");
            picker.FileTypeFilter().Append(L".jpeg");
            picker.FileTypeFilter().Append(L".png");
            picker.FileTypeFilter().Append(L".bmp");
            picker.FileTypeFilter().Append(L".gif");
            picker.FileTypeFilter().Append(L".webp");

            auto const hwnd = GetWindowHandle();
            if (hwnd)
            {
                check_hresult(picker.as<IInitializeWithWindow>()->Initialize(hwnd));
            }

            if (auto file = co_await picker.PickSingleFileAsync())
            {
                co_await SetBackgroundImageAsync(file);
            }
        }
        catch (...)
        {
        }
    }

    fire_and_forget MainWindow::SetBackgroundImageFromDropAsync(DragEventArgs args)
    {
        auto lifetime = get_strong();
        auto deferral = args.GetDeferral();

        try
        {
            auto const items = co_await args.DataView().GetStorageItemsAsync();
            for (auto const& item : items)
            {
                if (auto file = item.try_as<StorageFile>(); file && IsSupportedImageFile(file))
                {
                    co_await SetBackgroundImageAsync(file);
                    break;
                }
            }
        }
        catch (...)
        {
        }

        deferral.Complete();
    }

    IAsyncAction MainWindow::SetBackgroundImageAsync(StorageFile file)
    {
        try
        {
            if (!file || !IsSupportedImageFile(file))
            {
                co_return;
            }

            auto stream = co_await file.OpenReadAsync();
            Imaging::BitmapImage image;
            co_await image.SetSourceAsync(stream);
            BackgroundImage().Source(image);
            BackgroundImage().Visibility(Visibility::Visible);
        }
        catch (...)
        {
        }
    }

    void MainWindow::ClearBackgroundImage()
    {
        BackgroundImage().Source(nullptr);
        BackgroundImage().Visibility(Visibility::Collapsed);
    }

    void MainWindow::ApplyBackdropEffect()
    {
        if (!m_backdropBrushProtected)
        {
            return;
        }

        auto const compositor = Media::CompositionTarget::GetCompositorForCurrentThread();
        m_backdropEffectBrush = nullptr;
        m_gaussianBlurBrush = nullptr;
        switch (m_backdropEffect)
        {
        case BackdropEffectKind::LinearGradient:
        {
            auto brush = compositor.CreateLinearGradientBrush();
            brush.MappingMode(CompositionMappingMode::Relative);
            brush.StartPoint(Windows::Foundation::Numerics::float2{ 0.0f, 0.0f });
            brush.EndPoint(Windows::Foundation::Numerics::float2{ 1.0f, 1.0f });
            brush.ColorStops().Append(compositor.CreateColorGradientStop(
                0.0f,
                winrt::Windows::UI::Color{ 0xff, 0xff, 0x5c, 0x7a }));
            brush.ColorStops().Append(compositor.CreateColorGradientStop(
                0.48f,
                winrt::Windows::UI::Color{ 0xff, 0x26, 0xd0, 0xce }));
            brush.ColorStops().Append(compositor.CreateColorGradientStop(
                1.0f,
                winrt::Windows::UI::Color{ 0xff, 0xff, 0xd1, 0x66 }));
            m_backdropBrushProtected.CompositionBrush(brush);
            EffectCaption().Text(L"Relative composition linear gradient");
            break;
        }
        case BackdropEffectKind::Invert:
        {
            auto factory = compositor.CreateEffectFactory(CustomInvertEffect::CreateEffect());
            auto brush = factory.CreateBrush();
            brush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());
            m_backdropEffectBrush = brush;
            m_backdropBrushProtected.CompositionBrush(brush);
            EffectCaption().Text(L"Backdrop inversion");
            break;
        }
        case BackdropEffectKind::Blur:
        {
            auto factory = compositor.CreateEffectFactory(CustomBlurEffect::CreateEffect());
            auto brush = factory.CreateBrush();
            brush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());
            m_backdropEffectBrush = brush;
            m_backdropBrushProtected.CompositionBrush(brush);
            EffectCaption().Text(L"Backdrop blur");
            break;
        }
        case BackdropEffectKind::LiquidGlass:
        {
            auto blurAnimatableProperties = single_threaded_vector<hstring>();
            blurAnimatableProperties.Append(GaussianBlurEffect::BlurAmountPropertyPath);
            auto blurFactory = compositor.CreateEffectFactory(
                GaussianBlurEffect::CreateEffect(
                    L"Backdrop",
                    static_cast<float>(GaussianBlurRadiusSlider().Value())),
                blurAnimatableProperties);
            auto blurBrush = blurFactory.CreateBrush();
            blurBrush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());
            m_gaussianBlurBrush = blurBrush;

            auto animatableProperties = single_threaded_vector<hstring>();
            animatableProperties.Append(CustomLiquidGlassEffect::BlurRadiusPropertyPath);
            animatableProperties.Append(CustomLiquidGlassEffect::RefractionStrengthPropertyPath);
            animatableProperties.Append(CustomLiquidGlassEffect::CornerRadiusPropertyPath);
            animatableProperties.Append(CustomLiquidGlassEffect::BorderThicknessPropertyPath);
            animatableProperties.Append(CustomLiquidGlassEffect::HighlightStrengthPropertyPath);
            animatableProperties.Append(CustomLiquidGlassEffect::DispersionStrengthPropertyPath);

            auto factory = compositor.CreateEffectFactory(
                CustomLiquidGlassEffect::CreateEffect(),
                animatableProperties);
            auto brush = factory.CreateBrush();
#if 1
            brush.SetSourceParameter(L"Backdrop", blurBrush);
#else
            brush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());
#endif
            m_backdropEffectBrush = brush;
            m_backdropBrushProtected.CompositionBrush(brush);
            ApplyGaussianBlurProperties();
            ApplyLiquidGlassProperties();
            EffectCaption().Text(L"Liquid glass material");
            break;
        }
        case BackdropEffectKind::Solid:
        default:
            m_backdropBrushProtected.CompositionBrush(
                compositor.CreateColorBrush(winrt::Windows::UI::Color{ 0x99, 0xff, 0xff, 0xff }));
            EffectCaption().Text(L"Solid translucent brush");
            break;
        }

        UpdateLiquidGlassControlsState();
    }

    void MainWindow::ApplyLiquidGlassProperties()
    {
        if (m_backdropEffect != BackdropEffectKind::LiquidGlass || !m_backdropEffectBrush)
        {
            return;
        }

        auto properties = m_backdropEffectBrush.Properties();
        properties.InsertScalar(
            CustomLiquidGlassEffect::BlurRadiusPropertyPath,
            static_cast<float>(BlurRadiusSlider().Value()));
        properties.InsertScalar(
            CustomLiquidGlassEffect::RefractionStrengthPropertyPath,
            static_cast<float>(RefractionStrengthSlider().Value()));
        properties.InsertScalar(
            CustomLiquidGlassEffect::CornerRadiusPropertyPath,
            static_cast<float>(CornerRadiusSlider().Value()));
        properties.InsertScalar(
            CustomLiquidGlassEffect::BorderThicknessPropertyPath,
            static_cast<float>(MaterialBorderThicknessSlider().Value()));
        properties.InsertScalar(
            CustomLiquidGlassEffect::HighlightStrengthPropertyPath,
            static_cast<float>(HighlightStrengthSlider().Value()));
        properties.InsertScalar(
            CustomLiquidGlassEffect::DispersionStrengthPropertyPath,
            static_cast<float>(DispersionStrengthSlider().Value()));
    }

    void MainWindow::ApplyGaussianBlurProperties()
    {
        if (m_backdropEffect != BackdropEffectKind::LiquidGlass || !m_gaussianBlurBrush)
        {
            return;
        }

        m_gaussianBlurBrush.Properties().InsertScalar(
            GaussianBlurEffect::BlurAmountPropertyPath,
            static_cast<float>(GaussianBlurRadiusSlider().Value()));
    }

    void MainWindow::UpdateLiquidGlassControlsState()
    {
        auto const enabled = m_backdropEffect == BackdropEffectKind::LiquidGlass;
        LiquidGlassControls().IsHitTestVisible(enabled);
        LiquidGlassControls().Opacity(enabled ? 1.0 : 0.55);
    }

    void MainWindow::ClampBackdropFrameRect()
    {
        auto const rootWidth = static_cast<float>(BackdropHost().ActualWidth());
        auto const rootHeight = static_cast<float>(BackdropHost().ActualHeight());
        if (rootWidth <= 0.0f || rootHeight <= 0.0f)
        {
            return;
        }

        auto const width = std::clamp(
            static_cast<float>(BackdropFrame().Width()),
            kMinimumBackdropWidth,
            std::max(kMinimumBackdropWidth, rootWidth));
        auto const height = std::clamp(
            static_cast<float>(BackdropFrame().Height()),
            kMinimumBackdropHeight,
            std::max(kMinimumBackdropHeight, rootHeight));
        auto const x = std::clamp(
            static_cast<float>(Controls::Canvas::GetLeft(BackdropFrame())),
            0.0f,
            std::max(0.0f, rootWidth - width));
        auto const y = std::clamp(
            static_cast<float>(Controls::Canvas::GetTop(BackdropFrame())),
            0.0f,
            std::max(0.0f, rootHeight - height));

        BackdropFrame().Width(width);
        BackdropFrame().Height(height);
        Controls::Canvas::SetLeft(BackdropFrame(), x);
        Controls::Canvas::SetTop(BackdropFrame(), y);
    }

    bool MainWindow::HitTestBackdropFrame(Point const& position)
    {
        auto const x = static_cast<float>(Controls::Canvas::GetLeft(BackdropFrame()));
        auto const y = static_cast<float>(Controls::Canvas::GetTop(BackdropFrame()));
        auto const width = static_cast<float>(BackdropFrame().Width());
        auto const height = static_cast<float>(BackdropFrame().Height());
        return position.X >= x &&
            position.X <= x + width &&
            position.Y >= y &&
            position.Y <= y + height;
    }

    bool MainWindow::HitTestResizeGrip(Point const& position)
    {
        if (!HitTestBackdropFrame(position))
        {
            return false;
        }

        auto const x = static_cast<float>(Controls::Canvas::GetLeft(BackdropFrame()));
        auto const y = static_cast<float>(Controls::Canvas::GetTop(BackdropFrame()));
        auto const width = static_cast<float>(BackdropFrame().Width());
        auto const height = static_cast<float>(BackdropFrame().Height());
        return position.X >= x + width - kResizeGripSize &&
            position.Y >= y + height - kResizeGripSize;
    }

    void MainWindow::InitializeBackdropCursors()
    {
        m_arrowCursor = InputSystemCursor::Create(InputSystemCursorShape::Arrow);
        m_moveCursor = InputSystemCursor::Create(InputSystemCursorShape::SizeAll);
        m_resizeCursor = InputSystemCursor::Create(InputSystemCursorShape::SizeNorthwestSoutheast);
        SetBackdropCursor(m_arrowCursor);
    }

    void MainWindow::EnsurePointerSource()
    {
        if (m_pointerSource)
        {
            return;
        }

        auto const xamlRoot = BackdropHost().XamlRoot();
        if (!xamlRoot)
        {
            return;
        }

        auto const island = xamlRoot.ContentIsland();
        if (!island)
        {
            return;
        }

        m_pointerSource = InputPointerSource::GetForIsland(island);
    }

    void MainWindow::SetBackdropCursor(InputCursor const& cursor)
    {
        if (!cursor)
        {
            return;
        }

        if (auto protectedHost = BackdropHost().try_as<IUIElementProtected>())
        {
            protectedHost.ProtectedCursor(cursor);
        }

        // BackdropHost sits above regular XAML content, while pointer capture is
        // managed manually for drag/resize; setting the island cursor keeps the
        // cursor stable across those hit-test transitions.
        EnsurePointerSource();
        if (m_pointerSource)
        {
            m_pointerSource.Cursor(cursor);
        }
    }

    void MainWindow::UpdateBackdropCursor(Point const& position)
    {
        if (m_backdropInteraction == BackdropInteraction::Resize || HitTestResizeGrip(position))
        {
            SetBackdropCursor(m_resizeCursor);
            return;
        }

        if (m_backdropInteraction == BackdropInteraction::Drag || HitTestBackdropFrame(position))
        {
            SetBackdropCursor(m_moveCursor);
            return;
        }

        SetBackdropCursor(m_arrowCursor);
    }

    void MainWindow::OnBackdropHostPointerPressed(IInspectable const&, PointerRoutedEventArgs const& args)
    {
        auto const point = args.GetCurrentPoint(BackdropHost());
        auto const position = point.Position();
        if (!HitTestBackdropFrame(position))
        {
            SetBackdropCursor(m_arrowCursor);
            return;
        }

        m_backdropInteraction = HitTestResizeGrip(position)
            ? BackdropInteraction::Resize
            : BackdropInteraction::Drag;
        m_activePointerId = point.PointerId();
        m_startPointer = position;
        m_startOffsetX = static_cast<float>(Controls::Canvas::GetLeft(BackdropFrame()));
        m_startOffsetY = static_cast<float>(Controls::Canvas::GetTop(BackdropFrame()));
        m_startWidth = static_cast<float>(BackdropFrame().Width());
        m_startHeight = static_cast<float>(BackdropFrame().Height());

        UpdateBackdropCursor(position);
        BackdropHost().CapturePointer(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::OnBackdropHostPointerMoved(IInspectable const&, PointerRoutedEventArgs const& args)
    {
        auto const point = args.GetCurrentPoint(BackdropHost());
        auto const position = point.Position();

        if (m_backdropInteraction == BackdropInteraction::None)
        {
            UpdateBackdropCursor(position);
            return;
        }

        if (point.PointerId() != m_activePointerId)
        {
            return;
        }

        auto const deltaX = position.X - m_startPointer.X;
        auto const deltaY = position.Y - m_startPointer.Y;
        auto const rootWidth = static_cast<float>(BackdropHost().ActualWidth());
        auto const rootHeight = static_cast<float>(BackdropHost().ActualHeight());
        UpdateBackdropCursor(position);

        if (m_backdropInteraction == BackdropInteraction::Resize)
        {
            auto const maxWidth = std::max(kMinimumBackdropWidth, rootWidth - m_startOffsetX);
            auto const maxHeight = std::max(kMinimumBackdropHeight, rootHeight - m_startOffsetY);
            BackdropFrame().Width(std::clamp(m_startWidth + deltaX, kMinimumBackdropWidth, maxWidth));
            BackdropFrame().Height(std::clamp(m_startHeight + deltaY, kMinimumBackdropHeight, maxHeight));
        }
        else
        {
            auto const width = static_cast<float>(BackdropFrame().Width());
            auto const height = static_cast<float>(BackdropFrame().Height());
            Controls::Canvas::SetLeft(
                BackdropFrame(),
                std::clamp(m_startOffsetX + deltaX, 0.0f, std::max(0.0f, rootWidth - width)));
            Controls::Canvas::SetTop(
                BackdropFrame(),
                std::clamp(m_startOffsetY + deltaY, 0.0f, std::max(0.0f, rootHeight - height)));
        }

        args.Handled(true);
    }

    void MainWindow::OnBackdropFramePointerWheelChanged(IInspectable const&, PointerRoutedEventArgs const& args)
    {
        auto const position = args.GetCurrentPoint(CenterContentScroller()).Position();
        auto const isOverScroller =
            position.X >= 0.0 &&
            position.Y >= 0.0 &&
            position.X <= CenterContentScroller().ActualWidth() &&
            position.Y <= CenterContentScroller().ActualHeight();
        if (!isOverScroller)
        {
            return;
        }

        auto const delta = args.GetCurrentPoint(BackdropFrame()).Properties().MouseWheelDelta();
        auto const nextOffset = std::clamp(
            CenterContentScroller().VerticalOffset() - static_cast<double>(delta) / 120.0 * 48.0,
            0.0,
            CenterContentScroller().ScrollableHeight());
        CenterContentScroller().ChangeView(
            nullptr,
            box_value(nextOffset).as<Windows::Foundation::IReference<double>>(),
            nullptr,
            true);
        args.Handled(true);
    }

    void MainWindow::OnSetBackgroundImageClick(IInspectable const&, RoutedEventArgs const&)
    {
        PickBackgroundImageAsync();
    }

    void MainWindow::OnClearBackgroundImageClick(IInspectable const&, RoutedEventArgs const&)
    {
        ClearBackgroundImage();
    }

    void MainWindow::OnRootDragOver(IInspectable const&, DragEventArgs const& args)
    {
        if (args.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            args.AcceptedOperation(DataPackageOperation::Copy);
            args.DragUIOverride().Caption(L"Set background image");
            args.Handled(true);
        }
    }

    void MainWindow::OnRootDrop(IInspectable const&, DragEventArgs const& args)
    {
        if (!args.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            return;
        }

        args.AcceptedOperation(DataPackageOperation::Copy);
        SetBackgroundImageFromDropAsync(args);
        args.Handled(true);
    }

    void MainWindow::OnBackdropHostLoaded(IInspectable const&, RoutedEventArgs const&)
    {
        EnsurePointerSource();
        SetBackdropCursor(m_arrowCursor);
    }

    void MainWindow::OnBackdropHostSizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        ClampBackdropFrameRect();
    }

    void MainWindow::OnBackdropHostPointerReleased(IInspectable const&, PointerRoutedEventArgs const& args)
    {
        if (m_backdropInteraction != BackdropInteraction::None)
        {
            auto const position = args.GetCurrentPoint(BackdropHost()).Position();
            BackdropHost().ReleasePointerCapture(args.Pointer());
            EndBackdropInteraction();
            UpdateBackdropCursor(position);
            args.Handled(true);
        }
    }

    void MainWindow::OnBackdropHostPointerCanceled(IInspectable const&, PointerRoutedEventArgs const& args)
    {
        if (m_backdropInteraction != BackdropInteraction::None)
        {
            BackdropHost().ReleasePointerCapture(args.Pointer());
            EndBackdropInteraction();
            SetBackdropCursor(m_arrowCursor);
            args.Handled(true);
        }
    }

    void MainWindow::OnBackdropHostPointerCaptureLost(IInspectable const&, PointerRoutedEventArgs const&)
    {
        EndBackdropInteraction();
        SetBackdropCursor(m_arrowCursor);
    }

    void MainWindow::OnBackdropHostPointerExited(IInspectable const&, PointerRoutedEventArgs const&)
    {
        if (m_backdropInteraction == BackdropInteraction::None)
        {
            SetBackdropCursor(m_arrowCursor);
        }
    }

    void MainWindow::OnEffectSelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        auto const selectedIndex = EffectSelector().SelectedIndex();
        if (selectedIndex == 1)
        {
            m_backdropEffect = BackdropEffectKind::LinearGradient;
        }
        else if (selectedIndex == 2)
        {
            m_backdropEffect = BackdropEffectKind::Blur;
        }
        else if (selectedIndex == 3)
        {
            m_backdropEffect = BackdropEffectKind::Invert;
        }
        else if (selectedIndex == 4)
        {
            m_backdropEffect = BackdropEffectKind::LiquidGlass;
        }
        else
        {
            m_backdropEffect = BackdropEffectKind::Solid;
        }

        ApplyBackdropEffect();
    }

    void MainWindow::OnBorderWidthChanged(
        IInspectable const&,
        Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        m_borderWidth = static_cast<float>(args.NewValue());
        BackdropFrame().BorderThickness({ m_borderWidth, m_borderWidth, m_borderWidth, m_borderWidth });
    }

    void MainWindow::OnLiquidGlassParameterChanged(
        IInspectable const&,
        Controls::Primitives::RangeBaseValueChangedEventArgs const&)
    {
        ApplyLiquidGlassProperties();
    }

    void MainWindow::OnGaussianBlurRadiusChanged(
        IInspectable const&,
        Controls::Primitives::RangeBaseValueChangedEventArgs const&)
    {
        ApplyGaussianBlurProperties();
    }

    void MainWindow::EndBackdropInteraction()
    {
        m_backdropInteraction = BackdropInteraction::None;
        m_activePointerId = 0;
    }

    HWND MainWindow::GetWindowHandle()
    {
        HWND hwnd{};
        if (auto nativeWindow = this->try_as<IWindowNative>())
        {
            check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        }
        return hwnd;
    }
}
