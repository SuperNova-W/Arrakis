#pragma once

// Point-in-time XLK constituent membership.
//
// Source data: backend/data/metadata/xlk_holdings_history.csv
//   columns: symbol,effective_from,effective_to,weight,source
//
// The `effective_to` column in that file is a constant sentinel (2099-12-31) on
// every row and therefore carries no information. It is parsed for validation
// only and is NEVER used to decide membership. Membership is derived purely
// from the quarterly SEC N-PORT snapshot dates in `effective_from`:
//
//   * All rows sharing an `effective_from` form one snapshot.
//   * Snapshot k governs [date_k, date_(k+1) - 1 day]; the next snapshot fully
//     supersedes it, including for symbols that vanish from the fund.
//   * The final snapshot governs [date_last, +infinity). This is the only
//     forward extrapolation in the resolver, it is unavoidable (there is no
//     later filing in the file), and callers can detect it via
//     `is_extrapolated_forward()` / `last_snapshot_date()`.
//   * Dates before the first snapshot return an EMPTY constituent set. There is
//     deliberately no fallback to current holdings, which would inject
//     survivorship and lookahead bias.

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::news {

struct ConstituentWeight final {
    std::string symbol;
    double weight_percent{};  // percent of fund NAV as filed, e.g. 21.07 for 21.07%
};

struct MembershipSnapshot final {
    std::string effective_from;                   // YYYY-MM-DD, the N-PORT snapshot date
    std::string source;                           // e.g. SEC-NPORT-2023q4 (first row wins)
    std::vector<ConstituentWeight> constituents;  // sorted by symbol, unique
};

// Thrown for malformed input so callers fail loudly instead of silently
// resolving an incomplete universe.
class MembershipDataError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class XlkMembershipResolver final {
  public:
    [[nodiscard]] static XlkMembershipResolver from_csv(const std::filesystem::path& path);
    [[nodiscard]] static XlkMembershipResolver from_csv_text(std::string_view text);

    // Constituents in effect on `date` (YYYY-MM-DD). Empty before the first
    // snapshot. The returned vector is sorted by symbol.
    [[nodiscard]] const std::vector<ConstituentWeight>& constituents_on(std::string_view date) const;

    [[nodiscard]] bool held_on(std::string_view symbol, std::string_view date) const;

    // Conservative PIT lookup for an event assigned to `date`: only a
    // snapshot whose effective/availability date is strictly before `date`
    // may be used. This prevents a quarter-end filing from being applied to
    // news observed before that filing was public.
    [[nodiscard]] bool held_strictly_before(std::string_view symbol, std::string_view date) const;

    // Filed weight (percent of NAV) for `symbol` on `date`, or nullopt if not held.
    [[nodiscard]] std::optional<double> weight_on(std::string_view symbol, std::string_view date) const;

    // `effective_from` of the snapshot governing `date`, or nullopt when `date`
    // precedes the first snapshot.
    [[nodiscard]] std::optional<std::string> governing_snapshot(std::string_view date) const;
    [[nodiscard]] std::optional<std::string> governing_snapshot_strictly_before(
        std::string_view date
    ) const;

    // True when `date` is at or after the last snapshot, i.e. the answer is the
    // newest filing carried forward rather than a filing that covers `date`.
    [[nodiscard]] bool is_extrapolated_forward(std::string_view date) const;

    [[nodiscard]] const std::string& first_snapshot_date() const;
    [[nodiscard]] const std::string& last_snapshot_date() const;
    [[nodiscard]] const std::vector<MembershipSnapshot>& snapshots() const { return snapshots_; }

    // Default location relative to the backend root, overridable with the
    // XLK_HOLDINGS_HISTORY environment variable.
    [[nodiscard]] static std::filesystem::path default_history_path();

  private:
    explicit XlkMembershipResolver(std::vector<MembershipSnapshot> snapshots);

    std::vector<MembershipSnapshot> snapshots_;  // sorted ascending by effective_from
};

// True for tickers that plausibly resolve to a listed common stock at a news
// vendor. The holdings file also contains filing artifacts such as rights,
// when-issued lines and preferred/pending-conversion classes (GENVR, XRXDW,
// HPE-PC, MCHPP, ORCL-PD); those have no company-news feed.
[[nodiscard]] bool is_pollable_ticker(std::string_view symbol);

}  // namespace arrakis::news
