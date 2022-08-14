/**
 * @file table_ref.hpp
 * @author Ashot Vardanian
 * @date 4 Jul 2022
 * @brief C++ bindings for @see "ukv/docs.h".
 *
 * Most field-level operations are still accessible through normal @c `member_refs_t`.
 * This interface mostly helps with tabular and SoA <-> AoS operations involving:
 * > ukv_docs_gist(...)
 * > ukv_docs_gather_scalars(...)
 * > ukv_docs_gather_strings(...)
 */

#pragma once
#include <limits.h>    // `CHAR_BIT`
#include <string_view> // `std::basic_string_view`
#include <vector>      // `std::vector`

#include "ukv/docs.h"
#include "ukv/cpp/ranges.hpp"

namespace unum::ukv {

template <typename scalar_at>
struct cell_gt;

template <typename scalar_at>
class column_view_gt;

template <typename... column_types_at>
class table_view_gt;

template <typename... column_types_at>
class table_layout_gt;

class table_layout_punned_t;

template <typename scalar_at>
ukv_type_t ukv_type() {
    if constexpr (std::is_same_v<scalar_at, bool>)
        return ukv_type_bool_k;
    if constexpr (std::is_same_v<scalar_at, std::int8_t>)
        return ukv_type_i8_k;
    if constexpr (std::is_same_v<scalar_at, std::int16_t>)
        return ukv_type_i16_k;
    if constexpr (std::is_same_v<scalar_at, std::int32_t>)
        return ukv_type_i32_k;
    if constexpr (std::is_same_v<scalar_at, std::int64_t>)
        return ukv_type_i64_k;
    if constexpr (std::is_same_v<scalar_at, std::uint8_t>)
        return ukv_type_u8_k;
    if constexpr (std::is_same_v<scalar_at, std::uint16_t>)
        return ukv_type_u16_k;
    if constexpr (std::is_same_v<scalar_at, std::uint32_t>)
        return ukv_type_u32_k;
    if constexpr (std::is_same_v<scalar_at, std::uint64_t>)
        return ukv_type_u64_k;
    if constexpr (std::is_same_v<scalar_at, float>)
        return ukv_type_f32_k;
    if constexpr (std::is_same_v<scalar_at, double>)
        return ukv_type_f64_k;
    if constexpr (std::is_same_v<scalar_at, value_view_t>)
        return ukv_type_bin_k;
    if constexpr (std::is_same_v<scalar_at, std::string_view>)
        return ukv_type_str_k;
    return ukv_type_any_k;
}

/**
 * @bief The first column of the table, describing its contents.
 */
using table_index_t = std::pair<strided_range_gt<ukv_col_t const>, strided_range_gt<ukv_key_t const>>;

template <typename scalar_at>
struct cell_gt {
    bool valid = false;
    bool converted = false;
    bool collides = false;
    scalar_at value;
};

template <>
struct cell_gt<value_view_t> {
    bool valid = false;
    bool converted = false;
    bool collides = false;
    value_view_t value;
};

template <typename scalar_at>
class column_view_gt {
    ukv_1x8_t* validities_ = nullptr;
    ukv_1x8_t* conversions_ = nullptr;
    ukv_1x8_t* collisions_ = nullptr;
    scalar_at* scalars_ = nullptr;
    ukv_size_t count_ = 0;
    ukv_str_view_t name_ = nullptr;

  public:
    using scalar_t = scalar_at;
    using element_t = cell_gt<scalar_t>;
    using value_type = element_t;

    column_view_gt( //
        ukv_1x8_t* validities,
        ukv_1x8_t* conversions,
        ukv_1x8_t* collisions,
        scalar_t* scalars,
        ukv_size_t count,
        ukv_str_view_t name = nullptr) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), scalars_(scalars), count_(count),
          name_(name) {}

    column_view_gt(column_view_gt&&) = default;
    column_view_gt(column_view_gt const&) = default;
    column_view_gt& operator=(column_view_gt&&) = default;
    column_view_gt& operator=(column_view_gt const&) = default;

    ukv_str_view_t name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }
    element_t operator[](std::size_t i) const noexcept {
        // Bitmaps are indexed from the last bit within every byte
        // https://arrow.apache.org/docs/format/Columnar.html#validity-bitmaps
        ukv_1x8_t mask_bitmap = static_cast<ukv_1x8_t>(1 << (i % CHAR_BIT));
        element_t result;
        result.valid = validities_[i / CHAR_BIT] & mask_bitmap;
        result.converted = conversions_[i / CHAR_BIT] & mask_bitmap;
        result.collides = collisions_[i / CHAR_BIT] & mask_bitmap;
        result.value = scalars_[i];
        return result;
    }
};

template <>
class column_view_gt<value_view_t> {
    ukv_1x8_t* validities_ = nullptr;
    ukv_1x8_t* conversions_ = nullptr;
    ukv_1x8_t* collisions_ = nullptr;
    ukv_val_ptr_t tape_ = nullptr;
    ukv_val_len_t* offsets_ = nullptr;
    ukv_val_len_t* lengths_ = nullptr;
    ukv_size_t count_ = 0;
    ukv_str_view_t name_ = nullptr;

  public:
    using element_t = cell_gt<value_view_t>;
    using value_type = element_t;

    column_view_gt( //
        ukv_1x8_t* validities,
        ukv_1x8_t* conversions,
        ukv_1x8_t* collisions,
        ukv_val_ptr_t tape,
        ukv_val_len_t* offsets,
        ukv_val_len_t* lengths,
        ukv_size_t count,
        ukv_str_view_t name = nullptr) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), tape_(tape), offsets_(offsets),
          lengths_(lengths), count_(count), name_(name) {}

    column_view_gt(column_view_gt&&) = default;
    column_view_gt(column_view_gt const&) = default;
    column_view_gt& operator=(column_view_gt&&) = default;
    column_view_gt& operator=(column_view_gt const&) = default;

    ukv_str_view_t name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }
    element_t operator[](std::size_t i) const noexcept {
        // Bitmaps are indexed from the last bit within every byte
        // https://arrow.apache.org/docs/format/Columnar.html#validity-bitmaps
        ukv_1x8_t mask_bitmap = static_cast<ukv_1x8_t>(1 << (i % CHAR_BIT));
        element_t result;
        result.valid = validities_[i / CHAR_BIT] & mask_bitmap;
        result.converted = conversions_[i / CHAR_BIT] & mask_bitmap;
        result.collides = collisions_[i / CHAR_BIT] & mask_bitmap;
        result.value = {tape_ + offsets_[i], lengths_[i]};
        return result;
    }
};

template <>
class column_view_gt<void> {
    ukv_1x8_t* validities_ = nullptr;
    ukv_1x8_t* conversions_ = nullptr;
    ukv_1x8_t* collisions_ = nullptr;
    ukv_val_ptr_t scalars_ = nullptr;
    ukv_val_ptr_t tape_ = nullptr;
    ukv_val_len_t* offsets_ = nullptr;
    ukv_val_len_t* lengths_ = nullptr;
    ukv_size_t count_ = 0;
    ukv_str_view_t name_ = nullptr;
    ukv_type_t type_ = ukv_type_any_k;

  public:
    column_view_gt( //
        ukv_1x8_t* validities,
        ukv_1x8_t* conversions,
        ukv_1x8_t* collisions,
        ukv_val_ptr_t scalars,
        ukv_val_ptr_t tape,
        ukv_val_len_t* offsets,
        ukv_val_len_t* lengths,
        ukv_size_t count,
        ukv_str_view_t name = nullptr,
        ukv_type_t type = ukv_type_any_k) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), scalars_(scalars), tape_(tape),
          offsets_(offsets), lengths_(lengths), count_(count), name_(name), type_(type) {}

    column_view_gt(column_view_gt&&) = default;
    column_view_gt(column_view_gt const&) = default;
    column_view_gt& operator=(column_view_gt&&) = default;
    column_view_gt& operator=(column_view_gt const&) = default;

    ukv_str_view_t name() const noexcept { return name_; }
    ukv_type_t type() const noexcept { return type_; }
    std::size_t size() const noexcept { return count_; }

    template <typename scalar_at>
    column_view_gt<scalar_at> as() const noexcept {
        if constexpr (std::is_same_v<scalar_at, value_view_t>)
            return {validities_, conversions_, collisions_, tape_, offsets_, lengths_, count_, name_};
        else
            return {validities_, conversions_, collisions_, reinterpret_cast<scalar_at*>(scalars_), count_, name_};
    }
};

template <typename... column_types_at>
class table_view_gt {

    using types_tuple_t = std::tuple<column_types_at...>;
    using row_tuple_t = std::tuple<cell_gt<column_types_at>...>;

    ukv_size_t docs_count_;
    ukv_size_t fields_count_;

    strided_iterator_gt<ukv_col_t const> cols_;
    strided_iterator_gt<ukv_key_t const> keys_;
    strided_iterator_gt<ukv_str_view_t const> fields_;
    strided_iterator_gt<ukv_type_t const> types_;

    ukv_1x8_t** columns_validities_ = nullptr;
    ukv_1x8_t** columns_conversions_ = nullptr;
    ukv_1x8_t** columns_collisions_ = nullptr;
    ukv_val_ptr_t* columns_scalars_ = nullptr;
    ukv_val_len_t** columns_offsets_ = nullptr;
    ukv_val_len_t** columns_lengths_ = nullptr;
    ukv_val_ptr_t tape_ = nullptr;

  public:
    table_view_gt( //
        ukv_size_t docs_count,
        ukv_size_t fields_count,
        strided_iterator_gt<ukv_col_t const> cols,
        strided_iterator_gt<ukv_key_t const> keys,
        strided_iterator_gt<ukv_str_view_t const> fields,
        strided_iterator_gt<ukv_type_t const> types,
        ukv_1x8_t** columns_validities = nullptr,
        ukv_1x8_t** columns_conversions = nullptr,
        ukv_1x8_t** columns_collisions = nullptr,
        ukv_val_ptr_t* columns_scalars = nullptr,
        ukv_val_len_t** columns_offsets = nullptr,
        ukv_val_len_t** columns_lengths = nullptr,
        ukv_val_ptr_t tape = nullptr) noexcept
        : docs_count_(docs_count), fields_count_(fields_count), cols_(cols), keys_(keys), fields_(fields),
          types_(types), columns_validities_(columns_validities), columns_conversions_(columns_conversions),
          columns_collisions_(columns_collisions), columns_scalars_(columns_scalars), columns_offsets_(columns_offsets),
          columns_lengths_(columns_lengths), tape_(tape) {}

    table_view_gt(table_view_gt&&) = default;
    table_view_gt(table_view_gt const&) = default;
    table_view_gt& operator=(table_view_gt&&) = default;
    table_view_gt& operator=(table_view_gt const&) = default;

    table_index_t index() const noexcept { return {{cols_, docs_count_}, {keys_, docs_count_}}; }

    template <typename scalar_at = void>
    column_view_gt<scalar_at> column(std::size_t i) const noexcept {
        using punned_t = column_view_gt<void>;
        punned_t punned {
            columns_validities_[i],
            columns_conversions_[i],
            columns_collisions_[i],
            columns_scalars_[i],
            tape_,
            columns_offsets_[i],
            columns_lengths_[i],
            docs_count_,
            fields_[i],
            types_[i],
        };
        if constexpr (std::is_void_v<scalar_at>)
            return punned;
        else
            return punned.template as<scalar_at>();
    }

    template <std::size_t idx_ak>
    column_view_gt<std::tuple_element_t<idx_ak, types_tuple_t>> column() const noexcept {
        using scalar_t = std::tuple_element_t<idx_ak, types_tuple_t>;
        return this->template column<scalar_t>(idx_ak);
    }

    row_tuple_t row(std::size_t i) const noexcept {
        // TODO:
        return {};
    }

    std::size_t rows() const noexcept { return docs_count_; }
    std::size_t cols() const noexcept { return fields_count_; }

    ukv_1x8_t*** member_validities() noexcept { return &columns_validities_; }
    ukv_1x8_t*** member_conversions() noexcept { return &columns_conversions_; }
    ukv_1x8_t*** member_collisions() noexcept { return &columns_collisions_; }
    ukv_val_ptr_t** member_scalars() noexcept { return &columns_scalars_; }
    ukv_val_len_t*** member_offsets() noexcept { return &columns_offsets_; }
    ukv_val_len_t*** member_lengths() noexcept { return &columns_lengths_; }
    ukv_val_ptr_t* member_tape() noexcept { return &tape_; }
};

using table_view_t = table_view_gt<>;

template <typename scalar_at>
struct field_type_gt {
    ukv_str_view_t field = nullptr;
    ukv_type_t type = ukv_type_any_k;
};

template <>
struct field_type_gt<void> {
    ukv_str_view_t field = nullptr;
    ukv_type_t type = ukv_type_any_k;
};

using field_type_t = field_type_gt<void>;

/**
 * @brief Non-owning combination of index column and header row,
 * defining the order of contents in the table.
 *
 * @tparam column_types_at Optional type markers for columns.
 */
template <typename... column_types_at>
struct table_layout_view_gt {

    ukv_size_t docs_count;
    ukv_size_t fields_count;
    strided_iterator_gt<ukv_col_t const> cols;
    strided_iterator_gt<ukv_key_t const> keys;
    strided_iterator_gt<ukv_str_view_t const> fields;
    strided_iterator_gt<ukv_type_t const> types;
};

using table_layout_view_t = table_layout_view_gt<>;

/**
 * @brief Combination of index column and header row,
 * defining the order of @b statically-typed contents in the table.
 */
template <typename... column_types_at>
class table_layout_gt {
    using columns_info_t = std::tuple<field_type_gt<column_types_at>...>;
    using rows_info_t = std::vector<col_key_t>;

    rows_info_t rows_info_;
    columns_info_t columns_info_;

  public:
    using table_layout_view_t = table_layout_view_gt<column_types_at...>;

    table_layout_gt(std::size_t docs_count = 0) noexcept(false) : rows_info_(docs_count) {}
    table_layout_gt(rows_info_t&& rows, columns_info_t&& columns) noexcept
        : rows_info_(std::move(rows)), columns_info_(columns) {}

    table_layout_gt(table_layout_gt&&) = default;
    table_layout_gt(table_layout_gt const&) = default;
    table_layout_gt& operator=(table_layout_gt&&) = default;
    table_layout_gt& operator=(table_layout_gt const&) = default;

    void clear() noexcept { rows_info_.clear(); }

    template <std::size_t idx_ak>
    std::tuple_element_t<idx_ak, columns_info_t> const& header() const noexcept {
        return std::get<idx_ak>(columns_info_);
    }

    col_key_t& index(std::size_t i) noexcept { return rows_info_[i]; }
    table_index_t index() const noexcept {
        auto rows = strided_range(rows_info_).immutable();
        return {
            rows.members(&col_key_t::col),
            rows.members(&col_key_t::key),
        };
    }

    template <typename scalar_at>
    table_layout_gt<column_types_at..., scalar_at> with(ukv_str_view_t name) && {
        using new_field_t = field_type_gt<scalar_at>;
        using new_column_info_t = std::tuple<new_field_t>;
        new_field_t new_field {name, ukv_type<scalar_at>()};
        new_column_info_t new_column {new_field};
        return {std::move(rows_info_), std::tuple_cat(std::move(columns_info_), std::move(new_column))};
    }

    template <typename row_keys_at>
    table_layout_gt& add_row(row_keys_at&& row_keys) {
        if constexpr (std::is_same_v<row_keys_at, col_key_t>)
            rows_info_.push_back(row_keys);
        else
            rows_info_.push_back(static_cast<ukv_key_t>(row_keys));
        return *this;
    }

    template <typename row_keys_at>
    table_layout_gt& add_rows(row_keys_at&& row_keys) {
        for (auto it = std::begin(row_keys); it != std::end(row_keys); ++it)
            add_row(*it);
        return *this;
    }

    table_layout_gt& for_(std::initializer_list<ukv_key_t> row_keys) {
        clear();
        return add_rows(row_keys);
    }

    template <typename row_keys_at>
    table_layout_gt& for_(row_keys_at&& row_keys) {
        clear();
        if constexpr (is_one<row_keys_at>())
            return add_row(std::forward<row_keys_at>(row_keys));
        else
            return add_rows(std::forward<row_keys_at>(row_keys));
    }

    operator table_layout_view_t() const noexcept { return view(); }
    table_layout_view_t view() const noexcept {
        auto& first = header<0>();
        auto& punned = reinterpret_cast<field_type_t const&>(first);
        constexpr auto cols_count = std::tuple_size_v<columns_info_t>;

        auto rows = strided_range(rows_info_).immutable();
        auto cols = strided_range_gt<field_type_t const>(&punned, &punned + cols_count);
        return {
            static_cast<ukv_size_t>(rows_info_.size()),
            static_cast<ukv_size_t>(cols_count),
            rows.members(&col_key_t::col).begin(),
            rows.members(&col_key_t::key).begin(),
            cols.members(&field_type_t::field).begin(),
            cols.members(&field_type_t::type).begin(),
        };
    }
};

inline table_layout_gt<> table_layout() {
    return {};
}

/**
 * @brief Combination of index column and header row,
 * defining the order of @b dynamically-typed contents in the table.
 */
class table_layout_punned_t {

    std::vector<col_key_t> rows_info_;
    std::vector<field_type_t> columns_info_;

  public:
    table_layout_punned_t(std::size_t docs_count, std::size_t fields_count) noexcept(false)
        : rows_info_(docs_count), columns_info_(fields_count) {}

    void clear() noexcept {
        rows_info_.clear();
        columns_info_.clear();
    }

    field_type_t& header(std::size_t i) noexcept { return columns_info_[i]; }
    col_key_t& index(std::size_t i) noexcept { return rows_info_[i]; }
    table_index_t index() const noexcept {
        auto rows = strided_range(rows_info_).immutable();
        return {
            rows.members(&col_key_t::col),
            rows.members(&col_key_t::key),
        };
    }

    operator table_layout_view_t() const noexcept { return view(); }
    table_layout_view_t view() const noexcept {
        auto rows = strided_range(rows_info_).immutable();
        auto cols = strided_range(columns_info_).immutable();
        return {
            static_cast<ukv_size_t>(rows_info_.size()),
            static_cast<ukv_size_t>(columns_info_.size()),
            rows.members(&col_key_t::col).begin(),
            rows.members(&col_key_t::key).begin(),
            cols.members(&field_type_t::field).begin(),
            cols.members(&field_type_t::type).begin(),
        };
    }
};

/**
 * @brief Purpose-specific handle/reference for an existing collection
 * of documents allowing gathering tabular representations from unstructured docs.
 */
class table_ref_t {
    ukv_t db_ = nullptr;
    ukv_txn_t txn_ = nullptr;
    ukv_col_t col_default_ = ukv_col_main_k;
    ukv_arena_t* arena_ = nullptr;

  public:
    table_ref_t(ukv_t db, ukv_txn_t txn, ukv_col_t col, ukv_arena_t* arena) noexcept
        : db_(db), txn_(txn), col_default_(col), arena_(arena) {}

    table_ref_t(table_ref_t&&) = default;
    table_ref_t& operator=(table_ref_t&&) = default;
    table_ref_t(table_ref_t const&) = default;
    table_ref_t& operator=(table_ref_t const&) = default;

    table_ref_t& on(arena_t& arena) noexcept {
        arena_ = arena.member_ptr();
        return *this;
    }

    /**
     * @brief For N documents and M fields gather (N * M) responses.
     * You put in a `table_layout_view_gt` and you receive a `table_view_gt`.
     * Any column type annotation is optional.
     */
    template <typename... column_types_at>
    expected_gt<table_view_gt<column_types_at...>> gather(
        table_layout_view_gt<column_types_at...> const& layout) noexcept {
        status_t status;

        table_view_gt<column_types_at...> view {
            layout.docs_count,
            layout.fields_count,
            layout.cols,
            layout.keys,
            layout.fields,
            layout.types,
        };

        ukv_docs_gather( // Inputs:
            db_,
            txn_,
            layout.docs_count,
            layout.fields_count,
            layout.cols.get(),
            layout.cols.stride(),
            layout.keys.get(),
            layout.keys.stride(),
            layout.fields.get(),
            layout.fields.stride(),
            layout.types.get(),
            layout.types.stride(),
            ukv_options_default_k,

            // Outputs:
            view.member_validities(),
            view.member_conversions(),
            view.member_collisions(),
            view.member_scalars(),
            view.member_offsets(),
            view.member_lengths(),
            view.member_tape(),

            // Meta
            arena_,
            status.member_ptr());

        return {std::move(status), std::move(view)};
    }

    expected_gt<table_view_t> gather(table_layout_view_t const& layout) noexcept {
        return this->template gather<>(layout);
    }
};

} // namespace unum::ukv
