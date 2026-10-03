// Copyright 2023 Bloomberg Finance L.P.
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// BMQ
#include <bmqp_schemagenerator.h>

// BDE
#include <bdlb_random.h>
#include <bslstl_map.h>

// TEST DRIVER
#include <bmqtst_testhelper.h>

// BENCHMARKING LIBRARY
#ifdef BMQTST_BENCHMARK_ENABLED
#include <benchmark/benchmark.h>
#endif

#include <bsl_cstdlib.h>
#include <bsl_map.h>
#include <bsl_vector.h>

// CONVENIENCE
using namespace BloombergLP;
using namespace bsl;

// ============================================================================
//                                    TESTS
// ----------------------------------------------------------------------------

typedef bmqp::MessagePropertiesInfo::SchemaIdType SchemaIdType;

const SchemaIdType NO_SCHEMA = bmqp::MessagePropertiesInfo::k_NO_SCHEMA;

/// instead of `bmqp::MessagePropertiesHeader::k_MAX_SCHEMA`
const SchemaIdType MAX_SCHEMA = 100;

/// instead of `bmqp::MessageProperties::k_MAX_NUM_PROPERTIES`
const int NUM_PROPERTIES = 10;

/// n + (n - 1) + (n - 2) + .. + 2 + 1 == n * (n + 1) /2
const int NUM_COMBINATIONS = NUM_PROPERTIES * (NUM_PROPERTIES + 1) / 2;

/// (lap, combination) pair identifies unique Properties sequence in the
/// range exceeding MAX_SCHEMA (to test recycling).
const int NUM_LAPS = 3;

const int NUM_RECYCLED = NUM_COMBINATIONS * NUM_LAPS - MAX_SCHEMA;

// keep these on heap rather than on stack
static int length[NUM_COMBINATIONS];

/// shuffled integers in the range [0..NUM_COMBINATIONS)
static int sequence[NUM_COMBINATIONS];

static void generateMessageProperties(bmqp::MessageProperties* mps,
                                      int                      lap,
                                      int                      combination)
{
    // (test, combination) pair identifies unique Properties sequence.
    // Randomize calling 'setPropertyAsString'
    // Note that MessageProperties::d_properties is a map (not a HT)

    int property = bsl::rand() % length[combination];
    for (int l = 0; l < length[combination]; ++l) {
        bsl::string name("property_", bmqtst::TestHelperUtil::allocator());
        name += bsl::to_string(lap);
        name += "_";
        name += bsl::to_string(sequence[combination] + property);

        BMQTST_ASSERT_EQ(0, mps->setPropertyAsString(name, name));

        if (++property == length[combination]) {
            property = 0;
        }
    }
}

static void test1_breathingTest()
{
    int                         count = 0;
    bsl::map<SchemaIdType, int> ids(bmqtst::TestHelperUtil::allocator());
    bmqp::SchemaGenerator theGenerator(bmqtst::TestHelperUtil::allocator());

    theGenerator._setCapacity(MAX_SCHEMA);

    // generate all possible sequences of 'NUM_PROPERTIES' with lengths ranging
    // from '1' to 'NUM_PROPERTIES'
    for (int start = 0; start < NUM_PROPERTIES; ++start) {
        // 'start' of next sequence
        for (int len = 1; len <= (NUM_PROPERTIES - start); ++len, ++count) {
            // 'length' of next sequence
            length[count] = len;
        }
    }
    BSLS_ASSERT_SAFE(count == NUM_COMBINATIONS);

    // So far, have not reached the NUM_SEQUENCES, because of '255' limit of
    // k_MAX_NUM_PROPERTIES.
    // Use two 'MessageProperties' to over-reach 'k_MAX_SCHEMA'
    BSLS_ASSERT_SAFE(NUM_LAPS * count > MAX_SCHEMA + 33);

    for (int i = 0; i < NUM_COMBINATIONS; ++i) {
        sequence[i] = i;
    }

    // Shuffle by swapping two random properties (NUM_SEQUENCES / 2) times
    for (int i = NUM_COMBINATIONS >> 1; i > 0; --i) {
        int i1  = bsl::rand() % NUM_COMBINATIONS;
        int i2  = bsl::rand() % NUM_COMBINATIONS;
        int seq = sequence[i1];
        int len = length[i1];

        sequence[i1] = sequence[i2];
        sequence[i2] = seq;
        length[i1]   = length[i2];
        length[i2]   = len;
    }
    count = 0;
    // Use three 'MessageProperties' (there is the limit of 255)
    for (int lap = 0; lap < NUM_LAPS; ++lap) {
        // Up till 'k_MAX_SCHEMA' everything is unique (one time only)
        for (int i = 0; i < NUM_COMBINATIONS; ++i, ++count) {
            BSLS_ASSERT_SAFE(length[i]);

            bmqp::MessageProperties mps(bmqtst::TestHelperUtil::allocator());
            // For example, given {a, b, c} properties:
            //  [a]
            //  [a, b]
            //  [a, b, c]
            //  [b]
            //  [b, c]
            //  [c]

            generateMessageProperties(&mps, lap, i);

            bmqp::MessagePropertiesInfo logic = theGenerator.getSchemaId(&mps);
            SchemaIdType                schemaId = logic.schemaId();

            BMQTST_ASSERT_LE(schemaId, MAX_SCHEMA);
            BMQTST_ASSERT_NE(schemaId, NO_SCHEMA);
            ids[schemaId] = count;

            BMQTST_ASSERT(logic.isRecycled());  // either first use or recycled

            // Call 'getSchema' again
            logic = theGenerator.getSchemaId(&mps);
            BMQTST_ASSERT_EQ(schemaId, logic.schemaId());
            BMQTST_ASSERT(!logic.isRecycled());  // second use
        }
    }
    // Some number of first test combinations (65) got recycled.
    // Repeat them again and verify, they got new IDs.
    for (int i = 0; i < 33; ++i) {
        bmqp::MessageProperties mps(bmqtst::TestHelperUtil::allocator());
        generateMessageProperties(&mps, 0, i);
        // Sequence numbering starts from '0', schema's - from '1'

        // 'i'              is a sequence of MPs
        // 'i + 1'          is the expected id
        // 'schema1.d_id'   is post-recycling id
        // 'ids[i + 1]'     is the last sequence referenced by 'i + 1'

        BMQTST_ASSERT_NE(ids[i + 1], i);  // 'i + 1' was recycled

        bmqp::MessagePropertiesInfo logic = theGenerator.getSchemaId(&mps);
        BMQTST_ASSERT(logic.isRecycled());
        // We continue recycling
        BMQTST_ASSERT_EQ(logic.schemaId(), SchemaIdType(i + 1 + NUM_RECYCLED));

        // Call again to make number of hits equal to the rest (2)
        logic = theGenerator.getSchemaId(&mps);
        BMQTST_ASSERT(!logic.isRecycled());
        // We continue recycling
        BMQTST_ASSERT_EQ(logic.schemaId(), SchemaIdType(i + 1 + NUM_RECYCLED));
    }
}


static void populatePhaseProperties(bmqp::MessageProperties* mps,
                                    int                      numProperties,
                                    int                      nameLength,
                                    int                      seed)
{
    for (int i = 0; i < numProperties; ++i) {
        bsl::string name("p", bmqtst::TestHelperUtil::allocator());
        name += bsl::to_string(seed);
        name += "_";
        name += bsl::to_string(i);
        if (static_cast<int>(name.length()) < nameLength) {
            name.append(nameLength - name.length(), 'x');
        }
        BMQTST_ASSERT_EQ(0, mps->setPropertyAsString(name, "v"));
    }
}

static void printPhaseStats(const char*                       schemaName,
                            const char*                       mode,
                            const bmqp::SchemaGenerator::PhaseStats& stats)
{
    const double calls = static_cast<double>(stats.d_calls == 0 ? 1
                                                                : stats.d_calls);

    cout << "PHASE"
         << " schema=" << schemaName
         << " mode=" << mode
         << " calls=" << stats.d_calls
         << " misses=" << stats.d_misses
         << " hash_ns_per_call=" << stats.d_hashNs / calls
         << " lookup_ns_per_call=" << stats.d_lookupNs / calls
         << " compare_ns_per_call=" << stats.d_compareNs / calls
         << " lock_wait_ns_per_call=" << stats.d_lockWaitNs / calls
         << " lock_held_ns_per_call=" << stats.d_lockHeldNs / calls
         << " materialize_ns_per_call=" << stats.d_materializeNs / calls
         << endl;
}

static void test4_phaseAttribution()
{
    bmqtst::TestHelperUtil::ignoreCheckDefAlloc() = true;

    struct SchemaShape {
        const char* d_name;
        int         d_properties;
        int         d_nameLength;
    };

    const SchemaShape shapes[] = {
        {"small", 4, 32},
        {"medium", 16, 64},
        {"large", 64, 128}};

    for (int s = 0; s < 3; ++s) {
        {
            bmqp::SchemaGenerator generator(bmqtst::TestHelperUtil::allocator());
            bmqp::MessageProperties hot(bmqtst::TestHelperUtil::allocator());
            populatePhaseProperties(&hot,
                                    shapes[s].d_properties,
                                    shapes[s].d_nameLength,
                                    1);

            generator.getSchemaId(&hot);
            generator._resetPhaseStats();

            for (int i = 0; i < 5000; ++i) {
                generator.getSchemaId(&hot);
            }

            printPhaseStats(shapes[s].d_name,
                            "hit",
                            generator._phaseStats());
        }

        {
            bmqp::SchemaGenerator generator(bmqtst::TestHelperUtil::allocator());
            generator._setCapacity(16);

            bsl::vector<bmqp::MessageProperties> misses(
                bmqtst::TestHelperUtil::allocator());
            misses.reserve(32);
            for (int m = 0; m < 32; ++m) {
                misses.emplace_back();
                populatePhaseProperties(&misses.back(),
                                        shapes[s].d_properties,
                                        shapes[s].d_nameLength,
                                        1000 + m);
            }

            generator._resetPhaseStats();

            for (int i = 0; i < 2000; ++i) {
                generator.getSchemaId(&misses[i % misses.size()]);
            }

            printPhaseStats(shapes[s].d_name,
                            "miss",
                            generator._phaseStats());
        }
    }
}

#ifdef BMQTST_BENCHMARK_ENABLED
static void testN1_getSchemaIdHot_GoogleBenchmark(benchmark::State& state)
{
    bmqtst::TestHelperUtil::ignoreCheckDefAlloc() = true;

    const int numProperties = static_cast<int>(state.range(0));
    const int nameLength    = static_cast<int>(state.range(1));

    bmqp::MessageProperties mps(bmqtst::TestHelperUtil::allocator());
    for (int i = 0; i < numProperties; ++i) {
        bsl::string name("p", bmqtst::TestHelperUtil::allocator());
        name += bsl::to_string(i);
        if (static_cast<int>(name.length()) < nameLength) {
            name.append(nameLength - name.length(), 'x');
        }
        BMQTST_ASSERT_EQ(0, mps.setPropertyAsString(name, "v"));
    }

    bmqp::SchemaGenerator generator(bmqtst::TestHelperUtil::allocator());

    // Warm the schema cache so the timed region measures the steady-state hit
    // path used when a producer repeatedly publishes the same schema.
    benchmark::DoNotOptimize(generator.getSchemaId(&mps).schemaId());

    for (auto _ : state) {
        benchmark::DoNotOptimize(generator.getSchemaId(&mps).schemaId());
    }
}
#endif  // BMQTST_BENCHMARK_ENABLED

// ============================================================================
//                                 MAIN PROGRAM
// ----------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    TEST_PROLOG(bmqtst::TestHelper::e_DEFAULT);

    switch (_testCase) {
    case 0:
    case 4: test4_phaseAttribution(); break;
    case 1: test1_breathingTest(); break;
    case -1:
        BMQTST_BENCHMARK_WITH_ARGS(testN1_getSchemaIdHot,
                                   ArgsProduct({{1, 4, 8, 16, 32},
                                                {8, 32, 128}})
                                       ->Unit(benchmark::kNanosecond));
        break;
    default: {
        cerr << "WARNING: CASE '" << _testCase << "' NOT FOUND." << endl;
        bmqtst::TestHelperUtil::testStatus() = -1;
    } break;
    }
#ifdef BMQTST_BENCHMARK_ENABLED
    if (_testCase < 0) {
        benchmark::Initialize(&argc, argv);
        benchmark::RunSpecifiedBenchmarks();
    }
#endif

    TEST_EPILOG(bmqtst::TestHelper::e_CHECK_GBL_ALLOC);
}
