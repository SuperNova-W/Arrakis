# Design QA: sector ETF chart viewer

Source visual truth: `/var/folders/ll/b5xp2_894ml48vw4462xhv3h0000gn/T/codex-clipboard-9db43298-891b-4121-9d1c-d5c673579e54.png`

Implementation evidence: `frontend/design-qa-implementation.png`

Combined comparison evidence: `/private/tmp/arrakis-chart-comparison.png`

## Capture normalization

- Source image: 1348 × 812 pixels.
- Browser implementation: 1280 × 1135 pixels at the browser's default 1280-pixel CSS viewport and 1× capture density.
- Compared region: the implementation chart card was cropped to 1002 × 653 pixels and both chart regions were normalized to 650 pixels high in the combined comparison.
- State: XLB, light theme, completed persisted one-minute bars, 1D range selected.
- Primary interactions tested: initial REST load, 1D selected state, 5D range selection, five-minute data refresh, chart navigator rendering.
- Browser console: no errors after initial load or range selection.

## Full-view comparison

The implementation preserves the reference's white bordered card, teal price line and translucent area, dashed horizontal grid, large plot, compact segmented controls, restrained blue-gray typography, and clear price hierarchy. The full page adds the existing Arrakis navigation and research context without changing the chart's visual language.

## Focused chart-region comparison

The side-by-side normalized chart comparison confirms comparable line weight, plot density, whitespace, card treatment, control proportions, and axis legibility. Intentional product additions are the current quote/change row, OHLCV statistics, volume overlay, open baseline, right-aligned currency axis, and draggable navigator. These additions support the request for a more detailed viewer and do not obscure the primary price series.

## Required fidelity surfaces

- Fonts and typography: existing Arrakis sans-serif family retained; heading, quote, axes, metadata, and control weights have clear Yahoo-style hierarchy.
- Spacing and layout rhythm: chart card has generous padding and a large plot; controls and summary statistics align to consistent tracks.
- Colors and visual tokens: source-inspired white, pale blue-gray, dashed grid, and muted teal palette matched within the existing application tokens.
- Image quality and asset fidelity: the reference contains no raster assets or custom icons; the chart is rendered natively at browser resolution.
- Copy and content: labels describe real ETF OHLCV data, selected resolution/range, fallback source, and research limitations.

## Findings

- No actionable P0, P1, or P2 visual differences remain.
- P3: the implementation uses a right-side currency axis while the supplied reference places labels on the left. This is intentional to keep the main price trace unobstructed and align with finance-chart conventions.

## Comparison history

- Initial comparison: no P0/P1/P2 findings. No blocking visual fixes were required.

## Implementation checklist

- [x] REST-first historical range retrieval.
- [x] Chronological bar normalization.
- [x] Price/change and OHLCV summaries.
- [x] Detailed hover tooltip.
- [x] Price, volume, baseline, and navigator layers.
- [x] Responsive range controls.
- [x] Build, lint, interaction, and console verification.

final result: passed
