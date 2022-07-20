/**
 * @file test.cpp
 * @author Ashot Vardanian
 * @date 2022-07-06
 *
 * @brief A set of tests implemented using Google Test.
 */

#include <unordered_set>

#include <gtest/gtest.h>

#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

void round_trip(entries_ref_t ref, disjoint_values_view_t values) {

    EXPECT_TRUE(ref.set(values)) << "Failed to assign";

    EXPECT_TRUE(ref.get()) << "Failed to fetch inserted keys";

    // Validate that values match
    taped_values_view_t retrieved = *ref.get();
    EXPECT_EQ(retrieved.size(), ref.keys().size());
    tape_iterator_t it = retrieved.begin();
    for (std::size_t i = 0; i != ref.keys().size(); ++i, ++it) {
        auto expected_len = static_cast<std::size_t>(values.lengths[i]);
        auto expected_begin = reinterpret_cast<byte_t const*>(values.contents[i]) + values.offsets[i];

        value_view_t val_view = *it;
        EXPECT_EQ(val_view.size(), expected_len);
        EXPECT_TRUE(std::equal(val_view.begin(), val_view.end(), expected_begin));
    }
}

TEST(db, basic) {

    db_t db;
    EXPECT_TRUE(db.open(""));

    db_session_t session = db.session();

    std::vector<ukv_key_t> keys {34, 35, 36};
    ukv_val_len_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {34, 35, 36};
    std::vector<ukv_val_len_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_val_ptr_t>(vals.data());

    entries_ref_t ref = session[keys];
    disjoint_values_view_t values {
        .contents = {&vals_begin, 0, 3},
        .offsets = offs,
        .lengths = {val_len, 3},
    };
    round_trip(ref, values);

    // Overwrite those values with same size integers and try again
    for (auto& val : vals)
        val += 100;
    round_trip(ref, values);

    // Overwrite with empty values, but check for existence
    EXPECT_TRUE(ref.clear());
    for (ukv_key_t key : ref.keys()) {
        expected_gt<strided_range_gt<bool>> indicators = session[key].contains();
        EXPECT_TRUE(indicators);
        EXPECT_TRUE((*indicators)[0]);

        expected_gt<indexed_range_gt<ukv_val_len_t*>> lengths = session[key].lengths();
        EXPECT_TRUE(lengths);
        EXPECT_EQ((*lengths)[0], 0u);
    }

    // Check scans
    EXPECT_TRUE(session.keys());
    auto present_keys = *session.keys();
    auto present_it = std::move(present_keys).begin();
    auto expected_it = keys.begin();
    for (; expected_it != keys.end(); ++present_it, ++expected_it) {
        EXPECT_EQ(*expected_it, *present_it);
    }
    EXPECT_TRUE(present_it.is_end());

    // Remove all of the values and check that they are missing
    EXPECT_TRUE(ref.erase());
    for (ukv_key_t key : ref.keys()) {
        expected_gt<strided_range_gt<bool>> indicators = session[key].contains();
        EXPECT_TRUE(indicators);
        EXPECT_FALSE((*indicators)[0]);

        expected_gt<indexed_range_gt<ukv_val_len_t*>> lengths = session[key].lengths();
        EXPECT_TRUE(lengths);
        EXPECT_EQ((*lengths)[0], ukv_val_len_missing_k);
    }
}

TEST(db, named) {
    db_t db;
    EXPECT_TRUE(db.open(""));

    expected_gt<collection_t> col = db["col"];

    std::vector<located_key_t> keys {{*col, 34}, {*col, 35}, {*col, 36}};
    ukv_val_len_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {34, 35, 36};
    std::vector<ukv_val_len_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_val_ptr_t>(vals.data());

    disjoint_values_view_t values {
        .contents = {&vals_begin, 0, 3},
        .offsets = offs,
        .lengths = {val_len, 3},
    };

    db_session_t session = db.session();
    entries_ref_t ref = session[keys];
    EXPECT_TRUE(session.contains("col"));
    EXPECT_FALSE(session.contains("unknown_col"));
    round_trip(ref, values);
}

TEST(db, net) {

    db_t db;
    EXPECT_TRUE(db.open(""));

    collection_t col(db);
    graph_t net(col);

    std::vector<edge_t> triangle {
        {1, 2, 9},
        {2, 3, 10},
        {3, 1, 11},
    };

    EXPECT_TRUE(net.upsert(triangle));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(9));
    EXPECT_FALSE(*net.contains(10));
    EXPECT_FALSE(*net.contains(1000));

    EXPECT_EQ(*net.degree(1), 2u);
    EXPECT_EQ(*net.degree(2), 2u);
    EXPECT_EQ(*net.degree(3), 2u);
    EXPECT_EQ(*net.degree(1, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(2, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(3, ukv_vertex_source_k), 1u);

    EXPECT_TRUE(net.edges(1));
    EXPECT_EQ(net.edges(1)->size(), 2ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges(3, ukv_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges(2, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].source_id, 2);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].target_id, 3);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].id, 10);
    EXPECT_EQ(net.edges(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges(1, 3)->size(), 0ul);

    // Check scans
    EXPECT_TRUE(net.edges());
    {
        std::unordered_set<edge_t, edge_hash_t> expected_edges {triangle.begin(), triangle.end()};
        std::unordered_set<edge_t, edge_hash_t> exported_edges;

        auto present_edges = *net.edges();
        auto present_it = std::move(present_edges).begin();
        auto count_results = 0;
        while (!present_it.is_end()) {
            exported_edges.insert(*present_it);
            ++present_it;
            ++count_results;
        }
        EXPECT_EQ(count_results, triangle.size() * 2);
        EXPECT_EQ(exported_edges, expected_edges);
    }

    // Remove a single edge, making sure that the nodes info persists
    EXPECT_TRUE(net.remove({
        .source_ids = {triangle[0].source_id},
        .target_ids = {triangle[0].target_id},
        .edge_ids = {triangle[0].id},
    }));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_EQ(net.edges(1, 2)->size(), 0ul);

    // Bring that edge back
    EXPECT_TRUE(net.upsert({
        .source_ids = {triangle[0].source_id},
        .target_ids = {triangle[0].target_id},
        .edge_ids = {triangle[0].id},
    }));
    EXPECT_EQ(net.edges(1, 2)->size(), 1ul);

    // Remove a vertex
    ukv_key_t vertex_to_remove = 2;
    EXPECT_TRUE(net.remove({vertex_to_remove}));
    EXPECT_FALSE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);

    // Bring back the whole graph
    EXPECT_TRUE(net.upsert(triangle));
    EXPECT_TRUE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 2ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 1ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}