// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/anchor_element_metrics.h"

#include "base/metrics/histogram_macros.h"
#include "third_party/blink/public/mojom/loader/navigation_predictor.mojom-blink.h"
#include "third_party/blink/renderer/core/dom/flat_tree_traversal.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/anchor_element_metrics_sender.h"
#include "third_party/blink/renderer/core/html/html_anchor_element.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/platform/histogram.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

// If RecordAnchorMetricsClicked feature is enabled, then metrics of anchor
// elements clicked by the user will be extracted and recorded.
const base::Feature kRecordAnchorMetricsClicked{
    "RecordAnchorMetricsClicked", base::FEATURE_DISABLED_BY_DEFAULT};

namespace {

// Helper function that returns the root document the anchor element is in.
Document* GetRootDocument(const HTMLAnchorElement& anchor) {
  return anchor.GetDocument().GetFrame()->LocalFrameRoot().GetDocument();
}

// Accumulated scroll offset of all frames up to the local root frame.
int AccumulatedScrollOffset(const HTMLAnchorElement& anchor_element) {
  IntSize offset;
  Frame* frame = anchor_element.GetDocument().GetFrame();
  while (frame && frame->View() && frame->IsLocalFrame()) {
    offset += ToLocalFrame(frame)->View()->LayoutViewport()->ScrollOffsetInt();
    frame = frame->Tree().Parent();
  }
  return offset.Height();
}

// Whether the element is inside an iframe.
bool IsInIFrame(const HTMLAnchorElement& anchor_element) {
  Frame* frame = anchor_element.GetDocument().GetFrame();
  while (frame && frame->IsLocalFrame()) {
    HTMLFrameOwnerElement* owner =
        ToLocalFrame(frame)->GetDocument()->LocalOwner();
    if (owner && IsHTMLIFrameElement(owner))
      return true;
    frame = frame->Tree().Parent();
  }
  return false;
}

// Whether the anchor element contains an image element.
bool ContainsImage(const HTMLAnchorElement& anchor_element) {
  for (Node* node = FlatTreeTraversal::FirstChild(anchor_element); node;
       node = FlatTreeTraversal::Next(*node, &anchor_element)) {
    if (IsHTMLImageElement(*node))
      return true;
  }
  return false;
}

// Whether the link target has the same host as the root document.
bool IsSameHost(const HTMLAnchorElement& anchor_element) {
  String source_host = GetRootDocument(anchor_element)->Url().Host();
  String target_host = anchor_element.Href().Host();
  return source_host == target_host;
}

// Returns true if the two strings only differ by one number, and
// the second number equals the first number plus one. Examples:
// example.com/page9/cat5, example.com/page10/cat5 => true
// example.com/page9/cat5, example.com/page10/cat10 => false
bool IsStringIncrementedByOne(const String& source, const String& target) {
  // Consecutive numbers should differ in length by at most 1.
  int length_diff = target.length() - source.length();
  if (length_diff < 0 || length_diff > 1)
    return false;

  // The starting position of difference.
  unsigned int left = 0;
  while (left < source.length() && left < target.length() &&
         source[left] == target[left]) {
    left++;
  }

  // There is no difference, or the difference is not a digit.
  if (left == source.length() || left == target.length() ||
      !u_isdigit(source[left]) || !u_isdigit(target[left])) {
    return false;
  }

  // Expand towards right to extract the numbers.
  unsigned int source_right = left + 1;
  while (source_right < source.length() && u_isdigit(source[source_right]))
    source_right++;

  unsigned int target_right = left + 1;
  while (target_right < target.length() && u_isdigit(target[target_right]))
    target_right++;

  int source_number = source.Substring(left, source_right - left).ToInt();
  int target_number = target.Substring(left, target_right - left).ToInt();

  // The second number should increment by one and the rest of the strings
  // should be the same.
  return source_number + 1 == target_number &&
         source.Substring(source_right) == target.Substring(target_right);
}

// Extract source and target link url, and return IsStringIncrementedByOne().
bool IsUrlIncrementedByOne(const HTMLAnchorElement& anchor_element) {
  if (!IsSameHost(anchor_element))
    return false;

  String source_url = GetRootDocument(anchor_element)->Url().GetString();
  String target_url = anchor_element.Href().GetString();
  return IsStringIncrementedByOne(source_url, target_url);
}

// Returns the bounding box rect of a layout object, including visual
// overflows.
IntRect AbsoluteElementBoundingBoxRect(const LayoutObject* layout_object) {
  Vector<LayoutRect> rects;
  layout_object->AddElementVisualOverflowRects(rects, LayoutPoint());

  return layout_object
      ->LocalToAbsoluteQuad(FloatQuad(FloatRect(UnionRect(rects))))
      .EnclosingBoundingBox();
}

}  // anonymous namespace

// static
base::Optional<AnchorElementMetrics> AnchorElementMetrics::Create(
    const HTMLAnchorElement* anchor_element) {
  LocalFrame* local_frame = anchor_element->GetDocument().GetFrame();
  LayoutObject* layout_object = anchor_element->GetLayoutObject();
  if (!local_frame || !layout_object)
    return base::nullopt;

  LocalFrameView* local_frame_view = local_frame->View();
  LocalFrameView* root_frame_view = local_frame->LocalFrameRoot().View();
  if (!local_frame_view || !root_frame_view)
    return base::nullopt;

  IntRect viewport = root_frame_view->LayoutViewport()->VisibleContentRect();
  if (viewport.Size().IsEmpty())
    return base::nullopt;

  // Use the viewport size to normalize anchor element metrics.
  float base_height = static_cast<float>(viewport.Height());
  float base_width = static_cast<float>(viewport.Width());

  // The anchor element rect in the root frame.
  IntRect target = local_frame_view->ConvertToRootFrame(
      AbsoluteElementBoundingBoxRect(layout_object));

  // Limit the element size to the viewport size.
  float ratio_area = std::min(1.0f, target.Height() / base_height) *
                     std::min(1.0f, target.Width() / base_width);
  float ratio_distance_top_to_visible_top = target.Y() / base_height;
  float ratio_distance_center_to_visible_top =
      (target.Y() + target.Height() / 2.0) / base_height;

  float ratio_distance_root_top =
      (target.Y() + AccumulatedScrollOffset(*anchor_element)) / base_height;

  // Distance to the bottom is tricky if the element is inside sub/iframes.
  // Here we use the target location in the root viewport, and calculate
  // the distance from the bottom of the anchor element to the root bottom.
  int root_height = GetRootDocument(*anchor_element)
                        ->GetLayoutView()
                        ->GetScrollableArea()
                        ->ContentsSize()
                        .Height();
  int root_scrolled =
      root_frame_view->LayoutViewport()->ScrollOffsetInt().Height();
  float ratio_distance_root_bottom =
      (root_height - root_scrolled - target.Y() - target.Height()) /
      base_height;

  // Get the anchor element rect that intersects with the viewport.
  IntRect target_visible(target);
  target_visible.Intersect(IntRect(IntPoint(), viewport.Size()));

  // It guarantees to be less or equal to 1.
  float ratio_visible_area = (target_visible.Height() / base_height) *
                             (target_visible.Width() / base_width);

  return AnchorElementMetrics(
      anchor_element, ratio_area, ratio_visible_area,
      ratio_distance_top_to_visible_top, ratio_distance_center_to_visible_top,
      ratio_distance_root_top, ratio_distance_root_bottom,
      IsInIFrame(*anchor_element), ContainsImage(*anchor_element),
      IsSameHost(*anchor_element), IsUrlIncrementedByOne(*anchor_element));
}

// static
base::Optional<AnchorElementMetrics>
AnchorElementMetrics::MaybeExtractMetricsClicked(
    const HTMLAnchorElement* anchor_element) {
  if (!base::FeatureList::IsEnabled(kRecordAnchorMetricsClicked) ||
      !anchor_element->Href().ProtocolIsInHTTPFamily())
    return base::nullopt;

  auto anchor_metrics = Create(anchor_element);
  if (anchor_metrics.has_value()) {
    anchor_metrics.value().RecordMetrics();
    anchor_metrics.value().SendMetricsToBrowser();
  }

  return anchor_metrics;
}

void AnchorElementMetrics::SendMetricsToBrowser() const {
  DCHECK(anchor_element_->GetDocument().GetFrame());

  auto metrics = mojom::blink::AnchorElementMetrics::New();
  metrics->ratio_area = ratio_area_;
  metrics->ratio_distance_root_top = ratio_distance_root_top_;
  metrics->ratio_distance_center_to_visible_top =
      ratio_distance_center_to_visible_top_;
  metrics->target_url = anchor_element_->Href();

  Document* root_document =
      anchor_element_->GetDocument().GetFrame()->LocalFrameRoot().GetDocument();
  AnchorElementMetricsSender::From(*root_document)
      ->SendClickedAnchorMetricsToBrowser(std::move(metrics));
}

void AnchorElementMetrics::RecordMetrics() const {
  UMA_HISTOGRAM_PERCENTAGE("AnchorElementMetrics.Clicked.RatioArea",
                           int(ratio_area_ * 100));

  UMA_HISTOGRAM_PERCENTAGE("AnchorElementMetrics.Clicked.RatioVisibleArea",
                           int(ratio_visible_area_ * 100));

  UMA_HISTOGRAM_PERCENTAGE(
      "AnchorElementMetrics.Clicked.RatioDistanceTopToVisibleTop",
      int(std::min(ratio_distance_top_to_visible_top_, 1.0f) * 100));

  UMA_HISTOGRAM_PERCENTAGE(
      "AnchorElementMetrics.Clicked.RatioDistanceCenterToVisibleTop",
      int(std::min(ratio_distance_center_to_visible_top_, 1.0f) * 100));

  UMA_HISTOGRAM_COUNTS_10000(
      "AnchorElementMetrics.Clicked.RatioDistanceRootTop",
      int(std::min(ratio_distance_root_top_, 100.0f) * 100));

  UMA_HISTOGRAM_COUNTS_10000(
      "AnchorElementMetrics.Clicked.RatioDistanceRootBottom",
      int(std::min(ratio_distance_root_bottom_, 100.0f) * 100));

  UMA_HISTOGRAM_BOOLEAN("AnchorElementMetrics.Clicked.IsInIFrame",
                        is_in_iframe_);

  UMA_HISTOGRAM_BOOLEAN("AnchorElementMetrics.Clicked.ContainsImage",
                        contains_image_);

  UMA_HISTOGRAM_BOOLEAN("AnchorElementMetrics.Clicked.IsSameHost",
                        is_same_host_);

  UMA_HISTOGRAM_BOOLEAN("AnchorElementMetrics.Clicked.IsUrlIncrementedByOne",
                        is_url_incremented_by_one_);
}

}  // namespace blink