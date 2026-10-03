// Copyright 2022-2023 Bloomberg Finance L.P.
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
#include <bmqp_schemalearner.h>
#include <bmqscm_version.h>

// BDE
#include <bdlma_localsequentialallocator.h>
#include <bslh_defaulthashalgorithm.h>
#include <bsl_utility.h>
#include <bsls_timeutil.h>

namespace BloombergLP {
namespace bmqp {

namespace {

bsl::size_t fingerprint(const MessageProperties* mps)
{
    bslh::DefaultHashAlgorithm hash;
    MessagePropertiesIterator  it(mps);
    const char                 delimiter = '_';

    while (it.hasNext()) {
        hash(&delimiter, 1);
        const bsl::string& name = it.name();
        hash(name.data(), name.size());
    }

    return static_cast<bsl::size_t>(hash.computeHash());
}

bool matchesKey(const bsl::string& key, const MessageProperties* mps)
{
    MessagePropertiesIterator it(mps);
    bsl::size_t                position = 0;

    while (it.hasNext()) {
        const bsl::string& name = it.name();

        if (position >= key.size() || key[position] != '_') {
            return false;
        }
        ++position;

        if (position + name.size() > key.size() ||
            0 != key.compare(position, name.size(), name)) {
            return false;
        }

        position += name.size();
    }

    return position == key.size();
}

}  // close unnamed namespace

// ===============================
// struct SchemaGenerator::Context
// ===============================

/// This struct represents result of id generation - the id and reference
/// to the list of least recently used items where LRU is the front.
struct SchemaGenerator::Context {
    SchemaIdType d_id;
    // The generated id.

    bsl::size_t d_fingerprint;
    // Fingerprint of the canonical key.

    LRU::const_iterator d_listIterator;
    // Iterator in the list of keys which assists in
    // selecting least recently used Context.

    explicit Context(const LRU::const_iterator& cit);
};

// CREATORS
inline SchemaGenerator::Context::Context(const LRU::const_iterator& cit)
: d_id(k_NO_SCHEMA)
, d_fingerprint(0)
, d_listIterator(cit)
{
    // NOTHING
}

// ======================
// struct SchemaGenerator
// ======================

// CREATORS
SchemaGenerator::SchemaGenerator(bslma::Allocator* basicAllocator)
: d_allocator_p(bslma::Default::allocator(basicAllocator))
, d_capacity(k_MAX_SCHEMA)
, d_currentId(0)
, d_contextMap(d_allocator_p)
, d_fingerprintMap(d_allocator_p)
, d_lru(d_allocator_p)
, d_lock(bsls::SpinLock::s_unlocked)
, d_phaseStats()
{
    // NOTHING
}

SchemaGenerator::~SchemaGenerator()
{
    // NOTHING
}

// MANIPULATORS
MessagePropertiesInfo
SchemaGenerator::getSchemaId(const MessageProperties* mps)
{
    // Do not send empty MPs.  This has to agree with
    // 'bmqp::PutEventBuilder::packMessage'

    if (mps == 0 || mps->numProperties() == 0) {
        return MessagePropertiesInfo();  // RETURN
    }

    ++d_phaseStats.d_calls;

    bsls::Types::Int64 phaseStart = bsls::TimeUtil::getTimer();
    const bsl::size_t schemaFingerprint = fingerprint(mps);
    d_phaseStats.d_hashNs += bsls::TimeUtil::getTimer() - phaseStart;

    {
        const bsls::Types::Int64 waitStart = bsls::TimeUtil::getTimer();
        bsls::SpinLockGuard guard(&d_lock);  // LOCK
        const bsls::Types::Int64 lockStart = bsls::TimeUtil::getTimer();
        d_phaseStats.d_lockWaitNs += lockStart - waitStart;

        phaseStart = bsls::TimeUtil::getTimer();
        bsl::pair<FingerprintMap::iterator, FingerprintMap::iterator> range =
            d_fingerprintMap.equal_range(schemaFingerprint);
        d_phaseStats.d_lookupNs += bsls::TimeUtil::getTimer() - phaseStart;

        for (FingerprintMap::iterator fit = range.first; fit != range.second;
             ++fit) {
            phaseStart = bsls::TimeUtil::getTimer();
            const bool matches = matchesKey(*fit->second, mps);
            d_phaseStats.d_compareNs += bsls::TimeUtil::getTimer() - phaseStart;
            if (!matches) {
                continue;
            }

            ContextMap::iterator found = d_contextMap.find(*fit->second);
            BSLS_ASSERT_OPT(found != d_contextMap.end());

            Context&            context = found->second;
            LRU::const_iterator current = context.d_listIterator;

            BSLS_ASSERT_OPT(current != d_lru.end());
            BSLS_ASSERT_OPT(context.d_id != k_NO_SCHEMA);

            if (--d_lru.end() != current) {
                d_lru.splice(d_lru.end(), d_lru, current);
            }

            context.d_listIterator = current;
            d_phaseStats.d_lockHeldNs +=
                bsls::TimeUtil::getTimer() - lockStart;
            return MessagePropertiesInfo(true, context.d_id, false);  // RETURN
        }

        d_phaseStats.d_lockHeldNs += bsls::TimeUtil::getTimer() - lockStart;
    }

    ++d_phaseStats.d_misses;
    phaseStart = bsls::TimeUtil::getTimer();
    bsl::size_t keyLength = 0;
    MessagePropertiesIterator sizeIt(mps);
    while (sizeIt.hasNext()) {
        keyLength += 1 + sizeIt.name().size();
    }

    bdlma::LocalSequentialAllocator<1024> localAllocator(d_allocator_p);
    bsl::string                           key(&localAllocator);
    key.reserve(keyLength);

    MessagePropertiesIterator it(mps);
    while (it.hasNext()) {
        key += '_';
        key += it.name();
    }
    d_phaseStats.d_materializeNs += bsls::TimeUtil::getTimer() - phaseStart;

    typedef bsl::pair<ContextMap::iterator, bool> InsertOrLookup;

    const bsls::Types::Int64 waitStart = bsls::TimeUtil::getTimer();
    bsls::SpinLockGuard guard(&d_lock);  // LOCK
    const bsls::Types::Int64 lockStart = bsls::TimeUtil::getTimer();
    d_phaseStats.d_lockWaitNs += lockStart - waitStart;

    InsertOrLookup insertOrLookup = d_contextMap.emplace(key, d_lru.end());
    const Context& context        = insertOrLookup.first->second;
    SchemaIdType   result         = context.d_id;
    bool           isNew          = insertOrLookup.second;
    LRU::const_iterator current   = context.d_listIterator;

    if (isNew) {
        // Either 1st time using this 'key' or it was 'recycled' and erased.

        if (d_currentId < d_capacity) {
            // Assign new schema
            result = ++d_currentId;
        }
        else {
            // There is no room for new id; need to recycle existing one.
            // Get the least used schema which the front of 'd_listIterator'

            BSLS_ASSERT_OPT(!d_lru.empty());

            // Recycle id associated with the front of 'd_listIterator'.
            ContextMap::iterator recycled = d_contextMap.find(d_lru.front());
            BSLS_ASSERT_OPT(recycled != d_contextMap.end());

            result = recycled->second.d_id;

            bsl::pair<FingerprintMap::iterator, FingerprintMap::iterator>
                recycledRange = d_fingerprintMap.equal_range(
                    recycled->second.d_fingerprint);
            for (FingerprintMap::iterator fit = recycledRange.first;
                 fit != recycledRange.second;
                 ++fit) {
                if (fit->second == &recycled->first) {
                    d_fingerprintMap.erase(fit);
                    break;
                }
            }

            // Erase recycled context
            d_contextMap.erase(recycled);
            d_lru.pop_front();
        }

        BSLS_ASSERT_OPT(result != k_NO_SCHEMA);

        // Initialize newly allocated 'Context'
        insertOrLookup.first->second.d_id          = result;
        insertOrLookup.first->second.d_fingerprint = schemaFingerprint;
        d_fingerprintMap.emplace(schemaFingerprint,
                                 &insertOrLookup.first->first);
        // Start tracking it in LRU
        d_lru.push_back(key);
        current = --d_lru.end();
    }
    else {
        BSLS_ASSERT_OPT(current != d_lru.end());
        BSLS_ASSERT_OPT(insertOrLookup.first->second.d_id != k_NO_SCHEMA);

        // reorder LRU
        if (--d_lru.end() != current) {
            d_lru.splice(d_lru.end(), d_lru, current);
        }
    }

    // Update 'Context' with the LRU tracking
    insertOrLookup.first->second.d_listIterator = current;
    d_phaseStats.d_lockHeldNs += bsls::TimeUtil::getTimer() - lockStart;

    return MessagePropertiesInfo(true, result, isNew);
}

}  // close package namespace
}  // close enterprise namespace
