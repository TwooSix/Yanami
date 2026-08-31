#include "InstallerPainter.hpp"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace yanami::installer::ui {
namespace {

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

template <typename T>
struct ComValue {
    T* value = nullptr;
    ~ComValue() { release(value); }
    ComValue() = default;
    ComValue(const ComValue&) = delete;
    ComValue& operator=(const ComValue&) = delete;
    T* operator->() const noexcept { return value; }
};

D2D1_COLOR_F colorValue(COLORREF color) noexcept {
    return D2D1::ColorF(GetRValue(color) / 255.0f,
                       GetGValue(color) / 255.0f,
                       GetBValue(color) / 255.0f, 1.0f);
}

D2D1_RECT_F rectValue(const RECT& bounds) noexcept {
    return D2D1::RectF(static_cast<float>(bounds.left),
                       static_cast<float>(bounds.top),
                       static_cast<float>(bounds.right),
                       static_cast<float>(bounds.bottom));
}

bool nonEmpty(const RECT& bounds) noexcept {
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

std::wstring installedFamily(IDWriteFontCollection* collection, bool chinese) {
    const std::array<const wchar_t*, 3> candidates = chinese
        ? std::array<const wchar_t*, 3>{
              L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI"}
        : std::array<const wchar_t*, 3>{
              L"Segoe UI Variable Display", L"Segoe UI", L"Arial"};
    for (const wchar_t* candidate : candidates) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(candidate, &index, &exists))
            && exists) {
            return candidate;
        }
    }

    // Even on a stripped-down Windows installation, return an installed face
    // rather than allow an unrecognised name to silently fall back to a serif.
    ComValue<IDWriteFontFamily> family;
    ComValue<IDWriteLocalizedStrings> names;
    if (collection->GetFontFamilyCount() == 0
        || FAILED(collection->GetFontFamily(0, &family.value))
        || FAILED(family->GetFamilyNames(&names.value))
        || names->GetCount() == 0) {
        return {};
    }
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(0, &length))) {
        return {};
    }
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(0, name.data(), length + 1))) {
        return {};
    }
    name.resize(length);
    return name;
}

} // namespace

struct InstallerPainter::Impl {
    struct CachedLayer {
        ID2D1Layer* value = nullptr;
        bool active = false;
        ~CachedLayer() { release(value); }
    };

    struct RoundedClip {
        RECT bounds{};
        float radius = 0.0f;
        ComValue<ID2D1RoundedRectangleGeometry> geometry;
        // One geometry may be nested inside itself. A layer is reusable only
        // after PopLayer, so retain a small pool for that key's peak depth.
        std::vector<std::unique_ptr<CachedLayer>> layers;

        bool active() const noexcept {
            return std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
                return layer->active;
            });
        }
    };

    enum class ClipKind { AxisAligned, Rounded };
    struct ClipEntry {
        ClipKind kind;
        CachedLayer* layer = nullptr;
    };

    ID2D1Factory* factory = nullptr;
    IDWriteFactory* writeFactory = nullptr;
    IDWriteFontCollection* fonts = nullptr;
    IDWriteRenderingParams* renderingParams = nullptr;
    ID2D1DCRenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::wstring chineseFamily;
    std::wstring latinFamily;
    HRESULT error = S_OK;
    std::vector<ClipEntry> clips;
    std::vector<std::unique_ptr<RoundedClip>> roundedClips;
    bool drawing = false;

    ~Impl() {
        if (drawing && target) {
            while (!clips.empty()) {
                popClip();
            }
            target->EndDraw();
        }
        discardTarget();
        roundedClips.clear();
        release(renderingParams);
        release(fonts);
        release(writeFactory);
        release(factory);
    }

    bool check(HRESULT result) noexcept {
        if (FAILED(result)) {
            if (SUCCEEDED(error)) {
                error = result;
            }
            return false;
        }
        return true;
    }

    void discardTarget() noexcept {
        // Geometries belong to the factory and survive device loss; layers
        // belong to this render target's resource domain and must not survive.
        for (auto& clip : roundedClips) {
            clip->layers.clear();
        }
        release(brush);
        release(target);
    }

    void popClip() noexcept {
        const auto entry = clips.back();
        if (entry.kind == ClipKind::Rounded) {
            target->PopLayer();
            entry.layer->active = false;
        } else {
            target->PopAxisAlignedClip();
        }
        clips.pop_back();
    }

    RoundedClip* roundedClip(const RECT& bounds, float radius) {
        for (const auto& clip : roundedClips) {
            if (EqualRect(&clip->bounds, &bounds) && clip->radius == radius) {
                return clip.get();
            }
        }
        auto clip = std::make_unique<RoundedClip>();
        clip->bounds = bounds;
        clip->radius = radius;
        if (!check(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(rectValue(bounds), radius, radius),
                &clip->geometry.value))) {
            return nullptr;
        }
        // Bound idle DPI/layout variants without ever evicting a layer that
        // is still on the stack. The common animation key allocates only once.
        constexpr size_t kIdleClipCacheSize = 8;
        if (roundedClips.size() >= kIdleClipCacheSize) {
            const auto reusable = std::find_if(roundedClips.begin(), roundedClips.end(),
                [](const auto& candidate) { return !candidate->active(); });
            if (reusable != roundedClips.end()) {
                *reusable = std::move(clip);
                return reusable->get();
            }
        }
        roundedClips.push_back(std::move(clip));
        return roundedClips.back().get();
    }

    CachedLayer* availableLayer(RoundedClip& clip) {
        for (const auto& layer : clip.layers) {
            if (!layer->active) {
                return layer.get();
            }
        }
        auto layer = std::make_unique<CachedLayer>();
        if (!check(target->CreateLayer(nullptr, &layer->value))) {
            return nullptr;
        }
        clip.layers.push_back(std::move(layer));
        return clip.layers.back().get();
    }

    bool ensureFactories() {
        if (!factory
            && !check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       &factory))) {
            return false;
        }
        if (!writeFactory
            && !check(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&writeFactory)))) {
            return false;
        }
        if (!fonts
            && !check(writeFactory->GetSystemFontCollection(&fonts, FALSE))) {
            return false;
        }
        if (chineseFamily.empty()) {
            chineseFamily = installedFamily(fonts, true);
        }
        if (latinFamily.empty()) {
            latinFamily = installedFamily(fonts, false);
        }
        if (chineseFamily.empty() || latinFamily.empty()) {
            return check(DWRITE_E_NOFONT);
        }
        if (!renderingParams) {
            ComValue<IDWriteRenderingParams> defaults;
            if (!check(writeFactory->CreateRenderingParams(&defaults.value))
                || !check(writeFactory->CreateCustomRenderingParams(
                    defaults->GetGamma(), 0.0f, 0.0f,
                    DWRITE_PIXEL_GEOMETRY_FLAT,
                    DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                    &renderingParams))) {
                return false;
            }
        }
        return true;
    }

    bool ensureTarget() {
        if (!ensureFactories()) {
            return false;
        }
        if (!target) {
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f);
            if (!check(factory->CreateDCRenderTarget(&properties, &target))) {
                return false;
            }
        }
        if (!brush
            && !check(target->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::Black), &brush))) {
            discardTarget();
            return false;
        }
        return true;
    }

    bool ready() const noexcept {
        return drawing && target && brush && SUCCEEDED(error);
    }

    ID2D1SolidColorBrush* coloredBrush(COLORREF color) noexcept {
        brush->SetColor(colorValue(color));
        return brush;
    }
};

InstallerPainter::InstallerPainter() : impl_(std::make_unique<Impl>()) {}
InstallerPainter::~InstallerPainter() = default;

bool InstallerPainter::begin(HDC dc, const RECT& pixelBounds) {
    if (impl_->drawing) {
        return impl_->check(D2DERR_WRONG_STATE);
    }
    impl_->error = S_OK;
    if (!dc || !nonEmpty(pixelBounds)) {
        return impl_->check(E_INVALIDARG);
    }
    if (!impl_->ensureTarget()) {
        return false;
    }
    if (!impl_->check(impl_->target->BindDC(dc, &pixelBounds))) {
        impl_->discardTarget();
        return false;
    }
    impl_->target->SetDpi(96.0f, 96.0f);
    impl_->target->SetTransform(D2D1::Matrix3x2F::Translation(
        -static_cast<float>(pixelBounds.left),
        -static_cast<float>(pixelBounds.top)));
    impl_->target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    impl_->target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    impl_->target->SetTextRenderingParams(impl_->renderingParams);
    impl_->target->BeginDraw();
    impl_->drawing = true;
    impl_->clips.clear();
    return true;
}

bool InstallerPainter::end() {
    if (!impl_->drawing) {
        return impl_->check(D2DERR_WRONG_STATE);
    }
    while (!impl_->clips.empty()) {
        impl_->popClip();
    }
    const HRESULT result = impl_->target->EndDraw();
    impl_->drawing = false;
    impl_->check(result);
    if (FAILED(result)) {
        // Device-dependent resources are recreated lazily at the next frame,
        // including D2DERR_RECREATE_TARGET after display/remote-session changes.
        impl_->discardTarget();
    }
    return SUCCEEDED(impl_->error);
}

void InstallerPainter::clear(COLORREF color) {
    if (impl_->ready()) {
        impl_->target->Clear(colorValue(color));
    }
}

void InstallerPainter::fillRect(const RECT& bounds, COLORREF color) {
    if (impl_->ready() && nonEmpty(bounds)) {
        impl_->target->FillRectangle(rectValue(bounds), impl_->coloredBrush(color));
    }
}

void InstallerPainter::roundedRect(const RECT& bounds, float radiusPixels,
                                   COLORREF fill, COLORREF outline,
                                   float strokePixels) {
    roundedRectSubpixel(
        PixelRect{static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                  static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)},
        radiusPixels, fill, outline, strokePixels);
}

void InstallerPainter::roundedRectSubpixel(
    const PixelRect& bounds, float radiusPixels, COLORREF fill,
    COLORREF outline, float strokePixels) {
    if (!impl_->ready()) {
        return;
    }
    if (!std::isfinite(bounds.left) || !std::isfinite(bounds.top)
        || !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom)
        || !std::isfinite(radiusPixels) || !std::isfinite(strokePixels)) {
        impl_->check(E_INVALIDARG);
        return;
    }
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return;
    }
    auto rect = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom);
    const float maxRadius = std::min(rect.right - rect.left,
                                     rect.bottom - rect.top) / 2.0f;
    const float radius = std::clamp(radiusPixels, 0.0f, maxRadius);
    impl_->target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                        impl_->coloredBrush(fill));
    if (strokePixels > 0.0f) {
        const float inset = std::min(strokePixels / 2.0f, maxRadius);
        rect.left += inset;
        rect.top += inset;
        rect.right -= inset;
        rect.bottom -= inset;
        const float strokeRadius = std::max(0.0f, radius - inset);
        impl_->target->DrawRoundedRectangle(
            D2D1::RoundedRect(rect, strokeRadius, strokeRadius),
            impl_->coloredBrush(outline), std::min(strokePixels, maxRadius * 2));
    }
}

void InstallerPainter::ellipse(const RECT& bounds, COLORREF color) {
    if (!impl_->ready() || !nonEmpty(bounds)) {
        return;
    }
    const auto rect = rectValue(bounds);
    impl_->target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F((rect.left + rect.right) / 2.0f,
                                   (rect.top + rect.bottom) / 2.0f),
                       (rect.right - rect.left) / 2.0f,
                       (rect.bottom - rect.top) / 2.0f),
        impl_->coloredBrush(color));
}

void InstallerPainter::line(float x1, float y1, float x2, float y2,
                            COLORREF color, float widthPixels) {
    if (!impl_->ready()) {
        return;
    }
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2)
        || !std::isfinite(y2) || !std::isfinite(widthPixels)
        || widthPixels <= 0.0f) {
        impl_->check(E_INVALIDARG);
        return;
    }
    impl_->target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2),
                            impl_->coloredBrush(color), widthPixels);
}

void InstallerPainter::folderIcon(const RECT& bounds, COLORREF color,
                                  float strokePixels) {
    if (!impl_->ready() || !nonEmpty(bounds)) {
        return;
    }
    if (!std::isfinite(strokePixels) || strokePixels <= 0.0f) {
        impl_->check(E_INVALIDARG);
        return;
    }

    // Keep the same proportions in a non-square box. The stroke has spare room
    // on every side, including its antialiased edge; it is never clipped to fit.
    const auto rect = rectValue(bounds);
    const float size = std::min(rect.right - rect.left, rect.bottom - rect.top);
    const float unit = size / 20.0f;
    const float left = (rect.left + rect.right - size) / 2.0f;
    const float top = (rect.top + rect.bottom - size) / 2.0f;
    const auto point = [=](float x, float y) {
        return D2D1::Point2F(left + x * unit, top + y * unit);
    };
    ComValue<ID2D1PathGeometry> geometry;
    ComValue<ID2D1GeometrySink> sink;
    ComValue<ID2D1StrokeStyle> stroke;
    if (!impl_->check(impl_->factory->CreatePathGeometry(&geometry.value))
        || !impl_->check(geometry->Open(&sink.value))) {
        return;
    }
    sink->BeginFigure(point(3.5f, 4.0f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(point(7.25f, 4.0f));
    sink->AddLine(point(9.25f, 6.0f));
    sink->AddLine(point(16.5f, 6.0f));
    sink->AddBezier(D2D1::BezierSegment(
        point(17.33f, 6.0f), point(18.0f, 6.67f), point(18.0f, 7.5f)));
    sink->AddLine(point(18.0f, 14.5f));
    sink->AddBezier(D2D1::BezierSegment(
        point(18.0f, 15.33f), point(17.33f, 16.0f), point(16.5f, 16.0f)));
    sink->AddLine(point(3.5f, 16.0f));
    sink->AddBezier(D2D1::BezierSegment(
        point(2.67f, 16.0f), point(2.0f, 15.33f), point(2.0f, 14.5f)));
    sink->AddLine(point(2.0f, 5.5f));
    sink->AddBezier(D2D1::BezierSegment(
        point(2.0f, 4.67f), point(2.67f, 4.0f), point(3.5f, 4.0f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (!impl_->check(sink->Close())) {
        return;
    }
    const auto style = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
    if (!impl_->check(impl_->factory->CreateStrokeStyle(
            &style, nullptr, 0, &stroke.value))) {
        return;
    }
    impl_->target->DrawGeometry(geometry.value, impl_->coloredBrush(color),
                                std::min(strokePixels, 2.0f * unit), stroke.value);
}

void InstallerPainter::pushClip(const RECT& bounds) {
    if (impl_->ready()) {
        try {
            // Reserve stack state before issuing the non-throwing D2D call.
            impl_->clips.push_back({Impl::ClipKind::AxisAligned});
        } catch (const std::bad_alloc&) {
            impl_->check(E_OUTOFMEMORY);
            return;
        }
        impl_->target->PushAxisAlignedClip(rectValue(bounds),
                                           D2D1_ANTIALIAS_MODE_ALIASED);
    }
}

void InstallerPainter::pushRoundedClip(const RECT& bounds, float radiusPixels) {
    if (!impl_->ready()) {
        return;
    }
    if (!std::isfinite(radiusPixels)) {
        impl_->check(E_INVALIDARG);
        return;
    }
    if (!nonEmpty(bounds)) {
        // An empty mask must suppress drawing, not silently remove clipping.
        pushClip({bounds.left, bounds.top, bounds.left, bounds.top});
        return;
    }
    const auto rectangle = rectValue(bounds);
    const float radius = std::clamp(radiusPixels, 0.0f,
        std::min(rectangle.right - rectangle.left,
                 rectangle.bottom - rectangle.top) / 2.0f);
    try {
        auto* clip = impl_->roundedClip(bounds, radius);
        if (!clip) {
            return;
        }
        auto* layer = impl_->availableLayer(*clip);
        if (!layer) {
            return;
        }
        impl_->clips.push_back({Impl::ClipKind::Rounded, layer});
        layer->active = true;
        impl_->target->PushLayer(D2D1::LayerParameters(
            rectangle, clip->geometry.value, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Identity(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
            layer->value);
    } catch (const std::bad_alloc&) {
        impl_->check(E_OUTOFMEMORY);
    }
}

void InstallerPainter::popClip() {
    // Balance clips even if a subsequent draw operation failed.
    if (impl_->drawing && impl_->target && !impl_->clips.empty()) {
        impl_->popClip();
    }
}

void InstallerPainter::text(std::wstring_view value, const RECT& bounds,
                            float fontSizePixels, bool semibold, COLORREF color,
                            TextAlign align, bool verticallyCentered, bool wrap,
                            bool ellipsis, bool chinese, bool pathEllipsis) {
    if (!impl_->ready() || value.empty() || !nonEmpty(bounds)) {
        return;
    }
    if (!std::isfinite(fontSizePixels) || fontSizePixels <= 0.0f
        || value.size() > std::numeric_limits<UINT32>::max()) {
        impl_->check(E_INVALIDARG);
        return;
    }
    ComValue<IDWriteTextFormat> format;
    if (!impl_->check(impl_->writeFactory->CreateTextFormat(
            (chinese ? impl_->chineseFamily : impl_->latinFamily).c_str(),
            impl_->fonts,
            semibold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSizePixels, chinese ? L"zh-CN" : L"en-US", &format.value))) {
        return;
    }
    const auto alignment = align == TextAlign::Center
        ? DWRITE_TEXT_ALIGNMENT_CENTER
        : (align == TextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                    : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!impl_->check(format->SetTextAlignment(alignment))
        || !impl_->check(format->SetParagraphAlignment(verticallyCentered
            ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR))
        || !impl_->check(format->SetWordWrapping(wrap
            ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP))) {
        return;
    }
    if (ellipsis || pathEllipsis) {
        ComValue<IDWriteInlineObject> trimmingSign;
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        if (pathEllipsis) {
            trimming.delimiter = L'\\';
            trimming.delimiterCount = 1;
        }
        if (!impl_->check(impl_->writeFactory->CreateEllipsisTrimmingSign(
                format.value, &trimmingSign.value))
            || !impl_->check(format->SetTrimming(&trimming, trimmingSign.value))) {
            return;
        }
    }
    ComValue<IDWriteTextLayout> layout;
    if (!impl_->check(impl_->writeFactory->CreateTextLayout(
            value.data(), static_cast<UINT32>(value.size()), format.value,
            static_cast<float>(bounds.right - bounds.left),
            static_cast<float>(bounds.bottom - bounds.top), &layout.value))) {
        return;
    }
    // DirectWrite retains natural fractional glyph positions. Grayscale AA
    // avoids the red/blue ClearType fringes visible in scaled screenshots.
    impl_->target->DrawTextLayout(
        D2D1::Point2F(static_cast<float>(bounds.left),
                      static_cast<float>(bounds.top)),
        layout.value, impl_->coloredBrush(color),
        static_cast<D2D1_DRAW_TEXT_OPTIONS>(D2D1_DRAW_TEXT_OPTIONS_NO_SNAP
                                           | D2D1_DRAW_TEXT_OPTIONS_CLIP));
}

const std::wstring& InstallerPainter::fontFamily(bool chinese) {
    if (!impl_->drawing) {
        impl_->error = S_OK;
    }
    impl_->ensureFactories();
    return chinese ? impl_->chineseFamily : impl_->latinFamily;
}

HRESULT InstallerPainter::lastError() const noexcept { return impl_->error; }

} // namespace yanami::installer::ui
