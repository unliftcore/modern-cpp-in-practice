// ============================================================================
// json.cppm — Minimal JSON serialization with concepts (Ch 6 & 9)
//
// "Use concepts to constrain template interfaces at the boundary."
// "Concepts make requirements explicit — the compiler and the reader
//  see the same contract."
//
// C++23 features used:
//   • C++20 modules (Ch 11)         — named module interface unit
//   • Concepts & requires           — constrain serialization traits
//   • std::format                   — build JSON strings
//   • std::string_view              — non-owning text access
//   • std::ranges / std::views      — process collections
//   • constexpr                     — compile-time checks
//   • [[nodiscard]]                 — prevent dropped results
// ============================================================================
module;

export module webapi.json;

import std;

export namespace webapi::json {

// ── Concept: a type knows how to produce JSON (Ch 6: named constraints) ─────
template <typename T>
concept JsonSerializable = requires(const T& t) {
    { t.to_json() } -> std::convertible_to<std::string>;
};

// ── Concept: a type can be constructed from a JSON string ───────────────────
template <typename T>
concept JsonDeserializable = requires(std::string_view sv) {
    { T::from_json(sv) } -> std::same_as<std::optional<T>>;
};

// ── Serialize a range of JsonSerializable items (Ch 7: Ranges) ──────────────
// "Ranges express intent: filter, transform, collect."
template <std::ranges::input_range R>
    requires JsonSerializable<std::ranges::range_value_t<R>>
[[nodiscard]] std::string serialize_array(R&& range) {
    std::string result = "[";
    bool first = true;
    for (const auto& item : range) {
        if (!first) result += ',';
        result += item.to_json();
        first = false;
    }
    result += ']';
    return result;
}

// ── Tiny JSON string-value extractor (production code would use a library) ──
// This is intentionally minimal — the focus is on C++23 patterns,
// not on building a JSON parser.
[[nodiscard]] inline std::optional<std::string>
extract_string_field(std::string_view json, std::string_view key) {
    auto pattern = std::format("\"{}\"", key);
    auto pos = json.find(pattern);
    if (pos == std::string_view::npos) return std::nullopt;

    pos = json.find(':', pos + pattern.size());
    if (pos == std::string_view::npos) return std::nullopt;

    // skip whitespace and opening quote
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;  // past opening quote

    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return std::nullopt;

    return std::string(json.substr(pos, end - pos));
}

[[nodiscard]] inline std::optional<bool>
extract_bool_field(std::string_view json, std::string_view key) {
    auto pattern = std::format("\"{}\"", key);
    auto pos = json.find(pattern);
    if (pos == std::string_view::npos) return std::nullopt;

    pos = json.find(':', pos + pattern.size());
    if (pos == std::string_view::npos) return std::nullopt;

    // skip whitespace after colon
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;

    if (json.substr(pos, 4) == "true") return true;
    if (json.substr(pos, 5) == "false") return false;
    return std::nullopt;
}

}  // namespace webapi::json
