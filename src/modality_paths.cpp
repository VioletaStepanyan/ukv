/**
 * @file modality_paths.cpp
 * @author Ashot Vardanian
 *
 * @brief Paths (variable length keys) compatibility layer.
 * Sits on top of any @see "ukv.h"-compatible system.
 *
 * For every string key hash we store:
 * * N: number of entries (1 if no collisions appeared)
 * * N key offsets
 * * N value lengths
 * * N concatenated keys
 * * N concatenated values
 *
 * @section Mirror "Directory" Entries for Nested Paths
 *
 * Furthermore, we need to store mirror entries, that will
 * store the directory tree. In other words, for an input
 * like @b home/user/media/name we would keep:
 * > home/: @b home/user
 * > home/user/: @b home/user/media
 * > home/user/media/: @b home/user/media/name
 *
 * The mirror "directory" entries can have negative IDs.
 * Their values would be structured differently:
 * > N: number of direct children
 * >
 */

#include "ukv/paths.h"
#include "helpers/pmr.hpp"         // `stl_arena_t`
#include "helpers/algorithm.hpp"   // `sort_and_deduplicate`
#include "helpers/vector.hpp"      // `safe_vector_gt`
#include "ukv/cpp/ranges_args.hpp" // `places_arg_t`

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

struct hash_t {
    ukv_key_t operator()(std::string_view key_str) const noexcept {
        using stl_t = std::hash<std::string_view>;
        auto result = stl_t {}(key_str);
#ifdef UKV_DEBUG
        result %= 10ul;
#endif
        return static_cast<ukv_key_t>(result);
    }
};

constexpr std::size_t counter_size_k = sizeof(ukv_length_t);
constexpr std::size_t bytes_in_header_k = counter_size_k;

ukv_length_t get_bucket_size(value_view_t bucket) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    return bucket.size() > bytes_in_header_k ? *lengths : 0u;
}

indexed_range_gt<ukv_length_t const*> get_bucket_counters(value_view_t bucket, ukv_length_t size) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    return {lengths, lengths + size * 2u + 1u};
}

consecutive_strs_iterator_t get_bucket_keys(value_view_t bucket, ukv_length_t size) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    auto bytes_for_counters = size * 2u * counter_size_k;
    return {lengths + 1u, bucket.data() + bytes_in_header_k + bytes_for_counters};
}

consecutive_bins_iterator_t get_bucket_vals(value_view_t bucket, ukv_length_t size) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    auto bytes_for_counters = size * 2u * counter_size_k;
    auto bytes_for_keys = std::accumulate(lengths + 1u, lengths + 1u + size, 0ul);
    return {lengths + 1u + size, bucket.data() + bytes_in_header_k + bytes_for_counters + bytes_for_keys};
}

struct bucket_member_t {
    std::size_t idx = 0;
    std::string_view key;
    value_view_t value;

    operator bool() const noexcept { return value; }
};

template <typename bucket_member_callback_at>
void for_each_in_bucket(value_view_t bucket, bucket_member_callback_at member_callback) noexcept {
    auto bucket_size = get_bucket_size(bucket);
    if (!bucket_size)
        return;
    auto bucket_keys = get_bucket_keys(bucket, bucket_size);
    auto bucket_vals = get_bucket_vals(bucket, bucket_size);
    for (std::size_t i = 0; i != bucket_size; ++i, ++bucket_keys, ++bucket_vals)
        member_callback(bucket_member_t {i, *bucket_keys, *bucket_vals});
}

bucket_member_t find_in_bucket(value_view_t bucket, std::string_view key_str) noexcept {
    bucket_member_t result;
    for_each_in_bucket(bucket, [&](bucket_member_t const& member) {
        if (member.key == key_str)
            result = member;
    });
    return result;
}

bool starts_with(std::string_view str, std::string_view prefix) noexcept {
    return str.size() >= prefix.size() && str.substr(0, prefix.size()) == prefix;
}

std::size_t path_segments_counts(std::string_view key_str, ukv_char_t const c_separator) noexcept {
}

template <typename keys_callback_at>
void path_segments_enumerate(std::string_view key_str) noexcept {
}

/**
 * @brief Removes part of variable length string.
 * @return The shortened view of the input. Will start from same address.
 */
value_view_t remove_part(value_view_t full, value_view_t part) noexcept {
    auto removed_length = part.size();
    auto moved_length = full.size() - part.size();
    std::memmove((void*)part.begin(), (void*)part.end(), moved_length);
    return {full.begin(), full.size() - removed_length};
}

void remove_from_bucket(value_view_t& bucket, std::string_view key_str) noexcept {
    // If the entry was present, it must be clamped.
    // Matching key and length entry will be removed.
    auto [old_idx, old_key, old_val] = find_in_bucket(bucket, key_str);
    if (!old_val)
        return;

    // Most of the time slots contain just one entry
    auto old_size = get_bucket_size(bucket);
    if (old_size == 1) {
        bucket = {};
        return;
    }

    bucket = remove_part(bucket, old_val);
    bucket = remove_part(bucket, old_key);

    // Remove the value counter
    auto begin = bucket.data();
    value_view_t value_length_bytes {begin + counter_size_k * (old_size + old_idx + 1u), counter_size_k};
    bucket = remove_part(bucket, value_length_bytes);
    value_view_t key_length_bytes {begin + counter_size_k * (old_idx + 1u), counter_size_k};
    bucket = remove_part(bucket, key_length_bytes);

    // Decrement the size
    auto lengths = (ukv_length_t*)begin;
    lengths[0] -= 1u;
}

void upsert_in_bucket( //
    value_view_t& bucket,
    std::string_view key,
    value_view_t val,
    stl_arena_t& arena,
    ukv_error_t* c_error) noexcept {

    auto old_size = get_bucket_size(bucket);
    auto old_bytes_for_counters = old_size * 2u * counter_size_k;
    auto old_lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    auto old_bytes_for_keys = bucket ? std::accumulate(old_lengths + 1u, old_lengths + 1u + old_size, 0ul) : 0ul;
    auto old_bytes_for_vals =
        bucket ? std::accumulate(old_lengths + 1u + old_size, old_lengths + 1u + old_size * 2ul, 0ul) : 0ul;
    auto [old_idx, old_key, old_val] = find_in_bucket(bucket, key);
    bool is_missing = !old_val;

    auto new_size = old_size + is_missing;
    auto new_bytes_for_counters = new_size * 2u * counter_size_k;
    auto new_bytes_for_keys = old_bytes_for_keys - old_key.size() + key.size();
    auto new_bytes_for_vals = old_bytes_for_vals - old_val.size() + val.size();
    auto new_bytes = bytes_in_header_k + new_bytes_for_counters + new_bytes_for_keys + new_bytes_for_vals;

    auto new_begin = arena.alloc<byte_t>(new_bytes, c_error).begin();
    return_on_error(c_error);
    auto new_lengths = reinterpret_cast<ukv_length_t*>(new_begin);
    new_lengths[0] = new_size;
    auto new_keys_lengths = new_lengths + 1ul;
    auto new_vals_lengths = new_lengths + 1ul + new_size;
    auto new_keys_output = new_begin + bytes_in_header_k + new_bytes_for_counters;
    auto new_vals_output = new_begin + bytes_in_header_k + new_bytes_for_counters + new_bytes_for_keys;

    auto old_keys = get_bucket_keys(bucket, old_size);
    auto old_vals = get_bucket_vals(bucket, old_size);
    for (std::size_t i = 0; i != old_size; ++i, ++old_keys, ++old_vals) {
        if (!is_missing && i == old_idx)
            continue;

        value_view_t old_key = *old_keys;
        value_view_t old_val = *old_vals;
        new_keys_lengths[i] = static_cast<ukv_length_t>(old_key.size());
        new_vals_lengths[i] = static_cast<ukv_length_t>(old_val.size());
        std::memcpy(new_keys_output, old_key.data(), old_key.size());
        std::memcpy(new_vals_output, old_val.data(), old_val.size());

        new_keys_output += old_key.size();
        new_vals_output += old_val.size();
    }

    // Append the new entry at the end
    new_keys_lengths[new_size - 1] = static_cast<ukv_length_t>(key.size());
    new_vals_lengths[new_size - 1] = static_cast<ukv_length_t>(val.size());
    std::memcpy(new_keys_output, key.data(), key.size());
    std::memcpy(new_vals_output, val.data(), val.size());

    bucket = {new_begin, new_bytes};
}

void ukv_paths_write( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    ukv_size_t const c_tasks_count,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_length_t const* c_paths_offsets,
    ukv_size_t const c_paths_offsets_stride,

    ukv_length_t const* c_paths_lengths,
    ukv_size_t const c_paths_lengths_stride,

    ukv_str_view_t const* c_paths,
    ukv_size_t const c_paths_stride,

    ukv_octet_t const* c_values_presences,

    ukv_length_t const* c_values_offsets,
    ukv_size_t const c_values_offsets_stride,

    ukv_length_t const* c_values_lengths,
    ukv_size_t const c_values_lengths_stride,

    ukv_bytes_cptr_t const* c_values_bytes,
    ukv_size_t const c_values_bytes_stride,

    ukv_options_t const c_options,
    ukv_char_t const c_separator,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    stl_arena_t arena = prepare_arena(c_arena, c_options, c_error);
    return_on_error(c_error);

    contents_arg_t keys_str_args;
    keys_str_args.offsets_begin = {c_paths_offsets, c_paths_offsets_stride};
    keys_str_args.lengths_begin = {c_paths_lengths, c_paths_lengths_stride};
    keys_str_args.contents_begin = {(ukv_bytes_cptr_t const*)c_paths, c_paths_stride};
    keys_str_args.count = c_tasks_count;

    auto unique_col_keys = arena.alloc<collection_key_t>(c_tasks_count, c_error);
    return_on_error(c_error);

    // Parse and hash input string unique_col_keys
    hash_t hash;
    strided_iterator_gt<ukv_collection_t const> collections {c_collections, c_collections_stride};
    for (std::size_t i = 0; i != c_tasks_count; ++i)
        unique_col_keys[i] = {collections ? collections[i] : ukv_collection_main_k, hash(keys_str_args[i])};

    // We must sort and deduplicate this bucket IDs
    unique_col_keys = {unique_col_keys.begin(), sort_and_deduplicate(unique_col_keys.begin(), unique_col_keys.end())};

    // Read from disk
    // We don't need:
    // > presences: zero length buckets are impossible here.
    // > lengths: value lengths are always smaller than buckets.
    // We can infer those and export differently.
    ukv_arena_t buckets_arena = &arena;
    ukv_length_t* buckets_offsets = nullptr;
    ukv_byte_t* buckets_values = nullptr;
    places_arg_t unique_places;
    auto unique_col_keys_strided = strided_range(unique_col_keys.begin(), unique_col_keys.end()).immutable();
    unique_places.collections_begin = unique_col_keys_strided.members(&collection_key_t::collection).begin();
    unique_places.keys_begin = unique_col_keys_strided.members(&collection_key_t::key).begin();
    unique_places.fields_begin = {};
    unique_places.count = static_cast<ukv_size_t>(unique_col_keys.size());
    ukv_read( //
        c_db,
        c_txn,
        unique_places.count,
        unique_places.collections_begin.get(),
        unique_places.collections_begin.stride(),
        unique_places.keys_begin.get(),
        unique_places.keys_begin.stride(),
        c_options,
        nullptr,
        &buckets_offsets,
        nullptr,
        &buckets_values,
        &buckets_arena,
        c_error);
    return_on_error(c_error);

    joined_bins_t joined_buckets {unique_places.count, buckets_offsets, buckets_values};
    safe_vector_gt<value_view_t> updated_buckets(unique_places.count, arena, c_error);
    return_on_error(c_error);
    transform_n(joined_buckets.begin(), unique_places.count, updated_buckets.begin());

    strided_iterator_gt<ukv_octet_t const> presences {c_values_presences, sizeof(ukv_octet_t)};
    strided_iterator_gt<ukv_length_t const> offs {c_values_offsets, c_values_offsets_stride};
    strided_iterator_gt<ukv_length_t const> lens {c_values_lengths, c_values_lengths_stride};
    strided_iterator_gt<ukv_bytes_cptr_t const> vals {c_values_bytes, c_values_bytes_stride};
    contents_arg_t contents {presences, offs, lens, vals, c_tasks_count};

    // Update every unique bucket
    for (std::size_t i = 0; i != c_tasks_count; ++i) {
        std::string_view key_str = keys_str_args[i];
        ukv_key_t key = hash(key_str);
        value_view_t new_val = contents[i];
        collection_key_t collection_key {collections ? collections[i] : ukv_collection_main_k, key};
        auto bucket_idx = offset_in_sorted(unique_col_keys, collection_key);
        value_view_t& bucket = updated_buckets[bucket_idx];

        if (new_val) {
            upsert_in_bucket(bucket, key_str, new_val, arena, c_error);
            return_on_error(c_error);
        }
        else
            remove_from_bucket(bucket, key_str);
    }

    // Once all is updated, we can safely write back
    ukv_write( //
        c_db,
        c_txn,
        unique_places.count,
        unique_places.collections_begin.get(),
        unique_places.collections_begin.stride(),
        unique_places.keys_begin.get(),
        unique_places.keys_begin.stride(),
        nullptr,
        nullptr,
        0,
        updated_buckets[0].member_length(),
        sizeof(value_view_t),
        updated_buckets[0].member_ptr(),
        sizeof(value_view_t),
        c_options,
        &buckets_arena,
        c_error);
}

void ukv_paths_read( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    ukv_size_t const c_tasks_count,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_length_t const* c_paths_offsets,
    ukv_size_t const c_paths_offsets_stride,

    ukv_length_t const* c_paths_lengths,
    ukv_size_t const c_paths_lengths_stride,

    ukv_str_view_t const* c_paths,
    ukv_size_t const c_paths_stride,

    ukv_options_t const c_options,
    ukv_char_t const,

    ukv_octet_t** c_presences,
    ukv_length_t** c_offsets,
    ukv_length_t** c_lengths,
    ukv_byte_t** c_values,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    stl_arena_t arena = prepare_arena(c_arena, c_options, c_error);
    return_on_error(c_error);

    contents_arg_t keys_str_args;
    keys_str_args.offsets_begin = {c_paths_offsets, c_paths_offsets_stride};
    keys_str_args.lengths_begin = {c_paths_lengths, c_paths_lengths_stride};
    keys_str_args.contents_begin = {(ukv_bytes_cptr_t const*)c_paths, c_paths_stride};
    keys_str_args.count = c_tasks_count;

    // Getting hash-collisions is such a rare case, that we will not
    // optimize for it in the current implementation. Sorting and
    // deduplicating the IDs will cost more overall, than a repeated
    // read every once in a while.
    auto buckets_keys = arena.alloc<ukv_key_t>(c_tasks_count, c_error);
    return_on_error(c_error);

    // Parse and hash input string buckets_keys
    hash_t hash;
    for (std::size_t i = 0; i != c_tasks_count; ++i)
        buckets_keys[i] = hash(keys_str_args[i]);

    // Read from disk
    // We don't need:
    // > presences: zero length buckets are impossible here.
    // > lengths: value lengths are always smaller than buckets.
    // We can infer those and export differently.
    ukv_arena_t buckets_arena = &arena;
    ukv_length_t* buckets_offsets = nullptr;
    ukv_byte_t* buckets_values = nullptr;
    ukv_read( //
        c_db,
        c_txn,
        c_tasks_count,
        c_collections,
        c_collections_stride,
        buckets_keys.begin(),
        sizeof(ukv_key_t),
        c_options,
        nullptr,
        &buckets_offsets,
        nullptr,
        &buckets_values,
        &buckets_arena,
        c_error);
    return_on_error(c_error);

    // Some of the entries will contain more then one key-value pair in case of collisions.
    ukv_length_t exported_volume = 0;
    joined_bins_t buckets {c_tasks_count, buckets_offsets, buckets_values};
    auto presences = arena.alloc_or_dummy<ukv_octet_t>(divide_round_up<std::size_t>(c_tasks_count, bits_in_byte_k),
                                                       c_error,
                                                       c_presences);
    auto lengths = arena.alloc_or_dummy<ukv_length_t>(c_tasks_count, c_error, c_lengths);
    auto offsets = arena.alloc_or_dummy<ukv_length_t>(c_tasks_count, c_error, c_offsets);

    for (std::size_t i = 0; i != c_tasks_count; ++i) {
        std::string_view key_str = keys_str_args[i];
        value_view_t bucket = buckets[i];

        // Now that we have found our match - clamp everything else.
        value_view_t val = find_in_bucket(bucket, key_str).value;
        if (val) {
            presences[i] = true;
            offsets[i] = exported_volume;
            lengths[i] = static_cast<ukv_length_t>(val.size());
            if (c_values)
                std::memmove(buckets_values + exported_volume, val.data(), val.size());
            exported_volume += static_cast<ukv_length_t>(val.size());
        }
        else {
            presences[i] = false;
            offsets[i] = exported_volume;
            lengths[i] = ukv_length_missing_k;
        }
    }

    offsets[c_tasks_count] = exported_volume;
    if (c_values)
        *c_values = buckets_values;
}

/**
 * > Same collection
 * > One scan request
 * > Has previous results
 */
void scan_one_collection_one_range( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    ukv_collection_t collection,
    std::string_view prefix,
    std::string_view previous,
    ukv_length_t max_count,
    ukv_options_t const c_options,
    ukv_length_t& count,
    growing_tape_t& paths,
    stl_arena_t& arena,
    ukv_error_t* c_error) {

    hash_t hash;
    ukv_length_t found_paths = 0;
    ukv_arena_t c_arena = &arena;

    bool has_reached_previous = previous.empty();
    while (found_paths < max_count) {
        ukv_key_t previous_key = previous.empty() ? hash(previous) : ukv_key_unknown_k;
        ukv_length_t* found_buckets_count = nullptr;
        ukv_key_t* found_buckets_keys = nullptr;
        ukv_scan( //
            c_db,
            c_txn,
            1,
            &collection,
            0,
            &previous_key,
            0,
            nullptr,
            0,
            &max_count,
            0,
            c_options,
            nullptr,
            &found_buckets_count,
            &found_buckets_keys,
            &c_arena,
            c_error);
        return_on_error(c_error);

        if (found_buckets_count[0] <= 1)
            // We have reached the end of collection
            break;

        ukv_length_t* found_buckets_offsets = nullptr;
        ukv_byte_t* found_buckets_data = nullptr;
        ukv_read( //
            c_db,
            c_txn,
            found_buckets_count[0],
            &collection,
            0,
            found_buckets_keys,
            sizeof(ukv_key_t),
            ukv_options_t(c_options | ukv_option_dont_discard_memory_k),
            nullptr,
            &found_buckets_offsets,
            nullptr,
            &found_buckets_data,
            &c_arena,
            c_error);

        joined_bins_iterator_t found_buckets {found_buckets_offsets, found_buckets_data};
        for (std::size_t i = 0; i != found_buckets_count[0]; ++i, ++found_buckets) {
            value_view_t bucket = *found_buckets;
            for_each_in_bucket(bucket, [&](bucket_member_t const& member) {
                if (!starts_with(member.key, prefix))
                    // Skip irrelevant entries
                    return;
                if (member.key == previous) {
                    // We may have reached the boundary between old results and new ones
                    has_reached_previous = true;
                    return;
                }
                if (!has_reached_previous)
                    // Skip the results we have already seen
                    return;
                if (found_paths >= max_count)
                    // We have more than we need
                    return;

                // All the matches in this section should be exported
                value_view_t key_copy = paths.push_back(member.key, c_error);
                return_on_error(c_error);
                paths.add_terminator(byte_t {0}, c_error);
                return_on_error(c_error);
                ++found_paths;

                // Prepare for the next round of search
                previous = key_copy;
            });
        }
    }

    count = found_paths;
}

/**
 * > Same collection
 * > Multiple requests
 * > No previous results
 */
void scan_one_collection_many_prefixes( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    ukv_collection_t collection,
    contents_arg_t prefixes,
    strided_range_gt<ukv_length_t const> max_counts,
    ukv_options_t const options,
    span_gt<ukv_length_t> counts,
    growing_tape_t& paths,
    stl_arena_t& arena,
    ukv_error_t* c_error) {
}

/**
 * > Same collection
 * > Multiple requests
 * > Has previous results
 */
void scan_one_collection_many_ranges( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    ukv_collection_t collection,
    contents_arg_t prefixes,
    contents_arg_t previous,
    strided_range_gt<ukv_length_t const> max_counts,
    ukv_options_t const options,
    span_gt<ukv_length_t> counts,
    growing_tape_t& paths,
    stl_arena_t& arena,
    ukv_error_t* c_error) {
}

struct prefix_match_task_t {
    ukv_collection_t collection = ukv_collection_main_k;
    value_view_t prefix;
    value_view_t previous;
    ukv_length_t max_count = 0;
};

void ukv_paths_match( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    ukv_size_t const c_tasks_count,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_length_t const* c_prefixes_offsets,
    ukv_size_t const c_prefixes_offsets_stride,

    ukv_length_t const* c_prefixes_lengths,
    ukv_size_t const c_prefixes_lengths_stride,

    ukv_str_view_t const* c_prefixes,
    ukv_size_t const c_prefixes_stride,

    ukv_length_t const* c_previous_offsets,
    ukv_size_t const c_previous_offsets_stride,

    ukv_length_t const* c_previous_lengths,
    ukv_size_t const c_previous_lengths_stride,

    ukv_str_view_t const* c_previous,
    ukv_size_t const c_previous_stride,

    ukv_length_t const* c_scan_limits,
    ukv_size_t const c_scan_limits_stride,

    ukv_options_t const c_options,
    ukv_char_t const,

    ukv_length_t** c_counts,
    ukv_length_t** c_offsets,
    ukv_char_t** c_paths,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    stl_arena_t arena = prepare_arena(c_arena, c_options, c_error);
    return_on_error(c_error);

    contents_arg_t prefixes_args;
    prefixes_args.offsets_begin = {c_prefixes_offsets, c_prefixes_offsets_stride};
    prefixes_args.lengths_begin = {c_prefixes_lengths, c_prefixes_lengths_stride};
    prefixes_args.contents_begin = {(ukv_bytes_cptr_t const*)c_prefixes, c_prefixes_stride};
    prefixes_args.count = c_tasks_count;

    contents_arg_t previous_args;
    previous_args.offsets_begin = {c_previous_offsets, c_previous_offsets_stride};
    previous_args.lengths_begin = {c_previous_lengths, c_previous_lengths_stride};
    previous_args.contents_begin = {(ukv_bytes_cptr_t const*)c_previous, c_previous_stride};
    previous_args.count = c_tasks_count;

    strided_range_gt<ukv_collection_t const> collections {{c_collections, c_collections_stride}, c_tasks_count};
    strided_range_gt<ukv_length_t const> scan_limits {{c_scan_limits, c_scan_limits_stride}, c_tasks_count};

    auto first_collection = c_collections ? c_collections[0] : ukv_collection_main_k;
    auto is_same_collection = collections.same_elements();
    auto has_previous = c_previous != nullptr;

    auto scan_limits_sum = transform_reduce_n(scan_limits.begin(), c_tasks_count, 0ul);
    auto found_counts = arena.alloc<ukv_length_t>(c_tasks_count, c_error);
    auto found_paths = growing_tape_t(arena);

    if (c_tasks_count == 1)
        scan_one_collection_one_range( //
            c_db,
            c_txn,
            first_collection,
            prefixes_args[0],
            previous_args[0],
            c_scan_limits[0],
            c_options,
            found_counts[0],
            found_paths,
            arena,
            c_error);

    else if (is_same_collection) {
        has_previous                             //
            ? scan_one_collection_many_prefixes( //
                  c_db,
                  c_txn,
                  first_collection,
                  prefixes_args,
                  scan_limits,
                  c_options,
                  found_counts,
                  found_paths,
                  arena,
                  c_error)
            : scan_one_collection_many_ranges( //
                  c_db,
                  c_txn,
                  first_collection,
                  prefixes_args,
                  previous_args,
                  scan_limits,
                  c_options,
                  found_counts,
                  found_paths,
                  arena,
                  c_error);
    }

    else {
        // If we have multiple tasks fro different collections - lets group them together
        // and solve one by one.
    }

    // Export the results
    if (c_counts)
        *c_counts = found_counts.begin();
    if (c_offsets)
        *c_offsets = found_paths.offsets().begin().get();
    if (c_paths)
        *c_paths = (ukv_char_t*)found_paths.contents().begin().get();
}