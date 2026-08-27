// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackCoverArt.h"

#include "image/CoverArtPresenter.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::SizeChangedEventArgs;
    using winrt::Microsoft::UI::Xaml::Controls::Border;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::Image;
    using winrt::Microsoft::UI::Xaml::Media::Stretch;
    using winrt::Windows::Foundation::IInspectable;

    constexpr auto kPlaceholderStyleProp = std::string_view{"placeholderStyle"};

    /// The placeholder style @p node authors, or the one this slot defaults to.
    uimodel::CoverArtPlaceholderStyle placeholderStyleOf(uimodel::LayoutNode const& node)
    {
      auto const fallback = uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::Inspector);
      auto const styleId =
        node.propertyOr<std::string>(kPlaceholderStyleProp, std::string{uimodel::coverArtPlaceholderStyleId(fallback)});
      // Validation rejects a document that names a style the catalog does not
      // list, so an unparsed id here means the descriptor default was empty.
      return uimodel::parseCoverArtPlaceholderStyle(styleId).value_or(fallback);
    }

    /**
     * @brief The cover art of the focused view's selection.
     *
     * The artwork and its placeholder are layered inside one Border, which is
     * what rounds and clips both to the same corner radius. How wide that
     * Border is belongs to the frame, but a cover is square and no XAML panel
     * says so, so the component answers its own width with a matching height.
     */
    class TrackCoverArtComponent final : public LayoutComponent
    {
    public:
      TrackCoverArtComponent(LayoutBuildContext& ctx, uimodel::CoverArtPlaceholderStyle const style)
        : _textCatalog{ctx.textCatalog}
        , _coverArt{_image, _placeholder, ctx.resourceBytes, ctx.theme, style}
        , _focusedDetailPtr{ctx.focusedDetailPtr}
      {
        // Uniform rather than UniformToFill: the inspector shows the whole
        // cover, including one that is not square.
        _image.Stretch(Stretch::Uniform);

        auto layers = Grid{};
        layers.Children().Append(_placeholder);
        layers.Children().Append(_image);
        _root.Child(layers);

        _sizeChangedRevoker = _root.SizeChanged(
          winrt::auto_revoke, [this](IInspectable const&, SizeChangedEventArgs const&) { keepSquare(); });

        follow(ctx.asyncRuntime, ctx.workspace);
      }

      FrameworkElement element() const override { return _root; }

    private:
      void follow(async::Runtime& asyncRuntime, rt::WorkspaceService& workspace)
      {
        _projectionPtr = _focusedDetailPtr->projection(workspace);
        _coverArt.bind(asyncRuntime);
        // The projection hands the current snapshot to a new subscriber, so
        // this both installs the follower and draws what is selected now.
        _subscription =
          _projectionPtr->subscribe([this](rt::TrackDetailSnapshot const& snapshot) { applySnapshot(snapshot); });
      }

      /// Answer the width the frame gave with a height that matches it.
      void keepSquare()
      {
        // The assignment raises SizeChanged again, so it has to stop being
        // made once the height already answers the width.
        if (auto const side = _root.ActualWidth(); _root.Height() != side)
        {
          _root.Height(side);
        }
      }

      void applySnapshot(rt::TrackDetailSnapshot const& snapshot)
      {
        auto const album =
          uimodel::formatTrackFieldDisplayText(_textCatalog, rt::TrackField::Album, snapshot, "", false);
        auto const albumArtist =
          uimodel::formatTrackFieldDisplayText(_textCatalog, rt::TrackField::AlbumArtist, snapshot, "", false);
        auto const artist =
          uimodel::formatTrackFieldDisplayText(_textCatalog, rt::TrackField::Artist, snapshot, "", false);
        auto const title =
          uimodel::formatTrackFieldDisplayText(_textCatalog, rt::TrackField::Title, snapshot, "", false);
        auto const candidates = std::array<std::string_view, 4>{album, albumArtist, artist, title};
        _coverArt.select(snapshot.singleCoverArtId,
                         uimodel::makeCoverArtPlaceholderIdentity(candidates),
                         snapshot.selectionKind != rt::SelectionKind::None);
      }

      i18n::MessageCatalog _textCatalog;
      Border _root{};
      Image _image{};
      Grid _placeholder{};
      CoverArtPresenter _coverArt;
      std::shared_ptr<FocusedDetail> _focusedDetailPtr;
      std::shared_ptr<rt::TrackDetailProjection> _projectionPtr;
      FrameworkElement::SizeChanged_revoker _sizeChangedRevoker{};
      // Declared last so it is torn down first: no snapshot may reach a
      // component whose presenter is already gone.
      async::Subscription _subscription;
    };
  } // namespace

  Result<std::unique_ptr<LayoutComponent>> makeTrackCoverArt(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
  {
    return std::make_unique<TrackCoverArtComponent>(ctx, placeholderStyleOf(node));
  }
} // namespace ao::winui::layout
