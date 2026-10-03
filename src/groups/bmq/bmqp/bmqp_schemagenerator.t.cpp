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
#include <bdlf_bind.h>
#include <bsl_algorithm.h>
#include <bslma_newdeleteallocator.h>
#include <bslma_testallocator.h>
#include <bslmt_barrier.h>
#include <bslmt_threadgroup.h>
#include <bsls_timeutil.h>
#include <bslstl_map.h>
#include <bsl_vector.h>

// TEST DRIVER
#include <bmqtst_testhelper.h>

// BENCHMARKING LIBRARY
#ifdef BMQTST_BENCHMARK_ENABLED
#include <benchmark/benchmark.h>
#endif

#include <bsl_cstdlib.h>
#include <bsl_map.h>

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

static void test2_allocatorProfile()
{
    bmqtst::TestHelperUtil::ignoreCheckDefAlloc() = true;

    const int propertyCounts[] = {1, 4, 8, 16, 32, 64};
    const int nameLengths[]    = {8, 32, 64, 128, 256};

    const int numPropertyCounts =
        static_cast<int>(sizeof(propertyCounts) / sizeof(*propertyCounts));
    const int numNameLengths =
        static_cast<int>(sizeof(nameLengths) / sizeof(*nameLengths));

    for (int p = 0; p < numPropertyCounts; ++p) {
        for (int n = 0; n < numNameLengths; ++n) {
            bslma::TestAllocator allocator("schemaGeneratorProfile");

            bmqp::MessageProperties mps(&allocator);
            for (int i = 0; i < propertyCounts[p]; ++i) {
                bsl::string name("p", &allocator);
                name += bsl::to_string(i);
                if (static_cast<int>(name.length()) < nameLengths[n]) {
                    name.append(nameLengths[n] - name.length(), 'x');
                }
                BMQTST_ASSERT_EQ(0, mps.setPropertyAsString(name, "v"));
            }

            bmqp::SchemaGenerator generator(&allocator);
            generator.getSchemaId(&mps);

            bslma::TestAllocator& defaultAllocator =
                bmqtst::TestHelperUtil::defaultAllocator();

            const bsls::Types::Int64 blocksBefore = allocator.numBlocksTotal();
            const bsls::Types::Int64 bytesBefore  = allocator.numBytesTotal();
            const bsls::Types::Int64 inUseBefore  = allocator.numBytesInUse();
            const bsls::Types::Int64 defaultBlocksBefore =
                defaultAllocator.numBlocksTotal();
            const bsls::Types::Int64 defaultBytesBefore =
                defaultAllocator.numBytesTotal();
            const bsls::Types::Int64 defaultInUseBefore =
                defaultAllocator.numBytesInUse();

            const int iterations = 100;
            for (int i = 0; i < iterations; ++i) {
                generator.getSchemaId(&mps);
            }

            cout << "ALLOC_PROFILE generator properties=" << propertyCounts[p]
                 << " name_length=" << nameLengths[n]
                 << " blocks=" << allocator.numBlocksTotal() - blocksBefore
                 << " bytes=" << allocator.numBytesTotal() - bytesBefore
                 << " retained=" << allocator.numBytesInUse() - inUseBefore
                 << " default_blocks="
                 << defaultAllocator.numBlocksTotal() - defaultBlocksBefore
                 << " default_bytes="
                 << defaultAllocator.numBytesTotal() - defaultBytesBefore
                 << " default_retained="
                 << defaultAllocator.numBytesInUse() - defaultInUseBefore
                 << endl;
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


struct ConvergenceWorkerData {
    bmqp::SchemaGenerator*               d_generator_p;
    bslmt::Barrier*                      d_barrier_p;
    bmqp::MessageProperties              d_hot;
    bsl::vector<bmqp::MessageProperties> d_misses;
    bsl::vector<bsls::Types::Int64>      d_latencies;
    int                                  d_hitRate;
    int                                  d_iterations;
    bsls::Types::Int64                   d_start;
    bsls::Types::Int64                   d_end;
    bsls::Types::Int64                   d_hits;

    explicit ConvergenceWorkerData(bslma::Allocator* allocator)
    : d_generator_p(0)
    , d_barrier_p(0)
    , d_hot(allocator)
    , d_misses(allocator)
    , d_latencies(allocator)
    , d_hitRate(100)
    , d_iterations(0)
    , d_start(0)
    , d_end(0)
    , d_hits(0)
    {
    }
};

static void populateConvergenceProperties(bmqp::MessageProperties* mps,
                                          int                      numProperties,
                                          int                      nameLength,
                                          int                      schemaSeed)
{
    for (int i = 0; i < numProperties; ++i) {
        bsl::string name("p", bmqtst::TestHelperUtil::allocator());
        name += bsl::to_string(schemaSeed);
        name += "_";
        name += bsl::to_string(i);
        if (static_cast<int>(name.length()) < nameLength) {
            name.append(nameLength - name.length(), 'x');
        }
        BMQTST_ASSERT_EQ(0, mps->setPropertyAsString(name, "v"));
    }
}

static void convergenceWorker(ConvergenceWorkerData* data)
{
    data->d_barrier_p->wait();

    int missIndex = 0;
    data->d_start = bsls::TimeUtil::getTimer();

    for (int i = 0; i < data->d_iterations; ++i) {
        const bool useHot =
            data->d_hitRate != 0 && (i % 100) < data->d_hitRate;

        const bmqp::MessageProperties* mps;
        if (useHot) {
            mps = &data->d_hot;
        }
        else {
            mps = &data->d_misses[missIndex % data->d_misses.size()];
            ++missIndex;
        }

        const bool sample = (i & 3) == 0;
        bsls::Types::Int64 begin = 0;
        if (sample) {
            begin = bsls::TimeUtil::getTimer();
        }

        const bmqp::MessagePropertiesInfo info =
            data->d_generator_p->getSchemaId(mps);

        if (!info.isRecycled()) {
            ++data->d_hits;
        }

        if (sample) {
            data->d_latencies.push_back(bsls::TimeUtil::getTimer() - begin);
        }
    }

    data->d_end = bsls::TimeUtil::getTimer();
}

static void test3_convergenceMatrix()
{
    bmqtst::TestHelperUtil::ignoreCheckDefAlloc() = true;
    bmqtst::TestHelperUtil::ignoreCheckGblAlloc() = true;

    struct SchemaShape {
        const char* d_name;
        int         d_properties;
        int         d_nameLength;
    };

    const SchemaShape shapes[] = {
        {"small", 4, 32},
        {"medium", 16, 64},
        {"large", 64, 128}};

    const int hitRates[] = {0, 50, 90, 99, 100};
    const int threads[]  = {1, 2, 4, 8, 16};

    const int k_MISS_RING_SIZE = 32;
    const int k_ITERATIONS     = 2000;

    for (int s = 0; s < 3; ++s) {
        for (int h = 0; h < 5; ++h) {
            for (int t = 0; t < 5; ++t) {
                bslma::Allocator* allocator =
                    &bslma::NewDeleteAllocator::singleton();

                bmqp::SchemaGenerator generator(allocator);
                generator._setCapacity(16);

                const int numThreads = threads[t];
                bslmt::Barrier barrier(numThreads + 1);

                bsl::vector<ConvergenceWorkerData*> workers(allocator);
                workers.reserve(numThreads);

                for (int thread = 0; thread < numThreads; ++thread) {
                    ConvergenceWorkerData* data =
                        new (*allocator) ConvergenceWorkerData(allocator);
                    data->d_generator_p = &generator;
                    data->d_barrier_p   = &barrier;
                    data->d_hitRate     = hitRates[h];
                    data->d_iterations  = k_ITERATIONS;
                    data->d_latencies.reserve(k_ITERATIONS / 4 + 1);

                    populateConvergenceProperties(&data->d_hot,
                                                  shapes[s].d_properties,
                                                  shapes[s].d_nameLength,
                                                  1);

                    data->d_misses.reserve(k_MISS_RING_SIZE);
                    for (int m = 0; m < k_MISS_RING_SIZE; ++m) {
                        data->d_misses.emplace_back();
                        populateConvergenceProperties(
                            &data->d_misses.back(),
                            shapes[s].d_properties,
                            shapes[s].d_nameLength,
                            1000 + thread * k_MISS_RING_SIZE + m);
                    }

                    workers.push_back(data);
                }

                // Keep the intended hot schema resident before timing.
                generator.getSchemaId(&workers[0]->d_hot);

                bslmt::ThreadGroup threadGroup(allocator);
                for (int thread = 0; thread < numThreads; ++thread) {
                    const int rc = threadGroup.addThread(
                        bdlf::BindUtil::bind(&convergenceWorker,
                                             workers[thread]));
                    BMQTST_ASSERT_EQ(0, rc);
                }

                barrier.wait();
                threadGroup.joinAll();

                bsls::Types::Int64 firstStart = workers[0]->d_start;
                bsls::Types::Int64 lastEnd    = workers[0]->d_end;
                bsls::Types::Int64 hits       = 0;
                bsl::vector<bsls::Types::Int64> latencies(allocator);

                for (int thread = 0; thread < numThreads; ++thread) {
                    firstStart = bsl::min(firstStart, workers[thread]->d_start);
                    lastEnd    = bsl::max(lastEnd, workers[thread]->d_end);
                    hits += workers[thread]->d_hits;
                    latencies.insert(latencies.end(),
                                     workers[thread]->d_latencies.begin(),
                                     workers[thread]->d_latencies.end());
                }

                bsl::sort(latencies.begin(), latencies.end());

                const bsls::Types::Int64 totalOps =
                    static_cast<bsls::Types::Int64>(numThreads) *
                    k_ITERATIONS;
                const bsls::Types::Int64 elapsedNs = lastEnd - firstStart;
                const double throughput =
                    elapsedNs > 0
                        ? static_cast<double>(totalOps) * 1000000000.0 /
                              static_cast<double>(elapsedNs)
                        : 0.0;
                const double actualHitRate =
                    100.0 * static_cast<double>(hits) /
                    static_cast<double>(totalOps);

                const bsl::size_t p50Index =
                    latencies.empty() ? 0 : latencies.size() / 2;
                const bsl::size_t p99Index =
                    latencies.empty()
                        ? 0
                        : bsl::min(latencies.size() - 1,
                                   (latencies.size() * 99) / 100);

                cout << "CONVERGENCE"
                     << " schema=" << shapes[s].d_name
                     << " properties=" << shapes[s].d_properties
                     << " name_length=" << shapes[s].d_nameLength
                     << " target_hit_rate=" << hitRates[h]
                     << " actual_hit_rate=" << actualHitRate
                     << " threads=" << numThreads
                     << " ops=" << totalOps
                     << " throughput_ops_s=" << throughput
                     << " p50_ns="
                     << (latencies.empty() ? 0 : latencies[p50Index])
                     << " p99_ns="
                     << (latencies.empty() ? 0 : latencies[p99Index])
                     << endl;

                for (int thread = 0; thread < numThreads; ++thread) {
                    workers[thread]->~ConvergenceWorkerData();
                    allocator->deallocate(workers[thread]);
                }
            }
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
    case 3: test3_convergenceMatrix(); break;
    case 2: test2_allocatorProfile(); break;
    case 1: test1_breathingTest(); break;
    case -1:
        BMQTST_BENCHMARK_WITH_ARGS(testN1_getSchemaIdHot,
                                   ArgsProduct({{1, 4, 8, 16, 32, 64},
                                                {8, 32, 64, 128, 256}})
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
